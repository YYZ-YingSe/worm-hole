// Defines retriever routing helpers that execute route, selected concurrent
// retrieval, and fusion as one retriever-like flow primitive.
#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/any.hpp"
#include "wh/core/component.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/stdexec/concurrent_sender_vector.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/schema/document.hpp"

namespace wh::flow::retrieval {

/// One route-local retrieval result.
struct routed_retriever_result {
  /// Stable retriever name.
  std::string retriever_name{};
  /// Retrieved documents for this route.
  wh::retriever::retriever_response documents{};
};

namespace detail::router {

using callback_sink = wh::callbacks::callback_sink;

inline constexpr std::string_view router_stage_name = "Router";
inline constexpr std::string_view fusion_stage_name = "FusionFunc";

struct callback_state {
  wh::callbacks::run_info run_info{};
  wh::retriever::retriever_callback_event event{};
};

struct route_plan {
  /// Base request copied once at route boundary for later selected fan-out.
  wh::retriever::retriever_request request{};
  /// Stable selected retriever names after validation and de-duplication.
  std::vector<std::string> selected_names{};
  /// Stable selected retriever indices aligned with `selected_names`.
  std::vector<std::size_t> selected_indices{};
};

[[nodiscard]] inline auto join_names(
    const std::vector<std::string> &names) -> std::string {
  std::string joined{};
  for (std::size_t index = 0U; index < names.size(); ++index) {
    if (index != 0U) {
      joined.push_back(',');
    }
    joined.append(names[index]);
  }
  return joined;
}

[[nodiscard]] inline auto make_callback_state(
    const std::string_view name, const wh::retriever::retriever_request &request,
    std::string extra = {}) -> callback_state {
  callback_state state{};
  state.run_info.name = std::string{name};
  state.run_info.type = std::string{name};
  state.run_info.component = wh::core::component_kind::retriever;
  state.run_info =
      wh::callbacks::apply_component_run_info(std::move(state.run_info),
                                              request.options);

  const auto options = request.options.resolve_view();
  state.event.top_k = options.top_k;
  state.event.score_threshold = options.score_threshold;
  state.event.filter = std::string{options.filter};
  state.event.extra = std::move(extra);
  return state;
}

template <typename state_t>
inline auto emit_callback(const callback_sink &sink,
                          const wh::callbacks::stage stage,
                          const state_t &state) -> void {
  wh::callbacks::emit(sink, stage, state);
}

template <typename retriever_t>
concept retriever_component =
    requires(const retriever_t &retriever,
             const wh::retriever::retriever_request &request,
             wh::core::run_context &context) {
      { retriever.retrieve(request, context) }
      -> std::same_as<wh::core::result<wh::retriever::retriever_response>>;
    };

template <retriever_component retriever_t> struct retriever_binding {
  /// Stable authored retriever name.
  std::string name{};
  /// Frozen retriever component object.
  retriever_t retriever;
};

template <typename route_t>
concept route_policy =
    requires(route_t route, const wh::retriever::retriever_request &request,
             const std::vector<std::string> &names) {
      { route(request, names) }
      -> std::same_as<wh::core::result<std::vector<std::string>>>;
    };

template <typename fusion_t>
concept fusion_policy =
    requires(fusion_t fusion,
             const std::vector<routed_retriever_result> &results) {
      { fusion(results) }
      -> std::same_as<wh::core::result<wh::retriever::retriever_response>>;
    };

struct route_all_retrievers {
  [[nodiscard]] auto operator()(const wh::retriever::retriever_request &,
                                const std::vector<std::string> &names) const
      -> wh::core::result<std::vector<std::string>> {
    return names;
  }
};

struct reciprocal_rank_fusion {
  [[nodiscard]] auto operator()(
      const std::vector<routed_retriever_result> &results) const
      -> wh::core::result<wh::retriever::retriever_response> {
    std::unordered_map<std::string, std::pair<double, std::size_t>,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        scores{};
    std::unordered_map<std::string, wh::schema::document,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        documents{};

    std::size_t sequence = 0U;
    for (const auto &result : results) {
      for (std::size_t rank = 0U; rank < result.documents.size(); ++rank) {
        const auto &document = result.documents[rank];
        const auto key = document.content();
        auto [score_iter, inserted] = scores.try_emplace(key, 0.0, sequence++);
        score_iter->second.first += 1.0 / (static_cast<double>(rank) + 60.0);
        if (inserted) {
          documents.emplace(key, document);
        }
      }
    }

    std::vector<std::pair<std::string, std::pair<double, std::size_t>>> ranked{};
    ranked.reserve(scores.size());
    for (const auto &entry : scores) {
      ranked.push_back(entry);
    }
    std::ranges::sort(ranked, [](const auto &left, const auto &right) {
      if (left.second.first != right.second.first) {
        return left.second.first > right.second.first;
      }
      return left.second.second < right.second.second;
    });

    wh::retriever::retriever_response fused{};
    fused.reserve(ranked.size());
    for (const auto &entry : ranked) {
      auto document = documents.at(entry.first);
      document.with_score(entry.second.first);
      fused.push_back(std::move(document));
    }
    return fused;
  }
};

template <retriever_component retriever_t, route_policy route_t,
          fusion_policy fusion_t>
struct storage {
  explicit storage(route_t route, fusion_t fusion) noexcept(
      std::is_nothrow_move_constructible_v<route_t> &&
      std::is_nothrow_move_constructible_v<fusion_t>)
      : route_policy(std::move(route)), fusion_policy(std::move(fusion)) {}

