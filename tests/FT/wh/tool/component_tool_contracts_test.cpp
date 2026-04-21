#include <array>
#include <atomic>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/component_contract_support.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/tool/catalog.hpp"
#include "wh/tool/interrupt.hpp"
#include "wh/tool/tool.hpp"

namespace {

using wh::testing::helper::missing_tool_path;
using wh::testing::helper::register_test_callbacks;
using wh::testing::helper::sender_tool_impl;
using wh::testing::helper::sender_tool_invoke_impl;
using wh::testing::helper::sender_tool_stream_impl;
using wh::testing::helper::sync_tool_impl;
using wh::testing::helper::sync_tool_invoke_impl;
using wh::testing::helper::sync_tool_stream_impl;
using wh::testing::helper::take_try_chunk;
using wh::testing::helper::tool_async_invoke_available;

} // namespace

TEST_CASE("tool catalog cache supports handshake cache refresh and schema loading",
          "[core][tool][functional]") {
  std::size_t handshake_calls = 0U;
  std::size_t fetch_calls = 0U;
  wh::tool::tool_catalog_cache cache{wh::tool::tool_catalog_source{
      .handshake = [&handshake_calls]() -> wh::core::result<void> {
        ++handshake_calls;
        return {};
      },
      .fetch_catalog =
          [&fetch_calls]() -> wh::core::result<std::vector<wh::schema::tool_schema_definition>> {
        ++fetch_calls;
        wh::schema::tool_schema_definition schema{};
        schema.name = "catalog_tool";
        return std::vector<wh::schema::tool_schema_definition>{schema};
      },
  }};

  auto first = cache.load();
  REQUIRE(first.has_value());
  REQUIRE(first.value().size() == 1U);
  REQUIRE(handshake_calls == 1U);
  REQUIRE(fetch_calls == 1U);

  auto second = cache.load();
  REQUIRE(second.has_value());
  REQUIRE(handshake_calls == 1U);
  REQUIRE(fetch_calls == 1U);

  auto refreshed = cache.load(wh::tool::tool_catalog_load_options{.refresh = true});
  REQUIRE(refreshed.has_value());
  REQUIRE(handshake_calls == 2U);
  REQUIRE(fetch_calls == 2U);
  REQUIRE(refreshed.value().size() == 1U);
  REQUIRE(refreshed.value().front().name == "catalog_tool");

  auto bound = wh::tool::tool{
      refreshed.value().front(),
      sync_tool_invoke_impl{[](const std::string_view input, const wh::tool::tool_options &)
                                -> wh::core::result<std::string> { return std::string{input}; }}};
  wh::core::run_context callback_context{};
  auto invoke_result = bound.invoke(wh::tool::tool_request{"bound payload", {}}, callback_context);
  REQUIRE(invoke_result.has_value());
  REQUIRE(invoke_result.value() == "bound payload");
}

