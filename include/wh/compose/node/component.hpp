// Defines component-node bindings selected by component kind and node contract.
#pragma once

#include <concepts>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/compose/node/authored.hpp"
#include "wh/compose/node/detail/context.hpp"
#include "wh/compose/node/detail/contract.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/document/document.hpp"
#include "wh/document/parser/interface.hpp"
#include "wh/embedding/embedding.hpp"
#include "wh/indexer/indexer.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/prompt/chat_template.hpp"
#include "wh/retriever/retriever.hpp"
#include "wh/tool/tool.hpp"

namespace wh::compose {

namespace detail {

template <typename type_t> inline constexpr bool always_false_v = false;

template <typename value_t>
concept value_contract_payload = std::same_as<std::remove_cvref_t<value_t>, graph_value> ||
                                 std::copy_constructible<std::remove_cvref_t<value_t>>;

template <typename request_t>
using custom_sync_request_arg_t =
    std::conditional_t<std::copy_constructible<request_t>, const request_t &, request_t &&>;

[[nodiscard]] inline auto component_kind_name(const component_kind kind) -> std::string_view {
  switch (kind) {
  case component_kind::model:
    return "model";
  case component_kind::prompt:
    return "prompt";
  case component_kind::embedding:
    return "embedding";
  case component_kind::retriever:
    return "retriever";
  case component_kind::indexer:
    return "indexer";
  case component_kind::document:
    return "document";
  case component_kind::tool:
    return "tool";
  case component_kind::custom:
    return "custom";
  }
  return "custom";
}

template <typename key_t, typename options_t>
[[nodiscard]] inline auto decorate_component_options(key_t &&key, const component_kind kind,
                                                     options_t &&options)
    -> graph_add_node_options {
  return decorate_named_node_options(std::forward<key_t>(key), std::forward<options_t>(options),
                                     component_kind_name(kind), component_kind_name(kind));
}

template <typename request_t>
[[nodiscard]] inline auto read_request(const graph_value &input)
    -> wh::core::result<std::reference_wrapper<const request_t>> {
  if (const auto *typed = wh::core::any_cast<request_t>(&input); typed != nullptr) {
    return std::cref(*typed);
  }
  return wh::core::result<std::reference_wrapper<const request_t>>::failure(
      wh::core::errc::type_mismatch);
}

template <typename request_t>
[[nodiscard]] inline auto read_request(graph_value &input) -> request_t * {
  return wh::core::any_cast<request_t>(&input);
}

template <typename request_t> struct component_request_state {
  const request_t *borrowed{nullptr};
  std::optional<request_t> owned{};

  [[nodiscard]] static auto borrow(const request_t &request) -> component_request_state {
    return component_request_state{.borrowed = std::addressof(request)};
  }

  static auto borrow(request_t &&) -> component_request_state = delete;

  [[nodiscard]] static auto own(request_t request) -> component_request_state {
    return component_request_state{.owned = std::move(request)};
  }

  template <typename invoke_t> [[nodiscard]] auto apply(invoke_t &&invoke) && -> decltype(auto) {
    if constexpr (std::copy_constructible<request_t>) {
      if (owned.has_value()) {
        return std::forward<invoke_t>(invoke)(std::move(*owned));
      }
      return std::forward<invoke_t>(invoke)(*borrowed);
    } else {
      return std::forward<invoke_t>(invoke)(std::move(*owned));
    }
  }

  [[nodiscard]] auto into_owned() && -> request_t {
    if constexpr (std::copy_constructible<request_t>) {
      if (owned.has_value()) {
        return std::move(*owned);
      }
      return request_t{*borrowed};
    } else {
      return std::move(*owned);
    }
  }
};

template <component_request_with_options request_t>
[[nodiscard]] inline auto request_matches_runtime(const request_t &request,
                                                  const node_runtime &runtime) -> bool {
  const auto resolved = request.options.component_options().resolve_view();
  const auto &observation = node_observation(runtime);
  if (resolved.callbacks_enabled != observation.callbacks_enabled) {
    return false;
  }
  const auto &trace = node_trace(runtime);
  if (!trace.trace_id.empty() && resolved.trace_id != trace.trace_id) {
    return false;
  }
  if (!trace.span_id.empty() && resolved.span_id != trace.span_id) {
    return false;
  }
  return true;
}

template <typename request_t>
[[nodiscard]] inline auto resolve_component_request(const graph_value &input,
                                                    const node_runtime &runtime)
    -> wh::core::result<component_request_state<request_t>> {
  auto request = read_request<request_t>(input);
  if (request.has_error()) {
    return wh::core::result<component_request_state<request_t>>::failure(request.error());
  }
  const auto &request_ref = request.value().get();
  if constexpr (!component_request_with_options<request_t>) {
    return component_request_state<request_t>::borrow(request_ref);
  } else if (request_matches_runtime(request_ref, runtime)) {
    return component_request_state<request_t>::borrow(request_ref);
  } else {
    request_t patched{request_ref};
    patch_component_request(patched, node_observation(runtime), node_trace(runtime));
    return component_request_state<request_t>::own(std::move(patched));
  }
}

template <typename request_t>
[[nodiscard]] inline auto resolve_component_request(graph_value &input, const node_runtime &runtime)
    -> wh::core::result<component_request_state<request_t>> {
  auto *request = read_request<request_t>(input);
  if (request == nullptr) {
    return wh::core::result<component_request_state<request_t>>::failure(
        wh::core::errc::type_mismatch);
  }

  if constexpr (!component_request_with_options<request_t>) {
    if constexpr (std::copy_constructible<request_t>) {
      return component_request_state<request_t>::borrow(*request);
    } else {
      return component_request_state<request_t>::own(std::move(*request));
    }
  } else if (request_matches_runtime(*request, runtime)) {
    if constexpr (std::copy_constructible<request_t>) {
      return component_request_state<request_t>::borrow(*request);
    } else {
      return component_request_state<request_t>::own(std::move(*request));
    }
  } else {
    request_t patched{std::move(*request)};
    patch_component_request(patched, node_observation(runtime), node_trace(runtime));
    return component_request_state<request_t>::own(std::move(patched));
  }
}

[[nodiscard]] inline auto make_component_context(const wh::core::run_context &parent,
                                                 const node_runtime &runtime)
    -> std::optional<wh::core::run_context> {
  return make_node_callback_context(parent, node_observation(runtime), node_trace(runtime));
}

template <typename value_t>
[[nodiscard]] inline auto make_graph_output(value_t &&value) -> wh::core::result<graph_value> {
  static_assert(value_contract_payload<value_t>,
                "value contract output type must be copy-constructible; wrap dynamic "
                "payloads in graph_value/wh::core::any explicitly");
  return graph_value{std::forward<value_t>(value)};
}

template <typename reader_t>
[[nodiscard]] inline auto make_graph_stream_output(reader_t &&reader)
    -> wh::core::result<graph_value> {
  auto canonical = to_graph_stream_reader(std::forward<reader_t>(reader));
  if (canonical.has_error()) {
    return wh::core::result<graph_value>::failure(canonical.error());
  }
  return graph_value{std::move(canonical).value()};
}

template <typename status_t>
[[nodiscard]] inline auto to_graph_output(status_t status) -> wh::core::result<graph_value> {
  if (status.has_error()) {
    return wh::core::result<graph_value>::failure(status.error());
  }
  return make_graph_output(std::move(status).value());
}

template <typename request_t, typename status_t, typename sender_factory_t>
[[nodiscard]] inline auto bind_component_sender(const graph_value &input,
                                                const node_runtime &runtime,
                                                wh::core::run_context &context,
                                                sender_factory_t &&make_sender) -> graph_sender {
  auto request = resolve_component_request<request_t>(input, runtime);
  if (request.has_error()) {
    return ::wh::compose::detail::failure_graph_sender(request.error());
  }
  auto owned_request = std::move(request).value().into_owned();
  return bind_node_sender(
      context, runtime,
      [request = std::move(owned_request),
       make_sender = std::forward<sender_factory_t>(make_sender)](
          wh::core::run_context &component_context) mutable {
        return wh::core::detail::map_result_sender<wh::core::result<graph_value>>(
            make_sender(std::move(request), component_context),
            [](typename status_t::value_type value) {
              return make_graph_output(std::move(value));
            });
      });
}

template <typename request_t, typename status_t, typename sender_factory_t>
[[nodiscard]] inline auto
bind_component_stream_sender(const graph_value &input, const node_runtime &runtime,
                             wh::core::run_context &context, sender_factory_t &&make_sender)
    -> graph_sender {
  auto request = resolve_component_request<request_t>(input, runtime);
  if (request.has_error()) {
    return ::wh::compose::detail::failure_graph_sender(request.error());
  }
  auto owned_request = std::move(request).value().into_owned();
  return bind_node_sender(
      context, runtime,
      [request = std::move(owned_request),
       make_sender = std::forward<sender_factory_t>(make_sender)](
          wh::core::run_context &component_context) mutable {
        return wh::core::detail::map_result_sender<wh::core::result<graph_value>>(
            make_sender(std::move(request), component_context),
            [](typename status_t::value_type reader) {
              return make_graph_stream_output(std::move(reader));
            });
      });
}

template <typename request_t, typename status_t, typename component_t, typename invoke_t>
[[nodiscard]] inline auto
bind_component_async_result(const component_t &component, const graph_value &input,
                            const node_runtime &runtime, wh::core::run_context &context,
                            invoke_t &&invoke) -> graph_sender {
  return bind_component_sender<request_t, status_t>(
      input, runtime, context,
      [&component, invoke = std::forward<invoke_t>(invoke)](
          auto &&request, wh::core::run_context &component_context) mutable {
        return std::invoke(invoke, component, std::forward<decltype(request)>(request),
                           component_context);
      });
}

template <typename request_t, typename invoke_t>
[[nodiscard]] inline auto bind_component_result(const graph_value &input,
                                                const node_runtime &runtime,
                                                wh::core::run_context &context, invoke_t &&invoke)
    -> wh::core::result<graph_value> {
  auto request = resolve_component_request<request_t>(input, runtime);
  if (request.has_error()) {
    return wh::core::result<graph_value>::failure(request.error());
  }
  auto callback_context = make_component_context(context, runtime);
  wh::core::run_context empty_context{};
  auto &component_context = callback_context.has_value() ? *callback_context : empty_context;
  return std::move(request).value().apply([&](auto &&resolved_request) {
    return to_graph_output(std::invoke(std::forward<invoke_t>(invoke),
                                       std::forward<decltype(resolved_request)>(resolved_request),
                                       component_context));
  });
}

template <typename request_t, typename invoke_t>
[[nodiscard]] inline auto
bind_component_stream_result(const graph_value &input, const node_runtime &runtime,
                             wh::core::run_context &context, invoke_t &&invoke)
    -> wh::core::result<graph_value> {
  auto request = resolve_component_request<request_t>(input, runtime);
  if (request.has_error()) {
    return wh::core::result<graph_value>::failure(request.error());
  }
  auto callback_context = make_component_context(context, runtime);
  wh::core::run_context empty_context{};
  auto &component_context = callback_context.has_value() ? *callback_context : empty_context;
  return std::move(request).value().apply([&](auto &&resolved_request) {
    auto output =
        std::invoke(std::forward<invoke_t>(invoke),
                    std::forward<decltype(resolved_request)>(resolved_request), component_context);
    if (output.has_error()) {
      return wh::core::result<graph_value>::failure(output.error());
    }
    return make_graph_stream_output(std::move(output).value());
  });
}

template <component_kind Kind, node_contract To> struct explicit_component_types;

template <> struct explicit_component_types<component_kind::model, node_contract::value> {
  using request_type = wh::model::chat_request;
  using async_result_type = wh::model::chat_invoke_result;
};

template <> struct explicit_component_types<component_kind::model, node_contract::stream> {
  using request_type = wh::model::chat_request;
  using async_result_type = wh::model::chat_message_stream_result;
};

template <> struct explicit_component_types<component_kind::prompt, node_contract::value> {
  using request_type = wh::prompt::prompt_render_request;
  using async_result_type = wh::core::result<std::vector<wh::schema::message>>;
};

template <> struct explicit_component_types<component_kind::embedding, node_contract::value> {
  using request_type = wh::embedding::embedding_request;
  using async_result_type = wh::core::result<wh::embedding::embedding_response>;
};

template <> struct explicit_component_types<component_kind::retriever, node_contract::value> {
  using request_type = wh::retriever::retriever_request;
  using async_result_type = wh::core::result<wh::retriever::retriever_response>;
};

template <> struct explicit_component_types<component_kind::indexer, node_contract::value> {
  using request_type = wh::indexer::indexer_request;
  using async_result_type = wh::core::result<wh::indexer::indexer_response>;
};

template <> struct explicit_component_types<component_kind::tool, node_contract::value> {
  using request_type = wh::tool::tool_request;
  using async_result_type = wh::tool::tool_invoke_result;
};

template <> struct explicit_component_types<component_kind::tool, node_contract::stream> {
  using request_type = wh::tool::tool_request;
  using async_result_type = wh::tool::tool_output_stream_result;
};

template <> struct explicit_component_types<component_kind::document, node_contract::value> {
  using request_type = wh::document::document_request;
  using async_result_type = wh::core::result<wh::document::document_batch>;
};

template <component_kind Kind, node_contract To>
using explicit_component_request_t = typename explicit_component_types<Kind, To>::request_type;

template <component_kind Kind, node_contract To>
using explicit_component_async_result_t =
    typename explicit_component_types<Kind, To>::async_result_type;

template <component_kind Kind, node_contract To>
using explicit_component_output_t =
    typename explicit_component_async_result_t<Kind, To>::value_type;

template <component_kind Kind, node_contract To>
[[nodiscard]] constexpr auto explicit_component_input_gate() noexcept -> input_gate {
  return typed_input_gate<node_contract::value, explicit_component_request_t<Kind, To>>();
}

template <component_kind Kind, node_contract To>
[[nodiscard]] constexpr auto explicit_component_output_gate() noexcept -> output_gate {
  if constexpr (To == node_contract::value) {
    return typed_output_gate<node_contract::value, explicit_component_output_t<Kind, To>>();
  } else {
    return output_gate::reader();
  }
}

template <typename component_t, typename request_t, typename result_t>
concept async_invoke_result_sender =
    requires(const component_t &value, wh::core::run_context &callback_context) {
      requires result_typed_sender<
          decltype(value.async_invoke(std::declval<request_t &&>(), callback_context)), result_t>;
    } || requires(const component_t &value, wh::core::run_context &callback_context,
                  const node_runtime &runtime) {
      requires result_typed_sender<decltype(value.async_invoke(std::declval<request_t &&>(),
                                                               callback_context, runtime)),
                                   result_t>;
    };

template <typename component_t, typename request_t, typename result_t>
concept async_stream_result_sender =
    requires(const component_t &value, wh::core::run_context &callback_context) {
      requires result_typed_sender<
          decltype(value.async_stream(std::declval<request_t &&>(), callback_context)), result_t>;
    } || requires(const component_t &value, wh::core::run_context &callback_context,
                  const node_runtime &runtime) {
      requires result_typed_sender<decltype(value.async_stream(std::declval<request_t &&>(),
                                                               callback_context, runtime)),
                                   result_t>;
    };

template <typename component_t, typename request_t, typename result_t>
concept async_render_result_sender =
    requires(const component_t &value, wh::core::run_context &callback_context) {
      requires result_typed_sender<
          decltype(value.async_render(std::declval<request_t &&>(), callback_context)), result_t>;
    };

template <typename component_t, typename request_t, typename result_t>
concept async_embed_result_sender =
    requires(const component_t &value, wh::core::run_context &callback_context) {
      requires result_typed_sender<
          decltype(value.async_embed(std::declval<request_t &&>(), callback_context)), result_t>;
    };

template <typename component_t, typename request_t, typename result_t>
concept async_retrieve_result_sender =
    requires(const component_t &value, wh::core::run_context &callback_context) {
      requires result_typed_sender<
          decltype(value.async_retrieve(std::declval<request_t &&>(), callback_context)), result_t>;
    };

template <typename component_t, typename request_t, typename result_t>
concept async_write_result_sender =
    requires(const component_t &value, wh::core::run_context &callback_context) {
      requires result_typed_sender<
          decltype(value.async_write(std::declval<request_t &&>(), callback_context)), result_t>;
    };

template <typename component_t, typename request_t, typename result_t>
concept async_process_result_sender =
    requires(const component_t &value, wh::core::run_context &callback_context) {
      requires result_typed_sender<
          decltype(value.async_process(std::declval<request_t &&>(), callback_context)), result_t>;
    };

template <typename component_t, typename request_t, typename result_t>
concept custom_sync_invoke_result =
    requires(const component_t &value, wh::core::run_context &callback_context) {
      {
        value.invoke(std::declval<custom_sync_request_arg_t<request_t>>(), callback_context)
      } -> std::same_as<result_t>;
    };

template <typename component_t, typename request_t, typename result_t>
concept custom_sync_invoke_result_with_runtime = requires(const component_t &value,
                                                          wh::core::run_context &callback_context,
                                                          const node_runtime &runtime) {
  {
    value.invoke(std::declval<custom_sync_request_arg_t<request_t>>(), callback_context, runtime)
  } -> std::same_as<result_t>;
};

template <typename component_t, typename request_t, typename result_t>
concept custom_sync_stream_result =
    requires(const component_t &value, wh::core::run_context &callback_context) {
      {
        value.stream(std::declval<custom_sync_request_arg_t<request_t>>(), callback_context)
      } -> std::same_as<result_t>;
    };

template <typename component_t, typename request_t, typename result_t>
concept custom_sync_stream_result_with_runtime = requires(const component_t &value,
                                                          wh::core::run_context &callback_context,
                                                          const node_runtime &runtime) {
  {
    value.stream(std::declval<custom_sync_request_arg_t<request_t>>(), callback_context, runtime)
  } -> std::same_as<result_t>;
};

template <component_kind Kind, node_contract From, node_contract To, typename component_t>
struct component_contract;
template <component_kind Kind, node_contract From, node_contract To, typename component_t>
struct component_contract {
  static auto run(const component_t &, const graph_value &, wh::core::run_context &)
      -> wh::core::result<graph_value> = delete;
};

template <typename component_t>
struct component_contract<component_kind::model, node_contract::value, node_contract::value,
                          component_t> {
  using request_type = explicit_component_request_t<component_kind::model, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime)
      -> wh::core::result<graph_value> {
    return bind_component_result<request_type>(
        input, runtime, context,
        [&component](auto &&request, wh::core::run_context &component_context) {
          return component.invoke(std::forward<decltype(request)>(request), component_context);
        });
  }
};

template <typename component_t>
struct component_contract<component_kind::model, node_contract::value, node_contract::stream,
                          component_t> {
  using request_type = explicit_component_request_t<component_kind::model, node_contract::stream>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime)
      -> wh::core::result<graph_value> {
    return bind_component_stream_result<request_type>(
        input, runtime, context,
        [&component](auto &&request, wh::core::run_context &component_context) {
          return component.stream(std::forward<decltype(request)>(request), component_context);
        });
  }
};

