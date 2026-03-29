// Defines the multi-agent host flow facade that reuses existing chat-model and
// flow message bridges.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/flow/agent/multiagent/host/callback.hpp"
#include "wh/flow/agent/multiagent/host/options.hpp"
#include "wh/flow/agent/multiagent/host/types.hpp"
#include "wh/flow/agent/utils.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/message.hpp"

namespace wh::flow::agent::multiagent::host {

namespace detail {

using invoke_specialist = wh::core::callback_function<
    wh::flow::agent::message_result(const wh::model::chat_request &,
                                    wh::core::run_context &) const>;
using stream_specialist = wh::core::callback_function<
    wh::core::result<wh::flow::agent::message_reader>(
        const wh::model::chat_request &, wh::core::run_context &) const>;
using summarize_callback = wh::core::callback_function<
    wh::flow::agent::message_result(
        const std::vector<specialist_result> &, wh::core::run_context &) const>;

struct specialist_entry {
  std::string name{};
  std::string description{};
  specialist_kind kind{specialist_kind::value};
  invoke_specialist invoke{nullptr};
  stream_specialist stream{nullptr};
};

[[nodiscard]] inline auto render_message_text(const wh::schema::message &message)
    -> std::string {
  return wh::flow::agent::render_message_text(message);
}

[[nodiscard]] inline auto parse_specialist_names(
    const wh::schema::message &message) -> std::unordered_set<std::string,
                                                               wh::core::transparent_string_hash,
                                                               wh::core::transparent_string_equal> {
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      names{};
  for (const auto &part : message.parts) {
    const auto *tool = std::get_if<wh::schema::tool_call_part>(&part);
    if (tool == nullptr || tool->name.empty()) {
      continue;
    }
    names.insert(tool->name);
  }
  return names;
}

[[nodiscard]] inline auto default_summary(
    const std::vector<specialist_result> &results) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  std::string text{};
  for (std::size_t index = 0U; index < results.size(); ++index) {
    if (index != 0U) {
      text.push_back('\n');
    }
    text.append(results[index].specialist_name);
    text.append(": ");
    text.append(render_message_text(results[index].message));
  }
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

} // namespace detail

/// Multi-agent host flow that routes one host model output to zero or more
/// specialists and optionally one summarizer.
template <wh::model::chat_model_like model_t>
class host_flow {
public:
  /// Creates one host flow shell from the required name and host model.
  host_flow(std::string name, model_t host_model) noexcept
      : name_(std::move(name)), host_model_(std::move(host_model)) {}

  host_flow(const host_flow &) = delete;
  auto operator=(const host_flow &) -> host_flow & = delete;
  host_flow(host_flow &&) noexcept = default;
  auto operator=(host_flow &&) noexcept -> host_flow & = default;
  ~host_flow() = default;

  /// Replaces the frozen authoring options.
  auto set_options(host_options options) -> void { options_ = std::move(options); }

  /// Replaces the exported callbacks.
  auto set_callbacks(host_callbacks callbacks) -> void {
    callbacks_ = std::move(callbacks);
  }

  /// Returns the exported graph name.
  [[nodiscard]] auto graph_name() const noexcept -> std::string_view {
    return options_.graph_name;
  }

