# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_NAME := fuzz/local-agent

MODULE_TYPE := userapp

MODULE_SRCS += \
    $(LOCAL_DIR)/local-agent.cpp

MODULE_LIBS := \
    system/ulib/c \
    system/ulib/launchpad \
    system/ulib/magenta \

MODULE_STATIC_LIBS := \
    system/ulib/fuzz \
    system/ulib/mx \
    system/ulib/mxalloc \
    system/ulib/mxcpp \
    system/ulib/mxtl \

include make/module.mk
