#pragma once

#include <concepts>
#include <cstdint>
#include <exception>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::core {

template <typename source_t>
using cursor_reader_result_t =
    std::remove_cvref_t<decltype(std::declval<source_t &>().read())>;

template <typename source_t>
using cursor_reader_source_try_t =
    std::remove_cvref_t<decltype(std::declval<source_t &>().try_read())>;

} // namespace wh::core

namespace wh::core::cursor_reader_detail {

template <typename try_t, typename result_t> struct try_result_traits;

template <typename result_t> struct try_result_traits<result_t, result_t> {
  [[nodiscard]] static constexpr auto is_pending(const result_t &) noexcept
      -> bool {
    return false;
  }

  [[nodiscard]] static auto project(result_t status) noexcept -> result_t {
    return status;
  }
};

template <typename result_t>
struct try_result_traits<std::optional<result_t>, result_t> {
  [[nodiscard]] static auto
  is_pending(const std::optional<result_t> &status) noexcept -> bool {
    return !status.has_value();
  }

  [[nodiscard]] static auto project(std::optional<result_t> status) noexcept
      -> result_t {
    return std::move(*status);
  }
};

template <typename... option_ts, typename result_t>
struct try_result_traits<std::variant<option_ts...>, result_t> {
  static_assert(
      (std::same_as<option_ts, result_t> + ...) == 1U,
      "cursor try result variant must contain exactly one result alternative");

  [[nodiscard]] static auto
  is_pending(const std::variant<option_ts...> &status) noexcept -> bool {
    return !std::holds_alternative<result_t>(status);
  }

  [[nodiscard]] static auto project(std::variant<option_ts...> status) noexcept
      -> result_t {
    return std::move(std::get<result_t>(status));
  }
};

template <typename try_t, typename result_t>
concept try_result_like = requires(try_t status) {
  {
    try_result_traits<std::remove_cvref_t<try_t>, result_t>::is_pending(status)
  } -> std::same_as<bool>;
  {
    try_result_traits<std::remove_cvref_t<try_t>, result_t>::project(
        std::move(status))
  } -> std::same_as<result_t>;
};

template <typename error_t>
[[nodiscard]] inline auto to_exception_ptr(error_t &&error) noexcept
    -> std::exception_ptr {
  if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                             std::exception_ptr>) {
    return std::forward<error_t>(error);
  } else {
    return std::make_exception_ptr(std::forward<error_t>(error));
  }
}

} // namespace wh::core::cursor_reader_detail

namespace wh::core {

/// Source concept for fixed-cardinality retained fanout readers.
template <typename source_t>
concept cursor_reader_source = requires(source_t &source, const source_t &) {
  typename cursor_reader_result_t<source_t>;
  typename cursor_reader_source_try_t<source_t>;
  requires wh::core::is_result_v<cursor_reader_result_t<source_t>>;
  requires wh::core::cursor_reader_detail::try_result_like<
      cursor_reader_source_try_t<source_t>, cursor_reader_result_t<source_t>>;
  { source.read() } -> std::same_as<cursor_reader_result_t<source_t>>;
  { source.try_read() } -> std::same_as<cursor_reader_source_try_t<source_t>>;
  source.close();
};

} // namespace wh::core

namespace wh::core::cursor_reader_detail {

template <typename source_t>
concept async_source =
    wh::core::cursor_reader_source<source_t> && requires(source_t &source) {
      { source.read_async() } -> stdexec::sender;
    };

template <typename source_t> struct default_policy {
  using result_type = wh::core::cursor_reader_result_t<source_t>;
  using source_try_type = wh::core::cursor_reader_source_try_t<source_t>;
  using try_result_type = std::optional<result_type>;

  [[nodiscard]] static auto is_terminal(const result_type &status) noexcept
      -> bool {
    return status.has_error();
  }

  [[nodiscard]] static auto is_pending(const source_try_type &status) noexcept
      -> bool {
    return try_result_traits<source_try_type, result_type>::is_pending(status);
  }

  [[nodiscard]] static auto project_try(source_try_type status) noexcept
      -> result_type {
    return try_result_traits<source_try_type, result_type>::project(
        std::move(status));
  }

  [[nodiscard]] static auto pending() noexcept -> try_result_type {
    return std::nullopt;
  }

  [[nodiscard]] static auto ready(result_type status) noexcept
      -> try_result_type {
    return try_result_type{std::move(status)};
  }

  [[nodiscard]] static auto closed_result() noexcept -> result_type {
    return result_type::failure(wh::core::errc::channel_closed);
  }

  [[nodiscard]] static auto internal_result() noexcept -> result_type {
    return result_type::failure(wh::core::errc::internal_error);
  }

  static auto set_automatic_close(source_t &, const bool) noexcept -> void {}
};

template <typename source_t, typename policy_t>
concept policy_for =
    wh::core::cursor_reader_source<source_t> &&
    requires(source_t &source,
             const wh::core::cursor_reader_result_t<source_t> &status,
             const wh::core::cursor_reader_source_try_t<source_t> &try_status,
             const bool enabled) {
      typename policy_t::result_type;
      typename policy_t::source_try_type;
      typename policy_t::try_result_type;
      requires std::same_as<typename policy_t::result_type,
                            wh::core::cursor_reader_result_t<source_t>>;
      requires std::same_as<typename policy_t::source_try_type,
                            wh::core::cursor_reader_source_try_t<source_t>>;
      { policy_t::is_terminal(status) } -> std::same_as<bool>;
      { policy_t::is_pending(try_status) } -> std::same_as<bool>;
      {
        policy_t::project_try(
            std::declval<typename policy_t::source_try_type>())
      } -> std::same_as<typename policy_t::result_type>;
      {
        policy_t::pending()
      } -> std::same_as<typename policy_t::try_result_type>;
      {
        policy_t::ready(std::declval<typename policy_t::result_type>())
      } -> std::same_as<typename policy_t::try_result_type>;
      {
        policy_t::closed_result()
      } -> std::same_as<typename policy_t::result_type>;
      {
        policy_t::internal_result()
      } -> std::same_as<typename policy_t::result_type>;
      { policy_t::set_automatic_close(source, enabled) } -> std::same_as<void>;
    };

} // namespace wh::core::cursor_reader_detail