template <typename component_t>
struct component_contract<component_kind::prompt, node_contract::value, node_contract::value,
                          component_t> {
  using request_type = explicit_component_request_t<component_kind::prompt, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime)
      -> wh::core::result<graph_value> {
    return bind_component_result<request_type>(
        input, runtime, context,
        [&component](auto &&request, wh::core::run_context &component_context) {
          return component.render(std::forward<decltype(request)>(request), component_context);
        });
  }
};

template <typename component_t>
struct component_contract<component_kind::embedding, node_contract::value, node_contract::value,
                          component_t> {
  using request_type =
      explicit_component_request_t<component_kind::embedding, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime)
      -> wh::core::result<graph_value> {
    return bind_component_result<request_type>(
        input, runtime, context,
        [&component](auto &&request, wh::core::run_context &component_context) {
          return component.embed(std::forward<decltype(request)>(request), component_context);
        });
  }
};

template <typename component_t>
struct component_contract<component_kind::retriever, node_contract::value, node_contract::value,
                          component_t> {
  using request_type =
      explicit_component_request_t<component_kind::retriever, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime)
      -> wh::core::result<graph_value> {
    return bind_component_result<request_type>(
        input, runtime, context,
        [&component](auto &&request, wh::core::run_context &component_context) {
          return component.retrieve(std::forward<decltype(request)>(request), component_context);
        });
  }
};

