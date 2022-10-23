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
#include <algorithm>
#include <random>
#include <vector>


/*
 * Tests should return zero if they pass. The runner itself is main().
 */


static bool drop(float percent)
{
    static std::random_device rd;
    static std::mt19937 engine(rd());
    static std::uniform_real_distribution<double> d(0.0, 100000.0);
    return (d(engine) < (percent * 1000));
}

int test_calibrated(float percent)
{
    int flow_burst_len = 0;
    int flow_burst_count = 0;
    uint64_t sn = 10;
    uint64_t flow_sn = 1;
    uint64_t not_sent = 0;
    int port = 0;
    /*
     * Number of samples needs to increase inversely with desired error
     * percentage.
     */
    auto total = int64_t(3000000.0 / percent);

    auto b = new ReorderBuffer(1024);
    b->addPacket(sn, sn, port, &flow_burst_len, &flow_burst_count);
    sn += 1;
    for (auto i = 0; i < total; i++) {
        if (drop(percent)) { not_sent += 1; }
        else {
            b->addPacket(sn, sn, port, &flow_burst_len, &flow_burst_count);
        }
        sn += 1;
    }
    for (int i = 0; i < 1024; i++) {
        b->addPacket(sn, sn, port, &flow_burst_len, &flow_burst_count);
        sn += 1;
    }
    auto iv = b->flowInterval(port);
    if (not_sent != iv.seq_breaks) {
        spdlog::error("unexpected seq_breaks {} (expected {})", iv.seq_breaks, not_sent);
        return 1;
    }
    auto ret = (not_sent != b->drops());
    if (ret) {
        spdlog::error("for desired percent {}, expected {} drops but drop count is {}", percent, not_sent, b->drops());
    }
    return ret;
}

int test_spike(int buffer_len)
{
    int flow_burst_len = 0;
    int flow_burst_count = 0;
    int burst_len = 0;
    uint64_t sn = 12345;
    uint64_t flow_sn = 1;
    int port = 0;
    auto b = new ReorderBuffer(buffer_len);
    for (int i = 0; i < 1024; i++) {
        burst_len = b->addPacket(sn, flow_sn, port, &flow_burst_len, &flow_burst_count);
        auto drops = b->drops();
        if ((burst_len != 0) || (drops != 0)) {
            spdlog::error("0: unexpected result: burst_len: {}, drops: {}", burst_len, drops);
            b->dump();
            return 1;
        }
        sn += 1;
        flow_sn += 1;
    }
    /*
     * Now a large burst
     */
    auto gap = 2000;
    flow_sn += gap;
    sn += gap;
    spdlog::debug("adding gap of 2000: new sn = {}, was {}, current drops {}", sn, sn - gap, b->drops());
    burst_len = b->addPacket(sn, flow_sn, port, &flow_burst_len, &flow_burst_count);
    auto drops = b->drops();
    if ((burst_len != gap) || (drops != gap)) {
        spdlog::error("1: unexpected result: burst_len: {}, drops: {}  (gap is {})", burst_len, drops, gap);
        //b->dump();
        return 1;
    }
    sn += 1;
    for (int i = 0; i < 1024; i++) {
        burst_len = b->addPacket(sn, flow_sn, port, &flow_burst_len, &flow_burst_count);
        auto drops = b->drops();
        if ((burst_len != 0) || (drops != gap)) {
            spdlog::error("2: unexpected result: burst_len: {}, drops: {}  (gap is {})", burst_len, drops, gap);
            //b->dump();
            return 1;
        }
        sn += 1;
        flow_sn += 1;
    }
    return 0;
}

int test_out_of_order()
{
    auto b = new ReorderBuffer(1024);
    int flow_burst_len = 0;
    int flow_burst_count = 0;
    int burst_len = 0;
    uint64_t sn = 1;
    uint64_t flow_sn = 1;
    std::vector<uint64_t> sn_vec{};
    for (auto i = 0; i < 200; i++) { sn_vec.push_back(uint64_t(i + 1)); }
    unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
    std::shuffle(std::begin(sn_vec), std::end(sn_vec), std::default_random_engine(seed));
    uint64_t prev_sn = 0;
    int ooo = 0;
    for (auto sn: sn_vec) {
        if (sn < prev_sn) { ooo += 1; }
        prev_sn = sn;
        burst_len = b->addPacket(sn, sn, 0, &flow_burst_len, &flow_burst_count);
        if (burst_len) {
            spdlog::error("ooo: unexpected drops: sn {} burst_len {} flow_burst_len {} flow_burst_count {}", sn,
                          burst_len, flow_burst_len, flow_burst_count);
            b->dump();
            return 1;
        }
    }
    auto iv = b->flowInterval(0);
    if (iv.reverses != ooo) {
        spdlog::error("ooo2: iv.reverses = {}, expecting {}", iv.reverses, ooo);
        return 1;
    }
    if (b->drops() > 0) {
        spdlog::error("ooo2: got {} drops -- was expecting none", b->drops());
        b->dump();
        return 1;
    }
    for (auto sn = 200; sn < 2000; sn++) {
        burst_len = b->addPacket(sn, sn, 0, &flow_burst_len, &flow_burst_count);
        if (burst_len) {
            spdlog::error("ooo3: unexpected drops: sn {} burst_len {} flow_burst_len {} flow_burst_count {}", sn,
                          burst_len, flow_burst_len, flow_burst_count);
            b->dump();
            return 1;
        }
    }
    return 0;
}

