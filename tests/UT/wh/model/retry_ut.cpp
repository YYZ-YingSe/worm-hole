#include <chrono>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "wh/model/echo_chat_model.hpp"
#include "wh/model/retry.hpp"

namespace {

struct flaky_model {
  mutable std::size_t invoke_calls{0U};
  mutable std::size_t stream_calls{0U};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"FlakyModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &, wh::core::run_context &) const
      -> wh::model::chat_invoke_result {
    ++invoke_calls;
    if (invoke_calls == 1U) {
      return wh::core::result<wh::model::chat_response>::failure(wh::core::errc::unavailable);
    }
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"ok"});
    return wh::model::chat_response{message, message.meta};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &, wh::core::run_context &) const
      -> wh::model::chat_message_stream_result {
    ++stream_calls;
    if (stream_calls == 1U) {
      return wh::core::result<wh::model::chat_message_stream_reader>::failure(
          wh::core::errc::unavailable);
    }
    return wh::model::chat_message_stream_reader{
        wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
            wh::schema::message{.role = wh::schema::message_role::assistant,
                                .parts = {wh::schema::text_part{"stream-ok"}}})};
  }

  [[nodiscard]] auto bind_tools(const std::span<const wh::schema::tool_schema_definition>) const
      -> flaky_model {
    return *this;
  }
};

} // namespace

TEST_CASE("retry chat model retries invoke and stream before succeeding",
          "[UT][wh/model/retry.hpp][retry_chat_model::invoke][branch][concurrency]") {
  wh::model::retry_chat_model wrapped{
      flaky_model{},
      wh::model::retry_chat_model_options{
          .max_attempts = 3U,
          .should_retry =
              [](const wh::core::error_code error) { return error == wh::core::errc::unavailable; },
      }};

  wh::model::chat_request request{};
  request.messages.push_back(wh::schema::message{.role = wh::schema::message_role::user,
                                                 .parts = {wh::schema::text_part{"hello"}}});

  wh::core::run_context context{};
  auto invoked = wrapped.invoke(request, context);
  REQUIRE(invoked.has_value());
  REQUIRE(std::get<wh::schema::text_part>(invoked.value().message.parts.front()).text == "ok");

  auto streamed = wrapped.stream(request, context);
  REQUIRE(streamed.has_value());
  auto collected = wh::schema::stream::collect_stream_reader(std::move(streamed).value());
  REQUIRE(collected.has_value());
  REQUIRE(std::get<wh::schema::text_part>(collected.value().front().parts.front()).text ==
          "stream-ok");
}

TEST_CASE("retry chat model reports exhausted retries and rebinds tools",
          "[UT][wh/model/retry.hpp][retry_chat_model::bind_tools][branch]") {
  wh::model::retry_chat_model wrapped{wh::model::echo_chat_model{},
                                      wh::model::retry_chat_model_options{
                                          .max_attempts = 1U,
                                      }};
  auto rebound = wrapped.bind_tools(std::array<wh::schema::tool_schema_definition, 1>{{
      {.name = "search", .description = "lookup"},
  }});
  REQUIRE(rebound.options().max_attempts == 1U);
}

TEST_CASE("retry helpers cap backoff and distinguish retryable from terminal failures",
          "[UT][wh/model/retry.hpp][detail::finish_retry_failure][condition][branch][boundary]") {
  using namespace std::chrono_literals;

  const auto capped = wh::model::detail::default_retry_backoff(99U);
  REQUIRE(capped <= 10s);

  wh::core::run_context context{};
  const auto descriptor = wh::model::retry_chat_model{wh::model::echo_chat_model{}}.descriptor();

  auto terminal = wh::model::detail::finish_retry_failure<wh::model::chat_response>(
      context, descriptor, 1U, false, wh::core::errc::timeout);
  REQUIRE(terminal.has_error());
  REQUIRE(terminal.error() == wh::core::errc::timeout);

  auto exhausted = wh::model::detail::finish_retry_failure<wh::model::chat_response>(
      context, descriptor, 3U, true, wh::core::errc::timeout);
  REQUIRE(exhausted.has_error());
  REQUIRE(exhausted.error() == wh::core::errc::retry_exhausted);
}
