// Defines a chain executor that renders prompt messages and invokes/streams
// the chat model with unified callbacks and error propagation.
#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/callbacks/callbacks.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/type_utils.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/prompt/chat_template.hpp"

namespace wh::chain {

namespace detail {

template <typename prompt_holder_t>
using prompt_component_t =
    wh::core::remove_cvref_t<decltype(wh::core::deref_or_self(
        std::declval<const prompt_holder_t &>()))>;

template <typename model_holder_t>
using model_component_t =
    wh::core::remove_cvref_t<decltype(wh::core::deref_or_self(
        std::declval<const model_holder_t &>()))>;

template <typename prompt_holder_t>
concept prompt_holder =
    wh::prompt::chat_template_like<prompt_component_t<prompt_holder_t>>;

template <typename model_holder_t>
concept model_holder =
    wh::model::chat_model_like<model_component_t<model_holder_t>>;

template <typename prompt_holder_t>
concept async_prompt_holder =
    prompt_holder<prompt_holder_t> &&
    requires(const prompt_component_t<prompt_holder_t> &prompt,
             wh::prompt::prompt_render_request request,
             wh::core::run_context &context) {
      { prompt.async_render(std::move(request), context) } -> stdexec::sender;
    };

template <typename model_holder_t>
concept async_invoke_model_holder =
    model_holder<model_holder_t> &&
    requires(const model_component_t<model_holder_t> &model,
             wh::model::chat_request request, wh::core::run_context &context) {
      { model.async_invoke(std::move(request), context) } -> stdexec::sender;
    };

template <typename model_holder_t>
concept async_stream_model_holder =
    model_holder<model_holder_t> &&
    requires(const model_component_t<model_holder_t> &model,
             wh::model::chat_request request, wh::core::run_context &context) {
      { model.async_stream(std::move(request), context) } -> stdexec::sender;
    };

using model_request_result = wh::core::result<wh::model::chat_request>;

[[nodiscard]] inline auto
make_callback_context(const wh::callbacks::callback_sink &sink)
    -> wh::core::run_context {
  wh::core::run_context callback_context{};
  if (const auto *manager = sink.manager_ptr(); manager != nullptr) {
    callback_context.callbacks = wh::core::callback_runtime{
        .manager = *manager,
    };
  }
  if (sink.metadata.has_value()) {
    if (!callback_context.callbacks.has_value()) {
      callback_context.callbacks.emplace();
    }
    callback_context.callbacks->metadata = *sink.metadata;
  }
  return callback_context;
}

template <typename prompt_t>
[[nodiscard]] inline auto render_model_request(
    const prompt_t &prompt, wh::prompt::prompt_render_request request,
    wh::model::chat_model_options options, wh::callbacks::callback_sink sink);

template <typename prompt_holder_t, bool Enabled>
struct rendered_model_request_sender_selector {
  using type = wh::core::detail::ready_sender_t<model_request_result>;
};

template <typename prompt_holder_t>
struct rendered_model_request_sender_selector<prompt_holder_t, true> {
  using type = decltype(render_model_request(
      std::declval<const prompt_component_t<prompt_holder_t> &>(),
      std::declval<wh::prompt::prompt_render_request>(),
      std::declval<wh::model::chat_model_options>(),
      std::declval<wh::callbacks::callback_sink>()));
};

template <typename prompt_holder_t>
using rendered_model_request_sender_t =
    typename rendered_model_request_sender_selector<
        prompt_holder_t, async_prompt_holder<prompt_holder_t>>::type;

template <typename prompt_t>
[[nodiscard]] inline auto render_model_request(
    const prompt_t &prompt, wh::prompt::prompt_render_request request,
    wh::model::chat_model_options options, wh::callbacks::callback_sink sink) {
  auto callback_context = make_callback_context(sink);
  return wh::core::detail::map_result_sender<model_request_result>(
      prompt.async_render(std::move(request), callback_context),
      [options = std::move(options)](
          std::vector<wh::schema::message> messages) mutable {
        wh::model::chat_request model_request{};
        model_request.options = std::move(options);
        model_request.messages = std::move(messages);
        return model_request;
      });
}

} // namespace detail

/// Data contract for `chat_chain_callback_event`.
struct chat_chain_callback_event {
  /// True when request uses stream path instead of invoke path.
  bool stream_path{false};
  /// Number of input messages forwarded to the model.
  std::size_t input_message_count{0U};
  /// Number of output messages/chunks produced by the chain.
  std::size_t output_message_count{0U};
};

/// Data contract for `chat_chain_request`.
struct chat_chain_request {
  /// Template placeholder values used by prompt rendering.
  wh::prompt::template_context context{};
  /// Direct conversation messages that bypass prompt rendering when non-empty.
  std::vector<wh::schema::message> messages{};
  /// Prompt-layer options for template rendering behavior.
  wh::prompt::prompt_options prompt_options{};
  /// Model-layer options for invocation/stream execution.
  wh::model::chat_model_options model_options{};
};

/// Public API for `chat_chain`.
template <typename Prompt, typename Model>
  requires(detail::prompt_holder<Prompt> && detail::model_holder<Model>)
class chat_chain {
public:
  chat_chain(const Prompt &prompt, const Model &model)
      : prompt_(prompt), model_(model) {}
  chat_chain(Prompt &&prompt, Model &&model)
      : prompt_(std::move(prompt)), model_(std::move(model)) {}

