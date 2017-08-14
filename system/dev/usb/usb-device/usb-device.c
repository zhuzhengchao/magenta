// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>

#include <ddk/binding.h>
#include <ddk/device.h>
#include <ddk/driver.h>
#include <ddk/protocol/usb-dci.h>
#include <ddk/protocol/usb-function.h>
#include <magenta/listnode.h>
#include <magenta/device/usb-device.h>
#include <magenta/hw/usb.h>

typedef struct {
    mx_device_t* mxdev;
    mx_device_t* dci_dev;
    struct usb_device* dev;
    list_node_t node;
    usb_function_interface_t interface;
    usb_function_descriptor_t desc;
    usb_descriptor_header_t* descriptors;
    size_t descriptors_length;
    uint8_t interface_number;
} usb_function_t;

typedef struct usb_device {
    mx_device_t* mxdev;
    mx_device_t* dci_dev;
    usb_dci_protocol_t usb_dci;
    usb_device_descriptor_t device_desc;
    usb_configuration_descriptor_t* config_desc;
    usb_function_t* endpoint_map[USB_MAX_EPS];
    char* strings[256];
    list_node_t functions;
    mtx_t lock;
    bool functions_bound;
    uint8_t function_count;
} usb_device_t;

// for mapping bEndpointAddress value to/from index in range 0 - 31
// OUT endpoints are in range 1 - 15, IN endpoints are in range 17 - 31
#define ep_address_to_index(addr) (((addr) & 0xF) | (((addr) & 0x80) >> 3))
#define ep_index_to_address(index) (((index) & 0xF) | (((index) & 0x10) << 3))
#define OUT_EP_START    1
#define OUT_EP_END      15
#define IN_EP_START     17
#define IN_EP_END       31

static void usb_function_iotxn_queue(void* ctx, iotxn_t* txn) {
    usb_function_t* function = ctx;
    // pass down to the DCI driver
    iotxn_queue(function->dci_dev, txn);
}

static void usb_function_release(void* ctx) {
printf("usb_function_release\n");
    usb_function_t* function = ctx;
    free(function->descriptors);
    free(function);
}

static mx_protocol_device_t function_proto = {
    .version = DEVICE_OPS_VERSION,
    .iotxn_queue = usb_function_iotxn_queue,
    .release = usb_function_release,
};

static mx_status_t usb_device_function_registered(usb_device_t* dev) {
    mtx_lock(&dev->lock);

    if (dev->config_desc) {
        printf("usb_device_function_registered: already have configuration descriptor!\n");
        mtx_unlock(&dev->lock);
        return MX_ERR_BAD_STATE;
    }

    // check to see if we have all our functions registered
    // if so, we can build our configuration descriptor and tell the DCI driver we are ready
    usb_function_t* function;
    size_t length = sizeof(usb_configuration_descriptor_t);
    list_for_every_entry(&dev->functions, function, usb_function_t, node) {
        if (function->descriptors) {
            length += function->descriptors_length;
        } else {
            // need to wait for more functions to register
            mtx_unlock(&dev->lock);
            return MX_OK;
        }
    }

    // build our configuration descriptor
    usb_configuration_descriptor_t* config_desc = malloc(length);
    if (!config_desc) {
        mtx_unlock(&dev->lock);
        return MX_ERR_NO_MEMORY;
    }

    config_desc->bLength = sizeof(*config_desc);
    config_desc->bDescriptorType = USB_DT_CONFIG;
    config_desc->wTotalLength = htole16(length);
    config_desc->bNumInterfaces = 0;
    config_desc->bConfigurationValue = 1;
    config_desc->iConfiguration = 0;
    // TODO(voydanoff) add a way to configure bmAttributes and bMaxPower
    config_desc->bmAttributes = 0xE0;   // self powered
    config_desc->bMaxPower = 0;

    void* dest = config_desc + 1;
    list_for_every_entry(&dev->functions, function, usb_function_t, node) {
        memcpy(dest, function->descriptors, function->descriptors_length);
        dest += function->descriptors_length;
        config_desc->bNumInterfaces++;
    }
    dev->config_desc = config_desc;

    mtx_unlock(&dev->lock);

// TODO - clean up if this fails?
    return usb_dci_set_enabled(&dev->usb_dci, true);
}

