// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <string.h>

#include <device_id.h>
#include <inet6.h>
#include <netifc.h>

#include <magenta/boot/netboot.h>

#include "eventloop.h"
#include "receivemap.h"

#include <xefi.h>

// Each of these values is in 100ns units, to match the UEFI timer resolution.
#define ACK_RATE             1000000 // 0.1s - Frequency of ACKs
#define FAST_ADVERTISE_RATE  1000000 // 0.1s
#define SLOW_ADVERTISE_RATE 10000000 // 1s
#define RESEND_RATE             1000 // 0.0001s - Frequency of RESEND requests

// The packet size for data transmissions is expected to remain constant
static int packet_size = -1;

// item being downloaded
static nbfile* item;
static int total_size;

static recv_map_t recv_map;

static char advertise_nodename[64] = "";
static char advertise_data[256] = "nodename=magenta";

// Event handlers
static void advertise(void* cookie);
static void request_resend(void* cookie);
static void send_ack(void* cookie);

// Event loop filter
static bool require_network(void* cookie);

typedef struct {
    const ip6_addr* saddr;
    uint16_t sport;
    const ip6_addr* daddr;
    uint16_t dport;
} addr_info_t;

typedef void (*udp6_handler)(nbmsg* msg, size_t len, addr_info_t addr_info);

typedef struct {
    bool enabled;
    uint32_t cmd;
    udp6_handler callback;
} udp6_handler_t;

#define MAX_UDP_HANDLERS 256

static udp6_handler_t udp_handlers[MAX_UDP_HANDLERS];
static size_t num_udp_handlers = 0;

// UDP6 handlers
void receive_file(nbmsg* msg, size_t len, addr_info_t addr_info);
void receive_first_data(nbmsg* msg, size_t len, addr_info_t addr_info);
void receive_data(nbmsg* msg, size_t len, addr_info_t addr_info);
void receive_last_data(nbmsg* msg, size_t len, addr_info_t addr_info);

typedef struct udp_msg {
    nbmsg msg;
    uint16_t sport;
    ip6_addr daddr;
    uint16_t dport;
} udp_msg_t;

static void advertise(void* cookie) {
    uint8_t buffer[sizeof(nbmsg) + sizeof(advertise_data)];
    nbmsg* msg = (void*)buffer;
    msg->magic = NB_MAGIC;
    msg->cookie = 0;
    msg->cmd = NB_ADVERTISE;
    msg->arg = NB_VERSION_CURRENT;
    size_t data_len = strlen(advertise_data) + 1;
    memcpy(msg->data, advertise_data, data_len);
    udp6_send(buffer, sizeof(nbmsg) + data_len, &ip6_ll_all_nodes,
              NB_ADVERT_PORT, NB_SERVER_PORT);
}

#define MAX_FAST_ADVERTISEMENTS 100

static void advertise_fast(void *cookie) {
    static unsigned int advertisements_sent = 0;
    if (advertisements_sent++ > MAX_FAST_ADVERTISEMENTS) {
        eloop_rm_event(advertise_fast);
        eloop_add_event(SLOW_ADVERTISE_RATE, advertise, NULL);
        advertisements_sent = 0;
    } else {
        advertise(cookie);
    }
}

void udp6_add_handler(uint32_t nb_msg, udp6_handler callback) {
    if (num_udp_handlers >= MAX_UDP_HANDLERS) {
        printf("netboot: Unable to add more than %d UDP packet handlers\n",
               MAX_UDP_HANDLERS);
    }
    udp_handlers[num_udp_handlers].cmd = nb_msg;
    udp_handlers[num_udp_handlers].callback = callback;
    udp_handlers[num_udp_handlers].enabled = true;
    num_udp_handlers++;
}

void udp6_rm_handler(udp6_handler callback) {
    size_t ndx;
    for (ndx = 0; ndx < num_udp_handlers; ndx++) {
        if (udp_handlers[ndx].callback == callback)
            udp_handlers[ndx].enabled = false;
    }
}

void udp6_rm_all_handlers(void) {
    num_udp_handlers = 0;
}

