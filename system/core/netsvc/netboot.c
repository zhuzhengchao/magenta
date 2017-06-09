// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "netsvc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <inet6/inet6.h>
#include <inet6/netifc.h>

#include <launchpad/launchpad.h>
#include <magenta/process.h>
#include <magenta/processargs.h>
#include <magenta/syscalls.h>

#include <magenta/boot/netboot.h>

static uint32_t last_cookie = 0;
static uint32_t last_cmd = 0;
static uint32_t last_arg = 0;
static uint32_t last_ack_cmd = 0;
static uint32_t last_ack_arg = 0;

#define PAGE_ROUNDUP(x) ((x + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))
#define MAX_ADVERTISE_DATA_LEN 256

static bool xfer_active = false;
static bool resend_state = false;
static bool resend_done_state = false;

typedef struct nbfilecontainer {
    nbfile file;
    mx_handle_t data;   // handle to vmo that backs netbootfile.
} nbfilecontainer_t;

static nbfilecontainer_t nbkernel;
static nbfilecontainer_t nbbootdata;

// Pointer to the currently active transfer.
static nbfile* active;
static size_t pktlen = 0;

typedef struct nbbitset {
    uint64_t* bits;
    size_t count;
    size_t size;
} nbbitset_t;

static uint32_t resend_cookie = 0;
static uint32_t resend_reqs = 0;
static ip6_addr_t resend_addr;
static uint16_t resend_port;

static nbbitset_t recv_map;

mx_status_t nbbitset_init(nbbitset_t* bs, const size_t size) {
    assert(bs);

    if (bs->bits) {
        // Bitset has already been initialized.
        return ERR_BAD_STATE;
    }

    const size_t elem_size = sizeof(*(bs->bits)) * 8;
    printf("nbbitset_init elem size = %lu\n", elem_size);

    const size_t buflen = (size / elem_size) + 1;

    bs->bits = calloc(buflen, elem_size);
    if (!bs->bits) {
        return ERR_NO_MEMORY;
    }

    const size_t padding = size % elem_size;

    bs->bits[buflen - 1] = ((1 << padding) - 1) << (64 - padding);
    bs->size = size;
    bs->count = buflen;

    printf("initialized bitset count = %lu, size = %lu\n", bs->count, bs->size);

    return NO_ERROR;
}

void nbbitset_deinit(nbbitset_t* bs) {
    if (!bs) return;

    if (!(bs->bits)) return;

    free(bs->bits);
    bs->bits = NULL;
    bs->count = 0;
    bs->size = 0;
}

bool nbbitset_is_initialized(nbbitset_t* bs) {
    return bs->bits;
}

void nbbitset_get_idx(const nbbitset_t* bs, const size_t elem,
                      size_t* idx, size_t* bitidx) {
    assert(elem < bs->size);
    assert(idx && bitidx && bs);
    assert(bs->bits);

    const size_t elem_size = sizeof(*(bs->bits)) * 8;
    *idx = (elem / elem_size);
    *bitidx = elem % elem_size;

    assert(*idx < bs->count);
}

bool nbbitset_isset(nbbitset_t* bs, const size_t elem) {
    size_t idx, bitidx;
    nbbitset_get_idx(bs, elem, &idx, &bitidx);

    return (bs->bits[idx]) & (1 << bitidx);
}

void nbbitset_set(nbbitset_t* bs, const size_t elem, const bool val) {
    // printf("bitset set bit %lu\n", elem);

    size_t idx, bitidx;
    nbbitset_get_idx(bs, elem, &idx, &bitidx);

    // printf("Bitset set bit %lu (%lu of elem %lu)\n", elem, bitidx, idx);

    if (val) {
        bs->bits[idx] |= (1 << bitidx);
    } else {
        bs->bits[idx] &= ~(1 << bitidx);
    }
}

