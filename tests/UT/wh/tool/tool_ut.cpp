#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "helper/sender_env.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/core/compiler.hpp"
#include "wh/tool/tool.hpp"

namespace {

template <typename reader_t>
[[nodiscard]] auto take_try_chunk(reader_t &reader) ->
    typename std::remove_cvref_t<reader_t>::chunk_result_type {
  auto next = reader.try_read();
  REQUIRE_FALSE(std::holds_alternative<wh::schema::stream::stream_signal>(next));
  return std::move(std::get<typename std::remove_cvref_t<reader_t>::chunk_result_type>(next));
}

template <typename fn_t> struct sync_tool_invoke_impl {
  fn_t fn;
  [[nodiscard]] auto invoke(const wh::tool::tool_request &request) const
      -> decltype(std::invoke(fn, std::declval<std::string_view>(),
                              std::declval<const wh::tool::tool_options &>())) {
    return std::invoke(fn, std::string_view{request.input_json}, request.options);
  }
};

template <typename fn_t> struct sync_tool_stream_impl {
  fn_t fn;
  [[nodiscard]] auto stream(const wh::tool::tool_request &request) const
      -> decltype(std::invoke(fn, std::declval<std::string_view>(),
                              std::declval<const wh::tool::tool_options &>())) {
    return std::invoke(fn, std::string_view{request.input_json}, request.options);
  }
};

struct missing_tool_path final {};

template <typename invoke_impl_t = missing_tool_path, typename stream_impl_t = missing_tool_path>
struct sync_tool_impl {
  wh_no_unique_address invoke_impl_t invoke_impl{};
  wh_no_unique_address stream_impl_t stream_impl{};

  [[nodiscard]] auto invoke(const wh::tool::tool_request &request) const -> decltype(auto)
    requires(!std::same_as<invoke_impl_t, missing_tool_path>)
  {
    return invoke_impl.invoke(request);
  }

  [[nodiscard]] auto stream(const wh::tool::tool_request &request) const -> decltype(auto)
    requires(!std::same_as<stream_impl_t, missing_tool_path>)
  {
    return stream_impl.stream(request);
  }
};

template <typename fn_t> struct sender_tool_invoke_impl {
  fn_t fn;
  [[nodiscard]] auto invoke_sender(wh::tool::tool_request request) const {
    return stdexec::just(std::invoke(fn, std::string_view{request.input_json}, request.options));
  }
};

template <typename fn_t> struct sender_tool_stream_impl {
  fn_t fn;
  [[nodiscard]] auto stream_sender(wh::tool::tool_request request) const {
    return stdexec::just(std::invoke(fn, std::string_view{request.input_json}, request.options));
  }
};

template <typename invoke_impl_t = missing_tool_path, typename stream_impl_t = missing_tool_path>
struct sender_tool_impl {
  wh_no_unique_address invoke_impl_t invoke_impl{};
  wh_no_unique_address stream_impl_t stream_impl{};

  [[nodiscard]] auto invoke_sender(wh::tool::tool_request request) const -> decltype(auto)
    requires(!std::same_as<invoke_impl_t, missing_tool_path>)
  {
    return invoke_impl.invoke_sender(std::move(request));
  }

  [[nodiscard]] auto stream_sender(wh::tool::tool_request request) const -> decltype(auto)
    requires(!std::same_as<stream_impl_t, missing_tool_path>)
  {
    return stream_impl.stream_sender(std::move(request));
  }
};

template <typename timing_checker_t, typename callback_t>
[[nodiscard]] auto register_callbacks(wh::core::run_context &&context,
                                      timing_checker_t &&timing_checker, callback_t &&callback)
    -> wh::core::result<wh::core::run_context> {
  wh::core::stage_view_callback stage_callback{std::forward<callback_t>(callback)};
  wh::core::stage_callbacks callbacks{};
  callbacks.on_start = stage_callback;
  callbacks.on_end = stage_callback;
  callbacks.on_error = stage_callback;
  callbacks.on_stream_start = stage_callback;
  callbacks.on_stream_end = std::move(stage_callback);
  return wh::core::register_local_callbacks(std::move(context),
                                            std::forward<timing_checker_t>(timing_checker),
                                            std::move(callbacks), std::string{});
}

inline auto update_max_depth(std::atomic<int> &max_depth, const int current) noexcept -> void {
  auto previous = max_depth.load(std::memory_order_acquire);
  while (previous < current &&
         !max_depth.compare_exchange_weak(previous, current, std::memory_order_acq_rel,
                                          std::memory_order_acquire)) {
  }
}

struct inline_retry_sender {
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(wh::tool::tool_invoke_result)>;

