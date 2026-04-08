#include <catch2/catch_test_macros.hpp>

#include "wh/model/echo_chat_model.hpp"

TEST_CASE("echo chat model echoes last user text for invoke and stream",
          "[UT][wh/model/echo_chat_model.hpp][echo_chat_model][branch][boundary]") {
  wh::model::echo_chat_model model{};

  wh::model::chat_request request{};
  wh::schema::message user{};
  user.role = wh::schema::message_role::user;
  user.parts.emplace_back(wh::schema::text_part{"hello"});
  request.messages.push_back(user);

  wh::core::run_context context{};
  auto invoked = model.invoke(request, context);
  REQUIRE(invoked.has_value());
  REQUIRE(std::get<wh::schema::text_part>(invoked.value().message.parts.front()).text ==
          "hello");

  auto streamed = model.stream(request, context);
  REQUIRE(streamed.has_value());
  auto collected =
      wh::schema::stream::collect_stream_reader(std::move(streamed).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 1U);
  REQUIRE(std::get<wh::schema::text_part>(collected.value().front().parts.front()).text ==
          "hello");

  auto invalid = model.invoke(wh::model::chat_request{}, context);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
}

TEST_CASE("echo chat model binds tools and reports fixed stream event metadata",
          "[UT][wh/model/echo_chat_model.hpp][echo_chat_model_impl::bind_tools][branch]") {
  wh::model::detail::echo_chat_model_impl impl{};
  std::vector<wh::schema::tool_schema_definition> tools{
      {.name = "search", .description = "lookup"},
  };
  auto bound = impl.bind_tools(tools);
  REQUIRE(bound.bound_tools().size() == 1U);
  REQUIRE(bound.bound_tools().front().name == "search");

  wh::model::chat_model_callback_event event{};
  bound.finish_stream_event(wh::model::chat_request{}, event);
  REQUIRE(event.emitted_chunks == 1U);
  REQUIRE(event.usage.total_tokens == 2);
}

TEST_CASE("echo chat model detail helpers handle non-text parts and invalid empty requests",
          "[UT][wh/model/echo_chat_model.hpp][detail::make_echo_response][condition][branch][boundary]") {
  wh::schema::message non_text{};
  non_text.role = wh::schema::message_role::user;
  non_text.parts.emplace_back(wh::schema::tool_call_part{});
  REQUIRE(wh::model::detail::first_text_part(non_text).empty());

  wh::model::chat_request request{};
  request.messages.push_back(non_text);
  auto echoed = wh::model::detail::make_echo_response(request);
  REQUIRE(echoed.has_value());
  REQUIRE(std::get<wh::schema::text_part>(echoed.value().message.parts.front()).text
          .empty());
  REQUIRE(echoed.value().meta.usage.total_tokens == 2);

  auto invalid = wh::model::detail::make_echo_response(wh::model::chat_request{});
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);
}
