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

/// Canonical error codes used across modules.
enum class errc : std::uint16_t {
  /// No error.
  ok = 0U,
  /// One or more input arguments are invalid.
  invalid_argument,
  /// API preconditions or invariants were violated.
  contract_violation,
  /// Operation was canceled by caller or stop signal.
  canceled,
  /// Operation exceeded its timeout or deadline.
  timeout,
  /// Requested resource is temporarily unavailable.
  unavailable,
  /// Channel was closed before operation could complete.
  channel_closed = 6U,
  /// Non-blocking pop observed an empty queue.
  queue_empty = 7U,
  /// Non-blocking push observed a full queue.
  queue_full,
  /// Required scheduler/context is not bound.
  scheduler_not_bound,
  /// Configuration is invalid or incomplete.
  config_error,
  /// Parse or decode failed.
  parse_error,
  /// Encode or serialization failed.
  serialize_error,
  /// Runtime value type does not match expected type.
  type_mismatch,
  /// Target resource already exists.
  already_exists,
  /// Target resource was not found.
  not_found,
  /// Network transport failed.
  network_error,
  /// Protocol framing or semantics are invalid.
  protocol_error,
  /// Authentication or authorization failed.
  auth_error,
  /// Resource quota or capacity is exhausted.
  resource_exhausted,
  /// Capability is not supported by current backend.
  not_supported,
  /// Retry budget has been exhausted.
  retry_exhausted,
  /// Unexpected internal failure.
  internal_error,
};

/// High-level error categories for policy/telemetry decisions.
enum class error_kind : std::uint8_t {
  /// Successful outcome category.
  success,
  /// Contract and argument validation errors.
  contract,
  /// Scheduler and execution-context errors.
  scheduler,
  /// Cancellation-driven termination category.
  canceled,
  /// Timeout and deadline-expired category.
  timeout,
  /// Temporary unavailability category.
  unavailable,
  /// Parse/decode error category.
  parse,
  /// Serialize/encode error category.
  serialize,
  /// Type mismatch/conversion error category.
  type,
  /// Lookup/existence error category.
  lookup,
  /// Network transport error category.
  network,
  /// Protocol violation error category.
  protocol,
  /// Authentication/authorization error category.
  auth,
  /// Resource exhaustion error category.
  resource,
  /// Unsupported capability category.
  unsupported,
  /// Internal unexpected failure category.
  internal,
};

/// Maps a concrete error code to its error category.
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

