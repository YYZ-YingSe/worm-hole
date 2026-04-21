// Defines schema serialization entrypoints for encoding/decoding supported
// WH schema types to and from JSON values.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wh/core/json.hpp"
#include "wh/core/result.hpp"
#include "wh/internal/serialization.hpp"
#include "wh/schema/serialization/registry.hpp"

namespace wh::schema {

/// Fast-path serialization using compile-time codec dispatch.
template <typename type_t>
[[nodiscard]] inline auto serialize_fast(const type_t &value)
    -> wh::core::result<wh::core::json_document> {
  wh::core::json_document output;
  auto encoded = wh::internal::to_json(value, output, output.GetAllocator());
  if (encoded.has_error()) {
    return wh::core::result<wh::core::json_document>::failure(encoded.error());
  }
  return output;
}

/// Fast-path serialization into an existing JSON document.
template <typename type_t>
[[nodiscard]] inline auto serialize_fast_to(const type_t &value, wh::core::json_document &output)
    -> wh::core::result<void> {
  output.SetNull();
  return wh::internal::to_json(value, output, output.GetAllocator());
}

/// Fast-path deserialization using compile-time codec dispatch.
template <typename type_t>
[[nodiscard]] inline auto deserialize_fast(const wh::core::json_value &input)
    -> wh::core::result<type_t> {
  return wh::internal::from_json_value<type_t>(input);
}

/// Registers built-in default types into serialization registry.
[[nodiscard]] inline auto register_default_types(serialization_registry &registry)
    -> wh::core::result<void> {
  registry.reserve(7U, 12U);

  auto status = registry.register_type<std::string>("std.string", {"string", "text"});
  if (status.has_error()) {
    return status;
  }

  status = registry.register_type<bool>("std.bool", {"bool"});
  if (status.has_error()) {
    return status;
  }

  status = registry.register_type<std::int64_t>("std.i64", {"int64", "integer"});
  if (status.has_error()) {
    return status;
  }

  status = registry.register_type<std::uint64_t>("std.u64", {"uint64"});
  if (status.has_error()) {
    return status;
  }

  status = registry.register_type<double>("std.f64", {"double", "number"});
  if (status.has_error()) {
    return status;
  }

  status = registry.register_type<std::vector<std::string>>("std.vector.string", {"list.string"});
  if (status.has_error()) {
    return status;
  }

  status = registry.register_type<std::map<std::string, std::string>>("std.map.string.string",
                                                                      {"map.string.string"});
  if (status.has_error()) {
    return status;
  }

  return {};
}

/// Creates a registry preloaded with built-in default types.
[[nodiscard]] inline auto make_default_serialization_registry()
    -> wh::core::result<serialization_registry> {
  serialization_registry registry;
  auto status = register_default_types(registry);
  if (status.has_error()) {
    return wh::core::result<serialization_registry>::failure(status.error());
  }
  return registry;
}

} // namespace wh::schema
