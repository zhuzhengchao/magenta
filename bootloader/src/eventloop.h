// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

// TODO: Make this dynamic
#define MAX_LOOP_EVENTS 128
#define MAX_LOOP_FILTERS 16

// An eventloop filter is a global filter that inhibits *all* events
// from occuring when it returns true. Consider it a precondition of
// execution.
typedef bool (*eloop_filter)(void *cookie);

typedef void (*eloop_callback)(void *cookie);

// If |repeat_time| is non-zero, execute the callback immediately, and
// every |repeat_time| instances of 100ns (chosen to correspond to UEFI
// timers).
// It |repeat_time| is zero, execute the callback on every iteration
// through the event loop.
bool eloop_add_event(uint64_t repeat_time, eloop_callback callback,
                     void *cookie);
bool eloop_add_filter(eloop_filter callback, void *cookie);
bool eloop_rm_event(eloop_callback callback);
int eloop_start(void);
void eloop_end(int return_value);

