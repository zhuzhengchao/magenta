// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdio.h>
#include <threads.h>

#include <magenta/syscalls.h>
#include <magenta/syscalls/port.h>

#include <unittest/unittest.h>

static bool basic_test(void) {
    BEGIN_TEST;
    mx_status_t status;

    mx_handle_t port;
    status = mx_port_create(MX_PORT_OPT_V2, &port);
    EXPECT_EQ(status, 0, "could not create port v2");

    const mx_port_packet_t in = {
        12ull,
        MX_PKT_TYPE_USER + 5u,    // kernel overrides the |type|.
        -3,
        { {} }
    };

    mx_port_packet_t out = {};

    status = mx_port_queue(port, nullptr, 0u);
    EXPECT_EQ(status, ERR_INVALID_ARGS, "");

    status = mx_port_queue(port, &in, 0u);
    EXPECT_EQ(status, NO_ERROR, "");

    status = mx_port_wait(port, MX_TIME_INFINITE, &out, 0u);
    EXPECT_EQ(status, NO_ERROR, "");

    EXPECT_EQ(out.key, 12u, "");
    EXPECT_EQ(out.type, MX_PKT_TYPE_USER, "");
    EXPECT_EQ(out.status, -3, "");

    EXPECT_EQ(memcmp(&in.user, &out.user, sizeof(mx_port_packet_t::user)), 0, "");

    status = mx_handle_close(port);
    EXPECT_EQ(status, NO_ERROR, "");

    END_TEST;
}

static bool queue_and_close_test(void) {
    BEGIN_TEST;
    mx_status_t status;

    mx_handle_t port;
    status = mx_port_create(MX_PORT_OPT_V2, &port);
    EXPECT_EQ(status, NO_ERROR, "could not create port v2");

    mx_port_packet_t out0 = {};
    status = mx_port_wait(port, 1000u, &out0, 0u);
    EXPECT_EQ(status, ERR_TIMED_OUT, "");

    const mx_port_packet_t in = {
        1ull,
        MX_PKT_TYPE_USER,
        0,
        { {} }
    };

    status = mx_port_queue(port, &in, 0u);
    EXPECT_EQ(status, NO_ERROR, "");

    status = mx_handle_close(port);
    EXPECT_EQ(status, NO_ERROR, "");

    END_TEST;
}

static bool async_wait_channel_test(void) {
    BEGIN_TEST;
    mx_status_t status;

    const uint64_t key0 = 6567ull;

    mx_handle_t port;
    status = mx_port_create(MX_PORT_OPT_V2, &port);
    EXPECT_EQ(status, NO_ERROR, "");

    mx_handle_t ch[2];
    status = mx_channel_create(0u, &ch[0], &ch[1]);
    EXPECT_EQ(status, NO_ERROR, "");

    for (int ix = 0; ix != 5; ++ix) {
        mx_port_packet_t out = {};
        status = mx_object_wait_async(ch[1], port, key0, MX_CHANNEL_READABLE, MX_WAIT_ASYNC_ONCE);
        EXPECT_EQ(status, NO_ERROR, "");

        status = mx_port_wait(port, 200000u, &out, 0u);
        EXPECT_EQ(status, ERR_TIMED_OUT, "");

        status = mx_channel_write(ch[0], 0u, "here", 4, nullptr, 0u);
        EXPECT_EQ(status, NO_ERROR, "");

        status = mx_port_wait(port, MX_TIME_INFINITE, &out, 0u);
        EXPECT_EQ(status, NO_ERROR, "");

        EXPECT_EQ(out.key, key0, "");
        EXPECT_EQ(out.type, MX_PKT_TYPE_SIGNAL_ONE, "");
        EXPECT_EQ(out.signal.effective, MX_CHANNEL_WRITABLE | MX_CHANNEL_READABLE, "");
        EXPECT_EQ(out.signal.trigger, MX_CHANNEL_READABLE, "");
        EXPECT_EQ(out.signal.count, 1u, "");

        status = mx_channel_read(ch[1], MX_CHANNEL_READ_MAY_DISCARD,
                                 nullptr, 0u, nullptr, nullptr, 0, nullptr);
        EXPECT_EQ(status, ERR_BUFFER_TOO_SMALL, "");
    }

    mx_port_packet_t out1 = {};

    status = mx_port_wait(port, 200000u, &out1, 0u);
    EXPECT_EQ(status, ERR_TIMED_OUT, "");

    status = mx_object_wait_async(ch[1], port, key0, MX_CHANNEL_READABLE, MX_WAIT_ASYNC_ONCE);
    EXPECT_EQ(status, NO_ERROR, "");

    status = mx_handle_close(ch[1]);
    EXPECT_EQ(status, NO_ERROR, "");

    status = mx_handle_close(ch[0]);
    EXPECT_EQ(status, NO_ERROR, "");

    status = mx_handle_close(port);
    EXPECT_EQ(status, NO_ERROR, "");

    END_TEST;
}

