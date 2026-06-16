/**
 * @file        tests/unit/codegen/parallel_for_test.cpp
 * @brief       Unit tests for the deterministic index-parallel map (parallel_for.h)
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <catch2/catch_test_macros.hpp>

#include "codegen/parallel_for.h"

#include <atomic>
#include <cstddef>
#include <string>
#include <vector>

#include <fmt/format.h>

using rex::codegen::parallelMap;
using rex::codegen::resolveWorkerCount;

// Worker counts exercised by the determinism/completeness tests. Includes 1
// (serial path), small counts, and counts far exceeding any realistic core
// count to force heavy oversubscription.
static const std::vector<unsigned> kWorkerCounts = {1, 2, 3, 4, 8, 16, 64, 256};

// ---------------------------------------------------------------------------
// resolveWorkerCount
// ---------------------------------------------------------------------------

TEST_CASE("resolveWorkerCount returns 0 for no work", "[codegen][parallel]") {
  REQUIRE(resolveWorkerCount(0, 0) == 0);
  REQUIRE(resolveWorkerCount(8, 0) == 0);
}

TEST_CASE("resolveWorkerCount clamps to item count", "[codegen][parallel]") {
  REQUIRE(resolveWorkerCount(64, 1) == 1);
  REQUIRE(resolveWorkerCount(64, 5) == 5);
  REQUIRE(resolveWorkerCount(4, 100) == 4);
}

TEST_CASE("resolveWorkerCount autodetects when requested is 0", "[codegen][parallel]") {
  // 0 = autodetect: result must be a usable count in [1, item_count].
  unsigned w = resolveWorkerCount(0, 1000);
  REQUIRE(w >= 1);
  REQUIRE(w <= 1000);
}

TEST_CASE("resolveWorkerCount honors an explicit request", "[codegen][parallel]") {
  REQUIRE(resolveWorkerCount(3, 1000) == 3);
}

// ---------------------------------------------------------------------------
// parallelMap: edge cases
// ---------------------------------------------------------------------------

TEST_CASE("parallelMap on empty range yields empty result", "[codegen][parallel]") {
  for (unsigned w : kWorkerCounts) {
    auto out = parallelMap(0, w, [](std::size_t i) { return i; });
    REQUIRE(out.empty());
  }
}

TEST_CASE("parallelMap on a single element runs once", "[codegen][parallel]") {
  std::atomic<int> calls{0};
  auto out = parallelMap(1, 64, [&](std::size_t i) {
    calls.fetch_add(1, std::memory_order_relaxed);
    return i * 10;
  });
  REQUIRE(out.size() == 1);
  REQUIRE(out[0] == 0);
  REQUIRE(calls.load() == 1);
}

// ---------------------------------------------------------------------------
// parallelMap: index mapping is preserved
// ---------------------------------------------------------------------------

TEST_CASE("parallelMap writes each result to its own slot", "[codegen][parallel]") {
  constexpr std::size_t kCount = 1000;
  for (unsigned w : kWorkerCounts) {
    auto out = parallelMap(kCount, w, [](std::size_t i) { return i * 2 + 1; });
    REQUIRE(out.size() == kCount);
    for (std::size_t i = 0; i < kCount; ++i) {
      REQUIRE(out[i] == i * 2 + 1);
    }
  }
}

// ---------------------------------------------------------------------------
// parallelMap: completeness -- every index processed exactly once
// ---------------------------------------------------------------------------

TEST_CASE("parallelMap processes every index exactly once", "[codegen][parallel]") {
  constexpr std::size_t kCount = 4096;
  for (unsigned w : kWorkerCounts) {
    std::vector<std::atomic<int>> visits(kCount);
    for (auto& v : visits)
      v.store(0, std::memory_order_relaxed);

    auto out = parallelMap(kCount, w, [&](std::size_t i) {
      visits[i].fetch_add(1, std::memory_order_relaxed);
      return i;
    });

    REQUIRE(out.size() == kCount);
    bool all_once = true;
    for (std::size_t i = 0; i < kCount; ++i) {
      if (visits[i].load(std::memory_order_relaxed) != 1)
        all_once = false;
    }
    REQUIRE(all_once);
  }
}

// ---------------------------------------------------------------------------
// parallelMap: determinism -- output is identical regardless of thread count.
// This is the core invariant the codegen writer relies on (recompiled source
// must be byte-identical no matter how many workers run).
// ---------------------------------------------------------------------------

TEST_CASE("parallelMap output is independent of worker count", "[codegen][parallel]") {
  constexpr std::size_t kCount = 2000;

  // String workload mirrors emitCpp(): each item produces a distinct chunk of
  // text that gets concatenated downstream.
  auto emit = [](std::size_t i) {
    return fmt::format("// function {}\nvoid sub_{:08X}() {{ return; }}\n", i, 0x82000000u + i * 4);
  };

  std::vector<std::string> reference = parallelMap(kCount, 1, emit);  // serial baseline

  for (unsigned w : kWorkerCounts) {
    auto out = parallelMap(kCount, w, emit);
    REQUIRE(out == reference);
  }

  // Concatenated output (what the writer actually emits) must also match.
  std::string ref_joined;
  for (const auto& s : reference)
    ref_joined += s;

  for (unsigned w : kWorkerCounts) {
    auto out = parallelMap(kCount, w, emit);
    std::string joined;
    for (const auto& s : out)
      joined += s;
    REQUIRE(joined == ref_joined);
  }
}

// ---------------------------------------------------------------------------
// parallelMap: stress -- repeated heavily-oversubscribed runs stay correct.
// Repetition raises the odds of surfacing a data race or torn write.
// ---------------------------------------------------------------------------

TEST_CASE("parallelMap is stable under repeated oversubscription", "[codegen][parallel]") {
  constexpr std::size_t kCount = 5000;
  auto compute = [](std::size_t i) -> std::size_t { return (i * 2654435761u) ^ (i << 3); };

  auto reference = parallelMap(kCount, 1, compute);

  for (int rep = 0; rep < 25; ++rep) {
    auto out = parallelMap(kCount, 256, compute);
    REQUIRE(out == reference);
  }
}
