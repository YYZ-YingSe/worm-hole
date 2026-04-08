#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/any.hpp"
#include "wh/core/component.hpp"
#include "wh/core/run_context.hpp"
#include "wh/document/document.hpp"
#include "wh/embedding/embedding.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/model/fallback_chat_model.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/stream/core/types.hpp"
#include "wh/tool/tool.hpp"

namespace wh::testing::helper {

template <typename text_t>
  requires std::constructible_from<std::string, text_t &&>
[[nodiscard]] auto make_user_message(text_t &&text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(
      wh::schema::text_part{std::forward<text_t>(text)});
  return message;
}

template <typename text_t>
  requires std::constructible_from<std::string, text_t &&>
[[nodiscard]] auto make_assistant_message(text_t &&text)
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.parts.emplace_back(
      wh::schema::text_part{std::forward<text_t>(text)});
  return message;
}

template <typename timing_checker_t, typename callback_t, typename name_t>
  requires std::constructible_from<wh::core::callback_timing_checker,
                                   timing_checker_t &&> &&
           std::constructible_from<wh::core::stage_view_callback,
                                   callback_t &&> &&
           std::constructible_from<std::string, name_t &&>
[[nodiscard]] auto register_test_callbacks(
    wh::core::run_context &&context, timing_checker_t &&timing_checker,
    callback_t &&callback, name_t &&name = {})
    -> wh::core::result<wh::core::run_context> {
  wh::core::stage_view_callback stage_callback{
      std::forward<callback_t>(callback)};
  wh::core::stage_callbacks callbacks{};
  callbacks.on_start = stage_callback;
  callbacks.on_end = stage_callback;
  callbacks.on_error = stage_callback;
  callbacks.on_stream_start = stage_callback;
  callbacks.on_stream_end = std::move(stage_callback);
  return wh::core::register_local_callbacks(
      std::move(context), std::forward<timing_checker_t>(timing_checker),
      std::move(callbacks), std::string{std::forward<name_t>(name)});
}

template <typename reader_t>
[[nodiscard]] auto take_try_chunk(reader_t &reader)
    -> typename std::remove_cvref_t<reader_t>::chunk_result_type {
  auto next = reader.try_read();
  if (std::holds_alternative<wh::schema::stream::stream_signal>(next)) {
    return wh::core::result<
        typename std::remove_cvref_t<reader_t>::chunk_type>::failure(
        wh::core::errc::invalid_argument);
  }
  return std::move(std::get<
                   typename std::remove_cvref_t<reader_t>::chunk_result_type>(
      next));
}

