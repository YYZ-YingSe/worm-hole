#include <catch2/catch_test_macros.hpp>

#include <array>
#include <sstream>
#include <string>
#include <unordered_set>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"

TEST_CASE("unified error api keeps main path lightweight",
          "[core][error][condition]") {
  const auto ok = wh::core::make_error(wh::core::errc::ok);
  REQUIRE(wh::core::is_ok(ok));
  REQUIRE_FALSE(wh::core::is_error(ok));
  REQUIRE(wh::core::classify(ok) == wh::core::error_kind::success);

  const auto parse = wh::core::make_error(wh::core::errc::parse_error);
  REQUIRE(wh::core::is_error(parse));
  REQUIRE(wh::core::classify(parse) == wh::core::error_kind::parse);

  const auto timeout = wh::core::make_error(wh::core::errc::timeout);
  const auto unavailable = wh::core::make_error(wh::core::errc::unavailable);
  const auto network = wh::core::make_error(wh::core::errc::network_error);
  const auto auth = wh::core::make_error(wh::core::errc::auth_error);

  REQUIRE(wh::core::is_retryable(timeout));
  REQUIRE(wh::core::is_retryable(unavailable));
  REQUIRE(wh::core::is_retryable(network));
  REQUIRE_FALSE(wh::core::is_retryable(auth));
}

TEST_CASE("error code to_string and stream mapping",
          "[core][error][condition]") {
  REQUIRE(wh::core::to_string(wh::core::errc::ok) == "ok");
  REQUIRE(wh::core::to_string(wh::core::errc::timeout) == "timeout");
  REQUIRE(wh::core::to_string(wh::core::errc::channel_closed) ==
          "channel_closed");
  REQUIRE(wh::core::to_string(wh::core::errc::queue_full) == "queue_full");
  REQUIRE(wh::core::to_string(wh::core::errc::scheduler_not_bound) ==
          "scheduler_not_bound");
  REQUIRE(wh::core::to_string(wh::core::errc::parse_error) == "parse_error");
  REQUIRE(wh::core::to_string(wh::core::errc::serialize_error) ==
          "serialize_error");
  REQUIRE(wh::core::to_string(wh::core::errc::type_mismatch) ==
          "type_mismatch");
  REQUIRE(wh::core::to_string(wh::core::errc::already_exists) ==
          "already_exists");
  REQUIRE(wh::core::to_string(wh::core::errc::not_found) == "not_found");
  REQUIRE(wh::core::to_string(wh::core::errc::network_error) ==
          "network_error");
  REQUIRE(wh::core::to_string(wh::core::errc::protocol_error) ==
          "protocol_error");
  REQUIRE(wh::core::to_string(wh::core::errc::auth_error) == "auth_error");
  REQUIRE(wh::core::to_string(wh::core::errc::resource_exhausted) ==
          "resource_exhausted");
  REQUIRE(wh::core::to_string(wh::core::errc::not_supported) ==
          "not_supported");
  REQUIRE(wh::core::to_string(wh::core::errc::retry_exhausted) ==
          "retry_exhausted");
  REQUIRE(wh::core::to_string(wh::core::errc::internal_error) ==
          "internal_error");
  REQUIRE(wh::core::to_string(static_cast<wh::core::errc>(65535)) == "unknown");

  std::ostringstream stream;
  stream << wh::core::errc::channel_closed;
  REQUIRE(stream.str() == "channel_closed");

  std::ostringstream code_stream;
  code_stream << wh::core::make_error(wh::core::errc::timeout);
  REQUIRE(code_stream.str() == "timeout");
}

TEST_CASE("error taxonomy covers master plan categories",
          "[core][error][branch]") {
  REQUIRE(wh::core::classify(wh::core::errc::contract_violation) ==
          wh::core::error_kind::contract);
  REQUIRE(wh::core::classify(wh::core::errc::scheduler_not_bound) ==
          wh::core::error_kind::scheduler);
  REQUIRE(wh::core::classify(wh::core::errc::timeout) ==
          wh::core::error_kind::timeout);
  REQUIRE(wh::core::classify(wh::core::errc::canceled) ==
          wh::core::error_kind::canceled);
  REQUIRE(wh::core::classify(wh::core::errc::network_error) ==
          wh::core::error_kind::network);
  REQUIRE(wh::core::classify(wh::core::errc::parse_error) ==
          wh::core::error_kind::parse);
  REQUIRE(wh::core::classify(wh::core::errc::config_error) ==
          wh::core::error_kind::scheduler);
}

TEST_CASE("error_code equality hash order and unknown fallback",
          "[core][error][condition]") {
  const auto timeout = wh::core::make_error(wh::core::errc::timeout);
  const auto canceled = wh::core::make_error(wh::core::errc::canceled);
  REQUIRE(timeout != canceled);
  REQUIRE(timeout == wh::core::errc::timeout);

  const auto queue_empty = wh::core::make_error(wh::core::errc::queue_empty);
  const auto queue_full = wh::core::make_error(wh::core::errc::queue_full);
  REQUIRE((queue_empty < queue_full || queue_full < queue_empty));

  std::unordered_set<wh::core::error_code> unique_codes;
  unique_codes.insert(timeout);
  unique_codes.insert(wh::core::make_error(wh::core::errc::timeout));
  unique_codes.insert(wh::core::make_error(wh::core::errc::canceled));
  REQUIRE(unique_codes.size() == 2U);

  const auto unknown = wh::core::make_error(static_cast<wh::core::errc>(65535));
  REQUIRE(unknown.message() == "unknown");
  REQUIRE(unknown.kind() == wh::core::error_kind::internal);
}

TEST_CASE("error_code message buffer contract", "[core][error][boundary]") {
  const auto timeout = wh::core::make_error(wh::core::errc::timeout);
  std::array<char, 32> small_buffer{};
  const auto *written =
      timeout.message(small_buffer.data(), small_buffer.size());
  REQUIRE(written == small_buffer.data());
  REQUIRE(std::string_view{small_buffer.data()}.starts_with("timeout"));
}

TEST_CASE("error_info diagnostics are out-of-band", "[core][error][boundary]") {
  const auto info =
      wh::core::make_error_info(wh::core::errc::network_error, "call_provider",
                                "tcp reset", std::source_location::current());
  REQUIRE(info.code == wh::core::errc::network_error);
  REQUIRE(info.operation == "call_provider");
  REQUIRE(info.detail == "tcp reset");
  REQUIRE(info.location.line() > 0U);

  const auto root = wh::core::make_error_info(wh::core::errc::invalid_argument,
                                              "parse_input", "missing field");
  const auto child = wh::core::make_error_info(
      wh::core::errc::timeout, "fetch_model", "provider timed out", {}, &root);
  REQUIRE(child.has_cause());
  REQUIRE(child.cause == &root);
  REQUIRE(child.cause->code == wh::core::errc::invalid_argument);
}

TEST_CASE("error object size budget stays lightweight", "[core][error][size]") {
  REQUIRE(sizeof(wh::core::error_code) <= 8U);
  REQUIRE(sizeof(wh::core::result<int>) <= 24U);
}
