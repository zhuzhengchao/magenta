// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/agent.h>

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <threads.h>

#include <fuzz/channel.h>
#include <magenta/types.h>
#include <mx/event.h>
#include <mx/time.h>
#include <unittest/unittest.h>

#include "util.h"

namespace fuzz {

// An implementation of fuzz::Agent that is useful for unit testing that class.
class TestAgent : public Agent {
public:
    static constexpr const char* kArg = "foo";
    static const uint32_t kTimeout = 100;

    static constexpr const char* kFault = "test fault";
    static constexpr size_t kFaultLen = strlen(kFault) + 1;

    TestAgent() {}
    virtual ~TestAgent() {}

    mx_status_t StartTest(Channel* fuzzer) {
        mx_status_t rc;
        fuzzer_ = fuzzer;
        if ((rc = mx::event::create(0, &launch_)) != NO_ERROR) {
            return rc;
        }
        if ((rc = launch_.signal(MX_USER_SIGNAL_ALL, MX_SIGNAL_NONE)) !=
            NO_ERROR) {
            return rc;
        }
        if (thrd_create(&thrd_, AgentThread, this) != thrd_success) {
            return ERR_NO_RESOURCES;
        }
        mx_time_t deadline = mx::time::get(MX_CLOCK_MONOTONIC) + MX_MSEC(100);
        if ((rc = launch_.wait_one(MX_EVENT_SIGNALED, deadline, nullptr)) !=
            NO_ERROR) {
            return rc;
        }
        return NO_ERROR;
    }

    mx_status_t StopTest() {
        int rc;
        thrd_join(thrd_, &rc);
        return rc;
    }

protected:
    void OnStdout(const char* str) override {
        // No-op
    }

    void OnStderr(const char* str) override {
        // No-op
    }

    // Don't start a process.  Just save the channel handle somewhere we can
    // retrieve it.
    mx_status_t Launch(int argc, const char** argv,
                       mx_handle_t remote) override {
        mx_status_t rc = fuzzer_->Connect(remote);
        launch_.signal(MX_SIGNAL_NONE, MX_EVENT_SIGNALED);
        return rc;
    }

private:
    static int AgentThread(void* arg) {
        Agent* agent = static_cast<Agent*>(arg);
        const char* argv = TestAgent::kArg;
        return agent->Run(1, &argv, kTimeout);
    }

    Channel* fuzzer_;
    mx::event launch_;
    thrd_t thrd_;
};

} // namespace fuzz

