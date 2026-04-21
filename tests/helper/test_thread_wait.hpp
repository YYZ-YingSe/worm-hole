// Defines synchronous sender bridge helpers for top-level test threads only.
// Never call these helpers from scheduler worker threads such as
// `exec::static_thread_pool` workers. For in-flight async observation inside
// scheduler-driven tests, use `sender_capture`, `manual_scheduler`, or an
// explicit receiver state instead.
#pragma once

#include <tuple>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

namespace wh::testing::helper {

template <typename sender_t> [[nodiscard]] auto sync_wait_on_test_thread(sender_t &&sender) {
  return stdexec::sync_wait(std::forward<sender_t>(sender));
}

template <typename sender_t> [[nodiscard]] auto wait_value_on_test_thread(sender_t &&sender) {
  auto waited = sync_wait_on_test_thread(std::forward<sender_t>(sender));
  REQUIRE(waited.has_value());
  return std::get<0>(std::move(waited).value());
}

} // namespace wh::testing::helper
