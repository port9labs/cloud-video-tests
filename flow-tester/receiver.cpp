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

#include "ReorderBuffer.h"
#include "spdlog/spdlog.h"
#include "sqlite3.h"
#include <boost/any.hpp>
#include <boost/program_options.hpp>
#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>
#include <uv.h>

using std::chrono::duration_cast;
using std::chrono::high_resolution_clock;
using std::chrono::nanoseconds;

inline static uint64_t get_system_time_ns()
{
    return (uint64_t) duration_cast<nanoseconds>((high_resolution_clock::now()).time_since_epoch()).count();
}

#define TEST_ERROR                                                                                                     \
    if (rc) {                                                                                                          \
        spdlog::error("error {} binding prepared statement: {}", rc, sqlite3_errstr(rc));                              \
        m_lock.unlock();                                                                                               \
        return;                                                                                                        \
    }
namespace po = boost::program_options;

void print_usage(po::options_description &desc)
{
    std::stringstream ss;
    ss << desc;
    spdlog::info("{}", ss.str());
}

static void hexdump(const unsigned char *ptr, int buflen)
{
    auto *buf = (unsigned char *) ptr;
    int i, j;
    for (i = 0; i < buflen; i += 16) {
        printf("%06x: ", i);
        for (j = 0; j < 16; j++)
            if (i + j < buflen) printf("%02x ", buf[i + j]);
            else
                printf("   ");
        printf(" ");
        for (j = 0; j < 16; j++)
            if (i + j < buflen) printf("%c", isprint(buf[i + j]) ? buf[i + j] : '.');
        printf("\n");
    }
}

const int PACKET_SIZE = 1 << 14;

#pragma mark Reporting

/**
 * DBReportDrops contains information for one drops table row entry
 */
struct DBReportDrops {
    uv_work_t req;
    uint64_t packets_dropped;
    uint64_t packets_total;
    uint64_t duplicates;
    float media_rate;
    int64_t timestamp;
};

/**
 * DBReportBurst contains information for one bursts table row entry
 */
struct DBReportBurst {
    uv_work_t req;
    int port;
    uint64_t packets_received;
    uint64_t bytes_received;
    uint64_t burst_errors;
    int burst_length;
    int64_t timestamp;
};

/**
 * DBReportFlowStats contains information for one flow table raw enrty
 */
struct DBReportFlowStats {
    uv_work_t req;
    int port;
    int burst_count;
    int64_t reverses;
    int64_t duplicates;
    int64_t longest_burst;
    int64_t seq_breaks;
    int64_t timestamp;
};

/**
 * DBReportStreamBurst contains information of one sburst table row entry
 */
struct DBReportStreamBurst {
    uv_work_t req;
    int burst_len;
    int64_t timestamp;
};

/**
 * DBRecorder holds a database for recording statistics.
 * We expect that the data reporting methods will be called from worker threads.
 */
class DBRecorder
{
public:
    /**
     * Construct a DBRecorder that creates one sqlite database file
     * @param filepath the path name of the file to create
     */
    DBRecorder(const std::string &filepath);

    /**
     * Adds a row to the bursts table
     * @param drb the data for this row
     */
    void addBurstReading(DBReportBurst *drb);

    /**
     * Adds a row to the drops table
     * @param drd the data for this row
     */
    void addDropsReading(DBReportDrops *drd);

    /**
     * Adds a row to the sbursts table
     * @param drb the data for this row
     */
    void addStreamBurstReading(DBReportStreamBurst *drb);

    /**
     * Adds a row to the flows table
     * @param drf the data for this row
     */
    void addFlowStatsReading(DBReportFlowStats *drf);

private:
    sqlite3 *m_db{};
    sqlite3_stmt *m_bursts_insert_stmt{};
    sqlite3_stmt *m_drops_insert_stmt{};
    sqlite3_stmt *m_stream_bursts_insert_stmt{};
    sqlite3_stmt *m_flow_insert_stmt{};
    std::mutex m_lock{};
};