  std::atomic<int> *attempts{nullptr};
  std::atomic<int> *active_depth{nullptr};
  std::atomic<int> *max_depth{nullptr};
  int fail_before_success{0};

  template <typename receiver_t> struct operation {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;
    std::atomic<int> *attempts{nullptr};
    std::atomic<int> *active_depth{nullptr};
    std::atomic<int> *max_depth{nullptr};
    int fail_before_success{0};

    auto start() & noexcept -> void {
      struct depth_guard {
        std::atomic<int> *depth{nullptr};
        ~depth_guard() { depth->fetch_sub(1, std::memory_order_acq_rel); }
      };

      const auto current = active_depth->fetch_add(1, std::memory_order_acq_rel) + 1;
      depth_guard guard{active_depth};
      update_max_depth(*max_depth, current);

      const auto attempt = attempts->fetch_add(1, std::memory_order_relaxed);
      if (attempt < fail_before_success) {
        stdexec::set_value(std::move(receiver),
                           wh::tool::tool_invoke_result::failure(wh::core::errc::network_error));
        return;
      }
      stdexec::set_value(std::move(receiver), wh::tool::tool_invoke_result{std::string{"ok"}});
    }
  };

  template <typename receiver_t> auto connect(receiver_t receiver) && -> operation<receiver_t> {
    return operation<receiver_t>{std::move(receiver), attempts, active_depth, max_depth,
                                 fail_before_success};
  }
};

struct inline_retry_impl {
  std::atomic<int> *attempts{nullptr};
  std::atomic<int> *active_depth{nullptr};
  std::atomic<int> *max_depth{nullptr};
  int fail_before_success{0};

  [[nodiscard]] auto invoke_sender(wh::tool::tool_request) const {
    return inline_retry_sender{
        .attempts = attempts,
        .active_depth = active_depth,
        .max_depth = max_depth,
        .fail_before_success = fail_before_success,
    };
  }
};

struct attempt_lifetime_probe_state {
  std::atomic<bool> child_destroyed{false};
  std::atomic<bool> child_destroyed_before_start_return{false};
};

struct attempt_lifetime_probe_sender {
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(wh::tool::tool_invoke_result)>;

  template <typename receiver_t> struct operation {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;
    std::shared_ptr<attempt_lifetime_probe_state> state;

    ~operation() { state->child_destroyed.store(true, std::memory_order_release); }

    auto start() & noexcept -> void {
      auto keep_alive = state;
      stdexec::set_value(std::move(receiver), wh::tool::tool_invoke_result{std::string{"ok"}});
      if (keep_alive->child_destroyed.load(std::memory_order_acquire)) {
        keep_alive->child_destroyed_before_start_return.store(true, std::memory_order_release);
      }
    }
  };

  template <typename receiver_t> auto connect(receiver_t receiver) const -> operation<receiver_t> {
    return operation<receiver_t>{std::move(receiver), state};
  }

  std::shared_ptr<attempt_lifetime_probe_state> state;
};

struct attempt_lifetime_probe_impl {
  std::shared_ptr<attempt_lifetime_probe_state> state{
      std::make_shared<attempt_lifetime_probe_state>()};

  [[nodiscard]] auto invoke_sender(wh::tool::tool_request) const {
    return attempt_lifetime_probe_sender{state};
  }
};

} // namespace