static bool async_wait_close_order(const int order[3], uint32_t wait_option) {
    BEGIN_TEST;
    mx_status_t status;

    const uint64_t key0 = 1122ull;

    mx_handle_t port;
    status = mx_port_create(MX_PORT_OPT_V2, &port);
    EXPECT_EQ(status, NO_ERROR, "");

    mx_handle_t ch[2];
    status = mx_channel_create(0u, &ch[0], &ch[1]);
    EXPECT_EQ(status, NO_ERROR, "");

    status = mx_object_wait_async(ch[1], port, key0,
        MX_CHANNEL_READABLE | MX_CHANNEL_PEER_CLOSED, wait_option);
    EXPECT_EQ(status, NO_ERROR, "");

    for (int ix = 0; ix != 3; ++ix) {
        switch (order[ix]) {
        case 0: status = mx_handle_close(ch[1]); break;
        case 1: status = mx_handle_close(ch[0]); break;
        case 2: status = mx_handle_close(port); break;
        }
        EXPECT_EQ(status, NO_ERROR, "");
    }

    END_TEST;
}

static bool async_wait_close_order_1() {
    int order[] = {0, 1, 2};
    return async_wait_close_order(order, MX_WAIT_ASYNC_ONCE) &&
           async_wait_close_order(order, MX_WAIT_ASYNC_REPEATING);
}

static bool async_wait_close_order_2() {
    int order[] = {0, 2, 1};
    return async_wait_close_order(order, MX_WAIT_ASYNC_ONCE) &&
           async_wait_close_order(order, MX_WAIT_ASYNC_REPEATING);
}

static bool async_wait_close_order_3() {
    int order[] = {1, 2, 0};
    return async_wait_close_order(order, MX_WAIT_ASYNC_ONCE) &&
           async_wait_close_order(order, MX_WAIT_ASYNC_REPEATING);
}

static bool async_wait_close_order_4() {
    int order[] = {1, 0, 2};
    return async_wait_close_order(order, MX_WAIT_ASYNC_ONCE) &&
           async_wait_close_order(order, MX_WAIT_ASYNC_REPEATING);
}

static bool async_wait_close_order_5() {
    int order[] = {2, 1, 0};
    return async_wait_close_order(order, MX_WAIT_ASYNC_ONCE) &&
           async_wait_close_order(order, MX_WAIT_ASYNC_REPEATING);
}

static bool async_wait_close_order_6() {
    int order[] = {2, 0, 1};
    return async_wait_close_order(order, MX_WAIT_ASYNC_ONCE) &&
           async_wait_close_order(order, MX_WAIT_ASYNC_REPEATING);
}

static bool async_wait_event_test_single(void) {
    BEGIN_TEST;
    mx_status_t status;

    mx_handle_t port;
    status = mx_port_create(MX_PORT_OPT_V2, &port);
    EXPECT_EQ(status, NO_ERROR, "");

    mx_handle_t ev;
    status = mx_event_create(0u, &ev);
    EXPECT_EQ(status, NO_ERROR, "");

    const uint32_t kNumAwaits = 7;

    for (uint32_t ix = 0; ix != kNumAwaits; ++ix) {
        status = mx_object_wait_async(ev, port, ix, MX_EVENT_SIGNALED, MX_WAIT_ASYNC_ONCE);
        EXPECT_EQ(status, NO_ERROR, "");
    }

    status = mx_object_signal(ev, 0u, MX_EVENT_SIGNALED);
    EXPECT_EQ(status, NO_ERROR, "");

    mx_port_packet_t out = {};
    uint64_t key_sum = 0u;

    for (uint32_t ix = 0; ix != (kNumAwaits - 2); ++ix) {
        EXPECT_EQ(status, NO_ERROR, "");
        status = mx_port_wait(port, MX_TIME_INFINITE, &out, 0u);
        EXPECT_EQ(status, NO_ERROR, "");
        key_sum += out.key;
        EXPECT_EQ(out.type, MX_PKT_TYPE_SIGNAL_ONE, "");
        EXPECT_EQ(out.signal.count, 1u, "");
    }

    EXPECT_EQ(key_sum, 20u, "");

    // The port has packets left in it.
    status = mx_handle_close(port);
    EXPECT_EQ(status, NO_ERROR, "");

    status = mx_handle_close(ev);
    EXPECT_EQ(status, NO_ERROR, "");

    END_TEST;
}

