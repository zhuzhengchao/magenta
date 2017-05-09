// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>

#include <magenta/types.h>
#include <mx/channel.h>
#include <mxalloc/new.h>
#include <mxtl/array.h>
#include <mxtl/macros.h>
#include <mxtl/type_support.h>

namespace fuzz {

class Channel final {
public:
    static const mx_time_t kDefaultTimeout;
    static const uint32_t kHandleInfo;
    static const size_t kMaxMessageLen;

    Channel();
    ~Channel();

    mx_time_t GetLast() const { return last_; }

    mx_time_t GetTimeout() const { return timeout_; }

    void SetTimeout(mx_time_t timeout) { timeout_ = timeout; }

    mx_status_t Listen(mx_handle_t* handle);

    mx_status_t Connect(mx_handle_t handle);

    mx_status_t ReadVal(void* out, size_t len);

    mx_status_t ReadBuf(mxtl::Array<uint8_t>* out);

    mx_status_t Write(const void* buf, size_t len) const;

    void Close();

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(Channel);

    mx::channel channel_;

    mx_time_t last_;

    mx_time_t timeout_;
};

// Helper function for allocating mxtl::Arrays.
template <typename T> mx_status_t AllocArray(mxtl::Array<T>* out, size_t len) {
    // Must have array.
    if (!out) {
        return ERR_INVALID_ARGS;
    }
    // If length is zero, just free the array and return.
    if (len == 0) {
        out->reset();
        return NO_ERROR;
    }
    // Allocate memory; the old memory will be freed upon reset.
    AllocChecker ac;
    T* buf = new (&ac) T[len];
    if (!ac.check()) {
        return ERR_NO_MEMORY;
    }
    out->reset(buf, len);
    return NO_ERROR;
}

} // namespace fuzz
