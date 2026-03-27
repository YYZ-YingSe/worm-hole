// Defines JSON serialization/deserialization concepts used to constrain
// generic conversion helpers and schema tooling.
#pragma once

#include <concepts>

#include <rapidjson/document.h>

#include "wh/core/type_traits.hpp"

namespace wh::core {

/// RapidJSON-compatible value/document concept used by generic JSON helpers.
template <typename value_t>
concept json_value_like =
    requires(remove_cvref_t<value_t> value, const remove_cvref_t<value_t> cvalue) {
  { cvalue.IsNull() } -> std::convertible_to<bool>;
  { cvalue.IsBool() } -> std::convertible_to<bool>;
  { cvalue.IsNumber() } -> std::convertible_to<bool>;
  { cvalue.IsString() } -> std::convertible_to<bool>;
  { cvalue.IsArray() } -> std::convertible_to<bool>;
  { cvalue.IsObject() } -> std::convertible_to<bool>;
  { cvalue.GetBool() } -> std::convertible_to<bool>;
  { cvalue.GetString() } -> std::same_as<const char *>;
  { cvalue.GetStringLength() } -> std::convertible_to<rapidjson::SizeType>;
  { cvalue.MemberCount() } -> std::convertible_to<rapidjson::SizeType>;
  { cvalue.Size() } -> std::convertible_to<rapidjson::SizeType>;

  value.SetNull();
  value.SetArray();
  value.SetObject();
};

static_assert(json_value_like<rapidjson::Value>);
static_assert(json_value_like<rapidjson::Document>);

} // namespace wh::core