static mx_status_t usb_func_register(void* ctx, usb_function_interface_t* interface) {
    usb_function_t* function = ctx;
    usb_function_t** endpoint_map = function->dev->endpoint_map;

    size_t length;
    const usb_descriptor_header_t* descriptors = usb_function_get_descriptors(interface, &length);

    // validate the descriptor list
    if (!descriptors || length < sizeof(usb_interface_descriptor_t)) {
        return MX_ERR_INVALID_ARGS;
    }

    usb_interface_descriptor_t* intf_desc = (usb_interface_descriptor_t *)descriptors;
    if (intf_desc->bDescriptorType != USB_DT_INTERFACE ||
            intf_desc->bLength != sizeof(usb_interface_descriptor_t)) {
        printf("usb_func_register: first descriptor not an interface descriptor\n");
        return MX_ERR_INVALID_ARGS;
    }

    const usb_descriptor_header_t* end = (void *)descriptors + length;
    const usb_descriptor_header_t* header = descriptors;

    while (header < end) {
        if (header->bDescriptorType == USB_DT_INTERFACE) {
            usb_interface_descriptor_t* desc = (usb_interface_descriptor_t *)header;
            if (desc->bInterfaceNumber != function->interface_number) {
                printf("usb_func_register: bInterfaceNumber %u, expecting %u\n",
                       desc->bInterfaceNumber, function->interface_number);
                return MX_ERR_INVALID_ARGS;
            }
        } else if (header->bDescriptorType == USB_DT_ENDPOINT) {
            usb_endpoint_descriptor_t* desc = (usb_endpoint_descriptor_t *)header;
            unsigned index = ep_address_to_index(desc->bEndpointAddress);
            if (index == 0 || endpoint_map[index] != function) {
                printf("usb_func_register: bad endpoint address 0x%X\n",
                       desc->bEndpointAddress);
                return MX_ERR_INVALID_ARGS;
            }
        }

        header = (void *)header + header->bLength;
    }

    function->descriptors = malloc(length);
    if (!function->descriptors) {
        return MX_ERR_NO_MEMORY;
    }
    memcpy(function->descriptors, descriptors, length);
    function->descriptors_length = length;
    memcpy(&function->interface, interface, sizeof(function->interface));

    return usb_device_function_registered(function->dev);
}

static uint8_t usb_func_get_interface_number(void* ctx) {
    usb_function_t* function = ctx;
    return function->interface_number;
}

static mx_status_t usb_func_alloc_endpoint(void* ctx, uint8_t direction, uint8_t* out_address) {
    unsigned start, end;

    if (direction == USB_DIR_OUT) {
        start = OUT_EP_START;
        end = OUT_EP_END;
    } else if (direction == USB_DIR_IN) {
        start = IN_EP_START;
        end = IN_EP_END;
    } else {
        return MX_ERR_INVALID_ARGS;
    }

    usb_function_t* function = ctx;
    usb_device_t* dev = function->dev;
    usb_function_t** endpoint_map = dev->endpoint_map;

    mtx_lock(&dev->lock);
    for (unsigned index = start; index <= end; index++) {
        if (endpoint_map[index] == NULL) {
            endpoint_map[index] = function;
            mtx_unlock(&dev->lock);
            *out_address = ep_index_to_address(index);
            return MX_OK;
        }
    }

    mtx_unlock(&dev->lock);
    return MX_ERR_NO_RESOURCES;
}

static void usb_func_queue(void* ctx, iotxn_t* txn, uint8_t ep_address) {
    usb_function_t* function = ctx;
    txn->protocol = MX_PROTOCOL_USB_FUNCTION;
    usb_function_protocol_data_t* data = iotxn_pdata(txn, usb_function_protocol_data_t);
    data->ep_address = ep_address;
    iotxn_queue(function->dci_dev, txn);
}

usb_function_protocol_ops_t usb_function_proto = {
    .register_func = usb_func_register,
    .get_interface_number = usb_func_get_interface_number,
    .alloc_endpoint = usb_func_alloc_endpoint,
    .queue = usb_func_queue,
};

