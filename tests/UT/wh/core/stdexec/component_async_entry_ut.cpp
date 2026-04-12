#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/callbacks/callbacks.hpp"
#include "wh/core/stdexec/component_async_entry.hpp"

namespace {

using result_t = wh::core::result<int>;
using scheduler_t = wh::testing::helper::manual_scheduler<void>;

struct request_options {
  wh::core::component_options storage{};

  [[nodiscard]] auto component_options() const noexcept
      -> const wh::core::component_options & {
    return storage;
  }
};

struct test_request {
  int value{0};
  request_options options{};
};

struct invalid_request {
  int value{0};
};

struct callback_state {
  int request_value{0};
  std::string phase{};
};

struct callback_probe {
  int make_state_calls{0};
  int start_calls{0};
  int success_calls{0};
  int error_calls{0};
  bool start_sink_has_value{false};
  bool success_sink_has_value{false};
  bool error_sink_has_value{false};
  int last_request_value{0};
  std::string last_phase{};
  std::optional<wh::core::error_code> last_error{};
};

static_assert(
    wh::core::detail::callback_filterable_request<test_request>);
static_assert(
    !wh::core::detail::callback_filterable_request<invalid_request>);

} // namespace

TEST_CASE("component async entry filters callback sink and runs start success error hooks only when enabled",
          "[UT][wh/core/stdexec/component_async_entry.hpp][component_async_entry][condition][branch][boundary]") {
  wh::core::run_context context{};
  context.callbacks.emplace();
  auto sink = wh::callbacks::make_callback_sink(context);

  auto success_probe = std::make_shared<callback_probe>();
  auto success_sender =
      wh::core::detail::component_async_entry<wh::core::resume_mode::unchanged>(
          test_request{.value = 7},
          sink,
          wh::core::detail::resume_passthrough,
          [](test_request &&request) {
            return stdexec::just(result_t{request.value + 5});
          },
          [success_probe](const test_request &request) {
            ++success_probe->make_state_calls;
            success_probe->last_request_value = request.value;
            return callback_state{
                .request_value = request.value,
                .phase = "built",
            };
          },
          [success_probe](const wh::callbacks::callback_sink &start_sink,
                          const callback_state &state) {
            ++success_probe->start_calls;
            success_probe->start_sink_has_value = start_sink.has_value();
            success_probe->last_phase = state.phase;
          },
          [success_probe](const wh::callbacks::callback_sink &success_sink,
                          callback_state &state, const result_t &status) {
            ++success_probe->success_calls;
            success_probe->success_sink_has_value = success_sink.has_value();
            success_probe->last_phase = state.phase;
            if (status.has_value()) {
              success_probe->last_request_value = status.value();
            }
          },
          [success_probe](const wh::callbacks::callback_sink &error_sink,
                          callback_state &state, const result_t &status) {
            ++success_probe->error_calls;
            success_probe->error_sink_has_value = error_sink.has_value();
            success_probe->last_phase = state.phase;
            if (status.has_error()) {
              success_probe->last_error = status.error();
            }
          });

  auto success =
      wh::testing::helper::wait_value_on_test_thread(std::move(success_sender));
  REQUIRE(success.has_value());
  REQUIRE(success.value() == 12);
  REQUIRE(success_probe->make_state_calls == 1);
  REQUIRE(success_probe->start_calls == 1);
  REQUIRE(success_probe->success_calls == 1);
  REQUIRE(success_probe->error_calls == 0);
  REQUIRE(success_probe->start_sink_has_value);
  REQUIRE(success_probe->success_sink_has_value);
  REQUIRE(success_probe->last_request_value == 12);
  REQUIRE(success_probe->last_phase == "built");

  auto error_probe = std::make_shared<callback_probe>();
  auto error_sender =
      wh::core::detail::component_async_entry<wh::core::resume_mode::unchanged>(
          test_request{.value = 9},
          sink,
          wh::core::detail::resume_passthrough,
          [](test_request &&) {
            return stdexec::just(
                result_t::failure(wh::core::errc::invalid_argument));
          },
          [error_probe](const test_request &request) {
            ++error_probe->make_state_calls;
            return callback_state{
                .request_value = request.value,
                .phase = "error-built",
            };
          },
          [error_probe](const wh::callbacks::callback_sink &start_sink,
                        const callback_state &state) {
            ++error_probe->start_calls;
            error_probe->start_sink_has_value = start_sink.has_value();
            error_probe->last_phase = state.phase;
          },
          [error_probe](const wh::callbacks::callback_sink &success_sink,
                        callback_state &state, const result_t &) {
            ++error_probe->success_calls;
            error_probe->success_sink_has_value = success_sink.has_value();
            error_probe->last_phase = state.phase;
          },
          [error_probe](const wh::callbacks::callback_sink &error_sink,
                        callback_state &state, const result_t &status) {
            ++error_probe->error_calls;
            error_probe->error_sink_has_value = error_sink.has_value();
            error_probe->last_phase = state.phase;
            if (status.has_error()) {
              error_probe->last_error = status.error();
            }
          });

  auto error =
      wh::testing::helper::wait_value_on_test_thread(std::move(error_sender));
  REQUIRE(error.has_error());
  REQUIRE(error.error() == wh::core::errc::invalid_argument);
  REQUIRE(error_probe->make_state_calls == 1);
  REQUIRE(error_probe->start_calls == 1);
  REQUIRE(error_probe->success_calls == 0);
  REQUIRE(error_probe->error_calls == 1);
  REQUIRE(error_probe->start_sink_has_value);
  REQUIRE(error_probe->error_sink_has_value);
  REQUIRE(error_probe->last_phase == "error-built");
  REQUIRE(error_probe->last_error == wh::core::errc::invalid_argument);

  auto disabled_probe = std::make_shared<callback_probe>();
  test_request disabled_request{.value = 4};
  disabled_request.options.storage.set_base(
      wh::core::component_common_options{.callbacks_enabled = false});
  auto disabled_sender =
      wh::core::detail::component_async_entry<wh::core::resume_mode::unchanged>(
          std::move(disabled_request),
          sink,
          wh::core::detail::resume_passthrough,
          [](test_request &&request) {
            return stdexec::just(result_t{request.value});
          },
          [disabled_probe](const test_request &) {
            ++disabled_probe->make_state_calls;
            return callback_state{};
          },
          [disabled_probe](const wh::callbacks::callback_sink &,
                           const callback_state &) {
            ++disabled_probe->start_calls;
          },
          [disabled_probe](const wh::callbacks::callback_sink &,
                           callback_state &, const result_t &) {
            ++disabled_probe->success_calls;
          },
          [disabled_probe](const wh::callbacks::callback_sink &,
                           callback_state &, const result_t &) {
            ++disabled_probe->error_calls;
          });

  auto disabled =
      wh::testing::helper::wait_value_on_test_thread(std::move(disabled_sender));
  REQUIRE(disabled.has_value());
  REQUIRE(disabled.value() == 4);
  REQUIRE(disabled_probe->make_state_calls == 0);
  REQUIRE(disabled_probe->start_calls == 0);
  REQUIRE(disabled_probe->success_calls == 0);
  REQUIRE(disabled_probe->error_calls == 0);
}

