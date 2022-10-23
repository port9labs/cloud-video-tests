/*
MIT License

Copyright (c) 2022 Port 9 Labs

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include "protocol.h"
#include "spdlog/spdlog.h"
#include <boost/any.hpp>
#include <boost/program_options.hpp>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::nanoseconds;

inline static uint64_t get_system_time_ns()
{
    return (uint64_t) duration_cast<nanoseconds>((high_resolution_clock::now()).time_since_epoch()).count();
}

namespace po = boost::program_options;

void print_usage(po::options_description &desc)
{
    std::stringstream ss;
    ss << desc;
    spdlog::info("{}", ss.str());
}

uint64_t s_bytes_sent = 0;
uint64_t last_report_bytes = 0;
std::chrono::steady_clock::time_point last_report_time{};

/**
 * reporter is a loop that prints stats to stdout every few seconds
 */
void reporter()
{
    using namespace std::chrono_literals;
    while (true) {
        std::this_thread::sleep_for(10s);
        auto now = std::chrono::steady_clock::now();
        if (last_report_bytes != 0) {
            auto nowbytes = s_bytes_sent;
            float diffbps = 8.0 * (float) (nowbytes - last_report_bytes);
            float diffns = (float) std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_report_time).count();
            spdlog::info("transmit rate: {} gbits", (diffbps / diffns));
        }
        last_report_time = now;
        last_report_bytes = s_bytes_sent;
    }
}

/**
 * MARKER is written to the first 8 bytes of packet payload and tested by
 * the receiver
 */
const uint64_t MARKER = 0x12345678;

static std::string any_to_str(const boost::any &v)
{
    using boost::any_cast;
    if (v.type() == typeid(int)) { return fmt::format("{}", any_cast<int>(v)); }
    if (v.type() == typeid(float)) { return fmt::format("{}", any_cast<float>(v)); }
    if (v.type() == typeid(std::string)) { return any_cast<std::string>(v); }
    return "unknown";
}

int main(int argc, char **argv)
{
    using namespace std::chrono_literals;
    spdlog::set_level(spdlog::level::info);
    po::options_description desc("Allowed options");
    desc.add_options()("help", "produce help message")("dst", po::value<std::string>()->default_value("127.0.0.0:5678"),
                                                       "destination address:port")(
            "flowlets", po::value<int>()->default_value(1), "number of flowlets")(
            "plen", po::value<int>()->default_value(8100), "payload length (suggest 1400 on azure)")(
            "fmt", po::value<std::string>()->default_value("422"),
            "video format: 422, 444, 4444")("bpf", po::value<int>(), "bytes per frame (overrides --fmt setting)")(
            "rate", po::value<float>()->default_value(60.0), "frame rate in Hz");
    po::variables_map vm;
    try {
        po::store(po::parse_command_line(argc, argv, desc), vm);
        po::notify(vm);
    }
    catch (...) {
        print_usage(desc);
        exit(1);
    }
    for (auto const &[key, val]: vm) { spdlog::debug("option {} value {}", key, any_to_str(val.value())); }
    if (vm.count("help")) {
        print_usage(desc);
        exit(0);
    }
    auto dst_arg = std::string("127.0.0.1:5678");
    auto dst_addr_str = std::string("127.0.0.1");
    auto dst_port_str = std::string("5678");
    if (vm.count("dst")) { dst_arg = vm["dst"].as<std::string>(); }
    auto pos = dst_arg.find(':');
    if ((pos == std::string::npos) || (pos == dst_arg.size() - 1)) { spdlog::info("no port provided -- using 5678"); }
    else {
        dst_port_str = dst_arg.substr(pos + 1).c_str();
        dst_addr_str = dst_arg.substr(0, pos);
    }
    int flowlet_count = 1;
    if (vm.count("flowlets")) { flowlet_count = vm["flowlets"].as<int>(); }
    auto sock_fds = std::vector<int>();

    for (int i = 0; i < flowlet_count; ++i) {
        UDPAddress *destination_address = nullptr;
        try {
            destination_address = new UDPAddress(dst_addr_str, dst_port_str, i);
        }
        catch (std::exception &e) {
            spdlog::error("error looking up receiver address: {}", e.what());
            exit(1);
        }
        auto sock_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock_fd == -1) {
            spdlog::error("error creating socket {}", strerror(errno));
            exit(1);
        }
        auto err = destination_address->connect(sock_fd);
        if (err == -1) {
            spdlog::error("error connecting socket {}", strerror(errno));
            exit(1);
        }
        delete destination_address;
        int ttl = 123;
        err = setsockopt(sock_fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
        if (err == -1) {
            spdlog::error("error setting socket TTL: {}", strerror(errno));
            exit(1);
        }
        sock_fds.push_back(sock_fd);
    }
    auto frame_rate = 60.0;
    if (vm.count("rate")) { frame_rate = vm["rate"].as<float>(); }
    int packets_per_frame = 640;
    int payload_length = 8100;
    if (vm.count("plen")) { payload_length = vm["plen"].as<int>(); }
    int bytes_per_frame = 5184000;// 4:2:2
    if (vm.count("fmt")) {
        auto ppfstr = vm["fmt"].as<std::string>();
        if (ppfstr == "422") { bytes_per_frame = 5184000; }
        else if (ppfstr == "444") {
            bytes_per_frame = 12441600;
        }
        else if (ppfstr == "4444") {
            bytes_per_frame = 16588800;
        }
    }
    if (vm.count("bpf")) {
        bytes_per_frame = vm["bpf"].as<int>();
        spdlog::info("using bpf setting of {} bytes per frame", bytes_per_frame);
    }
    packets_per_frame = bytes_per_frame / payload_length;
    spdlog::info("sending {} {} byte packets per frame", packets_per_frame, payload_length);
    auto payload = new uint8_t[payload_length];
    auto frame_duration = (uint64_t) (1000000000.0 / (frame_rate * packets_per_frame));
    int fd_idx = 0;
    uint64_t sequence_number = 1;
    std::thread logger(reporter);
    std::this_thread::sleep_for(1s);
    uint64_t flow_sn = 1;
    spdlog::info("sending to {}", dst_arg);
    while (true) {
        auto idx = fd_idx;
        auto fd = sock_fds[fd_idx++];
        if (fd_idx >= sock_fds.size()) {
            fd_idx = 0;
            flow_sn += 1;
        }
        uint64_t frame_start_time = get_system_time_ns();
        auto ptr = (uint64_t *) payload;
        *ptr++ = MARKER;
        *ptr++ = sequence_number++;
        *ptr++ = flow_sn;
        *ptr++ = frame_start_time;
        auto len = send(fd, payload, payload_length, 0);
        if (len == -1) { spdlog::error("error writing to socket idx {}: {}", idx, strerror(errno)); }
        s_bytes_sent += len;
        while (get_system_time_ns() - frame_start_time <= frame_duration) {}
    }
    return 0;
}
