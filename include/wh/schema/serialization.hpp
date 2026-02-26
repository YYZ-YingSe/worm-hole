#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/types/json_types.hpp"
#include "wh/internal/serialization.hpp"
#include "wh/schema/serialization_registry.hpp"

namespace wh::schema {

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

template <typename type_t>
[[nodiscard]] inline auto deserialize_fast(const wh::core::json_value &input)
    -> wh::core::result<type_t> {
  return wh::internal::from_json_value<type_t>(input);
}

[[nodiscard]] inline auto
register_default_types(serialization_registry &registry)
    -> wh::core::result<void> {
  auto status =
      registry.register_type<std::string>("std.string", {"string", "text"});
  if (status.has_error()) {
    return status;
  }

  status = registry.register_type<bool>("std.bool", {"bool"});
  if (status.has_error()) {
    return status;
  }

  status =
      registry.register_type<std::int64_t>("std.i64", {"int64", "integer"});
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

  status = registry.register_type<std::vector<std::string>>("std.vector.string",
                                                            {"list.string"});
  if (status.has_error()) {
    return status;
  }

  status = registry.register_type<std::map<std::string, std::string>>(
      "std.map.string.string", {"map.string.string"});
  if (status.has_error()) {
    return status;
  }

  return {};
}

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
