// Defines parent-document recovery flow lowered onto one frozen compose
// chain.
#pragma once

#include <algorithm>
#include <concepts>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/authored/chain.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/document/keys.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/document.hpp"

namespace wh::flow::retrieval {

inline constexpr std::string_view parent_id_metadata_key =
    wh::document::parent_id_metadata_key;

namespace detail::parent {

template <typename retriever_t>
concept retriever_component =
    requires(const retriever_t &retriever,
             const wh::retriever::retriever_request &request,
             wh::core::run_context &context) {
      { retriever.retrieve(request, context) }
      -> std::same_as<wh::core::result<wh::retriever::retriever_response>>;
    };

template <typename loader_t>
concept parent_loader =
    requires(loader_t loader, const std::vector<std::string> &parent_ids,
             wh::core::run_context &context) {
      { loader(parent_ids, context) }
      -> std::same_as<wh::core::result<std::vector<wh::schema::document>>>;
    };

struct parent_lookup_state {
  /// Ordered unique parent ids extracted from child hits.
  std::vector<std::string> parent_ids{};
};

template <retriever_component retriever_t> struct borrowed_retriever {
  /// Borrowed child-retriever component kept alive by the frozen parent flow.
  const retriever_t *retriever{nullptr};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return retriever->descriptor();
  }

  [[nodiscard]] auto retrieve(
      const wh::retriever::retriever_request &request,
      wh::core::run_context &context) const
      -> wh::core::result<wh::retriever::retriever_response> {
    return retriever->retrieve(request, context);
  }

  [[nodiscard]] auto retrieve(wh::retriever::retriever_request &&request,
                              wh::core::run_context &context) const
      -> wh::core::result<wh::retriever::retriever_response> {
    return retriever->retrieve(std::move(request), context);
  }

  template <typename request_t>
    requires requires(const retriever_t &value, request_t &&request,
                      wh::core::run_context &context) {
      { value.async_retrieve(std::forward<request_t>(request), context) }
      -> stdexec::sender;
    }
  [[nodiscard]] auto async_retrieve(request_t &&request,
                                    wh::core::run_context &context) const {
    return retriever->async_retrieve(std::forward<request_t>(request), context);
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

[[nodiscard]] inline auto map_parent_result_sender(auto &&sender) {
  using retrieve_result = wh::core::result<wh::retriever::retriever_response>;
  return wh::core::detail::normalize_result_sender<retrieve_result>(
      wh::core::detail::map_result_sender<retrieve_result>(
          std::forward<decltype(sender)>(sender),
          [](wh::compose::graph_value value) -> retrieve_result {
            return read_graph_value<wh::retriever::retriever_response>(
                std::move(value));
          }));
}

} // namespace detail::parent

/// Parent retriever that maps child hits back to unique parent documents.
template <detail::parent::retriever_component retriever_t,
          detail::parent::parent_loader parent_loader_t>
class parent {
  struct storage {
    storage(retriever_t child_retriever, parent_loader_t parent_loader) noexcept
        : child_retriever(
              std::make_unique<retriever_t>(std::move(child_retriever))),
          parent_loader(
              std::make_unique<parent_loader_t>(std::move(parent_loader))) {}

    std::unique_ptr<retriever_t> child_retriever{};
    std::unique_ptr<parent_loader_t> parent_loader{};
    wh::compose::chain runtime_graph{};
    bool frozen{false};
  };

public:
  parent(retriever_t child_retriever, parent_loader_t parent_loader) noexcept
      : storage_(std::make_shared<storage>(std::move(child_retriever),
                                           std::move(parent_loader))) {}

  parent(const parent &) = default;
  auto operator=(const parent &) -> parent & = default;
  parent(parent &&) noexcept = default;
  auto operator=(parent &&) noexcept -> parent & = default;
  ~parent() = default;

  /// Returns component metadata when used as one retriever-like primitive.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"ParentRetriever", wh::core::component_kind::retriever};
  }

  /// Returns true after the authored parent-retriever graph has frozen.
  [[nodiscard]] auto frozen() const noexcept -> bool {
    return storage_ != nullptr && storage_->frozen;
  }

