// Copyright 2017 The Fuchsia Authors. All riusghts reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/usb-client.h>
#include <ddk/protocol/usb.h>

#include <magenta/types.h>
#include <magenta/device/usb-client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include "usb-virtual-bus.h"

typedef struct {
    // the device we implement
    mx_device_t* mxdev;
    mx_handle_t channel_handle;
    usb_client_interface_t interface;
} usb_virtual_client_t;

static void handle_packet(usb_virtual_client_t* client, usb_virt_header_t* header) {
printf("handle_packet length %zu\n", header->data_length);
        char    response_buffer[USB_VIRT_BUFFER_SIZE];
        usb_virt_header_t* response = (usb_virt_header_t *)response_buffer;

    if (header->ep_addr == 0 && header->data_length >= sizeof(usb_setup_t)) {
        mx_status_t status;

        if (client->interface.ops) {
            usb_setup_t* setup = (usb_setup_t *)header->data;

    printf("handle_packet type: 0x%02X req: %d value: %d index: %d length: %d\n",
            setup->bmRequestType, setup->bRequest, le16toh(setup->wValue), le16toh(setup->wIndex), le16toh(setup->wLength));
            
            void* buffer;
            size_t length;
            if ((setup->bmRequestType & USB_DIR_MASK) == USB_DIR_IN) {
                buffer = response->data;
                length = sizeof(response_buffer) - sizeof(usb_virt_header_t);
            } else {
                buffer = setup + 1;
                length = header->data_length - sizeof(*setup);
            }
            status = usb_client_intf_control(&client->interface, setup, buffer, length);
            printf("control returned %d\n", status);
        } else {
            status = MX_ERR_UNAVAILABLE;
        }

        // send response
        printf("status %d write response\n", status);
        response->cmd = USB_VIRT_PACKET_RESP;
        response->cookie = header->cookie;
        response->status = (status > 0 ? MX_OK : status);
        response->data_length = (status < 0 ? 0 : status);
        size_t packet_length = sizeof(usb_virt_header_t);
        if (status > 0) packet_length += status;
        mx_channel_write(client->channel_handle, 0, response, packet_length, NULL, 0);
    } else {
        printf("non ep0 not supported yet\n");
    }
}

static mx_status_t usb_virtual_client_set_interface(void* ctx, usb_client_interface_t* interface) {
printf("usb_virtual_client_set_callbacks\n");
    usb_virtual_client_t* client = ctx;
    memcpy(&client->interface, interface, sizeof(client->interface));
    return MX_OK;
}

static mx_status_t usb_virtual_client_config_ep(void* ctx,
                                                const usb_endpoint_descriptor_t* ep_desc) {
    return MX_OK;
}

static void usb_virtual_client_set_connected(usb_virtual_client_t* client, bool connected) {
    usb_virt_header_t connect;
    connect.cmd = (connected ? USB_VIRT_CONNECT : USB_VIRT_DISCONNECT);
    mx_channel_write(client->channel_handle, 0, &connect, sizeof(connect), NULL, 0);
}

usb_client_protocol_ops_t virtual_client_protocol = {
    .set_interface = usb_virtual_client_set_interface,
    .config_ep = usb_virtual_client_config_ep,
};

static mx_status_t usb_virtual_client_open(void* ctx, mx_device_t** dev_out, uint32_t flags) {
printf("usb_virtual_client_open\n");
    return MX_OK;
}

static mx_status_t usb_virtual_client_ioctl(void* ctx, uint32_t op, const void* in_buf,
                                            size_t in_len, void* out_buf, size_t out_len,
                                            size_t* out_actual) {
    usb_virtual_client_t* client = ctx;

    switch (op) {
    case IOCTL_USB_CLIENT_SET_CONNNECTED: {
        if (!in_buf || in_len != sizeof(int)) {
            return MX_ERR_INVALID_ARGS;
        }
        int connected = *((int *)in_buf);
        printf("IOCTL_USB_CLIENT_SET_CONNNECTED %d\n", connected);
        usb_virtual_client_set_connected(client, !!connected);
        return MX_OK;
    }
    }
    return MX_ERR_NOT_SUPPORTED;
}

static void usb_virtual_client_iotxn_queue(void* ctx, iotxn_t* txn) {
}

static void usb_virtual_client_unbind(void* ctx) {
    printf("usb_virtual_client_unbind\n");
//    usb_virtual_client_t* client = dev_to_usb_virtual_client(dev);

}

static void usb_virtual_client_release(void* ctx) {
    // FIXME - do something here
}

static mx_protocol_device_t usb_virtual_client_device_proto = {
    .version = DEVICE_OPS_VERSION,
    .open = usb_virtual_client_open,
    .ioctl = usb_virtual_client_ioctl,
    .iotxn_queue = usb_virtual_client_iotxn_queue,
    .unbind = usb_virtual_client_unbind,
    .release = usb_virtual_client_release,
};


static int usb_virtual_client_thread(void* arg) {
    usb_virtual_client_t* client = (usb_virtual_client_t*)arg;

printf("usb_virtual_client_thread\n");
    while (1) {
        char    buffer[USB_VIRT_BUFFER_SIZE];
        uint32_t actual;
        
        mx_status_t status = mx_object_wait_one(client->channel_handle, MX_CHANNEL_READABLE, MX_TIME_INFINITE, NULL);
printf("mx_object_wait_one returned %d\n", status);
        status = mx_channel_read(client->channel_handle, 0, buffer, NULL, sizeof(buffer), 0,
                                             &actual, NULL);
        if (status != MX_OK) {
            printf("usb_virtual_client_thread mx_channel_read failed %d\n", status);
            return status;
        }

        usb_virt_header_t* header = (usb_virt_header_t *)buffer;
        switch (header->cmd) {
        case USB_VIRT_PACKET:
printf("client got packet\n");
            handle_packet(client, header);
            break;
        default:
            printf("usb_virtual_client_thread bad command %d\n", header->cmd);
            break;
        }
    }

    return 0;
}

mx_device_t* usb_virtual_client_add(mx_device_t* parent, mx_handle_t channel_handle) {
printf("usb_virtual_client_add\n");
    usb_virtual_client_t* client = calloc(1, sizeof(usb_virtual_client_t));
    if (!client) {
        mx_handle_close(channel_handle);
        return NULL;
    }

    client->channel_handle = channel_handle;

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "usb-virtual-client",
        .ctx = client,
        .ops = &usb_virtual_client_device_proto,
        .proto_id = MX_PROTOCOL_USB_CLIENT,
        .proto_ops = &virtual_client_protocol,
    };

    mx_status_t status = device_add(parent, &args, &client->mxdev);
    if (status != MX_OK) {
printf("usb_virtual_client_add device_add failed %d\n", status);
        mx_handle_close(channel_handle);
        free(client);
        return NULL;
    }

    thrd_t thread;
    thrd_create_with_name(&thread, usb_virtual_client_thread, client, "usb_virtual_client_thread");
    thrd_detach(thread);

    return client->mxdev;
}