DBRecorder::DBRecorder(const std::string &filepath)
{
    unlink(filepath.c_str());
    m_lock.lock();
    auto rc = sqlite3_open(filepath.c_str(), &m_db);

    if (rc) {
        spdlog::error("rc from sqlite3_open {}: {}", rc, sqlite3_errmsg(m_db));
        throw std::invalid_argument(sqlite3_errmsg(m_db));
    }
    const char *create_statement = R"(create table drops
(
    x INTEGER PRIMARY KEY ASC,
    packets_dropped NUMERIC,
    packets_total   NUMERIC,
    duplicates      NUMERIC,
    media_rate     REAL,
    timestamp        NUMERIC
);)";

    char *errmsg = nullptr;
    rc = sqlite3_exec(m_db, create_statement, nullptr, nullptr, &errmsg);
    if (rc) {
        spdlog::error("rc from sqlite3_exec {}: {}", rc, sqlite3_errmsg(m_db));
        throw std::invalid_argument(sqlite3_errmsg(m_db));
    }

    create_statement = R"(create table bursts
(
    x INTEGER PRIMARY KEY ASC,
    port             INTEGER,
    packets_received NUMERIC,
    bytes_received   NUMERIC,
    burst_errors     INTEGER,
    burst_length     INTEGER,
    timestamp        NUMERIC
);)";

    rc = sqlite3_exec(m_db, create_statement, nullptr, nullptr, &errmsg);
    if (rc) {
        spdlog::error("rc from sqlite3_exec {}: {}", rc, sqlite3_errmsg(m_db));
        throw std::invalid_argument(sqlite3_errmsg(m_db));
    }

    create_statement = R"(create table sbursts
(
    x INTEGER PRIMARY KEY ASC,
    burst_length     INTEGER,
    timestamp        NUMERIC
);)";

    rc = sqlite3_exec(m_db, create_statement, nullptr, nullptr, &errmsg);
    if (rc) {
        spdlog::error("rc from sqlite3_exec {}: {}", rc, sqlite3_errmsg(m_db));
        throw std::invalid_argument(sqlite3_errmsg(m_db));
    }

    create_statement = R"(create table flows
(
    x INTEGER PRIMARY KEY ASC,
    port     INTEGER,
    burst_count     INTEGER,
    reverses        NUMERIC,
    duplicates      NUMERIC,
    longest_burst   NUMERIC,
    sequence_breaks NUMERIC,
    timestamp        NUMERIC
);)";

    rc = sqlite3_exec(m_db, create_statement, nullptr, nullptr, &errmsg);
    if (rc) {
        spdlog::error("rc from sqlite3_exec {}: {}", rc, sqlite3_errmsg(m_db));
        throw std::invalid_argument(sqlite3_errmsg(m_db));
    }

    const char *insert_statement =
            R"(insert into bursts(port, packets_received, bytes_received, burst_errors, burst_length, timestamp) VALUES (?, ?, ?, ?, ?, ?))";
    rc = sqlite3_prepare_v2(m_db, insert_statement, -1, &m_bursts_insert_stmt, nullptr);
    if (rc) {
        spdlog::error("rc from sqlite3_prepare_v2  bursts{}: {}", rc, sqlite3_errmsg(m_db));
        throw std::invalid_argument(sqlite3_errmsg(m_db));
    }
    insert_statement =
            R"(insert into drops(packets_dropped, packets_total, duplicates, media_rate, timestamp) VALUES (?, ?, ?, ?, ?))";
    rc = sqlite3_prepare_v2(m_db, insert_statement, -1, &m_drops_insert_stmt, nullptr);
    if (rc) {
        spdlog::error("rc from sqlite3_prepare_v2 drops {}: {}", rc, sqlite3_errmsg(m_db));
        throw std::invalid_argument(sqlite3_errmsg(m_db));
    }

    insert_statement = R"(insert into sbursts(burst_length, timestamp) VALUES (?, ?))";
    rc = sqlite3_prepare_v2(m_db, insert_statement, -1, &m_stream_bursts_insert_stmt, nullptr);
    if (rc) {
        spdlog::error("rc from sqlite3_prepare_v2 sbursts {}: {}", rc, sqlite3_errmsg(m_db));
        throw std::invalid_argument(sqlite3_errmsg(m_db));
    }

    insert_statement =
            R"(insert into flows(port, burst_count, reverses, duplicates, longest_burst, sequence_breaks, timestamp)
                          VALUES (?, ?, ?, ?, ?, ?, ?))";
    rc = sqlite3_prepare_v2(m_db, insert_statement, -1, &m_flow_insert_stmt, nullptr);
    if (rc) {
        spdlog::error("rc from sqlite3_prepare_v2 flows {}: {}", rc, sqlite3_errmsg(m_db));
        throw std::invalid_argument(sqlite3_errmsg(m_db));
    }

    spdlog::info("database created at {}", filepath);
    m_lock.unlock();
}

