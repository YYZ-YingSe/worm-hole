#include <benchmark/benchmark.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <vector>

#include "wh/sync/sender_notify.hpp"

namespace {

using wh::core::sender_notify;

struct callback_owner {
  std::atomic<std::uint64_t> wake_count{0U};
  std::atomic<bool> woke{false};
};

void notify_callback(void *owner, sender_notify::waiter *) noexcept {
  auto *state = static_cast<callback_owner *>(owner);
  state->wake_count.fetch_add(1U, std::memory_order_relaxed);
  state->woke.store(true, std::memory_order_release);
}

void init_waiter(sender_notify::waiter &waiter,
                 std::atomic<std::uint64_t> *turn_ptr,
                 const std::uint64_t expected_turn,
                 callback_owner *owner) noexcept {
  waiter.turn_ptr = turn_ptr;
  waiter.expected_turn = expected_turn;
  waiter.next = nullptr;
  waiter.prev = nullptr;
  waiter.owner = owner;
  waiter.notify = &notify_callback;
  waiter.channel_hint =
      sender_notify::suggest_channel_index(turn_ptr, expected_turn);
  waiter.channel_index.store(sender_notify::invalid_channel_index,
                             std::memory_order_relaxed);
  waiter.armed.store(false, std::memory_order_relaxed);
  waiter.linked.store(false, std::memory_order_relaxed);
  waiter.notifying.store(false, std::memory_order_relaxed);
}

void set_items_processed(benchmark::State &state,
                         const std::size_t items_per_iteration = 1U) {
  const auto iterations = static_cast<int64_t>(state.iterations());
  const auto items = static_cast<int64_t>(items_per_iteration);
  state.SetItemsProcessed(iterations * items);
}

void BM_sender_notify_notify_no_waiter(benchmark::State &state) {
  sender_notify notify{};
  std::atomic<std::uint64_t> turn{0U};
  std::uint64_t expected_turn = 1U;

  for (auto _ : state) {
    notify.notify(&turn, expected_turn);
    ++expected_turn;
  }

  benchmark::DoNotOptimize(expected_turn);
  set_items_processed(state);
}

void BM_sender_notify_arm_disarm_single_thread(benchmark::State &state) {
  sender_notify notify{};
  std::atomic<std::uint64_t> turn{0U};
  callback_owner owner{};
  sender_notify::waiter waiter{};

  for (auto _ : state) {
    init_waiter(waiter, &turn, 1U, &owner);
    if (notify.arm(waiter)) {
      notify.disarm(waiter);
    }
  }

  benchmark::DoNotOptimize(owner.wake_count.load(std::memory_order_relaxed));
  set_items_processed(state);
}

void BM_sender_notify_arm_notify_single_waiter(benchmark::State &state) {
  sender_notify notify{};
  std::atomic<std::uint64_t> turn{0U};
  callback_owner owner{};
  sender_notify::waiter waiter{};
  std::uint64_t expected_turn = 1U;

  for (auto _ : state) {
    owner.woke.store(false, std::memory_order_relaxed);
    init_waiter(waiter, &turn, expected_turn, &owner);

    if (notify.arm(waiter)) {
      turn.store(expected_turn, std::memory_order_release);
      notify.notify(&turn, expected_turn);
      while (!owner.woke.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
    }

    ++expected_turn;
  }

  benchmark::DoNotOptimize(owner.wake_count.load(std::memory_order_relaxed));
  set_items_processed(state);
}

void BM_sender_notify_notify_fanout(benchmark::State &state) {
  const auto waiter_count = static_cast<std::size_t>(state.range(0));
  sender_notify notify{};
  std::atomic<std::uint64_t> turn{0U};
  std::vector<callback_owner> owners(waiter_count);
  std::vector<sender_notify::waiter> waiters(waiter_count);
  std::uint64_t expected_turn = 1U;
  std::uint64_t woken_total = 0U;

  for (auto _ : state) {
    turn.store(expected_turn - 1U, std::memory_order_relaxed);
    for (std::size_t index = 0U; index < waiter_count; ++index) {
      owners[index].woke.store(false, std::memory_order_relaxed);
      init_waiter(waiters[index], &turn, expected_turn, &owners[index]);
      static_cast<void>(notify.arm(waiters[index]));
    }

    turn.store(expected_turn, std::memory_order_release);
    notify.notify(&turn, expected_turn);

    for (std::size_t index = 0U; index < waiter_count; ++index) {
      if (owners[index].woke.load(std::memory_order_acquire)) {
        ++woken_total;
      }
    }
    ++expected_turn;
  }

  benchmark::DoNotOptimize(woken_total);
  set_items_processed(state, waiter_count);
}

void BM_sender_notify_concurrent_arm_disarm(benchmark::State &state) {
  struct shared_context {
    sender_notify notify{};
    std::array<std::atomic<std::uint64_t>, 256U> turns{};
    shared_context() {
      for (auto &turn : turns) {
        turn.store(0U, std::memory_order_relaxed);
      }
    }
  };

  static shared_context shared{};
  thread_local callback_owner owner{};
  thread_local sender_notify::waiter waiter{};
  std::uint64_t expected_turn = 1U;

  auto &turn = shared.turns[static_cast<std::size_t>(state.thread_index()) &
                            (shared.turns.size() - 1U)];

  for (auto _ : state) {
    init_waiter(waiter, &turn, expected_turn, &owner);
    if (shared.notify.arm(waiter)) {
      shared.notify.disarm(waiter);
    }
    ++expected_turn;
  }

  benchmark::DoNotOptimize(owner.wake_count.load(std::memory_order_relaxed));
  set_items_processed(state);
}

BENCHMARK(BM_sender_notify_notify_no_waiter);
BENCHMARK(BM_sender_notify_arm_disarm_single_thread);
BENCHMARK(BM_sender_notify_arm_notify_single_waiter);
BENCHMARK(BM_sender_notify_notify_fanout)->Arg(1)->Arg(8)->Arg(64)->Arg(256);
BENCHMARK(BM_sender_notify_concurrent_arm_disarm)->ThreadRange(1, 64);

} // namespace

BENCHMARK_MAIN();
