// Defines retriever component contracts for query execution and scored
// document candidate retrieval.
#pragma once

#include <algorithm>
#include <cctype>
#include <concepts>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_set>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/component.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/retriever/callback_event.hpp"
#include "wh/retriever/options.hpp"
#include "wh/schema/document.hpp"

namespace wh::core {
struct run_context;
}

namespace wh::retriever {

/// Data contract for `retriever_request`.
struct retriever_request {
  /// User query text used for retrieval.
  std::string query{};
  /// Logical index identifier.
  std::string index{};
  /// Optional sub-index filter.
  std::string sub_index{};
  /// Optional query embedding vector.
  std::vector<double> embedding{};
  /// Retrieval options including filtering/routing controls.
  retriever_options options{};
};

namespace detail {

/// Trims ASCII whitespace from both ends of the provided string view.
[[nodiscard]] inline auto trim_ascii(const std::string_view value)
    -> std::string_view {
  std::size_t begin = 0U;
  while (begin < value.size() &&
         std::isspace(static_cast<unsigned char>(value[begin])) != 0) {
    ++begin;
  }

  std::size_t end = value.size();
  while (end > begin &&
         std::isspace(static_cast<unsigned char>(value[end - 1U])) != 0) {
    --end;
  }
  return value.substr(begin, end - begin);
}

/// Evaluates whether document metadata satisfies the filter expression.
[[nodiscard]] inline auto
matches_filter_expression(const wh::schema::document &document,
                          const std::string_view expression) -> bool {
  const auto trimmed = trim_ascii(expression);
  if (trimmed.empty()) {
    return true;
  }

  const auto equals = trimmed.find('=');
  if (equals == std::string_view::npos) {
    return document.content().find(trimmed) != std::string::npos;
  }

  const auto key = trim_ascii(trimmed.substr(0U, equals));
  const auto value = trim_ascii(trimmed.substr(equals + 1U));
  if (key.empty()) {
    return false;
  }

  if (key == "content") {
    return document.content() == value;
  }
  if (key == "sub_index" || key == "_sub_index") {
    return document.sub_index() == value;
  }
  if (key == "dsl" || key == "_dsl") {
    return document.dsl() == value;
  }
  if (key == "extra_info" || key == "_extra_info") {
    return document.extra_info() == value;
  }

  if (const auto *typed = document.metadata_ptr<std::string>(key);
      typed != nullptr) {
    return *typed == value;
  }
  if (const auto *typed = document.metadata_ptr<std::int64_t>(key);
      typed != nullptr) {
    return std::to_string(*typed) == value;
  }
  if (const auto *typed = document.metadata_ptr<double>(key);
      typed != nullptr) {
    return std::to_string(*typed) == value;
  }
  if (const auto *typed = document.metadata_ptr<bool>(key); typed != nullptr) {
    return (*typed ? "true" : "false") == value;
  }

  return false;
}

using retriever_result = wh::core::result<std::vector<wh::schema::document>>;

struct response_policy {
  retriever_common_options options{};
  std::string sub_index{};
};

struct retriever_callback_state {
  wh::callbacks::run_info run_info{};
  retriever_callback_event event{};
};

using callback_sink = wh::callbacks::callback_sink;
using wh::callbacks::borrow_callback_sink;
using wh::callbacks::make_callback_sink;

template <typename... args_t>
inline auto emit_callback(args_t &&...args) -> void {
  wh::callbacks::emit(std::forward<args_t>(args)...);
}

template <typename impl_t>
concept sync_retriever_handler =
    requires(const impl_t &impl, const retriever_request &request) {
      { impl.retrieve(request) } -> std::same_as<retriever_result>;
    };

template <typename impl_t>
concept sender_retriever_handler_const =
    requires(const impl_t &impl, const retriever_request &request) {
      { impl.retrieve_sender(request) } -> stdexec::sender;
    };

template <typename impl_t>
concept sender_retriever_handler_move =
    requires(const impl_t &impl, retriever_request &&request) {
      { impl.retrieve_sender(std::move(request)) } -> stdexec::sender;
    };

template <typename impl_t>
concept async_retriever_handler = sender_retriever_handler_const<impl_t> ||
                                  sender_retriever_handler_move<impl_t>;

template <typename impl_t>
concept sender_retriever_handler = async_retriever_handler<impl_t>;

template <typename impl_t>
concept retriever_impl =
    async_retriever_handler<impl_t> || sync_retriever_handler<impl_t>;

template <typename impl_t>
[[nodiscard]] inline auto
run_sync_retriever_impl(const impl_t &impl, const retriever_request &request)
    -> retriever_result {
  if constexpr (requires {
                  { impl.retrieve(request) } -> std::same_as<retriever_result>;
                }) {
    return impl.retrieve(request);
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_retriever_impl(const impl_t &impl,
                                                  retriever_request &&request)
    -> retriever_result {
  if constexpr (requires {
                  {
                    impl.retrieve(std::move(request))
                  } -> std::same_as<retriever_result>;
                }) {
    return impl.retrieve(std::move(request));
  } else {
    return run_sync_retriever_impl(impl, request);
  }
}

[[nodiscard]] inline auto make_callback_state(const retriever_request &request)
    -> retriever_callback_state {
  retriever_callback_state state{};
  state.run_info.name = "Retriever";
  state.run_info.type = "Retriever";
  state.run_info.component = wh::core::component_kind::retriever;
  state.run_info = wh::callbacks::apply_component_run_info(
      std::move(state.run_info), request.options);

  const auto options = request.options.resolve_view();
  state.event.top_k = options.top_k;
  state.event.score_threshold = options.score_threshold;
  state.event.filter = options.filter;
  state.event.extra = request.query;
  return state;
}

[[nodiscard]] inline auto make_response_policy(const retriever_request &request)
    -> response_policy {
  return response_policy{request.options.resolve(), request.sub_index};
}

[[nodiscard]] inline auto apply_response_policy(retriever_result result,
                                                const response_policy &policy)
    -> retriever_result {
  if (result.has_error()) {
    return result;
  }

  auto merged = std::move(result).value();
  const auto &options = policy.options;

  if (options.merge_policy == recall_merge_policy::dedupe_by_content) {
    std::unordered_set<std::string, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>
        seen{};
    seen.reserve(merged.size());
    std::vector<wh::schema::document> deduped{};
    deduped.reserve(merged.size());
    for (auto &doc : merged) {
      auto [_, inserted] = seen.emplace(doc.content());
      if (inserted) {
        deduped.push_back(std::move(doc));
      }
    }
    merged = std::move(deduped);
  }

  const auto filtered_begin =
      std::ranges::remove_if(merged, [&](const wh::schema::document &document) {
        if (document.score() < options.score_threshold) {
          return true;
        }
        if (!policy.sub_index.empty() &&
            document.sub_index() != policy.sub_index) {
          return true;
        }
        if (!options.dsl.empty() && document.dsl() != options.dsl) {
          return true;
        }
        return !matches_filter_expression(document, options.filter);
      });
  merged.erase(filtered_begin.begin(), filtered_begin.end());

  if (merged.size() > options.top_k) {
    merged.resize(options.top_k);
  }

  return merged;
}

template <typename impl_t, typename request_t>
  requires async_retriever_handler<impl_t>
[[nodiscard]] inline auto make_impl_sender(const impl_t &impl,
                                           request_t &&request) {
  using request_value_t = std::remove_cvref_t<request_t>;
  static_assert(std::same_as<request_value_t, retriever_request>,
                "retriever sender factory requires retriever_request input");
  return wh::core::detail::request_result_sender<retriever_result>(
      std::forward<request_t>(request),
      [&impl](auto &&forwarded_request) -> decltype(auto) {
        return impl.retrieve_sender(
            std::forward<decltype(forwarded_request)>(forwarded_request));
      });
}

template <wh::core::resume_mode Resume, typename impl_t, typename request_t,
          typename scheduler_t>
[[nodiscard]] inline auto
make_async_sender(const impl_t &impl, request_t &&request, callback_sink sink,
                  scheduler_t scheduler) {
  auto policy = make_response_policy(request);
  return wh::core::detail::component_async_entry<Resume>(
      std::forward<request_t>(request), std::move(sink), std::move(scheduler),
      [&impl](auto &&forwarded_request) {
        return make_impl_sender(
            impl, std::forward<decltype(forwarded_request)>(forwarded_request));
      },
      [](const retriever_request &state_request) {
        return make_callback_state(state_request);
      },
      [](const callback_sink &start_sink,
         const retriever_callback_state &state) {
        emit_callback(start_sink, wh::callbacks::stage::start, state);
      },
      [policy = std::move(policy)](const callback_sink &success_sink,
                                   retriever_callback_state &state,
                                   retriever_result &status) mutable {
        status = apply_response_policy(std::move(status), policy);
        state.event.extra = std::to_string(status.value().size());
        emit_callback(success_sink, wh::callbacks::stage::end, state);
      },
      [](const callback_sink &error_sink, retriever_callback_state &state,
         retriever_result &) {
        emit_callback(error_sink, wh::callbacks::stage::error, state);
      });
}

} // namespace detail

/// Retriever output document collection.
using retriever_response = std::vector<wh::schema::document>;
/// Public interface for `retriever`.
template <detail::retriever_impl impl_t,
          wh::core::resume_mode Resume = wh::core::resume_mode::unchanged>
class retriever {
public:
  /// Stores one retriever implementation object by value.
  explicit retriever(const impl_t &impl)
    requires std::copy_constructible<impl_t>
      : impl_(impl) {}

  /// Stores one movable retriever implementation object by value.
  explicit retriever(impl_t &&impl) noexcept(
      std::is_nothrow_move_constructible_v<impl_t>)
      : impl_(std::move(impl)) {}

  retriever(const retriever &) = default;
  retriever(retriever &&) noexcept = default;
  auto operator=(const retriever &) -> retriever & = default;
  auto operator=(retriever &&) noexcept -> retriever & = default;
  ~retriever() = default;

  /// Returns static descriptor metadata for this component.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"Retriever",
                                          wh::core::component_kind::retriever};
  }

