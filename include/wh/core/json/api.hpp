// Defines JSON value aliases and helper APIs for parsing, serialization,
// member lookup, and type-safe JSON field access.
#pragma once

#include <string>
#include <string_view>

#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "wh/core/error.hpp"
#include "wh/core/json/types.hpp"
#include "wh/core/result.hpp"

namespace wh::core {

/// Returns the unified JSON kind for a RapidJSON value.
[[nodiscard]] inline constexpr auto json_kind(const json_value &value) noexcept
    -> json_value_kind {
  if (value.IsNull()) {
    return json_value_kind::null_value;
  }
  if (value.IsBool()) {
    return json_value_kind::bool_value;
  }
  if (value.IsNumber()) {
    return json_value_kind::number_value;
  }
  if (value.IsString()) {
    return json_value_kind::string_value;
  }
  if (value.IsArray()) {
    return json_value_kind::array_value;
  }
  return json_value_kind::object_value;
}

/// Parses text into a DOM document and maps parse failures to `errc`.
[[nodiscard]] inline auto parse_json(const std::string_view text)
    -> result<json_document> {
  json_document document;
  document.Parse(text.data(), text.size());
  if (document.HasParseError()) {
    return result<json_document>::failure(errc::parse_error);
  }
  return document;
}

/// Parses text into a DOM document and returns detailed parse error info.
[[nodiscard]] inline auto parse_json_with_error(const std::string_view text)
    -> result<json_document, json_parse_error> {
  json_document document;
  document.Parse(text.data(), text.size());
  if (document.HasParseError()) {
    const json_parse_error parse_error{document.GetParseError(),
                                       document.GetErrorOffset()};
    return result<json_document, json_parse_error>::failure(parse_error);
  }

  return document;
}

/// Serializes a JSON value into compact text.
[[nodiscard]] inline auto json_to_string(const json_value &value)
    -> result<std::string> {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  if (!value.Accept(writer)) {
    return result<std::string>::failure(errc::serialize_error);
  }
  return std::string{buffer.GetString(), buffer.GetSize()};
}

/// Finds an object member by key.
[[nodiscard]] inline auto json_find_member(const json_value &value,
                                           const std::string_view key)
    -> result<const json_value *> {
  if (!value.IsObject()) {
    return result<const json_value *>::failure(errc::type_mismatch);
  }

  const auto member = value.FindMember(rapidjson::StringRef(
      key.data(), static_cast<rapidjson::SizeType>(key.size())));
  if (member == value.MemberEnd()) {
    return result<const json_value *>::failure(errc::not_found);
  }

  return &member->value;
}

/// Returns an array element by index.
[[nodiscard]] inline auto json_at(const json_value &value,
                                  const std::size_t index)
    -> result<const json_value *> {
  if (!value.IsArray()) {
    return result<const json_value *>::failure(errc::type_mismatch);
  }
  if (index >= value.Size()) {
    return result<const json_value *>::failure(errc::not_found);
  }
  return &value[static_cast<json_size_type>(index)];
}

/// Converts parse error code to readable text.
[[nodiscard]] inline auto parse_error_message(const json_parse_error &error)
    -> std::string {
  return std::string{rapidjson::GetParseError_En(error.code)};
}

} // namespace wh::core
