// Copyright 2017 The Fuchsia Authors. All riusghts reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/usb-hci.h>
#include <magenta/syscalls.h>
#include <stdlib.h>
#include <stdio.h>
#include <threads.h>

#include "usb-virtual-bus.h"

typedef struct {
    mx_device_t* mxdev;
    mx_device_t* hci_dev;
    mx_device_t* client_dev;
} usb_virtual_bus_t;

static void usb_bus_unbind(void* ctx) {
    usb_virtual_bus_t* bus = ctx;

    device_remove(bus->hci_dev);
    device_remove(bus->client_dev);
    device_remove(bus->mxdev);
}

static void usb_bus_release(void* ctx) {
    usb_virtual_bus_t* bus = ctx;
    free(bus);
}

static mx_protocol_device_t usb_virtual_bus_proto = {
    .version = DEVICE_OPS_VERSION,
    .unbind = usb_bus_unbind,
    .release = usb_bus_release,
};

static mx_status_t usb_virtual_bus_bind(void* ctx, mx_device_t* parent, void** cookie) {
printf("usb_virtual_bus_bind\n");
    usb_virtual_bus_t* bus = calloc(1, sizeof(usb_virtual_bus_t));
    if (!bus) {
        return MX_ERR_NO_MEMORY;
    }

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "usb-virtual-bus",
        .ctx = bus,
        .ops = &usb_virtual_bus_proto,
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    mx_status_t status = device_add(parent, &args, &bus->mxdev);
    if (status != MX_OK) {
        free(bus);
        return status;
    }

    mx_handle_t channel_handles[2];
    mx_channel_create(0, &channel_handles[0], &channel_handles[1]);

    bus->hci_dev = usb_virtual_hci_add(bus->mxdev, channel_handles[0]);
    bus->client_dev = usb_virtual_client_add(bus->mxdev, channel_handles[1]);
    
    return MX_OK;
}

static mx_driver_ops_t bus_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = usb_virtual_bus_bind,
};

// clang-format off
MAGENTA_DRIVER_BEGIN(usb_virtual_bus, bus_driver_ops, "magenta", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, MX_PROTOCOL_MISC_PARENT),
MAGENTA_DRIVER_END(usb_virtual_bus)
