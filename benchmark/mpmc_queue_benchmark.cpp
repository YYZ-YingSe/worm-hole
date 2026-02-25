#include <benchmark/benchmark.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>

#include "wh/core/mpmc_queue.hpp"

namespace {

using bounded_queue_t = wh::core::mpmc_queue<std::uint64_t, false>;
using dynamic_queue_t = wh::core::mpmc_queue<std::uint64_t, true>;

template <typename queue_t> struct shared_queue_state {
  std::unique_ptr<queue_t> queue{};
  std::atomic<bool> ready{false};
  std::atomic<std::uint64_t> latency_sum_ns{0U};
  std::atomic<std::uint64_t> latency_max_ns{0U};
  std::atomic<std::uint64_t> latency_samples{0U};
  std::atomic<std::uint32_t> barrier_arrived{0U};
  std::atomic<std::uint32_t> barrier_generation{0U};
};

template <typename queue_t>
void wait_for_threads(shared_queue_state<queue_t> &shared,
                      const std::uint32_t thread_count) {
  const auto generation = shared.barrier_generation.load(std::memory_order_acquire);
  if (shared.barrier_arrived.fetch_add(1U, std::memory_order_acq_rel) +
          1U ==
      thread_count) {
    shared.barrier_arrived.store(0U, std::memory_order_release);
    shared.barrier_generation.fetch_add(1U, std::memory_order_acq_rel);
    return;
  }

  while (shared.barrier_generation.load(std::memory_order_acquire) ==
         generation) {
    std::this_thread::yield();
  }
}

template <typename queue_t>
void prepare_shared_queue(benchmark::State &state,
                          shared_queue_state<queue_t> &shared,
                          const std::size_t capacity) {
  if (state.thread_index() == 0) {
    shared.queue = std::make_unique<queue_t>(capacity);
    shared.latency_sum_ns.store(0U, std::memory_order_relaxed);
    shared.latency_max_ns.store(0U, std::memory_order_relaxed);
    shared.latency_samples.store(0U, std::memory_order_relaxed);
    shared.ready.store(true, std::memory_order_release);
  }
  while (!shared.ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  wait_for_threads(shared, static_cast<std::uint32_t>(state.threads()));
}

template <typename queue_t>
void finalize_shared_queue(benchmark::State &state, shared_queue_state<queue_t> &shared) {
  wait_for_threads(shared, static_cast<std::uint32_t>(state.threads()));
  if (state.thread_index() == 0) {
    shared.ready.store(false, std::memory_order_release);
    shared.queue.reset();
  }
  wait_for_threads(shared, static_cast<std::uint32_t>(state.threads()));
}

template <typename queue_t>
void BM_mpmc_single_thread_try_push_pop(benchmark::State &state) {
  const auto capacity = static_cast<std::size_t>(state.range(0));
  queue_t queue(capacity);
  std::uint64_t value = 0U;

  for (auto _ : state) {
    auto pushed = queue.try_push(value++);
    benchmark::DoNotOptimize(pushed);
    if (!pushed.has_value()) {
      while (queue.try_pop().has_value()) {
      }
      pushed = queue.try_push(value++);
    }

    auto popped = queue.try_pop();
    benchmark::DoNotOptimize(popped);
  }

  state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) * 2);
}

template <typename queue_t>
void BM_mpmc_throughput_contended(benchmark::State &state) {
  const auto capacity = static_cast<std::size_t>(state.range(0));
  static shared_queue_state<queue_t> shared{};

  prepare_shared_queue(state, shared, capacity);

  const auto thread_index = static_cast<std::size_t>(state.thread_index());
  const bool is_producer = (thread_index % 2U) == 0U;
  std::uint64_t value = static_cast<std::uint64_t>(thread_index) << 48U;

  for (auto _ : state) {
    if (is_producer) {
      while (true) {
        auto status = shared.queue->try_push(value++);
        if (status.has_value()) {
          break;
        }
        std::this_thread::yield();
      }
    } else {
      while (true) {
        auto status = shared.queue->try_pop();
        if (status.has_value()) {
          auto popped = status.value();
          benchmark::DoNotOptimize(std::move(popped));
          break;
        }
        std::this_thread::yield();
      }
    }
  }

  if (state.thread_index() == 0) {
    state.SetItemsProcessed(static_cast<std::int64_t>(state.iterations()) *
                            static_cast<std::int64_t>(state.threads()));
  }
  finalize_shared_queue(state, shared);
}