  /// Returns static descriptor metadata for this chain.
  [[nodiscard]] static auto descriptor() -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"ChatChain",
                                          wh::core::component_kind::chain};
  }

  /// Runs the primary request path and emits callbacks through the run context.
  [[nodiscard]] auto invoke(const chat_chain_request &request,
                            wh::core::run_context &callback_context) const
      -> wh::core::result<wh::model::chat_response> {
    return invoke_impl(request,
                       wh::callbacks::borrow_callback_sink(callback_context));
  }

  /// Runs the primary request path with movable owning request and emits
  /// callbacks through the run context.
  [[nodiscard]] auto invoke(chat_chain_request &&request,
                            wh::core::run_context &callback_context) const
      -> wh::core::result<wh::model::chat_response> {
    return invoke_impl(std::move(request),
                       wh::callbacks::borrow_callback_sink(callback_context));
  }

  /// Starts streaming execution and emits callbacks through the run context.
  [[nodiscard]] auto stream(const chat_chain_request &request,
                            wh::core::run_context &callback_context) const
      -> wh::core::result<wh::model::chat_stream_reader> {
    return stream_impl(request,
                       wh::callbacks::borrow_callback_sink(callback_context));
  }

  /// Starts streaming execution with movable owning request and emits
  /// callbacks through the run context.
  [[nodiscard]] auto stream(chat_chain_request &&request,
                            wh::core::run_context &callback_context) const
      -> wh::core::result<wh::model::chat_stream_reader> {
    return stream_impl(std::move(request),
                       wh::callbacks::borrow_callback_sink(callback_context));
  }

  /// Runs the primary request path asynchronously and emits callbacks through
  /// the run context.
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_chain_request> &&
             detail::async_prompt_holder<Prompt> &&
             detail::async_invoke_model_holder<Model>
  [[nodiscard]] auto async_invoke(request_t &&request,
                                  wh::core::run_context &callback_context) const
      -> auto {
    return invoke_async_impl(
        std::forward<request_t>(request),
        wh::callbacks::make_callback_sink(callback_context));
  }

  /// Starts streaming execution asynchronously and emits callbacks through the
  /// run context.
  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_chain_request> &&
             detail::async_prompt_holder<Prompt> &&
             detail::async_stream_model_holder<Model>
  [[nodiscard]] auto async_stream(request_t &&request,
                                  wh::core::run_context &callback_context) const
      -> auto {
    return stream_async_impl(
        std::forward<request_t>(request),
        wh::callbacks::make_callback_sink(callback_context));
  }

