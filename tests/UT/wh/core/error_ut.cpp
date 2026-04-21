#include <array>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>

#include <catch2/catch_test_macros.hpp>

#include "wh/core/error.hpp"

namespace {

using wh::core::errc;
using wh::core::error_code;
using wh::core::error_kind;

} // namespace

TEST_CASE("classify maps every canonical error code to a stable category",
          "[UT][wh/core/error.hpp][classify][branch]") {
  constexpr std::array cases{
      std::pair{errc::ok, error_kind::success},
      std::pair{errc::invalid_argument, error_kind::contract},
      std::pair{errc::contract_violation, error_kind::contract},
      std::pair{errc::channel_closed, error_kind::contract},
      std::pair{errc::scheduler_not_bound, error_kind::scheduler},
      std::pair{errc::config_error, error_kind::scheduler},
      std::pair{errc::canceled, error_kind::canceled},
      std::pair{errc::timeout, error_kind::timeout},
      std::pair{errc::unavailable, error_kind::unavailable},
      std::pair{errc::parse_error, error_kind::parse},
      std::pair{errc::serialize_error, error_kind::serialize},
      std::pair{errc::type_mismatch, error_kind::type},
      std::pair{errc::already_exists, error_kind::lookup},
      std::pair{errc::not_found, error_kind::lookup},
      std::pair{errc::network_error, error_kind::network},
      std::pair{errc::protocol_error, error_kind::protocol},
      std::pair{errc::auth_error, error_kind::auth},
      std::pair{errc::queue_empty, error_kind::resource},
      std::pair{errc::queue_full, error_kind::resource},
      std::pair{errc::resource_exhausted, error_kind::resource},
      std::pair{errc::not_supported, error_kind::unsupported},
      std::pair{errc::retry_exhausted, error_kind::internal},
      std::pair{errc::internal_error, error_kind::internal},
  };

  for (const auto &[code, expected] : cases) {
    REQUIRE(wh::core::classify(code) == expected);
  }

  REQUIRE(wh::core::classify(static_cast<errc>(65535)) == error_kind::internal);
}

TEST_CASE("to_string returns symbolic names and unknown fallback",
          "[UT][wh/core/error.hpp][to_string][branch][boundary]") {
  REQUIRE(wh::core::to_string(errc::ok) == "ok");
  REQUIRE(wh::core::to_string(errc::timeout) == "timeout");
  REQUIRE(wh::core::to_string(errc::resource_exhausted) == "resource_exhausted");
  REQUIRE(wh::core::to_string(static_cast<errc>(65535)) == "unknown");
}

TEST_CASE("operator stream formats errc symbolic name", "[UT][wh/core/error.hpp][operator<<]") {
  std::ostringstream stream{};
  stream << errc::protocol_error;
  REQUIRE(stream.str() == "protocol_error");
}

TEST_CASE("detail is_known_errc_value accepts defined values only",
          "[UT][wh/core/error.hpp][detail::is_known_errc_value][branch][boundary]") {
  REQUIRE(wh::core::detail::is_known_errc_value(static_cast<int>(errc::ok)));
  REQUIRE(wh::core::detail::is_known_errc_value(static_cast<int>(errc::internal_error)));
  REQUIRE_FALSE(wh::core::detail::is_known_errc_value(-1));
  REQUIRE_FALSE(wh::core::detail::is_known_errc_value(65535));
}

TEST_CASE("detail write_cstr_buffer handles null empty full and truncated writes",
          "[UT][wh/core/error.hpp][detail::write_cstr_buffer][branch][boundary]") {
  char full[16]{};
  REQUIRE(wh::core::detail::write_cstr_buffer("hello", full, sizeof(full)) == full);
  REQUIRE(std::strcmp(full, "hello") == 0);

  char truncated[4]{};
  REQUIRE(wh::core::detail::write_cstr_buffer("abcdef", truncated, sizeof(truncated)) == truncated);
  REQUIRE(std::strcmp(truncated, "abc") == 0);

  char untouched[4] = {'x', 'x', 'x', '\0'};
  REQUIRE(wh::core::detail::write_cstr_buffer("ignored", nullptr, sizeof(untouched)) == nullptr);
  REQUIRE(wh::core::detail::write_cstr_buffer("ignored", untouched, 0U) == untouched);
  REQUIRE(std::strcmp(untouched, "xxx") == 0);
}