TEST_CASE("tool callbacks include lifecycle payload and schema error context",
          "[core][tool][functional]") {
  wh::schema::tool_parameter_schema count{};
  count.name = "count";
  count.type = wh::schema::tool_parameter_type::integer;
  count.required = true;

  wh::schema::tool_schema_definition info{};
  info.name = "validator";
  info.parameters.push_back(count);

  wh::tool::tool component{
      info, sync_tool_impl{
                sync_tool_invoke_impl{
                    [](const std::string_view input, const wh::tool::tool_options &)
                        -> wh::core::result<std::string> { return std::string{input}; }},
                sync_tool_stream_impl{[](const std::string_view, const wh::tool::tool_options &)
                                          -> wh::core::result<wh::tool::tool_output_stream_reader> {
                  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(4U);
                  auto write_status = writer.try_write("chunk");
                  if (write_status.has_error()) {
                    return wh::core::result<wh::tool::tool_output_stream_reader>::failure(
                        write_status.error());
                  }
                  auto close_status = writer.close();
                  if (close_status.has_error()) {
                    return wh::core::result<wh::tool::tool_output_stream_reader>::failure(
                        close_status.error());
                  }
                  return wh::tool::tool_output_stream_reader{std::move(reader)};
                }}}};

  std::atomic<int> started{0};
  std::atomic<int> ended{0};
  std::atomic<int> errored{0};
  std::atomic<int> start_payload_hits{0};
  std::atomic<int> end_payload_hits{0};
  std::string error_context{};

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context), [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage, const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::tool::tool_callback_event>();
        REQUIRE(typed != nullptr);
        REQUIRE(typed->tool_name == "validator");
        if (stage == wh::core::callback_stage::start) {
          started.fetch_add(1, std::memory_order_release);
          start_payload_hits.fetch_add(1, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          ended.fetch_add(1, std::memory_order_release);
          end_payload_hits.fetch_add(1, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          error_context = typed->error_context;
          errored.fetch_add(1, std::memory_order_release);
        }
      },
      "tool-events");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  wh::tool::tool_options options{};
  auto invalid =
      component.invoke(wh::tool::tool_request{R"({"count":"bad"})", options}, callback_context);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
  REQUIRE(error_context == "$.count");

  auto valid_invoke =
      component.invoke(wh::tool::tool_request{R"({"count":1})", options}, callback_context);
  REQUIRE(valid_invoke.has_value());
  REQUIRE(valid_invoke.value() == R"({"count":1})");

  auto valid_stream =
      component.stream(wh::tool::tool_request{R"({"count":1})", options}, callback_context);
  REQUIRE(valid_stream.has_value());

  REQUIRE(started.load(std::memory_order_acquire) == 3);
  REQUIRE(ended.load(std::memory_order_acquire) == 2);
  REQUIRE(errored.load(std::memory_order_acquire) == 1);
  REQUIRE(start_payload_hits.load(std::memory_order_acquire) == 3);
  REQUIRE(end_payload_hits.load(std::memory_order_acquire) == 2);
}

TEST_CASE("tool failure policy applies retry skip and timeout label", "[core][tool][functional]") {
  wh::schema::tool_schema_definition info{};
  info.name = "policy_tool";

  std::atomic<int> invoke_attempts{0};
  wh::tool::tool retry_tool{
      info, sync_tool_impl{sync_tool_invoke_impl{
                [&](const std::string_view,
                    const wh::tool::tool_options &) -> wh::core::result<std::string> {
                  const int current = invoke_attempts.fetch_add(1, std::memory_order_relaxed);
                  if (current < 2) {
                    return wh::core::result<std::string>::failure(wh::core::errc::network_error);
                  }
                  return std::string{"ok"};
                }}}};

  wh::tool::tool_options retry_options{};
  wh::tool::tool_common_options retry_common{};
  retry_common.failure_policy = wh::tool::tool_failure_policy::retry;
  retry_common.max_retries = 3U;
  retry_options.set_base(retry_common);
  wh::core::run_context retry_context{};
  auto retry_status = retry_tool.invoke(wh::tool::tool_request{"{}", retry_options}, retry_context);
  REQUIRE(retry_status.has_value());
  REQUIRE(retry_status.value() == "ok");
  REQUIRE(invoke_attempts.load(std::memory_order_relaxed) == 3);

  std::atomic<bool> skipped_end{false};
  std::string skip_error_context{};
  wh::tool::tool skip_tool{
      info, sync_tool_impl{
                sync_tool_invoke_impl{[](const std::string_view, const wh::tool::tool_options &)
                                          -> wh::core::result<std::string> {
                  return wh::core::result<std::string>::failure(wh::core::errc::timeout);
                }},
                sync_tool_stream_impl{[](const std::string_view, const wh::tool::tool_options &)
                                          -> wh::core::result<wh::tool::tool_output_stream_reader> {
                  return wh::core::result<wh::tool::tool_output_stream_reader>::failure(
                      wh::core::errc::timeout);
                }}}};
  wh::tool::tool_options skip_options{};
  wh::tool::tool_common_options skip_common{};
  skip_common.failure_policy = wh::tool::tool_failure_policy::skip;
  skip_common.timeout_label = "budgetA";
  skip_options.set_base(skip_common);

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context), [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage, const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::tool::tool_callback_event>();
        if (typed == nullptr) {
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          skipped_end.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          skip_error_context = typed->error_context;
        }
      },
      "tool-skip-events");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  auto skipped = skip_tool.invoke(wh::tool::tool_request{"{}", skip_options}, callback_context);
  REQUIRE(skipped.has_value());
  REQUIRE(skipped.value().empty());
  REQUIRE(skipped_end.load(std::memory_order_acquire));
  REQUIRE(skip_error_context == "budgetA");

  auto skipped_stream =
      skip_tool.stream(wh::tool::tool_request{"{}", skip_options}, callback_context);
  REQUIRE(skipped_stream.has_value());
  auto reader = std::move(skipped_stream).value();
  auto first = take_try_chunk(reader);
  REQUIRE(first.has_value());
  REQUIRE(first.value().eof);
}