static mx_status_t usb_dev_get_descriptor(usb_device_t* dev, uint8_t request_type,
                                          uint16_t value, uint16_t index, void* buffer,
                                          size_t length, size_t* out_actual) {
    uint8_t type = request_type & USB_TYPE_MASK;

    if (type == USB_TYPE_STANDARD) {
        uint8_t desc_type = value >> 8;
        if (desc_type == USB_DT_DEVICE && index == 0) {
            const usb_device_descriptor_t* desc = &dev->device_desc;
            if (desc->bLength == 0) {
                printf("usb_dev_get_descriptor: device descriptor not set\n");
                return MX_ERR_INTERNAL;
            }
            if (length > sizeof(*desc)) length = sizeof(*desc);
            memcpy(buffer, desc, length);
            *out_actual = length;
            return MX_OK;
        } else if (desc_type == USB_DT_CONFIG && index == 0) {
            const usb_configuration_descriptor_t* desc = dev->config_desc;
            if (!desc) {
                printf("usb_dev_get_descriptor: configuration descriptor not set\n");
                return MX_ERR_INTERNAL;
            }
            uint16_t desc_length = letoh16(desc->wTotalLength);
            if (length > desc_length) length =desc_length;
            memcpy(buffer, desc, length);
            *out_actual = length;
            return MX_OK;
        }
        else if (value >> 8 == USB_DT_STRING) {
            uint8_t desc[255];
            usb_descriptor_header_t* header = (usb_descriptor_header_t *)desc;
            header->bDescriptorType = USB_DT_STRING;

            uint8_t string_index = value & 0xFF;
            if (string_index == 0) {
                // special case - return language list
                header->bLength = 4;
                desc[2] = 0x09;     // language ID
                desc[3] = 0x04;
            } else {
                char* string = dev->strings[string_index];
                unsigned index = 2;

                // convert ASCII to Unicode
                if (string) {
                    while (*string && index < sizeof(desc) - 2) {
                        desc[index++] = *string++;
                        desc[index++] = 0;
                    }
                }
                // zero terminate
                desc[index++] = 0;
                desc[index++] = 0;
                header->bLength = index;
            }

            if (header->bLength < length) length = header->bLength;
            memcpy(buffer, desc, length);
            *out_actual = length;
            return MX_OK;
        }
    }

    printf("usb_device_get_descriptor unsupported value: %d index: %d\n", value, index);
    return MX_ERR_NOT_SUPPORTED;
}

static mx_status_t usb_dev_control(void* ctx, const usb_setup_t* setup, void* buffer,
                                   size_t buffer_length, size_t* out_actual) {
    usb_device_t* dev = ctx;
    uint8_t request_type = setup->bmRequestType;
    uint8_t request = setup->bRequest;
    uint16_t value = le16toh(setup->wValue);
    uint16_t index = le16toh(setup->wIndex);
    uint16_t length = le16toh(setup->wLength);
    if (length > buffer_length) length = buffer_length;

    printf("usb_dev_control type: 0x%02X req: %d value: %d index: %d length: %d\n",
            request_type, request, value, index, length);

    switch (request_type & USB_RECIP_MASK) {
    case USB_RECIP_DEVICE:
        // handle standard device requests
        if ((request_type & USB_DIR_MASK) == USB_DIR_IN && request == USB_REQ_GET_DESCRIPTOR) {
            return usb_dev_get_descriptor(dev, request_type, value, index, buffer, length,
                                             out_actual);
        } else if (request_type == (USB_DIR_OUT | USB_TYPE_STANDARD | USB_RECIP_DEVICE) &&
                   request == USB_REQ_SET_CONFIGURATION && length == 0) {
            // nothing to do
            return MX_OK;
        }
        break;
    case USB_RECIP_INTERFACE: {
        // delegate to the function driver for the interface
        usb_function_t* function;
        int interface_num = 0;
        list_for_every_entry(&dev->functions, function, usb_function_t, node) {
            if (interface_num++ == index && function->interface.ops) {
                return usb_function_control(&function->interface, setup, buffer, buffer_length,
                                            out_actual);
            }
        }
        break;
    }
    case USB_RECIP_ENDPOINT: {
        // delegate to the function driver for the endpoint
        index = ep_address_to_index(index);
        if (index == 0 || index >= USB_MAX_EPS) {
            return MX_ERR_INVALID_ARGS;
        }
        usb_function_t* function = dev->endpoint_map[index];
        if (function) {
            return usb_function_control(&function->interface, setup, buffer, buffer_length,
                                        out_actual);
        }
        break;
    }
    case USB_RECIP_OTHER:
        // TODO(voydanoff) - how to handle this?
    default:
        break;
    }

    return MX_ERR_NOT_SUPPORTED;
}