static void udp6_reap_handlers(void) {
    size_t ndx = 0;
    while (ndx < num_udp_handlers) {
        if (!udp_handlers[ndx].enabled) {
            if (ndx < (num_udp_handlers - 1)) {
                memmove(&udp_handlers[ndx], &udp_handlers[ndx + 1],
                        ((num_udp_handlers - ndx) + 1) *
                         sizeof(udp6_handler_t));
            }
            num_udp_handlers--;
            continue;
        }
        ndx++;
    }
}

void udp6_recv(void* data, size_t len,
               const ip6_addr* daddr, uint16_t dport,
               const ip6_addr* saddr, uint16_t sport) {
    nbmsg* msg = data;

    if (dport != NB_SERVER_PORT)
        return;

    if (len < sizeof(nbmsg))
        return;
    len -= sizeof(nbmsg);

    size_t ndx;
    addr_info_t addr_info = {.daddr = daddr, .dport = dport,
                             .saddr = saddr, .sport = sport};

    for (ndx = 0; ndx < num_udp_handlers; ndx++) {
        if (udp_handlers[ndx].enabled && msg->cmd == udp_handlers[ndx].cmd) {
            udp_handlers[ndx].callback(msg, len, addr_info);
            break; // Only allow one handler per message
        }
    }
    udp6_reap_handlers();
}

int netboot_init(const char* nodename) {
    if (netifc_open()) {
        printf("netboot: Failed to open network interface\n");
        return -1;
    }
    char buf[DEVICE_ID_MAX];
    if (!nodename || (nodename[0] == 0)) {
        device_id(eth_addr(), buf);
        nodename = buf;
    }
    if (nodename) {
        strncpy(advertise_nodename, nodename, sizeof(advertise_nodename) - 1);
        snprintf(advertise_data, sizeof(advertise_data),
                 "version=%s;nodename=%s", BOOTLOADER_VERSION, nodename);
    }
    return 0;
}

const char* netboot_nodename() {
    return advertise_nodename;
}

static bool require_network(void* cookie) {
    static int nb_online = 0;
    if (netifc_active()) {
        if (nb_online == 0) {
            printf("netboot: interface online\n");
            nb_online = 1;
        }
        return false;
    }
    if (nb_online == 1) {
        printf("netboot: interface offline\n");
        nb_online = 0;
    }
    return true;
}

static void send_ack(void* cookie) {
    udp_msg_t* ack = (udp_msg_t*)cookie;
    udp6_send(&ack->msg, sizeof(ack->msg), &ack->daddr, ack->dport,
              ack->sport);
}

typedef struct resend_state {
    uint32_t cookie;  // From the original NB_LAST_DATA msg from server
    uint32_t next;    // Index into received map table
    uint32_t requests_sent;
    ip6_addr dst_addr;
    uint16_t dst_port;
} resend_state_t;

// All done with our resend requests, write out a NB_RESEND_DONE message
static void finish_resend_request(resend_state_t* state) {
    static udp_msg_t ack = { .msg = { .magic = NB_MAGIC,
                                      .cmd = NB_RESEND_DONE } };
    ack.msg.cookie = state->cookie;
    ack.msg.arg = state->requests_sent;
    memcpy(&ack.daddr, &state->dst_addr, sizeof(ip6_addr));
    ack.dport = state->dst_port;
    ack.sport = NB_SERVER_PORT;
    eloop_rm_event(request_resend);
    eloop_add_event(ACK_RATE, send_ack, &ack);
    udp6_add_handler(NB_DATA, receive_first_data);
    udp6_add_handler(NB_LAST_DATA, receive_last_data);
}

// When sending a NB_RESEND request, this is the maximum size of the payload
// (our missed offset values).
#define REPEAT_MAX_MSG_SIZE 1024