mx_status_t nbfilecontainer_init(size_t size, nbfilecontainer_t* target) {
    mx_status_t st = NO_ERROR;

    assert(target);

    // De-init the container if it's already initialized.
    if (target->file.data) {
        // For now, I can't see a valid reason that a client would send the same
        // filename twice.
        // We'll handle this case gracefully, but we'll print a warning to the
        // console just in case it was a mistake.
        printf("netbootloader: warning, reusing a previously initialized container\n");

        // Unmap the vmo from the address space.
        st = mx_vmar_unmap(mx_vmar_root_self(), (uintptr_t)target->file.data, target->file.size);
        if (st != NO_ERROR) {
            printf("netbootloader: failed to unmap existing vmo, st = %d\n", st);
            return st;
        }

        mx_handle_close(target->data);

        target->file.offset = 0;
        target->file.size = 0;
        target->file.data = 0;
    }

    size = PAGE_ROUNDUP(size);
    st = mx_vmo_create(size, 0, &target->data);
    if (st != NO_ERROR) {
        printf("netbootloader: Could not create a netboot vmo of size = %lu "
               "retcode = %d\n", size, st);
        return st;
    }

    uintptr_t buffer;
    st = mx_vmar_map(mx_vmar_root_self(), 0, target->data, 0, size,
                     MX_VM_FLAG_PERM_READ | MX_VM_FLAG_PERM_WRITE, &buffer);
    if (st != NO_ERROR) {
        printf("netbootloader: failed to map data vmo for buffer, st = %d\n", st);
        mx_handle_close(target->data);
        return st;
    }

    target->file.offset = 0;
    target->file.size = size;
    target->file.data = (uint8_t*)buffer;

    return NO_ERROR;
}

nbfile* netboot_get_buffer(const char* name, size_t size) {
    mx_status_t st = NO_ERROR;
    nbfilecontainer_t* result;

    if (!strncmp(name, "kernel.bin", 11)) {
        result = &nbkernel;
    } else if (!strncmp(name, "ramdisk.bin", 12)) {
        result = &nbbootdata;
    } else {
        return NULL;
    }

    st = nbfilecontainer_init(size, result);
    if (st != NO_ERROR) {
        printf("netbootloader: failed to initialize file container for "
               "file = '%s', retcode = %d\n", name, st);
        return NULL;
    }

    return &result->file;
}

#define REPEAT_MAX_MSG_SIZE 1024

bool request_resend(uint32_t cookie, const ip6_addr_t* saddr, uint16_t sport) {
    uint8_t msg[sizeof(nbmsg) + REPEAT_MAX_MSG_SIZE];
    nbmsg* hdr = (nbmsg*)msg;
    uint32_t* payload_ptr = (uint32_t*)(&msg[0] + sizeof(nbmsg));
    const size_t max_request_count = REPEAT_MAX_MSG_SIZE / sizeof(uint32_t);
    size_t actual_request_count = 0;

    printf("Resending packets = [");

    // TODO(gkalsi): fixme
    resend_reqs = 0;

    for (uint32_t i = 0; i < recv_map.count; i++) {
        // All the packets in this offset have been received.
        if (recv_map.bits[i] == ~((uint64_t)0)) {
            continue;
        }

        // The last element of the bitset might be partial so handle that case.

        for (uint32_t j = 0; j < 64; j++) {
            if ((recv_map.bits[i]) & (1 << j)) {
                continue;
            }

            payload_ptr[actual_request_count++] = (i * 64) + j;
            printf(" %d ", (i * 64) + j);
            if (actual_request_count == max_request_count) {
                goto loopexit;
            }

            resend_reqs++;
        }
    }
loopexit:
    
    printf("]\n");

    if (!actual_request_count) {
        // All packets recieved correctly?
        return true;
    }

    hdr->magic = NB_MAGIC;
    hdr->cookie = cookie;
    hdr->cmd = NB_RESEND;
    hdr->arg = actual_request_count;
    udp6_send(&msg, sizeof(nbmsg) + actual_request_count * sizeof(uint32_t),
              saddr, sport, NB_SERVER_PORT);

    return false;

}

void netboot_advertise(const char* nodename) {
    if (resend_state) {
        if(request_resend(0, &resend_addr, resend_port)) {
            printf("Resend all packets fulfilled!\n ");
            resend_state = false;
            resend_done_state = true;
        } else {
            nbmsg ack;
            ack.magic = NB_MAGIC;
            ack.cookie = resend_cookie;
            ack.cmd = NB_RESEND_DONE;
            ack.arg = resend_reqs;
            udp6_send(&ack, sizeof(ack), &resend_addr, resend_port, NB_SERVER_PORT);
        }
    }

    // Don't advertise if a transfer is active.
    if (xfer_active) return;

    uint8_t buffer[sizeof(nbmsg) + MAX_ADVERTISE_DATA_LEN];
    nbmsg* msg = (void*)buffer;
    msg->magic = NB_MAGIC;
    msg->cookie = 0;
    msg->cmd = NB_ADVERTISE;
    msg->arg = NB_VERSION_CURRENT;

    snprintf((char*)msg->data, MAX_ADVERTISE_DATA_LEN, "version=%s;nodename=%s",
             BOOTLOADER_VERSION, nodename);
    const size_t data_len = strlen((char*)msg->data) + 1;
    udp6_send(buffer, sizeof(nbmsg) + data_len, &ip6_ll_all_nodes,
              NB_ADVERT_PORT, NB_SERVER_PORT);
}

