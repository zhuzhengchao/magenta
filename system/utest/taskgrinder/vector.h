// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <mxtl/array.h>
#include <mxtl/type_support.h>

// A simple subset of std::vector.
template <typename T>
class Vector : private mxtl::Array<T> {
private:
    static constexpr size_t kInitialCapacity = 8;
    typedef mxtl::Array<T> arrt;

public:
    Vector()
        : arrt(Alloc(kInitialCapacity), kInitialCapacity) {
    }

    void push_back(T&& val) {
        if (size() == size_) {
            const size_t new_size = (size_ + 1) * 2;
            // TODO(dbort): To do this properly, we'd need to avoid
            // constructing any elements beyond size_.
            T* a = Alloc(new_size);
            for (size_t i = 0; i < size_; i++) {
                a[i] = mxtl::move(arrt::operator[](i));
            }
            arrt::reset(a, new_size);
        }
        arrt::operator[](size_++) = mxtl::move(val);
    }

    T& operator[](size_t i) const {
        MX_DEBUG_ASSERT(i < size_);
        return arrt::operator[](i);
    }

    using arrt::begin;

    T* end() const { return begin() + size_; }

    size_t size() const { return size_; }

    bool empty() const { return size_ == 0; }

private:
    T* Alloc(size_t size) {
        return new T[size];
    }

    size_t size_ = 0;
};
