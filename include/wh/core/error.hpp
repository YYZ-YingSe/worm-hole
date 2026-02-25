#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <ostream>
#include <source_location>
#include <string>
#include <string_view>

namespace wh::core {

enum class errc : std::uint16_t {
  ok = 0U,
  invalid_argument,
  contract_violation,
  canceled,
  timeout,
  unavailable,
  channel_closed = 6U,
  queue_empty = 7U,
  queue_full,
  scheduler_not_bound,
  config_error,
  parse_error,
  serialize_error,
  type_mismatch,
  already_exists,
  not_found,
  network_error,
  protocol_error,
  auth_error,
  resource_exhausted,
  not_supported,
  retry_exhausted,
  internal_error,
};

enum class error_kind : std::uint8_t {
  success,
  contract,
  scheduler,
  canceled,
  timeout,
  unavailable,
  parse,
  serialize,
  type,
  lookup,
  network,
  protocol,
  auth,
  resource,
  unsupported,
  internal,
};

[[nodiscard]] constexpr auto classify(const errc code) noexcept -> error_kind {
  switch (code) {
  case errc::ok:
    return error_kind::success;
  case errc::invalid_argument:
  case errc::contract_violation:
  case errc::channel_closed:
    return error_kind::contract;
  case errc::scheduler_not_bound:
  case errc::config_error:
    return error_kind::scheduler;
  case errc::canceled:
    return error_kind::canceled;
  case errc::timeout:
    return error_kind::timeout;
  case errc::unavailable:
    return error_kind::unavailable;
  case errc::parse_error:
    return error_kind::parse;
  case errc::serialize_error:
    return error_kind::serialize;
  case errc::type_mismatch:
    return error_kind::type;
  case errc::already_exists:
  case errc::not_found:
    return error_kind::lookup;
  case errc::network_error:
    return error_kind::network;
  case errc::protocol_error:
    return error_kind::protocol;
  case errc::auth_error:
    return error_kind::auth;
  case errc::queue_empty:
  case errc::queue_full:
  case errc::resource_exhausted:
    return error_kind::resource;
  case errc::not_supported:
    return error_kind::unsupported;
  case errc::retry_exhausted:
  case errc::internal_error:
    return error_kind::internal;
  }

  return error_kind::internal;
}

[[nodiscard]] constexpr auto to_string(const errc code) noexcept
    -> std::string_view {
  switch (code) {
  case errc::ok:
    return "ok";
  case errc::invalid_argument:
    return "invalid_argument";
  case errc::contract_violation:
    return "contract_violation";
  case errc::canceled:
    return "canceled";
  case errc::timeout:
    return "timeout";
  case errc::unavailable:
    return "unavailable";
  case errc::channel_closed:
    return "channel_closed";
  case errc::queue_empty:
    return "queue_empty";
  case errc::queue_full:
    return "queue_full";
  case errc::scheduler_not_bound:
    return "scheduler_not_bound";
  case errc::config_error:
    return "config_error";
  case errc::parse_error:
    return "parse_error";
  case errc::serialize_error:
    return "serialize_error";
  case errc::type_mismatch:
    return "type_mismatch";
  case errc::already_exists:
    return "already_exists";
  case errc::not_found:
    return "not_found";
  case errc::network_error:
    return "network_error";
  case errc::protocol_error:
    return "protocol_error";
  case errc::auth_error:
    return "auth_error";
  case errc::resource_exhausted:
    return "resource_exhausted";
  case errc::not_supported:
    return "not_supported";
  case errc::retry_exhausted:
    return "retry_exhausted";
  case errc::internal_error:
    return "internal_error";
  }

  return "unknown";
}

template <typename char_t, typename traits_t>
auto operator<<(std::basic_ostream<char_t, traits_t> &stream, const errc code)
    -> std::basic_ostream<char_t, traits_t> & {
  stream << to_string(code);
  return stream;
}

namespace detail {

[[nodiscard]] constexpr auto is_known_errc_value(const int value) noexcept
    -> bool {
  switch (static_cast<errc>(value)) {
  case errc::ok:
  case errc::invalid_argument:
  case errc::contract_violation:
  case errc::canceled:
  case errc::timeout:
  case errc::unavailable:
  case errc::channel_closed:
  case errc::queue_empty:
  case errc::queue_full:
  case errc::scheduler_not_bound:
  case errc::config_error:
  case errc::parse_error:
  case errc::serialize_error:
  case errc::type_mismatch:
  case errc::already_exists:
  case errc::not_found:
  case errc::network_error:
  case errc::protocol_error:
  case errc::auth_error:
  case errc::resource_exhausted:
  case errc::not_supported:
  case errc::retry_exhausted:
  case errc::internal_error:
    return true;
  }

  return false;
}

inline auto write_cstr_buffer(const std::string_view text, char *const buffer,
                              const std::size_t len) noexcept -> const char * {
  if (buffer == nullptr || len == 0U) {
    return buffer;
  }

  const auto copied = std::min(len - 1U, text.size());
  if (copied > 0U) {
    std::memcpy(buffer, text.data(), copied);
  }
  buffer[copied] = '\0';
  return buffer;
}

} // namespace detail

class error_code {
public:
  constexpr error_code() noexcept = default;
  constexpr error_code(const errc code) noexcept : code_(code) {}
  constexpr error_code(const errc code, std::source_location) noexcept
      : code_(code) {}