template <typename value_t>
[[nodiscard]] auto read_any(const wh::core::any &value)
    -> wh::core::result<value_t> {
  if (const auto *typed = wh::core::any_cast<value_t>(&value);
      typed != nullptr) {
    return *typed;
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename value_t>
[[nodiscard]] auto read_any(wh::core::any &&value)
    -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

template <typename document_t>
concept document_async_available =
    requires(document_t &component,
             wh::document::document_request document_request_value,
             wh::core::run_context &run_context) {
      component.async_process(std::move(document_request_value), run_context);
    };

template <typename tool_t>
concept tool_async_invoke_available =
    requires(tool_t &tool, wh::tool::tool_request tool_request_value,
             wh::core::run_context &run_context) {
      tool.async_invoke(std::move(tool_request_value), run_context);
    };

struct tool_binding_probe_state {
  std::size_t bind_calls{0U};
  std::size_t invoke_calls{0U};
  std::size_t stream_calls{0U};
  std::size_t last_bound_tool_count{0U};
};

class tool_binding_probe_model_impl final {
public:
  explicit tool_binding_probe_model_impl(
      const std::shared_ptr<tool_binding_probe_state> &state,
      const std::size_t bound_tool_count = 0U)
      : state_(state), bound_tool_count_(bound_tool_count) {}

  explicit tool_binding_probe_model_impl(
      std::shared_ptr<tool_binding_probe_state> &&state,
      const std::size_t bound_tool_count = 0U)
      : state_(std::move(state)), bound_tool_count_(bound_tool_count) {}

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{
        "ToolBindingProbeModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &) const
      -> wh::core::result<wh::model::chat_response> {
    ++state_->invoke_calls;
    state_->last_bound_tool_count = bound_tool_count_;
    if (bound_tool_count_ == 0U) {
      return wh::core::result<wh::model::chat_response>::failure(
          wh::core::errc::invalid_argument);
    }

    wh::schema::message response{};
    response.role = wh::schema::message_role::assistant;
    response.parts.emplace_back(wh::schema::text_part{"bound"});
    return wh::model::chat_response{std::move(response), {}};
  }

  [[nodiscard]] auto invoke(wh::model::chat_request &&request) const
      -> wh::core::result<wh::model::chat_response> {
    return invoke(static_cast<const wh::model::chat_request &>(request));
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &) const
      -> wh::core::result<wh::model::chat_message_stream_reader> {
    ++state_->stream_calls;
    state_->last_bound_tool_count = bound_tool_count_;
    if (bound_tool_count_ == 0U) {
      return wh::core::result<wh::model::chat_message_stream_reader>::failure(
          wh::core::errc::invalid_argument);
    }

    return wh::model::chat_message_stream_reader{
        wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
            make_assistant_message("bound-stream"))};
  }

  [[nodiscard]] auto stream(wh::model::chat_request &&request) const
      -> wh::core::result<wh::model::chat_message_stream_reader> {
    return stream(static_cast<const wh::model::chat_request &>(request));
  }

  [[nodiscard]] auto bind_tools(
      std::span<const wh::schema::tool_schema_definition> tools) const
      -> tool_binding_probe_model_impl {
    ++state_->bind_calls;
    return tool_binding_probe_model_impl{state_, tools.size()};
  }

private:
  std::shared_ptr<tool_binding_probe_state> state_{};
  std::size_t bound_tool_count_{0U};
};

using tool_binding_probe_model =
    wh::model::chat_model<tool_binding_probe_model_impl>;

template <typename fn_t> struct sync_tool_invoke_impl {
  fn_t fn;

  [[nodiscard]] auto invoke(const wh::tool::tool_request &request) const
      -> decltype(std::invoke(fn, std::declval<std::string_view>(),
                              std::declval<const wh::tool::tool_options &>())) {
    return std::invoke(fn, std::string_view{request.input_json},
                       request.options);
  }
};

template <typename fn_t>
sync_tool_invoke_impl(fn_t) -> sync_tool_invoke_impl<fn_t>;

template <typename fn_t> struct sync_tool_stream_impl {
  fn_t fn;

  [[nodiscard]] auto stream(const wh::tool::tool_request &request) const
      -> decltype(std::invoke(fn, std::declval<std::string_view>(),
                              std::declval<const wh::tool::tool_options &>())) {
    return std::invoke(fn, std::string_view{request.input_json},
                       request.options);
  }
};

template <typename fn_t>
sync_tool_stream_impl(fn_t) -> sync_tool_stream_impl<fn_t>;

struct missing_tool_path final {};

template <typename invoke_impl_t = missing_tool_path,
          typename stream_impl_t = missing_tool_path>
struct sync_tool_impl {
  [[no_unique_address]] invoke_impl_t invoke_impl{};
  [[no_unique_address]] stream_impl_t stream_impl{};

  [[nodiscard]] auto invoke(const wh::tool::tool_request &request) const
      -> decltype(auto)
    requires(!std::same_as<invoke_impl_t, missing_tool_path>)
  {
    return invoke_impl.invoke(request);
  }

  [[nodiscard]] auto stream(const wh::tool::tool_request &request) const
      -> decltype(auto)
    requires(!std::same_as<stream_impl_t, missing_tool_path>)
  {
    return stream_impl.stream(request);
  }
};

template <typename invoke_impl_t>
sync_tool_impl(invoke_impl_t) -> sync_tool_impl<invoke_impl_t, missing_tool_path>;

template <typename invoke_impl_t, typename stream_impl_t>
sync_tool_impl(invoke_impl_t, stream_impl_t)
    -> sync_tool_impl<invoke_impl_t, stream_impl_t>;

template <typename fn_t> struct sender_tool_invoke_impl {
  fn_t fn;

  [[nodiscard]] auto invoke_sender(wh::tool::tool_request request) const {
    return stdexec::just(
        std::invoke(fn, std::string_view{request.input_json}, request.options));
  }
};

template <typename fn_t>
sender_tool_invoke_impl(fn_t) -> sender_tool_invoke_impl<fn_t>;

template <typename fn_t> struct sender_tool_stream_impl {
  fn_t fn;

  [[nodiscard]] auto stream_sender(wh::tool::tool_request request) const {
    return stdexec::just(
        std::invoke(fn, std::string_view{request.input_json}, request.options));
  }
};

template <typename fn_t>
sender_tool_stream_impl(fn_t) -> sender_tool_stream_impl<fn_t>;

template <typename invoke_impl_t = missing_tool_path,
          typename stream_impl_t = missing_tool_path>
struct sender_tool_impl {
  [[no_unique_address]] invoke_impl_t invoke_impl{};
  [[no_unique_address]] stream_impl_t stream_impl{};

  [[nodiscard]] auto invoke_sender(wh::tool::tool_request request) const
      -> decltype(auto)
    requires(!std::same_as<invoke_impl_t, missing_tool_path>)
  {
    return invoke_impl.invoke_sender(std::move(request));
  }

  [[nodiscard]] auto stream_sender(wh::tool::tool_request request) const
      -> decltype(auto)
    requires(!std::same_as<stream_impl_t, missing_tool_path>)
  {
    return stream_impl.stream_sender(std::move(request));
  }
};

template <typename invoke_impl_t>
sender_tool_impl(invoke_impl_t)
    -> sender_tool_impl<invoke_impl_t, missing_tool_path>;

template <typename invoke_impl_t, typename stream_impl_t>
sender_tool_impl(invoke_impl_t, stream_impl_t)
    -> sender_tool_impl<invoke_impl_t, stream_impl_t>;

template <typename fn_t> struct sync_retriever_impl {
  fn_t fn;

  [[nodiscard]] auto retrieve(
      const wh::retriever::retriever_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t>
sync_retriever_impl(fn_t) -> sync_retriever_impl<fn_t>;

template <typename fn_t> struct sync_embedding_impl {
  fn_t fn;

  [[nodiscard]] auto embed(const wh::embedding::embedding_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t>
sync_embedding_impl(fn_t) -> sync_embedding_impl<fn_t>;

template <typename fn_t> struct sync_indexer_batch_impl {
  fn_t fn;

  [[nodiscard]] auto write(const wh::indexer::indexer_request &request) const
      -> decltype(std::invoke(fn, request)) {
    return std::invoke(fn, request);
  }
};

template <typename fn_t>
sync_indexer_batch_impl(fn_t) -> sync_indexer_batch_impl<fn_t>;

template <typename fn_t> struct sync_indexer_single_impl {
  fn_t fn;

  [[nodiscard]] auto write_one(
      const wh::schema::document &document,
      const wh::indexer::indexer_options &options) const
      -> decltype(std::invoke(fn, document, options)) {
    return std::invoke(fn, document, options);
  }
};

template <typename fn_t>
sync_indexer_single_impl(fn_t) -> sync_indexer_single_impl<fn_t>;

} // namespace wh::testing::helper