usb_dci_interface_ops_t dci_ops = {
    .control = usb_dev_control,
};

static mx_status_t usb_dev_set_device_desc(usb_device_t* dev, const void* in_buf, size_t in_len) {
    if (in_len != sizeof(dev->device_desc)) {
        return MX_ERR_INVALID_ARGS;
    }
    const usb_device_descriptor_t* desc = in_buf;
    if (desc->bLength != sizeof(*desc) ||
        desc->bDescriptorType != USB_DT_DEVICE) {
        return MX_ERR_INVALID_ARGS;
    }
    if (desc->bNumConfigurations != 1) {
        printf("usb_device_ioctl: bNumConfigurations: %u, only 1 supported\n",
               desc->bNumConfigurations);
        return MX_ERR_INVALID_ARGS;
    }
    memcpy(&dev->device_desc, desc, sizeof(dev->device_desc));
    return MX_OK;
}

static mx_status_t usb_dev_set_string_desc(usb_device_t* dev, const void* in_buf, size_t in_len) {
    if (in_len < sizeof(usb_device_string_t) + 1) {
        return MX_ERR_INVALID_ARGS;
    }
    const usb_device_string_t* string = in_buf;
    // make sure string is zero terminated
    *((char *)in_buf + in_len - 1) = 0;
    free(dev->strings[string->index]);
    dev->strings[string->index] = strdup(string->string);
    return dev->strings[string->index] ? MX_OK : MX_ERR_NO_MEMORY;
}

static mx_status_t usb_dev_add_function(usb_device_t* dev, const void* in_buf, size_t in_len) {
    if (dev->function_count == UINT8_MAX) {
        printf("usb_dev_add_function: no more functions available\n");
        return MX_ERR_NO_RESOURCES;
    }
    if (in_len != sizeof(usb_function_descriptor_t)) {
        return MX_ERR_INVALID_ARGS;
    }
    if (dev->functions_bound) {
        return MX_ERR_BAD_STATE;
    }

    usb_function_t* function = calloc(1, sizeof(usb_function_t));
    if (!function) {
        return MX_ERR_NO_MEMORY;
    }
    function->dci_dev = dev->dci_dev;
    function->dev = dev;
    memcpy(&function->desc, in_buf, sizeof(function->desc));
    function->interface_number = dev->function_count++;
    list_add_tail(&dev->functions, &function->node);

    return MX_OK;
}

static mx_status_t usb_dev_bind_functions(usb_device_t* dev) {
    if (dev->functions_bound) {
        printf("usb_dev_bind_functions: already bound!\n");
        return MX_ERR_BAD_STATE;
    }

    usb_device_descriptor_t* device_desc = &dev->device_desc;
    if (device_desc->bLength == 0) {
        printf("usb_dev_bind_functions: device descriptor not set\n");
        return MX_ERR_BAD_STATE;
    }
    if (list_is_empty(&dev->functions)) {
        printf("usb_dev_bind_functions: no functions to bind\n");
        return MX_ERR_BAD_STATE;
    }

    int index = 0;
    usb_function_t* function;
    list_for_every_entry(&dev->functions, function, usb_function_t, node) {
        char name[16];
        snprintf(name, sizeof(name), "function-%03d", index);

        usb_function_descriptor_t* desc = &function->desc;

        mx_device_prop_t props[] = {
            { BIND_PROTOCOL, 0, MX_PROTOCOL_USB_FUNCTION },
            { BIND_USB_CLASS, 0, desc->interface_class },
            { BIND_USB_SUBCLASS, 0, desc->interface_subclass },
            { BIND_USB_PROTOCOL, 0, desc->interface_protocol },
            { BIND_USB_VID, 0, device_desc->idVendor },
            { BIND_USB_PID, 0, device_desc->idProduct },
        };

        device_add_args_t args = {
            .version = DEVICE_ADD_ARGS_VERSION,
            .name = name,
            .ctx = function,
            .ops = &function_proto,
            .proto_id = MX_PROTOCOL_USB_FUNCTION,
            .proto_ops = &usb_function_proto,
            .props = props,
            .prop_count = countof(props),
        };

        mx_status_t status = device_add(dev->mxdev, &args, &function->mxdev);
        if (status != MX_OK) {
            printf("usb_dev_bind_functions add_device failed %d\n", status);
            return status;
        }

        index++;
    }

    dev->functions_bound = true;

    return MX_OK;
}

