// Defines the flow-level ReAct facade that reuses the ADK ReAct lowering and
// shared message reader/result bridges.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/adk/prebuilt/react.hpp"
#include "wh/adk/types.hpp"
#include "wh/adk/utils.hpp"
#include "wh/agent/toolset.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/flow/agent/react/callback.hpp"
#include "wh/flow/agent/react/option.hpp"
#include "wh/flow/agent/utils.hpp"
#include "wh/model/chat_model.hpp"

namespace wh::flow::agent::react {

namespace detail {

struct stored_tool {
  wh::schema::tool_schema_definition schema{};
  wh::compose::tool_entry entry{};
  wh::agent::tool_registration registration{};
};

[[nodiscard]] inline auto prepare_request(
    wh::model::chat_request request, const react_options &options)
    -> wh::core::result<wh::model::chat_request> {
  if (static_cast<bool>(options.rewrite_messages)) {
    auto rewritten = options.rewrite_messages(request.messages);
    if (rewritten.has_error()) {
      return wh::core::result<wh::model::chat_request>::failure(rewritten.error());
    }
    request.messages = std::move(rewritten).value();
  }
  if (static_cast<bool>(options.modify_messages)) {
    auto modified = options.modify_messages(request.messages);
    if (modified.has_error()) {
      return wh::core::result<wh::model::chat_request>::failure(modified.error());
    }
  }
  return request;
}

inline auto emit_callbacks(const std::vector<wh::flow::agent::message_reader_item> &items,
                           const react_options &options,
                           const react_callbacks &callbacks) -> void {
  for (const auto &item : items) {
    if (item.message.has_value() && static_cast<bool>(callbacks.on_message)) {
      callbacks.on_message(options.model_node_name, *item.message);
    }
    if (item.error.has_value() && static_cast<bool>(callbacks.on_error)) {
      callbacks.on_error(options.model_node_name, *item.error);
    }
  }
}

inline auto append_event_items(std::vector<wh::flow::agent::message_reader_item> &items,
                               wh::adk::agent_event_stream_reader reader) -> void {
  auto collected = wh::adk::collect_agent_events(std::move(reader));
  if (collected.has_error()) {
    items.push_back(wh::flow::agent::message_reader_item{.error = collected.error()});
    return;
  }
  for (auto &event : collected.value()) {
    if (auto *message = std::get_if<wh::adk::message_event>(&event.payload);
        message != nullptr) {
      auto snapshots = wh::adk::snapshot_message_event(std::move(*message));
      if (snapshots.has_error()) {
        items.push_back(
            wh::flow::agent::message_reader_item{.error = snapshots.error()});
        continue;
      }
      for (auto &entry : snapshots.value()) {
        items.push_back(
            wh::flow::agent::message_reader_item{.message = std::move(entry)});
      }
      continue;
    }
    if (auto *error = std::get_if<wh::adk::error_event>(&event.payload);
        error != nullptr) {
      items.push_back(
          wh::flow::agent::message_reader_item{.error = error->code});
    }
  }
}

} // namespace detail

/// Flow-level ReAct facade that rebuilds one ADK ReAct lowering per run so
/// runtime overrides can stay explicit and typed.
template <wh::model::chat_model_like model_t>
class react_flow {
public:
  /// Creates one flow-level ReAct shell from the required metadata and model.
  react_flow(std::string name, std::string description, model_t model) noexcept
      : name_(std::move(name)), description_(std::move(description)),
        model_(std::move(model)) {}

  react_flow(const react_flow &) = delete;
  auto operator=(const react_flow &) -> react_flow & = delete;
  react_flow(react_flow &&) noexcept = default;
  auto operator=(react_flow &&) noexcept -> react_flow & = default;
  ~react_flow() = default;

  /// Returns the exported graph name.
  [[nodiscard]] auto graph_name() const noexcept -> std::string_view {
    return options_.graph_name;
  }

  /// Returns the exported model-node name.
  [[nodiscard]] auto model_node_name() const noexcept -> std::string_view {
    return options_.model_node_name;
  }

  /// Returns the exported tools-node name.
  [[nodiscard]] auto tools_node_name() const noexcept -> std::string_view {
    return options_.tools_node_name;
  }

  /// Replaces the frozen authoring options.
  auto set_options(react_options options) -> wh::core::result<void> {
    options_ = std::move(options);
    return {};
  }

  /// Replaces the exported callbacks.
  auto set_callbacks(react_callbacks callbacks) -> wh::core::result<void> {
    callbacks_ = std::move(callbacks);
    return {};
  }

  /// Registers one executable tool component.
  template <wh::agent::detail::registered_tool_component tool_t>
  auto add_tool(const tool_t &tool,
                const wh::agent::tool_registration registration = {})
      -> wh::core::result<void> {
    tools_.push_back(detail::stored_tool{
        .schema = tool.schema(),
        .entry = wh::agent::detail::make_tool_entry(tool, registration.return_direct),
        .registration = registration,
    });
    return {};
  }

