# Copyright 2017 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := usertest

MODULE_SRCS += \
    $(LOCAL_DIR)/agent.cpp \
    $(LOCAL_DIR)/channel.cpp \
    $(LOCAL_DIR)/fuzzer.cpp \
    $(LOCAL_DIR)/seeded_prng.cpp \
    $(LOCAL_DIR)/main.c


MODULE_NAME := fuzz-test

MODULE_LIBS := \
    system/ulib/c \
    system/ulib/launchpad \
    system/ulib/magenta \
    system/ulib/unittest \

MODULE_STATIC_LIBS := \
    system/ulib/fuzz \
    system/ulib/mx \
    system/ulib/mxalloc \
    system/ulib/mxcpp \
    system/ulib/mxtl \
    system/ulib/pretty \
    third_party/ulib/boring-crypto \
    third_party/ulib/cryptolib \


include make/module.mk