  /// Registers one model-backed specialist.
  template <wh::model::chat_model_like specialist_model_t>
  auto add_specialist_model(std::string name, std::string description,
                            specialist_model_t model) -> wh::core::result<void> {
    if (name.empty() || description.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    specialists_.push_back(detail::specialist_entry{
        .name = std::move(name),
        .description = std::move(description),
        .kind = specialist_kind::model,
        .invoke =
            [model = std::move(model)](const wh::model::chat_request &request,
                                       wh::core::run_context &context)
                -> wh::flow::agent::message_result {
          auto status = model.invoke(request, context);
          if (status.has_error()) {
            return wh::flow::agent::message_result::failure(status.error());
          }
          return std::move(status).value().message;
        },
    });
    return {};
  }

  /// Registers one single-message specialist callable.
  auto add_specialist_invoke(std::string name, std::string description,
                             detail::invoke_specialist invoke)
      -> wh::core::result<void> {
    if (name.empty() || description.empty() || !static_cast<bool>(invoke)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    specialists_.push_back(detail::specialist_entry{
        .name = std::move(name),
        .description = std::move(description),
        .kind = specialist_kind::value,
        .invoke = std::move(invoke),
    });
    return {};
  }

  /// Registers one streaming specialist callable.
  auto add_specialist_stream(std::string name, std::string description,
                             detail::stream_specialist stream)
      -> wh::core::result<void> {
    if (name.empty() || description.empty() || !static_cast<bool>(stream)) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    specialists_.push_back(detail::specialist_entry{
        .name = std::move(name),
        .description = std::move(description),
        .kind = specialist_kind::stream,
        .stream = std::move(stream),
    });
    return {};
  }

  /// Installs one optional summarizer callback.
  auto set_summarizer(detail::summarize_callback summarizer) -> void {
    summarizer_ = std::move(summarizer);
  }

  /// Validates host and specialist authoring state.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || specialists_.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    for (const auto &specialist : specialists_) {
      if (specialist.name.empty() || specialist.description.empty()) {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
      if (!static_cast<bool>(specialist.invoke) &&
          !static_cast<bool>(specialist.stream)) {
        return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
      }
    }
    frozen_ = true;
    return {};
  }

  /// Runs the host flow once and returns the final message plus one report.
  auto run(const wh::model::chat_request &request, wh::core::run_context &context)
      -> wh::core::result<host_result> {
    auto frozen = freeze();
    if (frozen.has_error()) {
      return wh::core::result<host_result>::failure(frozen.error());
    }

    auto host_status = host_model_.invoke(request, context);
    if (host_status.has_error()) {
      return wh::core::result<host_result>::failure(host_status.error());
    }
    const auto &host_message = host_status.value().message;
    auto selected_names = detail::parse_specialist_names(host_message);
    if (selected_names.empty()) {
      return host_result{
          .message = host_message,
          .report = host_report{.direct_answer = true},
      };
    }

    host_report report{};
    std::vector<specialist_result> collected{};
    for (const auto &specialist : specialists_) {
      if (!selected_names.contains(specialist.name)) {
        continue;
      }
      report.selected_specialists.push_back(specialist.name);
      if (static_cast<bool>(callbacks_.on_handoff)) {
        callbacks_.on_handoff(options_.host_node_name, specialist.name);
      }

      specialist_result result{};
      result.specialist_name = specialist.name;
      result.kind = specialist.kind;
      if (static_cast<bool>(specialist.invoke)) {
        auto invoked = specialist.invoke(request, context);
        if (invoked.has_error()) {
          return wh::core::result<host_result>::failure(invoked.error());
        }
        result.message = std::move(invoked).value();
      } else {
        auto streamed = specialist.stream(request, context);
        if (streamed.has_error()) {
          return wh::core::result<host_result>::failure(streamed.error());
        }
        auto message =
            wh::flow::agent::message_reader_to_result(std::move(streamed).value());
        if (message.has_error()) {
          return wh::core::result<host_result>::failure(message.error());
        }
        result.message = std::move(message).value();
      }
      collected.push_back(std::move(result));
    }

    if (collected.empty()) {
      return wh::core::result<host_result>::failure(wh::core::errc::not_found);
    }

    wh::schema::message final_message{};
    if (collected.size() == 1U) {
      final_message = collected.front().message;
    } else if (static_cast<bool>(summarizer_)) {
      auto summarized = summarizer_(collected, context);
      if (summarized.has_error()) {
        return wh::core::result<host_result>::failure(summarized.error());
      }
      final_message = std::move(summarized).value();
    } else {
      final_message = detail::default_summary(collected);
    }

    report.specialist_results = collected;
    return host_result{
        .message = std::move(final_message),
        .report = std::move(report),
    };
  }

private:
  /// Stable host flow name.
  std::string name_{};
  /// Host model used to route direct answers and specialist handoffs.
  model_t host_model_;
  /// Frozen authoring options.
  host_options options_{};
  /// Optional exported callbacks.
  host_callbacks callbacks_{};
  /// Registered specialist set in stable declaration order.
  std::vector<detail::specialist_entry> specialists_{};
  /// Optional summarizer callback.
  detail::summarize_callback summarizer_{nullptr};
  /// True after authoring validation succeeds.
  bool frozen_{false};
};

} // namespace wh::flow::agent::multiagent::host