TEST_CASE("tool detail helpers validate schema and resolve options",
          "[UT][wh/tool/tool.hpp][validate_tool_input_schema][branch][boundary]") {
  REQUIRE(wh::tool::detail::append_path_segment("$", "field") == "$.field");
  REQUIRE(wh::tool::detail::append_path_index("$.items", 2) == "$.items[2]");

  wh::schema::tool_parameter_schema parameter{};
  parameter.name = "count";
  parameter.type = wh::schema::tool_parameter_type::integer;
  parameter.required = true;
  std::string error_path{};

  auto valid = wh::tool::detail::validate_tool_input_schema(
      R"({"count":2})", std::span<const wh::schema::tool_parameter_schema>{&parameter, 1},
      error_path);
  REQUIRE(valid.has_value());

  auto invalid = wh::tool::detail::validate_tool_input_schema(
      R"({"count":"bad"})", std::span<const wh::schema::tool_parameter_schema>{&parameter, 1},
      error_path);
  REQUIRE(invalid.has_error());
  REQUIRE(error_path == "$.count");

  wh::tool::tool_options base{};
  wh::tool::tool_common_options common{};
  common.failure_policy = wh::tool::tool_failure_policy::retry;
  common.max_retries = 2U;
  base.set_base(common);
  wh::tool::tool_options override{};
  override.set_call_override(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::skip, .timeout_label = "budget"});
  auto merged = wh::tool::detail::merge_options(base, override);
  REQUIRE(merged.resolve_view().failure_policy == wh::tool::tool_failure_policy::skip);
  REQUIRE(merged.resolve_view().max_retries == 0U);
  REQUIRE(merged.resolve_view().timeout_label == "budget");

  wh::tool::detail::callback_state state{};
  auto resolved_options = merged.resolve();
  auto resolved = wh::tool::detail::resolve_options_view(resolved_options);
  wh::tool::detail::mark_error(state, wh::core::errc::timeout, resolved);
  REQUIRE(state.event.error_context == "budget");
  REQUIRE_FALSE(state.event.interrupted);

  auto skipped = wh::tool::detail::make_skipped_stream();
  REQUIRE(skipped.has_value());
  auto reader = std::move(skipped).value();
  auto first = take_try_chunk(reader);
  REQUIRE(first.has_value());
  REQUIRE(first.value().eof);
}

TEST_CASE("tool wrapper exposes sync invoke stream retry and skip semantics",
          "[UT][wh/tool/tool.hpp][tool::invoke][branch][boundary]") {
  wh::schema::tool_schema_definition schema{};
  schema.name = "policy_tool";
  schema.description = "policy";

  std::atomic<int> attempts{0};
  wh::tool::tool retry_tool{
      schema, sync_tool_impl{sync_tool_invoke_impl{
                  [&](const std::string_view,
                      const wh::tool::tool_options &) -> wh::tool::tool_invoke_result {
                    const auto current = attempts.fetch_add(1, std::memory_order_relaxed);
                    if (current < 2) {
                      return wh::tool::tool_invoke_result::failure(wh::core::errc::network_error);
                    }
                    return std::string{"ok"};
                  }}}};
  REQUIRE(retry_tool.descriptor().kind == wh::core::component_kind::tool);
  REQUIRE(retry_tool.schema().name == "policy_tool");
  REQUIRE(retry_tool.default_options().resolve_view().max_retries == 0U);

  wh::tool::tool_options retry_options{};
  retry_options.set_base(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::retry, .max_retries = 3U});
  wh::core::run_context context{};
  auto retried = retry_tool.invoke(wh::tool::tool_request{"{}", retry_options}, context);
  REQUIRE(retried.has_value());
  REQUIRE(retried.value() == "ok");
  REQUIRE(attempts.load(std::memory_order_relaxed) == 3);

  std::string error_context{};
  std::atomic<int> ended{0};
  wh::tool::tool skip_tool{
      schema,
      sync_tool_impl{
          sync_tool_invoke_impl{[](const std::string_view,
                                   const wh::tool::tool_options &) -> wh::tool::tool_invoke_result {
            return wh::tool::tool_invoke_result::failure(wh::core::errc::timeout);
          }},
          sync_tool_stream_impl{[](const std::string_view, const wh::tool::tool_options &)
                                    -> wh::tool::tool_output_stream_result {
            return wh::tool::tool_output_stream_result::failure(wh::core::errc::timeout);
          }}}};
  wh::tool::tool_options skip_options{};
  skip_options.set_base(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::skip, .timeout_label = "budgetA"});

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_callbacks(
      std::move(callback_context), [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage, const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::tool::tool_callback_event>();
        if (typed == nullptr) {
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          error_context = typed->error_context;
        }
        if (stage == wh::core::callback_stage::end) {
          ended.fetch_add(1, std::memory_order_release);
        }
      });
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  auto skipped_invoke =
      skip_tool.invoke(wh::tool::tool_request{"{}", skip_options}, callback_context);
  REQUIRE(skipped_invoke.has_value());
  REQUIRE(skipped_invoke.value().empty());

  auto skipped_stream =
      skip_tool.stream(wh::tool::tool_request{"{}", skip_options}, callback_context);
  REQUIRE(skipped_stream.has_value());
  auto skipped_reader = std::move(skipped_stream).value();
  auto eof = take_try_chunk(skipped_reader);
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
  REQUIRE(error_context == "budgetA");
  REQUIRE(ended.load(std::memory_order_acquire) == 2);
}

