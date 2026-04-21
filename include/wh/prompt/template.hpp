// Defines prompt-owned template syntax, structured context values, and text
// rendering.
#pragma once

#include <cctype>
#include <charconv>
#include <concepts>
#include <cstdint>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <minja/minja.hpp>
#include <nlohmann/json.hpp>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

namespace wh::prompt {

/// Supported prompt template syntaxes.
enum class template_syntax {
  /// Lightweight placeholder expansion using `{{ name }}` slots.
  placeholder,
  /// Jinja-compatible syntax backed by `minja`.
  jinja_compatible,
};

class template_value;

/// Ordered object storage for template bindings.
using template_object = std::map<std::string, template_value, std::less<>>;

/// Ordered array storage for template bindings.
using template_array = std::vector<template_value>;

/// Structured value tree used by prompt template bindings.
class template_value {
public:
  using storage_type = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string,
                                    template_array, template_object>;

  template_value() noexcept = default;
  template_value(std::nullptr_t) noexcept : storage_(nullptr) {}
  template_value(const bool value) noexcept : storage_(value) {}

  template <std::integral integer_t>
    requires(!std::same_as<std::remove_cvref_t<integer_t>, bool>)
  template_value(const integer_t value) noexcept : storage_(static_cast<std::int64_t>(value)) {}

  template <std::floating_point floating_t>
  template_value(const floating_t value) noexcept : storage_(static_cast<double>(value)) {}

  template_value(const char *value)
      : storage_(value == nullptr ? std::string{} : std::string{value}) {}
  template_value(const std::string &value) : storage_(value) {}
  template_value(std::string &&value) noexcept : storage_(std::move(value)) {}
  template_value(const std::string_view value) : storage_(std::string{value}) {}
  template_value(const template_array &value) : storage_(value) {}
  template_value(template_array &&value) noexcept : storage_(std::move(value)) {}
  template_value(const template_object &value) : storage_(value) {}
  template_value(template_object &&value) noexcept : storage_(std::move(value)) {}

  [[nodiscard]] auto storage() const noexcept -> const storage_type & { return storage_; }

  [[nodiscard]] auto is_null() const noexcept -> bool {
    return std::holds_alternative<std::nullptr_t>(storage_);
  }

  [[nodiscard]] auto string_if() const noexcept -> const std::string * {
    return std::get_if<std::string>(&storage_);
  }

  [[nodiscard]] auto integer_if() const noexcept -> const std::int64_t * {
    return std::get_if<std::int64_t>(&storage_);
  }

  [[nodiscard]] auto number_if() const noexcept -> const double * {
    return std::get_if<double>(&storage_);
  }

  [[nodiscard]] auto bool_if() const noexcept -> const bool * {
    return std::get_if<bool>(&storage_);
  }

  [[nodiscard]] auto array_if() const noexcept -> const template_array * {
    return std::get_if<template_array>(&storage_);
  }

  [[nodiscard]] auto object_if() const noexcept -> const template_object * {
    return std::get_if<template_object>(&storage_);
  }

private:
  storage_type storage_{nullptr};
};

/// Root object used by prompt template rendering.
using template_context = template_object;

namespace detail {

using ordered_json = nlohmann::ordered_json;

struct template_delimiters {
  std::string_view begin{};
  std::string_view end{};
};

[[nodiscard]] inline auto trim(std::string_view input) -> std::string_view {
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.front()))) {
    input.remove_prefix(1U);
  }
  while (!input.empty() && std::isspace(static_cast<unsigned char>(input.back()))) {
    input.remove_suffix(1U);
  }
  return input;
}

[[nodiscard]] inline auto resolve_delimiters(const template_syntax syntax) -> template_delimiters {
  switch (syntax) {
  case template_syntax::placeholder:
  case template_syntax::jinja_compatible:
    return {"{{", "}}"};
  }
  return {"{{", "}}"};
}

[[nodiscard]] inline auto template_value_to_json(const template_value &value) -> ordered_json;

