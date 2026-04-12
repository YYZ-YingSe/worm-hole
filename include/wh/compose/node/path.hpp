// Defines compose node-path helpers built on top of core hierarchical address.
#pragma once

#include <initializer_list>
#include <span>
#include <string_view>
#include <vector>

#include "wh/core/address.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::compose {

/// Stable node path type used by workflow/routing targeting.
using node_path = wh::core::address;

/// Builds one node path from ordered path segments.
[[nodiscard]] inline auto
make_node_path(std::initializer_list<std::string_view> segments) -> node_path {
  return wh::core::make_address(segments);
}

/// Builds one node path from one pre-materialized segment span.
[[nodiscard]] inline auto
make_node_path(const std::span<const std::string_view> segments) -> node_path {
  return wh::core::make_address(segments);
}

/// Parses one slash-separated path string into a node path.
[[nodiscard]] inline auto parse_node_path(const std::string_view text)
    -> wh::core::result<node_path> {
  if (text.empty()) {
    return node_path{};
  }

  std::vector<std::string_view> segments{};
  std::size_t begin = 0U;
  while (begin <= text.size()) {
    const auto end = text.find('/', begin);
    const auto stop = end == std::string_view::npos ? text.size() : end;
    const auto segment = text.substr(begin, stop - begin);
    if (segment.empty()) {
      return wh::core::result<node_path>::failure(
          wh::core::errc::invalid_argument);
    }
    segments.push_back(segment);
    if (end == std::string_view::npos) {
      break;
    }
    begin = end + 1U;
  }

  return make_node_path(
      std::span<const std::string_view>{segments.data(), segments.size()});
}

} // namespace wh::compose
