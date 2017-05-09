// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>
#include <threads.h>

#include <fuzz/channel.h>
#include <fuzz/seeded_prng.h>
#include <fuzz/state_handler.h>
#include <magenta/types.h>
#include <mx/event.h>
#include <mxtl/array.h>
#include <mxtl/macros.h>

namespace fuzz {

class Fuzzer {
public:
    Fuzzer();
    virtual ~Fuzzer();

    // Sets a state helper.  This must be called before |ReceiveStart|.  It will
    // consume state from a 'Start' message and gather it for a 'Heartbeat'
    // message every heartbeat period.
    mx_status_t AddHandler(StateHandler* handler);

    // Connects to the agent, performs the handshake, and starts the heartbeat
    // monitor thread.
    mx_status_t Start();

    mx_status_t Draw(void* buf, size_t len);

    // Notifies the fuzzer that the state managed by one or more of the handlers
    // has changed.  If this fuzzer is running with a timeout of 0, this
    // triggers sending a state message.
    void SignalModified();

    mx_status_t SendFault(const void* buf, size_t len);

protected:
    // Performs the handshake with the agent.  This will start the heart monitor
    // thread.  This method is protected to allow unit tests to call it with a
    // provided handle, instead of a startup handle.
    mx_status_t Handshake(mx_handle_t handle);

    mx_status_t Join();

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(Fuzzer);

    static int HeartMonitor(void* arg);

    mx_status_t SendHeartbeats();

    Channel agent_;

    thrd_t heart_;

    bool monitoring_;

    mxtl::Array<uint8_t> state_;

    mx::event modified_;

    mx_time_t timeout_;

    SeededPRNG prng_;

    // One or more state helpers chained together.
    StateHandler* handlers_;
};

} // namespace fuzz