static void request_resend(void* cookie) {
    resend_state_t* state = (resend_state_t*)cookie;
    if (state->next == -1) {
        finish_resend_request(state);
        return;
    }
    uint8_t msg[sizeof(nbmsg) + REPEAT_MAX_MSG_SIZE];
    nbmsg* hdr = (nbmsg*)msg;
    uint32_t* payload_ptr = (uint32_t*) (&msg[0] + sizeof(nbmsg));
    size_t max_request_count = REPEAT_MAX_MSG_SIZE / sizeof(uint32_t);
    size_t actual_request_count;
    for (actual_request_count = 0; actual_request_count < max_request_count &&
                                   state->next != -1;
         actual_request_count++) {
        *payload_ptr++ = state->next;
        state->next = recv_map_next(recv_map, state->next); 
        state->requests_sent++;
    }
    hdr->magic = NB_MAGIC;
    hdr->cookie = state->cookie;
    hdr->cmd = NB_RESEND;
    hdr->arg = actual_request_count;
    int send_result;
    send_result = udp6_send(&msg, sizeof(nbmsg) + actual_request_count * sizeof(uint32_t),
              &state->dst_addr, state->dst_port, NB_SERVER_PORT);
    if (send_result != 0) {
        printf("netboot: Failed to send packet\n");
    }
}

static void start_boot(nbmsg* msg, size_t len, addr_info_t addr_info) {
    printf("netboot: Boot Kernel...\n");
    eloop_end(0);
}

static void handle_command(nbmsg* msg, size_t len, addr_info_t addr_info) {
    nbmsg ack = { .magic = NB_MAGIC,
                  .cmd = NB_ACK,
                  .cookie = msg->cookie,
                  .arg = 0 };
    udp6_send(&ack, sizeof(ack), addr_info.saddr, addr_info.sport,
              NB_SERVER_PORT);
}

static void send_query_ack(nbmsg* msg, size_t len, addr_info_t addr_info) {
    uint8_t buffer[256];
    nbmsg* ack = (void*)buffer;
    ack->magic = NB_MAGIC;
    ack->cookie = msg->cookie;
    ack->cmd = NB_ACK;
    ack->arg = NB_VERSION_CURRENT;
    memcpy(ack->data, advertise_nodename, sizeof(advertise_nodename));
    udp6_send(buffer, sizeof(nbmsg) + strlen(advertise_nodename) + 1,
              addr_info.saddr, addr_info.sport, NB_SERVER_PORT);
}

void receive_first_data(nbmsg* msg, size_t data_size, addr_info_t addr_info) {
    eloop_rm_event(send_ack);
    udp6_rm_handler(receive_first_data);
    udp6_add_handler(NB_DATA, receive_data);
    receive_data(msg, data_size, addr_info);
}

void receive_data(nbmsg* msg, size_t data_size, addr_info_t addr_info) {
    // Next % at which to provide an update
    static int report_threshold = 0;
    size_t offset = msg->arg;
    if (packet_size == -1) {
        packet_size = data_size;
        // On receipt of the first packet, NULL-terminate the receive map.
        recv_map_set_size(recv_map, packet_size);
        report_threshold = 0;
    } else {
        if ((data_size != packet_size) && (offset + data_size != total_size)) {
            printf("Inconsistent packet size - expected %d, saw %d\n",
                   (int) packet_size, (int) data_size);
            // We're pretty much hosed at this point
            while (1)
                ;
        }
    }
    if (offset + data_size > total_size) {
        nbmsg error_msg;
        error_msg.magic = NB_MAGIC;
        error_msg.cookie = msg->cookie;
        error_msg.cmd = NB_ERROR_TOO_LARGE;
        error_msg.arg = msg->arg;
        udp6_send(&error_msg, sizeof(error_msg), addr_info.saddr,
                  addr_info.sport, NB_SERVER_PORT);
    } else if (recv_map_mark_received(recv_map, offset)) {
        memcpy(item->data + offset, msg->data, data_size);
        item->offset = offset + data_size;
    }
    if (recv_map_progress(recv_map) >= report_threshold) {
        printf("%d%%%s", report_threshold,
               report_threshold == 100 ? "\n" : "...");
        report_threshold += 5;
    }
}

