// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <stdio.h> // TODO

#include <magenta/status.h>
#include <magenta/types.h>
#include <unittest/unittest.h>

#define BEGIN_TEST_WITH_RC                                                     \
    BEGIN_TEST;                                                                \
    mx_status_t __rc;
#define ASSERT_RC(expr, err)                                                   \
    ASSERT_EQ(__rc = (expr), err, mx_status_get_string(__rc))