TEST_CASE("component async entry restore mode hands final completion back to provided scheduler while unchanged stays inline",
          "[UT][wh/core/stdexec/component_async_entry.hpp][component_async_entry][branch][concurrency]") {
  wh::testing::helper::manual_scheduler_state scheduler_state{};
  scheduler_t scheduler{&scheduler_state};

  auto unchanged_sender =
      wh::core::detail::component_async_entry<wh::core::resume_mode::unchanged>(
          test_request{.value = 3},
          wh::callbacks::make_callback_sink(),
          scheduler,
          [](test_request &&request) {
            return stdexec::just(result_t{request.value});
          },
          [](const test_request &) { return callback_state{}; },
          [](const wh::callbacks::callback_sink &, const callback_state &) {},
          [](const wh::callbacks::callback_sink &, callback_state &,
             const result_t &) {},
          [](const wh::callbacks::callback_sink &, callback_state &,
             const result_t &) {});

  auto unchanged =
      wh::testing::helper::wait_value_on_test_thread(std::move(unchanged_sender));
  REQUIRE(unchanged.has_value());
  REQUIRE(unchanged.value() == 3);
  REQUIRE(scheduler_state.pending_count() == 0U);

  auto restore_sender =
      wh::core::detail::component_async_entry<wh::core::resume_mode::restore>(
          test_request{.value = 8},
          wh::callbacks::make_callback_sink(),
          scheduler,
          [](test_request &&request) {
            return stdexec::just(result_t{request.value + 1});
          },
          [](const test_request &) { return callback_state{}; },
          [](const wh::callbacks::callback_sink &, const callback_state &) {},
          [](const wh::callbacks::callback_sink &, callback_state &,
             const result_t &) {},
          [](const wh::callbacks::callback_sink &, callback_state &,
             const result_t &) {});

  wh::testing::helper::sender_capture<result_t> capture{};
  auto operation = stdexec::connect(
      std::move(restore_sender),
      wh::testing::helper::sender_capture_receiver<result_t>{&capture});
  stdexec::start(operation);

  REQUIRE_FALSE(capture.ready.try_acquire());
  REQUIRE(scheduler_state.pending_count() == 1U);
  REQUIRE(scheduler_state.run_one());
  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(capture.value->has_value());
  REQUIRE(capture.value->value() == 9);
}
