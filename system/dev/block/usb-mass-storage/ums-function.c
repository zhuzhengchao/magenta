// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/iotxn.h>
#include <ddk/protocol/usb-function.h>
#include <magenta/process.h>
#include <magenta/syscalls.h>
#include <magenta/device/usb-device.h>

#include "ums-hw.h"

#define BLOCK_SIZE      512     
#define STORAGE_SIZE    (10 * 1024 * 1024)     
#define BLOCK_COUNT     (STORAGE_SIZE / BLOCK_SIZE)
#define DATA_TXN_SIZE   16384

typedef enum {
    DATA_STATE_NONE,
    DATA_STATE_READ,
    DATA_STATE_WRITE,
} ums_data_state_t;

 static struct {
    usb_interface_descriptor_t intf;
    usb_endpoint_descriptor_t out_ep;
    usb_endpoint_descriptor_t in_ep;
} descriptors = {
    .intf = {
        .bLength = sizeof(usb_interface_descriptor_t),
        .bDescriptorType = USB_DT_INTERFACE,
//      .bInterfaceNumber set later
        .bAlternateSetting = 0,
        .bNumEndpoints = 2,
        .bInterfaceClass = USB_CLASS_MSC,
        .bInterfaceSubClass = USB_SUBCLASS_MSC_SCSI,
        .bInterfaceProtocol = USB_PROTOCOL_MSC_BULK_ONLY,
        .iInterface = 0,
    },
    .out_ep = {
        .bLength = sizeof(usb_endpoint_descriptor_t),
        .bDescriptorType = USB_DT_ENDPOINT,
//      .bEndpointAddress set later
        .bmAttributes = USB_ENDPOINT_BULK,
        .wMaxPacketSize = htole16(512),
        .bInterval = 0,
    },
    .in_ep = {
        .bLength = sizeof(usb_endpoint_descriptor_t),
        .bDescriptorType = USB_DT_ENDPOINT,
//      .bEndpointAddress set later
        .bmAttributes = USB_ENDPOINT_BULK,
        .wMaxPacketSize = htole16(512),
        .bInterval = 0,
    },
};

typedef struct {
    mx_device_t* mxdev;
    usb_function_protocol_t function;
    iotxn_t* cbw_iotxn;
    iotxn_t* data_iotxn;
    iotxn_t* csw_iotxn;

    // vmo for backing storage
    mx_handle_t storage_handle;
    void* storage;

    // command we are currently handling
    ums_cbw_t current_cbw;

    // state for data transfers
    ums_data_state_t    data_state;
    // state for reads and writes
    mx_off_t data_offset;
    size_t data_remaining;

    uint8_t bulk_out_addr;
    uint8_t bulk_in_addr;
} usb_ums_t;

static void ums_queue_cbw(usb_ums_t* ums) {
    iotxn_t* txn = ums->csw_iotxn;
    ums_csw_t* csw;
    iotxn_mmap(txn, (void **)&csw);

    csw->dCSWSignature = htole32(CSW_SIGNATURE);
    csw->dCSWTag = ums->current_cbw.dCBWTag;
    csw->dCSWDataResidue = 0;   // TODO(voydanoff) use correct value here
    csw->bmCSWStatus = CSW_SUCCESS;

    txn->length = sizeof(ums_csw_t);
    usb_function_queue(&ums->function, ums->csw_iotxn, ums->bulk_in_addr);
}

static void ums_continue_transfer(usb_ums_t* ums) {
    iotxn_t* txn = ums->data_iotxn;

    size_t length = ums->data_remaining;
    if (length > DATA_TXN_SIZE) {
        length = DATA_TXN_SIZE;
    }
    txn->length = length;

    if (ums->data_state == DATA_STATE_READ) {
        iotxn_copyto(txn, ums->storage + ums->data_offset, length, 0);
        usb_function_queue(&ums->function, txn, ums->bulk_in_addr);
    } else if (ums->data_state == DATA_STATE_WRITE) {
        usb_function_queue(&ums->function, txn, ums->bulk_out_addr);
    } else {
        printf("ums_continue_transfer: bad data state %d\n", ums->data_state);
    }
}

static void ums_start_transfer(usb_ums_t* ums, ums_data_state_t state, uint64_t lba,
                               uint32_t blocks) {
    mx_off_t offset = lba * BLOCK_SIZE;
    size_t length = blocks * BLOCK_SIZE;

    if (offset + length > STORAGE_SIZE) {
        printf("ums_start_transfer: transfer out of range: state: %d, lba: %zu blocks: %u\n",
               state, lba, blocks);
        // TODO(voydanoff) report error to host
        return;
    }

    ums->data_state = state;
    ums->data_offset = offset;
    ums->data_remaining = length;

    ums_continue_transfer(ums);
}

