// Defines parent-document indexing flow lowered onto one frozen compose
// chain.
#pragma once

#include <concepts>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/authored/chain.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/map_result_sender.hpp"
#include "wh/core/stdexec/ready_result_sender.hpp"
#include "wh/core/stdexec/result_sender.hpp"
#include "wh/core/stdexec/variant_sender.hpp"
#include "wh/document/keys.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/schema/document.hpp"

namespace wh::flow::indexing {

inline constexpr std::string_view parent_id_metadata_key = wh::document::parent_id_metadata_key;
inline constexpr std::string_view sub_id_metadata_key = wh::document::sub_id_metadata_key;

namespace detail::indexing {

template <typename indexer_t>
concept indexer_component =
    requires(const indexer_t &indexer, const wh::indexer::indexer_request &request,
             wh::core::run_context &context) {
      {
        indexer.write(request, context)
      } -> std::same_as<wh::core::result<wh::indexer::indexer_response>>;
    };

template <typename transformer_t>
concept parent_transformer =
    requires(transformer_t transformer, const wh::schema::document &document,
             wh::core::run_context &context) {
      {
        transformer(document, context)
      } -> std::same_as<wh::core::result<std::vector<wh::schema::document>>>;
    };

template <typename generator_t>
concept sub_id_generator = requires(generator_t generator, const wh::schema::document &document,
                                    std::size_t count, wh::core::run_context &context) {
  {
    generator(document, count, context)
  } -> std::same_as<wh::core::result<std::vector<std::string>>>;
};

[[nodiscard]] inline auto effective_parent_id(const wh::schema::document &document) -> std::string {
  if (const auto *typed = document.metadata_ptr<std::string>(parent_id_metadata_key);
      typed != nullptr && !typed->empty()) {
    return *typed;
  }
  return document.content();
}

template <indexer_component indexer_t> struct borrowed_indexer {
  /// Borrowed indexer component kept alive by the frozen parent-indexer flow.
  const indexer_t *indexer{nullptr};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return indexer->descriptor();
  }

  [[nodiscard]] auto write(const wh::indexer::indexer_request &request,
                           wh::core::run_context &context) const
      -> wh::core::result<wh::indexer::indexer_response> {
    return indexer->write(request, context);
  }

  [[nodiscard]] auto write(wh::indexer::indexer_request &&request,
                           wh::core::run_context &context) const
      -> wh::core::result<wh::indexer::indexer_response> {
    return indexer->write(std::move(request), context);
  }

  template <typename request_t>
    requires requires(const indexer_t &value, request_t &&request, wh::core::run_context &context) {
      { value.async_write(std::forward<request_t>(request), context) } -> stdexec::sender;
    }
  [[nodiscard]] auto async_write(request_t &&request, wh::core::run_context &context) const {
    return indexer->async_write(std::forward<request_t>(request), context);
  }
};

template <typename value_t>
[[nodiscard]] inline auto read_graph_value(wh::compose::graph_value &&value)
    -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto map_indexer_result_sender(auto &&sender) {
  using write_result = wh::core::result<wh::indexer::indexer_response>;
  return wh::core::detail::normalize_result_sender<write_result>(
      wh::core::detail::map_result_sender<write_result>(
          std::forward<decltype(sender)>(sender),
          [](wh::compose::graph_value value) -> write_result {
            return read_graph_value<wh::indexer::indexer_response>(std::move(value));
          }));
}

} // namespace detail::indexing

/// Parent indexer flow that transforms parent documents into child chunks.
template <detail::indexing::indexer_component indexer_t,
          detail::indexing::parent_transformer transformer_t,
          detail::indexing::sub_id_generator sub_id_generator_t>
class parent {
  struct authored_state {
    authored_state(indexer_t indexer_value, transformer_t transformer_value,
                   sub_id_generator_t sub_id_generator_value) noexcept
        : indexer(std::move(indexer_value)), transformer(std::move(transformer_value)),
          sub_id_generator(std::move(sub_id_generator_value)) {}

    indexer_t indexer;
    transformer_t transformer;
    sub_id_generator_t sub_id_generator;
  };

  struct runtime_state {
    runtime_state(indexer_t indexer_value, transformer_t transformer_value,
                  sub_id_generator_t sub_id_generator_value) noexcept
        : indexer(std::move(indexer_value)), transformer(std::move(transformer_value)),
          sub_id_generator(std::move(sub_id_generator_value)) {}

    indexer_t indexer;
    transformer_t transformer;
    sub_id_generator_t sub_id_generator;
    wh::compose::chain runtime_graph{};
  };

public:
  parent(indexer_t indexer, transformer_t transformer, sub_id_generator_t sub_id_generator) noexcept
      : authored_(std::in_place, std::move(indexer), std::move(transformer),
                  std::move(sub_id_generator)) {}

  parent(const parent &) = default;
  auto operator=(const parent &) -> parent & = default;
  parent(parent &&) noexcept = default;
  auto operator=(parent &&) noexcept -> parent & = default;
  ~parent() = default;

