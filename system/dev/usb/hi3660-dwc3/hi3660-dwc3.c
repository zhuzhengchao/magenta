// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/protocol/platform-device.h>
#include <ddk/protocol/usb-xhci.h>
#include <hw/reg.h>
#include <pretty/hexdump.h>

#include <stdlib.h>
#include <stdio.h>

#include "dwc3-regs.h"
#include "hi3660-regs.h"

// MMIO indices
enum {
    MMIO_USB3OTG,
    MMIO_USB3OTG_BC,
    MMIO_PERI_CRG,
    MMIO_PCTRL,
    MMIO_SCTRL,
    MMIO_PMCTRL,
};

// IRQ indices
enum {
    IRQ_USB3,
    IRQ_USB3_OTG,
    IRQ_USB3_BC,
};

typedef struct {
    mx_device_t* mxdev;
    platform_device_protocol_t pdev;
    pdev_mmio_buffer_t usb3otg;
    pdev_mmio_buffer_t usb3otg_bc;
    pdev_mmio_buffer_t peri_crg;
    pdev_mmio_buffer_t pctrl;
    pdev_mmio_buffer_t sctrl;
    pdev_mmio_buffer_t pmctrl;
} hi3360_dwc3_t;


static mx_status_t hi3360_dwc3_init(hi3360_dwc3_t* dwc) {

    volatile void* usb3otg_bc = dwc->usb3otg_bc.vaddr;
    volatile void* peri_crg = dwc->peri_crg.vaddr;
    volatile void* pctrl = dwc->pctrl.vaddr;
 
	writel(PERRSTEN4_USB3OTG, peri_crg + PERI_CRG_PERRSTEN4);
	writel(PERRSTEN4_USB3OTGPHY_POR, peri_crg + PERI_CRG_PERRSTEN4);
	writel(PERRSTEN4_USB3OTG_MUX | PERRSTEN4_USB3OTG_AHBIF | PERRSTEN4_USB3OTG_32K, peri_crg + PERI_CRG_PERRSTEN4);
	writel(PEREN4_GT_ACLK_USB3OTG | PEREN4_GT_CLK_USB3OTG_REF, peri_crg + PERI_CRG_PERDIS4);

	writel(~PCTRL_CTRL24_USB3PHY_3MUX1_SEL, pctrl + PCTRL_CTRL24);
	writel((PCTRL_CTRL3_USB_TXCO_EN << 16) | 0, pctrl + PCTRL_CTRL3);

    mx_nanosleep(mx_deadline_after(MX_MSEC(10)));

// release part
    mx_nanosleep(mx_deadline_after(MX_MSEC(10)));

   /* enable USB REFCLK ISO */
	writel(PERISOEN_USB_REFCLK_ISO_EN, peri_crg + PERI_CRG_ISODIS);
 
  /* enable USB_TXCO_EN */
	writel((PCTRL_CTRL3_USB_TXCO_EN << 16) | PCTRL_CTRL3_USB_TXCO_EN, pctrl + PCTRL_CTRL3);

	writel(~PCTRL_CTRL24_USB3PHY_3MUX1_SEL, pctrl + PCTRL_CTRL24);
	writel(PEREN4_GT_ACLK_USB3OTG | PEREN4_GT_CLK_USB3OTG_REF, peri_crg + PERI_CRG_PEREN4);
	writel(PERRSTEN4_USB3OTG_MUX | PERRSTEN4_USB3OTG_AHBIF | PERRSTEN4_USB3OTG_32K, peri_crg + PERI_CRG_PERRSTDIS4);

	writel(PERRSTEN4_USB3OTG | PERRSTEN4_USB3OTGPHY_POR, peri_crg + PERI_CRG_PERRSTEN4);


  /* enable PHY REF CLK */
    uint32_t temp = readl(usb3otg_bc + USBOTG3_CTRL0);
 	writel(temp | USB3OTG_CTRL0_SC_USB3PHY_ABB_GT_EN, usb3otg_bc + USBOTG3_CTRL0);

    temp = readl(usb3otg_bc + USBOTG3_CTRL7);
 	writel(temp | USB3OTG_CTRL7_REF_SSP_EN, usb3otg_bc + USBOTG3_CTRL7);

  /* exit from IDDQ mode */
    temp = readl(usb3otg_bc + USBOTG3_CTRL2);
 	writel(temp & ~(USB3OTG_CTRL2_TEST_POWERDOWN_SSP | USB3OTG_CTRL2_TEST_POWERDOWN_HSP), usb3otg_bc + USBOTG3_CTRL2);

    mx_nanosleep(mx_deadline_after(MX_MSEC(10)));

	writel(PERRSTEN4_USB3OTGPHY_POR, peri_crg + PERI_CRG_PERRSTDIS4);
	writel(PERRSTEN4_USB3OTG, peri_crg + PERI_CRG_PERRSTDIS4);

    mx_nanosleep(mx_deadline_after(MX_MSEC(10)));

    temp = readl(usb3otg_bc + USBOTG3_CTRL3);
 	writel(temp | USB3OTG_CTRL3_VBUSVLDEXT | USB3OTG_CTRL3_VBUSVLDEXTSEL, usb3otg_bc + USBOTG3_CTRL7);

    mx_nanosleep(mx_deadline_after(MX_MSEC(10)));

	writel(0x1c466e3, usb3otg_bc + USBOTG3_CTRL4);
 
 #if 0
	/* usb refclk iso enable */
	writel(USB_REFCLK_ISO_EN, peri_crg + PERI_CRG_ISODIS);

	/* enable usb_tcxo_en */
	writel(USB_TCXO_EN | (USB_TCXO_EN << PERI_CTRL3_MSK_START),
			pctrl + PCTRL_PERI_CTRL3);

	/* select usbphy clk from abb */
	uint32_t temp = readl(pctrl + PCTRL_PERI_CTRL24);
	temp &= ~SC_CLK_USB3PHY_3MUX1_SEL;
	writel(temp, pctrl + PCTRL_PERI_CTRL24);

	/* open clk gate */
	writel(GT_CLK_USB3OTG_REF | GT_ACLK_USB3OTG,
			peri_crg + PERI_CRG_CLK_EN4);

#endif
    return MX_OK;
}