void DBRecorder::addBurstReading(DBReportBurst *drb)
{
    m_lock.lock();
    sqlite3_reset(m_bursts_insert_stmt);
    auto rc = sqlite3_bind_int(m_bursts_insert_stmt, 1, drb->port);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_bursts_insert_stmt, 2, drb->packets_received);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_bursts_insert_stmt, 3, drb->bytes_received);
    TEST_ERROR;
    rc = sqlite3_bind_int(m_bursts_insert_stmt, 4, drb->burst_errors);
    TEST_ERROR;
    rc = sqlite3_bind_int(m_bursts_insert_stmt, 5, drb->burst_length);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_bursts_insert_stmt, 6, drb->timestamp);
    TEST_ERROR;
    rc = sqlite3_step(m_bursts_insert_stmt);
    if (rc != SQLITE_DONE) { spdlog::error("rc from sqlite_step {}: {}", rc, sqlite3_errmsg(m_db)); }
    m_lock.unlock();
}

void DBRecorder::addDropsReading(DBReportDrops *drd)
{
    m_lock.lock();
    sqlite3_reset(m_drops_insert_stmt);
    auto rc = sqlite3_bind_int(m_drops_insert_stmt, 1, drd->packets_dropped);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_drops_insert_stmt, 2, drd->packets_total);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_drops_insert_stmt, 3, drd->duplicates);
    TEST_ERROR;
    rc = sqlite3_bind_double(m_drops_insert_stmt, 4, drd->media_rate);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_drops_insert_stmt, 5, drd->timestamp);
    TEST_ERROR;
    rc = sqlite3_step(m_drops_insert_stmt);
    if (rc != SQLITE_DONE) { spdlog::error("rc from sqlite_step {}: {}", rc, sqlite3_errmsg(m_db)); }
    m_lock.unlock();
}

void DBRecorder::addStreamBurstReading(DBReportStreamBurst *drb)
{
    spdlog::debug("got stream burst of length {}", drb->burst_len);
    m_lock.lock();
    sqlite3_reset(m_stream_bursts_insert_stmt);
    auto rc = sqlite3_bind_int(m_stream_bursts_insert_stmt, 1, drb->burst_len);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_stream_bursts_insert_stmt, 2, drb->timestamp);
    TEST_ERROR;
    rc = sqlite3_step(m_stream_bursts_insert_stmt);
    if (rc != SQLITE_DONE) { spdlog::error("rc from sqlite_step {}: {}", rc, sqlite3_errmsg(m_db)); }
    m_lock.unlock();
}

void DBRecorder::addFlowStatsReading(DBReportFlowStats *drf)
{
    m_lock.lock();
    sqlite3_reset(m_flow_insert_stmt);
    auto rc = sqlite3_bind_int(m_flow_insert_stmt, 1, drf->port);
    TEST_ERROR;
    rc = sqlite3_bind_int(m_flow_insert_stmt, 2, drf->burst_count);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_flow_insert_stmt, 3, drf->reverses);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_flow_insert_stmt, 4, drf->duplicates);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_flow_insert_stmt, 5, drf->longest_burst);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_flow_insert_stmt, 6, drf->seq_breaks);
    TEST_ERROR;
    rc = sqlite3_bind_int64(m_flow_insert_stmt, 7, drf->timestamp);
    TEST_ERROR;
    rc = sqlite3_step(m_flow_insert_stmt);
    if (rc != SQLITE_DONE) { spdlog::error("rc from sqlite_step {}: {}", rc, sqlite3_errmsg(m_db)); }
    m_lock.unlock();
}

