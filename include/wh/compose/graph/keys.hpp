// Defines the public reserved graph node keys.
#pragma once

#include <string_view>

namespace wh::compose {

/// Reserved START node key.
inline constexpr std::string_view graph_start_node_key = "__start__";
/// Reserved END node key.
inline constexpr std::string_view graph_end_node_key = "__end__";

} // namespace wh::compose
