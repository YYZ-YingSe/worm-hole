#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace wh::internal {

namespace detail {

template <typename t>
using remove_cvref_t = std::remove_cv_t<std::remove_reference_t<t>>;

[[nodiscard]] constexpr bool is_ascii_digit(const char ch) noexcept {
  return ch >= '0' && ch <= '9';
}

[[nodiscard]] constexpr std::string_view
trim_ascii_spaces(std::string_view value) noexcept {
  while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
    value.remove_prefix(1U);
  }
  while (!value.empty() && (value.back() == ' ' || value.back() == '\t')) {
    value.remove_suffix(1U);
  }
  return value;
}

[[nodiscard]] constexpr bool
has_numeric_suffix(std::string_view value) noexcept {
  if (value.empty()) {
    return false;
  }

  std::size_t index = value.size();
  while (index > 0U && is_ascii_digit(value[index - 1U])) {
    --index;
  }

  if (index == value.size()) {
    return false;
  }

  if (index == 0U) {
    return true;
  }

  const char marker = value[index - 1U];
  return marker == '_' || marker == '$' || marker == '#';
}

template <typename t>
[[nodiscard]] constexpr std::string_view raw_type_name() noexcept {
#if defined(__clang__)
  return __PRETTY_FUNCTION__;
#elif defined(__GNUC__)
  return __PRETTY_FUNCTION__;
#elif defined(_MSC_VER)
  return __FUNCSIG__;
#else
  return "";
#endif
}

template <typename t>
[[nodiscard]] constexpr std::string_view extract_pretty_name() noexcept {
  std::string_view raw = raw_type_name<t>();

#if defined(__clang__)
  constexpr std::string_view prefix =
      "std::string_view wh::internal::detail::raw_type_name() [t = ";
  constexpr std::string_view suffix = "]";
#elif defined(__GNUC__)
  constexpr std::string_view prefix =
      "constexpr std::string_view wh::internal::detail::raw_type_name() [with "
      "t = ";
  constexpr std::string_view suffix =
      "; std::string_view = std::basic_string_view<char>]";
#elif defined(_MSC_VER)
  constexpr std::string_view prefix =
      "class std::basic_string_view<char,struct std::char_traits<char> > "
      "__cdecl "
      "wh::internal::detail::raw_type_name<";
  constexpr std::string_view suffix = ">(void) noexcept";
#else
  constexpr std::string_view prefix = "";
  constexpr std::string_view suffix = "";
#endif

  if constexpr (prefix.empty()) {
    return trim_ascii_spaces(raw);
  }

  const auto begin = raw.find(prefix);
  if (begin == std::string_view::npos) {
    return trim_ascii_spaces(raw);
  }

  const auto start = begin + prefix.size();
  const auto end = raw.rfind(suffix);
  if (end == std::string_view::npos || end <= start) {
    return trim_ascii_spaces(raw.substr(start));
  }

  return trim_ascii_spaces(raw.substr(start, end - start));
}

} // namespace detail

template <typename t>
[[nodiscard]] constexpr std::string_view stable_type_name() noexcept {
  using normalized_t = detail::remove_cvref_t<t>;
  return detail::extract_pretty_name<normalized_t>();
}

template <typename t> struct type_alias {
  static constexpr std::string_view value{};
};

template <typename t>
inline constexpr bool has_explicit_type_alias_v =
    !type_alias<detail::remove_cvref_t<t>>::value.empty();

template <typename t>
[[nodiscard]] constexpr std::string_view diagnostic_type_alias() noexcept {
  using normalized_t = detail::remove_cvref_t<t>;
  if constexpr (has_explicit_type_alias_v<normalized_t>) {
    return type_alias<normalized_t>::value;
  }
  return stable_type_name<normalized_t>();
}

template <typename t>
[[nodiscard]] constexpr std::string_view persistent_type_alias() noexcept {
  using normalized_t = detail::remove_cvref_t<t>;
  static_assert(has_explicit_type_alias_v<normalized_t>,
                "persistent type serialization requires explicit "
                "wh::internal::type_alias<T>");
  return type_alias<normalized_t>::value;
}

[[nodiscard]] constexpr std::uint64_t
stable_name_hash(const std::string_view value) noexcept {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const char ch : value) {
    hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
    hash *= 1099511628211ULL;
  }
  return hash;
}

template <typename t>
[[nodiscard]] constexpr std::uint64_t stable_type_hash() noexcept {
  return stable_name_hash(diagnostic_type_alias<t>());
}

template <typename t>
[[nodiscard]] constexpr std::uint64_t persistent_type_hash() noexcept {
  return stable_name_hash(persistent_type_alias<t>());
}

template <typename... ts> struct type_alias_registry {
  using item = std::pair<std::string_view, std::uint64_t>;

  static_assert(
      (has_explicit_type_alias_v<ts> && ...),
      "type_alias_registry requires explicit aliases for all registered types");

  static constexpr std::array<item, sizeof...(ts)> entries = {
      item{persistent_type_alias<ts>(), persistent_type_hash<ts>()}...,
  };

  [[nodiscard]] static constexpr std::optional<std::uint64_t>
  find_hash(const std::string_view alias) noexcept {
    for (const auto &entry : entries) {
      if (entry.first == alias) {
        return entry.second;
      }
    }
    return std::nullopt;
  }

  [[nodiscard]] static constexpr std::string_view
  find_alias(const std::uint64_t hash) noexcept {
    for (const auto &entry : entries) {
      if (entry.second == hash) {
        return entry.first;
      }
    }
    return {};
  }
};

template <typename... ts>
constexpr std::array<typename type_alias_registry<ts...>::item, sizeof...(ts)>
    type_alias_registry<ts...>::entries;

[[nodiscard]] constexpr std::string_view
stable_function_name(const std::string_view runtime_name) noexcept {
  const auto trimmed = detail::trim_ascii_spaces(runtime_name);
  if (trimmed.empty()) {
    return {};
  }

  if (trimmed.find("lambda") != std::string_view::npos) {
    return {};
  }

  if (detail::has_numeric_suffix(trimmed)) {
    return {};
  }

  return trimmed;
}

[[nodiscard]] constexpr std::string_view
stable_runtime_type_name(const std::string_view runtime_name) noexcept {
  const auto trimmed = detail::trim_ascii_spaces(runtime_name);
  if (trimmed.empty()) {
    return {};
  }

  if (trimmed.find("lambda") != std::string_view::npos) {
    return {};
  }

  if (detail::has_numeric_suffix(trimmed)) {
    return {};
  }

  return trimmed;
}

} // namespace wh::internal
