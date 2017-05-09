// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <threads.h>

#include <fuzz/channel.h>
#include <magenta/types.h>
#include <mx/event.h>
#include <mx/process.h>
#include <mxtl/array.h>
#include <mxtl/macros.h>

namespace fuzz {

class Agent {
public:
    virtual ~Agent();

    // Runs the Agent.  This will start the fuzzer process described by |argc|
    // and |argv|, perform the handshake, and enter the message processing loop.
    // This method returns when the fuzzer exits or an error is encountered.
    mx_status_t Run(int argc, const char** argv, uint32_t timeout);

protected:
    Agent();

    // Starts the fuzzer process and passes it the other end of the channel.
    // This method is protected to allow unit tests to override it with a
    // version that does not start a process.
    virtual mx_status_t Launch(int argc, const char** argv, mx_handle_t remote);

    mx_status_t ToStdin(const char* in);
    virtual void OnStdout(const char* str) = 0;
    virtual void OnStderr(const char* str) = 0;

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(Agent);

    static int HandleStdout(void* arg);
    static int HandleStderr(void* arg);

    mx_status_t Handshake(mx_time_t timeout);
    mx_status_t HandleState();
    mx_status_t HandleCrash();

    mx_status_t HandleStdio(int fd);

    // Name of the fuzzer being run.
    const char* argv0_;
    // Control channel to fuzzer
    Channel fuzzer_;
    // Process structure for running fuzzer.
    mx::process proc_;
    // Recorded start time.
    mx_time_t start_;
    // Last reported fuzzer state.
    mxtl::Array<uint8_t> state_;
    // Pipes to/from the fuzzer process.
    int stdin_;
    int stdout_;
    int stderr_;

    thrd_t out_thrd_;
    thrd_t err_thrd_;

    mx::event ready_;
};

} // namespace fuzz
