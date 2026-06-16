/**
 * @file        codegen/parallel_for.h
 * @brief       Deterministic index-parallel map used by the codegen writer
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <atomic>
#include <cstddef>
#include <thread>
#include <type_traits>
#include <vector>

namespace rex::codegen {

/// Resolve the worker-thread count for a parallel pass.
/// @param requested   Configured count; 0 means autodetect via
///                     std::thread::hardware_concurrency().
/// @param item_count  Number of work items.
/// @returns Worker count clamped to [1, item_count], or 0 when there is no work.
inline unsigned resolveWorkerCount(unsigned requested, std::size_t item_count) {
  if (item_count == 0)
    return 0;
  unsigned hw = std::thread::hardware_concurrency();
  unsigned workers = requested ? requested : (hw ? hw : 1u);
  if (workers > item_count)
    workers = static_cast<unsigned>(item_count);
  return workers;
}

/// Evaluate `fn(i)` for every i in [0, count) and return the results in a vector
/// indexed by i.
///
/// Work is distributed across worker threads via an atomic cursor, but each
/// result is written only to its own slot, so `out[i]` depends solely on
/// `fn(i)` -- never on scheduling or the worker count. The output is therefore
/// byte-for-byte identical regardless of how many threads run. With <= 1 worker
/// the pass runs inline on the calling thread.
///
/// `fn` is invoked exactly once per index and must be safe to call concurrently
/// (it may read shared-const state but must not mutate anything shared).
template <class Fn>
auto parallelMap(std::size_t count, unsigned requested_workers, Fn&& fn)
    -> std::vector<std::decay_t<decltype(fn(std::size_t{}))>> {
  using T = std::decay_t<decltype(fn(std::size_t{}))>;
  std::vector<T> out(count);
  if (count == 0)
    return out;

  const unsigned workers = resolveWorkerCount(requested_workers, count);
  if (workers <= 1) {
    for (std::size_t i = 0; i < count; ++i)
      out[i] = fn(i);
    return out;
  }

  std::atomic<std::size_t> cursor{0};
  auto worker = [&]() {
    for (;;) {
      std::size_t i = cursor.fetch_add(1, std::memory_order_relaxed);
      if (i >= count)
        break;
      out[i] = fn(i);
    }
  };

  std::vector<std::thread> pool;
  pool.reserve(workers - 1);
  for (unsigned t = 0; t + 1 < workers; ++t)
    pool.emplace_back(worker);
  worker();  // the calling thread takes a share too
  for (auto& th : pool)
    th.join();

  return out;
}

}  // namespace rex::codegen
