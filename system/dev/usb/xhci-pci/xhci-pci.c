// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/pci.h>
#include <ddk/protocol/usb-xhci.h>

#include <hw/reg.h>
#include <magenta/syscalls.h>
#include <magenta/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    mx_device_t* mxdev;
    pci_protocol_t pci;
    mx_handle_t cfg_handle;
    mx_handle_t mmio_handle;
    void* mmio;
    size_t mmio_length;
    bool legacy_irq_mode;
} xhci_pci_t;

static mx_status_t xhci_pci_get_mmio(void* ctx, void** out_vaddr, size_t* out_length) {
    xhci_pci_t* xhci = ctx;
    *out_vaddr = xhci->mmio;
    *out_length = xhci->mmio_length;
    return MX_OK;
}

static uint32_t xhci_pci_get_interrupt_count(void* ctx) {
    return 1;
}

static mx_status_t xhci_pci_get_interrupt(void* ctx, uint32_t index, mx_handle_t* out_handle) {
    xhci_pci_t* xhci = ctx;
    return pci_map_interrupt(&xhci->pci, index, out_handle);
}

static bool xhci_pci_legacy_irq_mode(void* ctx) {
    xhci_pci_t* xhci = ctx;
    return xhci->legacy_irq_mode;
}

usb_xhci_protocol_ops_t xhci_protocol = {
    .get_mmio = xhci_pci_get_mmio,
    .get_interrupt_count = xhci_pci_get_interrupt_count,
    .get_interrupt = xhci_pci_get_interrupt,
    .legacy_irq_mode = xhci_pci_legacy_irq_mode,
};

static void xhci_pci_unbind(void* ctx) {
    xhci_pci_t* xhci = ctx;
    device_remove(xhci->mxdev);
}

static void xhci_pci_release(void* ctx) {
    xhci_pci_t* xhci = ctx;

    if (xhci->mmio) {
        mx_vmar_unmap(mx_vmar_root_self(), (uintptr_t)xhci->mmio, xhci->mmio_length);
    }
    mx_handle_close(xhci->cfg_handle);
    mx_handle_close(xhci->mmio_handle);
    free(xhci);
}

static mx_protocol_device_t xhci_pci_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .unbind = xhci_pci_unbind,
    .release = xhci_pci_release,
};

static mx_status_t xhci_pci_bind(void* ctx, mx_device_t* dev, void** cookie) {
    mx_status_t status;

    xhci_pci_t* xhci = calloc(1, sizeof(xhci_pci_t));
    if (!xhci) {
        return MX_ERR_NO_MEMORY;
    }

    status = device_get_protocol(dev, MX_PROTOCOL_PCI, &xhci->pci);
    if (status != MX_OK) {
        goto fail;
    }

    /*
     * eXtensible Host Controller Interface revision 1.1, section 5, xhci
     * should only use BARs 0 and 1. 0 for 32 bit addressing, and 0+1 for 64 bit addressing.
     */
    status = pci_map_resource(&xhci->pci, PCI_RESOURCE_BAR_0, MX_CACHE_POLICY_UNCACHED_DEVICE,
                              &xhci->mmio, &xhci->mmio_length, &xhci->mmio_handle);
    if (status != MX_OK) {
        printf("xhci_pci_bind could not find bar\n");
        goto fail;
    }

    // enable bus master
    status = pci_enable_bus_master(&xhci->pci, true);
    if (status != MX_OK) {
        printf("xhci_pci_bind enable_bus_master failed %d\n", status);
        goto fail;
    }

    // select our IRQ mode
    status = pci_set_irq_mode(&xhci->pci, MX_PCIE_IRQ_MODE_MSI, 1);
    if (status != MX_OK) {
        mx_status_t status_legacy = pci_set_irq_mode(&xhci->pci, MX_PCIE_IRQ_MODE_LEGACY, 1);

        if (status_legacy != MX_OK) {
            printf("xhci_pci_bind Failed to set IRQ mode to either MSI "
                   "(err = %d) or Legacy (err = %d)\n",
                   status, status_legacy);
            goto fail;
        }

        xhci->legacy_irq_mode = true;
    }

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "xhci-pci",
        .ctx = xhci,
        .ops = &xhci_pci_device_proto,
        .proto_id = MX_PROTOCOL_USB_XHCI,
        .proto_ops = &xhci_protocol,
    };

    status = device_add(dev, &args, &xhci->mxdev);
    if (status != MX_OK) {
        goto fail;
    }

    return MX_OK;

fail:
    xhci_pci_release(xhci);
    return status;
}

static mx_driver_ops_t xhci_pci_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = xhci_pci_bind,
};

// clang-format off
MAGENTA_DRIVER_BEGIN(xhci_pci, xhci_pci_driver_ops, "magenta", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, MX_PROTOCOL_PCI),
    BI_ABORT_IF(NE, BIND_PCI_CLASS, 0x0C),
    BI_ABORT_IF(NE, BIND_PCI_SUBCLASS, 0x03),
    BI_MATCH_IF(EQ, BIND_PCI_INTERFACE, 0x30),
MAGENTA_DRIVER_END(xhci_pci)