DBRecorder *dbRecorder = nullptr;

void db_burst_reporter(uv_work_t *req)
{
    if (dbRecorder == nullptr) { return; }
    auto drw = (DBReportBurst *) req->data;
    dbRecorder->addBurstReading(drw);
}

void db_burst_cleanup(uv_work_t *req, int status)
{
    auto drw = (DBReportBurst *) req->data;
    delete drw;
}

void db_drop_reporter(uv_work_t *req)
{
    if (dbRecorder == nullptr) { return; }
    auto drd = (DBReportDrops *) req->data;
    dbRecorder->addDropsReading(drd);
}

void db_drop_cleanup(uv_work_t *req, int status)
{
    if (dbRecorder == nullptr) { return; }
    auto drd = (DBReportDrops *) req->data;
    delete drd;
}

void db_stream_burst_reporter(uv_work_t *req)
{
    if (dbRecorder == nullptr) { return; }
    auto drb = (DBReportStreamBurst *) req->data;
    dbRecorder->addStreamBurstReading(drb);
}

void db_stream_burst_cleanup(uv_work_t *req, int status)
{
    if (dbRecorder == nullptr) { return; }
    auto drb = (DBReportStreamBurst *) req->data;
    delete drb;
}

void db_flows_reporter(uv_work_t *req)
{
    if (dbRecorder == nullptr) { return; }
    auto drf = (DBReportFlowStats *) req->data;
    dbRecorder->addFlowStatsReading(drf);
}

void db_flows_cleanup(uv_work_t *req, int status)
{
    if (dbRecorder == nullptr) { return; }
    auto drf = (DBReportFlowStats *) req->data;
    delete drf;
}

#pragma mark Receiving

const uint64_t MARKER = 0x12345678;

class UDPReceiver
{
public:
    UDPReceiver(int port, uv_loop_t *loop, ReorderBuffer *reorderBuffer);
    char *getPacket();
    void freePacket(char *packet);

    static size_t packet_len() { return PACKET_SIZE; }
    int port() const { return m_port; }
    uint64_t received_packets() const { return m_received_packets; }
    uint64_t received_bytes() const { return m_received_bytes; }
    uint64_t sn_discontinuities() const { return m_sn_discontinuities; }
    static void set_flowlet_count(int count) { s_flowlet_count = count; }

private:
    static void my_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
    static void on_read(uv_udp_t *req, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr, unsigned flags);
    static char *alloc() { return new char[PACKET_SIZE]; }
    int m_port;
    ReorderBuffer *m_reorderBuffer{};
    std::deque<char *> m_packet_storage{};
    uv_udp_t recv_socket;
    uint64_t m_received_packets{0};
    uint64_t m_received_bytes{0};
    uint64_t m_previous_sn{0};
    uint64_t m_sn_discontinuities{0};
    inline static int s_flowlet_count = 0;
};

UDPReceiver::UDPReceiver(int port, uv_loop_t *loop, ReorderBuffer *reorderBuffer)
    : m_port(port), m_reorderBuffer(reorderBuffer)
{
    for (int i = 0; i < 64; ++i) { m_packet_storage.push_back(alloc()); }
    uv_udp_init(loop, &recv_socket);
    uv_handle_set_data(reinterpret_cast<uv_handle_t *>(&recv_socket), this);
    struct sockaddr_in recv_addr {
    };
    uv_ip4_addr("0.0.0.0", m_port, &recv_addr);
    uv_udp_bind(&recv_socket, (const struct sockaddr *) &recv_addr, UV_UDP_REUSEADDR);
    uv_udp_recv_start(&recv_socket, my_alloc_cb, on_read);
}

char *UDPReceiver::getPacket()
{
    if (m_packet_storage.empty()) {
        auto ret = alloc();
        spdlog::debug("alloc {}", (void *) ret);
        return ret;
    }
    auto p = m_packet_storage.front();
    m_packet_storage.pop_front();
    return p;
}

void UDPReceiver::my_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    auto sf = (UDPReceiver *) uv_handle_get_data(handle);
    buf->base = sf->getPacket();
    buf->len = sf->packet_len();
}