static void ums_handle_inquiry(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_inquiry\n");

    iotxn_t* txn = ums->data_iotxn;
    uint8_t* buffer;
    iotxn_mmap(txn, (void **)&buffer);
    memset(buffer, 0, UMS_INQUIRY_TRANSFER_LENGTH);
    // TODO(voydanoff) fill this in
    txn->length = UMS_INQUIRY_TRANSFER_LENGTH;
    usb_function_queue(&ums->function, txn, ums->bulk_in_addr);
    ums_queue_cbw(ums);
}

static void ums_handle_test_unit_ready(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_test_unit_ready\n");

    // no data phase here. Just return status OK
    ums_queue_cbw(ums);
}

static void ums_handle_request_sense(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_request_sense\n");
}

static void ums_handle_read_capacity10(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_read_capacity10\n");

    iotxn_t* txn = ums->data_iotxn;
    scsi_read_capacity_10_t* data;
    iotxn_mmap(txn, (void **)&data);

    uint64_t lba = BLOCK_COUNT - 1;
    if (lba > UINT32_MAX) {
        data->lba = htobe32(UINT32_MAX);
    } else {
        data->lba = htobe32(lba);
    }
    data->block_length = htobe32(BLOCK_SIZE);

    txn->length = sizeof(*data);
    usb_function_queue(&ums->function, txn, ums->bulk_in_addr);
    ums_queue_cbw(ums);
}

static void ums_handle_read_capacity16(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_read_capacity16\n");

    iotxn_t* txn = ums->data_iotxn;
    scsi_read_capacity_16_t* data;
    iotxn_mmap(txn, (void **)&data);
    memset(data, 0, sizeof(*data));

    data->lba = htobe64(BLOCK_COUNT - 1);
    data->block_length = htobe32(BLOCK_SIZE);

    txn->length = sizeof(*data);
    usb_function_queue(&ums->function, txn, ums->bulk_in_addr);
    ums_queue_cbw(ums);
}

static void ums_handle_mode_sense6(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_mode_sense6\n");

    iotxn_t* txn = ums->data_iotxn;
    scsi_mode_sense_6_data_t* data;
    iotxn_mmap(txn, (void **)&data);
    memset(data, 0, sizeof(*data));

    // TODO(voydanoff) fill in data here

    txn->length = sizeof(*data);
    usb_function_queue(&ums->function, txn, ums->bulk_in_addr);
    ums_queue_cbw(ums);
}

static void ums_handle_read10(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_read10\n");

    scsi_command10_t* command = (scsi_command10_t *)cbw->CBWCB;
    uint32_t lba = be32toh(command->lba);
    uint32_t blocks = ((uint32_t)command->length_hi << 8) | (uint32_t)command->length_lo;
    ums_start_transfer(ums, DATA_STATE_READ, lba, blocks);
}

static void ums_handle_read12(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_read12\n");

    scsi_command12_t* command = (scsi_command12_t *)cbw->CBWCB;
    uint64_t lba = be64toh(command->lba);
    uint32_t blocks = be32toh(command->length);
    ums_start_transfer(ums, DATA_STATE_READ, lba, blocks);
}

static void ums_handle_read16(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_read16\n");

    scsi_command16_t* command = (scsi_command16_t *)cbw->CBWCB;
    uint32_t lba = be32toh(command->lba);
    uint32_t blocks = be32toh(command->length);
    ums_start_transfer(ums, DATA_STATE_READ, lba, blocks);
}

static void ums_handle_write10(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_write10\n");

    scsi_command10_t* command = (scsi_command10_t *)cbw->CBWCB;
    uint32_t lba = be32toh(command->lba);
    uint32_t blocks = ((uint32_t)command->length_hi << 8) | (uint32_t)command->length_lo;
    ums_start_transfer(ums, DATA_STATE_WRITE, lba, blocks);
}

static void ums_handle_write12(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_write12\n");

    scsi_command12_t* command = (scsi_command12_t *)cbw->CBWCB;
    uint64_t lba = be64toh(command->lba);
    uint32_t blocks = be32toh(command->length);
    ums_start_transfer(ums, DATA_STATE_WRITE, lba, blocks);
}

static void ums_handle_write16(usb_ums_t* ums, ums_cbw_t* cbw) {
    printf("ums_handle_write16\n");

    scsi_command12_t* command = (scsi_command12_t *)cbw->CBWCB;
    uint64_t lba = be64toh(command->lba);
    uint32_t blocks = be32toh(command->length);
    ums_start_transfer(ums, DATA_STATE_WRITE, lba, blocks);
}

