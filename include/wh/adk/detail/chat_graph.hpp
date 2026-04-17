// Defines the internal chat graph lowerer that maps the public authored chat
// shell onto one compose chain without introducing a second runtime.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

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

[[nodiscard]] inline auto read_model_messages(
    wh::compose::graph_stream_reader reader)
    -> wh::core::result<std::vector<wh::schema::message>> {
  auto values = wh::compose::collect_graph_stream_reader(std::move(reader));
  if (values.has_error()) {
    return wh::core::result<std::vector<wh::schema::message>>::failure(
        values.error());
  }

  std::vector<wh::schema::message> messages{};
  messages.reserve(values.value().size());
  for (auto &value : values.value()) {
    auto *typed = wh::core::any_cast<wh::schema::message>(&value);
    if (typed == nullptr) {
      return wh::core::result<std::vector<wh::schema::message>>::failure(
          wh::core::errc::type_mismatch);
    }
    messages.push_back(std::move(*typed));
  }
  return messages;
}

[[nodiscard]] inline auto make_instruction_message(
    const std::string_view description, const std::string_view instruction)
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

[[nodiscard]] inline auto render_message_text(const wh::schema::message &message)
    -> std::string {
  std::string text{};
  for (const auto &part : message.parts) {
    if (const auto *typed = std::get_if<wh::schema::text_part>(&part);
        typed != nullptr) {
      text.append(typed->text);
    }
  }
  return text;
}

inline auto write_output_value(wh::agent::agent_output &output,
                               const std::string_view output_key,
                               const wh::agent::chat_output_mode output_mode,
                               const wh::schema::message &message) -> void {
  if (output_key.empty()) {
    return;
  }
  if (output_mode == wh::agent::chat_output_mode::value) {
    output.output_values.insert_or_assign(std::string{output_key},
                                          wh::core::any{message});
    return;
  }
  output.output_values.insert_or_assign(std::string{output_key},
                                        wh::core::any{render_message_text(message)});
}

} // namespace chat_detail

/// Internal chat lowering shell that produces one real compose graph.
class chat_graph {
public:
  explicit chat_graph(const wh::agent::chat &authored) noexcept
      : authored_(std::addressof(authored)) {}

  [[nodiscard]] auto lower() const -> wh::core::result<wh::compose::graph> {
    if (authored_ == nullptr) {
      return wh::core::result<wh::compose::graph>::failure(
          wh::core::errc::invalid_argument);
    }

    auto model_node = authored_->model_node();
    if (model_node.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_node.error());
    }

    auto description = std::string{authored_->description()};
    auto instruction = authored_->render_instruction();
    auto output_key = std::string{authored_->output_key()};
    const auto output_mode = authored_->output_mode();

    wh::compose::graph_compile_options compile_options{};
    compile_options.name = std::string{authored_->name()};
    wh::compose::chain lowered{std::move(compile_options)};

    auto prepare_request = wh::compose::make_lambda_node(
        "prepare_request",
        [description = std::move(description),
         instruction = std::move(instruction)](
            wh::compose::graph_value &input, wh::core::run_context &,
            const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          auto *messages =
              wh::core::any_cast<std::vector<wh::schema::message>>(&input);
          if (messages == nullptr) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::type_mismatch);
          }

          wh::model::chat_request request{};
          request.messages.reserve(messages->size() + 1U);
          if (auto system =
                  chat_detail::make_instruction_message(description, instruction);
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

    auto model_added = lowered.append(model_node.value().get());
    if (model_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(model_added.error());
    }

    auto finalize = wh::compose::make_lambda_node<
        wh::compose::node_contract::stream, wh::compose::node_contract::value>(
        "finalize",
        [output_key = std::move(output_key),
         output_mode](wh::compose::graph_stream_reader reader,
                      wh::core::run_context &,
                      const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          auto messages = chat_detail::read_model_messages(std::move(reader));
          if (messages.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                messages.error());
          }
          if (messages.value().empty()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                wh::core::errc::not_found);
          }

          auto output_messages = std::move(messages).value();
          auto final_message = output_messages.back();
          wh::agent::agent_output output{
              .final_message = final_message,
              .history_messages = std::move(output_messages),
              .transfer =
                  wh::adk::extract_transfer_from_message(final_message),
          };
          chat_detail::write_output_value(output, output_key, output_mode,
                                          output.final_message);
          return wh::compose::graph_value{std::move(output)};
        });
    auto finalize_added = lowered.append(std::move(finalize));
    if (finalize_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(finalize_added.error());
    }

    auto compiled = lowered.compile();
    if (compiled.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(compiled.error());
    }

    return std::move(lowered).release_graph();
  }

private:
  const wh::agent::chat *authored_{nullptr};
};

/// Wraps one frozen chat shell as the common executable agent surface.
[[nodiscard]] inline auto bind_chat_agent(wh::agent::chat authored)
    -> wh::core::result<wh::agent::agent> {
  if (!authored.frozen()) {
    return wh::core::result<wh::agent::agent>::failure(
        wh::core::errc::contract_violation);
  }

  wh::agent::agent exported{std::string{authored.name()}};
  auto described = exported.set_description(std::string{authored.description()});
  if (described.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(described.error());
  }

  auto shell = std::make_unique<wh::agent::chat>(std::move(authored));
  auto bound = exported.bind_execution(
      nullptr,
      [shell = std::move(shell)]() mutable
          -> wh::core::result<wh::compose::graph> {
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