template <typename component_t>
struct component_contract<component_kind::indexer, node_contract::value, node_contract::value,
                          component_t> {
  using request_type = explicit_component_request_t<component_kind::indexer, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime)
      -> wh::core::result<graph_value> {
    return bind_component_result<request_type>(
        input, runtime, context,
        [&component](auto &&request, wh::core::run_context &component_context) {
          return component.write(std::forward<decltype(request)>(request), component_context);
        });
  }
};

template <typename component_t>
struct component_contract<component_kind::tool, node_contract::value, node_contract::value,
                          component_t> {
  using request_type = explicit_component_request_t<component_kind::tool, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime)
      -> wh::core::result<graph_value> {
    return bind_component_result<request_type>(
        input, runtime, context,
        [&component](auto &&request, wh::core::run_context &component_context) {
          return component.invoke(std::forward<decltype(request)>(request), component_context);
        });
  }
};

template <typename component_t>
struct component_contract<component_kind::tool, node_contract::value, node_contract::stream,
                          component_t> {
  using request_type = explicit_component_request_t<component_kind::tool, node_contract::stream>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime)
      -> wh::core::result<graph_value> {
    return bind_component_stream_result<request_type>(
        input, runtime, context,
        [&component](auto &&request, wh::core::run_context &component_context) {
          return component.stream(std::forward<decltype(request)>(request), component_context);
        });
  }
};