  auto rebuild_frozen_view() -> void {
    frozen_names.clear();
    frozen_name_to_index.clear();
    frozen_names.reserve(retrievers.size());
    frozen_name_to_index.reserve(retrievers.size());
    for (std::size_t index = 0U; index < retrievers.size(); ++index) {
      frozen_names.push_back(retrievers[index].name);
      frozen_name_to_index.emplace(retrievers[index].name, index);
    }
  }

  route_t route_policy;
  fusion_t fusion_policy;
  std::vector<retriever_binding<retriever_t>> retrievers{};
  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      name_index{};
  std::vector<std::string> frozen_names{};
  std::unordered_map<std::string, std::size_t,
                     wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      frozen_name_to_index{};
  bool frozen{false};
};

template <retriever_component retriever_t>
[[nodiscard]] inline auto make_retrieve_sender(
    const retriever_t &retriever, std::string retriever_name,
    wh::retriever::retriever_request request, wh::core::run_context &context) {
  using retrieve_status = wh::core::result<routed_retriever_result>;
  if constexpr (requires(const retriever_t &value,
                         wh::retriever::retriever_request &&owned_request,
                         wh::core::run_context &run_context) {
                  { value.async_retrieve(std::move(owned_request), run_context) }
                  -> stdexec::sender;
                }) {
    return wh::core::detail::map_result_sender<retrieve_status>(
        wh::core::detail::normalize_result_sender<
            wh::core::result<wh::retriever::retriever_response>>(
            retriever.async_retrieve(std::move(request), context)),
        [retriever_name = std::move(retriever_name)](
            wh::retriever::retriever_response documents) mutable
            -> retrieve_status {
          return routed_retriever_result{
              .retriever_name = std::move(retriever_name),
              .documents = std::move(documents),
          };
        });
  } else {
    auto status = retriever.retrieve(std::move(request), context);
    if (status.has_error()) {
      return stdexec::just(retrieve_status::failure(status.error()));
    }
    return stdexec::just(retrieve_status{routed_retriever_result{
        .retriever_name = std::move(retriever_name),
        .documents = std::move(status).value(),
    }});
  }
}

template <retriever_component retriever_t>
using retrieve_sender_t =
    decltype(make_retrieve_sender(std::declval<const retriever_t &>(),
                                  std::declval<std::string>(),
                                  std::declval<wh::retriever::retriever_request>(),
                                  std::declval<wh::core::run_context &>()));

template <retriever_component retriever_t, route_policy route_t,
          fusion_policy fusion_t>
[[nodiscard]] inline auto build_route_plan(
    const storage<retriever_t, route_t, fusion_t> &state,
    const wh::retriever::retriever_request &request,
    wh::core::run_context &context) -> wh::core::result<route_plan> {
  auto sink = wh::callbacks::filter_callback_sink(
      wh::callbacks::borrow_callback_sink(context), request.options);
  auto callback_state =
      make_callback_state(router_stage_name, request, request.query);
  emit_callback(sink, wh::callbacks::stage::start, callback_state);

  auto selected = state.route_policy(request, state.frozen_names);
  if (selected.has_error()) {
    emit_callback(sink, wh::callbacks::stage::error, callback_state);
    return wh::core::result<route_plan>::failure(selected.error());
  }
  if (selected.value().empty()) {
    emit_callback(sink, wh::callbacks::stage::error, callback_state);
    return wh::core::result<route_plan>::failure(wh::core::errc::not_found);
  }

  std::unordered_set<std::string, wh::core::transparent_string_hash,
                     wh::core::transparent_string_equal>
      seen{};
  route_plan plan{};
  plan.request = request;
  plan.selected_names.reserve(selected.value().size());
  plan.selected_indices.reserve(selected.value().size());
  for (const auto &name : selected.value()) {
    const auto index_iter = state.frozen_name_to_index.find(name);
    if (index_iter == state.frozen_name_to_index.end()) {
      emit_callback(sink, wh::callbacks::stage::error, callback_state);
      return wh::core::result<route_plan>::failure(wh::core::errc::not_found);
    }
    if (!seen.insert(name).second) {
      continue;
    }
    plan.selected_names.push_back(name);
    plan.selected_indices.push_back(index_iter->second);
  }

  if (plan.selected_indices.empty()) {
    emit_callback(sink, wh::callbacks::stage::error, callback_state);
    return wh::core::result<route_plan>::failure(wh::core::errc::not_found);
  }

  callback_state.event.extra = join_names(plan.selected_names);
  emit_callback(sink, wh::callbacks::stage::end, callback_state);
  return plan;
}

template <retriever_component retriever_t, route_policy route_t,
          fusion_policy fusion_t>
[[nodiscard]] inline auto fuse_results(
    const storage<retriever_t, route_t, fusion_t> &state,
    const route_plan &plan, std::vector<routed_retriever_result> results,
    wh::core::run_context &context)
    -> wh::core::result<wh::retriever::retriever_response> {
  auto sink = wh::callbacks::filter_callback_sink(
      wh::callbacks::borrow_callback_sink(context), plan.request.options);
  auto callback_state = make_callback_state(
      fusion_stage_name, plan.request, std::to_string(results.size()));
  emit_callback(sink, wh::callbacks::stage::start, callback_state);

  auto fused = state.fusion_policy(results);
  if (fused.has_error()) {
    emit_callback(sink, wh::callbacks::stage::error, callback_state);
    return wh::core::result<wh::retriever::retriever_response>::failure(
        fused.error());
  }

  callback_state.event.extra = std::to_string(fused.value().size());
  emit_callback(sink, wh::callbacks::stage::end, callback_state);
  return fused;
}

template <retriever_component retriever_t, route_policy route_t,
          fusion_policy fusion_t>
[[nodiscard]] inline auto make_fanout_sender(
    std::shared_ptr<const storage<retriever_t, route_t, fusion_t>> state,
    route_plan plan, wh::core::run_context &context) {
  using retrieve_status = wh::core::result<routed_retriever_result>;
  using output_status = wh::core::result<wh::retriever::retriever_response>;
  using child_sender_t = retrieve_sender_t<retriever_t>;

  std::vector<child_sender_t> senders{};
  senders.reserve(plan.selected_indices.size());
  for (std::size_t offset = 0U; offset < plan.selected_indices.size(); ++offset) {
    const auto retriever_index = plan.selected_indices[offset];
    const auto &binding = state->retrievers[retriever_index];
    senders.push_back(
        make_retrieve_sender(binding.retriever, binding.name, plan.request, context));
  }

  return wh::core::detail::make_concurrent_sender_vector<retrieve_status>(
             std::move(senders), plan.selected_indices.size()) |
         stdexec::then(
             [state, plan = std::move(plan), &context](
                 std::vector<retrieve_status> statuses) mutable -> output_status {
               std::vector<routed_retriever_result> results{};
               results.reserve(statuses.size());
               for (auto &status : statuses) {
                 if (status.has_error()) {
                   return output_status::failure(status.error());
                 }
                 results.push_back(std::move(status).value());
               }
               return fuse_results(*state, plan, std::move(results), context);
             });
}

template <retriever_component retriever_t, route_policy route_t,
          fusion_policy fusion_t>
[[nodiscard]] inline auto make_pipeline_sender(
    std::shared_ptr<const storage<retriever_t, route_t, fusion_t>> state,
    wh::retriever::retriever_request request, wh::core::run_context &context) {
  using route_status = wh::core::result<route_plan>;
  using output_status = wh::core::result<wh::retriever::retriever_response>;
  using route_failure_sender_t =
      decltype(stdexec::just(output_status::failure(wh::core::errc::internal_error)));
  using fanout_sender_t = decltype(make_fanout_sender(
      std::declval<std::shared_ptr<const storage<retriever_t, route_t, fusion_t>>>(),
      std::declval<route_plan>(), std::declval<wh::core::run_context &>()));
  using route_dispatch_sender_t =
      wh::core::detail::variant_sender<route_failure_sender_t, fanout_sender_t>;

  auto route_sender = stdexec::just() | stdexec::then(
      [state, request = std::move(request), &context]() mutable -> route_status {
        return build_route_plan(*state, request, context);
      });

  return stdexec::let_value(
      std::move(route_sender),
      [state, &context](route_status routed) mutable {
        if (routed.has_error()) {
          return route_dispatch_sender_t{
              stdexec::just(output_status::failure(routed.error()))};
        }
        return route_dispatch_sender_t{
            make_fanout_sender(state, std::move(routed).value(), context)};
      });
}

} // namespace detail::router

/// Router flow over one homogeneous retriever set.
template <detail::router::retriever_component retriever_t,
          detail::router::route_policy route_t =
              detail::router::route_all_retrievers,
          detail::router::fusion_policy fusion_t =
              detail::router::reciprocal_rank_fusion>
class router {
  using storage_t = detail::router::storage<retriever_t, route_t, fusion_t>;

public:
  router()
    requires std::default_initializable<route_t> &&
             std::default_initializable<fusion_t>
      : router(route_t{}, fusion_t{}) {}

