#pragma once

#include <cstddef>

#include <rapidjson/document.h>

namespace wh::core {

/// Owning DOM document type used by framework JSON helpers.
using json_document = rapidjson::Document;
/// Non-owning JSON node type inside a `json_document`.
using json_value = rapidjson::Value;
/// Allocator type bound to `json_document`.
using json_allocator = rapidjson::Document::AllocatorType;
/// RapidJSON array/object size index type.
using json_size_type = rapidjson::SizeType;

/// Framework-level JSON kind abstraction.
enum class json_value_kind {
  /// JSON null value kind.
  null_value,
  /// JSON boolean value kind.
  bool_value,
  /// JSON number value kind.
  number_value,
  /// JSON string value kind.
  string_value,
  /// JSON array value kind.
  array_value,
  /// JSON object value kind.
  object_value,
};

/// Lightweight parse error payload from JSON parser.
struct json_parse_error {
  /// Parser failure reason code from RapidJSON.
  rapidjson::ParseErrorCode code{rapidjson::kParseErrorNone};
  /// Byte offset where parsing stopped.
  std::size_t offset{0U};
};

} // namespace wh::core