/// Returns a stable string representation for an error code.
[[nodiscard]] constexpr auto to_string(const errc code) noexcept -> std::string_view {
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

/// Streams `errc` symbolic name into standard output streams.
template <typename char_t, typename traits_t>
auto operator<<(std::basic_ostream<char_t, traits_t> &stream, const errc code)
    -> std::basic_ostream<char_t, traits_t> & {
  stream << to_string(code);
  return stream;
}

namespace detail {

/// Validates whether an integer maps to a defined `errc` value.
[[nodiscard]] constexpr auto is_known_errc_value(const int value) noexcept -> bool {
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

/// Writes a string view into a user-provided C buffer (NUL-terminated).
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

/// Lightweight value type wrapping `errc` with helper utilities.
class error_code {
public:
  constexpr error_code() noexcept = default;
  constexpr error_code(const errc code) noexcept : code_(code) {}
  constexpr error_code(const errc code, std::source_location) noexcept : code_(code) {}

  /// Returns the raw `errc` value.
  [[nodiscard]] constexpr auto code() const noexcept -> errc { return code_; }

  /// Returns the numeric code value.
  [[nodiscard]] constexpr auto value() const noexcept -> int {
    return static_cast<int>(static_cast<std::uint16_t>(code_));
  }

  /// Returns the category for this error code.
  [[nodiscard]] constexpr auto kind() const noexcept -> error_kind {
    return detail::is_known_errc_value(value()) ? classify(code_) : error_kind::internal;
  }

  /// Returns true when the code is not `errc::ok`.
  [[nodiscard]] constexpr auto failed() const noexcept -> bool { return code_ != errc::ok; }

  [[nodiscard]] constexpr explicit operator bool() const noexcept { return failed(); }

  /// Materializes the message into a `std::string`.
  [[nodiscard]] auto message() const -> std::string {
    return std::string{wh::core::to_string(code_)};
  }

  /// Writes the message into an external C-style buffer.
  [[nodiscard]] auto message(char *const buffer, const std::size_t len) const noexcept -> const
      char * {
    return detail::write_cstr_buffer(wh::core::to_string(code_), buffer, len);
  }

  /// Returns the message string.
  [[nodiscard]] auto to_string() const -> std::string {
    return std::string{wh::core::to_string(code_)};
  }

  /// Compatibility helper aligned with exception-like naming.
  [[nodiscard]] auto what() const -> std::string { return to_string(); }

  [[nodiscard]] friend constexpr auto operator==(const error_code &lhs,
                                                 const error_code &rhs) noexcept -> bool {
    return lhs.code_ == rhs.code_;
  }

  [[nodiscard]] friend constexpr auto operator!=(const error_code &lhs,
                                                 const error_code &rhs) noexcept -> bool {
    return !(lhs == rhs);
  }

  [[nodiscard]] friend constexpr auto operator<(const error_code &lhs,
                                                const error_code &rhs) noexcept -> bool {
    return lhs.value() < rhs.value();
  }

  [[nodiscard]] friend constexpr auto operator==(const error_code &lhs, const errc rhs) noexcept
      -> bool {
    return lhs.code_ == rhs;
  }

  [[nodiscard]] friend constexpr auto operator==(const errc lhs, const error_code &rhs) noexcept
      -> bool {
    return lhs == rhs.code_;
  }

  [[nodiscard]] friend constexpr auto operator!=(const error_code &lhs, const errc rhs) noexcept
      -> bool {
    return !(lhs == rhs);
  }

  [[nodiscard]] friend constexpr auto operator!=(const errc lhs, const error_code &rhs) noexcept
      -> bool {
    return !(lhs == rhs);
  }

private:
  /// Canonical error enum value carried by this code object.
  errc code_{errc::ok};
};

/// Creates an `error_code` from `errc`.
[[nodiscard]] constexpr auto make_error(const errc value) noexcept -> error_code {
  return error_code{value};
}

/// Alias of `make_error`.
[[nodiscard]] constexpr auto make_error_code(const errc value) noexcept -> error_code {
  return make_error(value);
}

/// Creates an `error_code` with source-location context.
[[nodiscard]] constexpr auto make_error_code(const errc value,
                                             const std::source_location location) noexcept
    -> error_code {
  return error_code{value, location};
}

/// Categorizes an existing `error_code`.
[[nodiscard]] constexpr auto classify(const error_code code) noexcept -> error_kind {
  return code.kind();
}

/// Returns true for success.
[[nodiscard]] constexpr auto is_ok(const error_code code) noexcept -> bool {
  return !code.failed();
}

/// Returns true for failure.
[[nodiscard]] constexpr auto is_error(const error_code code) noexcept -> bool {
  return code.failed();
}

/// Returns true when code indicates timeout.
[[nodiscard]] constexpr auto is_timeout(const error_code code) noexcept -> bool {
  return code == errc::timeout;
}

/// Returns true when code indicates cancellation.
[[nodiscard]] constexpr auto is_canceled(const error_code code) noexcept -> bool {
  return code == errc::canceled;
}

/// Returns true when retry is typically meaningful.
[[nodiscard]] constexpr auto is_retryable(const error_code code) noexcept -> bool {
  const auto kind = classify(code);
  return kind == error_kind::timeout || kind == error_kind::unavailable ||
         kind == error_kind::network || kind == error_kind::resource;
}

/// Non-owning diagnostic payload for richer error reporting.
struct error_info_view {
  /// Primary error code.
  error_code code{};
  /// Logical operation name (for example `"parse_message"`).
  std::string_view operation{};
  /// Human-readable detail message.
  std::string_view detail{};
  /// Source location where the error info was created.
  std::source_location location{std::source_location::current()};
  /// Optional chained cause for hierarchical diagnostics.
  const error_info_view *cause{nullptr};

  /// Returns true if there is a chained cause.
  [[nodiscard]] auto has_cause() const noexcept -> bool { return cause != nullptr; }
};

using error_info = error_info_view;

/// Builds an `error_info_view` from `error_code`.
[[nodiscard]] inline auto
make_error_info(const error_code code, const std::string_view operation = {},
                const std::string_view detail = {},
                const std::source_location location = std::source_location::current(),
                const error_info_view *cause = nullptr) noexcept -> error_info_view {
  return {code, operation, detail, location, cause};
}

/// Builds an `error_info_view` from `errc`.
[[nodiscard]] inline auto
make_error_info(const errc code, const std::string_view operation = {},
                const std::string_view detail = {},
                const std::source_location location = std::source_location::current(),
                const error_info_view *cause = nullptr) noexcept -> error_info_view {
  return make_error_info(make_error(code), operation, detail, location, cause);
}

/// Hash helper for associative containers.
[[nodiscard]] constexpr auto hash_value(const error_code code) noexcept -> std::size_t {
  return static_cast<std::size_t>(code.value());
}

/// Streams `error_code` symbolic name into standard output streams.
template <typename char_t, typename traits_t>
auto operator<<(std::basic_ostream<char_t, traits_t> &stream, const error_code code)
    -> std::basic_ostream<char_t, traits_t> & {
  stream << code.to_string();
  return stream;
}

} // namespace wh::core

namespace std {

template <> struct hash<wh::core::error_code> {
  /// Hashes `error_code` by numeric value.
  [[nodiscard]] auto operator()(const wh::core::error_code code) const noexcept -> std::size_t {
    return wh::core::hash_value(code);
  }
};

} // namespace std
