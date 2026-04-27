// Defines business-layer typed tool payload codecs and typed value-tool
// adapters without exposing raw JSON handling to authored surfaces.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/compose/node/tools_contract.hpp"
#include "wh/core/json.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/internal/serialization.hpp"
#include "wh/schema/serialization/api.hpp"

namespace wh::agent {

using tool_text_result = wh::core::result<std::string>;
using tool_text_sender = wh::core::detail::result_sender<tool_text_result>;

namespace detail {

template <typename value_t>
inline auto write_json_member(wh::core::json_value &target, const std::string_view key,
                              const value_t &value, wh::core::json_allocator &allocator)
    -> wh::core::result<void> {
  wh::core::json_value encoded{};
  auto status = wh::internal::to_json(value, encoded, allocator);
  if (status.has_error()) {
    return wh::core::result<void>::failure(status.error());
  }
  wh::core::json_value key_node{};
  key_node.SetString(key.data(), static_cast<wh::core::json_size_type>(key.size()), allocator);
  target.AddMember(key_node.Move(), encoded.Move(), allocator);
  return {};
}

template <typename value_t>
inline auto write_optional_json_member(wh::core::json_value &target, const std::string_view key,
                                       const std::optional<value_t> &value,
                                       wh::core::json_allocator &allocator)
    -> wh::core::result<void> {
  if (!value.has_value()) {
    return {};
  }
  return write_json_member(target, key, *value, allocator);
}

template <typename value_t>
[[nodiscard]] inline auto read_required_json_member(const wh::core::json_value &input,
                                                    const std::string_view key)
    -> wh::core::result<value_t> {
  auto member = wh::core::json_find_member(input, key);
  if (member.has_error()) {
    return wh::core::result<value_t>::failure(member.error());
  }
  value_t output{};
  auto decoded = wh::internal::from_json(*member.value(), output);
  if (decoded.has_error()) {
    return wh::core::result<value_t>::failure(decoded.error());
  }
  return output;
}

template <typename value_t>
[[nodiscard]] inline auto read_optional_json_member(const wh::core::json_value &input,
                                                    const std::string_view key)
    -> wh::core::result<std::optional<value_t>> {
  auto member = wh::core::json_find_member(input, key);
  if (member.has_error()) {
    if (member.error() == wh::core::errc::not_found) {
      return std::optional<value_t>{};
    }
    return wh::core::result<std::optional<value_t>>::failure(member.error());
  }
  value_t output{};
  auto decoded = wh::internal::from_json(*member.value(), output);
  if (decoded.has_error()) {
    return wh::core::result<std::optional<value_t>>::failure(decoded.error());
  }
  return std::optional<value_t>{std::move(output)};
}

template <typename args_t>
[[nodiscard]] inline auto make_graph_value_result(tool_text_result status)
    -> wh::core::result<wh::compose::graph_value> {
  if (status.has_error()) {
    return wh::core::result<wh::compose::graph_value>::failure(status.error());
  }
  return wh::compose::graph_value{std::move(status).value()};
}

} // namespace detail

template <typename payload_t>
[[nodiscard]] inline auto encode_tool_payload(const payload_t &payload)
    -> wh::core::result<std::string> {
  auto encoded = wh::schema::serialize_fast(payload);
  if (encoded.has_error()) {
    return wh::core::result<std::string>::failure(encoded.error());
  }
  return wh::core::json_to_string(encoded.value());
}

template <typename payload_t>
[[nodiscard]] inline auto decode_tool_payload(const std::string_view payload)
    -> wh::core::result<payload_t> {
  auto parsed = wh::core::parse_json(payload);
  if (parsed.has_error()) {
    return wh::core::result<payload_t>::failure(parsed.error());
  }
  return wh::schema::deserialize_fast<payload_t>(parsed.value());
}

template <typename args_t>
using sync_value_tool_handler =
    wh::core::callback_function<tool_text_result(const wh::compose::tool_call &, args_t) const>;

template <typename args_t>
using async_value_tool_handler =
    wh::core::callback_function<tool_text_sender(wh::compose::tool_call, args_t) const>;

template <typename args_t> struct value_tool_binding {
  sync_value_tool_handler<args_t> sync{nullptr};
  async_value_tool_handler<args_t> async{nullptr};

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(sync) || static_cast<bool>(async);
  }
};

template <typename args_t>
[[nodiscard]] inline auto make_value_tool_entry(value_tool_binding<args_t> binding)
    -> wh::compose::tool_entry {
  wh::compose::tool_entry entry{};
  if (binding.sync) {
    entry.invoke = wh::compose::tool_invoke{
        [binding](const wh::compose::tool_call &call,
                  wh::tool::call_scope) -> wh::core::result<wh::compose::graph_value> {
          auto decoded = decode_tool_payload<args_t>(call.arguments);
          if (decoded.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(decoded.error());
          }
          return detail::make_graph_value_result<args_t>(
              binding.sync(call, std::move(decoded).value()));
        }};
  }
  if (binding.async) {
    entry.async_invoke = wh::compose::tool_async_invoke{
        [binding](wh::compose::tool_call call,
                  wh::tool::call_scope) -> wh::compose::tools_invoke_sender {
          auto decoded = decode_tool_payload<args_t>(call.arguments);
          if (decoded.has_error()) {
            return wh::compose::tools_invoke_sender{
                wh::core::detail::failure_result_sender<
                    wh::core::result<wh::compose::graph_value>>(decoded.error())};
          }
          return wh::compose::tools_invoke_sender{
              std::move(binding.async(call, std::move(decoded).value())) |
              stdexec::then([](tool_text_result status)
                                -> wh::core::result<wh::compose::graph_value> {
                return detail::make_graph_value_result<args_t>(std::move(status));
              })};
        }};
  }
  return entry;
}

} // namespace wh::agent