  /// Freezes the authored parent-retriever shape into one compose chain.
  auto freeze() -> wh::core::result<void> {
    if (storage_ == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (storage_->frozen) {
      return {};
    }
    if (storage_->child_retriever == nullptr || storage_->parent_loader == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }

    wh::compose::graph_compile_options options{};
    options.name = "retrieval_parent";
    wh::compose::chain lowered{std::move(options)};

    auto child_added = lowered.append(
        wh::compose::make_component_node<wh::compose::component_kind::retriever,
                                         wh::compose::node_contract::value,
                                         wh::compose::node_contract::value>(
            "parent_child_retriever",
            detail::parent::borrowed_retriever<retriever_t>{
                .retriever = storage_->child_retriever.get(),
            }));
    if (child_added.has_error()) {
      return child_added;
    }

    auto collect_added = lowered.append(wh::compose::make_lambda_node(
        "parent_collect_ids",
        [](const wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          auto child_hits =
              detail::parent::read_graph_value<wh::retriever::retriever_response>(
                  wh::compose::graph_value{input});
          if (child_hits.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                child_hits.error());
          }

          std::unordered_set<std::string, wh::core::transparent_string_hash,
                             wh::core::transparent_string_equal>
              seen{};
          detail::parent::parent_lookup_state state{};
          for (const auto &document : child_hits.value()) {
            const auto *parent_id =
                document.template metadata_ptr<std::string>(parent_id_metadata_key);
            if (parent_id == nullptr || parent_id->empty()) {
              continue;
            }
            if (seen.insert(*parent_id).second) {
              state.parent_ids.push_back(*parent_id);
            }
          }
          return wh::compose::graph_value{std::move(state)};
        }));
    if (collect_added.has_error()) {
      return collect_added;
    }

    auto load_added = lowered.append(wh::compose::make_lambda_node(
        "parent_load_documents",
        [loader = storage_->parent_loader.get()](
            const wh::compose::graph_value &input, wh::core::run_context &context,
            const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          auto state =
              detail::parent::read_graph_value<detail::parent::parent_lookup_state>(
                  wh::compose::graph_value{input});
          if (state.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                state.error());
          }

          auto loaded = (*loader)(state.value().parent_ids, context);
          if (loaded.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(
                loaded.error());
          }

          std::unordered_map<std::string, wh::schema::document,
                             wh::core::transparent_string_hash,
                             wh::core::transparent_string_equal>
              documents{};
          for (auto &document : loaded.value()) {
            const auto *parent_id =
                document.template metadata_ptr<std::string>(parent_id_metadata_key);
            if (parent_id == nullptr) {
              continue;
            }
            documents.insert_or_assign(*parent_id, std::move(document));
          }

          wh::retriever::retriever_response ordered{};
          ordered.reserve(state.value().parent_ids.size());
          for (const auto &parent_id : state.value().parent_ids) {
            const auto iter = documents.find(parent_id);
            if (iter != documents.end()) {
              ordered.push_back(iter->second);
            }
          }
          return wh::compose::graph_value{std::move(ordered)};
        }));
    if (load_added.has_error()) {
      return load_added;
    }

    auto compiled = lowered.compile();
    if (compiled.has_error()) {
      return compiled;
    }
    storage_->runtime_graph = std::move(lowered);
    storage_->frozen = true;
    return {};
  }

  /// Runs child retrieval, extracts stable parent ids, and loads parent docs in
  /// first-hit order.
  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request,
                              wh::core::run_context &context) const {
    using retrieve_result = wh::core::result<wh::retriever::retriever_response>;
    using failure_sender_t = decltype(
        wh::core::detail::failure_result_sender<retrieve_result>(
            wh::core::errc::internal_error));
    using mapped_sender_t = decltype(detail::parent::map_parent_result_sender(
        std::declval<wh::compose::chain &>().invoke(
            context, wh::compose::graph_value{wh::core::any(std::declval<
                         wh::retriever::retriever_request>())})));
    using dispatch_sender_t =
        wh::core::detail::variant_sender<failure_sender_t, mapped_sender_t>;

    auto self = const_cast<parent *>(this);
    auto frozen = self->freeze();
    if (frozen.has_error()) {
      return dispatch_sender_t{
          wh::core::detail::failure_result_sender<retrieve_result>(
              frozen.error())};
    }
    return dispatch_sender_t{detail::parent::map_parent_result_sender(
        storage_->runtime_graph.invoke(
            context, wh::compose::graph_value{wh::core::any(request)}))};
  }

  /// Async component-node entry that forwards to the flow-level sender.
  [[nodiscard]] auto async_retrieve(
      const wh::retriever::retriever_request &request,
      wh::core::run_context &context) const {
    return retrieve(request, context);
  }

  /// Async component-node entry that forwards to the flow-level sender.
  [[nodiscard]] auto async_retrieve(wh::retriever::retriever_request &&request,
                                    wh::core::run_context &context) const {
    return retrieve(request, context);
  }

private:
  std::shared_ptr<storage> storage_{};
};

} // namespace wh::flow::retrieval
