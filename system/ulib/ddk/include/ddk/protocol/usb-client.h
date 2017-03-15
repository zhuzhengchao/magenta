// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <magenta/compiler.h>
#include <magenta/types.h>
#include <magenta/hw/usb.h>

__BEGIN_CDECLS;

// callbacks installed by function driver
typedef struct {
    // callback for handling ep0 control requests
    mx_status_t (*control)(void* ctx, const usb_setup_t* setup, void* buffer, int length);
} usb_client_interface_ops_t;

typedef struct {
    usb_client_interface_ops_t* ops;
    void* ctx;
} usb_client_interface_t;

static inline mx_status_t usb_client_intf_control(usb_client_interface_t* intf,
                                                  const usb_setup_t* setup,
                                                  void* buffer, int length) {
    return intf->ops->control(intf->ctx, setup, buffer, length);
}

typedef struct {
    mx_status_t (*set_interface)(void* ctx, usb_client_interface_t* interface);
    mx_status_t (*config_ep)(void* ctx, const usb_endpoint_descriptor_t* ep_desc);
} usb_client_protocol_ops_t;

typedef struct {
    usb_client_protocol_ops_t* ops;
    void* ctx;
} usb_client_protocol_t;

static inline void usb_client_set_interface(usb_client_protocol_t* client,
                                            usb_client_interface_t* intf) {
    client->ops->set_interface(client->ctx, intf);
}

static inline void usb_client_config_ep(usb_client_protocol_t* client,
                                        const usb_endpoint_descriptor_t* ep_desc) {
    client->ops->config_ep(client->ctx, ep_desc);
}

__END_CDECLS;
