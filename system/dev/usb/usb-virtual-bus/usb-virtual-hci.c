// Copyright 2017 The Fuchsia Authors. All riusghts reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/usb-bus.h>
#include <ddk/protocol/usb-hci.h>
#include <ddk/protocol/usb.h>
#include <sync/completion.h>

#include <magenta/listnode.h>
#include <magenta/types.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "usb-virtual-bus.h"
#include "util.h"

#define CLIENT_SLOT_ID  0
#define CLIENT_HUB_ID   0
#define CLIENT_SPEED    USB_SPEED_HIGH

typedef struct {
    mx_device_t* mxdev;
    usb_bus_interface_t bus;
    mx_handle_t channel_handle;

    mtx_t lock;
    completion_t completion;
    bool connected;
    bool was_connected;

    list_node_t ep_txns[USB_MAX_EPS];
} usb_virtual_hci_t;

static void usb_virtual_hci_set_bus_interface(void* ctx, usb_bus_interface_t* bus) {
    usb_virtual_hci_t* hci = ctx;

    if (bus) {
        memcpy(&hci->bus, bus, sizeof(hci->bus));

        mtx_lock(&hci->lock);
        bool connected = hci->connected;
        hci->was_connected = connected;
        mtx_unlock(&hci->lock);
        
        if (connected) {
            usb_bus_add_device(&hci->bus, CLIENT_SLOT_ID, CLIENT_HUB_ID, CLIENT_SPEED);
        }
    } else {
        memset(&hci->bus, 0, sizeof(hci->bus));
    }
}

static size_t usb_virtual_hci_get_max_device_count(void* ctx) {
    return 1;
}

static mx_status_t usb_virtual_hci_enable_ep(void* ctx, uint32_t device_id,
                                  usb_endpoint_descriptor_t* ep_desc,
                                  usb_ss_ep_comp_descriptor_t* ss_comp_desc, bool enable) {
    return MX_OK;
}

static uint64_t usb_virtual_hci_get_frame(void* ctx) {
    return 0;
}

mx_status_t usb_virtual_hci_config_hub(void* ctx, uint32_t device_id, usb_speed_t speed,
                            usb_hub_descriptor_t* descriptor) {
    return MX_OK;
}

mx_status_t usb_virtual_hci_hub_device_added(void* ctx, uint32_t hub_address, int port,
                                  usb_speed_t speed) {
    return MX_OK;
}

mx_status_t usb_virtual_hci_hub_device_removed(void* ctx, uint32_t hub_address, int port) {
    return MX_OK;
}

mx_status_t usb_virtual_hci_reset_endpoint(void* ctx, uint32_t device_id, uint8_t ep_address) {
    return MX_ERR_NOT_SUPPORTED;
}

size_t usb_virtual_hci_get_max_transfer_size(void* ctx, uint32_t device_id, uint8_t ep_address) {
    return 65536;
}

static usb_hci_protocol_ops_t virtual_hci_protocol = {
    .set_bus_interface = usb_virtual_hci_set_bus_interface,
    .get_max_device_count = usb_virtual_hci_get_max_device_count,
    .enable_endpoint = usb_virtual_hci_enable_ep,
    .get_current_frame = usb_virtual_hci_get_frame,
    .configure_hub = usb_virtual_hci_config_hub,
    .hub_device_added = usb_virtual_hci_hub_device_added,
    .hub_device_removed = usb_virtual_hci_hub_device_removed,
    .reset_endpoint = usb_virtual_hci_reset_endpoint,
    .get_max_transfer_size = usb_virtual_hci_get_max_transfer_size,
};

static void usb_virtual_hci_iotxn_queue(void* ctx, iotxn_t* txn) {
    printf("usb_virtual_hci_iotxn_queue\n");

    usb_virtual_hci_t* hci = ctx;
    usb_protocol_data_t* data = iotxn_pdata(txn, usb_protocol_data_t);
    if (data->device_id != CLIENT_SLOT_ID) {
        iotxn_complete(txn, MX_ERR_INVALID_ARGS, 0);
        return;
    }
    uint8_t ep_index = ep_addr_to_index(data->ep_address);
    if (ep_index >= USB_MAX_EPS) {
        iotxn_complete(txn, MX_ERR_INVALID_ARGS, 0);
        return;
    }
    if (txn->length > USB_VIRT_MAX_PACKET) {
        iotxn_complete(txn, MX_ERR_OUT_OF_RANGE, 0);
        return;
    }

    // queue the transaction for the specified endpoint
    list_add_tail(&hci->ep_txns[ep_index], &txn->node);

    bool out = ((data->ep_address & USB_ENDPOINT_DIR_MASK) == USB_ENDPOINT_OUT);
    if (out) {
        char    buffer[USB_VIRT_BUFFER_SIZE];
        usb_virt_header_t* header = (usb_virt_header_t *)buffer;
        header->cmd = USB_VIRT_PACKET;
        header->cookie = (uintptr_t)txn;
        header->ep_addr = data->ep_address;

        if (data->ep_address == 0) {
            usb_setup_t* setup = (usb_setup_t *)header->data;
            memcpy(setup, &data->setup, sizeof(usb_setup_t));
            
printf("sending type: 0x%02X req: %d value: %d index: %d length: %d\n",
            setup->bmRequestType, setup->bRequest, le16toh(setup->wValue), le16toh(setup->wIndex), le16toh(setup->wLength));
            
            header->data_length = sizeof(usb_setup_t);
            if (txn->length > 0 && (setup->bmRequestType & USB_DIR_MASK) == USB_DIR_OUT) {
                header->data_length += txn->length;    
                iotxn_copyfrom(txn, &header->data[sizeof(usb_setup_t)], txn->length, 0);
            }
        } else {
            // FIXME only for out direction
            header->data_length = txn->length;
            iotxn_copyfrom(txn, header->data, txn->length, 0);
        }
        mx_channel_write(hci->channel_handle, 0, buffer, sizeof(usb_virt_header_t) + header->data_length,
                         NULL, 0);
    }
}

