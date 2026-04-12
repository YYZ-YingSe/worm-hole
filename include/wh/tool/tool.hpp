// Defines the executable tool component and its execution contracts.
#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/component.hpp"
#include "wh/core/error.hpp"
#include "wh/core/json.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/detail/callback_guard.hpp"
#include "wh/core/stdexec/detail/receiver_completion.hpp"
#include "wh/core/stdexec/detail/scheduled_drive_loop.hpp"
#include "wh/core/stdexec/detail/shared_operation_state.hpp"
#include "wh/core/stdexec/detail/single_completion_slot.hpp"
#include "wh/core/stdexec/manual_lifetime_box.hpp"
#include "wh/schema/stream.hpp"
#include "wh/schema/tool.hpp"
#include "wh/tool/callback_event.hpp"
#include "wh/tool/options.hpp"
#include "wh/tool/utils/common.hpp"

namespace wh::tool {

using tool_output_stream_reader = wh::schema::stream::any_stream_reader<std::string>;
using tool_output_stream_writer = wh::schema::stream::any_stream_writer<std::string>;
using tool_invoke_result = wh::core::result<std::string>;
using tool_output_stream_result = wh::core::result<tool_output_stream_reader>;

struct tool_request {
  /// Raw JSON input payload passed across the tool contract boundary.
  std::string input_json{};
  tool_options options{};
};

namespace detail {

[[nodiscard]] inline auto append_path_segment(const std::string &path,
                                              const std::string_view segment) -> std::string {
  if (path.empty()) {
    return std::string{segment};
  }
  std::string next = path;
  next.push_back('.');
  next.append(segment);
  return next;
}

[[nodiscard]] inline auto append_path_index(const std::string &path, const std::size_t index)
    -> std::string {
  std::string next = path;
  next.push_back('[');
  next.append(std::to_string(index));
  next.push_back(']');
  return next;
}

[[nodiscard]] inline auto validate_value_against_schema(
    const wh::core::json_value &value, const wh::schema::tool_parameter_schema &schema,
    const std::string &path, std::string &error_path, const bool capture_error = true) -> bool {
  const auto fail = [&]() -> bool {
    if (capture_error) {
      error_path = path;
    }
    return false;
  };

  const auto type_ok = [&]() -> bool {
    switch (schema.type) {
    case wh::schema::tool_parameter_type::string:
      return value.IsString();
    case wh::schema::tool_parameter_type::integer:
      return value.IsInt64() || value.IsUint64();
    case wh::schema::tool_parameter_type::number:
      return value.IsNumber();
    case wh::schema::tool_parameter_type::boolean:
      return value.IsBool();
    case wh::schema::tool_parameter_type::object:
      return value.IsObject();
    case wh::schema::tool_parameter_type::array:
      return value.IsArray();
    }
    return false;
  };

  if (!type_ok()) {
    return fail();
  }

  if (schema.type == wh::schema::tool_parameter_type::string && !schema.enum_values.empty()) {
    const std::string_view current{value.GetString(),
                                   static_cast<std::size_t>(value.GetStringLength())};
    if (std::ranges::find(schema.enum_values, current) == schema.enum_values.end()) {
      return fail();
    }
  }

  if (!schema.one_of.empty()) {
    bool matched = false;
    for (const auto &candidate : schema.one_of) {
      std::string candidate_error{};
      if (validate_value_against_schema(value, candidate, path, candidate_error, false)) {
        matched = true;
        break;
      }
    }
    if (!matched) {
      return fail();
    }
  }

  if (schema.type == wh::schema::tool_parameter_type::object) {
    for (const auto &property : schema.properties) {
      const auto child_path = append_path_segment(path, property.name);
      const auto member = value.FindMember(rapidjson::StringRef(
          property.name.data(), static_cast<rapidjson::SizeType>(property.name.size())));
      if (member == value.MemberEnd()) {
        if (property.required) {
          if (capture_error) {
            error_path = child_path;
          }
          return false;
        }
        continue;
      }
      if (!validate_value_against_schema(member->value, property, child_path, error_path,
                                         capture_error)) {
        return false;
      }
    }
  }

  if (schema.type == wh::schema::tool_parameter_type::array && !schema.item_types.empty()) {
    for (std::size_t index = 0U; index < value.Size(); ++index) {
      const auto &item = value[static_cast<wh::core::json_size_type>(index)];
      const auto item_path = append_path_index(path, index);
      if (schema.item_types.size() == 1U) {
        if (!validate_value_against_schema(item, schema.item_types.front(), item_path, error_path,
                                           capture_error)) {
          return false;
        }
        continue;
      }
      bool matched = false;
      for (const auto &candidate : schema.item_types) {
        std::string candidate_error{};
        if (validate_value_against_schema(item, candidate, item_path, candidate_error, false)) {
          matched = true;
          break;
        }
      }
      if (!matched) {
        if (capture_error) {
          error_path = item_path;
        }
        return false;
      }
    }
  }

  return true;
}

[[nodiscard]] inline auto
validate_tool_input_schema(const std::string_view input,
                           const std::span<const wh::schema::tool_parameter_schema> parameters,
                           std::string &error_path) -> wh::core::result<void> {
  if (parameters.empty()) {
    return {};
  }

  auto parsed = wh::core::parse_json(input);
  if (parsed.has_error()) {
    error_path = "$";
    return wh::core::result<void>::failure(parsed.error());
  }
  if (!parsed.value().IsObject()) {
    error_path = "$";
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }

  for (const auto &parameter : parameters) {
    const auto param_path = append_path_segment("$", parameter.name);
    const auto member = parsed.value().FindMember(rapidjson::StringRef(
        parameter.name.data(), static_cast<rapidjson::SizeType>(parameter.name.size())));
    if (member == parsed.value().MemberEnd()) {
      if (parameter.required) {
        error_path = param_path;
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
      continue;
    }

    if (!validate_value_against_schema(member->value, parameter, param_path, error_path)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
  }

  return {};
}

struct callback_state {
  wh::callbacks::run_info run_info{};
  tool_callback_event event{};
};

using callback_sink = wh::callbacks::callback_sink;
using wh::callbacks::borrow_callback_sink;
using wh::callbacks::make_callback_sink;

template <typename... args_t> inline auto emit_callback(args_t &&...args) -> void {
  wh::callbacks::emit(std::forward<args_t>(args)...);
}

[[nodiscard]] inline auto make_callback_state(const std::string_view tool_name,
                                              const tool_request &request) -> callback_state {
  callback_state state{};
  state.run_info.name = std::string{tool_name};
  state.run_info.type = wh::tool::utils::to_camel_case(tool_name);
  state.run_info.component = wh::core::component_kind::tool;
  state.run_info =
      wh::callbacks::apply_component_run_info(std::move(state.run_info), request.options);
  state.event.tool_name = std::string{tool_name};
  state.event.input_json = request.input_json;
  return state;
}

[[nodiscard]] inline auto merge_options(const tool_options &base, const tool_options &call_override)
    -> tool_options {
  tool_options merged = base;
  merged.set_call_override(call_override.resolve());
  return merged;
}

inline auto mark_error(callback_state &state, const wh::core::error_code error,
                       const resolved_tool_options_view &options) -> void {
  state.event.interrupted =
      error == wh::core::errc::canceled || error == wh::core::errc::contract_violation;
  if (state.event.error_context.empty() && !options.timeout_label.empty()) {
    state.event.error_context = options.timeout_label;
  }
}

[[nodiscard]] inline auto make_skipped_stream() -> tool_output_stream_result {
  return tool_output_stream_reader{wh::schema::stream::make_empty_stream_reader<std::string>()};
}

[[nodiscard]] inline auto resolve_options_view(const tool_common_options &options) noexcept
    -> resolved_tool_options_view {
  return resolved_tool_options_view{options.failure_policy, options.max_retries,
                                    options.timeout_label};
}

template <typename output_t>
concept invoke_output_compatible =
    std::same_as<std::remove_cvref_t<output_t>, tool_invoke_result> ||
    std::same_as<std::remove_cvref_t<output_t>, std::string> ||
    std::convertible_to<std::remove_cvref_t<output_t>, std::string_view>;

template <typename output_t>
concept stream_output_compatible =
    std::same_as<std::remove_cvref_t<output_t>, tool_output_stream_result> ||
    std::same_as<std::remove_cvref_t<output_t>, tool_output_stream_reader>;

template <typename impl_t>
concept sync_invoke_handler = requires(const impl_t &impl, const tool_request &request) {
  requires invoke_output_compatible<decltype(impl.invoke(request))>;
};

template <typename impl_t>
concept sync_stream_handler = requires(const impl_t &impl, const tool_request &request) {
  requires stream_output_compatible<decltype(impl.stream(request))>;
};

template <typename impl_t>
concept sender_invoke_handler_const = requires(const impl_t &impl, const tool_request &request) {
  { impl.invoke_sender(request) } -> stdexec::sender;
};

template <typename impl_t>
concept sender_invoke_handler_move = requires(const impl_t &impl, tool_request &&request) {
  { impl.invoke_sender(std::move(request)) } -> stdexec::sender;
};

template <typename impl_t>
concept sender_stream_handler_const = requires(const impl_t &impl, const tool_request &request) {
  { impl.stream_sender(request) } -> stdexec::sender;
};

template <typename impl_t>
concept sender_stream_handler_move = requires(const impl_t &impl, tool_request &&request) {
  { impl.stream_sender(std::move(request)) } -> stdexec::sender;
};

template <typename impl_t>
concept async_invoke_capable =
    sender_invoke_handler_const<impl_t> || sender_invoke_handler_move<impl_t>;

template <typename impl_t>
concept async_stream_capable =
    sender_stream_handler_const<impl_t> || sender_stream_handler_move<impl_t>;

template <typename impl_t>
concept sender_invoke_handler = async_invoke_capable<impl_t>;

template <typename impl_t>
concept sender_stream_handler = async_stream_capable<impl_t>;

template <typename impl_t>
concept invoke_capable = async_invoke_capable<impl_t> || sync_invoke_handler<impl_t>;

template <typename impl_t>
concept stream_capable = async_stream_capable<impl_t> || sync_stream_handler<impl_t>;

template <typename impl_t>
concept tool_impl = invoke_capable<impl_t> || stream_capable<impl_t>;

template <typename output_t>
[[nodiscard]] inline auto normalize_invoke_output(output_t &&output) -> tool_invoke_result {
  using value_t = std::remove_cvref_t<output_t>;
  if constexpr (std::same_as<value_t, tool_invoke_result>) {
    return std::forward<output_t>(output);
  } else if constexpr (std::same_as<value_t, std::string>) {
    return std::forward<output_t>(output);
  } else if constexpr (std::convertible_to<value_t, std::string_view>) {
    return std::string{std::string_view{std::forward<output_t>(output)}};
  } else {
    return tool_invoke_result::failure(wh::core::errc::contract_violation);
  }
}

template <typename output_t>
[[nodiscard]] inline auto normalize_stream_output(output_t &&output) -> tool_output_stream_result {
  using value_t = std::remove_cvref_t<output_t>;
  if constexpr (std::same_as<value_t, tool_output_stream_result>) {
    return std::forward<output_t>(output);
  } else if constexpr (std::same_as<value_t, tool_output_stream_reader>) {
    return std::forward<output_t>(output);
  } else {
    return tool_output_stream_result::failure(wh::core::errc::contract_violation);
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_invoke(const impl_t &impl, const tool_request &request)
    -> tool_invoke_result {
  if constexpr (requires {
                  { impl.invoke(request) };
                }) {
    return normalize_invoke_output(impl.invoke(request));
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_invoke(const impl_t &impl, tool_request &&request)
    -> tool_invoke_result {
  if constexpr (requires {
                  { impl.invoke(std::move(request)) };
                }) {
    return normalize_invoke_output(impl.invoke(std::move(request)));
  } else {
    return run_sync_invoke(impl, static_cast<const tool_request &>(request));
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_stream(const impl_t &impl, const tool_request &request)
    -> tool_output_stream_result {
  if constexpr (requires {
                  { impl.stream(request) };
                }) {
    return normalize_stream_output(impl.stream(request));
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_stream(const impl_t &impl, tool_request &&request)
    -> tool_output_stream_result {
  if constexpr (requires {
                  { impl.stream(std::move(request)) };
                }) {
    return normalize_stream_output(impl.stream(std::move(request)));
  } else {
    return run_sync_stream(impl, static_cast<const tool_request &>(request));
  }
}

template <typename impl_t, typename request_t>
  requires async_invoke_capable<impl_t>
[[nodiscard]] inline auto make_invoke_sender(const impl_t &impl, request_t &&request) {
  using request_value_t = std::remove_cvref_t<request_t>;
  static_assert(std::same_as<request_value_t, tool_request>,
                "tool sender factory requires tool_request input");
  return wh::core::detail::request_result_sender<tool_invoke_result>(
      std::forward<request_t>(request), [&impl](auto &&forwarded_request) -> decltype(auto) {
        return impl.invoke_sender(std::forward<decltype(forwarded_request)>(forwarded_request));
      });
}

template <typename impl_t, typename request_t>
  requires async_stream_capable<impl_t>
[[nodiscard]] inline auto make_stream_sender(const impl_t &impl, request_t &&request) {
  using request_value_t = std::remove_cvref_t<request_t>;
  static_assert(std::same_as<request_value_t, tool_request>,
                "tool sender factory requires tool_request input");
  return wh::core::detail::request_result_sender<tool_output_stream_result>(
      std::forward<request_t>(request), [&impl](auto &&forwarded_request) -> decltype(auto) {
        return impl.stream_sender(std::forward<decltype(forwarded_request)>(forwarded_request));
      });
}

template <typename result_t> struct tool_result_traits;

template <> struct tool_result_traits<tool_invoke_result> {
  static auto record_success(callback_state &state, tool_invoke_result &status) -> void {
    state.event.output_text = status.value();
  }

  [[nodiscard]] static auto make_skip_result() -> tool_invoke_result { return std::string{}; }
};

template <> struct tool_result_traits<tool_output_stream_result> {
  static auto record_success(callback_state &, tool_output_stream_result &) -> void {}

  [[nodiscard]] static auto make_skip_result() -> tool_output_stream_result {
    return make_skipped_stream();
  }
};

template <typename result_t> struct tool_run_state {
  tool_request request{};
  callback_sink sink{};
  callback_state callback{};
  tool_common_options resolved_options{};
  std::size_t next_attempt{0U};
  std::size_t max_attempts{1U};
  wh::core::error_code last_error = wh::core::make_error(wh::core::errc::ok);
  std::optional<result_t> success{};
};

template <typename result_t>
[[nodiscard]] inline auto prepare_tool_run(tool_request request, callback_sink sink,
                                           const wh::schema::tool_schema_definition &schema,
                                           const tool_options &default_options)
    -> wh::core::result<tool_run_state<result_t>> {
  auto effective_request =
      tool_request{std::move(request.input_json), merge_options(default_options, request.options)};
  sink = wh::callbacks::filter_callback_sink(std::move(sink), effective_request.options);
  auto resolved_options = effective_request.options.resolve();

  auto state = tool_run_state<result_t>{
      .request = std::move(effective_request),
      .sink = std::move(sink),
      .resolved_options = std::move(resolved_options),
      .last_error = wh::core::make_error(wh::core::errc::ok),
  };
  state.callback = make_callback_state(schema.name, state.request);
  state.max_attempts = state.resolved_options.failure_policy == tool_failure_policy::retry
                           ? state.resolved_options.max_retries + 1U
                           : 1U;

  emit_callback(state.sink, wh::callbacks::stage::start, state.callback);

  std::string validation_path{};
  auto validated =
      validate_tool_input_schema(state.request.input_json, schema.parameters, validation_path);
  if (validated.has_error()) {
    state.callback.event.error_context = std::move(validation_path);
    emit_callback(state.sink, wh::callbacks::stage::error, state.callback);
    return wh::core::result<tool_run_state<result_t>>::failure(validated.error());
  }
  return state;
}

template <typename result_t>
[[nodiscard]] inline auto consume_tool_attempt_result(tool_run_state<result_t> &state,
                                                      result_t status) -> bool {
  if (status.has_value()) {
    tool_result_traits<result_t>::record_success(state.callback, status);
    state.success.emplace(std::move(status));
    return true;
  }

  state.last_error = status.error();
  mark_error(state.callback, state.last_error, resolve_options_view(state.resolved_options));
  emit_callback(state.sink, wh::callbacks::stage::error, state.callback);
  return state.resolved_options.failure_policy != tool_failure_policy::retry ||
         state.next_attempt >= state.max_attempts;
}

template <typename result_t>
[[nodiscard]] inline auto finish_tool_run(tool_run_state<result_t> state) -> result_t {
  if (state.success.has_value()) {
    emit_callback(state.sink, wh::callbacks::stage::end, state.callback);
    return std::move(*state.success);
  }

  if (state.resolved_options.failure_policy == tool_failure_policy::skip) {
    auto skipped = tool_result_traits<result_t>::make_skip_result();
    if (skipped.has_error()) {
      state.callback.event.error_context =
          std::string{resolve_options_view(state.resolved_options).timeout_label};
      emit_callback(state.sink, wh::callbacks::stage::error, state.callback);
      return skipped;
    }
    emit_callback(state.sink, wh::callbacks::stage::end, state.callback);
    return skipped;
  }

  return result_t::failure(state.last_error);
}

template <typename result_t, typename run_attempt_t>
[[nodiscard]] inline auto run_sync_tool_loop(tool_run_state<result_t> state,
                                             run_attempt_t &&run_attempt) -> result_t {
  while (state.next_attempt < state.max_attempts) {
    ++state.next_attempt;
    if (consume_tool_attempt_result(
            state, std::invoke(std::forward<run_attempt_t>(run_attempt), state.request))) {
      break;
    }
  }
  return finish_tool_run(std::move(state));
}

template <typename result_t, typename state_t, typename make_attempt_t>
class tool_attempt_loop_sender {
  template <typename receiver_t>
  class operation
      : public std::enable_shared_from_this<operation<receiver_t>>,
        private wh::core::detail::scheduled_drive_loop<operation<receiver_t>,
                                                       wh::core::detail::any_resume_scheduler_t> {
    using drive_loop_t =
        wh::core::detail::scheduled_drive_loop<operation<receiver_t>,
                                               wh::core::detail::any_resume_scheduler_t>;
    friend drive_loop_t;
    friend class wh::core::detail::callback_guard<operation>;

    using receiver_env_t = decltype(stdexec::get_env(std::declval<const receiver_t &>()));
    using final_completion_t = wh::core::detail::receiver_completion<receiver_t, result_t>;

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      operation *op{nullptr};
      receiver_env_t env_{};

      auto set_value(result_t status) && noexcept -> void {
        auto scope = op->callbacks_.enter(op);
        op->finish_child(std::move(status));
      }

      template <typename error_t> auto set_error(error_t &&) && noexcept -> void {
        auto scope = op->callbacks_.enter(op);
        op->finish_child(result_t::failure(wh::core::errc::internal_error));
      }

      auto set_stopped() && noexcept -> void {
        auto scope = op->callbacks_.enter(op);
        op->finish_child(result_t::failure(wh::core::errc::canceled));
      }

      [[nodiscard]] auto get_env() const noexcept { return env_; }
    };

    using child_sender_t = std::remove_cvref_t<std::invoke_result_t<make_attempt_t &, state_t &>>;
    using child_op_t = stdexec::connect_result_t<child_sender_t, child_receiver>;

  public:
    template <typename stored_state_t, typename stored_make_attempt_t>
      requires std::constructible_from<wh::core::result<state_t>, stored_state_t &&> &&
                   std::constructible_from<make_attempt_t, stored_make_attempt_t &&>
    explicit operation(stored_state_t &&state,
                       const wh::core::detail::any_resume_scheduler_t &drive_scheduler,
                       stored_make_attempt_t &&make_attempt, receiver_t receiver)
        : drive_loop_t(drive_scheduler), receiver_(std::move(receiver)),
          state_(std::forward<stored_state_t>(state)),
          make_attempt_(std::forward<stored_make_attempt_t>(make_attempt)) {}

    auto start() noexcept -> void { request_drive(); }

  private:
    [[nodiscard]] auto finished() const noexcept -> bool {
      return delivered_.load(std::memory_order_acquire);
    }

    [[nodiscard]] auto completion_pending() const noexcept -> bool {
      return pending_completion_.has_value();
    }

    [[nodiscard]] auto take_completion() noexcept -> std::optional<final_completion_t> {
      if (!pending_completion_.has_value()) {
        return std::nullopt;
      }
      auto completion = std::move(pending_completion_);
      pending_completion_.reset();
      return completion;
    }

    [[nodiscard]] auto acquire_owner_lifetime_guard() noexcept -> std::shared_ptr<operation> {
      auto keepalive = this->weak_from_this().lock();
      if (!keepalive) {
        ::wh::core::contract_violation(::wh::core::contract_kind::invariant,
                                       "tool attempt loop owner lifetime guard expired");
      }
      return keepalive;
    }

    auto on_callback_exit() noexcept -> void {
      if (completion_.ready()) {
        request_drive();
      }
    }

    auto request_drive() noexcept -> void { drive_loop_t::request_drive(); }

    auto drive() noexcept -> void {
      while (!finished()) {
        if (callbacks_.active()) {
          return;
        }

        if (auto current = completion_.take(); current.has_value()) {
          if (!attempt_in_flight_) {
            std::terminate();
          }
          child_op_.reset();
          attempt_in_flight_ = false;
          terminal_ready_ = consume_tool_attempt_result(state_.value(), std::move(*current));
          continue;
        }

        if (state_.has_error()) {
          finish(result_t::failure(state_.error()));
          return;
        }

        if (terminal_ready_) {
          finish(finish_tool_run(std::move(state_).value()));
          return;
        }

        if (attempt_in_flight_) {
          return;
        }

        start_next_attempt();
        if (completion_.ready() || terminal_ready_) {
          continue;
        }
        if (attempt_in_flight_) {
          return;
        }
      }
    }

    auto drive_error(const wh::core::error_code error) noexcept -> void {
      finish(result_t::failure(error));
    }

    auto start_next_attempt() noexcept -> void {
      auto &run_state = state_.value();
      if (run_state.next_attempt >= run_state.max_attempts) {
        terminal_ready_ = true;
        return;
      }

      ++run_state.next_attempt;
      attempt_in_flight_ = true;

      try {
        child_op_.emplace_from(stdexec::connect, std::invoke(make_attempt_, run_state),
                               child_receiver{this, stdexec::get_env(receiver_)});
      } catch (...) {
        child_op_.reset();
        attempt_in_flight_ = false;
        run_state.last_error = wh::core::make_error(wh::core::errc::internal_error);
        terminal_ready_ = true;
        return;
      }

      stdexec::start(child_op_.get());
    }

    auto finish_child(result_t status) noexcept -> void {
      if (finished()) {
        return;
      }
#ifndef NDEBUG
      wh_invariant(completion_.publish(std::move(status)));
#else
      completion_.publish(std::move(status));
#endif
      request_drive();
    }

    auto finish(result_t status) noexcept -> void {
      if (delivered_.exchange(true, std::memory_order_acq_rel)) {
        return;
      }
      child_op_.reset();
      attempt_in_flight_ = false;
      completion_.reset();
      pending_completion_.emplace(
          final_completion_t::set_value(std::move(receiver_), std::move(status)));
    }

    receiver_t receiver_;
    wh::core::result<state_t> state_;
    make_attempt_t make_attempt_;
    wh::core::detail::manual_lifetime_box<child_op_t> child_op_{};
    wh::core::detail::single_completion_slot<result_t> completion_{};
    wh::core::detail::callback_guard<operation> callbacks_{};
    std::optional<final_completion_t> pending_completion_{};
    std::atomic<bool> delivered_{false};
    bool attempt_in_flight_{false};
    bool terminal_ready_{false};
  };

public:
  using sender_concept = stdexec::sender_t;

  template <typename stored_make_attempt_t>
    requires std::constructible_from<make_attempt_t, stored_make_attempt_t &&>
  explicit tool_attempt_loop_sender(wh::core::result<state_t> state,
                                    stored_make_attempt_t &&make_attempt)
      : state_(std::move(state)), make_attempt_(std::forward<stored_make_attempt_t>(make_attempt)) {
  }

  template <typename self_t, stdexec::receiver receiver_t>
    requires std::same_as<std::remove_cvref_t<self_t>, tool_attempt_loop_sender> &&
             (!std::is_const_v<std::remove_reference_t<self_t>>)
  STDEXEC_EXPLICIT_THIS_BEGIN(auto connect)(this self_t &&self, receiver_t receiver) {
    using stored_receiver_t = std::remove_cvref_t<receiver_t>;
    using env_t = std::remove_cvref_t<decltype(stdexec::get_env(receiver))>;
    auto drive_scheduler = [&]() -> wh::core::detail::any_resume_scheduler_t {
      if constexpr (wh::core::detail::env_with_resume_scheduler<stdexec::set_value_t, env_t>) {
        return wh::core::detail::erase_resume_scheduler(
            wh::core::detail::select_resume_scheduler<stdexec::set_value_t>(
                stdexec::get_env(receiver)));
      } else {
        return wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});
      }
    }();
    using operation_t = operation<stored_receiver_t>;
    return wh::core::detail::shared_operation_state<operation_t>{std::make_shared<operation_t>(
        std::forward<self_t>(self).state_, std::move(drive_scheduler),
        std::forward<self_t>(self).make_attempt_, std::move(receiver))};
  }
  STDEXEC_EXPLICIT_THIS_END(connect)

  template <typename self_t, typename... env_t>
    requires std::same_as<std::remove_cvref_t<self_t>, tool_attempt_loop_sender> &&
             (sizeof...(env_t) >= 1U)
  static consteval auto get_completion_signatures() {
    return stdexec::completion_signatures<stdexec::set_value_t(result_t)>{};
  }

private:
  template <typename> friend class operation;

  wh::core::result<state_t> state_;
  make_attempt_t make_attempt_;
};

template <typename result_t, typename state_t, typename make_attempt_t>
[[nodiscard]] inline auto make_tool_attempt_loop_sender(wh::core::result<state_t> state,
                                                        make_attempt_t &&make_attempt)
    -> tool_attempt_loop_sender<result_t, state_t, std::remove_cvref_t<make_attempt_t>> {
  using sender_t = tool_attempt_loop_sender<result_t, state_t, std::remove_cvref_t<make_attempt_t>>;
  return sender_t{std::move(state), std::forward<make_attempt_t>(make_attempt)};
}

} // namespace detail

/// Public interface for one executable tool component.
template <detail::tool_impl impl_t, wh::core::resume_mode Resume = wh::core::resume_mode::unchanged>
class tool {
public:
  explicit tool(const wh::schema::tool_schema_definition &schema, const impl_t &impl,
                const tool_options &default_options = tool_options{})
    requires std::copy_constructible<impl_t>
      : schema_(schema), impl_(impl), default_options_(default_options) {}

  explicit tool(wh::schema::tool_schema_definition &&schema, const impl_t &impl,
                const tool_options &default_options = tool_options{})
    requires std::copy_constructible<impl_t>
      : schema_(std::move(schema)), impl_(impl), default_options_(default_options) {}

  explicit tool(const wh::schema::tool_schema_definition &schema, impl_t &&impl,
                const tool_options &default_options = tool_options{})
      : schema_(schema), impl_(std::move(impl)), default_options_(default_options) {}

  explicit tool(wh::schema::tool_schema_definition &&schema, impl_t &&impl,
                const tool_options &default_options = tool_options{})
      : schema_(std::move(schema)), impl_(std::move(impl)), default_options_(default_options) {}

  tool(const tool &) = default;
  tool(tool &&) noexcept = default;
  auto operator=(const tool &) -> tool & = default;
  auto operator=(tool &&) noexcept -> tool & = default;
  ~tool() = default;

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{wh::tool::utils::to_camel_case(schema_.name),
                                          wh::core::component_kind::tool};
  }

  [[nodiscard]] auto schema() const noexcept -> const wh::schema::tool_schema_definition & {
    return schema_;
  }

  [[nodiscard]] auto default_options() const noexcept -> const tool_options & {
    return default_options_;
  }

  [[nodiscard]] auto impl() const noexcept -> const impl_t & { return impl_; }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, tool_request>
  [[nodiscard]] auto invoke(request_t &&request, wh::core::run_context &callback_context) const
      -> tool_invoke_result
    requires detail::sync_invoke_handler<impl_t>
  {
    return invoke_sync_impl(std::forward<request_t>(request),
                            detail::borrow_callback_sink(callback_context));
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, tool_request>
  [[nodiscard]] auto stream(request_t &&request, wh::core::run_context &callback_context) const
      -> tool_output_stream_result
    requires detail::sync_stream_handler<impl_t>
  {
    return stream_sync_impl(std::forward<request_t>(request),
                            detail::borrow_callback_sink(callback_context));
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, tool_request>
  [[nodiscard]] auto async_invoke(request_t &&request,
                                  wh::core::run_context &callback_context) const -> auto
    requires detail::async_invoke_capable<impl_t>
  {
    return invoke_async_impl(std::forward<request_t>(request),
                             detail::make_callback_sink(callback_context));
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, tool_request>
  [[nodiscard]] auto async_stream(request_t &&request,
                                  wh::core::run_context &callback_context) const -> auto
    requires detail::async_stream_capable<impl_t>
  {
    return stream_async_impl(std::forward<request_t>(request),
                             detail::make_callback_sink(callback_context));
  }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, tool_request> &&
             detail::sync_invoke_handler<impl_t>
  [[nodiscard]] auto invoke_sync_impl(request_t &&request, detail::callback_sink sink) const
      -> tool_invoke_result {
    auto prepared = detail::prepare_tool_run<tool_invoke_result>(
        tool_request{request.input_json, request.options}, std::move(sink), schema_,
        default_options_);
    if (prepared.has_error()) {
      return tool_invoke_result::failure(prepared.error());
    }
    return detail::run_sync_tool_loop(std::move(prepared).value(),
                                      [this](tool_request &prepared_request) {
                                        return detail::run_sync_invoke(impl_, prepared_request);
                                      });
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, tool_request> &&
             detail::sync_stream_handler<impl_t>
  [[nodiscard]] auto stream_sync_impl(request_t &&request, detail::callback_sink sink) const
      -> tool_output_stream_result {
    auto prepared = detail::prepare_tool_run<tool_output_stream_result>(
        tool_request{request.input_json, request.options}, std::move(sink), schema_,
        default_options_);
    if (prepared.has_error()) {
      return tool_output_stream_result::failure(prepared.error());
    }
    return detail::run_sync_tool_loop(std::move(prepared).value(),
                                      [this](tool_request &prepared_request) {
                                        return detail::run_sync_stream(impl_, prepared_request);
                                      });
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, tool_request> &&
             detail::async_invoke_capable<impl_t>
  [[nodiscard]] auto invoke_async_impl(request_t &&request, detail::callback_sink sink) const
      -> auto {
    return wh::core::detail::defer_resume_sender<Resume>(
        [this, request = tool_request{request.input_json, request.options},
         sink = std::move(sink)](auto scheduler) mutable {
          auto prepared = detail::prepare_tool_run<tool_invoke_result>(
              std::move(request), std::move(sink), schema_, default_options_);
          return detail::make_tool_attempt_loop_sender<tool_invoke_result>(
              std::move(prepared),
              [this, scheduler = std::move(scheduler)](auto &loop_state) mutable {
                return wh::core::detail::resume_if<Resume>(
                    detail::make_invoke_sender(impl_, loop_state.request), scheduler);
              });
        });
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, tool_request> &&
             detail::async_stream_capable<impl_t>
  [[nodiscard]] auto stream_async_impl(request_t &&request, detail::callback_sink sink) const
      -> auto {
    return wh::core::detail::defer_resume_sender<Resume>(
        [this, request = tool_request{request.input_json, request.options},
         sink = std::move(sink)](auto scheduler) mutable {
          auto prepared = detail::prepare_tool_run<tool_output_stream_result>(
              std::move(request), std::move(sink), schema_, default_options_);
          return detail::make_tool_attempt_loop_sender<tool_output_stream_result>(
              std::move(prepared),
              [this, scheduler = std::move(scheduler)](auto &loop_state) mutable {
                return wh::core::detail::resume_if<Resume>(
                    detail::make_stream_sender(impl_, loop_state.request), scheduler);
              });
        });
  }

  wh::schema::tool_schema_definition schema_{};
  wh_no_unique_address impl_t impl_{};
  tool_options default_options_{};
};

template <typename schema_t, typename impl_t>
tool(schema_t &&, impl_t &&) -> tool<std::remove_cvref_t<impl_t>>;

template <typename schema_t, typename impl_t, typename options_t>
tool(schema_t &&, impl_t &&, options_t &&) -> tool<std::remove_cvref_t<impl_t>>;

} // namespace wh::tool
