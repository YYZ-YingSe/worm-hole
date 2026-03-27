// Defines tool schema structures, parameter descriptors, and tool-choice
// controls used by model/tool integration.
#pragma once

#include <algorithm>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/json.hpp"
#include "wh/core/result.hpp"

namespace wh::schema {

/// Tool invocation policy for model-side tool usage.
enum class tool_call_mode {
  /// Disable tool calling for this request.
  disable,
  /// Allow model to decide whether to call tools.
  allow,
  /// Force calling one or more tools.
  force,
};

/// Supported parameter schema primitive/container kinds.
enum class tool_parameter_type {
  /// Parameter type is string.
  string,
  /// Parameter type is integer.
  integer,
  /// Placeholder expects numeric value.
  number,
  /// Placeholder expects boolean value.
  boolean,
  /// Parameter type is object.
  object,
  /// Parameter type is array.
  array,
};

/// Tool selection strategy sent to model requests.
struct tool_choice {
  /// Whether tools are disabled, optional, or mandatory.
  tool_call_mode mode{tool_call_mode::allow};
};

/// Recursive schema node used to describe tool parameters.
struct tool_parameter_schema {
  /// Parameter field name used in JSON schema.
  std::string name{};
  /// JSON-schema type for this parameter.
  tool_parameter_type type{tool_parameter_type::string};
  /// Human-readable parameter description.
  std::string description{};
  /// True means this property must be provided.
  bool required{false};
  /// Allowed enum literal values.
  std::vector<std::string> enum_values{};
  /// Object properties when `type == object`.
  std::vector<tool_parameter_schema> properties{};
  /// One-of alternatives for union-like parameters.
  std::vector<tool_parameter_schema> one_of{};
  /// Array item schema list when `type == array`.
  std::vector<tool_parameter_schema> item_types{};
};

/// Full tool schema definition used for registration/export.
struct tool_schema_definition {
  /// Registered tool/function name.
  std::string name{};
  /// Human-readable tool description.
  std::string description{};
  /// Structured parameter schema used when raw schema is absent.
  std::vector<tool_parameter_schema> parameters{};
  /// Prebuilt JSON schema string overriding `parameters` when set.
  std::string raw_parameters_json_schema{};
};

namespace detail {

/// Converts parameter type enum to JSON-schema type name.
[[nodiscard]] inline auto parameter_type_name(const tool_parameter_type type)
    -> std::string_view {
  switch (type) {
  case tool_parameter_type::string:
    return "string";
  case tool_parameter_type::integer:
    return "integer";
  case tool_parameter_type::number:
    return "number";
  case tool_parameter_type::boolean:
    return "boolean";
  case tool_parameter_type::object:
    return "object";
  case tool_parameter_type::array:
    return "array";
  }
  return "string";
}

/// Adds string member to a RapidJSON object.
inline auto add_json_string_member(wh::core::json_value &target,
                                   const std::string_view key,
                                   const std::string_view value,
                                   wh::core::json_allocator &allocator)
    -> void {
  wh::core::json_value key_node;
  key_node.SetString(
      key.data(), static_cast<wh::core::json_size_type>(key.size()), allocator);

  wh::core::json_value value_node;
  value_node.SetString(value.data(),
                       static_cast<wh::core::json_size_type>(value.size()),
                       allocator);
  target.AddMember(key_node.Move(), value_node.Move(), allocator);
}

/// Adds value member to a RapidJSON object.
inline auto add_json_value_member(wh::core::json_value &target,
                                  const std::string_view key,
                                  wh::core::json_value value,
                                  wh::core::json_allocator &allocator) -> void {
  wh::core::json_value key_node;
  key_node.SetString(
      key.data(), static_cast<wh::core::json_size_type>(key.size()), allocator);
  target.AddMember(key_node.Move(), value.Move(), allocator);
}

/// Builds JSON schema for one parameter node recursively.
[[nodiscard]] inline auto
build_parameter_schema(const tool_parameter_schema &parameter,
                       wh::core::json_allocator &allocator)
    -> wh::core::result<wh::core::json_value> {
  wh::core::json_value output;
  output.SetObject();
  add_json_string_member(output, "type", parameter_type_name(parameter.type),
                         allocator);
  if (!parameter.description.empty()) {
    add_json_string_member(output, "description", parameter.description,
                           allocator);
  }

  if (!parameter.enum_values.empty()) {
    wh::core::json_value enum_values;
    enum_values.SetArray();
    for (const auto &item : parameter.enum_values) {
      wh::core::json_value value;
      value.SetString(item.data(),
                      static_cast<wh::core::json_size_type>(item.size()),
                      allocator);
      enum_values.PushBack(value.Move(), allocator);
    }
    add_json_value_member(output, "enum", std::move(enum_values), allocator);
  }

  if (!parameter.one_of.empty()) {
    wh::core::json_value one_of;
    one_of.SetArray();
    for (const auto &item : parameter.one_of) {
      auto nested = build_parameter_schema(item, allocator);
      if (nested.has_error()) {
        return wh::core::result<wh::core::json_value>::failure(nested.error());
      }
      one_of.PushBack(std::move(nested).value(), allocator);
    }
    add_json_value_member(output, "oneOf", std::move(one_of), allocator);
  }

  if (parameter.type == tool_parameter_type::object) {
    wh::core::json_value properties;
    properties.SetObject();
    wh::core::json_value required;
    required.SetArray();

    std::vector<const tool_parameter_schema *> sorted_properties{};
    sorted_properties.reserve(parameter.properties.size());
    for (const auto &property : parameter.properties) {
      sorted_properties.push_back(&property);
    }
    std::ranges::sort(sorted_properties,
                      [](const tool_parameter_schema *left,
                         const tool_parameter_schema *right) {
                        return left->name < right->name;
                      });
    for (const auto *property : sorted_properties) {
      auto property_schema = build_parameter_schema(*property, allocator);
      if (property_schema.has_error()) {
        return wh::core::result<wh::core::json_value>::failure(
            property_schema.error());
      }

      wh::core::json_value key;
      key.SetString(property->name.data(),
                    static_cast<wh::core::json_size_type>(property->name.size()),
                    allocator);
      properties.AddMember(key.Move(), std::move(property_schema).value(),
                           allocator);
      if (property->required) {
        wh::core::json_value required_key;
        required_key.SetString(
            property->name.data(),
            static_cast<wh::core::json_size_type>(property->name.size()),
            allocator);
        required.PushBack(required_key.Move(), allocator);
      }
    }

    add_json_value_member(output, "properties", std::move(properties),
                          allocator);
    add_json_value_member(output, "required", std::move(required), allocator);
  }

  if (parameter.type == tool_parameter_type::array) {
    if (!parameter.item_types.empty()) {
      if (parameter.item_types.size() == 1U) {
        auto item_schema =
            build_parameter_schema(parameter.item_types.front(), allocator);
        if (item_schema.has_error()) {
          return wh::core::result<wh::core::json_value>::failure(
              item_schema.error());
        }
        add_json_value_member(output, "items", std::move(item_schema).value(),
                              allocator);
      } else {
        wh::core::json_value any_of;
        any_of.SetArray();
        for (const auto &item : parameter.item_types) {
          auto nested = build_parameter_schema(item, allocator);
          if (nested.has_error()) {
            return wh::core::result<wh::core::json_value>::failure(
                nested.error());
          }
          any_of.PushBack(std::move(nested).value(), allocator);
        }
        wh::core::json_value items;
        items.SetObject();
        add_json_value_member(items, "anyOf", std::move(any_of), allocator);
        add_json_value_member(output, "items", std::move(items), allocator);
      }
    } else {
      wh::core::json_value items;
      items.SetObject();
      add_json_string_member(items, "type", "string", allocator);
      add_json_value_member(output, "items", std::move(items), allocator);
    }
  }

  return output;
}

} // namespace detail

/// Builds root JSON schema object for tool parameters.
[[nodiscard]] inline auto build_parameters_json_schema(
    const std::span<const tool_parameter_schema> parameters)
    -> wh::core::result<wh::core::json_document> {
  wh::core::json_document output;
  output.SetObject();
  detail::add_json_string_member(output, "type", "object",
                                 output.GetAllocator());

  wh::core::json_value properties;
  properties.SetObject();
  wh::core::json_value required;
  required.SetArray();

  std::vector<const tool_parameter_schema *> sorted_parameters{};
  sorted_parameters.reserve(parameters.size());
  for (const auto &parameter : parameters) {
    sorted_parameters.push_back(&parameter);
  }
  std::ranges::sort(sorted_parameters,
                    [](const tool_parameter_schema *left,
                       const tool_parameter_schema *right) {
                      return left->name < right->name;
                    });
  for (const auto *parameter : sorted_parameters) {
    auto schema =
        detail::build_parameter_schema(*parameter, output.GetAllocator());
    if (schema.has_error()) {
      return wh::core::result<wh::core::json_document>::failure(schema.error());
    }

    wh::core::json_value key;
    key.SetString(parameter->name.data(),
                  static_cast<wh::core::json_size_type>(parameter->name.size()),
                  output.GetAllocator());
    properties.AddMember(key.Move(), std::move(schema).value(),
                         output.GetAllocator());
    if (parameter->required) {
      wh::core::json_value required_name;
      required_name.SetString(
          parameter->name.data(),
          static_cast<wh::core::json_size_type>(parameter->name.size()),
          output.GetAllocator());
      required.PushBack(required_name.Move(), output.GetAllocator());
    }
  }

  detail::add_json_value_member(output, "properties", std::move(properties),
                                output.GetAllocator());
  detail::add_json_value_member(output, "required", std::move(required),
                                output.GetAllocator());
  return output;
}

/// Builds the default function-style tool JSON schema export.
[[nodiscard]] inline auto
build_default_tool_json_schema(const tool_schema_definition &definition)
    -> wh::core::result<wh::core::json_document> {
  if (definition.name.empty()) {
    return wh::core::result<wh::core::json_document>::failure(
        wh::core::errc::invalid_argument);
  }

  wh::core::json_document output;
  output.SetObject();
  detail::add_json_string_member(output, "type", "function",
                                 output.GetAllocator());

  wh::core::json_value function;
  function.SetObject();
  detail::add_json_string_member(function, "name", definition.name,
                                 output.GetAllocator());
  detail::add_json_string_member(function, "description",
                                 definition.description, output.GetAllocator());

  wh::core::json_value parameters;
  if (!definition.raw_parameters_json_schema.empty()) {
    auto raw = wh::core::parse_json(definition.raw_parameters_json_schema);
    if (raw.has_error()) {
      return wh::core::result<wh::core::json_document>::failure(
          wh::core::errc::parse_error);
    }
    parameters.CopyFrom(raw.value(), output.GetAllocator());
  } else {
    auto built = build_parameters_json_schema(definition.parameters);
    if (built.has_error()) {
      return wh::core::result<wh::core::json_document>::failure(built.error());
    }
    parameters.CopyFrom(built.value(), output.GetAllocator());
  }
  detail::add_json_value_member(function, "parameters", std::move(parameters),
                                output.GetAllocator());
  detail::add_json_value_member(output, "function", std::move(function),
                                output.GetAllocator());
  return output;
}

} // namespace wh::schema
