/*
 * Copyright (c) 2015 Mikhail Baranov
 * Copyright (c) 2015 Victor Gaydov
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <CppUTest/TestHarness.h>

#include "roc_config/config.h"
#include "roc_core/scoped_ptr.h"
#include "roc_rtp/parser.h"
#include "roc_datagram/datagram_queue.h"
#include "roc_pipeline/server.h"

#include "test_packet_stream.h"
#include "test_sample_stream.h"
#include "test_sample_queue.h"

namespace roc {
namespace test {

using namespace pipeline;

using datagram::IDatagramPtr;

TEST_GROUP(server) {
    enum {
        // No FEC and resampling.
        ServerOptions = 0,

        // Number of samples in every channel per read.
        ReadBufsz = SampleStream::ReadBufsz,

        // Number of samples in every channel per packet.
        PktSamples = (ReadBufsz * 5),

        // Number of packets enought to start rendering.
        NumPackets = (ROC_CONFIG_DEFAULT_RENDERER_LATENCY / PktSamples + 1),

        // Maximum number of packets.
        MaxPackets = ROC_CONFIG_MAX_SESSION_PACKETS,

        // Number of ticks without packets after wich session is terminated.
        Timeout = ROC_CONFIG_DEFAULT_SESSION_TIMEOUT * 2
    };

    SampleQueue<(MaxPackets + 1) * PktSamples / ReadBufsz> output;

    datagram::DatagramQueue input;

    rtp::Parser parser;

    ServerConfig config;
    core::ScopedPtr<Server> server;

    void setup() {
        config.options = ServerOptions;
        config.channels = ChannelMask;
        config.timeout = Timeout;

        server.reset(new Server(input, output, config));
    }

    void teardown() {
        LONGS_EQUAL(0, output.size());
    }

    void add_port(datagram::port_t port) {
        server->add_port(new_address(port), parser);
    }

    void tick(size_t n_samples) {
        const size_t n_datagrams =
            ROC_CONFIG_MAX_SESSION_PACKETS * ROC_CONFIG_MAX_SESSIONS * 2;

        CHECK(n_samples % ReadBufsz == 0);

        CHECK(server->tick(n_datagrams, n_samples / ReadBufsz, ReadBufsz));
    }

    void expect_num_sessions(size_t n_sessions) {
        LONGS_EQUAL(n_sessions, server->num_sessions());
    }
};

TEST(server, no_sessions) {
    SampleStream ss;

    for (size_t n = 0; n < NumPackets; n++) {
        tick(ReadBufsz);
        expect_num_sessions(0);

        ss.read_zeros(output, ReadBufsz);
    }
}

TEST(server, no_parsers) {
    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    SampleStream ss;

    for (size_t n = 0; n < NumPackets; n++) {
        tick(ReadBufsz);
        expect_num_sessions(0);

        ss.read_zeros(output, ReadBufsz);
    }
}

TEST(server, one_session) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    tick(NumPackets * PktSamples);
    expect_num_sessions(1);

    SampleStream ss;
    ss.read(output, NumPackets * PktSamples);
}

TEST(server, one_session_long_run) {
    enum { NumIterations = 10 };

    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    SampleStream ss;

    for (size_t i = 0; i < NumIterations; i++) {
        for (size_t p = 0; p < NumPackets; p++) {
            tick(PktSamples);
            expect_num_sessions(1);

            ss.read(output, PktSamples);
            ps.write(input, 1, PktSamples);
        }
    }
}

TEST(server, wait_min_input_size) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    SampleStream ss;

    for (size_t p = 0; p < NumPackets; p++) {
        tick(PktSamples);
        ss.read_zeros(output, PktSamples);

        ps.write(input, 1, PktSamples);
    }

    tick(NumPackets * PktSamples);
    ss.read(output, NumPackets * PktSamples);
}

TEST(server, wait_min_input_size_timeout) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, 1, PktSamples);

    SampleStream ss;

    for (size_t n = 0; n < Timeout - 1; n++) {
        tick(PktSamples);
        expect_num_sessions(1);

        ss.read_zeros(output, PktSamples);
    }

    tick(PktSamples);
    expect_num_sessions(0);

    ss.read_zeros(output, PktSamples);
}

TEST(server, wait_next_packet_timeout) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    SampleStream ss;

    for (size_t p = 0; p < NumPackets; p++) {
        tick(PktSamples);
        expect_num_sessions(1);

        ss.read(output, PktSamples);
    }

    for (size_t n = 0; n < Timeout; n++) {
        tick(PktSamples);
        expect_num_sessions(1);

        ss.read_zeros(output, PktSamples);
    }

    tick(PktSamples);
    expect_num_sessions(0);

    ss.read_zeros(output, PktSamples);
}

TEST(server, two_sessions_synchronous) {
    add_port(PacketStream::DstPort);

    PacketStream ps1;
    PacketStream ps2;

    ps1.src += 1;
    ps2.src += 2;

    ps1.write(input, NumPackets, PktSamples);
    ps2.write(input, NumPackets, PktSamples);

    tick(NumPackets * PktSamples);
    expect_num_sessions(2);

    SampleStream ss;
    ss.set_sessions(2);
    ss.read(output, NumPackets * PktSamples);
}

TEST(server, two_sessions_overlapping) {
    add_port(PacketStream::DstPort);

    PacketStream ps1;
    ps1.src++;
    ps1.write(input, NumPackets, PktSamples);

    tick(NumPackets * PktSamples);
    expect_num_sessions(1);

    SampleStream ss;
    ss.read(output, NumPackets * PktSamples);

    PacketStream ps2 = ps1;
    ps2.src++;
    ps2.sn += 10;
    ps2.ts += 10 * PktSamples;

    ps1.write(input, NumPackets, PktSamples);
    ps2.write(input, NumPackets, PktSamples);

    tick(NumPackets * PktSamples);
    expect_num_sessions(2);

    ss.set_sessions(2);
    ss.read(output, NumPackets * PktSamples);
}

TEST(server, two_sessions_two_parsers) {
    PacketStream ps1;
    ps1.src += 1;
    ps1.dst += 1;

    PacketStream ps2;
    ps2.src += 2;
    ps2.dst += 2;

    add_port(ps1.dst);
    add_port(ps2.dst);

    ps1.write(input, NumPackets, PktSamples);
    ps2.write(input, NumPackets, PktSamples);

    tick(NumPackets * PktSamples);
    expect_num_sessions(2);

    SampleStream ss;
    ss.set_sessions(2);
    ss.read(output, NumPackets * PktSamples);
}

TEST(server, drop_above_max_sessions) {
    enum { MaxSessions = ROC_CONFIG_MAX_SESSIONS };

    add_port(PacketStream::DstPort);

    for (datagram::port_t n = 0; n < MaxSessions; n++) {
        PacketStream ps;
        ps.src += n;
        ps.write(input, 1, PktSamples);

        tick(PktSamples);
        expect_num_sessions(n + 1);
    }

    expect_num_sessions(MaxSessions);

    PacketStream ps;
    ps.src += MaxSessions;
    ps.write(input, 1, PktSamples);

    tick(PktSamples);
    expect_num_sessions(MaxSessions);

    output.clear();
}

TEST(server, drop_above_max_packets) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, MaxPackets + 1, PktSamples);

    tick(MaxPackets * PktSamples);

    SampleStream ss;
    ss.read(output, MaxPackets * PktSamples);

    ps.write(input, 1, PktSamples);
    tick(PktSamples * 2);

    ss.read_zeros(output, PktSamples);
    ss.advance(PktSamples);

    ss.read(output, PktSamples);
}

TEST(server, seqnum_overflow) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.sn = packet::seqnum_t(-1) - 3;

    ps.write(input, NumPackets, PktSamples);

    tick(NumPackets * PktSamples);

    SampleStream ss;
    ss.read(output, NumPackets * PktSamples);
}

TEST(server, seqnum_reorder) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.sn = 10000;
    ps.ts = 100000;
    ps.value += PktSamples * (NumPackets - 1);

    for (size_t p = NumPackets; p > 0; p--) {
        input.write(ps.make(PktSamples));
        ps.sn--;
        ps.ts -= PktSamples;
        ps.value -= PktSamples;
    }

    tick(NumPackets * PktSamples);

    SampleStream ss;
    ss.read(output, NumPackets * PktSamples);
}

TEST(server, seqnum_drop_late) {
    enum { NumDelayed = 5 };

    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets - NumDelayed, PktSamples);

    // store position of delayed packets
    PacketStream delayed = ps;

    // skip delayed packets now
    ps.sn += NumDelayed;
    ps.ts += NumDelayed * PktSamples;
    ps.value += NumDelayed * PktSamples;

    // write more packets
    ps.write(input, NumPackets, PktSamples);
    tick(NumPackets * PktSamples);

    SampleStream ss;

    // read samples before delayed packets
    ss.read(output, (NumPackets - NumDelayed) * PktSamples);

    // read zeros instead of delayed packets
    ss.read_zeros(output, NumDelayed * PktSamples);
    ss.advance(NumDelayed * PktSamples);

    // write delayed packets
    delayed.write(input, NumDelayed, PktSamples);
    tick(NumPackets * PktSamples * 2);

    // read samples after delayed packets (delayed packets are ignored)
    ss.read(output, NumPackets * PktSamples);

    // ensure there are no more samples
    ss.read_zeros(output, NumPackets * PktSamples);
}

TEST(server, seqnum_ignore_gap) {
    enum { Gap = 33 };

    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    ps.sn += Gap;
    ps.write(input, NumPackets, PktSamples);

    tick(NumPackets * 2 * PktSamples);

    SampleStream ss;
    ss.read(output, NumPackets * 2 * PktSamples);
}

TEST(server, seqnum_shutdown_on_jump) {
    enum { Jump = ROC_CONFIG_MAX_SN_JUMP + 1 };

    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    ps.sn += Jump;
    ps.write(input, NumPackets, PktSamples);

    tick(NumPackets * PktSamples + ReadBufsz);
    expect_num_sessions(1);

    SampleStream ss;

    ss.read(output, NumPackets * PktSamples);
    ss.read_zeros(output, ReadBufsz);

    tick(ReadBufsz);
    expect_num_sessions(0);

    ss.read_zeros(output, ReadBufsz);
}

TEST(server, timestamp_overflow) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.ts = packet::timestamp_t(-1) - 33;

    ps.write(input, NumPackets, PktSamples);

    tick(NumPackets * PktSamples);

    SampleStream ss;
    ss.read(output, NumPackets * PktSamples);
}

TEST(server, timestamp_zeros_on_late) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    packet::timestamp_t late = ps.ts;

    ps.ts += PktSamples;
    ps.value += PktSamples;
    ps.write(input, NumPackets, PktSamples);

    ps.ts = late;
    ps.write(input, 1, PktSamples);

    tick((NumPackets * 3 + 1) * PktSamples);

    SampleStream ss;

    ss.read(output, NumPackets * PktSamples);

    ss.read_zeros(output, PktSamples);
    ss.advance(PktSamples);

    ss.read(output, NumPackets * PktSamples);

    ss.read_zeros(output, NumPackets * PktSamples);
}

TEST(server, timestamp_zeros_on_gap) {
    enum { Gap = 10 };

    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    ps.ts += Gap * PktSamples;
    ps.value += Gap * PktSamples;

    ps.write(input, NumPackets, PktSamples);

    tick((NumPackets * 2 + Gap) * PktSamples);

    SampleStream ss;

    ss.read(output, NumPackets * PktSamples);

    ss.read_zeros(output, Gap * PktSamples);
    ss.advance(Gap * PktSamples);

    ss.read(output, NumPackets * PktSamples);
}

TEST(server, timestamp_overlapping) {
    enum { Overlap = PktSamples / 2 };

    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    ps.ts -= Overlap;
    ps.value -= Overlap;

    ps.write(input, NumPackets, PktSamples);
    ps.write(input, 1, PktSamples - Overlap);

    tick((NumPackets * 2 + 1) * PktSamples);

    SampleStream ss;

    ss.read(output, NumPackets * 2 * PktSamples);
    ss.read_zeros(output, PktSamples);
}

TEST(server, timestamp_shutdown_on_jump) {
    enum { Jump = ROC_CONFIG_MAX_TS_JUMP + 1 };

    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    ps.ts += Jump;
    ps.write(input, NumPackets, PktSamples);

    tick((NumPackets + 1) * PktSamples);
    expect_num_sessions(1);

    SampleStream ss;
    ss.read(output, NumPackets * PktSamples);
    ss.read_zeros(output, PktSamples);

    tick(PktSamples);
    expect_num_sessions(0);

    ss.read_zeros(output, PktSamples);
}

TEST(server, tiny_packets) {
    CHECK(ReadBufsz % 2 == 0);

    enum {
        TinyPacketSamples = ReadBufsz / 2,
        TinyPackets = NumPackets * (PktSamples / TinyPacketSamples)
    };

    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, TinyPackets, TinyPacketSamples);

    tick(TinyPackets * TinyPacketSamples);

    SampleStream ss;
    ss.read(output, TinyPackets * TinyPacketSamples);
}

TEST(server, non_aligned_packets) {
    CHECK(PktSamples % 2 == 0);

    add_port(PacketStream::DstPort);

    PacketStream ps;

    ps.write(input, 1, PktSamples / 2);
    ps.write(input, 1, PktSamples);
    ps.write(input, 1, PktSamples / 2);

    ps.write(input, NumPackets - 2, PktSamples);

    tick(NumPackets * PktSamples);

    SampleStream ss;
    ss.read(output, NumPackets * PktSamples);
}

TEST(server, corrupted_packet_drop_new_session) {
    add_port(PacketStream::DstPort);

    PacketStream ps;

    IDatagramPtr corrupted = ps.make(*new_byte_buffer<1>());

    input.write(corrupted);

    tick(ReadBufsz);
    expect_num_sessions(0);

    IDatagramPtr good = ps.make(1);
    input.write(good);

    tick(ReadBufsz);
    expect_num_sessions(1);

    output.clear();
}

TEST(server, corrupted_packet_ignore_in_existing_session) {
    add_port(PacketStream::DstPort);

    PacketStream ps;
    ps.write(input, NumPackets, PktSamples);

    IDatagramPtr corrupted = ps.make(*new_byte_buffer<1>());
    input.write(corrupted);

    ps.write(input, NumPackets, PktSamples);

    tick(NumPackets * 2 * PktSamples);

    SampleStream ss;
    ss.read(output, NumPackets * 2 * PktSamples);
}

} // namespace test
} // namespace roc