static void ums_handle_cbw(usb_ums_t* ums, ums_cbw_t* cbw) {
    if (le32toh(cbw->dCBWSignature) != CBW_SIGNATURE) {
        printf("ums_handle_cbw: bad dCBWSignature 0x%x\n", le32toh(cbw->dCBWSignature));
        return;
    }

    // all SCSI commands have opcode in the same place, so using scsi_command6_t works here.
    scsi_command6_t* command = (scsi_command6_t *)cbw->CBWCB;
    switch (command->opcode) {
    case UMS_INQUIRY:
        ums_handle_inquiry(ums, cbw);
        break;
    case UMS_TEST_UNIT_READY:
        ums_handle_test_unit_ready(ums, cbw);
        break;
    case UMS_REQUEST_SENSE:
        ums_handle_request_sense(ums, cbw);
        break;
    case UMS_READ_CAPACITY10:
        ums_handle_read_capacity10(ums, cbw);
        break;
    case UMS_READ_CAPACITY16:
        ums_handle_read_capacity16(ums, cbw);
        break;
    case UMS_MODE_SENSE6:
        ums_handle_mode_sense6(ums, cbw);
        break;
    case UMS_READ10:
        ums_handle_read10(ums, cbw);
        break;
    case UMS_READ12:
        ums_handle_read12(ums, cbw);
        break;
    case UMS_READ16:
        ums_handle_read16(ums, cbw);
        break;
    case UMS_WRITE10:
        ums_handle_write10(ums, cbw);
        break;
    case UMS_WRITE12:
        ums_handle_write12(ums, cbw);
        break;
    case UMS_WRITE16:
        ums_handle_write16(ums, cbw);
        break;
    default:
        printf("ums_handle_cbw: unsupported opcode %d\n", command->opcode);
        break;
    }
}

static void ums_cbw_complete(iotxn_t* txn, void* cookie) {
    usb_ums_t* ums = cookie;

    printf("ums_cbw_complete %d %ld\n", txn->status, txn->actual);

    if (txn->status == MX_OK && txn->actual == sizeof(ums_cbw_t)) {
        ums_cbw_t* cbw = &ums->current_cbw;
        iotxn_copyfrom(txn, cbw, sizeof(*cbw), 0);
        ums_handle_cbw(ums, cbw);
    }
}

static void ums_data_complete(iotxn_t* txn, void* cookie) {
    usb_ums_t* ums = cookie;

    printf("ums_data_complete %d %ld\n", txn->status, txn->actual);

    if (ums->data_state == DATA_STATE_WRITE) {
        iotxn_copyfrom(txn, ums->storage + ums->data_offset, txn->actual, 0);
    } else if (ums->data_state != DATA_STATE_READ) {
        return;
    }

    ums->data_offset += txn->actual;
    if (ums->data_remaining > txn->actual) {
        ums->data_remaining -= txn->actual;
    } else {
        ums->data_remaining = 0;
    }

    if (ums->data_remaining > 0) {
        ums_continue_transfer(ums);
    } else {
        ums->data_state = DATA_STATE_NONE;
        ums_queue_cbw(ums);
    } 
}

static void ums_csw_complete(iotxn_t* txn, void* cookie) {
    usb_ums_t* ums = cookie;
    printf("ums_csw_complete %d %ld\n", txn->status, txn->actual);

    usb_function_queue(&ums->function, ums->cbw_iotxn, ums->bulk_out_addr);
}

static const usb_descriptor_header_t* ums_get_descriptors(void* ctx, size_t* out_length) {
    *out_length = sizeof(descriptors);
    return (const usb_descriptor_header_t *)&descriptors;
}

static mx_status_t ums_control(void* ctx, const usb_setup_t* setup, void* buffer,
                                         size_t length, size_t* out_actual) {
    if (setup->bmRequestType == (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE) &&
        setup->bRequest == USB_REQ_GET_MAX_LUN && setup->wValue == 0 && setup->wIndex == 0 &&
        setup->wLength >= sizeof(uint8_t)) {
        *((uint8_t *)buffer) = 0;
        *out_actual = sizeof(uint8_t);
        return MX_OK;
    }

    return MX_ERR_NOT_SUPPORTED;
}

usb_function_interface_ops_t device_ops = {
    .get_descriptors = ums_get_descriptors,
    .control = ums_control,
};

static void usb_ums_unbind(void* ctx) {
printf("usb_ums_unbind\n");
    usb_ums_t* ums = ctx;
    device_remove(ums->mxdev);
}