  /// Registers one raw compose tool entry.
  auto add_tool_entry(wh::schema::tool_schema_definition schema,
                      wh::compose::tool_entry entry,
                      const wh::agent::tool_registration registration = {})
      -> wh::core::result<void> {
    tools_.push_back(detail::stored_tool{
        .schema = std::move(schema),
        .entry = std::move(entry),
        .registration = registration,
    });
    return {};
  }

  /// Appends one tool middleware layer.
  auto add_tool_middleware(wh::compose::tool_middleware middleware)
      -> wh::core::result<void> {
    tool_middlewares_.push_back(std::move(middleware));
    return {};
  }

  /// Appends one ADK model middleware layer.
  auto add_middleware(wh::adk::chat_model_agent_middleware middleware)
      -> wh::core::result<void> {
    middlewares_.push_back(std::move(middleware));
    return {};
  }

  /// Invokes the flow and returns the terminal message result.
  auto invoke(const wh::model::chat_request &request, wh::core::run_context &context,
              const react_run_options &run_options = {}) const
      -> auto {
    return execute_invoke(request, context, run_options);
  }

  /// Starts the flow and returns the visible message reader view.
  auto stream(const wh::model::chat_request &request, wh::core::run_context &context,
              const react_run_options &run_options = {}) const
      -> auto {
    return execute_stream(request, context, run_options);
  }

private:
  struct runtime_state {
    /// Wrapped ADK ReAct agent that must outlive the sender chain.
    wh::adk::prebuilt::react<model_t> agent;

    runtime_state(std::string name, std::string description, const model_t &model) noexcept
        : agent(std::move(name), std::move(description), model) {}
  };

  template <typename sender_t>
  [[nodiscard]] static auto
  map_invoke_sender(std::unique_ptr<runtime_state> runtime_holder, sender_t &&sender) {
    using message_result = wh::flow::agent::message_result;
    return wh::core::detail::normalize_result_sender<message_result>(
        wh::core::detail::map_result_sender<message_result>(
            std::forward<sender_t>(sender),
            [runtime_holder = std::move(runtime_holder)](
                wh::adk::chat_model_agent_result result) -> message_result {
              (void)runtime_holder;
              if (result.report.final_error.has_value()) {
                return message_result::failure(*result.report.final_error);
              }
              if (!result.report.final_message.has_value()) {
                return message_result::failure(wh::core::errc::not_found);
              }
              return *result.report.final_message;
            }));
  }

  template <typename sender_t>
  [[nodiscard]] static auto map_stream_sender(std::unique_ptr<runtime_state> runtime_holder,
                                              sender_t &&sender,
                                              react_options options,
                                              react_callbacks callbacks) {
    using message_reader_result = wh::core::result<wh::flow::agent::message_reader>;
    return wh::core::detail::normalize_result_sender<message_reader_result>(
        wh::core::detail::map_result_sender<message_reader_result>(
            std::forward<sender_t>(sender),
            [options = std::move(options), callbacks = std::move(callbacks),
             runtime_holder = std::move(runtime_holder)](
                wh::adk::chat_model_agent_result result)
                -> message_reader_result {
              (void)runtime_holder;
              std::vector<wh::flow::agent::message_reader_item> items{};
              detail::append_event_items(items, std::move(result.events));
              detail::emit_callbacks(items, options, callbacks);
              return wh::flow::agent::make_message_reader(std::move(items));
            }));
  }

  auto configure_agent(wh::adk::prebuilt::react<model_t> &agent,
                       const react_run_options &run_options,
                       const bool emit_internal_messages) const
      -> wh::core::result<void> {
    auto max_iterations = agent.set_max_iterations(options_.max_iterations);
    if (max_iterations.has_error()) {
      return wh::core::result<void>::failure(max_iterations.error());
    }
    auto emit = agent.set_emit_internal_events(options_.emit_internal_messages ||
                                               emit_internal_messages);
    if (emit.has_error()) {
      return wh::core::result<void>::failure(emit.error());
    }
    if (!options_.output_key.empty()) {
      auto output_key = agent.set_output_key(options_.output_key);
      if (output_key.has_error()) {
        return wh::core::result<void>::failure(output_key.error());
      }
      auto output_mode = agent.set_output_mode(options_.output_mode);
      if (output_mode.has_error()) {
        return wh::core::result<void>::failure(output_mode.error());
      }
    }
    for (const auto &middleware : middlewares_) {
      auto added = agent.add_middleware(middleware);
      if (added.has_error()) {
        return wh::core::result<void>::failure(added.error());
      }
    }
    for (const auto &middleware : tool_middlewares_) {
      auto added = agent.add_tool_middleware(middleware);
      if (added.has_error()) {
        return wh::core::result<void>::failure(added.error());
      }
    }
    for (const auto &tool : tools_) {
      auto registration = tool.registration;
      if (run_options.return_direct_tool_names.contains(tool.schema.name)) {
        registration.return_direct = true;
      }
      auto added =
          agent.add_tool_entry(tool.schema, tool.entry, registration);
      if (added.has_error()) {
        return wh::core::result<void>::failure(added.error());
      }
    }
    return {};
  }

