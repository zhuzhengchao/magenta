// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <string.h>

#include <xefi.h>

#include "eventloop.h"

// It would certainly be possible to allocate eventloops dynamically, to
// support multiple instances. However, it's unlikely we will need this
// and memory allocation is a bit painful. Perhaps for a more general
// implementation, someday...

static bool loop_done;
static int loop_return;

typedef struct loop_event {
    enum {LE_DISABLED, LE_ENABLED, LE_ALWAYS} status;
    efi_event timer_event;
    eloop_callback callback;
    void *cookie;
} el_event_t;

static el_event_t loop_events[MAX_LOOP_EVENTS];
static size_t allocated_events = 0;

typedef struct loop_filter {
    eloop_filter callback;
    void *cookie;
} el_filter_t;

static el_filter_t loop_filters[MAX_LOOP_FILTERS];
static size_t allocated_filters = 0;

static void eloop_cleanup(void) {
    while (allocated_events) {
        allocated_events--;
        gBS->CloseEvent(loop_events[allocated_events].timer_event);
    }
    allocated_filters = 0;
}

// repeat_time is in 100ns units, to match UEFI timers
bool eloop_add_event(uint64_t repeat_time, eloop_callback callback,
                     void *cookie) {
    if (allocated_events >= MAX_LOOP_EVENTS) {
        printf("eventloop: Attempt to allocate more than %d events\n",
               MAX_LOOP_EVENTS);
        return false;
    }

    if (repeat_time == 0) {
        loop_events[allocated_events].status = LE_ALWAYS;
    } else {
        // Recurring timer events are first executed immediately
        callback(cookie);

        efi_status status;
        status = gBS->CreateEvent(EVT_TIMER, TPL_APPLICATION, NULL, NULL,
                                  &loop_events[allocated_events].timer_event);
        if (status != EFI_SUCCESS) {
            printf("eventloop: Unable to create timer event\n");
            return false;
        }

        status = gBS->SetTimer(loop_events[allocated_events].timer_event,
                               TimerPeriodic, repeat_time);
        if (status != EFI_SUCCESS) {
            printf("eventloop: Unable to set timer\n");
            gBS->CloseEvent(loop_events[allocated_events].timer_event);
            return false;
        }

        loop_events[allocated_events].status = LE_ENABLED;
    }
 
    loop_events[allocated_events].callback = callback;
    loop_events[allocated_events].cookie = cookie;
    allocated_events++;
    return true;
}

bool eloop_add_filter(eloop_filter callback, void *cookie) {
    if (allocated_filters >= MAX_LOOP_FILTERS) {
        printf("eventloop: Attempt to allocate more than %d filters\n",
               MAX_LOOP_FILTERS);
        return false;
    }
    loop_filters[allocated_filters].callback = callback;
    loop_filters[allocated_filters].cookie = cookie;
    return true;
}

static void rm_event(size_t ndx) {
    gBS->CloseEvent(loop_events[ndx].timer_event);
    if (ndx < (allocated_events - 1))
        memmove(&loop_events[ndx], &loop_events[ndx + 1],
                ((allocated_events - ndx) + 1) * sizeof(el_event_t));
    allocated_events--;
}

static void reap_events(void) {
    size_t ndx = 0;
    while (ndx < allocated_events) {
        if (loop_events[ndx].status == LE_DISABLED) {
            rm_event(ndx);
            continue;
        }
        ndx++;
    }
}

bool eloop_rm_event(eloop_callback callback) {
    size_t ndx;
    for (ndx = 0; ndx < allocated_events; ndx++) {
        if (loop_events[ndx].callback == callback) {
            loop_events[ndx].status = LE_DISABLED;
            return true;
        }
    }
    return false;
}

// Our main loop body
int eloop_start(void) {
    loop_done = false;
    while (! loop_done) {
        size_t ndx;
        bool skip_iteration = false;
        for (ndx = 0; ndx < allocated_filters; ndx++) {
            if (loop_filters[ndx].callback(loop_filters[ndx].cookie))
                skip_iteration = true;
        }

        if (!skip_iteration) {
            for (ndx = 0; ndx < allocated_events; ndx++) {
                if (loop_events[ndx].status == LE_DISABLED)
                    continue;
                if (loop_events[ndx].status == LE_ALWAYS ||
                    gBS->CheckEvent(loop_events[ndx].timer_event) ==
                        EFI_SUCCESS)
                    loop_events[ndx].callback(loop_events[ndx].cookie);
            }
        }
        reap_events();
    }
    eloop_cleanup();
    return loop_return;
}

void eloop_end(int return_value) {
    loop_done = true;
    loop_return = return_value;
}