[[nodiscard]] inline auto template_object_to_json(const template_object &object) -> ordered_json {
  ordered_json json_object = ordered_json::object();
  for (const auto &[key, value] : object) {
    json_object[key] = template_value_to_json(value);
  }
  return json_object;
}

[[nodiscard]] inline auto template_array_to_json(const template_array &array) -> ordered_json {
  ordered_json json_array = ordered_json::array();
  for (const auto &value : array) {
    json_array.push_back(template_value_to_json(value));
  }
  return json_array;
}

[[nodiscard]] inline auto template_value_to_json(const template_value &value) -> ordered_json {
  return std::visit(
      []<typename value_t>(const value_t &inner) -> ordered_json {
        using inner_t = std::remove_cvref_t<value_t>;
        if constexpr (std::same_as<inner_t, std::nullptr_t>) {
          return nullptr;
        } else if constexpr (std::same_as<inner_t, bool> || std::same_as<inner_t, std::int64_t> ||
                             std::same_as<inner_t, double> || std::same_as<inner_t, std::string>) {
          return inner;
        } else if constexpr (std::same_as<inner_t, template_array>) {
          return template_array_to_json(inner);
        } else {
          return template_object_to_json(inner);
        }
      },
      value.storage());
}

[[nodiscard]] inline auto stringify_placeholder_value(const template_value &value) -> std::string {
  return std::visit(
      []<typename value_t>(const value_t &inner) -> std::string {
        using inner_t = std::remove_cvref_t<value_t>;
        if constexpr (std::same_as<inner_t, std::nullptr_t>) {
          return "null";
        } else if constexpr (std::same_as<inner_t, bool>) {
          return inner ? "true" : "false";
        } else if constexpr (std::same_as<inner_t, std::int64_t>) {
          return std::to_string(inner);
        } else if constexpr (std::same_as<inner_t, double>) {
          std::ostringstream out;
          out << inner;
          return out.str();
        } else if constexpr (std::same_as<inner_t, std::string>) {
          return inner;
        } else {
          return template_value_to_json(template_value{inner}).dump();
        }
      },
      value.storage());
}

[[nodiscard]] inline auto find_object_value(const template_object &object,
                                            const std::string_view key)
    -> wh::core::result<const template_value *> {
  const auto iter = object.find(key);
  if (iter == object.end()) {
    return wh::core::result<const template_value *>::failure(wh::core::errc::not_found);
  }
  return std::addressof(iter->second);
}

[[nodiscard]] inline auto parse_index(const std::string_view token)
    -> wh::core::result<std::size_t> {
  std::size_t index = 0U;
  const auto *begin = token.data();
  const auto *end = token.data() + token.size();
  const auto parsed = std::from_chars(begin, end, index);
  if (parsed.ec != std::errc{} || parsed.ptr != end) {
    return wh::core::result<std::size_t>::failure(wh::core::errc::type_mismatch);
  }
  return index;
}

