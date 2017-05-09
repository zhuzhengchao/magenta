// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/channel.h>

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

#include <magenta/errors.h>
#include <magenta/processargs.h>
#include <magenta/types.h>
#include <mx/channel.h>
#include <mx/time.h>

namespace fuzz {

const mx_time_t Channel::kDefaultTimeout = MX_MSEC(200);
const uint32_t Channel::kHandleInfo = PA_HND(PA_USER0, 0);
const size_t Channel::kMaxMessageLen = 0x10000;
static_assert(Channel::kMaxMessageLen < UINT32_MAX,
              "max msg len must fit in uint32_t");

namespace {

mx_time_t Deadline(mx_time_t timeout) {
    return (timeout == 0 ? MX_TIME_INFINITE
                         : mx::time::get(MX_CLOCK_MONOTONIC) + timeout);
}

mx_status_t Read(const mx::channel& rx, void* buf, size_t len,
                 mx_time_t deadline) {
    mx_status_t rc;
    // Exit early if nothing to do.
    if (len == 0) {
        return NO_ERROR;
    }
    // Must have buffer if length is not zero.
    if (!buf) {
        return ERR_INVALID_ARGS;
    }
    // Must be connected.
    if (!rx) {
        return ERR_BAD_STATE;
    }
    // Limit message size
    if (len > Channel::kMaxMessageLen) {
        return ERR_OUT_OF_RANGE;
    }
    uint32_t len32 = static_cast<uint32_t>(len);
    // Wait for the channel to become readable
    mx_time_t interval = mx::time::get(MX_CLOCK_MONOTONIC);
    while (true) {
        rc = rx.read(0, buf, len32, nullptr, nullptr, 0, nullptr);
        // Exit unless we need to wait
        if (rc != ERR_SHOULD_WAIT) {
            return rc;
        }
        // Limit polling to 10/second.
        interval = mx::time::get(MX_CLOCK_MONOTONIC) + MX_MSEC(100);
        rc = rx.wait_one(MX_CHANNEL_READABLE, interval, nullptr);
        if (rc == NO_ERROR) {
            continue;
        }
        if (rc != ERR_TIMED_OUT) {
            return rc;
        }
        if (deadline < interval) {
            return ERR_TIMED_OUT;
        }
    }
}

} // namespace

Channel::Channel() : last_(0), timeout_(kDefaultTimeout) {}

Channel::~Channel() {}

mx_status_t Channel::Listen(mx_handle_t* handle) {
    mx_status_t rc;
    // Must have somewhere to save the other end.
    if (!handle || *handle != MX_HANDLE_INVALID) {
        return ERR_INVALID_ARGS;
    }
    // Must not already be connected.
    if (channel_) {
        return ERR_BAD_STATE;
    }
    // Create the channel.
    mx::channel remote;
    if ((rc = mx::channel::create(0, &channel_, &remote)) != NO_ERROR) {
        return rc;
    }
    *handle = remote.release();
    last_ = mx::time::get(MX_CLOCK_MONOTONIC);
    return NO_ERROR;
}

mx_status_t Channel::Connect(mx_handle_t handle) {
    // Must be a valid handle.
    if (handle == MX_HANDLE_INVALID) {
        return ERR_INVALID_ARGS;
    }
    // Must not already be connected.
    if (channel_) {
        return ERR_BAD_STATE;
    }
    channel_.reset(handle);
    last_ = mx::time::get(MX_CLOCK_MONOTONIC);
    return NO_ERROR;
}

mx_status_t Channel::ReadVal(void* out, size_t len) {
    mx_status_t rc;
    // Get the envelope.
    mx_time_t deadline = Deadline(timeout_);
    uint32_t len32;
    if ((rc = Read(channel_, &len32, sizeof(len32), deadline)) != NO_ERROR) {
        return rc;
    }
    // Check the opcode and length are what we expected.
    if (len != len32) {
        return ERR_IO;
    }
    // Read the data.
    if ((rc = Read(channel_, out, len, deadline)) != NO_ERROR) {
        return rc;
    }
    last_ = mx::time::get(MX_CLOCK_MONOTONIC);
    return NO_ERROR;
}

mx_status_t Channel::ReadBuf(mxtl::Array<uint8_t>* out) {
    mx_status_t rc;
    // Must have output variables.
    if (!out) {
        return ERR_INVALID_ARGS;
    }
    // Get the envelope.
    mx_time_t deadline = Deadline(timeout_);
    uint32_t len;
    if ((rc = Read(channel_, &len, sizeof(len), deadline)) != NO_ERROR) {
        return rc;
    }
    // Dynamically allocate the requested space (possibly 0).
    if ((rc = AllocArray(out, len)) != NO_ERROR) {
        return rc;
    }
    // Read the data.
    if ((rc = Read(channel_, out->get(), len, deadline)) != NO_ERROR) {
        return rc;
    }
    last_ = mx::time::get(MX_CLOCK_MONOTONIC);
    return NO_ERROR;
}

mx_status_t Channel::Write(const void* buf, size_t len) const {
    mx_status_t rc;
    // Must have data or zero length.
    if (!buf && len != 0) {
        return ERR_INVALID_ARGS;
    }
    // Must be connected.
    if (!channel_) {
        return ERR_BAD_STATE;
    }
    // Limit message size
    if (len > kMaxMessageLen) {
        return ERR_OUT_OF_RANGE;
    }
    // Send the envelope
    uint32_t len32 = static_cast<uint32_t>(len);
    if ((rc = channel_.write(0, &len32, sizeof(len32), nullptr, 0)) !=
        NO_ERROR) {
        return rc;
    }
    // Done if no body.
    if (len == 0) {
        return NO_ERROR;
    }
    return channel_.write(0, buf, len32, nullptr, 0);
}

void Channel::Close() {
    channel_.reset();
}

} // namespace fuzz