  explicit router(route_t route_policy, fusion_t fusion_policy = {}) noexcept
      : storage_(
            std::make_shared<storage_t>(std::move(route_policy),
                                        std::move(fusion_policy))) {}

  router(const router &) = default;
  auto operator=(const router &) -> router & = default;
  router(router &&) noexcept = default;
  auto operator=(router &&) noexcept -> router & = default;
  ~router() = default;

  /// Returns component metadata when used as one retriever-like primitive.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"Router", wh::core::component_kind::retriever};
  }

  /// Returns true after registration has frozen successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool {
    return storage_ != nullptr && storage_->frozen;
  }

  /// Registers one named retriever before freeze.
  auto add_retriever(std::string name, retriever_t retriever)
      -> wh::core::result<void> {
    if (storage_ == nullptr || storage_->frozen) {
      return wh::core::result<void>::failure(
          wh::core::errc::contract_violation);
    }
    if (name.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (storage_->name_index.contains(name)) {
      return wh::core::result<void>::failure(wh::core::errc::already_exists);
    }
    storage_->name_index.insert(name);
    storage_->retrievers.push_back(detail::router::retriever_binding<retriever_t>{
        .name = std::move(name), .retriever = std::move(retriever)});
    return {};
  }

  /// Freezes the retriever registry and route lookup tables.
  auto freeze() -> wh::core::result<void> {
    if (storage_ == nullptr) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (storage_->frozen) {
      return {};
    }
    if (storage_->retrievers.empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    storage_->rebuild_frozen_view();
    storage_->frozen = true;
    return {};
  }

  /// Convenience alias kept at flow level.
  [[nodiscard]] auto retrieve(const wh::retriever::retriever_request &request,
                              wh::core::run_context &context) const {
    return async_retrieve(request, context);
  }

  /// Convenience alias kept at flow level.
  [[nodiscard]] auto retrieve(wh::retriever::retriever_request &&request,
                              wh::core::run_context &context) const {
    return async_retrieve(std::move(request), context);
  }

  /// Runs route, selected concurrent retrieval, and fusion as one sender.
  [[nodiscard]] auto async_retrieve(
      const wh::retriever::retriever_request &request,
      wh::core::run_context &context) const {
    return dispatch_request(wh::retriever::retriever_request{request}, context);
  }

  /// Runs route, selected concurrent retrieval, and fusion as one sender.
  [[nodiscard]] auto async_retrieve(wh::retriever::retriever_request &&request,
                                    wh::core::run_context &context) const {
    return dispatch_request(std::move(request), context);
  }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>,
                          wh::retriever::retriever_request>
  [[nodiscard]] auto dispatch_request(request_t &&request,
                                      wh::core::run_context &context) const {
    using output_status = wh::core::result<wh::retriever::retriever_response>;
    using failure_sender_t = decltype(
        wh::core::detail::failure_result_sender<output_status>(
            wh::core::errc::internal_error));
    using pipeline_sender_t = decltype(detail::router::make_pipeline_sender(
        std::declval<std::shared_ptr<const storage_t>>(),
        std::declval<wh::retriever::retriever_request>(),
        std::declval<wh::core::run_context &>()));
    using dispatch_sender_t =
        wh::core::detail::variant_sender<failure_sender_t, pipeline_sender_t>;

    auto self = const_cast<router *>(this);
    auto frozen = self->freeze();
    if (frozen.has_error()) {
      return dispatch_sender_t{
          wh::core::detail::failure_result_sender<output_status>(
              frozen.error())};
    }

    return dispatch_sender_t{detail::router::make_pipeline_sender(
        std::shared_ptr<const storage_t>{storage_},
        wh::retriever::retriever_request{std::forward<request_t>(request)},
        context)};
  }

  std::shared_ptr<storage_t> storage_{};
};

} // namespace wh::flow::retrieval
