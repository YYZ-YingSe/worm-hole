#pragma once

#include <string>
#include <string_view>

#include <rapidjson/error/en.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/types/json_types.hpp"

namespace wh::core {

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

[[nodiscard]] inline auto parse_json(const std::string_view text)
    -> result<json_document> {
  json_document document;
  document.Parse(text.data(), text.size());
  if (document.HasParseError()) {
    return result<json_document>::failure(errc::parse_error);
  }
  return document;
}

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

[[nodiscard]] inline auto json_to_string(const json_value &value)
    -> result<std::string> {
  rapidjson::StringBuffer buffer;
  rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
  if (!value.Accept(writer)) {
    return result<std::string>::failure(errc::serialize_error);
  }
  return std::string{buffer.GetString(), buffer.GetSize()};
}

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

[[nodiscard]] inline auto parse_error_message(const json_parse_error &error)
    -> std::string {
  return std::string{rapidjson::GetParseError_En(error.code)};
}

} // namespace wh::core