template <typename queue_t>
void BM_mpmc_handoff_latency(benchmark::State &state) {
  const auto capacity = static_cast<std::size_t>(state.range(0));
  static shared_queue_state<queue_t> shared{};

  prepare_shared_queue(state, shared, capacity);

  const bool is_producer = state.thread_index() == 0;

  for (auto _ : state) {
    if (is_producer) {
      const auto now = std::chrono::steady_clock::now().time_since_epoch();
      const auto stamp = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
      while (!shared.queue->try_push(stamp).has_value()) {
        std::this_thread::yield();
      }
    } else {
      std::uint64_t stamp = 0U;
      while (true) {
        auto popped = shared.queue->try_pop();
        if (popped.has_value()) {
          stamp = popped.value();
          break;
        }
        std::this_thread::yield();
      }

      const auto now = std::chrono::steady_clock::now().time_since_epoch();
      const auto now_ns = static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
      const auto latency_ns = now_ns - stamp;

      shared.latency_sum_ns.fetch_add(latency_ns, std::memory_order_relaxed);
      shared.latency_samples.fetch_add(1U, std::memory_order_relaxed);

      auto current_max = shared.latency_max_ns.load(std::memory_order_relaxed);
      while (latency_ns > current_max &&
             !shared.latency_max_ns.compare_exchange_weak(
                 current_max, latency_ns, std::memory_order_relaxed,
                 std::memory_order_relaxed)) {
      }
    }
  }

  if (state.thread_index() == 0) {
    const auto samples = shared.latency_samples.load(std::memory_order_relaxed);
    if (samples > 0U) {
      const auto sum = shared.latency_sum_ns.load(std::memory_order_relaxed);
      state.counters["avg_latency_ns"] =
          benchmark::Counter(static_cast<double>(sum) /
                             static_cast<double>(samples));
      state.counters["max_latency_ns"] = benchmark::Counter(static_cast<double>(
          shared.latency_max_ns.load(std::memory_order_relaxed)));
    }
    state.SetItemsProcessed(static_cast<std::int64_t>(samples));
  }
  finalize_shared_queue(state, shared);
}

void BM_mpmc_bounded_single_thread(benchmark::State &state) {
  BM_mpmc_single_thread_try_push_pop<bounded_queue_t>(state);
}

void BM_mpmc_dynamic_single_thread(benchmark::State &state) {
  BM_mpmc_single_thread_try_push_pop<dynamic_queue_t>(state);
}

void BM_mpmc_bounded_throughput(benchmark::State &state) {
  BM_mpmc_throughput_contended<bounded_queue_t>(state);
}

void BM_mpmc_dynamic_throughput(benchmark::State &state) {
  BM_mpmc_throughput_contended<dynamic_queue_t>(state);
}

void BM_mpmc_bounded_latency(benchmark::State &state) {
  BM_mpmc_handoff_latency<bounded_queue_t>(state);
}

void BM_mpmc_dynamic_latency(benchmark::State &state) {
  BM_mpmc_handoff_latency<dynamic_queue_t>(state);
}

BENCHMARK(BM_mpmc_bounded_single_thread)->Arg(1024);
BENCHMARK(BM_mpmc_dynamic_single_thread)->Arg(1024);

BENCHMARK(BM_mpmc_bounded_throughput)
    ->Arg(65536)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->Threads(32);
BENCHMARK(BM_mpmc_dynamic_throughput)
    ->Arg(65536)
    ->Threads(2)
    ->Threads(4)
    ->Threads(8)
    ->Threads(16)
    ->Threads(32);

BENCHMARK(BM_mpmc_bounded_latency)->Arg(1024)->Threads(2);
BENCHMARK(BM_mpmc_dynamic_latency)->Arg(1024)->Threads(2);

} // namespace

BENCHMARK_MAIN();