  [[nodiscard]] constexpr auto code() const noexcept -> errc { return code_; }

  [[nodiscard]] constexpr auto value() const noexcept -> int {
    return static_cast<int>(static_cast<std::uint16_t>(code_));
  }

  [[nodiscard]] constexpr auto kind() const noexcept -> error_kind {
    return detail::is_known_errc_value(value()) ? classify(code_)
                                                : error_kind::internal;
  }

  [[nodiscard]] constexpr auto failed() const noexcept -> bool {
    return code_ != errc::ok;
  }

  [[nodiscard]] constexpr explicit operator bool() const noexcept {
    return failed();
  }

  [[nodiscard]] auto message() const -> std::string {
    return std::string{wh::core::to_string(code_)};
  }

  [[nodiscard]] auto message(char *const buffer,
                             const std::size_t len) const noexcept -> const
      char * {
    return detail::write_cstr_buffer(wh::core::to_string(code_), buffer, len);
  }

  [[nodiscard]] auto to_string() const -> std::string {
    return std::string{wh::core::to_string(code_)};
  }

  [[nodiscard]] auto what() const -> std::string { return to_string(); }

  [[nodiscard]] friend constexpr auto operator==(const error_code &lhs,
                                                 const error_code &rhs) noexcept
      -> bool {
    return lhs.code_ == rhs.code_;
  }

  [[nodiscard]] friend constexpr auto operator!=(const error_code &lhs,
                                                 const error_code &rhs) noexcept
      -> bool {
    return !(lhs == rhs);
  }

  [[nodiscard]] friend constexpr auto operator<(const error_code &lhs,
                                                const error_code &rhs) noexcept
      -> bool {
    return lhs.value() < rhs.value();
  }

  [[nodiscard]] friend constexpr auto operator==(const error_code &lhs,
                                                 const errc rhs) noexcept
      -> bool {
    return lhs.code_ == rhs;
  }

  [[nodiscard]] friend constexpr auto operator==(const errc lhs,
                                                 const error_code &rhs) noexcept
      -> bool {
    return lhs == rhs.code_;
  }

  [[nodiscard]] friend constexpr auto operator!=(const error_code &lhs,
                                                 const errc rhs) noexcept
      -> bool {
    return !(lhs == rhs);
  }

  [[nodiscard]] friend constexpr auto operator!=(const errc lhs,
                                                 const error_code &rhs) noexcept
      -> bool {
    return !(lhs == rhs);
  }

private:
  errc code_{errc::ok};
};

[[nodiscard]] constexpr auto make_error(const errc value) noexcept
    -> error_code {
  return error_code{value};
}

[[nodiscard]] constexpr auto make_error_code(const errc value) noexcept
    -> error_code {
  return make_error(value);
}

[[nodiscard]] constexpr auto
make_error_code(const errc value, const std::source_location location) noexcept
    -> error_code {
  return error_code{value, location};
}

[[nodiscard]] constexpr auto classify(const error_code code) noexcept
    -> error_kind {
  return code.kind();
}

[[nodiscard]] constexpr auto is_ok(const error_code code) noexcept -> bool {
  return !code.failed();
}

[[nodiscard]] constexpr auto is_error(const error_code code) noexcept -> bool {
  return code.failed();
}

[[nodiscard]] constexpr auto is_timeout(const error_code code) noexcept
    -> bool {
  return code == errc::timeout;
}

[[nodiscard]] constexpr auto is_canceled(const error_code code) noexcept
    -> bool {
  return code == errc::canceled;
}

[[nodiscard]] constexpr auto is_retryable(const error_code code) noexcept
    -> bool {
  const auto kind = classify(code);
  return kind == error_kind::timeout || kind == error_kind::unavailable ||
         kind == error_kind::network || kind == error_kind::resource;
}

struct error_info_view {
  error_code code{};
  std::string_view operation{};
  std::string_view detail{};
  std::source_location location{std::source_location::current()};
  const error_info_view *cause{nullptr};

  [[nodiscard]] auto has_cause() const noexcept -> bool {
    return cause != nullptr;
  }
};

using error_info = error_info_view;

[[nodiscard]] inline auto make_error_info(
    const error_code code, const std::string_view operation = {},
    const std::string_view detail = {},
    const std::source_location location = std::source_location::current(),
    const error_info_view *cause = nullptr) noexcept -> error_info_view {
  return {code, operation, detail, location, cause};
}

[[nodiscard]] inline auto make_error_info(
    const errc code, const std::string_view operation = {},
    const std::string_view detail = {},
    const std::source_location location = std::source_location::current(),
    const error_info_view *cause = nullptr) noexcept -> error_info_view {
  return make_error_info(make_error(code), operation, detail, location, cause);
}

[[nodiscard]] constexpr auto hash_value(const error_code code) noexcept
    -> std::size_t {
  return static_cast<std::size_t>(code.value());
}

template <typename char_t, typename traits_t>
auto operator<<(std::basic_ostream<char_t, traits_t> &stream,
                const error_code code)
    -> std::basic_ostream<char_t, traits_t> & {
  stream << code.to_string();
  return stream;
}

} // namespace wh::core

namespace std {

template <> struct hash<wh::core::error_code> {
  [[nodiscard]] auto operator()(const wh::core::error_code code) const noexcept
      -> std::size_t {
    return wh::core::hash_value(code);
  }
};

} // namespace std