  /// Retrieves matching documents and emits callbacks through the run context.
  [[nodiscard]] auto retrieve(const retriever_request &request,
                              wh::core::run_context &callback_context) const
      -> detail::retriever_result
    requires detail::sync_retriever_handler<impl_t>
  {
    return retrieve_sync_impl(request,
                              detail::borrow_callback_sink(callback_context));
  }

  /// Retrieves matching documents for movable owning request and emits
  /// callbacks through the run context.
  [[nodiscard]] auto retrieve(retriever_request &&request,
                              wh::core::run_context &callback_context) const
      -> detail::retriever_result
    requires detail::sync_retriever_handler<impl_t>
  {
    return retrieve_sync_impl(std::move(request),
                              detail::borrow_callback_sink(callback_context));
  }

  /// Retrieves matching documents asynchronously and emits callbacks through
  /// the run context.
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, retriever_request> &&
             detail::async_retriever_handler<impl_t>
  [[nodiscard]] auto
  async_retrieve(request_t &&request,
                 wh::core::run_context &callback_context) const -> auto {
    return retrieve_async_impl(std::forward<request_t>(request),
                               detail::make_callback_sink(callback_context));
  }

  /// Returns the stored implementation object.
  [[nodiscard]] auto impl() const noexcept -> const impl_t & { return impl_; }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, retriever_request> &&
             detail::sync_retriever_handler<impl_t>
  [[nodiscard]] auto retrieve_sync_impl(request_t &&request,
                                        detail::callback_sink sink) const
      -> detail::retriever_result {
    sink =
        wh::callbacks::filter_callback_sink(std::move(sink), request.options);
    auto policy = detail::make_response_policy(request);
    auto callback_state = detail::make_callback_state(request);
    detail::emit_callback(sink, wh::callbacks::stage::start, callback_state);

    auto output = detail::run_sync_retriever_impl(
        impl_, std::forward<request_t>(request));
    output = detail::apply_response_policy(std::move(output), policy);
    if (output.has_error()) {
      detail::emit_callback(sink, wh::callbacks::stage::error, callback_state);
      return output;
    }

    callback_state.event.extra = std::to_string(output.value().size());
    detail::emit_callback(sink, wh::callbacks::stage::end, callback_state);
    return output;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, retriever_request> &&
             detail::async_retriever_handler<impl_t>
  [[nodiscard]] auto retrieve_async_impl(request_t &&request,
                                         detail::callback_sink sink) const
      -> auto {
    return wh::core::detail::defer_resume_sender<Resume>(
        [this, request = retriever_request{std::forward<request_t>(request)},
         sink = std::move(sink)](auto scheduler) mutable {
          return detail::make_async_sender<Resume>(
              impl_, std::move(request), std::move(sink), std::move(scheduler));
        });
  }

  /// Stored retriever implementation object.
  [[no_unique_address]] impl_t impl_;
};

template <typename impl_t>
retriever(impl_t &&) -> retriever<std::remove_cvref_t<impl_t>>;

} // namespace wh::retriever
