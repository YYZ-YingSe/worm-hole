// Defines hierarchical address/path value types used to locate nodes,
// interrupts, and resume targets across execution graphs.
#pragma once

#include <algorithm>
#include <concepts>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include "wh/core/compiler.hpp"
#include "wh/core/small_vector.hpp"

namespace wh::core {

/// Immutable hierarchical address used for workflow/component path matching.
class address {
public:
  address() = default;

  /// Builds address from ordered path segments passed as individual values.
  template <typename... segment_ts>
    requires (sizeof...(segment_ts) > 0U &&
              (std::constructible_from<std::string_view, segment_ts &&> && ...))
  explicit address(segment_ts &&...segments) {
    segments_.reserve(sizeof...(segment_ts));
    (append_segment(std::forward<segment_ts>(segments)), ...);
  }

  /// Builds address from one C-string array segment list.
  template <std::size_t count_v>
  explicit address(const char *const (&segments)[count_v]) {
    segments_.reserve(count_v);
    for (const auto segment : segments) {
      append_segment(segment);
    }
  }

  /// Builds address from one string-view array segment list.
  template <std::size_t count_v>
  explicit address(const std::string_view (&segments)[count_v]) {
    append_segments(std::span<const std::string_view>{segments});
  }

  /// Builds address from one pre-materialized segment span.
  [[nodiscard]] static auto from_segments(
      const std::span<const std::string_view> segments) -> address {
    address built{};
    built.append_segments(segments);
    return built;
  }

  /// Returns a new address with one segment appended.
  [[nodiscard]] auto append(std::string_view segment) const -> address {
    address next{*this};
    [[maybe_unused]] auto &stored_segment = next.segments_.emplace_back(segment);
    return next;
  }

  /// Returns parent address by dropping the last segment.
  [[nodiscard]] auto parent() const -> address {
    if (segments_.empty()) {
      return {};
    }

    address parent_path{*this};
    parent_path.segments_.pop_back();
    return parent_path;
  }

  /// Checks whether this address has `prefix` as leading segment sequence.
  [[nodiscard]] auto starts_with(const address &prefix) const noexcept -> bool {
    if (prefix.segments_.size() > segments_.size()) {
      return false;
    }

    return std::ranges::equal(prefix.segments_,
                              segments_ |
                                  std::views::take(prefix.segments_.size()));
  }

  /// Joins segments into string form with configurable separator.
  [[nodiscard]] auto to_string(const std::string_view separator = "/") const
      -> std::string {
    std::string joined;
    if (segments_.empty()) {
      return joined;
    }

    std::size_t total_size = 0U;
    for (const auto &segment : segments_) {
      total_size += segment.size();
    }
    total_size += separator.size() * (segments_.size() - 1U);
    joined.reserve(total_size);

    bool first = true;
    for (const auto &segment : segments_) {
      if (!first) {
        joined += separator;
      }
      joined += segment;
      first = false;
    }
    return joined;
  }

  /// Number of segments in this address.
  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return segments_.size();
  }

  /// Returns true when address has no segments.
  [[nodiscard]] auto empty() const noexcept -> bool {
    return segments_.empty();
  }

  /// Returns read-only segment view.
  [[nodiscard]] auto segments() const noexcept -> std::span<const std::string> {
    return {segments_.data(), segments_.size()};
  }

  /// Equality compares all segments in order.
  [[nodiscard]] friend auto operator==(const address &lhs,
                                       const address &rhs) noexcept -> bool {
    return std::ranges::equal(lhs.segments_, rhs.segments_);
  }

  /// Inequality compares all segments in order.
  [[nodiscard]] friend auto operator!=(const address &lhs,
                                       const address &rhs) noexcept -> bool {
    return !(lhs == rhs);
  }

private:
  auto append_segment(const std::string_view segment) -> void {
    [[maybe_unused]] auto &stored_segment = segments_.emplace_back(segment);
  }

  auto append_segments(const std::span<const std::string_view> segments) -> void {
    segments_.reserve(segments.size());
    for (const auto segment : segments) {
      append_segment(segment);
    }
  }

  /// Inline-first segment storage optimized for shallow paths.
  wh::core::small_vector<std::string, 6U> segments_{};
};

/// Builds one address from one ordered span of path segments.
[[nodiscard]] inline auto
make_address(const std::span<const std::string_view> segments) -> address {
  return address::from_segments(segments);
}

/// Builds one address from one ordered initializer-list of path segments.
[[nodiscard]] inline auto
make_address(const std::initializer_list<std::string_view> segments) -> address {
  return address::from_segments(
      std::span<const std::string_view>{segments.begin(), segments.size()});
}

} // namespace wh::core
