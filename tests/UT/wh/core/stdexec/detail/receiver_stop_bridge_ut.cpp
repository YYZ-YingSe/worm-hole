#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <exception>
#include <stdexcept>

#include <stdexec/execution.hpp>

#include "helper/sender_capture.hpp"
#include "wh/core/stdexec/detail/receiver_stop_bridge.hpp"

namespace {

struct get_marker_t {
  [[nodiscard]] auto operator()(const auto &env) const
      noexcept(noexcept(env.marker)) -> decltype(env.marker) {
    return env.marker;
  }
};

inline constexpr get_marker_t get_marker{};

struct tagged_env {
  int marker{0};
  stdexec::inplace_stop_token stop_token{};

  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
      -> stdexec::inplace_stop_token {
    return stop_token;
  }
};

struct stop_observer {
  std::atomic<bool> *fired{nullptr};

  auto operator()() const noexcept -> void {
    fired->store(true, std::memory_order_release);
  }
};

} // namespace

TEST_CASE("receiver_stop_bridge forwards outer stop requests into the inner stop env",
          "[UT][wh/core/stdexec/detail/receiver_stop_bridge.hpp][receiver_stop_bridge::bind_outer_stop][branch][concurrency]") {
  stdexec::inplace_stop_source outer_stop{};
  wh::testing::helper::sender_capture<int> capture{};
  using receiver_t =
      wh::testing::helper::sender_capture_receiver<int, tagged_env>;

  wh::core::detail::receiver_stop_bridge<receiver_t> bridge{
      receiver_t{&capture, tagged_env{.marker = 17, .stop_token = outer_stop.get_token()}}};

  REQUIRE(bridge.bind_outer_stop());
  REQUIRE(bridge.env().query(get_marker) == 17);

  std::atomic<bool> inner_stopped{false};
  auto inner_token = stdexec::get_stop_token(bridge.env());
  stdexec::stop_callback_for_t<stdexec::inplace_stop_token, stop_observer>
      callback{inner_token, stop_observer{&inner_stopped}};

  outer_stop.request_stop();

  REQUIRE(inner_token.stop_requested());
  REQUIRE(inner_stopped.load(std::memory_order_acquire));

  bridge.set_value(7);
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal ==
          wh::testing::helper::sender_terminal_kind::stopped);
}

TEST_CASE("receiver_stop_bridge completes at most once and respects pre-stopped outer tokens",
          "[UT][wh/core/stdexec/detail/receiver_stop_bridge.hpp][receiver_stop_bridge::set_value][boundary][branch]") {
  {
    wh::testing::helper::sender_capture<int> capture{};
    using receiver_t =
        wh::testing::helper::sender_capture_receiver<int, tagged_env>;
    wh::core::detail::receiver_stop_bridge<receiver_t> bridge{
        receiver_t{&capture, tagged_env{.marker = 3}}};

    REQUIRE(bridge.bind_outer_stop());
    bridge.set_value(11);
    bridge.set_error(std::make_exception_ptr(std::runtime_error{"late"}));

    REQUIRE(capture.ready.try_acquire());
    REQUIRE(capture.terminal ==
            wh::testing::helper::sender_terminal_kind::value);
    REQUIRE(capture.value.has_value());
    REQUIRE(*capture.value == 11);
  }

  {
    stdexec::inplace_stop_source outer_stop{};
    outer_stop.request_stop();

    wh::testing::helper::sender_capture<int> capture{};
    using receiver_t =
        wh::testing::helper::sender_capture_receiver<int, tagged_env>;
    wh::core::detail::receiver_stop_bridge<receiver_t> bridge{
        receiver_t{&capture,
                   tagged_env{.marker = 9, .stop_token = outer_stop.get_token()}}};

    REQUIRE_FALSE(bridge.bind_outer_stop());

    bridge.set_value(13);
    REQUIRE(capture.ready.try_acquire());
    REQUIRE(capture.terminal ==
            wh::testing::helper::sender_terminal_kind::stopped);
  }
}