private:
  [[nodiscard]] static auto make_run_info() -> wh::callbacks::run_info {
    const auto component_descriptor = descriptor();
    wh::callbacks::run_info run_info{};
    run_info.name = component_descriptor.type_name;
    run_info.type = component_descriptor.type_name;
    run_info.component = component_descriptor.kind;
    return run_info;
  }

  [[nodiscard]] static auto
  make_callback_event(const chat_chain_request &request, const bool stream_path)
      -> chat_chain_callback_event {
    chat_chain_callback_event event{};
    event.stream_path = stream_path;
    event.input_message_count = request.messages.size();
    return event;
  }

  using model_invoke_result = wh::core::result<wh::model::chat_response>;
  using model_stream_result = wh::core::result<wh::model::chat_stream_reader>;

  struct async_state {
    wh::callbacks::callback_sink sink{};
    wh::callbacks::run_info run_info{};
    chat_chain_callback_event event{};
  };

  template <bool Stream>
  using async_result_t =
      std::conditional_t<Stream, model_stream_result, model_invoke_result>;

  template <bool Stream>
  [[nodiscard]] static auto finish_async(async_state state,
                                         async_result_t<Stream> status)
      -> async_result_t<Stream> {
    if (status.has_error()) {
      wh::callbacks::emit(state.sink, wh::callbacks::stage::error, state.event,
                          state.run_info);
      return status;
    }
    state.event.output_message_count = 1U;
    wh::callbacks::emit(state.sink, wh::callbacks::stage::end, state.event,
                        state.run_info);
    return status;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_chain_request>
  [[nodiscard]] auto invoke_impl(request_t &&request,
                                 wh::callbacks::callback_sink sink) const
      -> wh::core::result<wh::model::chat_response> {
    auto run_info = make_run_info();
    auto callback_event = make_callback_event(request, false);
    wh::callbacks::emit(sink, wh::callbacks::stage::start, callback_event,
                        run_info);

    auto model_request =
        make_model_request(std::forward<request_t>(request), sink);
    if (model_request.has_error()) {
      wh::callbacks::emit(sink, wh::callbacks::stage::error, callback_event,
                          run_info);
      return wh::core::result<wh::model::chat_response>::failure(
          model_request.error());
    }

    callback_event.input_message_count = model_request.value().messages.size();
    auto invoked = invoke_model(std::move(model_request).value(), sink);
    if (invoked.has_error()) {
      wh::callbacks::emit(sink, wh::callbacks::stage::error, callback_event,
                          run_info);
      return invoked;
    }

    callback_event.output_message_count = 1U;
    wh::callbacks::emit(sink, wh::callbacks::stage::end, callback_event,
                        run_info);
    return invoked;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_chain_request>
  [[nodiscard]] auto stream_impl(request_t &&request,
                                 wh::callbacks::callback_sink sink) const
      -> wh::core::result<wh::model::chat_stream_reader> {
    auto run_info = make_run_info();
    auto callback_event = make_callback_event(request, true);
    wh::callbacks::emit(sink, wh::callbacks::stage::start, callback_event,
                        run_info);

    auto model_request =
        make_model_request(std::forward<request_t>(request), sink);
    if (model_request.has_error()) {
      wh::callbacks::emit(sink, wh::callbacks::stage::error, callback_event,
                          run_info);
      return wh::core::result<wh::model::chat_stream_reader>::failure(
          model_request.error());
    }

    callback_event.input_message_count = model_request.value().messages.size();
    auto streamed = stream_model(std::move(model_request).value(), sink);
    if (streamed.has_error()) {
      wh::callbacks::emit(sink, wh::callbacks::stage::error, callback_event,
                          run_info);
      return streamed;
    }

    callback_event.output_message_count = 1U;
    wh::callbacks::emit(sink, wh::callbacks::stage::end, callback_event,
                        run_info);
    return streamed;
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_chain_request> &&
             detail::async_prompt_holder<Prompt> &&
             detail::async_invoke_model_holder<Model>
  [[nodiscard]] auto invoke_async_impl(request_t &&request,
                                       wh::callbacks::callback_sink sink) const
      -> auto {
    return make_async_sender<false>(std::forward<request_t>(request),
                                    std::move(sink));
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_chain_request> &&
             detail::async_prompt_holder<Prompt> &&
             detail::async_stream_model_holder<Model>
  [[nodiscard]] auto stream_async_impl(request_t &&request,
                                       wh::callbacks::callback_sink sink) const
      -> auto {
    return make_async_sender<true>(std::forward<request_t>(request),
                                   std::move(sink));
  }

  template <bool Stream, typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, chat_chain_request>
  [[nodiscard]] auto make_async_sender(request_t &&request,
                                       wh::callbacks::callback_sink sink) const
      -> auto {
    auto state = std::optional<async_state>{};
    if (sink.has_value()) {
      state.emplace();
      state->sink = sink;
      state->run_info = make_run_info();
      state->event = make_callback_event(request, Stream);
    }
    return wh::core::read_resume_scheduler(
        [this, request = chat_chain_request{std::forward<request_t>(request)},
         state = std::move(state)](auto scheduler) mutable {
          return stdexec::schedule(std::move(scheduler)) |
                 stdexec::let_value([this, request = std::move(request),
                                     state = std::move(state)]() mutable {
                   if (state.has_value()) {
                     wh::callbacks::emit(state->sink,
                                         wh::callbacks::stage::start,
                                         state->event, state->run_info);
                   }

                   auto model_request =
                       make_model_request_sender(
                           std::move(request),
                           state.has_value() ? state->sink
                                             : wh::callbacks::callback_sink{}) |
                       stdexec::then([state = std::move(state)](
                                         detail::model_request_result
                                             model_request) mutable {
                         if (state.has_value() && !model_request.has_error()) {
                           state->event.input_message_count =
                               model_request.value().messages.size();
                         }
                         return std::pair{std::move(state),
                                          std::move(model_request)};
                       });

                   return std::move(model_request) |
                          stdexec::let_value(
                              [this](std::pair<std::optional<async_state>,
                                               detail::model_request_result>
                                         data) mutable {
                                auto state = std::move(data.first);
                                auto model_sender = make_model_sender<Stream>(
                                    std::move(data.second),
                                    state.has_value()
                                        ? state->sink
                                        : wh::callbacks::callback_sink{});
                                return std::move(model_sender) |
                                       stdexec::then([state = std::move(state)](
                                                         async_result_t<Stream>
                                                             status) mutable {
                                         if (!state.has_value()) {
                                           return status;
                                         }
                                         return finish_async<Stream>(
                                             std::move(*state),
                                             std::move(status));
                                       });
                              });
                 });
        });
  }

  template <typename prompt_t = Prompt>
    requires detail::async_prompt_holder<prompt_t>
  [[nodiscard]] auto
  make_model_request_sender(chat_chain_request request,
                            const wh::callbacks::callback_sink &sink) const
      -> auto {
    using model_request_sender_t = wh::core::detail::variant_sender<
        wh::core::detail::ready_sender_t<detail::model_request_result>,
        detail::rendered_model_request_sender_t<prompt_t>>;

    if (!wh::core::has_instance(prompt_) || !wh::core::has_instance(model_)) {
      return model_request_sender_t{
          wh::core::detail::failure_result_sender<detail::model_request_result>(
              wh::core::errc::not_found)};
    }

    wh::model::chat_request model_request{};
    model_request.options = std::move(request.model_options);
    const auto &prompt = wh::core::deref_or_self(prompt_);
    if (!request.messages.empty()) {
      model_request.messages = std::move(request.messages);
      return model_request_sender_t{wh::core::detail::ready_sender(
          detail::model_request_result{std::move(model_request)})};
    }

    return model_request_sender_t{detail::render_model_request(
        prompt,
        wh::prompt::prompt_render_request{std::move(request.context),
                                          std::move(request.prompt_options)},
        std::move(model_request.options), sink)};
  }

  template <bool Stream, typename model_t = Model>
    requires((!Stream && detail::async_invoke_model_holder<model_t>) ||
             (Stream && detail::async_stream_model_holder<model_t>))
  [[nodiscard]] auto
  make_model_sender(detail::model_request_result model_request,
                    const wh::callbacks::callback_sink &sink) const -> auto {
    using result_t = async_result_t<Stream>;
    const auto &model = wh::core::deref_or_self(model_);
    using failure_sender_t = wh::core::detail::ready_sender_t<result_t>;
    using model_sender_t = wh::core::detail::variant_sender<
        failure_sender_t, std::remove_cvref_t<decltype([&]() {
          auto callback_context = detail::make_callback_context(sink);
          if constexpr (Stream) {
            return model.async_stream(std::move(model_request).value(),
                                      callback_context);
          } else {
            return model.async_invoke(std::move(model_request).value(),
                                      callback_context);
          }
        }())>>;

    if (model_request.has_error()) {
      return model_sender_t{wh::core::detail::failure_result_sender<result_t>(
          model_request.error())};
    }

    auto callback_context = detail::make_callback_context(sink);
    if constexpr (Stream) {
      return model_sender_t{model.async_stream(std::move(model_request).value(),
                                               callback_context)};
    } else {
      return model_sender_t{model.async_invoke(std::move(model_request).value(),
                                               callback_context)};
    }
  }

  [[nodiscard]] auto
  make_model_request(const chat_chain_request &request,
                     const wh::callbacks::callback_sink &sink) const
      -> wh::core::result<wh::model::chat_request> {
    if (!wh::core::has_instance(prompt_) || !wh::core::has_instance(model_)) {
      return wh::core::result<wh::model::chat_request>::failure(
          wh::core::errc::not_found);
    }

    wh::model::chat_request model_request{};
    model_request.options = request.model_options;
    if (!request.messages.empty()) {
      model_request.messages = request.messages;
      return model_request;
    }

    auto rendered =
        render_prompt(wh::prompt::prompt_render_request{request.context,
                                                        request.prompt_options},
                      sink);
    if (rendered.has_error()) {
      return wh::core::result<wh::model::chat_request>::failure(
          rendered.error());
    }
    model_request.messages = std::move(rendered).value();
    return model_request;
  }

  [[nodiscard]] auto
  make_model_request(chat_chain_request &&request,
                     const wh::callbacks::callback_sink &sink) const
      -> wh::core::result<wh::model::chat_request> {
    if (!wh::core::has_instance(prompt_) || !wh::core::has_instance(model_)) {
      return wh::core::result<wh::model::chat_request>::failure(
          wh::core::errc::not_found);
    }

    wh::model::chat_request model_request{};
    model_request.options = std::move(request.model_options);
    if (!request.messages.empty()) {
      model_request.messages = std::move(request.messages);
      return model_request;
    }

    auto rendered = render_prompt(
        wh::prompt::prompt_render_request{std::move(request.context),
                                          std::move(request.prompt_options)},
        sink);
    if (rendered.has_error()) {
      return wh::core::result<wh::model::chat_request>::failure(
          rendered.error());
    }
    model_request.messages = std::move(rendered).value();
    return model_request;
  }

  [[nodiscard]] auto
  render_prompt(const wh::prompt::prompt_render_request &request,
                const wh::callbacks::callback_sink &sink) const
      -> wh::core::result<std::vector<wh::schema::message>> {
    auto callback_context = detail::make_callback_context(sink);
    return wh::core::deref_or_self(prompt_).render(request, callback_context);
  }

  [[nodiscard]] auto
  render_prompt(wh::prompt::prompt_render_request &&request,
                const wh::callbacks::callback_sink &sink) const
      -> wh::core::result<std::vector<wh::schema::message>> {
    auto callback_context = detail::make_callback_context(sink);
    return wh::core::deref_or_self(prompt_).render(std::move(request),
                                                   callback_context);
  }

  [[nodiscard]] auto
  invoke_model(wh::model::chat_request &&request,
               const wh::callbacks::callback_sink &sink) const
      -> wh::core::result<wh::model::chat_response> {
    auto callback_context = detail::make_callback_context(sink);
    return wh::core::deref_or_self(model_).invoke(std::move(request),
                                                  callback_context);
  }

  [[nodiscard]] auto
  stream_model(wh::model::chat_request &&request,
               const wh::callbacks::callback_sink &sink) const
      -> wh::core::result<wh::model::chat_stream_reader> {
    auto callback_context = detail::make_callback_context(sink);
    return wh::core::deref_or_self(model_).stream(std::move(request),
                                                  callback_context);
  }

  /// Prompt renderer used to materialize base messages.
  Prompt prompt_;
  /// Chat model used for invoke/stream execution.
  Model model_;
};

} // namespace wh::chain
