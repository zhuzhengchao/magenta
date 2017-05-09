// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fuzz/seeded_prng.h>

#include <stddef.h>
#include <stdint.h>

#include <fuzz/fuzzer.h>
#include <magenta/types.h>
#include <unittest/unittest.h>

#include "util.h"

namespace {

using fuzz::Fuzzer;
using fuzz::SeededPRNG;

bool SeededPrngChain() {
    BEGIN_TEST_WITH_RC;
    SeededPRNG prng1, prng2;
    Fuzzer fuzzer1, fuzzer2;
    // Chain without fuzzer.
    ASSERT_RC(prng1.Chain(nullptr, nullptr), ERR_INVALID_ARGS);
    // Make a single element chain.
    ASSERT_RC(prng1.Chain(&fuzzer1, nullptr), NO_ERROR);
    // Chain when already chained.
    ASSERT_RC(prng1.Chain(&fuzzer1, nullptr), ERR_BAD_STATE);
    // Chain with mismatched fuzzer.
    ASSERT_RC(prng2.Chain(&fuzzer2, &prng1), ERR_INVALID_ARGS);
    // Make a 2 element chain.
    ASSERT_RC(prng2.Chain(&fuzzer1, &prng1), NO_ERROR);
    END_TEST;
}

bool SeededPrngCheckLengths() {
    BEGIN_TEST_WITH_RC;
    SeededPRNG prng1, prng2;
    Fuzzer fuzzer;
    // Check unchained length
    size_t length = prng1.GetStateLength();
    ASSERT_EQ(prng1.GetSnapshotLength(), length, "unchained total != length");
    // Check chained length
    ASSERT_RC(prng1.Chain(&fuzzer, nullptr), NO_ERROR);
    ASSERT_RC(prng2.Chain(&fuzzer, &prng1), NO_ERROR);
    ASSERT_EQ(prng2.GetSnapshotLength(), length * 2,
              "chained total != sum of lengths");
    END_TEST;
}

bool SeededPrngDraw() {
    BEGIN_TEST_WITH_RC;
    SeededPRNG prng1;
    Fuzzer fuzzer;
    uint64_t x, y;
    // Draw without initializing (via Chain)
    ASSERT_RC(prng1.Draw(&x, sizeof(x)), ERR_BAD_STATE);
    // Initialize and draw
    ASSERT_RC(prng1.Chain(&fuzzer, nullptr), NO_ERROR);
    ASSERT_RC(prng1.Draw(&x, sizeof(x)), NO_ERROR);
    // Draw too much, but only if size_t is more than 32 bits.
    size_t n = UINT32_MAX + 1;
    if (n != 0) {
        prng1.Draw(&x, UINT32_MAX + 1);
    }
    // Check that outputs change.
    prng1.Draw(&x, sizeof(x));
    prng1.Draw(&y, sizeof(y));
    // P(x == y) is about 2^-128.  It's possible, but if it happens it is
    // overwhelmingly more likely due to a bug than due to getting "lucky".
    ASSERT_NEQ(x, y, "not random enough");
    END_TEST;
}

bool SeededPrngRevert() {
    BEGIN_TEST_WITH_RC;
    SeededPRNG prng1, prng2;
    Fuzzer fuzzer;
    uint64_t drawn1, drawn2, x;
    // Chain handlers
    ASSERT_RC(prng2.Chain(&fuzzer, nullptr), NO_ERROR);
    ASSERT_RC(prng1.Chain(&fuzzer, &prng2), NO_ERROR);
    // Allocate snapshots
    size_t total = prng1.GetSnapshotLength();
    uint8_t snapshot[total + 1];
    // Randomize state
    prng1.Draw(snapshot, total);
    ASSERT_RC(prng1.Revert(snapshot, total + 1), NO_ERROR);
    ASSERT_RC(prng1.Revert(snapshot, total - 1), ERR_BUFFER_TOO_SMALL);
    ASSERT_RC(prng2.Revert(snapshot, total), NO_ERROR);
    ASSERT_RC(prng1.Revert(snapshot, total), NO_ERROR);
    // Save values after snapshot
    prng1.Draw(&drawn1, sizeof(drawn1));
    prng2.Draw(&drawn2, sizeof(drawn2));
    // Revert to snapshot
    ASSERT_RC(prng1.Revert(snapshot, total), NO_ERROR);
    // Check the reverted values
    prng1.Draw(&x, sizeof(x));
    ASSERT_EQ(x, drawn1, "not same after revert");
    prng2.Draw(&x, sizeof(x));
    ASSERT_EQ(x, drawn2, "not same after revert");
    END_TEST;
}

bool SeededPrngSnapshot() {
    BEGIN_TEST_WITH_RC;
    SeededPRNG prng1, prng2;
    Fuzzer fuzzer;
    uint64_t drawn1, drawn2, x;
    // Chain handlers
    ASSERT_RC(prng2.Chain(&fuzzer, nullptr), NO_ERROR);
    ASSERT_RC(prng1.Chain(&fuzzer, &prng2), NO_ERROR);
    // Allocate snapshots
    size_t total = prng1.GetSnapshotLength();
    uint8_t snapshot1[total + 1];
    uint8_t snapshot2[total + 1];
    // Randomize state
    prng1.Draw(snapshot1, total);
    ASSERT_RC(prng1.Revert(snapshot1, total), NO_ERROR);
    // Modify the state
    prng1.Draw(&x, sizeof(x));
    prng1.Draw(&x, sizeof(x));
    prng2.Draw(&x, sizeof(x));
    // Snapshot the current state
    ASSERT_RC(prng1.Snapshot(snapshot2, total + 1), NO_ERROR);
    ASSERT_RC(prng1.Snapshot(snapshot2, total - 1), ERR_BUFFER_TOO_SMALL);
    ASSERT_RC(prng2.Snapshot(snapshot2, total), NO_ERROR);
    ASSERT_RC(prng1.Snapshot(snapshot2, total), NO_ERROR);
    // Save values after snapshot
    prng1.Draw(&drawn1, sizeof(drawn1));
    prng2.Draw(&drawn2, sizeof(drawn2));
    // Revert to snapshot
    ASSERT_RC(prng1.Revert(snapshot1, total), NO_ERROR);
    ASSERT_RC(prng1.Revert(snapshot2, total), NO_ERROR);
    // Check the reverted values
    prng1.Draw(&x, sizeof(x));
    ASSERT_EQ(x, drawn1, "not same after revert");
    prng2.Draw(&x, sizeof(x));
    ASSERT_EQ(x, drawn2, "not same after revert");
    END_TEST;
}

} // namespace

BEGIN_TEST_CASE(FuzzSeededPrngTests)
RUN_TEST(SeededPrngChain)
RUN_TEST(SeededPrngCheckLengths)
RUN_TEST(SeededPrngDraw)
RUN_TEST(SeededPrngRevert)
RUN_TEST(SeededPrngSnapshot)
END_TEST_CASE(FuzzSeededPrngTests)
