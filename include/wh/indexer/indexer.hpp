// Defines indexer component contracts for inserting and updating document
// vectors/metadata into backing retrieval indexes.
#pragma once

#include <concepts>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/compiler.hpp"
#include "wh/core/component/types.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec/component_async_entry.hpp"
#include "wh/core/stdexec/request_result_sender.hpp"
#include "wh/core/stdexec/resume_policy.hpp"
#include "wh/indexer/callback_event.hpp"
#include "wh/indexer/options.hpp"
#include "wh/schema/document.hpp"

namespace wh::core {
struct run_context;
}

namespace wh::indexer {

/// Data contract for `indexer_request`.
struct indexer_request {
  /// Documents to insert/update in the index.
  std::vector<wh::schema::document> documents{};
  /// Optional embedding vector used for combined index path.
  std::vector<double> embedding{};
  /// Indexer options (retry/failure policy/combined-index toggles).
  indexer_options options{};
};

/// Data contract for `indexer_response`.
struct indexer_response {
  /// IDs of successfully written documents.
  std::vector<std::string> document_ids{};
  /// Count of successful writes.
  std::size_t success_count{0U};
  /// Count of failed writes.
  std::size_t failure_count{0U};
};

namespace detail {

using indexer_result = wh::core::result<indexer_response>;
struct indexer_callback_state {
  wh::callbacks::run_info run_info{};
  indexer_callback_event event{};
};

using callback_sink = wh::callbacks::callback_sink;
using wh::callbacks::borrow_callback_sink;
using wh::callbacks::make_callback_sink;

template <typename... args_t> inline auto emit_callback(args_t &&...args) -> void {
  wh::callbacks::emit(std::forward<args_t>(args)...);
}

template <typename impl_t>
concept sync_batch_indexer_handler = requires(const impl_t &impl, const indexer_request &request) {
  { impl.write(request) } -> std::same_as<indexer_result>;
};

template <typename impl_t>
concept sync_single_indexer_handler = requires(
    const impl_t &impl, const wh::schema::document &document, const indexer_options &options) {
  { impl.write_one(document, options) } -> std::same_as<wh::core::result<std::string>>;
};

template <typename impl_t>
concept sender_indexer_handler_const =
    requires(const impl_t &impl, const indexer_request &request) {
      { impl.write_sender(request) } -> stdexec::sender;
    };

template <typename impl_t>
concept sender_indexer_handler_move = requires(const impl_t &impl, indexer_request &&request) {
  { impl.write_sender(std::move(request)) } -> stdexec::sender;
};

template <typename impl_t>
concept async_indexer_handler =
    sender_indexer_handler_const<impl_t> || sender_indexer_handler_move<impl_t>;

template <typename impl_t>
concept sender_indexer_handler = async_indexer_handler<impl_t>;

template <typename impl_t>
concept indexer_impl = async_indexer_handler<impl_t> || sync_batch_indexer_handler<impl_t> ||
                       sync_single_indexer_handler<impl_t>;

[[nodiscard]] inline auto make_callback_state(const indexer_request &request)
    -> indexer_callback_state {
  indexer_callback_state state{};
  state.run_info.name = "Indexer";
  state.run_info.type = "Indexer";
  state.run_info.component = wh::core::component_kind::indexer;
  state.run_info =
      wh::callbacks::apply_component_run_info(std::move(state.run_info), request.options);
  state.event.batch_size = request.documents.size();
  return state;
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_batch_indexer_impl(const impl_t &impl,
                                                      const indexer_request &request)
    -> indexer_result {
  if constexpr (requires {
                  { impl.write(request) } -> std::same_as<indexer_result>;
                }) {
    return impl.write(request);
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_batch_indexer_impl(const impl_t &impl, indexer_request &&request)
    -> indexer_result {
  if constexpr (requires {
                  { impl.write(std::move(request)) } -> std::same_as<indexer_result>;
                }) {
    return impl.write(std::move(request));
  } else {
    return run_sync_batch_indexer_impl(impl, request);
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_single_indexer_impl(const impl_t &impl,
                                                       const wh::schema::document &document,
                                                       const indexer_options &options)
    -> wh::core::result<std::string> {
  if constexpr (requires {
                  {
                    impl.write_one(document, options)
                  } -> std::same_as<wh::core::result<std::string>>;
                }) {
    return impl.write_one(document, options);
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_indexer_impl(const impl_t &impl, const indexer_request &request)
    -> indexer_result {
  if constexpr (sync_batch_indexer_handler<impl_t>) {
    return run_sync_batch_indexer_impl(impl, request);
  } else {
    const auto options = request.options.resolve_view();
    if (options.combine_with_embedding &&
        (options.embedding_model.empty() || request.embedding.empty())) {
      return indexer_result::failure(wh::core::errc::invalid_argument);
    }

    indexer_response response{};
    response.document_ids.reserve(request.documents.size());
    for (const auto &document : request.documents) {
      auto retries = options.max_retries;
      while (true) {
        auto status = run_sync_single_indexer_impl(impl, document, request.options);
        if (status.has_value()) {
          response.document_ids.push_back(std::move(status).value());
          ++response.success_count;
          break;
        }
        if (retries == 0U) {
          ++response.failure_count;
          if (options.failure_policy == write_failure_policy::stop) {
            return indexer_result::failure(status.error());
          }
          if (options.failure_policy == write_failure_policy::retry) {
            return indexer_result::failure(wh::core::errc::retry_exhausted);
          }
          break;
        }
        --retries;
      }
    }
    return response;
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_indexer_impl(const impl_t &impl, indexer_request &&request)
    -> indexer_result {
  if constexpr (sync_batch_indexer_handler<impl_t>) {
    return run_sync_batch_indexer_impl(impl, std::move(request));
  } else {
    return run_sync_indexer_impl(impl, static_cast<const indexer_request &>(request));
  }
}

template <typename impl_t, typename request_t>
  requires async_indexer_handler<impl_t>
[[nodiscard]] inline auto make_impl_sender(const impl_t &impl, request_t &&request) {
  using request_value_t = std::remove_cvref_t<request_t>;
  static_assert(std::same_as<request_value_t, indexer_request>,
                "indexer sender factory requires indexer_request input");
  return wh::core::detail::request_result_sender<indexer_result>(
      std::forward<request_t>(request), [&impl](auto &&forwarded_request) -> decltype(auto) {
        return impl.write_sender(std::forward<decltype(forwarded_request)>(forwarded_request));
      });
}

template <wh::core::resume_mode Resume, typename impl_t, typename request_t, typename scheduler_t>
[[nodiscard]] inline auto make_async_sender(const impl_t &impl, request_t &&request,
                                            callback_sink sink, scheduler_t scheduler) {
  return wh::core::detail::component_async_entry<Resume>(
      std::forward<request_t>(request), std::move(sink), std::move(scheduler),
      [&impl](auto &&forwarded_request) {
        return make_impl_sender(impl, std::forward<decltype(forwarded_request)>(forwarded_request));
      },
      [](const indexer_request &state_request) { return make_callback_state(state_request); },
      [](const callback_sink &start_sink, const indexer_callback_state &state) {
        emit_callback(start_sink, wh::callbacks::stage::start, state);
      },
      [](const callback_sink &success_sink, indexer_callback_state &state, indexer_result &status) {
        state.event.success_count = status.value().success_count;
        state.event.failure_count = status.value().failure_count;
        emit_callback(success_sink, wh::callbacks::stage::end, state);
      },
      [](const callback_sink &error_sink, indexer_callback_state &state, indexer_result &) {
        emit_callback(error_sink, wh::callbacks::stage::error, state);
      });
}

} // namespace detail

/// Public interface for `indexer`.
template <detail::indexer_impl impl_t,
          wh::core::resume_mode Resume = wh::core::resume_mode::unchanged>
class indexer {
public:
  /// Stores one indexer implementation object by value.
  explicit indexer(const impl_t &impl)
    requires std::copy_constructible<impl_t>
      : impl_(impl) {}

  /// Stores one movable indexer implementation object by value.
  explicit indexer(impl_t &&impl) noexcept(std::is_nothrow_move_constructible_v<impl_t>)
      : impl_(std::move(impl)) {}

  indexer(const indexer &) = default;
  indexer(indexer &&) noexcept = default;
  auto operator=(const indexer &) -> indexer & = default;
  auto operator=(indexer &&) noexcept -> indexer & = default;
  ~indexer() = default;

  /// Returns static descriptor metadata for this component.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"Indexer", wh::core::component_kind::indexer};
  }

  /// Writes documents synchronously and emits callbacks through the run
  /// context.
  [[nodiscard]] auto write(const indexer_request &request,
                           wh::core::run_context &callback_context) const -> detail::indexer_result
    requires detail::sync_batch_indexer_handler<impl_t> ||
             detail::sync_single_indexer_handler<impl_t>
  {
    return write_sync_impl(request, detail::borrow_callback_sink(callback_context));
  }

  /// Writes documents synchronously for movable request and emits callbacks
  /// through the run context.
  [[nodiscard]] auto write(indexer_request &&request, wh::core::run_context &callback_context) const
      -> detail::indexer_result
    requires detail::sync_batch_indexer_handler<impl_t> ||
             detail::sync_single_indexer_handler<impl_t>
  {
    return write_sync_impl(std::move(request), detail::borrow_callback_sink(callback_context));
  }

  /// Writes documents asynchronously and emits callbacks through the run
  /// context.
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, indexer_request> &&
             detail::async_indexer_handler<impl_t>
  [[nodiscard]] auto async_write(request_t &&request, wh::core::run_context &callback_context) const
      -> auto {
    return write_async_impl(std::forward<request_t>(request),
                            detail::make_callback_sink(callback_context));
  }

  /// Returns the stored implementation object.
  [[nodiscard]] auto impl() const noexcept -> const impl_t & { return impl_; }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, indexer_request> &&
             (detail::sync_batch_indexer_handler<impl_t> ||
              detail::sync_single_indexer_handler<impl_t>)
  [[nodiscard]] auto write_sync_impl(request_t &&request, detail::callback_sink sink) const
      -> detail::indexer_result {
    sink = wh::callbacks::filter_callback_sink(std::move(sink), request.options);
    auto callback_state = detail::make_callback_state(request);
    detail::emit_callback(sink, wh::callbacks::stage::start, callback_state);

    auto output = detail::run_sync_indexer_impl(impl_, std::forward<request_t>(request));
    if (output.has_error()) {
      detail::emit_callback(sink, wh::callbacks::stage::error, callback_state);
      return output;
    }

    callback_state.event.success_count = output.value().success_count;
    callback_state.event.failure_count = output.value().failure_count;
    detail::emit_callback(sink, wh::callbacks::stage::end, callback_state);
    return output;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, indexer_request> &&
             detail::async_indexer_handler<impl_t>
  [[nodiscard]] auto write_async_impl(request_t &&request, detail::callback_sink sink) const
      -> auto {
    return wh::core::detail::defer_resume_sender<Resume>(
        [this, request = indexer_request{std::forward<request_t>(request)},
         sink = std::move(sink)](auto scheduler) mutable {
          return detail::make_async_sender<Resume>(impl_, std::move(request), std::move(sink),
                                                   std::move(scheduler));
        });
  }

  /// Stored indexer implementation object.
  wh_no_unique_address impl_t impl_;
};

template <typename impl_t> indexer(impl_t &&) -> indexer<std::remove_cvref_t<impl_t>>;

} // namespace wh::indexer