void UDPReceiver::freePacket(char *packet)
{
    if (packet != nullptr) { m_packet_storage.push_back(packet); }
}

/**
 * on_read is called each time a packet is received on a socket. The packet is added to the reorder
 * buffer and if anything interesting happens on its flow it will be logged
 * @param req the libuv UDP handle
 * @param nread number of bytes received (can be zero)
 * @param buf the buffer containing the received adta
 * @param addr the address of the sender (can be nullptr)
 * @param flags UDP handle flags
 */
void UDPReceiver::on_read(uv_udp_t *req, ssize_t nread, const uv_buf_t *buf, const struct sockaddr *addr,
                          unsigned int flags)
{
    if (nread != 0) {
        //hexdump(reinterpret_cast<const unsigned char *>(buf->base), nread);
    }
    int burst_len = 0;
    int burst_count = 0;
    int stream_burst_len = 0;
    if (buf != nullptr) {
        if (buf->base != nullptr) {
            auto rx = (UDPReceiver *) uv_handle_get_data(reinterpret_cast<const uv_handle_t *>(req));
            if (nread != 0) {
                if (buf->len == 0) { spdlog::error("{}: got buffer length 0", __FUNCTION__); }
                else {
                    auto ptr = (uint64_t *) buf->base;
                    uint64_t sentinel = *(ptr++);
                    if (sentinel != MARKER) { spdlog::error("unexpected sentinel value {}", sentinel); }
                    else {
                        // add packet to the reorder buffer
                        auto sn = *ptr++;
                        auto flow_sn = *ptr;
                        stream_burst_len =
                                rx->m_reorderBuffer->addPacket(sn, flow_sn, rx->m_port, &burst_len, &burst_count);
                    }
                }
            }
            rx->freePacket(buf->base);
            if (nread > 0) {
                // update statistics
                rx->m_received_packets += 1;
                rx->m_received_bytes += nread;
                if (burst_len != 0) {
                    // report this flow burst
                    auto reportWork = new DBReportBurst;
                    reportWork->req.data = reportWork;
                    reportWork->port = rx->m_port;
                    reportWork->burst_length = burst_len;
                    reportWork->burst_errors = burst_count;
                    reportWork->bytes_received = rx->received_bytes();
                    reportWork->packets_received = rx->received_packets();
                    uv_queue_work(rx->recv_socket.loop, &reportWork->req, db_burst_reporter, db_burst_cleanup);
                }
                if (stream_burst_len > 1) {
                    // report this stream burst
                    auto rw = new DBReportStreamBurst;
                    rw->req.data = rw;
                    rw->burst_len = stream_burst_len;
                    rw->timestamp = get_system_time_ns();
                    uv_queue_work(rx->recv_socket.loop, &rw->req, db_stream_burst_reporter, db_stream_burst_cleanup);
                }
            }
        }
    }
}

auto receivers = std::vector<UDPReceiver *>();

uint64_t last_report_bytes = 0;
std::chrono::steady_clock::time_point last_report_time{};

/**
 * drops_timer_cb is called every few seconds and runs on the main event loop.
 * We use it to collect data for one drops table row entry, so a row gets
 * added every few seconds containing statistics updated for that moment
 * @param handle the timer handle
 */
