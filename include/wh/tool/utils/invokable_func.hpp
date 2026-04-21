// Provides declarations and utilities for `wh/tool/utils/invokable_func.hpp`.
#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <utility>

#include "wh/core/function.hpp"
#include "wh/core/json.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/internal/serialization.hpp"
#include "wh/tool/tool.hpp"

namespace wh::tool::utils {

namespace detail {

template <typename> inline constexpr bool always_false_v = false;

template <typename result_t> struct result_value;

template <typename value_t, typename error_t>
/// Extracts value type from `wh::core::result<T, E>`.
struct result_value<wh::core::result<value_t, error_t>> {
  using type = value_t;
};

template <typename value_t>
/// Serializes tool output value into response text.
[[nodiscard]] inline auto serialize_tool_value(value_t &&value) -> wh::core::result<std::string> {
  using normalized_t = wh::core::remove_cvref_t<value_t>;

  if constexpr (std::same_as<normalized_t, std::string>) {
    return std::forward<value_t>(value);
  } else if constexpr (std::convertible_to<normalized_t, std::string_view>) {
    return std::string{std::string_view{std::forward<value_t>(value)}};
  } else {
    wh::core::json_document document;
    auto encoded = wh::internal::to_json(value, document, document.GetAllocator());
    if (encoded.has_error()) {
      return wh::core::result<std::string>::failure(encoded.error());
    }
    return wh::core::json_to_string(document);
  }
}

template <typename output_t>
/// Normalizes invoke result/value into `result<string>`.
[[nodiscard]] inline auto normalize_invokable_output(output_t &&output)
    -> wh::core::result<std::string> {
  using normalized_t = wh::core::remove_cvref_t<output_t>;
  if constexpr (wh::core::is_result_v<normalized_t>) {
    auto status = std::forward<output_t>(output);
    if (status.has_error()) {
      return wh::core::result<std::string>::failure(status.error());
    }
    using value_t = typename result_value<normalized_t>::type;
    if constexpr (std::same_as<value_t, void>) {
      return std::string{};
    } else {
      return serialize_tool_value(std::move(status).value());
    }
  } else if constexpr (std::same_as<normalized_t, void>) {
    return wh::core::result<std::string>::failure(wh::core::errc::contract_violation);
  } else {
    return serialize_tool_value(std::forward<output_t>(output));
  }
}

} // namespace detail

template <typename params_t>
using input_deserializer = wh::core::function<wh::core::result<params_t>(std::string_view) const>;

template <typename params_t>
/// Decodes tool input text into typed params.
[[nodiscard]] inline auto
decode_tool_input(const std::string_view input,
                  const input_deserializer<params_t> &custom_deserializer = nullptr)
    -> wh::core::result<params_t> {
  if (custom_deserializer) {
    auto decoded = custom_deserializer(input);
    if (decoded.has_error()) {
      return wh::core::result<params_t>::failure(decoded.error());
    }
    return decoded;
  }

  auto parsed = wh::core::parse_json(input);
  if (parsed.has_error()) {
    return wh::core::result<params_t>::failure(parsed.error());
  }

  params_t output{};
  auto decoded = wh::internal::from_json(parsed.value(), output);
  if (decoded.has_error()) {
    return wh::core::result<params_t>::failure(decoded.error());
  }
  return output;
}

template <typename function_t>
/// Adapts untyped invoke function into one invoke-compatible callable.
[[nodiscard]] inline auto make_invokable_func(function_t &&function) {
  using stored_function_t = wh::core::remove_cvref_t<function_t>;
  return [function = stored_function_t{std::forward<function_t>(function)}](
             const std::string_view input,
             const tool_options &options) -> wh::core::result<std::string> {
    using output_t =
        wh::core::callable_result_t<stored_function_t, std::string_view, const tool_options &>;
    static_assert(!std::same_as<wh::core::remove_cvref_t<output_t>, void>,
                  "invokable function must return value or result<value>");
    return detail::normalize_invokable_output(std::invoke(function, input, options));
  };
}

template <typename params_t, typename function_t>
/// Adapts typed invoke function into one invoke-compatible callable.
[[nodiscard]] inline auto
make_invokable_func(function_t &&function,
                    input_deserializer<params_t> custom_deserializer = nullptr) {
  using stored_function_t = wh::core::remove_cvref_t<function_t>;
  return [function = stored_function_t{std::forward<function_t>(function)},
          custom_deserializer = std::move(custom_deserializer)](
             const std::string_view input,
             const tool_options &options) -> wh::core::result<std::string> {
    auto decoded = decode_tool_input<params_t>(input, custom_deserializer);
    if (decoded.has_error()) {
      return wh::core::result<std::string>::failure(decoded.error());
    }

    if constexpr (wh::core::callable_with<stored_function_t, const params_t &,
                                          const tool_options &>) {
      using output_t =
          wh::core::callable_result_t<stored_function_t, const params_t &, const tool_options &>;
      static_assert(!std::same_as<wh::core::remove_cvref_t<output_t>, void>,
                    "typed invokable function must return value or result<value>");
      return detail::normalize_invokable_output(std::invoke(function, decoded.value(), options));
    } else if constexpr (wh::core::callable_with<stored_function_t, params_t,
                                                 const tool_options &>) {
      using output_t =
          wh::core::callable_result_t<stored_function_t, params_t, const tool_options &>;
      auto value = std::move(decoded).value();
      static_assert(!std::same_as<wh::core::remove_cvref_t<output_t>, void>,
                    "typed invokable function must return value or result<value>");
      return detail::normalize_invokable_output(std::invoke(function, std::move(value), options));
    } else {
      static_assert(detail::always_false_v<stored_function_t>,
                    "typed invokable function must accept (params_t, options)");
    }
  };
}

} // namespace wh::tool::utils