template <typename component_t>
struct component_contract<component_kind::document, node_contract::value, node_contract::value,
                          component_t> {
  using request_type = explicit_component_request_t<component_kind::document, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime)
      -> wh::core::result<graph_value> {
    return bind_component_result<request_type>(
        input, runtime, context,
        [&component](auto &&request, wh::core::run_context &component_context) {
          return component.process(std::forward<decltype(request)>(request), component_context);
        });
  }
};

template <component_kind Kind, node_contract From, node_contract To, typename component_t>
concept explicit_component_contract = requires(
    const component_t &component, const graph_value &input, wh::core::run_context &context) {
  {
    component_contract<Kind, From, To, component_t>::run(component, input, context,
                                                         std::declval<const node_runtime &>())
  } -> std::same_as<wh::core::result<graph_value>>;
};

template <component_kind Kind, node_contract From, node_contract To, typename component_t>
inline constexpr bool component_contract_supported =
    explicit_component_contract<Kind, From, To, component_t>;

template <component_kind Kind, node_contract From, node_contract To, typename component_t>
struct component_async_contract;
template <component_kind Kind, node_contract From, node_contract To, typename component_t>
struct component_async_contract {
  static auto run(const component_t &, const graph_value &, wh::core::run_context &,
                  const node_runtime &) -> graph_sender = delete;
};

template <typename component_t>
struct component_async_contract<component_kind::model, node_contract::value, node_contract::value,
                                component_t> {
  using request_type = explicit_component_request_t<component_kind::model, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime) -> graph_sender {
    if constexpr (async_invoke_result_sender<component_t, request_type,
                                             explicit_component_async_result_t<
                                                 component_kind::model, node_contract::value>>) {
      return bind_component_async_result<
          request_type,
          explicit_component_async_result_t<component_kind::model, node_contract::value>>(
          component, input, runtime, context,
          [](const component_t &value, auto &&request, wh::core::run_context &component_context) {
            return value.async_invoke(std::forward<decltype(request)>(request), component_context);
          });
    } else {
      static_assert(always_false_v<component_t>, "component_node<async> requires "
                                                 "model.async_invoke(request, run_context&) -> "
                                                 "sender<result<chat_response>>");
    }
  }
};

template <typename component_t>
struct component_async_contract<component_kind::model, node_contract::value, node_contract::stream,
                                component_t> {
  using request_type = explicit_component_request_t<component_kind::model, node_contract::stream>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime) -> graph_sender {
    if constexpr (async_stream_result_sender<component_t, request_type,
                                             explicit_component_async_result_t<
                                                 component_kind::model, node_contract::stream>>) {
      return bind_component_stream_sender<
          request_type,
          explicit_component_async_result_t<component_kind::model, node_contract::stream>>(
          input, runtime, context,
          [&component](auto &&request, wh::core::run_context &component_context) {
            return component.async_stream(std::forward<decltype(request)>(request),
                                          component_context);
          });
    } else {
      static_assert(always_false_v<component_t>, "component_node<async> requires "
                                                 "model.async_stream(request, run_context&) -> "
                                                 "sender<result<chat_message_stream_reader>>");
    }
  }
};

template <typename component_t>
struct component_async_contract<component_kind::prompt, node_contract::value, node_contract::value,
                                component_t> {
  using request_type = explicit_component_request_t<component_kind::prompt, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime) -> graph_sender {
    if constexpr (async_render_result_sender<component_t, request_type,
                                             explicit_component_async_result_t<
                                                 component_kind::prompt, node_contract::value>>) {
      return bind_component_async_result<
          request_type,
          explicit_component_async_result_t<component_kind::prompt, node_contract::value>>(
          component, input, runtime, context,
          [](const component_t &value, auto &&request, wh::core::run_context &component_context) {
            return value.async_render(std::forward<decltype(request)>(request), component_context);
          });
    } else {
      static_assert(always_false_v<component_t>, "component_node<async> requires "
                                                 "prompt.async_render(request, run_context&) -> "
                                                 "sender<result<vector<message>>>");
    }
  }
};