TEST_CASE("tool wrapper preserves async validation retry and stream semantics",
          "[UT][wh/tool/tool.hpp][tool::async_invoke][branch]") {
  wh::schema::tool_schema_definition schema{};
  schema.name = "async_tool";
  schema.parameters = {wh::schema::tool_parameter_schema{
      .name = "count", .type = wh::schema::tool_parameter_type::integer, .required = true}};

  std::atomic<int> attempts{0};
  wh::tool::tool tool{
      schema,
      sender_tool_impl{
          sender_tool_invoke_impl{[&](const std::string_view, const wh::tool::tool_options &)
                                      -> wh::tool::tool_invoke_result {
            const auto current = attempts.fetch_add(1, std::memory_order_relaxed);
            if (current < 2) {
              return wh::tool::tool_invoke_result::failure(wh::core::errc::network_error);
            }
            return std::string{"async-ok"};
          }},
          sender_tool_stream_impl{[](const std::string_view input, const wh::tool::tool_options &)
                                      -> wh::tool::tool_output_stream_result {
            return wh::tool::tool_output_stream_reader{
                wh::schema::stream::make_single_value_stream_reader<std::string>(
                    std::string{input})};
          }}}};

  wh::core::run_context invalid_context{};
  auto invalid = wh::testing::helper::wait_value_on_test_thread(
      tool.async_invoke(wh::tool::tool_request{R"({"count":"bad"})", {}}, invalid_context));
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  wh::tool::tool_options retry_options{};
  retry_options.set_base(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::retry, .max_retries = 3U});
  wh::core::run_context async_context{};
  auto invoked = wh::testing::helper::wait_value_on_test_thread(
      tool.async_invoke(wh::tool::tool_request{R"({"count":1})", retry_options}, async_context));
  REQUIRE(invoked.has_value());
  REQUIRE(invoked.value() == "async-ok");
  REQUIRE(attempts.load(std::memory_order_relaxed) == 3);

  auto streamed = wh::testing::helper::wait_value_on_test_thread(
      tool.async_stream(wh::tool::tool_request{R"({"count":2})", {}}, async_context));
  REQUIRE(streamed.has_value());
  auto reader = std::move(streamed).value();
  auto first = take_try_chunk(reader);
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(first.value().value.value() == R"({"count":2})");
}