void drops_timer_cb(uv_timer_t *handle)
{
    using namespace std::chrono_literals;
    auto reorderBuffer = (ReorderBuffer *) uv_handle_get_data(reinterpret_cast<const uv_handle_t *>(handle));
    uint64_t bytes_received = 0;
    uint64_t packets_received = 0;
    for (int i = 0; i < receivers.size(); i++) {
        auto r = receivers[i];
        bytes_received += r->received_bytes();
        packets_received += r->received_packets();
    }
    auto now = std::chrono::steady_clock::now();
    float media_rate = 0.0;
    if (last_report_bytes != 0) {
        auto nowbytes = bytes_received;
        float diffbps = 8.0 * (float) (nowbytes - last_report_bytes);
        float diffns = (float) std::chrono::duration_cast<std::chrono::nanoseconds>(now - last_report_time).count();
        media_rate = (diffbps / diffns);
    }
    last_report_time = now;
    last_report_bytes = bytes_received;
    spdlog::info("total packets received: {} dropped: {}, media rate: {} gbits", packets_received,
                 reorderBuffer->drops(), media_rate);
    auto drd = new DBReportDrops;
    drd->req.data = drd;
    drd->timestamp = get_system_time_ns();
    drd->media_rate = media_rate;
    drd->packets_total = packets_received;
    drd->packets_dropped = reorderBuffer->drops();
    drd->duplicates = reorderBuffer->duplicates();
    // add the drops report from a worker thread
    uv_queue_work(handle->loop, &drd->req, db_drop_reporter, db_drop_cleanup);
    // asks the reorder buffer to log its burst stats to the console
    if (reorderBuffer != nullptr) { reorderBuffer->report_bursts(); }
}

struct FlowTimerData {
    ReorderBuffer *reorderBuffer;
    int starting_port;
    int flow_count;
};

/**
 * flows_timer_cb is called every few seconds and runs on the main event loop.
 * We use it to collect data for one flows table row entry, so a row gets
 * added every few seconds containing statistics updated for that moment
 * @param handle the timer handle
 */
void flows_timer_cb(uv_timer_t *handle)
{
    auto flowTimerData = (FlowTimerData *) uv_handle_get_data(reinterpret_cast<const uv_handle_t *>(handle));
    for (int i = 0; i < flowTimerData->flow_count; i++) {
        int port = flowTimerData->starting_port + i;
        auto iv = flowTimerData->reorderBuffer->flowInterval(port);
        if (iv.last_known_sn != 0) {
            auto drf = new DBReportFlowStats;
            drf->port = port;
            drf->req.data = drf;
            drf->timestamp = get_system_time_ns();
            drf->seq_breaks = iv.seq_breaks;
            drf->longest_burst = iv.longest_burst;
            drf->duplicates = iv.duplicates;
            drf->reverses = iv.reverses;
            drf->burst_count = iv.burst_count;
            // add the flows report from a worker thread
            uv_queue_work(handle->loop, &drf->req, db_flows_reporter, db_flows_cleanup);
        }
    }
}

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
    spdlog::set_level(spdlog::level::info);
    po::options_description desc("Allowed options");
    desc.add_options()("help", "produce help message")("port", po::value<int>()->default_value(5678), "listen port")(
            "flowlets", po::value<int>()->default_value(1), "number of flowlets");
    po::variables_map vm;
    auto listen_port = 5678;
    auto flowlet_count = 1;
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
    if (vm.count("port")) { listen_port = vm["port"].as<int>(); }
    if (vm.count("flowlets")) { flowlet_count = vm["flowlets"].as<int>(); }

    /*
     * Create a libuv event loop that will run all udp receivers and the log timer.
     */
    auto loop = uv_default_loop();

    /*
     * The reordering buffer keeps track of the readings
     */
    auto reorderBuffer = new ReorderBuffer(1024);

    /*
     * Create a receiver for every flowlet
     */
    UDPReceiver::set_flowlet_count(flowlet_count);
    for (int i = 0; i < flowlet_count; ++i) {
        auto rx = new UDPReceiver(listen_port + i, loop, reorderBuffer);
        receivers.push_back(rx);
    }

    /*
     * Create a recorder that writes our sqlite file
     */
    dbRecorder = new DBRecorder("/tmp/cloudnet.db");

    uv_timer_t drops_timer{};
    uv_timer_init(loop, &drops_timer);
    drops_timer.data = reorderBuffer;
    uv_timer_start(&drops_timer, drops_timer_cb, 10000, 10000);

    uv_timer_t flows_timer{};
    uv_timer_init(loop, &flows_timer);
    auto ftd = new FlowTimerData;
    ftd->reorderBuffer = reorderBuffer;
    ftd->starting_port = listen_port;
    ftd->flow_count = flowlet_count;
    flows_timer.data = ftd;
    uv_timer_start(&flows_timer, flows_timer_cb, 15000, 10000);

    return uv_run(loop, UV_RUN_DEFAULT);
}