static void usb_ums_release(void* ctx) {
printf("usb_ums_release\n");
    usb_ums_t* ums = ctx;

    if (ums->storage) {
        mx_vmar_unmap(mx_vmar_root_self(), (uintptr_t)ums->storage, STORAGE_SIZE);
    }
    mx_handle_close(ums->storage_handle);

    if (ums->cbw_iotxn) {
        iotxn_release(ums->cbw_iotxn);
    }
    if (ums->data_iotxn) {
        iotxn_release(ums->data_iotxn);
    }
    if (ums->cbw_iotxn) {
        iotxn_release(ums->csw_iotxn);
    }
    free(ums);
}

static mx_protocol_device_t usb_ums_proto = {
    .version = DEVICE_OPS_VERSION,
    .unbind = usb_ums_unbind,
    .release = usb_ums_release,
};

mx_status_t usb_ums_bind(void* ctx, mx_device_t* parent, void** cookie) {
    printf("usb_ums_bind\n");

    usb_ums_t* ums = calloc(1, sizeof(usb_ums_t));
    if (!ums) {
        return MX_ERR_NO_MEMORY;
    }
    ums->data_state = DATA_STATE_NONE;

    mx_status_t status =device_get_protocol(parent, MX_PROTOCOL_USB_FUNCTION, &ums->function);
    if (status != MX_OK) {
        goto fail;
    }

    status =  iotxn_alloc(&ums->cbw_iotxn, 0, sizeof(ums_cbw_t));
    if (status != MX_OK) {
        goto fail;
    }
    status =  iotxn_alloc(&ums->data_iotxn, 0, DATA_TXN_SIZE);
    if (status != MX_OK) {
        goto fail;
    }
    status =  iotxn_alloc(&ums->csw_iotxn, 0, sizeof(ums_csw_t));
    if (status != MX_OK) {
        goto fail;
    }

    // create and map a VMO
    status = mx_vmo_create(STORAGE_SIZE, 0, &ums->storage_handle);
    if (status != MX_OK) {
        goto fail;
    }
    status = mx_vmar_map(mx_vmar_root_self(), 0, ums->storage_handle, 0, STORAGE_SIZE,
                         MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE, (mx_vaddr_t *)&ums->storage);
    if (status != MX_OK) {
        goto fail;
    }

    ums->cbw_iotxn->length = sizeof(ums_cbw_t);
    ums->csw_iotxn->length = sizeof(ums_csw_t);
    ums->cbw_iotxn->complete_cb = ums_cbw_complete;
    ums->data_iotxn->complete_cb = ums_data_complete;
    ums->csw_iotxn->complete_cb = ums_csw_complete;
    ums->cbw_iotxn->cookie = ums;
    ums->data_iotxn->cookie = ums;
    ums->csw_iotxn->cookie = ums;

    descriptors.intf.bInterfaceNumber = usb_function_get_interface_number(&ums->function);

    status = usb_function_alloc_endpoint(&ums->function, USB_DIR_OUT, &ums->bulk_out_addr);
    if (status != MX_OK) {
        printf("usb_ums_bind: usb_function_alloc_endpoint failed\n");
        goto fail;
    }
    status = usb_function_alloc_endpoint(&ums->function, USB_DIR_IN, &ums->bulk_in_addr);
    if (status != MX_OK) {
        printf("usb_ums_bind: usb_function_alloc_endpoint failed\n");
        goto fail;
    }

   descriptors.out_ep.bEndpointAddress = ums->bulk_out_addr;
   descriptors.in_ep.bEndpointAddress = ums->bulk_in_addr;

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "usb-ums-function",
        .ctx = ums,
        .ops = &usb_ums_proto,
    };

    status = device_add(parent, &args, &ums->mxdev);
    if (status != MX_OK) {
        printf("usb_device_bind add_device failed %d\n", status);
        goto fail;
    }

    usb_function_queue(&ums->function, ums->cbw_iotxn, ums->bulk_out_addr);

    usb_function_interface_t intf = {
        .ops = &device_ops,
        .ctx = ums,
    };
    usb_function_register(&ums->function, &intf);

    return MX_OK;

fail:
    usb_ums_release(ums);
    return status;
}

static mx_driver_ops_t usb_ums_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = usb_ums_bind,
};

// clang-format off
MAGENTA_DRIVER_BEGIN(usb_ums, usb_ums_ops, "magenta", "0.1", 4)
    BI_ABORT_IF(NE, BIND_PROTOCOL, MX_PROTOCOL_USB_FUNCTION),
    BI_MATCH_IF(EQ, BIND_USB_CLASS, USB_CLASS_MSC),
    BI_MATCH_IF(EQ, BIND_USB_SUBCLASS, USB_SUBCLASS_MSC_SCSI),
    BI_MATCH_IF(EQ, BIND_USB_PROTOCOL, USB_PROTOCOL_MSC_BULK_ONLY),
MAGENTA_DRIVER_END(usb_ums)
