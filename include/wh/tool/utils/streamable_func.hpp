// Provides declarations and utilities for `wh/tool/utils/streamable_func.hpp`.
#pragma once

#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/tool/tool.hpp"

namespace wh::tool::utils {

template <typename function_t>
/// Adapts user function into one stream-compatible callable.
[[nodiscard]] inline auto make_streamable_func(function_t &&function) {
  using stored_function_t = wh::core::remove_cvref_t<function_t>;
  return [function = stored_function_t{std::forward<function_t>(function)}](
             const std::string_view input, const tool_options &options)
             -> wh::core::result<tool_stream_reader> {
    using output_t =
        wh::core::remove_cvref_t<wh::core::callable_result_t<
            stored_function_t, std::string_view, const tool_options &>>;
    if constexpr (std::same_as<output_t, wh::core::result<tool_stream_reader>>) {
      return std::invoke(function, input, options);
    } else if constexpr (std::same_as<output_t, tool_stream_reader>) {
      return std::invoke(function, input, options);
    } else {
      static_assert(std::same_as<output_t, void>,
                    "streamable function must return stream reader or result");
      return wh::core::result<tool_stream_reader>::failure(
          wh::core::errc::contract_violation);
    }
  };
}

} // namespace wh::tool::utils
