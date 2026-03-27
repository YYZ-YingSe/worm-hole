// Provides declarations and utilities for `wh/tool/utils/schema_infer.hpp`.
#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/core/function.hpp"
#include "wh/schema/tool.hpp"

namespace wh::tool::utils {

using schema_modifier =
    wh::core::callback_function<
        void(wh::schema::tool_parameter_schema &, std::string_view) const>;

namespace detail {

/// Applies schema modifier recursively across nested schema nodes.
inline auto apply_modifier_recursive(wh::schema::tool_parameter_schema &schema,
                                     const std::string &path,
                                     const schema_modifier &modifier) -> void {
  if (modifier) {
    modifier(schema, path.empty() ? std::string_view{"_root"}
                                  : std::string_view{path});
  }

  for (auto &property : schema.properties) {
    std::string property_path = path;
    if (!property_path.empty()) {
      property_path += '.';
    }
    property_path += property.name;
    apply_modifier_recursive(property, property_path, modifier);
  }

  for (std::size_t index = 0U; index < schema.item_types.size(); ++index) {
    std::string item_path = path;
    item_path += "[";
    item_path += std::to_string(index);
    item_path += "]";
    apply_modifier_recursive(schema.item_types[index], item_path, modifier);
  }

  for (std::size_t index = 0U; index < schema.one_of.size(); ++index) {
    std::string one_of_path = path;
    one_of_path += ".one_of[";
    one_of_path += std::to_string(index);
    one_of_path += "]";
    apply_modifier_recursive(schema.one_of[index], one_of_path, modifier);
  }
}

} // namespace detail

template <typename params_t, typename name_t, typename description_t>
  requires std::constructible_from<std::string, name_t &&> &&
           std::constructible_from<std::string, description_t &&>
/// Builds minimal schema definition (without inferred parameters).
[[nodiscard]] inline auto infer_schema(name_t &&name, description_t &&description)
    -> wh::schema::tool_schema_definition {
  [[maybe_unused]] constexpr auto params_size = sizeof(params_t);
  wh::schema::tool_schema_definition schema{};
  schema.name = std::forward<name_t>(name);
  schema.description = std::forward<description_t>(description);
  return schema;
}

template <typename name_t, typename description_t, typename parameters_t>
  requires std::constructible_from<std::string, name_t &&> &&
           std::constructible_from<std::string, description_t &&> &&
           std::constructible_from<
               std::vector<wh::schema::tool_parameter_schema>, parameters_t &&>
/// Builds schema definition with explicit parameter list.
[[nodiscard]] inline auto infer_schema(
    name_t &&name, description_t &&description, parameters_t &&parameters)
    -> wh::schema::tool_schema_definition {
  wh::schema::tool_schema_definition schema{};
  schema.name = std::forward<name_t>(name);
  schema.description = std::forward<description_t>(description);
  schema.parameters = std::vector<wh::schema::tool_parameter_schema>{
      std::forward<parameters_t>(parameters)};
  return schema;
}

template <typename name_t, typename description_t, typename parameters_t>
  requires std::constructible_from<std::string, name_t &&> &&
           std::constructible_from<std::string, description_t &&> &&
           std::constructible_from<
               std::vector<wh::schema::tool_parameter_schema>, parameters_t &&>
/// Builds schema and runs recursive modifier on all parameter nodes.
[[nodiscard]] inline auto infer_schema(
    name_t &&name, description_t &&description, parameters_t &&parameters,
    const schema_modifier &modifier) -> wh::schema::tool_schema_definition {
  auto stored_parameters = std::vector<wh::schema::tool_parameter_schema>{
      std::forward<parameters_t>(parameters)};
  for (auto &parameter : stored_parameters) {
    detail::apply_modifier_recursive(parameter, parameter.name, modifier);
  }
  return infer_schema(std::forward<name_t>(name),
                      std::forward<description_t>(description),
                      std::move(stored_parameters));
}

} // namespace wh::tool::utils
