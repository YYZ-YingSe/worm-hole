// Defines common component concepts, including descriptor provider checks
// used by compile-time validation in generic component code.
#pragma once

#include <concepts>

#include "wh/core/component/types.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::core {

/// Requires a component to expose static descriptor metadata.
template <typename component_t>
concept component_descriptor_provider = requires(const remove_cvref_t<component_t> &component) {
  { component.descriptor() } -> std::convertible_to<component_descriptor>;
};

/// Requires synchronous invoke entry returning typed `result`.
template <typename component_t, typename request_t, typename response_t>
concept invokable_component =
    requires(component_t component, const request_t &request, run_context &context) {
      { component.invoke(request, context) } -> std::same_as<result<response_t>>;
    };

/// Requires stream entry returning typed stream wrapper in `result`.
template <typename component_t, typename request_t, typename stream_t>
concept streamable_component =
    requires(component_t component, const request_t &request, run_context &context) {
      { component.stream(request, context) } -> std::same_as<result<stream_t>>;
    };

} // namespace wh::core