static void hi3360_dwc3_release(void* ctx) {
    hi3360_dwc3_t* dwc = ctx;
    pdev_mmio_buffer_release(&dwc->usb3otg);
    pdev_mmio_buffer_release(&dwc->usb3otg_bc);
    pdev_mmio_buffer_release(&dwc->peri_crg);
    pdev_mmio_buffer_release(&dwc->pctrl);
    pdev_mmio_buffer_release(&dwc->sctrl);
    pdev_mmio_buffer_release(&dwc->pmctrl);
    free(dwc);
}

static mx_status_t hi3360_dwc3_get_mmio(void* ctx, void** out_vaddr, size_t* out_length) {
    hi3360_dwc3_t* dwc = ctx;
    *out_vaddr = dwc->usb3otg.vaddr;
    *out_length = dwc->usb3otg.size;
    return MX_OK;
}

static uint32_t hi3360_dwc3_get_interrupt_count(void* ctx) {
    return 1;
}

static mx_status_t hi3360_dwc3_get_interrupt(void* ctx, uint32_t index, mx_handle_t* out_handle) {
    hi3360_dwc3_t* dwc = ctx;
    if (index != 0) {
        return MX_ERR_INVALID_ARGS;
    }
    return pdev_map_interrupt(&dwc->pdev, 0, out_handle);
}

static bool hi3360_dwc3_legacy_irq_mode(void* ctx) {
    return false;
}

usb_xhci_protocol_ops_t xhci_protocol = {
    .get_mmio = hi3360_dwc3_get_mmio,
    .get_interrupt_count = hi3360_dwc3_get_interrupt_count,
    .get_interrupt = hi3360_dwc3_get_interrupt,
    .legacy_irq_mode = hi3360_dwc3_legacy_irq_mode,
};