TEST_CASE("tool async stream survives external stop without dangling stop callbacks",
          "[UT][wh/tool/tool.hpp][tool::async_stream][lifecycle][concurrency]") {
  struct delayed_stream_impl {
    wh::testing::helper::manual_scheduler<> scheduler{};

    [[nodiscard]] auto stream_sender(wh::tool::tool_request request) const {
      return stdexec::schedule(scheduler) |
             stdexec::then(
                 [request = std::move(request)]() mutable -> wh::tool::tool_output_stream_result {
                   return wh::tool::tool_output_stream_result{
                       wh::schema::stream::make_single_value_stream_reader<std::string>(
                           std::move(request.input_json))};
                 });
    }
  };

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  wh::schema::tool_schema_definition schema{};
  schema.name = "delayed-stream";

  wh::tool::tool delayed_tool{
      schema, delayed_stream_impl{
                  .scheduler = wh::testing::helper::manual_scheduler<>{&scheduler_state},
              }};

  wh::core::run_context context{};
  wh::testing::helper::sender_capture<wh::tool::tool_output_stream_result> capture{};
  stdexec::inplace_stop_source stop_source{};

  auto operation =
      stdexec::connect(delayed_tool.async_stream(wh::tool::tool_request{"{}", {}}, context),
                       wh::testing::helper::sender_capture_receiver{
                           &capture,
                           wh::testing::helper::make_scheduler_env(stdexec::inline_scheduler{},
                                                                   stop_source.get_token()),
                       });

  stdexec::start(operation);
  REQUIRE(scheduler_state.pending_count() == 1U);

  stop_source.request_stop();
  REQUIRE(scheduler_state.run_one());
  REQUIRE(capture.ready.try_acquire_for(std::chrono::milliseconds(100)));
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(capture.value->has_error());
  REQUIRE(capture.value->error() == wh::core::errc::canceled);
}

TEST_CASE("tool async invoke treats child set_stopped as terminal cancellation",
          "[UT][wh/tool/tool.hpp][tool::async_invoke][stopped][concurrency]") {
  wh::schema::tool_schema_definition schema{};
  schema.name = "stopped-tool";

  std::atomic<int> attempts{0};
  struct stopped_invoke_impl {
    std::atomic<int> *attempts{nullptr};

    [[nodiscard]] auto invoke_sender(wh::tool::tool_request) const {
      attempts->fetch_add(1, std::memory_order_relaxed);
      return stdexec::just_stopped();
    }
  };

  wh::tool::tool stopped_tool{
      schema,
      stopped_invoke_impl{.attempts = &attempts},
  };

  wh::tool::tool_options retry_options{};
  retry_options.set_base(wh::tool::tool_common_options{
      .failure_policy = wh::tool::tool_failure_policy::retry, .max_retries = 3U});

  wh::core::run_context context{};
  auto result = wh::testing::helper::wait_value_on_test_thread(
      stopped_tool.async_invoke(wh::tool::tool_request{"{}", retry_options}, context));

  REQUIRE(result.has_error());
  REQUIRE(result.error() == wh::core::errc::canceled);
  REQUIRE(attempts.load(std::memory_order_relaxed) == 1);
}

TEST_CASE("tool async invoke retries without recursive inline child starts",
          "[UT][wh/tool/tool.hpp][tool::async_invoke][retry][trampoline]") {
  std::atomic<int> attempts{0};
  std::atomic<int> active_depth{0};
  std::atomic<int> max_depth{0};

  wh::schema::tool_schema_definition schema{};
  schema.name = "inline-retry";

  constexpr int fail_before_success = 256;
  wh::tool::tool retry_tool{
      schema,
      inline_retry_impl{
          .attempts = &attempts,
          .active_depth = &active_depth,
          .max_depth = &max_depth,
          .fail_before_success = fail_before_success,
      },
  };

  wh::tool::tool_options retry_options{};
  retry_options.set_base(
      wh::tool::tool_common_options{.failure_policy = wh::tool::tool_failure_policy::retry,
                                    .max_retries = static_cast<std::size_t>(fail_before_success)});

  wh::core::run_context context{};
  auto result = wh::testing::helper::wait_value_on_test_thread(
      retry_tool.async_invoke(wh::tool::tool_request{"{}", retry_options}, context));

  REQUIRE(result.has_value());
  REQUIRE(result.value() == "ok");
  REQUIRE(attempts.load(std::memory_order_relaxed) == fail_before_success + 1);
  REQUIRE(max_depth.load(std::memory_order_relaxed) <= 16);
}

