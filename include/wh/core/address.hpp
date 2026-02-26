#pragma once

#include <algorithm>
#include <initializer_list>
#include <ranges>
#include <span>
#include <string>
#include <string_view>

#include "wh/core/small_vector.hpp"

namespace wh::core {

class address {
public:
  address() = default;

  address(std::initializer_list<std::string_view> segments) {
    const auto reserved = segments_.reserve(segments.size());
    if (reserved.has_error()) {
      return;
    }
    for (const auto segment : segments) {
      const auto appended = segments_.emplace_back(segment);
      if (appended.has_error()) {
        return;
      }
    }
  }

  [[nodiscard]] auto append(std::string_view segment) const -> address {
    address next{*this};
    const auto appended = next.segments_.emplace_back(segment);
    static_cast<void>(appended);
    return next;
  }

  [[nodiscard]] auto parent() const -> address {
    if (segments_.empty()) {
      return {};
    }

    address parent_path{*this};
    parent_path.segments_.pop_back();
    return parent_path;
  }

  [[nodiscard]] auto starts_with(const address &prefix) const noexcept -> bool {
    if (prefix.segments_.size() > segments_.size()) {
      return false;
    }

    return std::ranges::equal(prefix.segments_,
                              segments_ |
                                  std::views::take(prefix.segments_.size()));
  }

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

  [[nodiscard]] auto size() const noexcept -> std::size_t {
    return segments_.size();
  }

  [[nodiscard]] auto empty() const noexcept -> bool {
    return segments_.empty();
  }

  [[nodiscard]] auto segments() const noexcept -> std::span<const std::string> {
    return {segments_.data(), segments_.size()};
  }

  [[nodiscard]] friend auto operator==(const address &lhs,
                                       const address &rhs) noexcept -> bool {
    return std::ranges::equal(lhs.segments_, rhs.segments_);
  }

  [[nodiscard]] friend auto operator!=(const address &lhs,
                                       const address &rhs) noexcept -> bool {
    return !(lhs == rhs);
  }

private:
  wh::core::small_vector<std::string, 6U> segments_{};
};

} // namespace wh::core
