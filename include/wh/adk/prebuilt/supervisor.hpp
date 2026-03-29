// Defines the prebuilt supervisor scenario wrapper that reuses the existing
// host-flow lowering instead of introducing a second orchestration runtime.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/adk/event_stream.hpp"
#include "wh/adk/types.hpp"
#include "wh/core/any.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/flow/agent/multiagent/host/callback.hpp"
#include "wh/flow/agent/multiagent/host/compose.hpp"
#include "wh/flow/agent/multiagent/host/options.hpp"
#include "wh/flow/agent/multiagent/host/types.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/stream/reader.hpp"

namespace wh::adk::prebuilt {

/// Frozen authoring options for one supervisor wrapper.
struct supervisor_options {
  /// Stable exported graph name.
  std::string graph_name{"supervisor_graph"};
  /// Stable host routing node name.
  std::string host_node_name{"supervisor_host"};
  /// Stable summarize node name.
  std::string summarize_node_name{"supervisor_summarize"};
};

/// Optional callbacks emitted by one supervisor wrapper.
struct supervisor_callbacks {
  /// Called only when the host route hands off to one worker.
  wh::core::callback_function<void(std::string_view, std::string_view) const>
      on_handoff{nullptr};
};

/// Final result returned after one supervisor run.
struct supervisor_result {
  /// Event stream emitted by the wrapped host flow.
  agent_event_stream_reader events{};
  /// Final output message when the run completed successfully.
  std::optional<wh::schema::message> final_message{};
  /// Immutable host-flow report for diagnostics and tests.
  wh::flow::agent::multiagent::host::host_report report{};
};

namespace detail {

inline auto append_supervisor_handoff_event(
    std::vector<agent_event> &events, const std::string_view supervisor_name,
    const std::string_view worker_name) -> void {
  event_metadata metadata{};
  metadata.run_path =
      run_path{{"agent", supervisor_name, "specialist", worker_name}};
  metadata.agent_name = std::string{supervisor_name};
  metadata.tool_name = std::string{worker_name};
  metadata.attributes.insert_or_assign("sequence_id", wh::core::any{events.size()});
  events.push_back(make_custom_event("supervisor_handoff",
                                     wh::core::any{std::string{worker_name}},
                                     std::move(metadata)));
}

inline auto append_supervisor_final_message_event(
    std::vector<agent_event> &events, const std::string_view supervisor_name,
    wh::schema::message message) -> void {
  event_metadata metadata{};
  metadata.run_path = run_path{{"agent", supervisor_name, "final"}};
  metadata.agent_name = std::string{supervisor_name};
  metadata.attributes.insert_or_assign("sequence_id", wh::core::any{events.size()});
  events.push_back(make_message_event(std::move(message), std::move(metadata)));
}

[[nodiscard]] inline auto
make_supervisor_event_reader(std::vector<agent_event> events)
    -> agent_event_stream_reader {
  return agent_event_stream_reader{
      wh::schema::stream::make_values_stream_reader(std::move(events))};
}

} // namespace detail

/// Prebuilt supervisor scenario wrapper that forwards authoring and runtime
/// execution to the shared multi-agent host flow.
template <wh::model::chat_model_like host_model_t>
class supervisor {
public:
  /// Creates one supervisor wrapper from the required name and host model.
  supervisor(std::string name, host_model_t host_model) noexcept
      : name_(std::move(name)), flow_(name_, std::move(host_model)) {
    sync_flow_options();
    sync_flow_callbacks();
  }

  supervisor(const supervisor &) = delete;
  auto operator=(const supervisor &) -> supervisor & = delete;
  supervisor(supervisor &&) noexcept = default;
  auto operator=(supervisor &&) noexcept -> supervisor & = default;
  ~supervisor() = default;

  /// Returns the stable supervisor name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns the exported graph name.
  [[nodiscard]] auto graph_name() const noexcept -> std::string_view {
    return options_.graph_name;
  }

  /// Returns the exported host node name.
  [[nodiscard]] auto host_node_name() const noexcept -> std::string_view {
    return options_.host_node_name;
  }

  /// Replaces the frozen authoring options.
  auto set_options(supervisor_options options) -> wh::core::result<void> {
    options_ = std::move(options);
    sync_flow_options();
    return {};
  }

  /// Replaces the exported callbacks.
  auto set_callbacks(supervisor_callbacks callbacks) -> wh::core::result<void> {
    callbacks_ = std::move(callbacks);
    sync_flow_callbacks();
    return {};
  }

  /// Registers one model-backed worker.
  template <wh::model::chat_model_like worker_model_t>
  auto add_worker_model(std::string name, std::string description,
                        worker_model_t model) -> wh::core::result<void> {
    return flow_.add_specialist_model(std::move(name), std::move(description),
                                      std::move(model));
  }

  /// Registers one single-message worker callable.
  auto add_worker_invoke(
      std::string name, std::string description,
      wh::flow::agent::multiagent::host::detail::invoke_specialist invoke)
      -> wh::core::result<void> {
    return flow_.add_specialist_invoke(std::move(name), std::move(description),
                                       std::move(invoke));
  }

  /// Registers one streaming worker callable.
  auto add_worker_stream(
      std::string name, std::string description,
      wh::flow::agent::multiagent::host::detail::stream_specialist stream)
      -> wh::core::result<void> {
    return flow_.add_specialist_stream(std::move(name), std::move(description),
                                       std::move(stream));
  }

  /// Installs one explicit summarizer callback.
  auto set_summarizer(
      wh::flow::agent::multiagent::host::detail::summarize_callback summarizer)
      -> void {
    flow_.set_summarizer(std::move(summarizer));
  }

  /// Validates the wrapped host flow authoring surface.
  auto freeze() -> wh::core::result<void> { return flow_.freeze(); }

  /// Runs the supervisor once and projects the host report into ADK events.
  auto run(const wh::model::chat_request &request, wh::core::run_context &context)
      -> wh::core::result<supervisor_result> {
    auto frozen = freeze();
    if (frozen.has_error()) {
      return wh::core::result<supervisor_result>::failure(frozen.error());
    }

    auto status = flow_.run(request, context);
    if (status.has_error()) {
      return wh::core::result<supervisor_result>::failure(status.error());
    }

    std::vector<agent_event> events{};
    for (const auto &worker_name : status.value().report.selected_specialists) {
      detail::append_supervisor_handoff_event(events, name_, worker_name);
    }
    detail::append_supervisor_final_message_event(events, name_,
                                                  status.value().message);
    return supervisor_result{
        .events = detail::make_supervisor_event_reader(std::move(events)),
        .final_message = status.value().message,
        .report = std::move(status).value().report,
    };
  }

private:
  auto sync_flow_options() -> void {
    flow_.set_options(wh::flow::agent::multiagent::host::host_options{
        .graph_name = options_.graph_name,
        .host_node_name = options_.host_node_name,
        .summarize_node_name = options_.summarize_node_name,
    });
  }

  auto sync_flow_callbacks() -> void {
    flow_.set_callbacks(wh::flow::agent::multiagent::host::host_callbacks{
        .on_handoff = callbacks_.on_handoff,
    });
  }

  /// Stable supervisor name.
  std::string name_{};
  /// Exported authoring options.
  supervisor_options options_{};
  /// Exported callbacks.
  supervisor_callbacks callbacks_{};
  /// Shared host-flow lowering reused by this wrapper.
  wh::flow::agent::multiagent::host::host_flow<host_model_t> flow_;
};

} // namespace wh::adk::prebuilt