TEST_CASE("error_code exposes raw code numeric value message and comparisons",
          "[UT][wh/core/error.hpp][error_code][branch][boundary]") {
  const error_code ok{};
  const error_code timeout{errc::timeout};
  const error_code timeout_with_location{errc::timeout, std::source_location::current()};
  const error_code missing{errc::not_found};

  REQUIRE(ok.code() == errc::ok);
  REQUIRE(ok.value() == 0);
  REQUIRE(ok.kind() == error_kind::success);
  REQUIRE_FALSE(ok.failed());
  REQUIRE_FALSE(static_cast<bool>(ok));

  REQUIRE(timeout.code() == errc::timeout);
  REQUIRE(timeout.value() == static_cast<int>(errc::timeout));
  REQUIRE(timeout.kind() == error_kind::timeout);
  REQUIRE(timeout.failed());
  REQUIRE(static_cast<bool>(timeout));
  REQUIRE(timeout_with_location == timeout);

  REQUIRE(timeout.message() == "timeout");
  REQUIRE(timeout.to_string() == "timeout");
  REQUIRE(timeout.what() == "timeout");

  char buffer[8]{};
  REQUIRE(timeout.message(buffer, sizeof(buffer)) == buffer);
  REQUIRE(std::strcmp(buffer, "timeout") == 0);

  REQUIRE(timeout == errc::timeout);
  REQUIRE(errc::timeout == timeout);
  REQUIRE(timeout != missing);
  REQUIRE(timeout != errc::not_found);
  REQUIRE(errc::not_found != timeout);
  REQUIRE(timeout < missing);
}

TEST_CASE("make_error helpers and error predicates preserve semantics",
          "[UT][wh/core/error.hpp][make_error_code][branch]") {
  const auto timeout = wh::core::make_error(errc::timeout);
  const auto canceled = wh::core::make_error_code(errc::canceled);
  const auto unavailable =
      wh::core::make_error_code(errc::unavailable, std::source_location::current());
  const auto ok = wh::core::make_error_code(errc::ok);

  REQUIRE(timeout == errc::timeout);
  REQUIRE(wh::core::classify(timeout) == error_kind::timeout);
  REQUIRE(wh::core::is_timeout(timeout));
  REQUIRE(wh::core::is_retryable(timeout));

  REQUIRE(canceled == errc::canceled);
  REQUIRE(wh::core::is_canceled(canceled));
  REQUIRE_FALSE(wh::core::is_retryable(canceled));

  REQUIRE(unavailable == errc::unavailable);
  REQUIRE(wh::core::is_retryable(unavailable));

  REQUIRE(wh::core::is_ok(ok));
  REQUIRE_FALSE(wh::core::is_error(ok));
  REQUIRE_FALSE(wh::core::is_timeout(ok));
  REQUIRE_FALSE(wh::core::is_canceled(ok));
  REQUIRE_FALSE(wh::core::is_retryable(ok));
}

TEST_CASE("make_error_info builds views and chained causes",
          "[UT][wh/core/error.hpp][make_error_info]") {
  const auto cause = wh::core::make_error_info(errc::network_error, "dial", "upstream unavailable");
  const auto info = wh::core::make_error_info(wh::core::make_error(errc::timeout), "fetch",
                                              "deadline", std::source_location::current(), &cause);

  REQUIRE(info.code == errc::timeout);
  REQUIRE(info.operation == "fetch");
  REQUIRE(info.detail == "deadline");
  REQUIRE(info.has_cause());
  REQUIRE(info.cause == &cause);
  REQUIRE(cause.code == errc::network_error);
  REQUIRE(cause.operation == "dial");
  REQUIRE(cause.detail == "upstream unavailable");
  REQUIRE_FALSE(wh::core::make_error_info(errc::ok).has_cause());
}

TEST_CASE("hash_value hash specialization and stream output stay consistent",
          "[UT][wh/core/error.hpp][hash_value]") {
  const error_code timeout{errc::timeout};
  const auto expected = static_cast<std::size_t>(static_cast<int>(errc::timeout));

  REQUIRE(wh::core::hash_value(timeout) == expected);
  REQUIRE(std::hash<error_code>{}(timeout) == expected);

  std::unordered_set<error_code> errors{};
  errors.insert(timeout);
  REQUIRE(errors.contains(timeout));

  std::ostringstream stream{};
  stream << timeout;
  REQUIRE(stream.str() == "timeout");
}

TEST_CASE("error predicates distinguish success timeout canceled and retryable branches",
          "[UT][wh/core/error.hpp][is_retryable][condition][branch]") {
  const error_code ok{};
  const error_code timeout{errc::timeout};
  const error_code unavailable{errc::unavailable};
  const error_code canceled{errc::canceled};
  const error_code invalid{errc::invalid_argument};

  REQUIRE(wh::core::is_ok(ok));
  REQUIRE_FALSE(wh::core::is_error(ok));

  REQUIRE(wh::core::is_error(timeout));
  REQUIRE(wh::core::is_timeout(timeout));
  REQUIRE(wh::core::is_retryable(timeout));

  REQUIRE_FALSE(wh::core::is_timeout(unavailable));
  REQUIRE(wh::core::is_retryable(unavailable));

  REQUIRE(wh::core::is_canceled(canceled));
  REQUIRE_FALSE(wh::core::is_retryable(canceled));

  REQUIRE_FALSE(wh::core::is_retryable(invalid));
}
