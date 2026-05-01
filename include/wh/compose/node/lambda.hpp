// Defines lambda-to-node adapters for compose node creation.
#pragma once

#include <concepts>
#include <functional>
#include <string>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/authored.hpp"
#include "wh/compose/node/detail/context.hpp"
#include "wh/compose/node/detail/lambda_concepts.hpp"
#include "wh/core/stdexec/map_result_sender.hpp"
#include "wh/internal/type_name.hpp"

namespace wh::compose {

namespace detail {

template <typename type_t> inline constexpr bool lambda_always_false_v = false;

template <typename lambda_t>
[[nodiscard]] inline auto decorate_lambda_options(graph_add_node_options options,
                                                  const node_contract) -> graph_add_node_options {
  auto type = std::string{wh::internal::diagnostic_type_alias<std::remove_cvref_t<lambda_t>>()};
  return decorate_node_options(std::move(options), type, type);
}

template <typename value_t>
[[nodiscard]] inline auto wrap_lambda_payload(wh::core::result<value_t> status)
    -> wh::core::result<graph_value> {
  if (status.has_error()) {
    return wh::core::result<graph_value>::failure(status.error());
  }
  return wh::core::any(std::move(status).value());
}

template <typename status_t>
[[nodiscard]] inline auto wrap_lambda_stream_payload(status_t status)
    -> wh::core::result<graph_value> {
  if (status.has_error()) {
    return wh::core::result<graph_value>::failure(status.error());
  }
  auto canonical = to_graph_stream_reader(std::move(status).value());
  if (canonical.has_error()) {
    return wh::core::result<graph_value>::failure(canonical.error());
  }
  return wh::core::any(std::move(canonical).value());
}

template <typename input_t> struct lambda_input_adapter;

template <> struct lambda_input_adapter<graph_value_map> {
  [[nodiscard]] static auto read(graph_value &input)
      -> wh::core::result<std::reference_wrapper<graph_value_map>> {
    if (auto *typed = wh::core::any_cast<graph_value_map>(&input); typed != nullptr) {
      return std::ref(*typed);
    }
    return wh::core::result<std::reference_wrapper<graph_value_map>>::failure(
        wh::core::errc::type_mismatch);
  }
};

template <> struct lambda_input_adapter<graph_stream_reader> {
  [[nodiscard]] static auto read(graph_value &input) -> wh::core::result<graph_stream_reader> {
    if (auto *typed = wh::core::any_cast<graph_stream_reader>(&input); typed != nullptr) {
      return std::move(*typed);
    }
    return wh::core::result<graph_stream_reader>::failure(wh::core::errc::type_mismatch);
  }
};

template <typename input_t> [[nodiscard]] inline auto read_lambda_input(graph_value &input) {
  return lambda_input_adapter<input_t>::read(input);
}

template <typename invoke_t>
[[nodiscard]] inline auto invoke_lambda_result(wh::core::run_context &context,
                                               const node_runtime &runtime, invoke_t &&invoke)
    -> wh::core::result<graph_value> {
  return with_node_call(context, runtime,
                        [&](wh::core::run_context &callback_context,
                            const graph_call_scope &call_options) -> wh::core::result<graph_value> {
                          return std::invoke(std::forward<invoke_t>(invoke), callback_context,
                                             call_options);
                        });
}

template <typename sender_t, typename mapper_t>
[[nodiscard]] inline auto map_lambda_sender(sender_t &&sender, mapper_t &&mapper) {
  return wh::core::detail::map_result_sender<wh::core::result<graph_value>>(
      std::forward<sender_t>(sender), std::forward<mapper_t>(mapper));
}

template <node_contract From, typename lambda_t>
[[nodiscard]] constexpr auto lambda_input_gate() noexcept -> input_gate {
  using stored_lambda_t = std::remove_cvref_t<lambda_t>;
  if constexpr (From == node_contract::value) {
    if constexpr (map_lambda<stored_lambda_t> || map_sender<stored_lambda_t>) {
      return input_gate::exact<graph_value_map>();
    }
    return input_gate::open();
  } else {
    return input_gate::reader();
  }
}

template <node_contract To, typename lambda_t>
[[nodiscard]] constexpr auto lambda_output_gate() noexcept -> output_gate {
  using stored_lambda_t = std::remove_cvref_t<lambda_t>;
  if constexpr (To == node_contract::stream) {
    return output_gate::reader();
  } else if constexpr (map_lambda<stored_lambda_t> || map_sender<stored_lambda_t>) {
    return output_gate::exact<graph_value_map>();
  } else {
    return output_gate::dynamic();
  }
}

template <node_contract From, node_contract To, typename lambda_t>
[[nodiscard]] inline auto make_lambda_descriptor(std::string key, const node_exec_mode exec_mode)
    -> node_descriptor {
  return node_descriptor{
      .key = std::move(key),
      .kind = node_kind::lambda,
      .exec_mode = exec_mode,
      .exec_origin = default_exec_origin(node_kind::lambda),
      .input_contract = From,
      .output_contract = To,
      .input_gate_info = lambda_input_gate<From, lambda_t>(),
      .output_gate_info = lambda_output_gate<To, lambda_t>(),
  };
}

template <node_exec_mode Exec, node_contract From, node_contract To, typename key_t, typename run_t>
[[nodiscard]] inline auto make_lambda_compiled_node(key_t &&key, run_t &&run,
                                                    graph_add_node_options options)
    -> compiled_node {
  if constexpr (Exec == node_exec_mode::sync) {
    return make_compiled_sync_node(node_kind::lambda, default_exec_origin(node_kind::lambda), From,
                                   To, std::forward<key_t>(key), std::forward<run_t>(run),
                                   std::move(options));
  } else {
    return make_compiled_async_node(node_kind::lambda, default_exec_origin(node_kind::lambda), From,
                                    To, std::forward<key_t>(key), std::forward<run_t>(run),
                                    std::move(options));
  }
}

template <node_exec_mode Exec, node_contract From, node_contract To, typename lambda_t,
          typename run_factory_t>
[[nodiscard]] inline auto make_lambda_payload_from_runner(lambda_t &&lambda,
                                                          run_factory_t &&make_run)
    -> lambda_payload {
  using stored_lambda_t = std::remove_cvref_t<lambda_t>;
  using stored_run_factory_t = std::remove_cvref_t<run_factory_t>;
  return lambda_payload{
      .lower = [lambda = stored_lambda_t{std::forward<lambda_t>(lambda)},
                make_run = stored_run_factory_t{std::forward<run_factory_t>(make_run)}](
                   std::string key,
                   graph_add_node_options options) mutable -> wh::core::result<compiled_node> {
        return detail::make_lambda_compiled_node<Exec, From, To>(
            std::move(key), make_run(std::move(lambda)), std::move(options));
      }};
}

template <node_exec_mode Exec, node_contract From, node_contract To, typename lambda_t>
[[nodiscard]] inline auto make_lambda_payload(lambda_t &&lambda) -> lambda_payload {
  using stored_lambda_t = std::remove_cvref_t<lambda_t>;

  if constexpr (From == node_contract::value && To == node_contract::value) {
    if constexpr (Exec == node_exec_mode::sync && value_lambda<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) -> wh::core::result<graph_value> {
              return invoke_lambda_result(
                  context, runtime,
                  [&](wh::core::run_context &callback_context,
                      const graph_call_scope &call_options) -> wh::core::result<graph_value> {
                    return lambda(input, callback_context, call_options);
                  });
            };
          });
    } else if constexpr (Exec == node_exec_mode::async && value_sender<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) mutable -> graph_sender {
              return bind_value_sender(input, context, runtime,
                                       [&lambda](graph_value &owned_input,
                                                 wh::core::run_context &callback_context,
                                                 const graph_call_scope &call_options) {
                                         return lambda(owned_input, callback_context, call_options);
                                       });
            };
          });
    } else if constexpr (Exec == node_exec_mode::sync && map_lambda<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) -> wh::core::result<graph_value> {
              auto typed_input = read_lambda_input<graph_value_map>(input);
              if (typed_input.has_error()) {
                return wh::core::result<graph_value>::failure(typed_input.error());
              }
              return invoke_lambda_result(
                  context, runtime,
                  [&](wh::core::run_context &callback_context,
                      const graph_call_scope &call_options) -> wh::core::result<graph_value> {
                    return wrap_lambda_payload(
                        lambda(typed_input.value().get(), callback_context, call_options));
                  });
            };
          });
    } else if constexpr (Exec == node_exec_mode::async && map_sender<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) mutable -> graph_sender {
              return bind_value_sender(
                  input, context, runtime,
                  [&lambda](graph_value &owned_input, wh::core::run_context &callback_context,
                            const graph_call_scope &call_options) -> graph_sender {
                    auto typed_input = read_lambda_input<graph_value_map>(owned_input);
                    if (typed_input.has_error()) {
                      return failure_graph_sender(typed_input.error());
                    }
                    return bridge_graph_sender(map_lambda_sender(
                        lambda(typed_input.value().get(), callback_context, call_options),
                        [](graph_value_map map) { return wh::core::any(std::move(map)); }));
                  });
            };
          });
    } else {
      static_assert(lambda_always_false_v<stored_lambda_t>,
                    "lambda_node selected execution mode is incompatible with "
                    "the value->value lambda contract; async lambdas must "
                    "return sender<result<graph_value>> and map lambdas must "
                    "return sender<result<graph_value_map>>");
    }
  } else if constexpr (From == node_contract::value && To == node_contract::stream) {
    if constexpr (Exec == node_exec_mode::sync && value_stream_lambda<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) -> wh::core::result<graph_value> {
              return invoke_lambda_result(
                  context, runtime,
                  [&](wh::core::run_context &callback_context,
                      const graph_call_scope &call_options) -> wh::core::result<graph_value> {
                    return wrap_lambda_stream_payload(
                        lambda(input, callback_context, call_options));
                  });
            };
          });
    } else if constexpr (Exec == node_exec_mode::async && value_stream_sender<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) mutable -> graph_sender {
              return bind_value_sender(
                  input, context, runtime,
                  [&lambda](graph_value &owned_input, wh::core::run_context &callback_context,
                            const graph_call_scope &call_options) {
                    return map_lambda_sender(
                        lambda(owned_input, callback_context, call_options),
                        [](auto reader) -> wh::core::result<graph_value> {
                          auto canonical = to_graph_stream_reader(std::move(reader));
                          if (canonical.has_error()) {
                            return wh::core::result<graph_value>::failure(canonical.error());
                          }
                          return wh::core::any(std::move(canonical).value());
                        });
                  });
            };
          });
    } else {
      static_assert(lambda_always_false_v<stored_lambda_t>,
                    "lambda_node selected execution mode is incompatible with "
                    "the value->stream lambda contract; async lambdas must "
                    "return sender<result<graph_stream_reader>>");
    }
  } else if constexpr (From == node_contract::stream && To == node_contract::value) {
    if constexpr (Exec == node_exec_mode::sync && stream_value_lambda<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) -> wh::core::result<graph_value> {
              auto stream_input = read_lambda_input<graph_stream_reader>(input);
              if (stream_input.has_error()) {
                return wh::core::result<graph_value>::failure(stream_input.error());
              }
              return invoke_lambda_result(
                  context, runtime,
                  [&](wh::core::run_context &callback_context,
                      const graph_call_scope &call_options) -> wh::core::result<graph_value> {
                    return lambda(std::move(stream_input).value(), callback_context, call_options);
                  });
            };
          });
    } else if constexpr (Exec == node_exec_mode::async && stream_value_sender<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) mutable -> graph_sender {
              return bind_reader_sender(input, context, runtime,
                                        [&lambda](graph_stream_reader owned_input,
                                                  wh::core::run_context &callback_context,
                                                  const graph_call_scope &call_options) {
                                          return lambda(std::move(owned_input), callback_context,
                                                        call_options);
                                        });
            };
          });
    } else {
      static_assert(lambda_always_false_v<stored_lambda_t>,
                    "lambda_node selected execution mode is incompatible with "
                    "the stream->value lambda contract; async lambdas must "
                    "return sender<result<graph_value>>");
    }
  } else if constexpr (From == node_contract::stream && To == node_contract::stream) {
    if constexpr (Exec == node_exec_mode::sync && stream_stream_lambda<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) -> wh::core::result<graph_value> {
              auto stream_input = read_lambda_input<graph_stream_reader>(input);
              if (stream_input.has_error()) {
                return wh::core::result<graph_value>::failure(stream_input.error());
              }
              return invoke_lambda_result(
                  context, runtime,
                  [&](wh::core::run_context &callback_context,
                      const graph_call_scope &call_options) -> wh::core::result<graph_value> {
                    return wrap_lambda_stream_payload(
                        lambda(std::move(stream_input).value(), callback_context, call_options));
                  });
            };
          });
    } else if constexpr (Exec == node_exec_mode::async && stream_stream_sender<stored_lambda_t>) {
      return make_lambda_payload_from_runner<Exec, From, To>(
          std::forward<lambda_t>(lambda), [](stored_lambda_t stored_lambda) {
            return [lambda = std::move(stored_lambda)](
                       graph_value &input, wh::core::run_context &context,
                       const node_runtime &runtime) mutable -> graph_sender {
              return bind_reader_sender(
                  input, context, runtime,
                  [&lambda](graph_stream_reader owned_input,
                            wh::core::run_context &callback_context,
                            const graph_call_scope &call_options) {
                    return map_lambda_sender(
                        lambda(std::move(owned_input), callback_context, call_options),
                        [](auto reader) -> wh::core::result<graph_value> {
                          auto canonical = to_graph_stream_reader(std::move(reader));
                          if (canonical.has_error()) {
                            return wh::core::result<graph_value>::failure(canonical.error());
                          }
                          return wh::core::any(std::move(canonical).value());
                        });
                  });
            };
          });
    } else {
      static_assert(lambda_always_false_v<stored_lambda_t>,
                    "lambda_node selected execution mode is incompatible with "
                    "the stream->stream lambda contract; async lambdas must "
                    "return sender<result<graph_stream_reader>>");
    }
  } else {
    static_assert(lambda_always_false_v<stored_lambda_t>, "unsupported lambda contract pair");
  }
}

} // namespace detail