template <typename component_t>
struct component_async_contract<component_kind::embedding, node_contract::value,
                                node_contract::value, component_t> {
  using request_type =
      explicit_component_request_t<component_kind::embedding, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime) -> graph_sender {
    if constexpr (async_embed_result_sender<component_t, request_type,
                                            explicit_component_async_result_t<
                                                component_kind::embedding, node_contract::value>>) {
      return bind_component_async_result<
          request_type,
          explicit_component_async_result_t<component_kind::embedding, node_contract::value>>(
          component, input, runtime, context,
          [](const component_t &value, auto &&request, wh::core::run_context &component_context) {
            return value.async_embed(std::forward<decltype(request)>(request), component_context);
          });
    } else {
      static_assert(always_false_v<component_t>, "component_node<async> requires "
                                                 "embedding.async_embed(request, run_context&) -> "
                                                 "sender<result<embedding_response>>");
    }
  }
};

template <typename component_t>
struct component_async_contract<component_kind::retriever, node_contract::value,
                                node_contract::value, component_t> {
  using request_type =
      explicit_component_request_t<component_kind::retriever, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime) -> graph_sender {
    if constexpr (async_retrieve_result_sender<
                      component_t, request_type,
                      explicit_component_async_result_t<component_kind::retriever,
                                                        node_contract::value>>) {
      return bind_component_async_result<
          request_type,
          explicit_component_async_result_t<component_kind::retriever, node_contract::value>>(
          component, input, runtime, context,
          [](const component_t &value, auto &&request, wh::core::run_context &component_context) {
            return value.async_retrieve(std::forward<decltype(request)>(request),
                                        component_context);
          });
    } else {
      static_assert(always_false_v<component_t>,
                    "component_node<async> requires retriever.async_retrieve(request, "
                    "run_context&) -> sender<result<retriever_response>>");
    }
  }
};

template <typename component_t>
struct component_async_contract<component_kind::indexer, node_contract::value, node_contract::value,
                                component_t> {
  using request_type = explicit_component_request_t<component_kind::indexer, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime) -> graph_sender {
    if constexpr (async_write_result_sender<component_t, request_type,
                                            explicit_component_async_result_t<
                                                component_kind::indexer, node_contract::value>>) {
      return bind_component_async_result<
          request_type,
          explicit_component_async_result_t<component_kind::indexer, node_contract::value>>(
          component, input, runtime, context,
          [](const component_t &value, auto &&request, wh::core::run_context &component_context) {
            return value.async_write(std::forward<decltype(request)>(request), component_context);
          });
    } else {
      static_assert(always_false_v<component_t>, "component_node<async> requires "
                                                 "indexer.async_write(request, run_context&) -> "
                                                 "sender<result<indexer_response>>");
    }
  }
};

template <typename component_t>
struct component_async_contract<component_kind::tool, node_contract::value, node_contract::value,
                                component_t> {
  using request_type = explicit_component_request_t<component_kind::tool, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime) -> graph_sender {
    if constexpr (async_invoke_result_sender<component_t, request_type,
                                             explicit_component_async_result_t<
                                                 component_kind::tool, node_contract::value>>) {
      return bind_component_async_result<
          request_type,
          explicit_component_async_result_t<component_kind::tool, node_contract::value>>(
          component, input, runtime, context,
          [](const component_t &value, auto &&request, wh::core::run_context &component_context) {
            return value.async_invoke(std::forward<decltype(request)>(request), component_context);
          });
    } else {
      static_assert(always_false_v<component_t>,
                    "component_node<async> requires tool.async_invoke(request, "
                    "run_context&) -> sender<result<string>>");
    }
  }
};

template <typename component_t>
struct component_async_contract<component_kind::tool, node_contract::value, node_contract::stream,
                                component_t> {
  using request_type = explicit_component_request_t<component_kind::tool, node_contract::stream>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime) -> graph_sender {
    if constexpr (async_stream_result_sender<component_t, request_type,
                                             explicit_component_async_result_t<
                                                 component_kind::tool, node_contract::stream>>) {
      return bind_component_stream_sender<
          request_type,
          explicit_component_async_result_t<component_kind::tool, node_contract::stream>>(
          input, runtime, context,
          [&component](auto &&request, wh::core::run_context &component_context) {
            return component.async_stream(std::forward<decltype(request)>(request),
                                          component_context);
          });
    } else {
      static_assert(always_false_v<component_t>,
                    "component_node<async> requires tool.async_stream(request, "
                    "run_context&) -> sender<result<tool_output_stream_reader>>");
    }
  }
};

template <typename component_t>
struct component_async_contract<component_kind::document, node_contract::value,
                                node_contract::value, component_t> {
  using request_type = explicit_component_request_t<component_kind::document, node_contract::value>;

  static auto run(const component_t &component, const graph_value &input,
                  wh::core::run_context &context, const node_runtime &runtime) -> graph_sender {
    if constexpr (async_process_result_sender<
                      component_t, request_type,
                      explicit_component_async_result_t<component_kind::document,
                                                        node_contract::value>>) {
      return bind_component_async_result<
          request_type,
          explicit_component_async_result_t<component_kind::document, node_contract::value>>(
          component, input, runtime, context,
          [](const component_t &value, auto &&request, wh::core::run_context &component_context) {
            return value.async_process(std::forward<decltype(request)>(request), component_context);
          });
    } else {
      static_assert(always_false_v<component_t>, "component_node<async> requires "
                                                 "document.async_process(request, run_context&) -> "
                                                 "sender<result<document_batch>>");
    }
  }
};

template <component_kind Kind, node_contract From, node_contract To, typename component_t>
concept explicit_component_async_contract = requires(
    const component_t &component, const graph_value &input, wh::core::run_context &context) {
  {
    component_async_contract<Kind, From, To, component_t>::run(component, input, context,
                                                               std::declval<const node_runtime &>())
  } -> std::same_as<graph_sender>;
};

template <component_kind Kind, node_contract From, node_contract To, typename component_t>
inline constexpr bool component_async_contract_supported =
    explicit_component_async_contract<Kind, From, To, component_t>;

