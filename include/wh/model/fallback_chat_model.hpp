// Defines one compose-ready chat-model wrapper that loads tool catalogs on
// demand and routes invoke/stream calls through fallback helpers.
#pragma once

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wh/core/component.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/tool/catalog.hpp"

namespace wh::model {

/// Compose-ready chat-model wrapper with optional catalog-backed tool binding
/// and invoke/stream fallback routing.
template <chat_model_like model_t>
class fallback_chat_model {
public:
  using catalog_cache_type = wh::tool::tool_catalog_cache;

  fallback_chat_model() = default;

  /// Stores fallback candidates by value.
  explicit fallback_chat_model(std::vector<model_t> candidates) noexcept
      : candidates_(std::move(candidates)) {}

  /// Stores fallback candidates plus one static tool schema set.
  fallback_chat_model(
      std::vector<model_t> candidates,
      std::vector<wh::schema::tool_schema_definition> tools) noexcept
      : candidates_(std::move(candidates)), bound_tools_(std::move(tools)) {}

  /// Stores fallback candidates plus one catalog cache used when requests do
  /// not provide tool schemas explicitly.
  fallback_chat_model(std::vector<model_t> candidates,
                      catalog_cache_type catalog) noexcept
      : candidates_(std::move(candidates)), catalog_(std::move(catalog)) {}

  /// Stores fallback candidates, static tools, and one optional catalog cache.
  fallback_chat_model(
      std::vector<model_t> candidates,
      std::vector<wh::schema::tool_schema_definition> tools,
      std::optional<catalog_cache_type> catalog) noexcept
      : candidates_(std::move(candidates)), bound_tools_(std::move(tools)),
        catalog_(std::move(catalog)) {}

  fallback_chat_model(const fallback_chat_model &) = default;
  fallback_chat_model(fallback_chat_model &&) noexcept = default;
  auto operator=(const fallback_chat_model &) -> fallback_chat_model & = default;
  auto operator=(fallback_chat_model &&) noexcept -> fallback_chat_model & = default;
  ~fallback_chat_model() = default;

  /// Stable descriptor metadata for compose/component bindings.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"FallbackChatModel",
                                          wh::core::component_kind::model};
  }

  /// Runs invoke fallback using request tools, bound tools, or catalog tools.
  [[nodiscard]] auto invoke(const chat_request &request,
                            wh::core::run_context &) const
      -> wh::core::result<chat_response> {
    auto prepared = prepare_request(request);
    if (prepared.has_error()) {
      return wh::core::result<chat_response>::failure(prepared.error());
    }

    auto report = invoke_with_fallback(std::span<const model_t>{candidates_},
                                       std::move(prepared).value());
    if (report.has_error()) {
      return wh::core::result<chat_response>::failure(report.error());
    }
    return std::move(report).value().response;
  }

  /// Runs invoke fallback using request tools, bound tools, or catalog tools.
  [[nodiscard]] auto invoke(chat_request &&request, wh::core::run_context &context) const
      -> wh::core::result<chat_response> {
    return invoke(static_cast<const chat_request &>(request), context);
  }

  /// Runs stream fallback using request tools, bound tools, or catalog tools.
  [[nodiscard]] auto stream(const chat_request &request,
                            wh::core::run_context &) const
      -> wh::core::result<chat_message_stream_reader> {
    auto prepared = prepare_request(request);
    if (prepared.has_error()) {
      return wh::core::result<chat_message_stream_reader>::failure(
          prepared.error());
    }

    auto report = stream_with_fallback(std::span<const model_t>{candidates_},
                                       std::move(prepared).value());
    if (report.has_error()) {
      return wh::core::result<chat_message_stream_reader>::failure(
          report.error());
    }
    return std::move(report).value().reader;
  }

  /// Runs stream fallback using request tools, bound tools, or catalog tools.
  [[nodiscard]] auto stream(chat_request &&request, wh::core::run_context &context) const
      -> wh::core::result<chat_message_stream_reader> {
    return stream(static_cast<const chat_request &>(request), context);
  }

  /// Returns a wrapper with the supplied static tool schema set bound.
  [[nodiscard]] auto bind_tools(
      const std::span<const wh::schema::tool_schema_definition> tools) const
      -> fallback_chat_model {
    auto next = *this;
    next.bound_tools_.assign(tools.begin(), tools.end());
    return next;
  }

  /// Exposes the stored static tool schema set for tests and diagnostics.
  [[nodiscard]] auto bound_tools() const noexcept
      -> const std::vector<wh::schema::tool_schema_definition> & {
    return bound_tools_;
  }

  /// Exposes the stored candidate models for tests and diagnostics.
  [[nodiscard]] auto candidates() const noexcept -> const std::vector<model_t> & {
    return candidates_;
  }

private:
  /// Resolves tool schemas for this request from explicit input, bound tools,
  /// or the optional catalog cache.
  [[nodiscard]] auto prepare_request(const chat_request &request) const
      -> wh::core::result<chat_request> {
    chat_request prepared{request};
    if (!prepared.tools.empty()) {
      return prepared;
    }

    if (!bound_tools_.empty()) {
      prepared.tools = bound_tools_;
      return prepared;
    }

    if (!catalog_.has_value()) {
      return prepared;
    }

    auto loaded = catalog_->load();
    if (loaded.has_error()) {
      return wh::core::result<chat_request>::failure(loaded.error());
    }
    prepared.tools.assign(loaded.value().begin(), loaded.value().end());
    return prepared;
  }

  /// Owned fallback candidate set used for invoke/stream startup routing.
  std::vector<model_t> candidates_{};
  /// Optional static tool schema set applied when requests omit tools.
  std::vector<wh::schema::tool_schema_definition> bound_tools_{};
  /// Optional catalog cache used to resolve tools dynamically.
  std::optional<catalog_cache_type> catalog_{};
};

} // namespace wh::model