void receive_last_data(nbmsg* msg, size_t len, addr_info_t addr_info) {
    udp6_rm_handler(receive_data);
    udp6_rm_handler(receive_last_data);
    if (packet_size == -1) {
        // We should report back that no packets were received, but since
        // we don't know the packet size yet, we have no way to communicate
        // this. In the very unlikely case that we end up here, at least
        // provide a diagnosable error message.
        printf("netboot: No packets received, slow down initial transfer rate\n");
        while (1)
            ;
    } else if (recv_map_isempty(recv_map)) {
        static udp_msg_t ack = { .msg = { .magic = NB_MAGIC,
                                          .cmd = NB_FILE_RECEIVED } };
        ack.msg.cookie = msg->cookie;
        ack.msg.arg = total_size;
        ack.sport = NB_SERVER_PORT;
        memcpy(&ack.daddr, addr_info.saddr, sizeof(ip6_addr));
        ack.dport = addr_info.sport;
        eloop_add_event(ACK_RATE, send_ack, &ack);
        udp6_add_handler(NB_SEND_FILE, receive_file);
    } else {
        static resend_state_t resend_state;
        resend_state.cookie = msg->cookie;
        resend_state.requests_sent = 0;
        resend_state.next = recv_map_first(recv_map);
        memcpy(&resend_state.dst_addr, addr_info.saddr, sizeof(ip6_addr));
        resend_state.dst_port = addr_info.sport;
        eloop_add_event(RESEND_RATE, request_resend, &resend_state);
    }
}

void receive_file(nbmsg* msg, size_t len, addr_info_t addr_info) {
    if (len == 0)
        return;

    static udp_msg_t ack = { .msg = { .magic = NB_MAGIC,
                                      .cmd = NB_ACK,
                                      .arg = 0 } };

    // Not sure if we came here from advertising or ack'ing a previous
    // file. Either way, there's no significant penalty to calling these
    // functions, and they are a nop if no event is installed.
    eloop_rm_event(advertise);
    eloop_rm_event(send_ack);

    msg->data[len - 1] = 0;
    for (int i = 0; i < (len - 1); i++) {
        if ((msg->data[i] < ' ') || (msg->data[i] > 127)) {
            msg->data[i] = '.';
        }
    }

    total_size = msg->arg;
    item = netboot_get_buffer((const char*)msg->data, msg->arg);
    if (!item) {
        printf("netboot: Rejected File '%s'...\n", (char*) msg->data);
        ack.msg.cmd = NB_ERROR_BAD_FILE;
        udp6_send(&ack.msg, sizeof(nbmsg), addr_info.saddr, addr_info.sport,
                  NB_SERVER_PORT);
        return;
    }

    item->offset = 0;
    printf("netboot: Receive File '%s' from port %d...\n",
           (char*) msg->data, (int)addr_info.sport);
    recv_map = recv_map_new(msg->arg);
    packet_size = -1;
    ack.sport = NB_SERVER_PORT;
    memcpy(&ack.daddr, addr_info.saddr, sizeof(ip6_addr));
    ack.dport = addr_info.sport;
    ack.msg.cookie = msg->cookie;
    eloop_add_event(ACK_RATE, send_ack, &ack);
    udp6_rm_handler(receive_file);
    udp6_add_handler(NB_DATA, receive_first_data);
    udp6_add_handler(NB_LAST_DATA, receive_last_data);
}

void netboot_poll(void) {
    eloop_add_filter(require_network, NULL);
    eloop_add_event(FAST_ADVERTISE_RATE, advertise_fast, NULL);
    eloop_add_event(0, netifc_poll, NULL);

    // This is the message we're anticipating to get things started
    udp6_add_handler(NB_SEND_FILE, receive_file);

    // These are the messages we need to be able to handle asynchronously
    udp6_add_handler(NB_COMMAND, handle_command);
    udp6_add_handler(NB_QUERY, send_query_ack);
    udp6_add_handler(NB_BOOT, start_boot);

    eloop_start();

    udp6_rm_all_handlers();
}

void netboot_close(void) {
    netifc_close();
}
