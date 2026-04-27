// Defines the internal chat graph lowerer that maps the public authored chat
// shell onto one compose chain without introducing a second runtime.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wh/adk/detail/agent_graph_view.hpp"
#include "wh/adk/deterministic_transfer.hpp"
#include "wh/agent/agent.hpp"
#include "wh/agent/chat.hpp"
#include "wh/compose/authored/chain.hpp"
#include "wh/compose/graph.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/message.hpp"

namespace wh::adk::detail {

namespace chat_detail {

inline constexpr std::string_view chat_model_messages_node_key = "__chat_model_messages__";

[[nodiscard]] inline auto make_instruction_message(const std::string_view description,
                                                   const std::string_view instruction)
    -> std::optional<wh::schema::message> {
  std::string text{};
  if (!description.empty()) {
    text.append(description);
  }
  if (!instruction.empty()) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    text.append(instruction);
  }
  if (text.empty()) {
    return std::nullopt;
  }

  wh::schema::message message{};
  message.role = wh::schema::message_role::system;
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

[[nodiscard]] inline auto render_message_text(const wh::schema::message &message) -> std::string {
  std::string text{};
  for (const auto &part : message.parts) {
    if (const auto *typed = std::get_if<wh::schema::text_part>(&part); typed != nullptr) {
      text.append(typed->text);
    }
  }
  return text;
}

inline auto write_output_value(wh::agent::agent_output &output, const std::string_view output_key,
                               const wh::agent::chat_output_mode output_mode,
                               const wh::schema::message &message) -> void {
  if (output_key.empty()) {
    return;
  }
  if (output_mode == wh::agent::chat_output_mode::value) {
    output.output_values.insert_or_assign(std::string{output_key}, wh::core::any{message});
    return;
  }
  output.output_values.insert_or_assign(std::string{output_key},
                                        wh::core::any{render_message_text(message)});
}

[[nodiscard]] inline auto make_agent_output(std::vector<wh::schema::message> messages,
                                            const std::string_view output_key,
                                            const wh::agent::chat_output_mode output_mode)
    -> wh::core::result<wh::agent::agent_output> {
  if (messages.empty()) {
    return wh::core::result<wh::agent::agent_output>::failure(wh::core::errc::not_found);
  }

  auto final_message = messages.back();
  wh::agent::agent_output output{
      .final_message = final_message,
      .history_messages = std::move(messages),
      .transfer = wh::adk::extract_transfer_from_message(final_message),
  };
  write_output_value(output, output_key, output_mode, output.final_message);
  return output;
}

} // namespace chat_detail

/// Internal chat lowering shell that produces one real compose graph.
class chat_graph {
public:
  explicit chat_graph(const wh::agent::chat &authored) noexcept
      : authored_(std::addressof(authored)) {}

  [[nodiscard]] auto lower() const -> wh::core::result<wh::compose::graph> {
    return lower_native();
  }

private:
  [[nodiscard]] auto lower_native() const -> wh::core::result<wh::compose::graph> {
    if (authored_ == nullptr) {
      return wh::core::result<wh::compose::graph>::failure(wh::core::errc::invalid_argument);
    }

    auto model_binding = authored_->model_binding();
    if (model_binding.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_binding.error());
    }
    auto model_node =
        model_binding.value().get().materialize(std::string{wh::agent::chat_model_node_key});
    if (model_node.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_node.error());
    }

    auto description = std::string{authored_->description()};
    auto instruction = authored_->render_instruction();
    wh::compose::graph_compile_options compile_options{};
    compile_options.name = std::string{authored_->name()};
    compile_options.boundary.output = wh::compose::node_contract::stream;
    wh::compose::chain lowered{std::move(compile_options)};

    auto prepare_request = wh::compose::make_lambda_node(
        "prepare_request",
        [description = std::move(description), instruction = std::move(instruction)](
            wh::compose::graph_value &input, wh::core::run_context &,
            const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto *messages = wh::core::any_cast<std::vector<wh::schema::message>>(&input);
          if (messages == nullptr) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::type_mismatch);
          }

          wh::model::chat_request request{};
          request.messages.reserve(messages->size() + 1U);
          if (auto system = chat_detail::make_instruction_message(description, instruction);
              system.has_value()) {
            request.messages.push_back(std::move(*system));
          }
          for (auto &message : *messages) {
            request.messages.push_back(std::move(message));
          }
          return wh::compose::graph_value{std::move(request)};
        });
    auto prepare_added = lowered.append(std::move(prepare_request));
    if (prepare_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(prepare_added.error());
    }

    auto model_added = lowered.append(std::move(model_node).value());
    if (model_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_added.error());
    }

    if (model_binding.value().get().output_contract() == wh::compose::node_contract::value) {
      auto project_model_output = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                                                wh::compose::node_contract::stream>(
          std::string{chat_detail::chat_model_messages_node_key},
          [](wh::compose::graph_value &input, wh::core::run_context &,
             const wh::compose::graph_call_scope &)
              -> wh::core::result<wh::compose::graph_stream_reader> {
            return wh::adk::detail::make_message_stream_from_value_payload(input);
          });
      auto project_added = lowered.append(std::move(project_model_output));
      if (project_added.has_error()) {
        return wh::core::result<wh::compose::graph>::failure(project_added.error());
      }
    }

    auto compiled = lowered.compile();
    if (compiled.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(compiled.error());
    }

    return std::move(lowered).release_graph();
  }
  const wh::agent::chat *authored_{nullptr};
};

/// Wraps one frozen chat shell as the common executable agent surface.
[[nodiscard]] inline auto bind_chat_agent(wh::agent::chat authored)
    -> wh::core::result<wh::agent::agent> {
  if (!authored.frozen()) {
    return wh::core::result<wh::agent::agent>::failure(wh::core::errc::contract_violation);
  }

  wh::agent::agent exported{std::string{authored.name()}};
  auto described = exported.set_description(std::string{authored.description()});
  if (described.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(described.error());
  }

  auto shell = std::make_unique<wh::agent::chat>(std::move(authored));
  auto bound = exported.bind_execution(
      nullptr, [shell = std::move(shell)]() mutable -> wh::core::result<wh::compose::graph> {
        return chat_graph{*shell}.lower();
      });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  auto exported_frozen = exported.freeze();
  if (exported_frozen.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(exported_frozen.error());
  }
  return exported;
}

} // namespace wh::adk::detail