inline auto lambda_node::compile() const & -> wh::core::result<compiled_node> {
  auto copied = *this;
  return std::move(copied).compile();
}

inline auto lambda_node::compile() && -> wh::core::result<compiled_node> {
  if (!static_cast<bool>(payload_.lower)) {
    return wh::core::result<compiled_node>::failure(wh::core::errc::not_supported);
  }
  return payload_.lower(std::move(descriptor_.key), std::move(options_));
}

template <node_contract From = node_contract::value, node_contract To = node_contract::value,
          node_exec_mode Exec = node_exec_mode::sync, typename key_t, typename lambda_t,
          typename options_t = graph_add_node_options>
  requires std::constructible_from<std::string, key_t &&> &&
           std::constructible_from<graph_add_node_options, options_t &&> &&
           lambda_contract<Exec, From, To, std::remove_cvref_t<lambda_t>>
/// Creates one graph node from a lambda contract.
[[nodiscard]] inline auto make_lambda_node(key_t &&key, lambda_t &&lambda, options_t &&options = {})
    -> lambda_node {
  auto stored_key = std::string{std::forward<key_t>(key)};
  auto decorated = detail::decorate_lambda_options<lambda_t>(
      graph_add_node_options{std::forward<options_t>(options)}, From);
  return lambda_node{
      detail::make_lambda_descriptor<From, To, lambda_t>(std::move(stored_key), Exec),
      detail::make_lambda_payload<Exec, From, To>(std::forward<lambda_t>(lambda)),
      std::move(decorated)};
}

} // namespace wh::compose
