// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <fuzz/state_handler.h>

#include <stdint.h>

#include <magenta/types.h>
#include <mxtl/macros.h>

namespace fuzz {

class SeededPRNG : public StateHandler {
public:
    SeededPRNG();
    virtual ~SeededPRNG();

    // Gets the length of state information for just this helper, and not the
    // whole chain.
    size_t GetStateLength() const override;

    // Returns pseudorandom bytes.
    mx_status_t Draw(void* buf, size_t len);

protected:
    // Collects state information for just this helper, and not the whole chain.
    mx_status_t GetState(uint8_t* buf) const override;

    // Consumes state information for just this helper, and not the whole chain.
    mx_status_t SetState(const uint8_t* buf) override;

private:
    DISALLOW_COPY_ASSIGN_AND_MOVE(SeededPRNG);

    uint8_t key_[32];
    uint8_t nonce_[12];
    uint32_t counter_;
};

} // namespace fuzz