int test_flowlets()
{
    auto b = new ReorderBuffer();
    int flow_burst_len = 0;
    int flow_burst_count = 0;
    int burst_len = 0;
    uint64_t sn = 1;
    uint64_t flow_sn = 1;

    /*
     * No drops in 4 flowlets
     */
    for (int i = 0; i < 8000; i++) {
        for (int port = 0; port < 4; port++) {
            burst_len = b->addPacket(sn, flow_sn, port, &flow_burst_len, &flow_burst_count);
            if (flow_burst_len || flow_burst_count || burst_len) {
                spdlog::error("unexpected drops: sn {} burst_len {} flow_burst_len {} flow_burst_count {}", sn,
                              burst_len, flow_burst_len, flow_burst_count);
                b->dump();
                return 1;
            }
            sn += 1;
        }
        flow_sn += 1;
    }
    auto drops = b->drops();
    if (drops != 0) { spdlog::error("unexpected drops {}", drops); }
    /*
     * Short burst in one flow
     */
    for (int i = 0; i < 800; i++) {
        for (int port = 0; port < 4; port++) {
            if ((port == 0) && (i > 100) && (i <= 200)) {
                // we're dropping this one
            }
            else {
                flow_burst_len = 0;
                flow_burst_count = 0;
                burst_len = b->addPacket(sn, flow_sn, port, &flow_burst_len, &flow_burst_count);
                if (flow_burst_len || flow_burst_count || burst_len) {
                    if ((port == 0) && (flow_burst_count == 1) && (flow_burst_len == 100)) {
                        // expected this one
                    }
                    else {
                        spdlog::error("unexpected drops: sn {}, port {}, flow_sn {}, burst_len {} flow_burst_len {} "
                                      "flow_burst_count {}",
                                      sn, port, flow_sn, burst_len, flow_burst_len, flow_burst_count);
                        b->dump();
                        return 1;
                    }
                }
            }
            sn += 1;
        }
        flow_sn += 1;
    }
    drops = b->drops();
    if (drops != 100) {
        spdlog::error("unexpected drops {}", drops);
        return 1;
    }
    return 0;
}

int test_drops()
{
    spdlog::debug("#### test 1 ####");
    auto b = new ReorderBuffer(64);
    int flow_burst_len = 0;
    int flow_burst_count = 0;
    int burst_len = 0;
    int port = 0;
    uint64_t sn = 1;
    uint64_t drops = 0;
    for (int i = 0; i < 800; i++) {
        burst_len = b->addPacket(sn, sn, port, &flow_burst_len, &flow_burst_count);
        if (flow_burst_len || flow_burst_count || burst_len) {
            spdlog::error("unexpected drops: sn {} burst_len {} flow_burst_len {} flow_burst_count {}", sn, burst_len,
                          flow_burst_len, flow_burst_count);
            b->dump();
            return 1;
        }
        sn += 1;
    }

    drops = b->drops();
    if (drops > 0) {
        spdlog::error("unexpected drops {} (expected 0)", drops);
        return 1;
    }
    auto iv = b->flowInterval(port);
    if (drops != iv.seq_breaks) {
        spdlog::error("unexpected seq_breaks {} (expected 0)", iv.seq_breaks);
        return 1;
    }
    spdlog::debug("stream packet count {}", b->stream_length_in_packets());

    spdlog::debug("#### test 2 ####");
    b = new ReorderBuffer(64);
    sn = 999;
    for (int i = 0; i < 8000; i++) {
        burst_len = b->addPacket(sn, sn, port, &flow_burst_len, &flow_burst_count);
        if (flow_burst_len || flow_burst_count || burst_len) {
            spdlog::error("unexpected drops: sn {} burst_len {} flow_burst_len {} flow_burst_count {}", sn, burst_len,
                          flow_burst_len, flow_burst_count);
            b->dump();
            return 1;
        }
        sn += 1;
    }
    drops = b->drops();
    if (drops > 0) {
        spdlog::error("unexpected drops {}", drops);
        return 1;
    }

    /*
     * Drop 50%
     */
    spdlog::debug("#### test 3 ####");
    b = new ReorderBuffer(512);
    sn = 100;
    for (int i = 0; i < 8000; i++) {
        auto seq_burst_len = b->addPacket(sn, sn, port, &flow_burst_len, &flow_burst_count);
        if (flow_burst_len || flow_burst_count || seq_burst_len) {
            spdlog::error("unexpected drops: sn {} seq_burst_len {} flow_burst_len {} flow_burst_count {}", sn,
                          seq_burst_len, flow_burst_len, flow_burst_count);
            b->dump();
            return 1;
        }
        sn += 2;
    }
    spdlog::debug("drops before purge: {}", b->drops());// 7743
    for (int i = 0; i < 800; i++) {
        b->addPacket(sn, sn, port, &flow_burst_len, &flow_burst_count);
        sn += 1;
    }
    drops = b->drops();
    if (drops != 8000) {
        spdlog::error("unexpected drops {} -- expected 8000", drops);
        //b->dump();
        return 1;
    }
    iv = b->flowInterval(port);
    if (drops != iv.seq_breaks) {
        spdlog::error("unexpected seq_breaks {} (expected {})", iv.seq_breaks, drops);
        return 1;
    }
    return 0;
}