static bool async_wait_event_test_repeat(void) {
    BEGIN_TEST;
    mx_status_t status;

    mx_handle_t port;
    status = mx_port_create(MX_PORT_OPT_V2, &port);
    EXPECT_EQ(status, NO_ERROR, "");

    mx_handle_t ev;
    status = mx_event_create(0u, &ev);
    EXPECT_EQ(status, NO_ERROR, "");

    const uint64_t key0 = 1122ull;

    status = mx_object_wait_async(ev, port, key0,
        MX_EVENT_SIGNALED | MX_USER_SIGNAL_2, MX_WAIT_ASYNC_REPEATING);
    EXPECT_EQ(status, NO_ERROR, "");

    for (int ix = 0; ix != 24; ++ix) {
        uint32_t ub = (ix % 2) ? 0u : MX_USER_SIGNAL_2;
        status = mx_object_signal(ev, 0u, MX_EVENT_SIGNALED | ub);
        status = mx_object_signal(ev, MX_EVENT_SIGNALED | ub, 0u);
        EXPECT_EQ(status, NO_ERROR, "");
    }

    const mx_port_packet_t in = {12ull, MX_PKT_TYPE_USER, 0, {{}}};
    status = mx_port_queue(port, &in, 0u);
    EXPECT_EQ(status, NO_ERROR, "");

    mx_port_packet_t out = {};
    uint64_t count[4] = {};

    while (true) {
        status = mx_port_wait(port, 0ull, &out, 0u);
        if (status != NO_ERROR)
            break;

        if (out.type == MX_PKT_TYPE_USER) {
            count[3] += 1;
        } else {
            ASSERT_EQ(out.type, MX_PKT_TYPE_SIGNAL_REP, "");
            ASSERT_EQ(out.signal.count, 1u, "");
            switch (out.signal.trigger) {
            case MX_EVENT_SIGNALED: count[0] += out.signal.count; break;
            case MX_USER_SIGNAL_2: count[1] += out.signal.count; break;
            default: count[2] += out.signal.count; break;
            }
        }
    }

    EXPECT_EQ(status, ERR_TIMED_OUT, "");
    EXPECT_EQ(count[0], 24u, "");
    EXPECT_EQ(count[1], 12u, "");
    EXPECT_EQ(count[2], 0u, "");
    EXPECT_EQ(count[3], 1u, "");

    status = mx_handle_close(port);
    EXPECT_EQ(status, NO_ERROR, "");

    status = mx_object_signal(ev, 0u, MX_EVENT_SIGNALED | MX_USER_SIGNAL_2);
    EXPECT_EQ(status, NO_ERROR, "");

    status = mx_handle_close(ev);
    EXPECT_EQ(status, NO_ERROR, "");

    END_TEST;
}


BEGIN_TEST_CASE(port_tests)
RUN_TEST(basic_test)
RUN_TEST(queue_and_close_test)
RUN_TEST(async_wait_channel_test)
RUN_TEST(async_wait_event_test_single)
RUN_TEST(async_wait_event_test_repeat)
RUN_TEST(async_wait_close_order_1);
RUN_TEST(async_wait_close_order_2);
RUN_TEST(async_wait_close_order_3);
RUN_TEST(async_wait_close_order_4);
RUN_TEST(async_wait_close_order_5);
RUN_TEST(async_wait_close_order_6);
END_TEST_CASE(port_tests)

#ifndef BUILD_COMBINED_TESTS
int main(int argc, char** argv) {
    return unittest_run_all_tests(argc, argv) ? 0 : -1;
}
#endif
