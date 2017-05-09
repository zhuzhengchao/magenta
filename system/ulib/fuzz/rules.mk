# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := userlib

MODULE_SRCS += \
    $(LOCAL_DIR)/agent.cpp \
    $(LOCAL_DIR)/channel.cpp \
    $(LOCAL_DIR)/fuzzer.cpp \
    $(LOCAL_DIR)/seeded_prng.cpp \
    $(LOCAL_DIR)/state_handler.cpp \

MODULE_LIBS := \
    system/ulib/launchpad \
    system/ulib/magenta \
    system/ulib/mx \
    system/ulib/mxalloc \
    system/ulib/mxcpp \
    system/ulib/mxtl \
    third_party/ulib/cryptolib \
    third_party/ulib/boring-crypto \

include make/module.mk
