// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/state_handler.h>

#include <assert.h>
#include <stdint.h>

#include <fuzz/fuzzer.h>
#include <magenta/errors.h>
#include <magenta/types.h>
#include <mxtl/auto_lock.h>

namespace fuzz {

// Public methods

StateHandler::~StateHandler() {}

size_t StateHandler::GetSnapshotLength() const {
    return (!next_ ? 0 : next_->GetSnapshotLength()) + GetStateLength();
}

mx_status_t StateHandler::Snapshot(uint8_t* buf, size_t len) {
    mx_status_t rc;
    assert(fuzzer_);
    mxtl::AutoLock lock(&lock_);
    size_t state_len = GetStateLength();
    if (len < state_len) {
        return ERR_BUFFER_TOO_SMALL;
    }
    if ((rc = GetState(buf)) != NO_ERROR) {
        return rc;
    }
    return (!next_ ? NO_ERROR
                   : next_->Snapshot(buf + state_len, len - state_len));
}

mx_status_t StateHandler::Revert(const uint8_t* buf, size_t len) {
    mx_status_t rc;
    assert(fuzzer_);
    mxtl::AutoLock lock(&lock_);
    size_t state_len = GetStateLength();
    if (len < state_len) {
        return ERR_BUFFER_TOO_SMALL;
    }
    if ((rc = SetState(buf)) != NO_ERROR) {
        return rc;
    }
    return (!next_ ? NO_ERROR
                   : next_->Revert(buf + state_len, len - state_len));
}

// Protected methods

StateHandler::StateHandler() : fuzzer_(nullptr), next_(nullptr) {}

mtx_t* StateHandler::GetLock() {
    if (!fuzzer_) {
        return nullptr;
    }
    return &lock_;
}

mx_status_t StateHandler::Chain(Fuzzer* fuzzer, StateHandler* chain) {
    // Fuzzer must be valid.
    if (!fuzzer) {
        return ERR_INVALID_ARGS;
    }
    // Chain must be null or having a matching parent.
    if (chain && fuzzer != chain->fuzzer_) {
        return ERR_INVALID_ARGS;
    }
    // Must not already be chained.
    if (fuzzer_) {
        return ERR_BAD_STATE;
    }
    // Initialize the lock.
    if (mtx_init(&lock_, mtx_plain) != thrd_success) {
        return ERR_NO_RESOURCES;
    }
    fuzzer_ = fuzzer;
    next_ = chain;
    return NO_ERROR;
}

mx_status_t StateHandler::SignalModified() {
    // Fuzzer must be set
    if (!fuzzer_) {
        return ERR_BAD_STATE;
    }
    fuzzer_->SignalModified();
    return NO_ERROR;
}

} // namespace fuzz