static void usb_virtual_hci_unbind(void* ctx) {
    printf("usb_virtual_hci_unbind\n");
    usb_virtual_hci_t* hci = ctx;

    device_remove(hci->mxdev);
}

static void usb_virtual_hci_release(void* ctx) {
 printf("usb_virtual_hci_release\n");
   usb_virtual_hci_t* hci = ctx;

    mx_handle_close(hci->channel_handle);
    free(hci);
}

static mx_protocol_device_t usb_virtual_hci_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .iotxn_queue = usb_virtual_hci_iotxn_queue,
    .unbind = usb_virtual_hci_unbind,
    .release = usb_virtual_hci_release,
};

static int connection_thread(void* arg) {
    usb_virtual_hci_t* hci = (usb_virtual_hci_t*)arg;

    while (1) {
        completion_wait(&hci->completion, MX_TIME_INFINITE);

        mtx_lock(&hci->lock);
        bool connect = hci->connected && !hci->was_connected;
        bool disconnect = !hci->connected && hci->was_connected;
        hci->was_connected = hci->connected;
        mtx_unlock(&hci->lock);

        if (hci->bus.ops) {
            if (connect) {
                usb_bus_add_device(&hci->bus, CLIENT_SLOT_ID, CLIENT_HUB_ID, CLIENT_SPEED);
            } else if (disconnect) {
                usb_bus_remove_device(&hci->bus, CLIENT_SLOT_ID);
            }
        }    
    }
    return 0;
}

static int channel_thread(void* arg) {
    usb_virtual_hci_t* hci = (usb_virtual_hci_t*)arg;

printf("channel_thread\n");
    while (1) {
        char    buffer[USB_VIRT_BUFFER_SIZE];
        uint32_t actual;
        
        mx_status_t status = mx_object_wait_one(hci->channel_handle, MX_CHANNEL_READABLE,
                                                MX_TIME_INFINITE, NULL);
        status = mx_channel_read(hci->channel_handle, 0, buffer, NULL, sizeof(buffer), 0,
                                             &actual, NULL);
        if (status != MX_OK) {
            printf("channel_thread mx_channel_read failed %d\n", status);
            return status;
        }

        usb_virt_header_t* header = (usb_virt_header_t *)buffer;
        switch (header->cmd) {
        case USB_VIRT_CONNECT:
        case USB_VIRT_DISCONNECT:
            mtx_lock(&hci->lock);
            hci->connected = (header->cmd == USB_VIRT_CONNECT);
            mtx_unlock(&hci->lock);
            completion_signal(&hci->completion);
            break;
        case USB_VIRT_PACKET:
printf("HCI: USB_VIRT_PACKET\n");
            break;
        case USB_VIRT_PACKET_RESP: {
printf("HCI: USB_VIRT_PACKET_RESP status %d length %zu\n", header->status, header->data_length);
            iotxn_t* txn = (iotxn_t *)header->cookie;
            if (header->data_length > 0) {
printf("copyto header %p data %p length %zu\n", header, header->data, header->data_length);
                iotxn_copyto(txn, header->data, header->data_length, 0);
            }
            iotxn_complete(txn, header->status, header->data_length);
            break;
        }
        default:
            printf("channel_thread bad command %d\n", header->cmd);
            break;
        }
    }

    return 0;
}

mx_device_t* usb_virtual_hci_add(mx_device_t* parent, mx_handle_t channel_handle) {
printf("usb_virtual_hci_add\n");
    usb_virtual_hci_t* hci = calloc(1, sizeof(usb_virtual_hci_t));
    if (!hci) {
        mx_handle_close(channel_handle);
        return NULL;
    }

    for (uint i = 0; i < countof(hci->ep_txns); i++) {
        list_initialize(&hci->ep_txns[i]);
    }
    mtx_init(&hci->lock, mtx_plain);
    completion_reset(&hci->completion);
    hci->channel_handle = channel_handle;

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "usb-virtual-hci",
        .ctx = hci,
        .ops = &usb_virtual_hci_device_proto,
        .proto_id = MX_PROTOCOL_USB_HCI,
        .proto_ops = &virtual_hci_protocol,
    };

    mx_status_t status = device_add(parent, &args, &hci->mxdev);
    if (status != MX_OK) {
printf("usb_virtual_hci_add device_add failed %d\n", status);
        mx_handle_close(channel_handle);
        free(hci);
        return NULL;
    }

    thrd_t thread;
    thrd_create_with_name(&thread, channel_thread, hci, "channel_thread");
    thrd_detach(thread);
    thrd_create_with_name(&thread, connection_thread, hci, "connection_thread");
    thrd_detach(thread);

    return hci->mxdev;
}
