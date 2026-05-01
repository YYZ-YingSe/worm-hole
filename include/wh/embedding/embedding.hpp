// Defines embedding component contracts for batch embedding execution.
#pragma once

#include <concepts>
#include <cstdint>
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
#include "wh/embedding/callback_event.hpp"
#include "wh/embedding/options.hpp"

namespace wh::core {
struct run_context;
}

namespace wh::embedding {

/// Data contract for `embedding_request`.
struct embedding_request {
  /// Input texts to be embedded.
  std::vector<std::string> inputs{};
  /// Embedding options including provider-specific overrides.
  embedding_options options{};
};

/// Batch embedding output vectors, one entry per input text.
using embedding_response = std::vector<std::vector<double>>;
namespace detail {

using embedding_result = wh::core::result<embedding_response>;

struct embedding_callback_state {
  wh::callbacks::run_info run_info{};
  embedding_callback_event event{};
};

[[nodiscard]] inline auto make_callback_state(const embedding_request &request)
    -> embedding_callback_state {
  embedding_callback_state state{};
  state.run_info.name = "Embedding";
  state.run_info.type = "Embedding";
  state.run_info.component = wh::core::component_kind::embedding;
  state.run_info =
      wh::callbacks::apply_component_run_info(std::move(state.run_info), request.options);

  const auto options = request.options.resolve_view();
  state.event.model_id = options.model_id;
  state.event.batch_size = request.inputs.size();
  state.event.usage.prompt_tokens = static_cast<std::int64_t>(request.inputs.size());
  state.event.usage.total_tokens = state.event.usage.prompt_tokens;
  return state;
}

using callback_sink = wh::callbacks::callback_sink;
using wh::callbacks::borrow_callback_sink;
using wh::callbacks::make_callback_sink;

template <typename... args_t> inline auto emit_callback(args_t &&...args) -> void {
  wh::callbacks::emit(std::forward<args_t>(args)...);
}

template <typename impl_t>
concept sync_embedding_handler = requires(const impl_t &impl, const embedding_request &request) {
  { impl.embed(request) } -> std::same_as<embedding_result>;
};

template <typename impl_t>
concept movable_sync_embedding_handler = requires(const impl_t &impl, embedding_request &&request) {
  { impl.embed(std::move(request)) } -> std::same_as<embedding_result>;
};

template <typename impl_t>
concept sender_embedding_handler_const =
    requires(const impl_t &impl, const embedding_request &request) {
      { impl.embed_sender(request) } -> stdexec::sender;
    };

template <typename impl_t>
concept sender_embedding_handler_move = requires(const impl_t &impl, embedding_request &&request) {
  { impl.embed_sender(std::move(request)) } -> stdexec::sender;
};

template <typename impl_t>
concept async_embedding_handler =
    sender_embedding_handler_const<impl_t> || sender_embedding_handler_move<impl_t>;

template <typename impl_t>
concept sender_embedding_handler = async_embedding_handler<impl_t>;

template <typename impl_t>
concept embedding_impl = async_embedding_handler<impl_t> || sync_embedding_handler<impl_t>;

template <typename impl_t>
[[nodiscard]] inline auto run_sync_embedding_impl(const impl_t &impl,
                                                  const embedding_request &request)
    -> embedding_result {
  if constexpr (requires {
                  { impl.embed(request) } -> std::same_as<embedding_result>;
                }) {
    return impl.embed(request);
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_embedding_impl(const impl_t &impl, embedding_request &&request)
    -> embedding_result {
  if constexpr (requires {
                  { impl.embed(std::move(request)) } -> std::same_as<embedding_result>;
                }) {
    return impl.embed(std::move(request));
  } else {
    return run_sync_embedding_impl(impl, request);
  }
}

template <typename impl_t, typename request_t>
  requires async_embedding_handler<impl_t>
[[nodiscard]] inline auto make_impl_sender(const impl_t &impl, request_t &&request) {
  using request_value_t = std::remove_cvref_t<request_t>;
  static_assert(std::same_as<request_value_t, embedding_request>,
                "embedding sender factory requires embedding_request input");
  return wh::core::detail::request_result_sender<embedding_result>(
      std::forward<request_t>(request), [&impl](auto &&forwarded_request) -> decltype(auto) {
        return impl.embed_sender(std::forward<decltype(forwarded_request)>(forwarded_request));
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
      [](const embedding_request &state_request) { return make_callback_state(state_request); },
      [](const callback_sink &start_sink, const embedding_callback_state &state) {
        emit_callback(start_sink, wh::callbacks::stage::start, state);
      },
      [](const callback_sink &success_sink, embedding_callback_state &state,
         embedding_result &status) {
        state.event.usage.completion_tokens = static_cast<std::int64_t>(status.value().size());
        state.event.usage.total_tokens =
            state.event.usage.prompt_tokens + state.event.usage.completion_tokens;
        emit_callback(success_sink, wh::callbacks::stage::end, state);
      },
      [](const callback_sink &error_sink, embedding_callback_state &state, embedding_result &) {
        emit_callback(error_sink, wh::callbacks::stage::error, state);
      });
}

} // namespace detail

/// Public interface for `embedding`.
template <detail::embedding_impl impl_t,
          wh::core::resume_mode Resume = wh::core::resume_mode::unchanged>
class embedding {
public:
  /// Stores one embedding implementation object by value.
  explicit embedding(const impl_t &impl)
    requires std::copy_constructible<impl_t>
      : impl_(impl) {}

  /// Stores one movable embedding implementation object by value.
  explicit embedding(impl_t &&impl) noexcept(std::is_nothrow_move_constructible_v<impl_t>)
      : impl_(std::move(impl)) {}

  embedding(const embedding &) = default;
  embedding(embedding &&) noexcept = default;
  auto operator=(const embedding &) -> embedding & = default;
  auto operator=(embedding &&) noexcept -> embedding & = default;
  ~embedding() = default;

  /// Returns static descriptor metadata for this component.
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"Embedding", wh::core::component_kind::embedding};
  }

  /// Generates embeddings synchronously and emits callbacks through the run
  /// context.
  [[nodiscard]] auto embed(const embedding_request &request,
                           wh::core::run_context &callback_context) const
      -> detail::embedding_result
    requires detail::sync_embedding_handler<impl_t>
  {
    return embed_sync_impl(request, detail::borrow_callback_sink(callback_context));
  }

  /// Generates embeddings synchronously for movable owning request inputs and
  /// emits callbacks through the run context.
  [[nodiscard]] auto embed(embedding_request &&request,
                           wh::core::run_context &callback_context) const
      -> detail::embedding_result
    requires detail::sync_embedding_handler<impl_t>
  {
    return embed_sync_impl(std::move(request), detail::borrow_callback_sink(callback_context));
  }

  /// Generates embeddings asynchronously and emits callbacks through the run
  /// context.
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, embedding_request> &&
             detail::async_embedding_handler<impl_t>
  [[nodiscard]] auto async_embed(request_t &&request, wh::core::run_context &callback_context) const
      -> auto {
    return embed_async_impl(std::forward<request_t>(request),
                            detail::make_callback_sink(callback_context));
  }

  /// Returns the stored implementation object.
  [[nodiscard]] auto impl() const noexcept -> const impl_t & { return impl_; }

private:
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, embedding_request> &&
             detail::sync_embedding_handler<impl_t>
  [[nodiscard]] auto embed_sync_impl(request_t &&request, detail::callback_sink sink) const
      -> detail::embedding_result {
    sink = wh::callbacks::filter_callback_sink(std::move(sink), request.options);
    auto state = detail::make_callback_state(request);
    detail::emit_callback(sink, wh::callbacks::stage::start, state);

    auto output = detail::run_sync_embedding_impl(impl_, std::forward<request_t>(request));
    if (output.has_error()) {
      detail::emit_callback(sink, wh::callbacks::stage::error, state);
      return output;
    }

    state.event.usage.completion_tokens = static_cast<std::int64_t>(output.value().size());
    state.event.usage.total_tokens =
        state.event.usage.prompt_tokens + state.event.usage.completion_tokens;
    detail::emit_callback(sink, wh::callbacks::stage::end, state);
    return output;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, embedding_request> &&
             detail::async_embedding_handler<impl_t>
  [[nodiscard]] auto embed_async_impl(request_t &&request, detail::callback_sink sink) const
      -> auto {
    return wh::core::detail::defer_resume_sender<Resume>(
        [this, request = embedding_request{std::forward<request_t>(request)},
         sink = std::move(sink)](auto scheduler) mutable {
          return detail::make_async_sender<Resume>(impl_, std::move(request), std::move(sink),
                                                   std::move(scheduler));
        });
  }

  /// Stored embedding implementation object.
  wh_no_unique_address impl_t impl_;
};

template <typename impl_t> embedding(impl_t &&) -> embedding<std::remove_cvref_t<impl_t>>;

} // namespace wh::embedding