TEST_CASE("tool async path preserves validation retry and skip semantics",
          "[core][tool][functional]") {
  wh::schema::tool_schema_definition validator_info{};
  validator_info.name = "async-validator";
  validator_info.parameters = {wh::schema::tool_parameter_schema{
      .name = "count", .type = wh::schema::tool_parameter_type::integer, .required = true}};

  wh::tool::tool validator_tool{
      validator_info,
      sender_tool_impl{
          sender_tool_invoke_impl{
              [](std::string_view input, const wh::tool::tool_options &)
                  -> wh::core::result<std::string> { return std::string{input}; }},
          sender_tool_stream_impl{[](std::string_view input, const wh::tool::tool_options &)
                                      -> wh::core::result<wh::tool::tool_output_stream_reader> {
            auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(1U);
            auto wrote = writer.try_write(std::string{input});
            if (wrote.has_error()) {
              return wh::core::result<wh::tool::tool_output_stream_reader>::failure(wrote.error());
            }
            auto closed = writer.close();
            if (closed.has_error()) {
              return wh::core::result<wh::tool::tool_output_stream_reader>::failure(closed.error());
            }
            return wh::tool::tool_output_stream_reader{std::move(reader)};
          }}}};

  struct sync_only_tool_async_guard {
    [[nodiscard]] auto invoke(const wh::tool::tool_request &) const
        -> wh::core::result<std::string> {
      return std::string{};
    }
  };

  static_assert(!tool_async_invoke_available<wh::tool::tool<sync_only_tool_async_guard>>);

  std::atomic<int> validation_started{0};
  std::atomic<int> validation_ended{0};
  std::atomic<int> validation_errored{0};
  std::string validation_error_context{};
  wh::core::run_context validation_context{};
  validation_context.callbacks.emplace();
  auto registered_validation = register_test_callbacks(
      std::move(validation_context), [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage, const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::tool::tool_callback_event>();
        REQUIRE(typed != nullptr);
        if (stage == wh::core::callback_stage::start) {
          validation_started.fetch_add(1, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          validation_ended.fetch_add(1, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          validation_error_context = typed->error_context;
          validation_errored.fetch_add(1, std::memory_order_release);
        }
      },
      "tool-async-validation");
  REQUIRE(registered_validation.has_value());
  validation_context = std::move(registered_validation).value();

  auto invalid_async = wh::testing::helper::wait_value_on_test_thread(validator_tool.async_invoke(
      wh::tool::tool_request{R"({"count":"bad"})", wh::tool::tool_options{}}, validation_context));
  REQUIRE(invalid_async.has_error());
  REQUIRE(invalid_async.error() == wh::core::errc::invalid_argument);
  REQUIRE(validation_started.load(std::memory_order_acquire) == 1);
  REQUIRE(validation_ended.load(std::memory_order_acquire) == 0);
  REQUIRE(validation_errored.load(std::memory_order_acquire) == 1);
  REQUIRE(validation_error_context == "$.count");

  wh::schema::tool_schema_definition policy_info{};
  policy_info.name = "async-policy";

  std::atomic<int> retry_attempts{0};
  wh::tool::tool retry_tool{
      policy_info,
      sender_tool_impl{sender_tool_invoke_impl{
          [&](const std::string_view,
              const wh::tool::tool_options &) -> wh::core::result<std::string> {
            const auto current = retry_attempts.fetch_add(1, std::memory_order_relaxed);
            if (current < 2) {
              return wh::core::result<std::string>::failure(wh::core::errc::network_error);
            }
            return std::string{"ok"};
          }}}};

  wh::tool::tool_options retry_options{};
  wh::tool::tool_common_options retry_common{};
  retry_common.failure_policy = wh::tool::tool_failure_policy::retry;
  retry_common.max_retries = 3U;
  retry_options.set_base(retry_common);

  std::atomic<int> retry_errors{0};
  std::atomic<int> retry_ended{0};
  wh::core::run_context retry_context{};
  retry_context.callbacks.emplace();
  auto registered_retry = register_test_callbacks(
      std::move(retry_context), [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage, const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::tool::tool_callback_event>();
        REQUIRE(typed != nullptr);
        if (stage == wh::core::callback_stage::error) {
          retry_errors.fetch_add(1, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          retry_ended.fetch_add(1, std::memory_order_release);
        }
      },
      "tool-async-retry");
  REQUIRE(registered_retry.has_value());
  retry_context = std::move(registered_retry).value();

  auto retry_result = wh::testing::helper::wait_value_on_test_thread(
      retry_tool.async_invoke(wh::tool::tool_request{"{}", retry_options}, retry_context));
  REQUIRE(retry_result.has_value());
  REQUIRE(retry_result.value() == "ok");
  REQUIRE(retry_attempts.load(std::memory_order_relaxed) == 3);
  REQUIRE(retry_errors.load(std::memory_order_acquire) == 2);
  REQUIRE(retry_ended.load(std::memory_order_acquire) == 1);

  wh::tool::tool skip_tool{
      policy_info,
      sender_tool_impl{
          sender_tool_invoke_impl{[](const std::string_view, const wh::tool::tool_options &)
                                      -> wh::core::result<std::string> {
            return wh::core::result<std::string>::failure(wh::core::errc::timeout);
          }},
          sender_tool_stream_impl{[](const std::string_view, const wh::tool::tool_options &)
                                      -> wh::core::result<wh::tool::tool_output_stream_reader> {
            return wh::core::result<wh::tool::tool_output_stream_reader>::failure(
                wh::core::errc::timeout);
          }}}};

  wh::tool::tool_options skip_options{};
  wh::tool::tool_common_options skip_common{};
  skip_common.failure_policy = wh::tool::tool_failure_policy::skip;
  skip_common.timeout_label = "budgetA";
  skip_options.set_base(skip_common);

  std::atomic<int> skip_ended{0};
  std::atomic<int> skip_errored{0};
  std::string skip_error_context{};
  wh::core::run_context skip_context{};
  skip_context.callbacks.emplace();
  auto registered_skip = register_test_callbacks(
      std::move(skip_context), [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage, const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::tool::tool_callback_event>();
        REQUIRE(typed != nullptr);
        if (stage == wh::core::callback_stage::error) {
          skip_error_context = typed->error_context;
          skip_errored.fetch_add(1, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          skip_ended.fetch_add(1, std::memory_order_release);
        }
      },
      "tool-async-skip");
  REQUIRE(registered_skip.has_value());
  skip_context = std::move(registered_skip).value();

  auto skipped_invoke = wh::testing::helper::wait_value_on_test_thread(
      skip_tool.async_invoke(wh::tool::tool_request{"{}", skip_options}, skip_context));
  REQUIRE(skipped_invoke.has_value());
  REQUIRE(skipped_invoke.value().empty());

  auto skipped_stream = wh::testing::helper::wait_value_on_test_thread(
      skip_tool.async_stream(wh::tool::tool_request{"{}", skip_options}, skip_context));
  REQUIRE(skipped_stream.has_value());
  auto skipped_reader = std::move(skipped_stream).value();
  auto skipped_chunk = take_try_chunk(skipped_reader);
  REQUIRE(skipped_chunk.has_value());
  REQUIRE(skipped_chunk.value().eof);
  REQUIRE(skip_ended.load(std::memory_order_acquire) == 2);
  REQUIRE(skip_errored.load(std::memory_order_acquire) == 2);
  REQUIRE(skip_error_context == "budgetA");
}

TEST_CASE("tool wrapper can restore caller completion when configured",
          "[core][tool][functional]") {
  struct restoring_tool_impl {
    exec::static_thread_pool::scheduler worker_scheduler;
    std::shared_ptr<std::thread::id> worker_thread{};

    [[nodiscard]] auto invoke_sender(const wh::tool::tool_request &) const {
      return stdexec::starts_on(worker_scheduler,
                                stdexec::just() | stdexec::then([worker_thread = worker_thread]() {
                                  *worker_thread = std::this_thread::get_id();
                                  return wh::core::result<std::string>{std::string{"ok"}};
                                }));
    }
  };

  exec::static_thread_pool caller_pool{1U};
  exec::static_thread_pool worker_pool{1U};
  auto worker_thread = std::make_shared<std::thread::id>();

  wh::schema::tool_schema_definition schema{};
  schema.name = "restore-tool";
  wh::tool::tool<restoring_tool_impl, wh::core::resume_mode::restore> component{
      schema, restoring_tool_impl{worker_pool.get_scheduler(), worker_thread}};

  std::thread::id caller_scheduler_thread{};
  auto caller_ready =
      stdexec::sync_wait(stdexec::schedule(caller_pool.get_scheduler()) | stdexec::then([&]() {
                           caller_scheduler_thread = std::this_thread::get_id();
                           return 0;
                         }));
  REQUIRE(caller_ready.has_value());

  std::thread::id worker_scheduler_thread{};
  auto worker_ready =
      stdexec::sync_wait(stdexec::schedule(worker_pool.get_scheduler()) | stdexec::then([&]() {
                           worker_scheduler_thread = std::this_thread::get_id();
                           return 0;
                         }));
  REQUIRE(worker_ready.has_value());

  wh::core::run_context context{};
  std::thread::id completion_thread{};
  auto waited = stdexec::sync_wait(
      stdexec::starts_on(caller_pool.get_scheduler(),
                         component.async_invoke(wh::tool::tool_request{"{}", {}}, context)) |
      stdexec::then([&](wh::core::result<std::string> status) {
        completion_thread = std::this_thread::get_id();
        return status;
      }));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(waited).value());
  REQUIRE(status.has_value());
  REQUIRE(status.value() == "ok");
  REQUIRE(*worker_thread == worker_scheduler_thread);
  REQUIRE(worker_scheduler_thread != caller_scheduler_thread);
  REQUIRE(completion_thread == caller_scheduler_thread);
}

TEST_CASE("tool interrupt utilities support resume target and root cause",
          "[core][tool][functional]") {
  wh::tool::tool_interrupt first{};
  first.interrupt_id = "interrupt-a";
  first.location = wh::core::address{}.append("graph").append("node");
  first.payload = std::string{"state-a"};

  wh::core::resume_state resume{};
  REQUIRE_FALSE(wh::tool::is_resume_target(first, resume));
  auto injected = wh::tool::inject_resume_data(first, resume);
  REQUIRE(injected.has_value());
  REQUIRE(wh::tool::is_resume_target(first, resume));
  REQUIRE(wh::tool::is_resume_target(first, resume, true));

  wh::tool::tool_interrupt second = first;
  second.interrupt_id = "interrupt-b";
  std::array<wh::tool::tool_interrupt, 2U> interrupts{first, second};
  auto aggregated = wh::tool::aggregate_interrupts(
      interrupts, wh::core::make_error(wh::core::errc::network_error));
  REQUIRE(aggregated.has_value());
  REQUIRE(aggregated.value().interrupts.size() == 2U);
  REQUIRE(aggregated.value().root_cause.has_value());
  REQUIRE(*aggregated.value().root_cause == wh::core::errc::network_error);

  std::array<wh::core::error_code, 2U> causes{wh::core::make_error(wh::core::errc::ok),
                                              wh::core::make_error(wh::core::errc::timeout)};
  auto root_cause = wh::tool::infer_root_cause(causes);
  REQUIRE(root_cause.has_value());
  REQUIRE(*root_cause == wh::core::errc::timeout);
}
