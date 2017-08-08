// Copyright 2017 The Fuchsia Authors
//
// Use of this source code is governed by a MIT-style
// license that can be found in the LICENSE file or at
// https://opensource.org/licenses/MIT

#pragma once

#include <magenta/types.h>
typedef uint64_t lk_time_t;

class Event {
public:
    Event(uint32_t opts = 0) {
    }
    ~Event() {
    }

    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;

    // Returns:
    // MX_OK - signaled
    // MX_ERR_TIMED_OUT - time out expired
    // MX_ERR_INTERNAL_INTR_KILLED - thread killed
    // Or the |status| which the caller specified in Event::Signal(status)
    mx_status_t Wait(lk_time_t deadline) {
        return MX_OK;
    }

    // returns number of ready threads
    int Signal(mx_status_t status = MX_OK) {
        return MX_OK;
    }

    mx_status_t Unsignal() {
        return MX_OK;
    }
};