template <component_kind Kind, node_contract To, typename component_t>
inline constexpr bool explicit_component_async_method_supported_v = [] {
  if constexpr (Kind == component_kind::model && To == node_contract::value) {
    return async_invoke_result_sender<component_t, explicit_component_request_t<Kind, To>,
                                      explicit_component_async_result_t<Kind, To>>;
  } else if constexpr (Kind == component_kind::model && To == node_contract::stream) {
    return async_stream_result_sender<component_t, explicit_component_request_t<Kind, To>,
                                      explicit_component_async_result_t<Kind, To>>;
  } else if constexpr (Kind == component_kind::prompt && To == node_contract::value) {
    return async_render_result_sender<component_t, explicit_component_request_t<Kind, To>,
                                      explicit_component_async_result_t<Kind, To>>;
  } else if constexpr (Kind == component_kind::embedding && To == node_contract::value) {
    return async_embed_result_sender<component_t, explicit_component_request_t<Kind, To>,
                                     explicit_component_async_result_t<Kind, To>>;
  } else if constexpr (Kind == component_kind::retriever && To == node_contract::value) {
    return async_retrieve_result_sender<component_t, explicit_component_request_t<Kind, To>,
                                        explicit_component_async_result_t<Kind, To>>;
  } else if constexpr (Kind == component_kind::indexer && To == node_contract::value) {
    return async_write_result_sender<component_t, explicit_component_request_t<Kind, To>,
                                     explicit_component_async_result_t<Kind, To>>;
  } else if constexpr (Kind == component_kind::tool && To == node_contract::value) {
    return async_invoke_result_sender<component_t, explicit_component_request_t<Kind, To>,
                                      explicit_component_async_result_t<Kind, To>>;
  } else if constexpr (Kind == component_kind::tool && To == node_contract::stream) {
    return async_stream_result_sender<component_t, explicit_component_request_t<Kind, To>,
                                      explicit_component_async_result_t<Kind, To>>;
  } else if constexpr (Kind == component_kind::document && To == node_contract::value) {
    return async_process_result_sender<component_t, explicit_component_request_t<Kind, To>,
                                       explicit_component_async_result_t<Kind, To>>;
  } else {
    return false;
  }
}();

template <component_kind Kind, node_contract From, node_contract To, node_exec_mode Exec,
          typename component_t>
concept explicit_component_binding =
    (Exec == node_exec_mode::sync && component_contract_supported<Kind, From, To, component_t>) ||
    (Exec == node_exec_mode::async &&
     component_async_contract_supported<Kind, From, To, component_t> &&
     explicit_component_async_method_supported_v<Kind, To, component_t>);

template <component_kind Kind, node_contract From, node_contract To, typename component_t>
[[nodiscard]] inline auto
bind_explicit_component(const component_t &component, const graph_value &input,
                        wh::core::run_context &context, const node_runtime &runtime)
    -> wh::core::result<graph_value> {
  if constexpr (component_contract_supported<Kind, From, To, component_t>) {
    return component_contract<Kind, From, To, component_t>::run(component, input, context, runtime);
  } else {
    static_assert(always_false_v<component_t>,
                  "component does not satisfy the explicit compose contract for "
                  "the selected component_kind and node_contract pair");
  }
}

template <node_contract To, typename component_t, typename request_t, typename response_t>
struct custom_component_contract {
  static auto run(const component_t &, const graph_value &, wh::core::run_context &,
                  const node_runtime &) -> wh::core::result<graph_value> = delete;
};

template <typename component_t, typename request_t, typename response_t>
struct custom_component_contract<node_contract::value, component_t, request_t, response_t> {
  static auto run(const component_t &component, graph_value &input, wh::core::run_context &context,
                  const node_runtime &runtime) -> wh::core::result<graph_value> {
    auto request = resolve_component_request<request_t>(input, runtime);
    if (request.has_error()) {
      return wh::core::result<graph_value>::failure(request.error());
    }
    return std::move(request).value().apply([&](auto &&resolved_request)
                                                -> wh::core::result<graph_value> {
      return with_node_context(
          context, runtime,
          [&](wh::core::run_context &component_context) -> wh::core::result<graph_value> {
            if constexpr (custom_sync_invoke_result_with_runtime<component_t, request_t,
                                                                 wh::core::result<response_t>>) {
              auto output =
                  component.invoke(std::forward<decltype(resolved_request)>(resolved_request),
                                   component_context, runtime);
              if (output.has_error()) {
                return wh::core::result<graph_value>::failure(output.error());
              }
              return make_graph_output(std::move(output).value());
            } else if constexpr (custom_sync_invoke_result<component_t, request_t,
                                                           wh::core::result<response_t>>) {
              auto output = component.invoke(
                  std::forward<decltype(resolved_request)>(resolved_request), component_context);
              if (output.has_error()) {
                return wh::core::result<graph_value>::failure(output.error());
              }
              return make_graph_output(std::move(output).value());
            } else {
              static_assert(always_false_v<component_t>,
                            "custom component requires const "
                            "invoke(request, run_context[, node_runtime]) -> "
                            "wh::core::result<response_t> when To == value");
            }
          });
    });
  }
};

template <typename component_t, typename request_t, typename response_t>
struct custom_component_contract<node_contract::stream, component_t, request_t, response_t> {
  static auto run(const component_t &component, graph_value &input, wh::core::run_context &context,
                  const node_runtime &runtime) -> wh::core::result<graph_value> {
    auto request = resolve_component_request<request_t>(input, runtime);
    if (request.has_error()) {
      return wh::core::result<graph_value>::failure(request.error());
    }
    return std::move(request).value().apply([&](auto &&resolved_request)
                                                -> wh::core::result<graph_value> {
      return with_node_context(
          context, runtime,
          [&](wh::core::run_context &component_context) -> wh::core::result<graph_value> {
            if constexpr (custom_sync_stream_result_with_runtime<component_t, request_t,
                                                                 wh::core::result<response_t>>) {
              auto output =
                  component.stream(std::forward<decltype(resolved_request)>(resolved_request),
                                   component_context, runtime);
              if (output.has_error()) {
                return wh::core::result<graph_value>::failure(output.error());
              }
              return make_graph_stream_output(std::move(output).value());
            } else if constexpr (custom_sync_stream_result<component_t, request_t,
                                                           wh::core::result<response_t>>) {
              auto output = component.stream(
                  std::forward<decltype(resolved_request)>(resolved_request), component_context);
              if (output.has_error()) {
                return wh::core::result<graph_value>::failure(output.error());
              }
              return make_graph_stream_output(std::move(output).value());
            } else {
              static_assert(always_false_v<component_t>,
                            "custom component requires const "
                            "stream(request, run_context[, node_runtime]) -> "
                            "wh::core::result<response_t> when To == stream");
            }
          });
    });
  }
};

template <component_kind Kind, node_contract From, node_contract To, typename component_t,
          typename request_t, typename response_t>
[[nodiscard]] inline auto
bind_explicit_custom_component(const component_t &component, graph_value &input,
                               wh::core::run_context &context, const node_runtime &runtime)
    -> wh::core::result<graph_value> {
  static_assert(Kind == component_kind::custom,
                "custom binder must only be used with component_kind::custom");
  return custom_component_contract<To, component_t, request_t, response_t>::run(component, input,
                                                                                context, runtime);
}

