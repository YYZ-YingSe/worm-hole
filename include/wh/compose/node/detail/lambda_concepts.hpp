// Defines lambda signature concepts used by compose lambda nodes.
#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/detail/contract.hpp"
#include "wh/compose/node/execution.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose {

/// Lambda concept for `value -> value` function-style nodes.
template <typename lambda_t>
concept value_lambda = requires(lambda_t lambda, graph_value &input,
                                wh::core::run_context &context,
                                const graph_call_scope &call_options) {
  {
    lambda(input, context, call_options)
  } -> std::same_as<wh::core::result<graph_value>>;
};

/// Lambda concept for `value -> value` sender-style nodes.
template <typename lambda_t>
concept value_sender = requires(lambda_t lambda, graph_value &input,
                                wh::core::run_context &context,
                                const graph_call_scope &call_options) {
  requires result_typed_sender<decltype(lambda(input, context, call_options)),
                               wh::core::result<graph_value>>;
};

/// Lambda concept for map-transform `value -> value` function-style nodes.
template <typename lambda_t>
concept map_lambda = requires(lambda_t lambda, graph_value_map &input,
                              wh::core::run_context &context,
                              const graph_call_scope &call_options) {
  {
    lambda(input, context, call_options)
  } -> std::same_as<wh::core::result<graph_value_map>>;
};

/// Lambda concept for map-transform `value -> value` sender-style nodes.
template <typename lambda_t>
concept map_sender = requires(lambda_t lambda, graph_value_map &input,
                              wh::core::run_context &context,
                              const graph_call_scope &call_options) {
  requires result_typed_sender<decltype(lambda(input, context, call_options)),
                               wh::core::result<graph_value_map>>;
};

/// Lambda concept for `value -> stream` function-style nodes.
template <typename lambda_t>
concept value_stream_lambda = requires(lambda_t lambda, graph_value &input,
                                       wh::core::run_context &context,
                                       const graph_call_scope &call_options) {
  requires detail::canonical_stream_status<decltype(lambda(input, context,
                                                           call_options))>;
};

/// Lambda concept for `value -> stream` sender-style nodes.
template <typename lambda_t>
concept value_stream_sender = requires(lambda_t lambda, graph_value &input,
                                       wh::core::run_context &context,
                                       const graph_call_scope &call_options) {
  requires detail::canonical_stream_sender<decltype(lambda(input, context,
                                                           call_options))>;
};

/// Lambda concept for `stream -> value` function-style nodes.
template <typename lambda_t>
concept stream_value_lambda =
    requires(lambda_t lambda, graph_stream_reader input,
             wh::core::run_context &context,
             const graph_call_scope &call_options) {
      {
        lambda(std::move(input), context, call_options)
      } -> std::same_as<wh::core::result<graph_value>>;
    };

/// Lambda concept for `stream -> value` sender-style nodes.
template <typename lambda_t>
concept stream_value_sender =
    requires(lambda_t lambda, graph_stream_reader input,
             wh::core::run_context &context,
             const graph_call_scope &call_options) {
      requires result_typed_sender<decltype(lambda(std::move(input), context,
                                                   call_options)),
                                   wh::core::result<graph_value>>;
    };

/// Lambda concept for `stream -> stream` function-style nodes.
template <typename lambda_t>
concept stream_stream_lambda =
    requires(lambda_t lambda, graph_stream_reader input,
             wh::core::run_context &context,
             const graph_call_scope &call_options) {
      requires detail::canonical_stream_status<decltype(lambda(
          std::move(input), context, call_options))>;
    };

/// Lambda concept for `stream -> stream` sender-style nodes.
template <typename lambda_t>
concept stream_stream_sender =
    requires(lambda_t lambda, graph_stream_reader input,
             wh::core::run_context &context,
             const graph_call_scope &call_options) {
      requires detail::canonical_stream_sender<decltype(lambda(
          std::move(input), context, call_options))>;
    };

template <node_exec_mode Exec, node_contract From, node_contract To,
          typename lambda_t>
inline constexpr bool lambda_contract_supported_v = [] {
  using stored_lambda_t = std::remove_cvref_t<lambda_t>;
  if constexpr (From == node_contract::value && To == node_contract::value) {
    if constexpr (Exec == node_exec_mode::sync) {
      return value_lambda<stored_lambda_t> || map_lambda<stored_lambda_t>;
    } else {
      return value_sender<stored_lambda_t> || map_sender<stored_lambda_t>;
    }
  } else if constexpr (From == node_contract::value &&
                       To == node_contract::stream) {
    if constexpr (Exec == node_exec_mode::sync) {
      return value_stream_lambda<stored_lambda_t>;
    } else {
      return value_stream_sender<stored_lambda_t>;
    }
  } else if constexpr (From == node_contract::stream &&
                       To == node_contract::value) {
    if constexpr (Exec == node_exec_mode::sync) {
      return stream_value_lambda<stored_lambda_t>;
    } else {
      return stream_value_sender<stored_lambda_t>;
    }
  } else if constexpr (From == node_contract::stream &&
                       To == node_contract::stream) {
    if constexpr (Exec == node_exec_mode::sync) {
      return stream_stream_lambda<stored_lambda_t>;
    } else {
      return stream_stream_sender<stored_lambda_t>;
    }
  } else {
    return false;
  }
}();

template <node_exec_mode Exec, node_contract From, node_contract To,
          typename lambda_t>
concept lambda_contract = lambda_contract_supported_v<Exec, From, To, lambda_t>;

} // namespace wh::compose
