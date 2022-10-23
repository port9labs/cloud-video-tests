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

ReorderBuffer::ReorderBuffer(int len) : m_buffer_len(len)
{
    m_buffer = new uint8_t[m_buffer_len](); // zero initialized
    m_tail_idx = 0;
    m_head_idx = 0;
    m_tail_sn = 0;
    m_last_spilled_good_sn = 0;
    m_drops = 0;
}

int ReorderBuffer::addPacket(uint64_t sn, uint64_t flow_sn, int port, int *flow_burst_len, int *flow_burst_count)
{
    //spdlog::debug("addPacket: sn = {}", sn);
    auto previous_largest_sn = m_largest_received_sn;
    m_largest_received_sn = std::max(sn, m_largest_received_sn);
    m_smallest_received_sn = std::min(sn, m_smallest_received_sn);
    auto len = check_for_burst_error(flow_sn, port, flow_burst_count);
    /*
     * first sample in empty buffer?
     */
    if (m_tail_sn == 0) {
        m_tail_sn = sn;
        m_head_idx = m_tail_idx;
        m_buffer[m_tail_idx] = 1;
        return 0;
    }

    if (flow_burst_len != nullptr) { *flow_burst_len = len; }
    /*
     * case 1: packet is too old
     */
    auto lsn = lowest_possible_sn();
    auto hsn = highest_possible_sn();
    if (sn < lsn) {
        spdlog::info("ignoring stale packet {} (lowest_possible_sn is {}", sn, lsn);
        return 0;
    }
    if (sn <= hsn) {
        /*
        * case 3: fits, no spill
        */
        auto idx = idx_for(sn);
        //                spdlog::debug("addPacket: sn = {} fits -- hsn = {},  buffer[{}] = 1", sn, hsn,
        //                             idx);
        assert(idx >= 0);
        assert(idx < m_buffer_len);
        mark(sn);
        return 0;
    }
    /*
     * case 3: spill
     */
    auto stream_burst_len = spill(sn);
    return std::max(0, stream_burst_len);
}

void ReorderBuffer::dump() const
{
    spdlog::debug("tail_idx = {}, tail_sn = {}, head_idx = {}, occupancy = {}, drops = {}", m_tail_idx, m_tail_sn,
                  m_head_idx, occupancy(), m_drops);
    for (int i = 0; i < m_buffer_len; ++i) { spdlog::debug("buf[{}] sn = {}, val = {}", i, slot_sn(i), m_buffer[i]); }
}

uint64_t ReorderBuffer::slot_sn(int idx) const
{
    assert(idx >= 0);
    assert(idx < m_buffer_len);
    auto diff = idx - m_tail_idx;
    auto sn = m_tail_sn + ((idx - m_tail_idx) % m_buffer_len);
    if (diff < 0) { sn = m_tail_sn + diff + m_buffer_len; }
    uint64_t ret = sn;

    if (sn > m_tail_sn + occupancy()) { ret = 0; }
    //spdlog::debug("slot_sn {} calculates {}, returns {}", idx, sn, ret);
    return ret;
}

//int ReorderBuffer::oldest_slot() const { return m_tail_idx; }

int ReorderBuffer::check_for_burst_error(uint64_t flow_sn, int port, int *burst_count)
{
    int ret = 0;
    auto interval = m_port_seq_map[port];
    if (interval == nullptr) {
        auto v = new ErrorInterval;
        v->last_known_sn = flow_sn;
        v->burst_count = 0;
        v->seq_breaks = 0;
        v->longest_burst = 0;
        m_port_seq_map[port] = v;
    }
    else {
        auto expected_sn = interval->last_known_sn + 1;
        interval->last_known_sn = flow_sn;
        int64_t discontinuity = flow_sn - expected_sn;
        if (std::abs(discontinuity) > 2 ) {
            spdlog::debug("detected continuity break of {} packets on port {}", discontinuity, port);
            interval->seq_breaks += 1;
        }
        if (discontinuity < -1) {
            interval->reverses += 1;
            spdlog::debug("updating to {} sequence reversals on port {}", interval->reverses, port);
            return 0;
        }
        if (discontinuity == -1) {
            interval->duplicates += 1;
            spdlog::debug("updating to {} sequence duplicates on port {}", interval->duplicates, port);
        }
        if (discontinuity > 1) {
            interval->burst_count += 1;
            ret = discontinuity;
            if (burst_count != nullptr) { *burst_count = interval->burst_count; }
            if (discontinuity > interval->longest_burst) {
                spdlog::debug("updating longest burst to {}  on port {}", discontinuity, port);
                interval->longest_burst = discontinuity;
            }
        }
    }
    return ret;
}


