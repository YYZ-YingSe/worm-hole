#include <chrono>
#include <stdexcept>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/cursor_reader/detail/waiter.hpp"

TEST_CASE("cursor sync waiter blocks until completion and returns stored result",
          "[UT][wh/core/cursor_reader/detail/waiter.hpp][sync_waiter::wait][concurrency][branch]") {
  wh::core::cursor_reader_detail::sync_waiter<int> waiter{};

  std::thread producer([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds{5});
    waiter.complete(7);
  });

  REQUIRE(waiter.wait() == 7);
  producer.join();
}

TEST_CASE("cursor async waiter state buffers and aliases expose ready bookkeeping types",
          "[UT][wh/core/cursor_reader/detail/"
          "waiter.hpp][async_waiter_base::store_ready][condition][branch][boundary]") {
  using namespace wh::core::cursor_reader_detail;

  async_waiter_base<int> waiter{};
  waiter.store_ready(9);
  REQUIRE(waiter.take_ready() == 9);
  REQUIRE_FALSE(waiter.waiting_registered());
  waiter.mark_waiting_registered();
  REQUIRE(waiter.waiting_registered());
  waiter.clear_waiting_registered();
  REQUIRE_FALSE(waiter.waiting_registered());

  reader_state<int> state{};
  REQUIRE(state.next_sequence == 0U);
  REQUIRE_FALSE(state.closed);
  REQUIRE(state.sync_waiters.empty());
  REQUIRE(state.async_waiters.empty());

  sequence_count_buffer counts{};
  counts.push_back(1U);
  counts.push_back(2U);
  REQUIRE(counts.size() == 2U);

  sync_waiter<int> sync_a{};
  sync_waiter<int> sync_b{};
  sync_ready_buffer<int> ready_sync{};
  ready_sync.push_back(&sync_a);
  ready_sync.push_back(&sync_b);
  REQUIRE(ready_sync.size() == 2U);

  async_ready_list<int> ready_async{};
  constexpr waiter_ops<async_waiter_base<int>> ops{
      [](async_waiter_base<int> *entry) noexcept { entry->store_ready(13); }};
  waiter.ops = &ops;
  ready_async.push_back(&waiter);
  ready_async.complete_all();
  REQUIRE(waiter.take_ready() == 13);
}
