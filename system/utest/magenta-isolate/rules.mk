# Copyright 2016 The Fuchsia Authors. All rights reserved.
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

LOCAL_DIR := $(GET_LOCAL_DIR)

MODULE := $(LOCAL_DIR)

MODULE_TYPE := usertest

MODULE_NAME := magenta-isolate-test

MODULE_SRCS += \
	$(LOCAL_DIR)/main.cpp \
	kernel/lib/magenta/dispatcher.cpp \
	kernel/lib/magenta/event_dispatcher.cpp \
	kernel/lib/magenta/handle.cpp \
	kernel/lib/magenta/state_tracker.cpp

MODULE_LIBS := \
    system/ulib/unittest \
    system/ulib/mxio \
    system/ulib/magenta \
    system/ulib/c

MODULE_STATIC_LIBS := \
    system/ulib/mxcpp \
    system/ulib/mxtl

MODULE_COMPILEFLAGS := \
  -Ikernel/lib/magenta/include \
  -Isystem/ulib/mxtl/include

include make/module.mk