void ReorderBuffer::report_bursts()
{
    if (m_port_seq_map.empty()) {
        return;
    }
    for (const auto &[port, v]: m_port_seq_map) {
        if (v != nullptr) {
            spdlog::info("port {} continuity breaks {}, continuity bursts: {}, longest {}", port, v->seq_breaks,
                         v->burst_count, v->longest_burst);
        }
    }
}

uint64_t ReorderBuffer::lowest_possible_sn() const
{
    uint64_t diff = (m_buffer_len - occupancy());
    if (m_tail_sn > diff) { return m_tail_sn - diff; }
    return 0;
}

uint64_t ReorderBuffer::highest_possible_sn() const { return m_tail_sn + m_buffer_len - 1; }

int ReorderBuffer::idx_for(uint64_t sn) const
{
    int ret = (m_tail_idx + (sn - m_tail_sn)) % m_buffer_len;
    //    spdlog::debug("idx_for {}: m_tail_idx {}, m_tail_sn {} returns {}",
    //                  sn, m_tail_idx, m_tail_sn, ret);
    return ret;
}

int ReorderBuffer::spill(u_int64_t sn)
{
    int patchup_burst = 0;
    int distance = sn - highest_possible_sn();
    assert(distance > 0);
    if (distance >= m_buffer_len) {
        patchup_burst = distance - 1;
        distance = m_buffer_len;
        m_drops += patchup_burst;
    }
    else {
        distance = std::min(occupancy(), distance);
    }
    int largest_burst = 0;
    int current_burst = 0;
    for (int i = 0; i < distance; i++) {
        auto tail = m_buffer[m_tail_idx];
        auto dropped = tail == 0;
        m_buffer[m_tail_idx] = 0;
        if (!dropped) {
            m_dups += tail - 1;
            current_burst = 0;
            m_last_spilled_good_sn = m_tail_sn;
            largest_burst = std::max(largest_burst, current_burst);
        }
        else {
            m_drops += 1;
            if (m_tail_sn > m_last_spilled_good_sn + 2) { current_burst += 1; }
        }
        m_tail_idx = (m_tail_idx + 1) % m_buffer_len;
        m_tail_sn += 1;
    }
    if (patchup_burst) {
        m_tail_sn = sn;
        m_head_idx = m_tail_idx;
        m_buffer[m_tail_idx] += 1;
    }
    else {
        mark(sn);
    }
    if (current_burst != 0) { largest_burst = std::max(largest_burst, current_burst + patchup_burst); }
    else {
        largest_burst = std::max(largest_burst, patchup_burst);
    }
    return largest_burst;
}

void ReorderBuffer::mark(uint64_t sn)
{
    auto largest_sn = m_tail_sn + occupancy() - 1;
    auto idx = idx_for(sn);
    m_buffer[idx] += 1;
    if (sn > largest_sn) { m_head_idx = idx; }
}

int ReorderBuffer::occupancy() const
{
    if (m_tail_sn == 0) { return 0; }
    auto diff = m_head_idx - m_tail_idx;
    auto ret = 1 + (diff % m_buffer_len);
    if (diff < 0) { ret = m_buffer_len + diff; }
    return ret;
}
