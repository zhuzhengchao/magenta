// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <magenta/compiler.h>
#include <magenta/process.h>
#include <magenta/types.h>
#include <stdbool.h>

__BEGIN_CDECLS;

typedef struct {
    mx_status_t (*get_mmio)(void* ctx, void** out_vaddr, size_t* out_length);
    uint32_t (*get_interrupt_count)(void* ctx);
    mx_status_t (*get_interrupt)(void* ctx, uint32_t index, mx_handle_t* out_handle);
    bool (*legacy_irq_mode)(void* ctx);
} usb_xhci_protocol_ops_t;

typedef struct {
    usb_xhci_protocol_ops_t* ops;
    void* ctx;
} usb_xhci_protocol_t;

// returns pointer and size to XHCI MMIO region.
// parent device is responsible for unmapping.
static inline mx_status_t usb_xhci_get_mmio(usb_xhci_protocol_t* xhci, void** out_vaddr,
                                            size_t* out_length) {
    return xhci->ops->get_mmio(xhci->ctx, out_vaddr, out_length);
}

// returns number of interrupts supported
static inline uint32_t usb_xhci_get_interrupt_count(usb_xhci_protocol_t* xhci) {
    return xhci->ops->get_interrupt_count(xhci->ctx);
}

// returns an interrupt handle for the specified interrupt index
// caller takes ownership of the handle
static inline mx_status_t usb_xhci_get_interrupt(usb_xhci_protocol_t* xhci, uint32_t index,
                                                 mx_handle_t* out_handle) {
    return xhci->ops->get_interrupt(xhci->ctx, index, out_handle);
}

// returns true if we are in PCI legacy mode
static inline bool usb_xhci_legacy_irq_mode(usb_xhci_protocol_t* xhci) {
    return xhci->ops->legacy_irq_mode(xhci->ctx);
}

__END_CDECLS;
