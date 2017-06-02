// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include <xefi.h>

#include "osboot.h"
#include "receivemap.h"

// We don't know how large our packet size will be until we start receiving
// messages. However, we don't want to wait until our first message to
// initialize the table, for fear of missing packets. So... make some
// assumptions about packet size (they will at least be 1024 bytes). If this is
// violated, we will terminate ourselves.
#define MIN_PACKET_SIZE 1024

// The receive map is organized into a doubly-linked list of entries, indexed
// by packet number. The  presence of a -1 in the "next" pointer indicates that
// a packet has already been received. This gives us good characteristics for
// our necessary operations:
//   Remove: constant time
//   Lookup: constant time
//   Iterate: linear on the number of remaining entries
// We also keep track of the number of locations remaining in the list as we
// go, so that we can calculate the number of received or unreceived packets
// in constant time.
typedef struct recv_map_entry {
    uint32_t prev;
    uint32_t next;
} recv_map_entry_t;

struct recv_map {
    uint32_t head;
    size_t num_entries;
    size_t packets_outstanding;
    ssize_t packet_size;
    size_t file_size;
    size_t pages_allocated;
    recv_map_entry_t entries[];
};

static void dump_map(recv_map_t map) __attribute__((unused));
static void dump_map(recv_map_t map) {
    printf("Receive map:\n");
    printf("    head: %d\n", (int)map->head);
    printf("    num_entries: %d\n", (int)map->num_entries);
    printf("    packets_outstanding: %d\n", (int)map->packets_outstanding);
    printf("    packet_size: %d\n", (int)map->packet_size);
    printf("    file_size: %d\n", (int)map->file_size);
    printf("    pages_allocated: %d\n", (int)map->pages_allocated);
}

static void verify_map_size_known(recv_map_t map) {
    if (map == NULL) {
        printf("Internal error: receive map not allocated\n");
        while (1)
            ;
    }
    if (map->packet_size == -1) {
        printf("Internal error: receive map size not set\n");
        while (1)
            ;
    }
}

recv_map_t recv_map_new(size_t filesize) {
    recv_map_t result = (void *)0xffffffff;
    size_t min_entries = (filesize + (RECV_MAP_MIN_PACKET_SIZE - 1)) /
                         RECV_MAP_MIN_PACKET_SIZE;
    size_t total_size_needed = sizeof(struct recv_map) +
                               (min_entries * sizeof(recv_map_entry_t));
    size_t actual_size = (total_size_needed + PAGE_MASK) & ~PAGE_SIZE;
    size_t actual_pages = actual_size / PAGE_SIZE;
    size_t actual_entries = (actual_size - sizeof(struct recv_map)) /
                            sizeof(recv_map_entry_t);

    if (gBS->AllocatePages(AllocateMaxAddress, EfiLoaderData, actual_pages,
                           (efi_physical_addr*)&result) != EFI_SUCCESS) {
        printf("Unable to allocate receive map buffers!\n");
        return NULL;
    }

    result->head = 0;
    result->num_entries = actual_entries;
    result->packets_outstanding = 0;
    result->packet_size = -1;
    result->file_size = filesize;
    result->pages_allocated = actual_pages;
    result->entries[0].prev = -1;
    uint32_t ndx;
    for (ndx = 0; ndx < (actual_entries - 1); ndx++) {
        result->entries[ndx].next = ndx + 1;
        result->entries[ndx + 1].prev = ndx;
    }
    result->entries[ndx].next = -1;
    return result;
}

void recv_map_set_size(recv_map_t map, ssize_t packet_size) {
    if (map->packet_size != -1) {
        printf("Internal error: attempt to change receive map size, already set\n");
        while (1)
            ;
    }

    map->packet_size = packet_size;

    size_t num_entries = (map->file_size + (packet_size - 1)) /
                         packet_size;
    if (num_entries > map->num_entries) {
        printf("Internal error: receive map not big enough, packet size likely too small\n");
        printf("                packet size is expected to be at least %d bytes\n",
               RECV_MAP_MIN_PACKET_SIZE);
        while (1)
            ;
    }

    // Now that we know how many entries for certain, terminate the list
    map->entries[num_entries - 1].next = -1;
    map->packets_outstanding = num_entries;
    map->num_entries = num_entries;
}

bool recv_map_mark_received(recv_map_t map, size_t offset) {
    verify_map_size_known(map);

    if ((offset % map->packet_size) != 0) {
        printf("Internal error: unexpected file offset value for receive map\n");
        while (1)
            ;
    }

    size_t ndx = offset / map->packet_size;
    uint32_t next_ndx = map->entries[ndx].next;
    uint32_t prev_ndx = map->entries[ndx].prev;

    // Check to see if it's already been written
    if (next_ndx == 0)
        return false;

    // Clear out the location
    map->entries[ndx].prev = 0;
    map->entries[ndx].next = 0;

    if (next_ndx != -1)
        map->entries[next_ndx].prev = prev_ndx;

    if (prev_ndx == -1)
        map->head = next_ndx;
    else
        map->entries[prev_ndx].next = next_ndx;

    map->packets_outstanding--;
    return true;
}

bool recv_map_isempty(recv_map_t map) {
    verify_map_size_known(map);
    return map->packets_outstanding == 0;
}

uint32_t recv_map_first(recv_map_t map) {
    verify_map_size_known(map);
    return map->head;
}

uint32_t recv_map_next(recv_map_t map, uint32_t ndx) {
    verify_map_size_known(map);
    return map->entries[ndx].next;
}

unsigned int recv_map_progress(recv_map_t map) {
    verify_map_size_known(map);
    return ((map->num_entries - map->packets_outstanding) * 100) /
           map->num_entries;
}

void recv_map_delete(recv_map_t map) {
    map->packet_size = -1;
    if (gBS->FreePages((efi_physical_addr)map, map->pages_allocated) !=
        EFI_SUCCESS) {
        printf("Failed to free receive map buffers!\n");
        while (1)
            ;
    }
}

