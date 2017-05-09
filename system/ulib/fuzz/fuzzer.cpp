// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/fuzzer.h>

#include <stdarg.h>
#include <stddef.h>

#include <fuzz/channel.h>
#include <fuzz/state_handler.h>
#include <magenta/errors.h>
#include <magenta/process.h>
#include <magenta/types.h>
#include <mx/time.h>
#include <mxalloc/new.h>
#include <mxtl/algorithm.h>

namespace fuzz {

// Public methods

Fuzzer::Fuzzer() : monitoring_(false), timeout_(0), handlers_(nullptr) {
    AddHandler(&prng_);
}

Fuzzer::~Fuzzer() {}

mx_status_t Fuzzer::AddHandler(StateHandler* handler) {
    mx_status_t rc;
    if ((rc = handler->Chain(this, handlers_)) != NO_ERROR) {
        return rc;
    }
    handlers_ = handler;
    return NO_ERROR;
}

mx_status_t Fuzzer::Start() {
    mx_handle_t handle = mx_get_startup_handle(Channel::kHandleInfo);
    return Handshake(handle);
}

mx_status_t Fuzzer::Draw(void* buf, size_t len) {
    return prng_.Draw(buf, len);
}

void Fuzzer::SignalModified() {
    if (timeout_ == 0) {
        modified_.signal(MX_SIGNAL_NONE, MX_EVENT_SIGNALED);
    }
}

// Protected methods

mx_status_t Fuzzer::Handshake(mx_handle_t handle) {
    mx_status_t rc;
    // Retrieve the channel handle and ready the 'Start' message.
    if ((rc = agent_.Connect(handle)) != NO_ERROR) {
        return rc;
    }
    // Perform the handshake steps
    timeout_ = 0;
    mx_time_t timeout;
    if ((rc = agent_.ReadVal(&timeout, sizeof(timeout))) != NO_ERROR) {
        return rc;
    }
    // Read the current state from the agent.
    if ((rc = agent_.ReadBuf(&state_)) != NO_ERROR) {
        return rc;
    }
    // If no state, randomly generate one.
    if (state_.size() == 0 &&
        ((rc = AllocArray(&state_, handlers_->GetSnapshotLength())) !=
             NO_ERROR ||
         (rc = Draw(state_.get(), state_.size())) != NO_ERROR)) {
        return rc;
    }
    // Set the state
    if ((rc = handlers_->Revert(state_.get(), state_.size())) != NO_ERROR) {
        return rc;
    }
    // Send the initial state back to the agent.
    if ((rc = agent_.Write(state_.get(), state_.size())) != NO_ERROR) {
        return rc;
    }
    // Create the state modification signal and make sure it's cleared.
    if ((rc = mx::event::create(0, &modified_)) != NO_ERROR) {
        return rc;
    }
    modified_.signal(MX_USER_SIGNAL_ALL, MX_SIGNAL_NONE);
    // Start the heartbeat thread, which will initially wait for a signal.
    if (thrd_create(&heart_, Fuzzer::HeartMonitor, this) != thrd_success) {
        return ERR_NO_RESOURCES;
    }
    monitoring_ = true;
    // This will wake up the heartbeat thread and get it going
    timeout_ = timeout;
    modified_.signal(MX_SIGNAL_NONE, MX_EVENT_SIGNALED);
    return NO_ERROR;
}

mx_status_t Fuzzer::Join() {
    mx_status_t rc;
    if (!monitoring_) {
        return NO_ERROR;
    }
    thrd_join(heart_, &rc);
    return rc;
}

// Private static methods

int Fuzzer::HeartMonitor(void* arg) {
    Fuzzer* fuzzer = static_cast<Fuzzer*>(arg);
    return fuzzer->SendHeartbeats();
}

// Private methods

mx_status_t Fuzzer::SendHeartbeats() {
    mx_status_t rc;
    mx_time_t deadline;
    while (true) {
        deadline =
            (timeout_ == 0 ? MX_TIME_INFINITE
                           : mx::time::get(MX_CLOCK_MONOTONIC) + timeout_);
        // Wait until first of signal or deadline.
        modified_.wait_one(MX_EVENT_SIGNALED, deadline, nullptr);
        // Clear the signal.
        modified_.signal(MX_USER_SIGNAL_ALL, MX_SIGNAL_NONE);
        handlers_->Snapshot(state_.get(), state_.size());
        // Send the state back.
        if ((rc = agent_.Write(state_.get(), state_.size())) != NO_ERROR) {
            return rc;
        }
    }
}

} // namespace fuzz
