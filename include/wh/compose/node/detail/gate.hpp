// Defines compile-time node gate metadata used by compose contract analysis.
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>
#include <type_traits>

#include "wh/core/any.hpp"

namespace wh::compose {

/// Coarse input boundary kinds known to the graph compiler.
enum class input_gate_kind : std::uint8_t {
  value_open,
  value_exact,
  reader,
};

/// Coarse output boundary kinds known to the graph compiler.
enum class output_gate_kind : std::uint8_t {
  value_dynamic,
  value_exact,
  value_passthrough,
  reader,
};

/// Borrowed exact value metadata reused from `wh::core::any`.
struct value_gate {
  const wh::core::any_type_info *info{nullptr};

  template <typename value_t>
  [[nodiscard]] static constexpr auto exact() noexcept -> value_gate {
    return value_gate{
        std::addressof(wh::core::any_info_v<std::remove_cvref_t<value_t>>)};
  }

  [[nodiscard]] constexpr auto empty() const noexcept -> bool {
    return info == nullptr;
  }

  [[nodiscard]] constexpr auto key() const noexcept -> wh::core::any_type_key {
    return info == nullptr ? wh::core::any_type_key{} : info->key;
  }

  [[nodiscard]] constexpr auto name() const noexcept -> std::string_view {
    return info == nullptr ? std::string_view{} : info->name;
  }

  friend constexpr bool operator==(const value_gate &,
                                   const value_gate &) noexcept = default;
};

/// Compile-visible input boundary declared by one node.
struct input_gate {
  input_gate_kind kind{input_gate_kind::value_open};
  value_gate value{};

  [[nodiscard]] static constexpr auto open() noexcept -> input_gate {
    return input_gate{};
  }

  template <typename value_t>
  [[nodiscard]] static constexpr auto exact() noexcept -> input_gate {
    return input_gate{input_gate_kind::value_exact,
                      value_gate::exact<value_t>()};
  }

  [[nodiscard]] static constexpr auto reader() noexcept -> input_gate {
    return input_gate{input_gate_kind::reader, {}};
  }

  friend constexpr bool operator==(const input_gate &,
                                   const input_gate &) noexcept = default;
};

/// Compile-visible output boundary declared by one node.
struct output_gate {
  output_gate_kind kind{output_gate_kind::value_dynamic};
  value_gate value{};

  [[nodiscard]] static constexpr auto dynamic() noexcept -> output_gate {
    return output_gate{};
  }

  template <typename value_t>
  [[nodiscard]] static constexpr auto exact() noexcept -> output_gate {
    return output_gate{output_gate_kind::value_exact,
                       value_gate::exact<value_t>()};
  }

  [[nodiscard]] static constexpr auto passthrough() noexcept -> output_gate {
    return output_gate{output_gate_kind::value_passthrough, {}};
  }

  [[nodiscard]] static constexpr auto reader() noexcept -> output_gate {
    return output_gate{output_gate_kind::reader, {}};
  }

  friend constexpr bool operator==(const output_gate &,
                                   const output_gate &) noexcept = default;
};

[[nodiscard]] constexpr auto gate_name(const input_gate_kind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case input_gate_kind::value_open:
    return "value_open";
  case input_gate_kind::value_exact:
    return "value_exact";
  case input_gate_kind::reader:
    return "reader";
  }
  return "value_open";
}

[[nodiscard]] constexpr auto gate_name(const output_gate_kind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case output_gate_kind::value_dynamic:
    return "value_dynamic";
  case output_gate_kind::value_exact:
    return "value_exact";
  case output_gate_kind::value_passthrough:
    return "value_passthrough";
  case output_gate_kind::reader:
    return "reader";
  }
  return "value_dynamic";
}

} // namespace wh::compose
