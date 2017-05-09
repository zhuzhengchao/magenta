// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/fuzzer.h>

#include <stddef.h>
#include <stdint.h>
#include <threads.h>

#include <fuzz/channel.h>
#include <fuzz/seeded_prng.h>
#include <magenta/types.h>
#include <unittest/unittest.h>

#include "util.h"

namespace fuzz {

// An implementation of fuzz::Fuzzer that is useful for unit testing that class.
class TestFuzzer : public Fuzzer {
public:
    static constexpr const char* kFault = "test fault";
    static constexpr size_t kFaultLen = strlen(kFault) + 1;

    TestFuzzer() : handle_(MX_HANDLE_INVALID) {}
    virtual ~TestFuzzer() {}

    mx_status_t TestHandshake(mx_handle_t handle) { return Handshake(handle); }

    mx_status_t StartTest(Channel* agent) {
        mx_status_t rc;
        if ((rc = agent->Listen(&handle_)) != NO_ERROR) {
            return rc;
        }
        if (thrd_create(&thrd_, HandshakeThread, this) != thrd_success) {
            return ERR_NO_RESOURCES;
        }
        agent->SetTimeout(200);
        return NO_ERROR;
    }

    mx_status_t StopTest(Channel* agent) {
        int rc;
        thrd_join(thrd_, &rc);
        return rc;
    }

private:
    static int HandshakeThread(void* arg) {
        TestFuzzer* fuzzer = static_cast<TestFuzzer*>(arg);
        mx_status_t rc;
        if ((rc = fuzzer->Handshake(fuzzer->handle_)) != NO_ERROR) {
            return rc;
        }
        return fuzzer->Join();
    }

    thrd_t thrd_;
    mx_handle_t handle_;
};

} // namespace fuzz

namespace {

using fuzz::Channel;
using fuzz::SeededPRNG;
using fuzz::TestFuzzer;

bool FuzzerBadHandle() {
    BEGIN_TEST_WITH_RC;
    TestFuzzer fuzzer;
    mx_handle_t handle = MX_HANDLE_INVALID;
    // Try handshake with a bad handle
    ASSERT_RC(fuzzer.TestHandshake(handle), ERR_INVALID_ARGS);
    END_TEST;
}

bool FuzzerTimeoutOnStart() {
    BEGIN_TEST_WITH_RC;
    TestFuzzer fuzzer;
    Channel agent;
    // Start fuzzer thread
    ASSERT_RC(fuzzer.StartTest(&agent), NO_ERROR);
    // Fuzzer thread should end with error
    ASSERT_RC(fuzzer.StopTest(&agent), ERR_TIMED_OUT);
    END_TEST;
}

bool FuzzerCloseOnStart() {
    BEGIN_TEST_WITH_RC;
    TestFuzzer fuzzer;
    Channel agent;
    // Start fuzzer thread
    ASSERT_RC(fuzzer.StartTest(&agent), NO_ERROR);
    agent.Close();
    // Fuzzer thread should end with error
    ASSERT_RC(fuzzer.StopTest(&agent), ERR_PEER_CLOSED);
    END_TEST;
}

bool FuzzerTimeoutOnInitialState() {
    BEGIN_TEST_WITH_RC;
    TestFuzzer fuzzer;
    Channel agent;
    // Start fuzzer thread
    ASSERT_RC(fuzzer.StartTest(&agent), NO_ERROR);
    mx_time_t timeout = 100;
    agent.Write(&timeout, sizeof(timeout));
    // Fuzzer thread should end with error
    ASSERT_RC(fuzzer.StopTest(&agent), ERR_TIMED_OUT);
    END_TEST;
}

bool FuzzerCloseOnInitialState() {
    BEGIN_TEST_WITH_RC;
    TestFuzzer fuzzer;
    Channel agent;
    // Start fuzzer thread
    ASSERT_RC(fuzzer.StartTest(&agent), NO_ERROR);
    mx_time_t timeout = 100;
    agent.Write(&timeout, sizeof(timeout));
    agent.Close();
    // Fuzzer thread should end with error
    ASSERT_RC(fuzzer.StopTest(&agent), ERR_PEER_CLOSED);
    END_TEST;
}

bool FuzzerCloseOnStateReply() {
    BEGIN_TEST_WITH_RC;
    TestFuzzer fuzzer;
    Channel agent;
    // Start fuzzer thread
    ASSERT_RC(fuzzer.StartTest(&agent), NO_ERROR);
    mx_time_t timeout = 100;
    agent.Write(&timeout, sizeof(timeout));
    agent.Write(nullptr, 0);
    agent.Close();
    // Fuzzer thread should end with error
    ASSERT_RC(fuzzer.StopTest(&agent), ERR_PEER_CLOSED);
    END_TEST;
}

bool FuzzerCloseOnMessage() {
    BEGIN_TEST_WITH_RC;
    TestFuzzer fuzzer;
    Channel agent;
    mxtl::Array<uint8_t> buf;
    // Start fuzzer thread
    ASSERT_RC(fuzzer.StartTest(&agent), NO_ERROR);
    mx_time_t timeout = 100;
    agent.Write(&timeout, sizeof(timeout));
    agent.Write(nullptr, 0);
    agent.ReadBuf(&buf);
    agent.Close();
    // Fuzzer thread should end with error
    ASSERT_RC(fuzzer.StopTest(&agent), ERR_PEER_CLOSED);
    END_TEST;
}

} // namespace

BEGIN_TEST_CASE(FuzzFuzzerTests)
RUN_TEST(FuzzerBadHandle)
RUN_TEST(FuzzerTimeoutOnStart)
RUN_TEST(FuzzerCloseOnStart)
RUN_TEST(FuzzerTimeoutOnInitialState)
RUN_TEST(FuzzerCloseOnInitialState)
RUN_TEST(FuzzerCloseOnStateReply)
RUN_TEST(FuzzerCloseOnMessage)
END_TEST_CASE(FuzzFuzzerTests)
