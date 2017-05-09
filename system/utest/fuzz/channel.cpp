// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/channel.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <magenta/types.h>
#include <unittest/unittest.h>

#include "util.h"

namespace {

using fuzz::Channel;

// Helper method to (re-)initialize the channels for testing.
bool ChannelReset(Channel* rx, Channel* tx) {
    mx_handle_t handle = MX_HANDLE_INVALID;
    if (!rx || !tx) {
        return false;
    }
    rx->Close();
    tx->Close();
    if (rx->Listen(&handle) != NO_ERROR || tx->Connect(handle) != NO_ERROR) {
        return false;
    }
    rx->SetTimeout(100);
    return true;
}

bool ChannelListen() {
    BEGIN_TEST_WITH_RC;
    mx_handle_t handle = MX_HANDLE_INVALID;
    Channel rx, tx;
    // Listen without capturing handle
    ASSERT_RC(rx.Listen(nullptr), ERR_INVALID_ARGS);
    // Listen correctly
    ASSERT_RC(rx.Listen(&handle), NO_ERROR);
    // Listen with handle in use
    ASSERT_RC(tx.Listen(&handle), ERR_INVALID_ARGS);
    // Listen with channel connected
    handle = MX_HANDLE_INVALID;
    ASSERT_RC(rx.Listen(&handle), ERR_BAD_STATE);
    END_TEST;
}

bool ChannelConnect() {
    BEGIN_TEST_WITH_RC;
    mx_handle_t handle = MX_HANDLE_INVALID;
    Channel rx, tx;
    // Connect to invalid handle
    ASSERT_RC(tx.Connect(handle), ERR_INVALID_ARGS);
    // Listen and connect
    ASSERT_RC(rx.Listen(&handle), NO_ERROR);
    ASSERT_RC(tx.Connect(handle), NO_ERROR);
    // Connect to handle in use
    ASSERT_RC(tx.Connect(handle), ERR_BAD_STATE);
    END_TEST;
}

bool ChannelWrite() {
    BEGIN_TEST_WITH_RC;
    Channel rx, tx;
    uint8_t buf[Channel::kMaxMessageLen + 1];
    memset(buf, 0xff, sizeof(buf));
    ASSERT_TRUE(ChannelReset(&rx, &tx), "channel reset failed");
    // Send a zero length buffer.
    ASSERT_RC(tx.Write(nullptr, 0), NO_ERROR);
    // Send a nonzero length buffer without data
    ASSERT_RC(tx.Write(nullptr, 1), ERR_INVALID_ARGS);
    // Send a too large buffer
    ASSERT_RC(tx.Write(buf, sizeof(buf)), ERR_OUT_OF_RANGE);
    // Send a max length buffer
    ASSERT_RC(tx.Write(buf, Channel::kMaxMessageLen), NO_ERROR);
    END_TEST;
}

bool ChannelReadValue() {
    BEGIN_TEST_WITH_RC;
    Channel rx, tx;
    uint8_t u8 = 8;
    uint64_t u64 = 64;
    // Read without connecting
    ASSERT_RC(rx.ReadVal(&u8, sizeof(u8)), ERR_BAD_STATE);
    // Read without writing
    ASSERT_TRUE(ChannelReset(&rx, &tx), "channel reset failed");
    ASSERT_RC(rx.ReadVal(&u8, sizeof(u8)), ERR_TIMED_OUT);
    // Read without output
    ASSERT_TRUE(ChannelReset(&rx, &tx), "channel reset failed");
    ASSERT_RC(tx.Write(&u8, sizeof(u8)), NO_ERROR);
    ASSERT_RC(rx.ReadVal(nullptr, sizeof(u8)), ERR_INVALID_ARGS);
    // Read a message with the wrong size
    ASSERT_TRUE(ChannelReset(&rx, &tx), "channel reset failed");
    ASSERT_RC(tx.Write(&u64, sizeof(u64)), NO_ERROR);
    ASSERT_RC(rx.ReadVal(&u8, sizeof(u8)), ERR_IO);
    // Read a valid message
    ASSERT_TRUE(ChannelReset(&rx, &tx), "channel reset failed");
    ASSERT_RC(tx.Write(&u8, sizeof(u8)), NO_ERROR);
    ASSERT_RC(rx.ReadVal(&u8, sizeof(u8)), NO_ERROR);
    // Check the received value
    ASSERT_EQ(u8, 8, "unexpected value");
    END_TEST;
}

bool ChannelReadBuffer() {
    BEGIN_TEST_WITH_RC;
    Channel rx, tx;
    uint8_t buf[Channel::kMaxMessageLen];
    memset(buf, 0xff, sizeof(buf));
    mxtl::Array<uint8_t> out;
    // Read without connecting
    ASSERT_RC(rx.ReadBuf(&out), ERR_BAD_STATE);
    // Read without writing
    ASSERT_TRUE(ChannelReset(&rx, &tx), "channel reset failed");
    ASSERT_RC(rx.ReadBuf(&out), ERR_TIMED_OUT);
    // Read with missing fields
    ASSERT_TRUE(ChannelReset(&rx, &tx), "channel reset failed");
    ASSERT_RC(tx.Write(buf, sizeof(buf)), NO_ERROR);
    ASSERT_RC(rx.ReadBuf(nullptr), ERR_INVALID_ARGS);
    // Read with matching opcode mask
    ASSERT_TRUE(ChannelReset(&rx, &tx), "channel reset failed");
    ASSERT_RC(tx.Write(buf, sizeof(buf)), NO_ERROR);
    ASSERT_RC(rx.ReadBuf(&out), NO_ERROR);
    // Check the received buffer
    ASSERT_EQ(out.size(), sizeof(buf), "unexpected buffer length");
    // hexdump8(out.get(), out.size());
    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(out[i], 0xff, "unexpected buffer contents");
    }
    END_TEST;
}

bool ChannelEndToEnd() {
    BEGIN_TEST_WITH_RC;
    Channel server, client;
    uint8_t buf[32];
    memset(buf, 0xff, sizeof(buf));
    uint32_t n = sizeof(buf);
    mxtl::Array<uint8_t> out;
    // Send and receive the 'start' message
    ASSERT_TRUE(ChannelReset(&server, &client), "channel reset failed");
    ASSERT_RC(server.Write(&n, sizeof(n)), NO_ERROR);
    ASSERT_RC(client.ReadVal(&n, sizeof(n)), NO_ERROR);
    ASSERT_EQ(n, sizeof(buf), "unexpected value");
    // Send and receive the 'stop' message
    n /= 2;
    ASSERT_RC(client.Write(buf, n), NO_ERROR);
    ASSERT_RC(server.ReadBuf(&out), NO_ERROR);
    // Check the received buffer
    ASSERT_EQ(out.size(), n, "unexpected buffer length");
    for (size_t i = 0; i < out.size(); ++i) {
        ASSERT_EQ(out[i], 0xff, "unexpected buffer contents");
    }
    END_TEST;
}

} // namespace

BEGIN_TEST_CASE(FuzzChannelTests)
RUN_TEST(ChannelListen)
RUN_TEST(ChannelConnect)
RUN_TEST(ChannelWrite)
RUN_TEST(ChannelReadValue)
RUN_TEST(ChannelReadBuffer)
RUN_TEST(ChannelEndToEnd)
END_TEST_CASE(FuzzChannelTests)
