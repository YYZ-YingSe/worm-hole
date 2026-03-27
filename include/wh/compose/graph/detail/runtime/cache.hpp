// Defines cache runtime helpers extracted from graph execution core.
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

#include "wh/core/any.hpp"
#include "wh/compose/types.hpp"
#include "wh/core/run_context.hpp"

namespace wh::compose::detail::cache_runtime {

using invoke_cache_store =
    std::unordered_map<std::string, graph_value, wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>;

inline constexpr std::string_view invoke_cache_store_session_key =
    "compose.graph.invoke_cache";

inline auto acquire_store(wh::core::run_context &context) -> invoke_cache_store & {
  auto iter = context.session_values.find(invoke_cache_store_session_key);
  if (iter == context.session_values.end()) {
    auto inserted = context.session_values.emplace(
        std::string{invoke_cache_store_session_key},
        wh::core::any{std::in_place_type<invoke_cache_store>});
    iter = inserted.first;
  }
  auto *cache_ptr = wh::core::any_cast<invoke_cache_store>(&iter->second);
  if (cache_ptr == nullptr) {
    iter->second = wh::core::any{std::in_place_type<invoke_cache_store>};
    cache_ptr = wh::core::any_cast<invoke_cache_store>(&iter->second);
  }
  return *cache_ptr;
}

[[nodiscard]] inline auto make_node_cache_key(
    const std::string_view cache_namespace, const std::string_view runtime_scope,
    const std::string_view graph_name, const std::string_view node_key)
    -> std::string {
  std::string key{};
  key.reserve(cache_namespace.size() + 1U + runtime_scope.size() + 1U +
              graph_name.size() + 1U + node_key.size());
  key += cache_namespace;
  key.push_back(':');
  key += runtime_scope;
  key.push_back(':');
  key += graph_name;
  key.push_back(':');
  key += node_key;
  return key;
}

} // namespace wh::compose::detail::cache_runtime