static mx_status_t usb_dev_clear_functions(usb_device_t* dev) {
    usb_function_t* function;
    while ((function = list_remove_head_type(&dev->functions, usb_function_t, node)) != NULL) {
        device_remove(function->mxdev);
    }
    free(dev->config_desc);
    dev->config_desc = NULL;
    dev->functions_bound = false;
    dev->function_count = 0;
    memset(dev->endpoint_map, 0, sizeof(dev->endpoint_map));
    return MX_OK;
}

static mx_status_t usb_dev_ioctl(void* ctx, uint32_t op, const void* in_buf, size_t in_len,
                                 void* out_buf, size_t out_len, size_t* out_actual) {
    usb_device_t* dev = ctx;

    switch (op) {
    case IOCTL_USB_DEVICE_SET_DEVICE_DESC:
        return usb_dev_set_device_desc(dev, in_buf, in_len);
    case IOCTL_USB_DEVICE_SET_STRING_DESC:
        return usb_dev_set_string_desc(dev, in_buf, in_len);
    case IOCTL_USB_DEVICE_ADD_FUNCTION:
        return usb_dev_add_function(dev, in_buf, in_len);
    case IOCTL_USB_DEVICE_BIND_FUNCTIONS:
        return usb_dev_bind_functions(dev);
    case IOCTL_USB_DEVICE_CLEAR_FUNCTIONS:
        return usb_dev_clear_functions(dev);
    default:
        return MX_ERR_NOT_SUPPORTED;
    }
}

static void usb_dev_unbind(void* ctx) {
printf("usb_dev_unbind\n");
    usb_device_t* dev = ctx;
    usb_dev_clear_functions(dev);
    device_remove(dev->mxdev);
}

static void usb_dev_release(void* ctx) {
printf("usb_dev_release\n");
    usb_device_t* dev = ctx;
    free(dev->config_desc);
    for (unsigned i = 0; i < countof(dev->strings); i++) {
        free(dev->strings[i]);
    }
    free(dev);
}

static mx_protocol_device_t device_proto = {
    .version = DEVICE_OPS_VERSION,
    .ioctl = usb_dev_ioctl,
    .unbind = usb_dev_unbind,
    .release = usb_dev_release,
};

mx_status_t usb_dev_bind(void* ctx, mx_device_t* parent, void** cookie) {
    printf("usb_dev_bind\n");

    usb_device_t* dev = calloc(1, sizeof(usb_device_t));
    if (!dev) {
        return MX_ERR_NO_MEMORY;
    }
    list_initialize(&dev->functions);
    mtx_init(&dev->lock, mtx_plain);
    dev->dci_dev = parent;

    if (device_get_protocol(parent, MX_PROTOCOL_USB_DCI, &dev->usb_dci)) {
        free(dev);
        return MX_ERR_NOT_SUPPORTED;
    }

    device_add_args_t args = {
        .version = DEVICE_ADD_ARGS_VERSION,
        .name = "usb-device",
        .ctx = dev,
        .ops = &device_proto,
        .proto_id = MX_PROTOCOL_USB_DEVICE,
        .flags = DEVICE_ADD_NON_BINDABLE,
    };

    mx_status_t status = device_add(parent, &args, &dev->mxdev);
    if (status != MX_OK) {
        printf("usb_device_bind add_device failed %d\n", status);
        free(dev);
        return status;
    }

    usb_dci_interface_t intf = {
        .ops = &dci_ops,
        .ctx = dev,
    };
    usb_dci_set_interface(&dev->usb_dci, &intf);

    return MX_OK;
}

static mx_driver_ops_t usb_device_ops = {
    .version = DRIVER_OPS_VERSION,
    .bind = usb_dev_bind,
};

// clang-format off
MAGENTA_DRIVER_BEGIN(usb_device, usb_device_ops, "magenta", "0.1", 1)
    BI_MATCH_IF(EQ, BIND_PROTOCOL, MX_PROTOCOL_USB_DCI),
MAGENTA_DRIVER_END(usb_device)
