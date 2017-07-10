// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>

#include <magenta/syscalls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <magenta/compiler.h>

#include "init.h"
#include "ec.h"
#include "pci.h"
#include "powerbtn.h"
#include "processor.h"
#include "resource_tree.h"

#define TRACE 1

#if TRACE
#define xprintf(fmt...) printf(fmt)
#else
#define xprintf(fmt...) \
    do {                \
    } while (0)
#endif

static mx_handle_t rpc[2] = { MX_HANDLE_INVALID, MX_HANDLE_INVALID };

mx_handle_t root_resource_handle;

static mx_protocol_device_t acpi_root_device_proto = {
    .version = DEVICE_OPS_VERSION,
    // TODO ioctls for reboot, ps0, etc.
};

static mx_protocol_device_t acpi_device_proto = {
    .version = DEVICE_OPS_VERSION,
};

static mx_status_t acpi_add_pci_root_device(mx_device_t* parent, const char* name) {
    mx_device_t* dev;
    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = name,
        .ctx = NULL,
        .ops = &acpi_device_proto,
        .proto_id = MX_PROTOCOL_ACPI,
        .flags = DEVICE_ADD_BUSDEV,
    };

    mx_status_t status = device_add(parent, &args, &dev);
    if (status != MX_OK) {
        xprintf("acpi-bus: error %d in device_add\n", status);
    }
    return status;
}

static mx_status_t acpi_drv_bind(void* ctx, mx_device_t* parent, void** cookie) {
    // ACPI is the root driver for its devhost so run init in the bind thread.
    xprintf("bus-acpi: bind\n");
    root_resource_handle = get_root_resource();

    // Initialize the RPC channel
    mx_status_t status = mx_channel_create(0, &rpc[0], &rpc[1]);
    if (status != MX_OK) {
        xprintf("bus-acpi: error %d in mx_channel_create()\n", status);
        return status;
    }

    if (init() != MX_OK) {
        xprintf("bus_acpi: failed to initialize ACPI\n");
        return MX_ERR_INTERNAL;
    }

    printf("acpi-bus: initialized\n");

    mx_handle_t port;
    status = mx_port_create(0, &port);
    if (status != MX_OK) {
        xprintf("acpi-bus: error %d in mx_port_create\n", status);
        return status;
    }

    ec_init();

    status = install_powerbtn_handlers();
    if (status != MX_OK) {
        xprintf("acpi-bus: error %d in install_powerbtn_handlers\n", status);
        return status;
    }

    // Report current resources to kernel PCI driver
    status = pci_report_current_resources(get_root_resource());
    if (status != MX_OK) {
        xprintf("acpi-bus: WARNING: ACPI failed to report all current resources!\n");
    }

    // Initialize kernel PCI driver
    mx_pci_init_arg_t* arg;
    uint32_t arg_size;
    status = get_pci_init_arg(&arg, &arg_size);
    if (status != MX_OK) {
        xprintf("acpi-bus: erorr %d in get_pci_init_arg\n", status);
        return status;
    }

    status = mx_pci_init(get_root_resource(), arg, arg_size);
    if (status != MX_OK) {
        xprintf("acpi-bus: error %d in mx_pci_init\n", status);
        return status;
    }

    free(arg);

#if 0
    // Launch event loop
    status = begin_processing(rpc[0]);
    if (status != MX_OK) {
        xprintf("acpi-bus: error %d starting event loop\n", status);
        return status;
    }
#endif

    // Publish ACPI control device
    mx_device_t* acpidev;
    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "acpi",
        .ctx = NULL,
        .ops = &acpi_root_device_proto,
        .proto_id = MX_PROTOCOL_ACPI_BUS,
    };

    status = device_add(parent, &args, &acpidev);
    if (status != MX_OK) {
        xprintf("acpi-bus: error %d in device_add\n", status);
        return status;
    }

    // Publish PCI root device
    // TODO: publish other ACPI devices
    return acpi_add_pci_root_device(acpidev, "pci-root");
}

static mx_status_t acpi_drv_create(void* ctx, mx_device_t* parent,
                                   const char* name, const char* args, mx_handle_t resource) {
    xprintf("acpi_drv_create: name=%s\n", name);
    return acpi_add_pci_root_device(parent, name);
}

static mx_driver_ops_t acpi_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = acpi_drv_bind,
    .create = acpi_drv_create,
};

MAGENTA_DRIVER_BEGIN(acpi, acpi_driver_ops, "magenta", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, MX_PROTOCOL_ROOT),
MAGENTA_DRIVER_END(acpi)