  [[nodiscard]] auto execute_invoke(wh::model::chat_request request,
                                    wh::core::run_context &context,
                                    react_run_options run_options) const {
    using message_result = wh::flow::agent::message_result;
    using failure_sender_t = decltype(
        wh::core::detail::failure_result_sender<message_result>(
            wh::core::errc::internal_error));
    using run_sender_t = decltype(std::declval<wh::adk::prebuilt::react<model_t> &>().run(
        std::declval<wh::model::chat_request>(), std::declval<wh::core::run_context &>()));
    using mapped_sender_t =
        decltype(map_invoke_sender(std::declval<std::unique_ptr<runtime_state>>(),
                                   std::declval<run_sender_t>()));
    using dispatch_sender_t =
        wh::core::detail::variant_sender<failure_sender_t, mapped_sender_t>;

    return wh::core::detail::defer_result_sender<message_result>(
        [this, request = std::move(request), run_options = std::move(run_options),
         &context]() mutable -> dispatch_sender_t {
          auto prepared = detail::prepare_request(std::move(request), options_);
          if (prepared.has_error()) {
            return dispatch_sender_t{
                wh::core::detail::failure_result_sender<message_result>(
                    prepared.error())};
          }

          auto runtime_holder = std::make_unique<runtime_state>(
              name_, description_, model_);
          auto configured =
              configure_agent(runtime_holder->agent, run_options, false);
          if (configured.has_error()) {
            return dispatch_sender_t{
                wh::core::detail::failure_result_sender<message_result>(
                    configured.error())};
          }

          auto agent_sender =
              runtime_holder->agent.run(std::move(prepared).value(), context);
          return dispatch_sender_t{
              map_invoke_sender(std::move(runtime_holder), std::move(agent_sender))};
        });
  }

  [[nodiscard]] auto execute_stream(wh::model::chat_request request,
                                    wh::core::run_context &context,
                                    react_run_options run_options) const {
    using message_reader_result = wh::core::result<wh::flow::agent::message_reader>;
    using failure_sender_t = decltype(
        wh::core::detail::failure_result_sender<message_reader_result>(
            wh::core::errc::internal_error));
    using run_sender_t = decltype(std::declval<wh::adk::prebuilt::react<model_t> &>().run(
        std::declval<wh::model::chat_request>(), std::declval<wh::core::run_context &>()));
    using mapped_sender_t =
        decltype(map_stream_sender(std::declval<std::unique_ptr<runtime_state>>(),
                                   std::declval<run_sender_t>(),
                                   std::declval<react_options>(),
                                   std::declval<react_callbacks>()));
    using dispatch_sender_t =
        wh::core::detail::variant_sender<failure_sender_t, mapped_sender_t>;

    return wh::core::detail::defer_result_sender<message_reader_result>(
        [this, request = std::move(request), run_options = std::move(run_options),
         &context]() mutable -> dispatch_sender_t {
          auto prepared = detail::prepare_request(std::move(request), options_);
          if (prepared.has_error()) {
            return dispatch_sender_t{
                wh::core::detail::failure_result_sender<message_reader_result>(
                    prepared.error())};
          }

          auto runtime_holder = std::make_unique<runtime_state>(
              name_, description_, model_);
          auto configured =
              configure_agent(runtime_holder->agent, run_options, true);
          if (configured.has_error()) {
            return dispatch_sender_t{
                wh::core::detail::failure_result_sender<message_reader_result>(
                    configured.error())};
          }

          auto agent_sender =
              runtime_holder->agent.run(std::move(prepared).value(), context);
          return dispatch_sender_t{map_stream_sender(std::move(runtime_holder),
                                                     std::move(agent_sender),
                                                     options_, callbacks_)};
        });
  }

  /// Stable flow name.
  std::string name_{};
  /// Human-readable flow description.
  std::string description_{};
  /// Stored model used to rebuild one ADK ReAct shell per run.
  model_t model_;
  /// Frozen authoring options.
  react_options options_{};
  /// Optional exported callbacks.
  react_callbacks callbacks_{};
  /// Stored tool bindings re-applied on each run.
  std::vector<detail::stored_tool> tools_{};
  /// Stored tool middlewares re-applied on each run.
  std::vector<wh::compose::tool_middleware> tool_middlewares_{};
  /// Stored ADK model middlewares re-applied on each run.
  std::vector<wh::adk::chat_model_agent_middleware> middlewares_{};
};

} // namespace wh::flow::agent::react