TEST_CASE("tool async invoke keeps inline child operation alive until child start returns",
          "[UT][wh/tool/tool.hpp][tool::async_invoke][lifetime][regression]") {
  wh::schema::tool_schema_definition schema{};
  schema.name = "attempt-lifetime";

  attempt_lifetime_probe_impl impl{};
  wh::tool::tool probe_tool{schema, impl};

  wh::core::run_context context{};
  auto result = wh::testing::helper::wait_value_on_test_thread(
      probe_tool.async_invoke(wh::tool::tool_request{"{}", {}}, context));

  REQUIRE(result.has_value());
  REQUIRE(result.value() == "ok");
  REQUIRE(impl.state->child_destroyed.load(std::memory_order_acquire));
  REQUIRE_FALSE(impl.state->child_destroyed_before_start_return.load(std::memory_order_acquire));
}

TEST_CASE(
    "tool schema validation covers required object enum array one-of and interruption flags",
    "[UT][wh/tool/tool.hpp][detail::validate_value_against_schema][condition][branch][boundary]") {
  wh::schema::tool_parameter_schema mode{};
  mode.name = "mode";
  mode.type = wh::schema::tool_parameter_type::string;
  mode.required = true;
  mode.enum_values = {"fast", "safe"};

  wh::schema::tool_parameter_schema payload{};
  payload.name = "payload";
  payload.type = wh::schema::tool_parameter_type::object;
  payload.required = true;
  payload.properties = {mode};

  wh::schema::tool_parameter_schema integer_item{};
  integer_item.type = wh::schema::tool_parameter_type::integer;
  wh::schema::tool_parameter_schema string_item{};
  string_item.type = wh::schema::tool_parameter_type::string;

  wh::schema::tool_parameter_schema ids{};
  ids.name = "ids";
  ids.type = wh::schema::tool_parameter_type::array;
  ids.item_types = {integer_item, string_item};

  const std::array parameters = {payload, ids};
  std::string error_path{};

  auto valid = wh::tool::detail::validate_tool_input_schema(
      R"({"payload":{"mode":"fast"},"ids":[1,"two",3]})", parameters, error_path);
  REQUIRE(valid.has_value());

  auto missing_required = wh::tool::detail::validate_tool_input_schema(
      R"({"payload":{},"ids":[1]})", parameters, error_path);
  REQUIRE(missing_required.has_error());
  REQUIRE(error_path == "$.payload.mode");

  auto invalid_enum = wh::tool::detail::validate_tool_input_schema(
      R"({"payload":{"mode":"slow"},"ids":[1]})", parameters, error_path);
  REQUIRE(invalid_enum.has_error());
  REQUIRE(error_path == "$.payload.mode");

  auto invalid_array = wh::tool::detail::validate_tool_input_schema(
      R"({"payload":{"mode":"safe"},"ids":[true]})", parameters, error_path);
  REQUIRE(invalid_array.has_error());
  REQUIRE(error_path == "$.ids[0]");

  wh::tool::detail::callback_state state{};
  wh::tool::tool_common_options common_options{.failure_policy =
                                                   wh::tool::tool_failure_policy::fail_fast,
                                               .max_retries = 0U,
                                               .timeout_label = "budgetB"};
  auto resolved = wh::tool::detail::resolve_options_view(common_options);
  wh::tool::detail::mark_error(state, wh::core::errc::canceled, resolved);
  REQUIRE(state.event.interrupted);
  REQUIRE(state.event.error_context == "budgetB");
}