template <typename request_t, typename response_t, typename sender_factory_t>
[[nodiscard]] inline auto
bind_custom_component_sender(graph_value &input, const node_runtime &runtime,
                             wh::core::run_context &context, sender_factory_t &&make_sender)
    -> graph_sender {
  auto request = resolve_component_request<request_t>(input, runtime);
  if (request.has_error()) {
    return failure_graph_sender(request.error());
  }
  auto owned_request = std::move(request).value().into_owned();
  return bind_node_sender(
      context, runtime,
      [request = std::move(owned_request),
       make_sender = std::forward<sender_factory_t>(make_sender)](
          wh::core::run_context &component_context) mutable {
        return wh::core::detail::map_result_sender<wh::core::result<graph_value>>(
            make_sender(std::move(request), component_context),
            [](response_t value) { return make_graph_output(std::move(value)); });
      });
}

template <typename request_t, typename response_t, typename sender_factory_t>
[[nodiscard]] inline auto
bind_custom_component_stream_sender(graph_value &input, const node_runtime &runtime,
                                    wh::core::run_context &context, sender_factory_t &&make_sender)
    -> graph_sender {
  auto request = resolve_component_request<request_t>(input, runtime);
  if (request.has_error()) {
    return failure_graph_sender(request.error());
  }
  auto owned_request = std::move(request).value().into_owned();
  return bind_node_sender(
      context, runtime,
      [request = std::move(owned_request),
       make_sender = std::forward<sender_factory_t>(make_sender)](
          wh::core::run_context &component_context) mutable {
        return wh::core::detail::map_result_sender<wh::core::result<graph_value>>(
            make_sender(std::move(request), component_context),
            [](response_t reader) { return make_graph_stream_output(std::move(reader)); });
      });
}

template <component_kind Kind, node_contract From, node_contract To, typename component_t>
[[nodiscard]] inline auto
bind_explicit_component_async(const component_t &component, graph_value &input,
                              wh::core::run_context &context, const node_runtime &runtime)
    -> graph_sender {
  if constexpr (component_async_contract_supported<Kind, From, To, component_t>) {
    return component_async_contract<Kind, From, To, component_t>::run(component, input, context,
                                                                      runtime);
  } else {
    static_assert(always_false_v<component_t>,
                  "component_node<async> does not support the selected "
                  "component_kind/node_contract pair");
  }
}

template <node_contract To, typename component_t, typename request_t, typename response_t>
struct custom_component_async_contract {
  static auto run(const component_t &, const graph_value &, wh::core::run_context &,
                  const node_runtime &) -> graph_sender = delete;
};

template <typename component_t, typename request_t, typename response_t>
struct custom_component_async_contract<node_contract::value, component_t, request_t, response_t> {
  static auto run(const component_t &component, graph_value &input, wh::core::run_context &context,
                  const node_runtime &runtime) -> graph_sender {
    if constexpr (async_invoke_result_sender<component_t, request_t,
                                             wh::core::result<response_t>>) {
      return bind_custom_component_sender<request_t, response_t>(
          input, runtime, context,
          [&component, runtime_copy = runtime](auto &&request,
                                               wh::core::run_context &component_context) {
            if constexpr (requires(const component_t &value,
                                   wh::core::run_context &callback_context) {
                            value.async_invoke(std::forward<decltype(request)>(request),
                                               callback_context, runtime_copy);
                          }) {
              return component.async_invoke(std::forward<decltype(request)>(request),
                                            component_context, runtime_copy);
            } else {
              return component.async_invoke(std::forward<decltype(request)>(request),
                                            component_context);
            }
          });
    } else {
      static_assert(always_false_v<component_t>,
                    "component_node<async> custom value output requires "
                    "async_invoke(request, run_context&) -> "
                    "sender<result<response_t>>");
    }
  }
};

template <typename component_t, typename request_t, typename response_t>
struct custom_component_async_contract<node_contract::stream, component_t, request_t, response_t> {
  static auto run(const component_t &component, graph_value &input, wh::core::run_context &context,
                  const node_runtime &runtime) -> graph_sender {
    if constexpr (async_stream_result_sender<component_t, request_t,
                                             wh::core::result<response_t>>) {
      return bind_custom_component_stream_sender<request_t, response_t>(
          input, runtime, context,
          [&component, runtime_copy = runtime](auto &&request,
                                               wh::core::run_context &component_context) {
            if constexpr (requires(const component_t &value,
                                   wh::core::run_context &callback_context) {
                            value.async_stream(std::forward<decltype(request)>(request),
                                               callback_context, runtime_copy);
                          }) {
              return component.async_stream(std::forward<decltype(request)>(request),
                                            component_context, runtime_copy);
            } else {
              return component.async_stream(std::forward<decltype(request)>(request),
                                            component_context);
            }
          });
    } else {
      static_assert(always_false_v<component_t>,
                    "component_node<async> custom stream output requires "
                    "async_stream(request, run_context&) -> "
                    "sender<result<response_t>>");
    }
  }
};

template <node_contract To, typename component_t, typename request_t, typename response_t>
concept custom_component_sync_binding =
    (To == node_contract::value &&
     (custom_sync_invoke_result_with_runtime<component_t, request_t,
                                             wh::core::result<response_t>> ||
      custom_sync_invoke_result<component_t, request_t, wh::core::result<response_t>>)) ||
    (To == node_contract::stream &&
     (custom_sync_stream_result_with_runtime<component_t, request_t,
                                             wh::core::result<response_t>> ||
      custom_sync_stream_result<component_t, request_t, wh::core::result<response_t>>));

template <node_contract To, typename component_t, typename request_t, typename response_t>
concept custom_component_async_binding =
    requires(const component_t &component, graph_value &input, wh::core::run_context &context) {
      {
        custom_component_async_contract<To, component_t, request_t, response_t>::run(
            component, input, context, std::declval<const node_runtime &>())
      } -> std::same_as<graph_sender>;
    };

template <node_contract To, typename component_t, typename request_t, typename response_t>
inline constexpr bool custom_component_async_method_supported_v = [] {
  if constexpr (To == node_contract::value) {
    return async_invoke_result_sender<component_t, request_t, wh::core::result<response_t>>;
  } else if constexpr (To == node_contract::stream) {
    return async_stream_result_sender<component_t, request_t, wh::core::result<response_t>>;
  } else {
    return false;
  }
}();

template <node_contract From, typename request_t>
[[nodiscard]] constexpr auto custom_component_input_gate() noexcept -> input_gate {
  return typed_input_gate<From, request_t>();
}

template <node_contract To, typename response_t>
[[nodiscard]] constexpr auto custom_component_output_gate() noexcept -> output_gate {
  return typed_output_gate<To, response_t>();
}

template <node_contract From, node_contract To, typename request_t, typename response_t,
          node_exec_mode Exec, typename component_t>
