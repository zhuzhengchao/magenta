// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/protocol/platform-devices.h>
#include <hw/reg.h>
#include <pretty/hexdump.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "dwc3.h"
#include "dwc3-regs.h"
#include "dwc3-types.h"

// MMIO indices
enum {
    MMIO_USB3OTG,
};

// IRQ indices
enum {
    IRQ_USB3,
};

static void dwc3_wait_bits(volatile uint32_t* ptr, uint32_t bits, uint32_t expected) {
    uint32_t value = readl(ptr);
    while ((value & bits) != expected) {
        usleep(1000);
        value = readl(ptr);
    }
}

static mx_status_t dwc3_start(usb_dwc3_t* dwc) {
printf("dwc3_start\n");
    // set device mode
    volatile void* mmio = dwc->mmio.vaddr;
    uint32_t temp = readl(mmio + GCTL);
    temp &= ~GCTL_PRTCAPDIR_MASK;
    temp |= GCTL_PRTCAPDIR_HOST;
    writel(temp, mmio + GCTL);

    temp = readl(mmio + DCTL);
    temp |= DCTL_CSFTRST;
    writel(temp, mmio + DCTL);
    dwc3_wait_bits(mmio + DCTL, DCTL_CSFTRST, 0);

    printf("global registers:\n");
    hexdump(dwc->mmio.vaddr + GSBUSCFG0, 256);
    printf("device registers:\n");
    hexdump(dwc->mmio.vaddr + DCFG, 256);

    return MX_OK;
}

static mx_status_t dwc_set_interface(void* ctx, usb_dci_interface_t* dci_intf) {
    usb_dwc3_t* dwc = ctx;
    memcpy(&dwc->dci_intf, dci_intf, sizeof(dwc->dci_intf));
    return MX_OK;
}

static mx_status_t dwc_config_ep(void* ctx, const usb_endpoint_descriptor_t* ep_desc) {
    return MX_OK;
}

static mx_status_t dwc_set_enabled(void* ctx, bool enabled) {
    usb_dwc3_t* dwc = ctx;

    if (enabled) {
        return dwc3_start(dwc);
    } else {
        // TODO(voydanoff) disable
        return MX_OK;
    }
}

usb_dci_protocol_ops_t dwc_dci_protocol = {
    .set_interface = dwc_set_interface,
    .config_ep = dwc_config_ep,
    .set_enabled = dwc_set_enabled,
};

static void dwc3_unbind(void* ctx) {
    usb_dwc3_t* dwc = ctx;

    mx_interrupt_signal(dwc->irq_handle);
    thrd_join(dwc->irq_thread, NULL);
    device_remove(dwc->mxdev);
}

static void dwc3_release(void* ctx) {
    usb_dwc3_t* dwc = ctx;
    pdev_mmio_buffer_release(&dwc->mmio);
    mx_handle_close(dwc->irq_handle);
    free(dwc);
}

static mx_protocol_device_t dwc3_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .release = dwc3_release,
};

static mx_status_t dwc3_bind(void* ctx, mx_device_t* dev, void** cookie) {
    printf("dwc3_bind\n");

    usb_dwc3_t* dwc = calloc(1, sizeof(usb_dwc3_t));
    if (!dwc) {
        return MX_ERR_NO_MEMORY;
    }

    platform_device_protocol_t pdev;
    mx_status_t status = device_get_protocol(dev, MX_PROTOCOL_PLATFORM_DEV, &pdev);
    if (status != MX_OK) {
        goto fail;
    }

    status = pdev_map_mmio_buffer(&pdev, MMIO_USB3OTG, MX_CACHE_POLICY_UNCACHED_DEVICE,
                                  &dwc->mmio);
    if (status != MX_OK) {
        goto fail;
    }

    status = pdev_map_interrupt(&pdev, IRQ_USB3, &dwc->irq_handle);
    if (status != MX_OK) {
        goto fail;
    }

    status = dwc3_events_init(dwc);
    if (status != MX_OK) {
        goto fail;
    }

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "dwc3",
        .ctx = dwc,
        .ops = &dwc3_device_proto,
        .proto_id = MX_PROTOCOL_USB_DCI,
        .proto_ops = &dwc_dci_protocol,
    };

    status = device_add(dev, &args, &dwc->mxdev);
    if (status != MX_OK) {
        goto fail;
    }

    dwc3_events_start(dwc);

    // start the controller
    volatile void* mmio = dwc->mmio.vaddr;
    DWC3_WRITE32(mmio + DCTL, DCTL_RUN_STOP);

    return MX_OK;

fail:
    printf("dwc3_bind failed %d\n", status);
    dwc3_release(dwc);
    return status;
}

static mx_driver_ops_t dwc3_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = dwc3_bind,
};

// The formatter does not play nice with these macros.
// clang-format off
MAGENTA_DRIVER_BEGIN(dwc3, dwc3_driver_ops, "magenta", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, PDEV_VID_GENERIC),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, PDEV_PID_GENERIC),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, PDEV_DID_USB_DWC3),
MAGENTA_DRIVER_END(dwc3)
// clang-format on
