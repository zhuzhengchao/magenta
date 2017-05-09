// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/seeded_prng.h>

#include <string.h>

#include <boring-crypto/chacha.h>
#include <lib/crypto/cryptolib.h>
#include <magenta/assert.h>
#include <mxtl/algorithm.h>
#include <mxtl/auto_lock.h>

namespace fuzz {

SeededPRNG::SeededPRNG() : key_{0}, nonce_{0}, counter_(0) {}

SeededPRNG::~SeededPRNG() {}

static_assert(UINT32_MAX <= SIZE_MAX);
mx_status_t SeededPRNG::Draw(void* buf, size_t len) {
    mtx_t* mtx = GetLock();
    if (!mtx) {
        return ERR_BAD_STATE;
    }
    mxtl::AutoLock lock(mtx);
    if (len > UINT32_MAX) {
        return ERR_OUT_OF_RANGE;
    }
    memset(buf, 0, len);
    // Check for overflow.  Chacha20 uses a block size of 64 bytes.
    uint32_t len32 = static_cast<uint32_t>(len);
    uint32_t blk32 = 64;
    uint32_t delta = mxtl::roundup(len32, blk32);
    MX_DEBUG_ASSERT(len == 0 || delta != 0);
    if (counter_ + delta < counter_) {
        static_assert(sizeof(nonce_) <= clSHA256_DIGEST_SIZE);
        uint8_t tmp[clSHA256_DIGEST_SIZE];
        clSHA256(nonce_, sizeof(nonce_), tmp);
        memcpy(nonce_, tmp, sizeof(nonce_));
        counter_ = 0;
    }
    // We don't have to memset to get random vaklues, but do to get the same
    // random values for a given initial state.
    uint8_t* bytes = static_cast<uint8_t*>(buf);
    memset(bytes, 0, len);
    // Draw the bytes and increment the counter.
    CRYPTO_chacha_20(bytes, bytes, len, key_, nonce_, counter_);
    counter_ += delta;
    return SignalModified();
}

size_t SeededPRNG::GetStateLength() const {
    return sizeof(nonce_) + sizeof(counter_);
}

mx_status_t SeededPRNG::GetState(uint8_t* buf) const {
    memcpy(buf, nonce_, sizeof(nonce_));
    buf += sizeof(nonce_);
    memcpy(buf, &counter_, sizeof(counter_));
    return NO_ERROR;
}

mx_status_t SeededPRNG::SetState(const uint8_t* buf) {
    memcpy(nonce_, buf, sizeof(nonce_));
    buf += sizeof(nonce_);
    memcpy(&counter_, buf, sizeof(counter_));
    // For the key, just use the digest of the nonce.  There's sufficient
    // entropy for fuzzing already (128 bits) and no security implications.
    static_assert(sizeof(key_) == clSHA256_DIGEST_SIZE);
    clSHA256(nonce_, sizeof(nonce_), key_);
    return NO_ERROR;
}

} // namespace fuzz
