// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#define RECV_MAP_MIN_PACKET_SIZE 1024

typedef struct recv_map *recv_map_t;

// Allocate a new map, and initialize it. Note that at this point we know the
// total filesize, but not packet size, which we won't have until the first
// packet arrives. Most of the heavy lifting is done here, however, so that
// we minimize response time when we are receiving packets.
recv_map_t recv_map_new(size_t filesize);

// Specify the packet size.
void recv_map_set_size(recv_map_t map, ssize_t packet_size);

// Mark a packet at the specified file offset as received. Returns true if
// the packet was already marked received, and false otherwise.
bool recv_map_mark_received(recv_map_t map, size_t offset);

bool recv_map_isempty(recv_map_t map);

// Iterators
uint32_t recv_map_first(recv_map_t map);
uint32_t recv_map_next(recv_map_t map, uint32_t ndx);

// Returns an integer value in the range [0,100] indicating % of packets
// already received.
unsigned int recv_map_progress(recv_map_t map);

void recv_map_delete(recv_map_t map);