static mx_protocol_device_t hi3360_dwc3_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .release = hi3360_dwc3_release,
};

static mx_status_t hi3360_dwc3_bind(void* ctx, mx_device_t* dev, void** cookie) {
    printf("hi3360_dwc3_bind\n");

    hi3360_dwc3_t* dwc = calloc(1, sizeof(hi3360_dwc3_t));
    if (!dwc) {
        return MX_ERR_NO_MEMORY;
    }

    mx_status_t status = device_get_protocol(dev, MX_PROTOCOL_PLATFORM_DEV, &dwc->pdev);
    if (status != MX_OK) {
        goto fail;
    }

    platform_device_protocol_t* pdev = &dwc->pdev;
    if ((status = pdev_map_mmio_buffer(pdev, MMIO_USB3OTG, MX_CACHE_POLICY_UNCACHED_DEVICE,
                                       &dwc->usb3otg)) != MX_OK ||
         (status = pdev_map_mmio_buffer(pdev, MMIO_USB3OTG_BC, MX_CACHE_POLICY_UNCACHED_DEVICE,
                                       &dwc->usb3otg_bc)) != MX_OK ||
         (status = pdev_map_mmio_buffer(pdev, MMIO_PERI_CRG, MX_CACHE_POLICY_UNCACHED_DEVICE,
                                       &dwc->peri_crg)) != MX_OK ||
         (status = pdev_map_mmio_buffer(pdev, MMIO_PCTRL, MX_CACHE_POLICY_UNCACHED_DEVICE,
                                       &dwc->pctrl)) != MX_OK ||
         (status = pdev_map_mmio_buffer(pdev, MMIO_SCTRL, MX_CACHE_POLICY_UNCACHED_DEVICE,
                                       &dwc->sctrl)) != MX_OK ||
         (status = pdev_map_mmio_buffer(pdev, MMIO_PMCTRL, MX_CACHE_POLICY_UNCACHED_DEVICE,
                                       &dwc->pmctrl)) != MX_OK) {
        goto fail;
    }


printf("call hi3360_dwc3_init\n");
    if ((status = hi3360_dwc3_init(dwc)) != MX_OK) {
        goto fail;
    }
printf("did hi3360_dwc3_init\n");


/*
    printf("usb3otg_bc:\n");
    hexdump8(dwc->usb3otg_bc.vaddr, 256);
    printf("peri_crg:\n");
    hexdump8(dwc->peri_crg.vaddr, 256);
*/

    printf("usbotg:\n");
    hexdump8(dwc->usb3otg.vaddr, 256);
    printf("global registers:\n");
    hexdump(dwc->usb3otg.vaddr + GSBUSCFG0, 256);
    printf("device registers:\n");
    hexdump(dwc->usb3otg.vaddr + DCFG, 256);

//	writel(0x1c466e3, dwc->usb3otg_bc.vaddr + USBOTG3_CTRL4);

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "hi3600-dwc3",
        .ctx = dwc,
        .ops = &hi3360_dwc3_device_proto,
        .proto_id = MX_PROTOCOL_USB_XHCI,
        .proto_ops = &xhci_protocol,
    };

    status = device_add(dev, &args, &dwc->mxdev);
    if (status != MX_OK) {
        goto fail;
    }

    return MX_OK;

fail:
    printf("hi3360_dwc3_bind failed %d\n", status);
    hi3360_dwc3_release(dwc);
    return status;
}

static mx_driver_ops_t hi3360_dwc3_driver_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = hi3360_dwc3_bind,
};

// The formatter does not play nice with these macros.
// clang-format off
MAGENTA_DRIVER_BEGIN(hi3360_dwc3, hi3360_dwc3_driver_ops, "magenta", "0.1", 3)
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_VID, 0x12D1),
    BI_ABORT_IF(NE, BIND_PLATFORM_DEV_PID, 0x0960),
    BI_MATCH_IF(EQ, BIND_PLATFORM_DEV_DID, 1),
MAGENTA_DRIVER_END(hi3360_dwc3)
// clang-format on