  /// Returns component metadata when used as one indexer-like primitive.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"ParentIndexer", wh::core::component_kind::indexer};
  }

  /// Returns true after the authored parent-indexer graph has frozen.
  [[nodiscard]] auto frozen() const noexcept -> bool { return runtime_ != nullptr; }

  /// Freezes the authored parent-indexer shape into one compose chain.
  auto freeze() -> wh::core::result<void> {
    if (runtime_ != nullptr) {
      return {};
    }
    if (!authored_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    auto runtime = std::make_shared<runtime_state>(std::move(authored_->indexer),
                                                   std::move(authored_->transformer),
                                                   std::move(authored_->sub_id_generator));

    wh::compose::graph_compile_options options{};
    options.name = "indexing_parent";
    runtime->runtime_graph = wh::compose::chain{std::move(options)};

    auto prepare_added = runtime->runtime_graph.append(wh::compose::make_lambda_node(
        "parent_prepare_sub_documents",
        [transformer = &runtime->transformer, sub_id_generator = &runtime->sub_id_generator](
            const wh::compose::graph_value &input, wh::core::run_context &context,
            const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto request = detail::indexing::read_graph_value<wh::indexer::indexer_request>(
              wh::compose::graph_value{input});
          if (request.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(request.error());
          }

          std::vector<wh::schema::document> sub_documents{};
          for (const auto &parent_document : request.value().documents) {
            auto transformed = (*transformer)(parent_document, context);
            if (transformed.has_error()) {
              return wh::core::result<wh::compose::graph_value>::failure(transformed.error());
            }
            if (transformed.value().empty()) {
              return wh::core::result<wh::compose::graph_value>::failure(
                  wh::core::errc::invalid_argument);
            }

            const auto parent_id = detail::indexing::effective_parent_id(parent_document);
            auto sub_ids =
                (*sub_id_generator)(parent_document, transformed.value().size(), context);
            if (sub_ids.has_error()) {
              return wh::core::result<wh::compose::graph_value>::failure(sub_ids.error());
            }
            if (sub_ids.value().size() != transformed.value().size()) {
              return wh::core::result<wh::compose::graph_value>::failure(
                  wh::core::errc::invalid_argument);
            }

            for (std::size_t index = 0U; index < transformed.value().size(); ++index) {
              auto document = std::move(transformed.value()[index]);
              document.set_metadata(parent_id_metadata_key, parent_id);
              document.set_metadata(sub_id_metadata_key, sub_ids.value()[index]);
              sub_documents.push_back(std::move(document));
            }
          }

          auto delegated = request.value();
          delegated.documents = std::move(sub_documents);
          return wh::compose::graph_value{std::move(delegated)};
        }));
    if (prepare_added.has_error()) {
      return prepare_added;
    }

    auto index_added = runtime->runtime_graph.append(
        wh::compose::make_component_node<wh::compose::component_kind::indexer,
                                         wh::compose::node_contract::value,
                                         wh::compose::node_contract::value>(
            "parent_write_index",
            detail::indexing::borrowed_indexer<indexer_t>{.indexer = &runtime->indexer}));
    if (index_added.has_error()) {
      return index_added;
    }

    auto compiled = runtime->runtime_graph.compile();
    if (compiled.has_error()) {
      return compiled;
    }
    runtime_ = std::move(runtime);
    authored_.reset();
    return {};
  }

  /// Expands parents into grouped child documents and delegates one batch write.
  [[nodiscard]] auto write(const wh::indexer::indexer_request &request,
                           wh::core::run_context &context) const {
    return dispatch_request(wh::indexer::indexer_request{request}, context);
  }

  /// Async component-node entry that forwards to the flow-level sender.
  [[nodiscard]] auto async_write(const wh::indexer::indexer_request &request,
                                 wh::core::run_context &context) const {
    return dispatch_request(wh::indexer::indexer_request{request}, context);
  }

  /// Async component-node entry that forwards to the flow-level sender.
  [[nodiscard]] auto async_write(wh::indexer::indexer_request &&request,
                                 wh::core::run_context &context) const {
    return dispatch_request(std::move(request), context);
  }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, wh::indexer::indexer_request>
  [[nodiscard]] auto dispatch_request(request_t &&request, wh::core::run_context &context) const {
    using write_result = wh::core::result<wh::indexer::indexer_response>;
    using failure_sender_t = decltype(wh::core::detail::failure_result_sender<write_result>(
        wh::core::errc::internal_error));
    using mapped_sender_t = decltype(detail::indexing::map_indexer_result_sender(
        std::declval<const wh::compose::chain &>().invoke(
            context, wh::compose::graph_value{
                         wh::core::any(std::declval<wh::indexer::indexer_request>())})));
    using dispatch_sender_t = wh::core::detail::variant_sender<failure_sender_t, mapped_sender_t>;

    if (runtime_ == nullptr) {
      return dispatch_sender_t{wh::core::detail::failure_result_sender<write_result>(
          wh::core::errc::contract_violation)};
    }

    return dispatch_sender_t{
        detail::indexing::map_indexer_result_sender(runtime_->runtime_graph.invoke(
            context, wh::compose::graph_value{wh::core::any(
                         wh::indexer::indexer_request{std::forward<request_t>(request)})}))};
  }

  std::optional<authored_state> authored_{};
  std::shared_ptr<const runtime_state> runtime_{};
};

} // namespace wh::flow::indexing