void netboot_recv(void* data, size_t len,
                  const ip6_addr_t* daddr, uint16_t dport,
                  const ip6_addr_t* saddr, uint16_t sport) {
    nbmsg* msg = data;
    nbmsg ack;

    bool do_transmit = true;
    bool do_boot = false;

    if (dport != NB_SERVER_PORT)
        return;

    if (len < sizeof(nbmsg))
        return;
    len -= sizeof(nbmsg);

    // if ((last_cookie == msg->cookie) &&
    //     (last_cmd == msg->cmd) && (last_arg == msg->arg)) {
    //     // host must have missed the ack. resend
    //     ack.magic = NB_MAGIC;
    //     ack.cookie = last_cookie;
    //     ack.cmd = NB_RESEND;
    //     ack.arg = last_ack_arg;

    //     printf("Missed ack?\n");

    //     goto transmit;
    // }

    ack.cmd = NB_ACK;
    ack.arg = 0;

    switch (msg->cmd) {
    case NB_COMMAND:
        if (len == 0)
            return;
        msg->data[len - 1] = 0;
        break;
    case NB_SEND_FILE:
        xfer_active = true;
        if (len == 0)
            return;
        msg->data[len - 1] = 0;
        for (size_t i = 0; i < (len - 1); i++) {
            if ((msg->data[i] < ' ') || (msg->data[i] > 127)) {
                msg->data[i] = '.';
            }
        }
        active = netboot_get_buffer((const char*)msg->data, msg->arg);
        if (active) {
            nbbitset_deinit(&recv_map);
            active->offset = 0;
            ack.arg = msg->arg;
            printf("netboot: Receive File '%s' length = %u...\n", (char*) msg->data, msg->arg);
        } else {
            printf("netboot: Rejected File '%s'...\n", (char*) msg->data);
            ack.cmd = NB_ERROR_BAD_FILE;
        }
        break;

    case NB_DATA:
    case NB_LAST_DATA:
        xfer_active = true;
        if (active == 0) {
            printf("netboot: > received chunk before NB_FILE\n");
            return;
        }
        if (!nbbitset_is_initialized(&recv_map)) {
            // TODO(gkalsi): Use div-round-up here?
            printf("packet size = %lu\n", len);
            size_t chunk_count = (active->size / len) +
                                 ((active->size % len) ? 1 : 0);
            pktlen = len;
            resend_done_state = false;
            resend_state = false;
            nbbitset_init(&recv_map, chunk_count);
        }
        if ((msg->arg + len) > active->size) {
            // Bootloader sent us a packet that was outside the range.
            ack.cmd = NB_ERROR_TOO_LARGE;
            ack.arg = msg->arg;
        } else {
            // printf("active len = %lu, pkt offset = %u, pkt len = %lu\n", active->size, msg->arg, len);

            memcpy(active->data + msg->arg, msg->data, len);

            printf("Got packet number %lu\n", msg->arg / pktlen);

            if (msg->cmd == NB_DATA) {
                nbbitset_set(&recv_map, msg->arg / pktlen, true);
            }

            ack.cmd = msg->cmd == NB_LAST_DATA ? NB_FILE_RECEIVED : NB_ACK;
            if (msg->cmd != NB_LAST_DATA) {
                do_transmit = false;
            } else {
                resend_cookie = msg->cookie;
                resend_port = sport;
                memcpy(&resend_addr, saddr, sizeof(ip6_addr_t));
                do_transmit = resend_done_state;
                resend_state = true;
                // xfer_active = false;
            }
        }
        break;
    case NB_BOOT:
        do_boot = true;
        printf("netboot: Boot Kernel...\n");
        break;
    default:
        // We don't have a handler for this command, let netsvc handle it.
        do_transmit = false;
    }

    last_cookie = msg->cookie;
    last_cmd = msg->cmd;
    last_arg = msg->arg;
    last_ack_cmd = ack.cmd;
    last_ack_arg = ack.arg;

    ack.cookie = msg->cookie;
    ack.magic = NB_MAGIC;
transmit:
    if (do_transmit) {
        // printf("Sending ACK offset = %lx\n", active->offset);
        udp6_send(&ack, sizeof(ack), saddr, sport, NB_SERVER_PORT);
    }

    if (do_boot) {
        mx_system_mexec(nbkernel.data, nbbootdata.data);
    }
}