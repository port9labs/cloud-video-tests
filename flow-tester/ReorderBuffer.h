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
#pragma once

#include <cstdint>
#include <map>

struct ErrorInterval {
    uint64_t last_known_sn{};
    int burst_count{};
    int64_t reverses{};
    int64_t duplicates{};
    int64_t longest_burst{};
    int64_t seq_breaks{};
};

class ReorderBuffer
{
public:
    explicit ReorderBuffer(int len = 512);
    /**
     * Add a received packet to the buffer.
     * @param sn The packet's sequence number within the entire stream.
     * @param flow_sn The packet's sequence number within its flow.
     * @param port The port for the packet's flow.
     * @param flow_burst_len If a flow burst was detected, its length will be written here.
     * @param flow_burst_count The total number of bursts for this flow will be written here.
     * @returns burst length if this packet ended a stream burst.
     */
    int addPacket(uint64_t sn, uint64_t flow_sn, int port, int *flow_burst_len, int *flow_burst_count);
    [[nodiscard]] uint64_t drops() const { return m_drops; }
    [[nodiscard]] uint64_t duplicates() const {return m_dups;}
    void dump() const;
    void report_bursts();

    /*
     * Returns the sequence number at buffer index idx.
     * It could be zero if that part of the buffer is empty.
     */
    uint64_t slot_sn(int idx) const;


    int check_for_burst_error(uint64_t flow_sn, int port, int *burst_count);


    [[nodiscard]] uint64_t stream_length_in_packets() const
    {
        return 1 + m_largest_received_sn - m_smallest_received_sn;
    }

    ErrorInterval flowInterval(int port) {
        auto iv = m_port_seq_map[port];
        if (iv) {
            return *iv;
        }
        ErrorInterval ret;
        ret.last_known_sn = 0;
        return ret;
    }

private:
    void mark(uint64_t sn);
    /*
     * buffer index for corresponding seq number or -1 if it doesn't fit
     */
    int idx_for(uint64_t sn) const;

    /*
     * Lowest and highest seq numbers that fit.
     * If a received sn is < lowest it will be tossed as stale.
     * If a received sn is > highest the buffer will be spilled,
     * creating space for it and counting drops of spilled packets.
     */
    uint64_t lowest_possible_sn() const;
    uint64_t highest_possible_sn() const;

    /*
     * Spill packets to create space in the buffer.
     * If packets are not marked they are considered dropped.
     * Returns the largest burst drop noted during the spill.
     * Spills enough to ensure that packet with seq number sn will
     * fit. It is assumed that sn is a received packet, so its reception
     * could mark the end of a burst.
     *
     * It is OK for sn to be much larger than any received sn so far.
     */
    int spill(uint64_t sn);

    /*
     * The buffer itself.
     * It is a circular buffer, and seq numbers will correspond to
     * index. We keep track of the lowest and highest sequence numbers
     * present to determine how much space remains for packets to be added.
     * The packets can be added before the lowest seq number or after the
     * highest seq number. If a packet seq number is too low we throw it
     * away as stale, but attempt to keep some buffer available for out of
     * order arrival.
     */
    uint8_t *m_buffer;
    int m_buffer_len{};

    /*
     * Occupancy is defined to be the number of packets
     * between head and tail.
     */
    [[nodiscard]] int occupancy() const;

    int m_tail_idx{0};  // index of oldest-by-seq sample
    int m_head_idx{0};  // index of newest-by-seq sample
    uint64_t m_tail_sn{0}; // seq of tail sample (0 if empty)

    /*
     * These are used to keep track of drops and to help
     * calculate statistics.
     */
    uint64_t m_last_spilled_good_sn{0};
    uint64_t m_drops{0};
    uint64_t m_dups{0};
    std::map<int, ErrorInterval *> m_port_seq_map{};
    uint64_t m_smallest_received_sn{};
    uint64_t m_largest_received_sn{};

};
