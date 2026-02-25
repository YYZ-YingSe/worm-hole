#pragma once

#include <array>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/types/json_types.hpp"

namespace wh::internal {

namespace detail {

template <typename type_t>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<type_t>>;

template <typename type_t>
concept custom_json_codec =
    requires(const remove_cvref_t<type_t> &value, wh::core::json_value &output,
             wh::core::json_allocator &allocator) {
      { wh_to_json(value, output, allocator) }
          -> std::same_as<wh::core::result<void>>;
    } &&
    requires(const wh::core::json_value &input, remove_cvref_t<type_t> &output) {
      { wh_from_json(input, output) } -> std::same_as<wh::core::result<void>>;
    };

template <typename type_t> struct is_std_array : std::false_type {};

template <typename value_t, std::size_t extent>
struct is_std_array<std::array<value_t, extent>> : std::true_type {};

template <typename type_t>
inline constexpr bool is_std_array_v =
    is_std_array<remove_cvref_t<type_t>>::value;

template <typename type_t> struct is_optional : std::false_type {};

template <typename value_t>
struct is_optional<std::optional<value_t>> : std::true_type {};

template <typename type_t>
inline constexpr bool is_optional_v = is_optional<remove_cvref_t<type_t>>::value;

template <typename type_t> struct is_unique_ptr : std::false_type {};

template <typename value_t, typename deleter_t>
struct is_unique_ptr<std::unique_ptr<value_t, deleter_t>> : std::true_type {};

template <typename type_t>
inline constexpr bool is_unique_ptr_v =
    is_unique_ptr<remove_cvref_t<type_t>>::value;

template <typename type_t> struct is_shared_ptr : std::false_type {};

template <typename value_t>
struct is_shared_ptr<std::shared_ptr<value_t>> : std::true_type {};

template <typename type_t>
inline constexpr bool is_shared_ptr_v =
    is_shared_ptr<remove_cvref_t<type_t>>::value;

template <typename type_t>
inline constexpr bool is_raw_pointer_v = std::is_pointer_v<remove_cvref_t<type_t>>;

template <typename type_t>
concept map_like = requires(remove_cvref_t<type_t> value,
                            typename remove_cvref_t<type_t>::key_type key,
                            typename remove_cvref_t<type_t>::mapped_type mapped) {
  typename remove_cvref_t<type_t>::key_type;
  typename remove_cvref_t<type_t>::mapped_type;
  value.clear();
  value.insert_or_assign(std::move(key), std::move(mapped));
  value.begin();
  value.end();
};

template <typename type_t>
concept sequence_like =
    (!map_like<type_t>) &&
    requires(remove_cvref_t<type_t> value,
             typename remove_cvref_t<type_t>::value_type item) {
      typename remove_cvref_t<type_t>::value_type;
      value.clear();
      value.push_back(std::move(item));
      value.begin();
      value.end();
    };

template <typename key_t>
[[nodiscard]] auto encode_json_key(const key_t &key)
    -> wh::core::result<std::string> {
  if constexpr (std::same_as<remove_cvref_t<key_t>, std::string>) {
    return key;
  } else if constexpr (std::integral<remove_cvref_t<key_t>> &&
                       !std::same_as<remove_cvref_t<key_t>, bool>) {
    return std::to_string(key);
  } else if constexpr (std::same_as<remove_cvref_t<key_t>, bool>) {
    return key ? std::string{"true"} : std::string{"false"};
  }

  return wh::core::result<std::string>::failure(wh::core::errc::not_supported);
}

template <typename key_t>
[[nodiscard]] auto decode_json_key(const std::string_view encoded)
    -> wh::core::result<key_t> {
  if constexpr (std::same_as<remove_cvref_t<key_t>, std::string>) {
    return std::string{encoded};
  } else if constexpr (std::integral<remove_cvref_t<key_t>> &&
                       !std::same_as<remove_cvref_t<key_t>, bool>) {
    key_t decoded{};
    const auto *begin = encoded.data();
    const auto *end = encoded.data() + encoded.size();
    const auto [parsed, error] = std::from_chars(begin, end, decoded);
    if (error != std::errc{} || parsed != end) {
      return wh::core::result<key_t>::failure(wh::core::errc::parse_error);
    }
    return decoded;
  } else if constexpr (std::same_as<remove_cvref_t<key_t>, bool>) {
    if (encoded == "true" || encoded == "1") {
      return true;
    }
    if (encoded == "false" || encoded == "0") {
      return false;
    }
    return wh::core::result<key_t>::failure(wh::core::errc::parse_error);
  }

  return wh::core::result<key_t>::failure(wh::core::errc::not_supported);
}

} // namespace detail

template <typename type_t>
auto to_json(const type_t &input, wh::core::json_value &output,
             wh::core::json_allocator &allocator) -> wh::core::result<void>;

template <typename type_t>
auto from_json(const wh::core::json_value &input, type_t &output)
    -> wh::core::result<void>;

template <typename type_t>
[[nodiscard]] auto to_json_value(const type_t &input,
                                 wh::core::json_allocator &allocator)
    -> wh::core::result<wh::core::json_value> {
  wh::core::json_value output;
  auto encoded = to_json(input, output, allocator);
  if (encoded.has_error()) {
    return wh::core::result<wh::core::json_value>::failure(encoded.error());
  }
  return output;
}

template <typename type_t>
[[nodiscard]] auto from_json_value(const wh::core::json_value &input)
    -> wh::core::result<type_t> {
  type_t output{};
  auto decoded = from_json(input, output);
  if (decoded.has_error()) {
    return wh::core::result<type_t>::failure(decoded.error());
  }
  return output;
}

template <typename type_t>
auto to_json(const type_t &input, wh::core::json_value &output,
             wh::core::json_allocator &allocator) -> wh::core::result<void> {
  using normalized_t = detail::remove_cvref_t<type_t>;

  try {
    if constexpr (detail::custom_json_codec<normalized_t>) {
      return wh_to_json(input, output, allocator);
    } else if constexpr (std::same_as<normalized_t, bool>) {
      output.SetBool(input);
      return {};
    } else if constexpr (std::integral<normalized_t> &&
                         !std::same_as<normalized_t, bool> &&
                         std::is_signed_v<normalized_t>) {
      output.SetInt64(static_cast<std::int64_t>(input));
      return {};
    } else if constexpr (std::integral<normalized_t> &&
                         !std::same_as<normalized_t, bool> &&
                         std::is_unsigned_v<normalized_t>) {
      output.SetUint64(static_cast<std::uint64_t>(input));
      return {};
    } else if constexpr (std::floating_point<normalized_t>) {
      output.SetDouble(static_cast<double>(input));
      return {};
    } else if constexpr (std::same_as<normalized_t, std::string>) {
      output.SetString(input.data(),
                       static_cast<wh::core::json_size_type>(input.size()),
                       allocator);
      return {};
    } else if constexpr (std::same_as<normalized_t, std::string_view>) {
      output.SetString(input.data(),
                       static_cast<wh::core::json_size_type>(input.size()),
                       allocator);
      return {};
    } else if constexpr (detail::is_optional_v<normalized_t>) {
      if (!input.has_value()) {
        output.SetNull();
        return {};
      }
      return to_json(*input, output, allocator);
    } else if constexpr (detail::is_unique_ptr_v<normalized_t> ||
                         detail::is_shared_ptr_v<normalized_t>) {
      if (!input) {
        output.SetNull();
        return {};
      }
      return to_json(*input, output, allocator);
    } else if constexpr (detail::is_raw_pointer_v<normalized_t>) {
      if (input == nullptr) {
        output.SetNull();
        return {};
      }
      return to_json(*input, output, allocator);
    } else if constexpr (detail::is_std_array_v<normalized_t>) {
      output.SetArray();
      for (const auto &item : input) {
        wh::core::json_value encoded_item;
        auto encoded = to_json(item, encoded_item, allocator);
        if (encoded.has_error()) {
          return wh::core::result<void>::failure(encoded.error());
        }
        output.PushBack(encoded_item.Move(), allocator);
      }
      return {};
    } else if constexpr (detail::sequence_like<normalized_t>) {
      output.SetArray();
      for (const auto &item : input) {
        wh::core::json_value encoded_item;
        auto encoded = to_json(item, encoded_item, allocator);
        if (encoded.has_error()) {
          return wh::core::result<void>::failure(encoded.error());
        }
        output.PushBack(encoded_item.Move(), allocator);
      }
      return {};
    } else if constexpr (detail::map_like<normalized_t>) {
      output.SetObject();
      for (const auto &[key, value] : input) {
        auto encoded_key = detail::encode_json_key(key);
        if (encoded_key.has_error()) {
          return wh::core::result<void>::failure(encoded_key.error());
        }

        wh::core::json_value name;
        const auto &name_string = encoded_key.value();
        name.SetString(
            name_string.data(),
            static_cast<wh::core::json_size_type>(name_string.size()),
            allocator);

        wh::core::json_value encoded_value;
        auto encoded = to_json(value, encoded_value, allocator);
        if (encoded.has_error()) {
          return wh::core::result<void>::failure(encoded.error());
        }
        output.AddMember(name.Move(), encoded_value.Move(), allocator);
      }
      return {};
    }
  } catch (const std::bad_alloc &) {
    return wh::core::result<void>::failure(wh::core::errc::resource_exhausted);
  } catch (...) {
    return wh::core::result<void>::failure(wh::core::errc::serialize_error);
  }

  return wh::core::result<void>::failure(wh::core::errc::not_supported);
}

template <typename type_t>
auto from_json(const wh::core::json_value &input, type_t &output)
    -> wh::core::result<void> {
  using normalized_t = detail::remove_cvref_t<type_t>;

  try {
    if constexpr (detail::custom_json_codec<normalized_t>) {
      return wh_from_json(input, output);
    } else if constexpr (std::same_as<normalized_t, bool>) {
      if (!input.IsBool()) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }
      output = input.GetBool();
      return {};
    } else if constexpr (std::integral<normalized_t> &&
                         !std::same_as<normalized_t, bool> &&
                         std::is_signed_v<normalized_t>) {
      if (!input.IsInt64()) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }
      const auto parsed = input.GetInt64();
      if (parsed < std::numeric_limits<normalized_t>::lowest() ||
          parsed > std::numeric_limits<normalized_t>::max()) {
        return wh::core::result<void>::failure(wh::core::errc::parse_error);
      }
      output = static_cast<normalized_t>(parsed);
      return {};
    } else if constexpr (std::integral<normalized_t> &&
                         !std::same_as<normalized_t, bool> &&
                         std::is_unsigned_v<normalized_t>) {
      if (!input.IsUint64()) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }
      const auto parsed = input.GetUint64();
      if (parsed > std::numeric_limits<normalized_t>::max()) {
        return wh::core::result<void>::failure(wh::core::errc::parse_error);
      }
      output = static_cast<normalized_t>(parsed);
      return {};
    } else if constexpr (std::floating_point<normalized_t>) {
      if (!input.IsNumber()) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }
      output = static_cast<normalized_t>(input.GetDouble());
      return {};
    } else if constexpr (std::same_as<normalized_t, std::string>) {
      if (!input.IsString()) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }
      output.assign(input.GetString(),
                    static_cast<std::size_t>(input.GetStringLength()));
      return {};
    } else if constexpr (detail::is_optional_v<normalized_t>) {
      if (input.IsNull()) {
        output.reset();
        return {};
      }
      typename normalized_t::value_type decoded_value{};
      auto decoded = from_json(input, decoded_value);
      if (decoded.has_error()) {
        return decoded;
      }
      output = std::move(decoded_value);
      return {};
    } else if constexpr (detail::is_unique_ptr_v<normalized_t>) {
      using pointee_t = typename normalized_t::element_type;
      if (input.IsNull()) {
        output.reset();
        return {};
      }
      auto value = std::make_unique<pointee_t>();
      auto decoded = from_json(input, *value);
      if (decoded.has_error()) {
        return decoded;
      }
      output = std::move(value);
      return {};
    } else if constexpr (detail::is_shared_ptr_v<normalized_t>) {
      using pointee_t = typename normalized_t::element_type;
      if (input.IsNull()) {
        output.reset();
        return {};
      }
      auto value = std::make_shared<pointee_t>();
      auto decoded = from_json(input, *value);
      if (decoded.has_error()) {
        return decoded;
      }
      output = std::move(value);
      return {};
    } else if constexpr (detail::is_raw_pointer_v<normalized_t>) {
      using pointee_t = std::remove_pointer_t<normalized_t>;
      if (input.IsNull()) {
        output = nullptr;
        return {};
      }
      auto *value = new pointee_t{};
      auto decoded = from_json(input, *value);
      if (decoded.has_error()) {
        delete value;
        return decoded;
      }
      output = value;
      return {};
    } else if constexpr (detail::is_std_array_v<normalized_t>) {
      if (!input.IsArray()) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }
      if (input.Size() != output.size()) {
        return wh::core::result<void>::failure(wh::core::errc::parse_error);
      }
      for (std::size_t index = 0; index < output.size(); ++index) {
        auto decoded = from_json(
            input[static_cast<wh::core::json_size_type>(index)], output[index]);
        if (decoded.has_error()) {
          return decoded;
        }
      }
      return {};
    } else if constexpr (detail::sequence_like<normalized_t>) {
      if (!input.IsArray()) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }
      output.clear();
      for (const auto &item : input.GetArray()) {
        typename normalized_t::value_type decoded_item{};
        auto decoded = from_json(item, decoded_item);
        if (decoded.has_error()) {
          return decoded;
        }
        output.push_back(std::move(decoded_item));
      }
      return {};
    } else if constexpr (detail::map_like<normalized_t>) {
      if (!input.IsObject()) {
        return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
      }

      output.clear();
      for (const auto &member : input.GetObject()) {
        const std::string_view encoded_key{
            member.name.GetString(),
            static_cast<std::size_t>(member.name.GetStringLength())};
        auto key = detail::decode_json_key<typename normalized_t::key_type>(
            encoded_key);
        if (key.has_error()) {
          return wh::core::result<void>::failure(key.error());
        }

        typename normalized_t::mapped_type decoded_value{};
        auto decoded = from_json(member.value, decoded_value);
        if (decoded.has_error()) {
          return decoded;
        }
        output.insert_or_assign(std::move(key).value(), std::move(decoded_value));
      }
      return {};
    }
  } catch (const std::bad_alloc &) {
    return wh::core::result<void>::failure(wh::core::errc::resource_exhausted);
  } catch (...) {
    return wh::core::result<void>::failure(wh::core::errc::parse_error);
  }

  return wh::core::result<void>::failure(wh::core::errc::not_supported);
}

} // namespace wh::internal
