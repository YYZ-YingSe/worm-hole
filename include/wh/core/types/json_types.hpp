#pragma once

#include <cstddef>

#include <rapidjson/document.h>

namespace wh::core {

using json_document = rapidjson::Document;
using json_value = rapidjson::Value;
using json_allocator = rapidjson::Document::AllocatorType;
using json_size_type = rapidjson::SizeType;

enum class json_value_kind {
  null_value,
  bool_value,
  number_value,
  string_value,
  array_value,
  object_value,
};

struct json_parse_error {
  rapidjson::ParseErrorCode code{rapidjson::kParseErrorNone};
  std::size_t offset{0U};
};

} // namespace wh::core
