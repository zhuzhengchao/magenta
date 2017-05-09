// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdint.h>
#include <threads.h>

#include <magenta/types.h>
#include <mx/event.h>
#include <mxtl/macros.h>

namespace fuzz {

class Fuzzer;

class StateHandler {
public:
    virtual ~StateHandler();

    // Adds the chain of handlers.  This should only be called through
    // Fuzzer::AddHandler.
    mx_status_t Chain(Fuzzer* fuzzer, StateHandler* chain);

    // Gets the length of state information for just this helper, and not the
    // whole chain.
    virtual size_t GetStateLength() const = 0;

    // Returns the total space needed for managing state with this chain of
    // StateHandlers.
    size_t GetSnapshotLength() const;

    // Consumes state information from |buf|.  Returns an error if |len| is less
    // than |GetLength()| or if the state can't be parsed.  This method is
    // called by |Fuzzer::SendHeartbeats()|.  Others may need to call it if
    // using fuzz::StateHandler without fuzz::Fuzzer.
    mx_status_t Revert(const uint8_t* buf, size_t len);

    // Collects state information and saves it to |buf|.  Returns an error if
    // |len| is less than |GetLength()| or if the state can't be saved.  This
    // method is called by |Fuzzer::Handshake()|.  Others may need to call it if
    // using fuzz::StateHandler without fuzz::Fuzzer.
    mx_status_t Snapshot(uint8_t* buf, size_t len);

protected:
    StateHandler();

    // Gets a pointer to the state handler's mutex.  Derived classes should use
    // this to create an AutoLock in methods that access or modify state., e.g.
    //    AutoLock lock(GetLock());
    mtx_t* GetLock();

    // Collects state information for just this helper, and not the whole chain.
    virtual mx_status_t GetState(uint8_t* buf) const = 0;

    // Consumes state information for just this helper, and not the whole chain.
    virtual mx_status_t SetState(const uint8_t* buf) = 0;

    // Signals the saved event that the state has changed.  This is useful with
    // "slow" starts, where a previous platform crash is being isolated.
    mx_status_t SignalModified();

private:
    DISALLOW_COPY_AND_ASSIGN_ALLOW_MOVE(StateHandler);

    // The fuzzer this handler has been added to.
    Fuzzer* fuzzer_;

    mtx_t lock_;

    // The next link in the helper chain.  This is null if it's the last link.
    StateHandler* next_;
};

} // namespace fuzz