namespace {

using fuzz::AllocArray;
using fuzz::Channel;
using fuzz::TestAgent;

bool AgentBadArgs() {
    BEGIN_TEST_WITH_RC;
    TestAgent agent;
    const char* argv = TestAgent::kArg;
    ASSERT_RC(agent.Run(0, &argv, 0), ERR_INVALID_ARGS);
    ASSERT_RC(agent.Run(UINT8_MAX + 1, &argv, 0), ERR_INVALID_ARGS);
    ASSERT_RC(agent.Run(1, nullptr, 0), ERR_INVALID_ARGS);
    END_TEST;
}

bool AgentTimeoutOnStart() {
    BEGIN_TEST_WITH_RC;
    TestAgent agent;
    Channel fuzzer;
    // Start agent thread
    ASSERT_RC(agent.StartTest(&fuzzer), NO_ERROR);
    // Agent thread should end with error
    ASSERT_RC(agent.StopTest(), ERR_TIMED_OUT);
    END_TEST;
}

bool AgentCloseOnStart() {
    BEGIN_TEST_WITH_RC;
    TestAgent agent;
    Channel fuzzer;
    // Start agent thread
    ASSERT_RC(agent.StartTest(&fuzzer), NO_ERROR);
    fuzzer.Close();
    // Agent thread should end with error
    ASSERT_RC(agent.StopTest(), ERR_PEER_CLOSED);
    END_TEST;
}

bool AgentTimeoutOnInitialState() {
    BEGIN_TEST_WITH_RC;
    TestAgent agent;
    Channel fuzzer;
    mx_time_t timeout;
    // Start agent thread
    ASSERT_RC(agent.StartTest(&fuzzer), NO_ERROR);
    ASSERT_RC(fuzzer.ReadVal(&timeout, sizeof(timeout)), NO_ERROR);
    // Agent thread should end with error
    ASSERT_RC(agent.StopTest(), ERR_TIMED_OUT);
    END_TEST;
}

bool AgentCloseOnInitialState() {
    BEGIN_TEST_WITH_RC;
    TestAgent agent;
    Channel fuzzer;
    mx_time_t timeout;
    // Start agent thread
    ASSERT_RC(agent.StartTest(&fuzzer), NO_ERROR);
    ASSERT_RC(fuzzer.ReadVal(&timeout, sizeof(timeout)), NO_ERROR);
    fuzzer.Close();
    // Agent thread should end with error
    ASSERT_RC(agent.StopTest(), ERR_PEER_CLOSED);
    END_TEST;
}

bool AgentTimeoutOnStateReply() {
    BEGIN_TEST_WITH_RC;
    TestAgent agent;
    Channel fuzzer;
    mx_time_t timeout;
    mxtl::Array<uint8_t> buf;
    // Start agent thread
    ASSERT_RC(agent.StartTest(&fuzzer), NO_ERROR);
    ASSERT_RC(fuzzer.ReadVal(&timeout, sizeof(timeout)), NO_ERROR);
    ASSERT_RC(fuzzer.ReadBuf(&buf), NO_ERROR);
    // Agent thread should end with error
    ASSERT_RC(agent.StopTest(), ERR_TIMED_OUT);
    END_TEST;
}

bool AgentCloseOnStateReply() {
    BEGIN_TEST_WITH_RC;
    TestAgent agent;
    Channel fuzzer;
    mx_time_t timeout;
    mxtl::Array<uint8_t> buf;
    // Start agent thread
    ASSERT_RC(agent.StartTest(&fuzzer), NO_ERROR);
    ASSERT_RC(fuzzer.ReadVal(&timeout, sizeof(timeout)), NO_ERROR);
    ASSERT_RC(fuzzer.ReadBuf(&buf), NO_ERROR);
    fuzzer.Close();
    // Agent thread should end with error
    ASSERT_RC(agent.StopTest(), ERR_PEER_CLOSED);
    END_TEST;
}

bool AgentTimeoutOnMessage() {
    BEGIN_TEST_WITH_RC;
    TestAgent agent;
    Channel fuzzer;
    mx_time_t timeout;
    mxtl::Array<uint8_t> buf;
    // Start agent thread
    ASSERT_RC(agent.StartTest(&fuzzer), NO_ERROR);
    ASSERT_RC(fuzzer.ReadVal(&timeout, sizeof(timeout)), NO_ERROR);
    ASSERT_RC(fuzzer.ReadBuf(&buf), NO_ERROR);
    // Get current state
    ASSERT_RC(AllocArray(&buf, sizeof(mx_time_t)), NO_ERROR);
    mx_time_t* now = reinterpret_cast<mx_time_t*>(buf.get());
    // Send state
    *now = mx::time::get(MX_CLOCK_MONOTONIC);
    fuzzer.Write(buf.get(), buf.size());
    // Agent thread should end with error
    ASSERT_RC(agent.StopTest(), NO_ERROR);
    // TODO: stderr?
    END_TEST;
}

bool AgentCloseOnMessage() {
    BEGIN_TEST_WITH_RC;
    TestAgent agent;
    Channel fuzzer;
    mx_time_t timeout;
    mxtl::Array<uint8_t> buf;
    // Start agent thread
    ASSERT_RC(agent.StartTest(&fuzzer), NO_ERROR);
    ASSERT_RC(fuzzer.ReadVal(&timeout, sizeof(timeout)), NO_ERROR);
    ASSERT_RC(fuzzer.ReadBuf(&buf), NO_ERROR);
    // Get current state
    ASSERT_RC(AllocArray(&buf, sizeof(mx_time_t)), NO_ERROR);
    mx_time_t* now = reinterpret_cast<mx_time_t*>(buf.get());
    // Send state
    *now = mx::time::get(MX_CLOCK_MONOTONIC);
    ASSERT_RC(fuzzer.Write(buf.get(), buf.size()), NO_ERROR);
    fuzzer.Close();
    // Agent thread should end with error
    ASSERT_RC(agent.StopTest(), NO_ERROR);
    // TODO: stderr?
    END_TEST;
}

} // namespace

BEGIN_TEST_CASE(FuzzAgentTests)
RUN_TEST(AgentBadArgs)
RUN_TEST(AgentTimeoutOnStart)
RUN_TEST(AgentCloseOnStart)
RUN_TEST(AgentTimeoutOnInitialState)
RUN_TEST(AgentCloseOnInitialState)
RUN_TEST(AgentTimeoutOnStateReply)
RUN_TEST(AgentCloseOnStateReply)
RUN_TEST(AgentTimeoutOnMessage)
RUN_TEST(AgentCloseOnMessage)
END_TEST_CASE(FuzzAgentTests)