concept custom_component_binding =
    typed_request<From, request_t> && typed_response<To, response_t> &&
    ((Exec == node_exec_mode::sync &&
      custom_component_sync_binding<To, component_t, request_t, response_t>) ||
     (Exec == node_exec_mode::async &&
      custom_component_async_binding<To, component_t, request_t, response_t> &&
      custom_component_async_method_supported_v<To, component_t, request_t, response_t>));

template <component_kind Kind, node_contract From, node_contract To, typename component_t,
          typename request_t, typename response_t>
[[nodiscard]] inline auto
bind_explicit_custom_component_async(const component_t &component, graph_value &input,
                                     wh::core::run_context &context, const node_runtime &runtime)
    -> graph_sender {
  static_assert(Kind == component_kind::custom,
                "custom binder must only be used with component_kind::custom");
  return custom_component_async_contract<To, component_t, request_t, response_t>::run(
      component, input, context, runtime);
}

} // namespace detail

inline auto component_node::compile() const & -> wh::core::result<compiled_node> {
  auto copied = *this;
  return std::move(copied).compile();
}

inline auto component_node::compile() && -> wh::core::result<compiled_node> {
  if (!static_cast<bool>(payload_.lower)) {
    return wh::core::result<compiled_node>::failure(wh::core::errc::not_supported);
  }
  return payload_.lower(std::move(descriptor_.key), std::move(options_));
}

template <component_kind Kind, node_contract From, node_contract To,
          node_exec_mode Exec = node_exec_mode::sync, typename key_t, typename component_t,
          typename options_t = graph_add_node_options>
  requires std::constructible_from<std::string, key_t &&> &&
           std::constructible_from<graph_add_node_options, options_t &&> &&
           (Kind != component_kind::custom) &&
           detail::explicit_component_binding<Kind, From, To, Exec,
                                              std::remove_cvref_t<component_t>>
/// Creates one graph node from a well-known component contract.
[[nodiscard]] inline auto make_component_node(key_t &&key, component_t &&component,
                                              options_t &&options = {}) -> component_node {
  using stored_component_t = std::remove_cvref_t<component_t>;
  auto stored_key = std::string{std::forward<key_t>(key)};
  auto node_options =
      detail::decorate_component_options(stored_key, Kind, std::forward<options_t>(options));
  return component_node{
      node_descriptor{
          .key = std::move(stored_key),
          .kind = node_kind::component,
          .exec_mode = Exec,
          .exec_origin = default_exec_origin(node_kind::component),
          .input_contract = From,
          .output_contract = To,
          .input_gate_info = detail::explicit_component_input_gate<Kind, To>(),
          .output_gate_info = detail::explicit_component_output_gate<Kind, To>(),
      },
      component_payload{
          .kind = Kind,
          .lower = [component = stored_component_t{std::forward<component_t>(component)}](
                       std::string lowered_key,
                       graph_add_node_options lowered_options) mutable
              -> wh::core::result<compiled_node> {
            if constexpr (Exec == node_exec_mode::sync) {
              return make_compiled_sync_node(
                  node_kind::component, default_exec_origin(node_kind::component), From, To,
                  std::move(lowered_key),
                  [component = std::move(component)](
                      graph_value &input, wh::core::run_context &context,
                      const node_runtime &runtime) -> wh::core::result<graph_value> {
                    return detail::bind_explicit_component<Kind, From, To>(component, input,
                                                                           context, runtime);
                  },
                  std::move(lowered_options));
            } else {
              return make_compiled_async_node(
                  node_kind::component, default_exec_origin(node_kind::component), From, To,
                  std::move(lowered_key),
                  [component = std::move(component)](graph_value &input,
                                                     wh::core::run_context &context,
                                                     const node_runtime &runtime) -> graph_sender {
                    return detail::bind_explicit_component_async<Kind, From, To>(component, input,
                                                                                 context, runtime);
                  },
                  std::move(lowered_options));
            }
          }},
      std::move(node_options)};
}

template <component_kind Kind, node_contract From, node_contract To, typename request_t,
          typename response_t, node_exec_mode Exec = node_exec_mode::sync, typename key_t,
          typename component_t, typename options_t = graph_add_node_options>
  requires std::constructible_from<std::string, key_t &&> &&
           std::constructible_from<graph_add_node_options, options_t &&> &&
           (Kind == component_kind::custom) &&
           detail::custom_component_binding<From, To, request_t, response_t, Exec,
                                            std::remove_cvref_t<component_t>>
/// Creates one graph node from a custom component contract.
[[nodiscard]] inline auto make_component_node(key_t &&key, component_t &&component,
                                              options_t &&options = {}) -> component_node {
  using stored_component_t = std::remove_cvref_t<component_t>;
  auto stored_key = std::string{std::forward<key_t>(key)};
  auto node_options =
      detail::decorate_component_options(stored_key, Kind, std::forward<options_t>(options));
  return component_node{
      node_descriptor{
          .key = std::move(stored_key),
          .kind = node_kind::component,
          .exec_mode = Exec,
          .exec_origin = default_exec_origin(node_kind::component),
          .input_contract = From,
          .output_contract = To,
          .input_gate_info = detail::custom_component_input_gate<From, request_t>(),
          .output_gate_info = detail::custom_component_output_gate<To, response_t>(),
      },
      component_payload{
          .kind = Kind,
          .lower = [component = stored_component_t{std::forward<component_t>(component)}](
                       std::string lowered_key,
                       graph_add_node_options lowered_options) mutable
              -> wh::core::result<compiled_node> {
            if constexpr (Exec == node_exec_mode::sync) {
              return make_compiled_sync_node(
                  node_kind::component, default_exec_origin(node_kind::component), From, To,
                  std::move(lowered_key),
                  [component = std::move(component)](
                      graph_value &input, wh::core::run_context &context,
                      const node_runtime &runtime) -> wh::core::result<graph_value> {
                    return detail::bind_explicit_custom_component<
                        Kind, From, To, stored_component_t, request_t, response_t>(
                        component, input, context, runtime);
                  },
                  std::move(lowered_options));
            } else {
              return make_compiled_async_node(
                  node_kind::component, default_exec_origin(node_kind::component), From, To,
                  std::move(lowered_key),
                  [component = std::move(component)](graph_value &input,
                                                     wh::core::run_context &context,
                                                     const node_runtime &runtime) -> graph_sender {
                    return detail::bind_explicit_custom_component_async<
                        Kind, From, To, stored_component_t, request_t, response_t>(
                        component, input, context, runtime);
                  },
                  std::move(lowered_options));
            }
          }},
      std::move(node_options)};
}

} // namespace wh::compose
