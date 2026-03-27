// Implements a deterministic chat model that echoes user text back as the
// assistant response across both invoke and stream execution paths.
#pragma once

#include <ranges>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/stream.hpp"

namespace wh::model {

namespace detail {

/// Returns the first text part from a message when present.
[[nodiscard]] inline auto first_text_part(const wh::schema::message &message)
    -> std::string {
  const auto iter = std::ranges::find_if(
      message.parts, [](const wh::schema::message_part &part) noexcept {
        return std::holds_alternative<wh::schema::text_part>(part);
      });
  if (iter == message.parts.end()) {
    return {};
  }
  return std::get<wh::schema::text_part>(*iter).text;
}

/// Builds a deterministic echo-model response from request input messages.
[[nodiscard]] inline auto make_echo_response(const chat_request &request)
    -> wh::core::result<chat_response> {
  if (request.messages.empty()) {
    return wh::core::result<chat_response>::failure(
        wh::core::errc::invalid_argument);
  }

  const auto &last = request.messages.back();
  wh::schema::message response_message{};
  response_message.role = wh::schema::message_role::assistant;
  response_message.parts.emplace_back(
      wh::schema::text_part{first_text_part(last)});
  response_message.meta.usage.prompt_tokens = 1;
  response_message.meta.usage.completion_tokens = 1;
  response_message.meta.usage.total_tokens = 2;

  return chat_response{response_message, response_message.meta};
}

class echo_chat_model_impl {
public:
  echo_chat_model_impl() = default;
  explicit echo_chat_model_impl(const chat_model_options &options)
      : options_(options) {}
  explicit echo_chat_model_impl(chat_model_options &&options)
      : options_(std::move(options)) {}

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{
        "EchoChatModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const chat_request &request) const
      -> wh::core::result<chat_response> {
    return make_echo_response(request);
  }

  [[nodiscard]] auto invoke(chat_request &&request) const
      -> wh::core::result<chat_response> {
    return invoke(static_cast<const chat_request &>(request));
  }

  [[nodiscard]] auto stream(const chat_request &request) const
      -> wh::core::result<chat_stream_reader> {
    auto response = make_echo_response(request);
    if (response.has_error()) {
      return wh::core::result<chat_stream_reader>::failure(response.error());
    }

    auto [writer, reader] =
        wh::schema::stream::make_pipe_stream<wh::schema::message>(8U);
    auto write_status = writer.try_write(std::move(response).value().message);
    if (write_status.has_error()) {
      return wh::core::result<chat_stream_reader>::failure(write_status.error());
    }
    auto close_status = writer.close();
    if (close_status.has_error()) {
      return wh::core::result<chat_stream_reader>::failure(close_status.error());
    }
    return chat_stream_reader{std::move(reader)};
  }

  [[nodiscard]] auto stream(chat_request &&request) const
      -> wh::core::result<chat_stream_reader> {
    return stream(static_cast<const chat_request &>(request));
  }

  [[nodiscard]] auto bind_tools(
      const std::span<const wh::schema::tool_schema_definition> tools) const
      -> echo_chat_model_impl {
    echo_chat_model_impl next{*this};
    next.bound_tools_.assign(tools.begin(), tools.end());
    return next;
  }

  auto finish_stream_event(const chat_request &,
                           chat_model_callback_event &event) const -> void {
    event.emitted_chunks = 1U;
    event.usage.prompt_tokens = 1U;
    event.usage.completion_tokens = 1U;
    event.usage.total_tokens = 2U;
  }

  [[nodiscard]] auto options() const noexcept -> const chat_model_options & {
    return options_;
  }

  [[nodiscard]] auto bound_tools() const noexcept
      -> const std::vector<wh::schema::tool_schema_definition> & {
    return bound_tools_;
  }

private:
  chat_model_options options_{};
  std::vector<wh::schema::tool_schema_definition> bound_tools_{};
};

} // namespace detail

using echo_chat_model = chat_model<detail::echo_chat_model_impl>;

} // namespace wh::model