int test_misc()
{
    auto b = new ReorderBuffer(8);
    int flow_burst_len = 0;
    int flow_burst_count = 0;
    int burst_len = 0;
    for (int i = 0; i < 8; ++i) {
        auto sn = 2 * i;
        spdlog::debug("add {}", 2 * i);
        burst_len = b->addPacket(sn, sn, 0, &flow_burst_len, &flow_burst_count);
        b->dump();
    }
    spdlog::debug("stream_burst_len = {}, flow_burst_len = {}, flow_burst_count = {}", burst_len, flow_burst_len,
                  flow_burst_count);
    b->dump();
    spdlog::debug("adding packet 100");
    burst_len = b->addPacket(100, 100, 0, &flow_burst_len, &flow_burst_count);
    b->dump();
    spdlog::debug("stream_burst_len = {}, flow_burst_len = {}, flow_burst_count = {}", burst_len, flow_burst_len,
                  flow_burst_count);
    uint64_t seqnum = 101;
    spdlog::debug("adding packet {}", seqnum);
    burst_len = b->addPacket(seqnum, seqnum, 0, &flow_burst_len, &flow_burst_count);
    spdlog::debug("stream_burst_len = {}, flow_burst_len = {}, flow_burst_count = {}", burst_len, flow_burst_len,
                  flow_burst_count);
    seqnum = 105;
    spdlog::debug("adding packet {}", seqnum);
    burst_len = b->addPacket(seqnum, seqnum, 0, &flow_burst_len, &flow_burst_count);
    spdlog::debug("stream_burst_len = {}, flow_burst_len = {}, flow_burst_count = {}", burst_len, flow_burst_len,
                  flow_burst_count);
    b->dump();
    for (int i = 0; i < 8; i++) {
        seqnum += 1;
        spdlog::debug("adding packet {}", seqnum);
        burst_len = b->addPacket(seqnum, seqnum, 0, &flow_burst_len, &flow_burst_count);
        spdlog::debug("stream_burst_len = {}, flow_burst_len = {}, flow_burst_count = {}", burst_len, flow_burst_len,
                      flow_burst_count);
    }
    return 0;
}

int basic()
{
    auto b = new ReorderBuffer(8);
    int flow_burst_len = 0;
    int flow_burst_count = 0;
    int burst_len = 0;
    uint64_t sn = 0;
    for (int i = 1; i < 9; ++i) {
        sn = 2 * i;
        burst_len = b->addPacket(sn, sn, 0, &flow_burst_len, &flow_burst_count);
        if (burst_len) {
            spdlog::error("unexpected burst_len {} for i = {}", burst_len, i);
            b->dump();
            return 1;
        }
    }
    for (int i = 0; i < 8; i++) {
        sn += 1;
        burst_len = b->addPacket(sn, sn, 0, &flow_burst_len, &flow_burst_count);
        if (burst_len) {
            spdlog::error("unexpected burst_len {} for i = {}", burst_len, i);
            b->dump();
            return 1;
        }
    }
    return 0;
}

#define RUN_TEST(call)                                                                                                 \
    ret = call;                                                                                                        \
    if (ret) { return ret; }

int main(int argc, char **argv)
{
    spdlog::set_level(spdlog::level::info);
    int ret;
    RUN_TEST(basic())
    RUN_TEST(test_out_of_order())
    RUN_TEST(test_drops())
    RUN_TEST(test_spike(1024))
    RUN_TEST(test_spike(512))
    RUN_TEST(test_spike(64))
    RUN_TEST(test_spike(333))
    RUN_TEST(test_flowlets())
    /*
     * Note that the calibrated drop tests
     * can take a while to execute in debug builds,
     * especially for low error percentages since that
     * requires a lot of samples.
     * Suggest uncommenting the low percentages
     * only if you have a reason to believe the test is needed.
     */
    RUN_TEST(test_calibrated(45.0));
    RUN_TEST(test_calibrated(10.0));
    RUN_TEST(test_calibrated(1.0));
    RUN_TEST(test_calibrated(0.125))
    //    RUN_TEST(test_calibrated(0.0125));
    //    RUN_TEST(test_calibrated(0.00125));

    return ret;
}