[[nodiscard]] inline auto descend_path(const template_value &current, const std::string_view token)
    -> wh::core::result<const template_value *> {
  if (const auto *object = current.object_if(); object != nullptr) {
    return find_object_value(*object, token);
  }
  if (const auto *array = current.array_if(); array != nullptr) {
    auto index = parse_index(token);
    if (index.has_error()) {
      return wh::core::result<const template_value *>::failure(index.error());
    }
    if (index.value() >= array->size()) {
      return wh::core::result<const template_value *>::failure(wh::core::errc::not_found);
    }
    return std::addressof((*array)[index.value()]);
  }
  return wh::core::result<const template_value *>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto resolve_placeholder_value(const template_context &context,
                                                    const std::string_view key)
    -> wh::core::result<const template_value *> {
  auto first_token_end = key.find('.');
  auto current = find_object_value(context, key.substr(0U, first_token_end));
  if (current.has_error()) {
    return current;
  }

  while (first_token_end != std::string_view::npos) {
    const auto next_token_start = first_token_end + 1U;
    const auto next_token_end = key.find('.', next_token_start);
    const auto token = key.substr(next_token_start, next_token_end == std::string_view::npos
                                                        ? std::string_view::npos
                                                        : next_token_end - next_token_start);
    current = descend_path(*current.value(), token);
    if (current.has_error()) {
      return current;
    }
    first_token_end = next_token_end;
  }

  return current;
}

[[nodiscard]] inline auto placeholder_render(const std::string_view text,
                                             const template_context &context)
    -> wh::core::result<std::string> {
  const auto delimiters = resolve_delimiters(template_syntax::placeholder);
  std::string rendered;
  rendered.reserve(text.size());
  std::size_t cursor = 0U;
  while (cursor < text.size()) {
    const auto begin = text.find(delimiters.begin, cursor);
    if (begin == std::string_view::npos) {
      rendered.append(text.substr(cursor));
      break;
    }

    rendered.append(text.substr(cursor, begin - cursor));
    const auto end = text.find(delimiters.end, begin + delimiters.begin.size());
    if (end == std::string_view::npos) {
      return wh::core::result<std::string>::failure(wh::core::errc::parse_error);
    }

    const auto key =
        trim(text.substr(begin + delimiters.begin.size(), end - (begin + delimiters.begin.size())));
    auto value = resolve_placeholder_value(context, key);
    if (value.has_error()) {
      return wh::core::result<std::string>::failure(value.error());
    }
    rendered.append(stringify_placeholder_value(*value.value()));
    cursor = end + delimiters.end.size();
  }
  return rendered;
}

[[nodiscard]] inline auto map_minja_runtime_error(const std::string_view message)
    -> wh::core::errc {
  if (message.find("Undefined variable") != std::string_view::npos ||
      message.find("Undefined value or reference") != std::string_view::npos ||
      message.find("is not defined") != std::string_view::npos) {
    return wh::core::errc::not_found;
  }
  if (message.find("not implemented") != std::string_view::npos ||
      message.find("not supported") != std::string_view::npos ||
      message.find("Unknown filter") != std::string_view::npos ||
      message.find("Undefined filter") != std::string_view::npos ||
      message.find("Undefined test") != std::string_view::npos) {
    return wh::core::errc::not_supported;
  }
  if (message.find("Value is not") != std::string_view::npos ||
      message.find("not an object") != std::string_view::npos ||
      message.find("not iterable") != std::string_view::npos ||
      message.find("Cannot subscript") != std::string_view::npos ||
      message.find("Trying to access property") != std::string_view::npos ||
      message.find("Cannot compare values") != std::string_view::npos ||
      message.find("object is not iterable") != std::string_view::npos ||
      message.find("Unhashable type") != std::string_view::npos) {
    return wh::core::errc::type_mismatch;
  }
  return wh::core::errc::internal_error;
}

[[nodiscard]] inline auto jinja_render(const std::string_view text, const template_context &context)
    -> wh::core::result<std::string> {
  std::shared_ptr<minja::TemplateNode> root{};
  try {
    root = minja::Parser::parse(std::string{text}, minja::Options{false, false, false});
  } catch (const std::runtime_error &) {
    return wh::core::result<std::string>::failure(wh::core::errc::parse_error);
  }

  try {
    auto values = template_object_to_json(context);
    auto render_context = minja::Context::make(minja::Value(values));
    return root->render(render_context);
  } catch (const std::runtime_error &error) {
    return wh::core::result<std::string>::failure(map_minja_runtime_error(error.what()));
  }
}

} // namespace detail

/// Renders prompt text template from a structured template context.
[[nodiscard]] inline auto
render_text_template(const std::string_view text, const template_context &context,
                     const template_syntax syntax = template_syntax::placeholder)
    -> wh::core::result<std::string> {
  switch (syntax) {
  case template_syntax::placeholder:
    return detail::placeholder_render(text, context);
  case template_syntax::jinja_compatible:
    return detail::jinja_render(text, context);
  }
  return wh::core::result<std::string>::failure(wh::core::errc::internal_error);
}

} // namespace wh::prompt
