// Defines shared node contract rules reused by compose node factories.
#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/detail/gate.hpp"
#include "wh/compose/types.hpp"

namespace wh::compose::detail {

template <typename reader_t>
concept canonical_reader = requires(reader_t reader) {
  {
    to_graph_stream_reader(std::move(reader))
  } -> std::same_as<wh::core::result<graph_stream_reader>>;
};

template <typename status_t>
concept canonical_stream_status = graph_stream_status<status_t>;

template <typename signature_t> struct set_value_signature : std::false_type {
  using result_type = void;
};

template <typename result_t>
struct set_value_signature<stdexec::set_value_t(result_t)> : std::true_type {
  using result_type = result_t;
};

template <typename... signatures_t>
[[nodiscard]] consteval auto canonical_stream_sender_signature_ok() -> bool {
  std::size_t value_count = 0U;
  bool value_ok = true;
  (([&] {
     if constexpr (set_value_signature<signatures_t>::value) {
       ++value_count;
       value_ok = value_ok &&
                  canonical_stream_status<typename set_value_signature<signatures_t>::result_type>;
     }
   }()),
   ...);
  return value_count == 1U && value_ok;
}

template <typename signatures_t> struct canonical_stream_sender_signature : std::false_type {};

template <typename... signatures_t>
struct canonical_stream_sender_signature<stdexec::completion_signatures<signatures_t...>>
    : std::bool_constant<canonical_stream_sender_signature_ok<signatures_t...>()> {};

template <typename sender_t>
concept canonical_stream_sender =
    stdexec::sender<std::remove_cvref_t<sender_t>> &&
    canonical_stream_sender_signature<
        stdexec::completion_signatures_of_t<std::remove_cvref_t<sender_t>, stdexec::env<>>>::value;

template <node_contract From, typename request_t>
concept typed_request = (From == node_contract::value &&
                         !std::same_as<std::remove_cvref_t<request_t>, graph_stream_reader>) ||
                        (From == node_contract::stream &&
                         std::same_as<std::remove_cvref_t<request_t>, graph_stream_reader>);

template <typename response_t>
concept exact_value_payload = std::copy_constructible<std::remove_cvref_t<response_t>> &&
                              !canonical_reader<std::remove_cvref_t<response_t>>;

template <node_contract To, typename response_t>
concept typed_response =
    (To == node_contract::value && exact_value_payload<std::remove_cvref_t<response_t>>) ||
    (To == node_contract::stream && canonical_reader<std::remove_cvref_t<response_t>>);

template <node_contract From, typename request_t>
[[nodiscard]] constexpr auto typed_input_gate() noexcept -> input_gate {
  if constexpr (From == node_contract::value) {
    return input_gate::exact<request_t>();
  } else {
    return input_gate::reader();
  }
}

template <node_contract To, typename response_t>
[[nodiscard]] constexpr auto typed_output_gate() noexcept -> output_gate {
  if constexpr (To == node_contract::value) {
    static_assert(exact_value_payload<std::remove_cvref_t<response_t>>,
                  "value contract output type must be copy-constructible and must not "
                  "be reader-like; use node_contract::stream for live reader outputs or "
                  "wrap dynamic payloads in graph_value/wh::core::any explicitly");
    return output_gate::exact<response_t>();
  } else {
    return output_gate::reader();
  }
}

} // namespace wh::compose::detail
