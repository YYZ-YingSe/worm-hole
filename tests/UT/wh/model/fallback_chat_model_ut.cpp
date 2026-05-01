#include <catch2/catch_test_macros.hpp>

#include "wh/model/echo_chat_model.hpp"
#include "wh/model/fallback_chat_model.hpp"
#include "wh/schema/stream/algorithm/collect_stream.hpp"

TEST_CASE("fallback chat model loads bound tools and returns invoke and stream outputs",
          "[UT][wh/model/fallback_chat_model.hpp][fallback_chat_model::invoke][branch][boundary]") {
  wh::model::echo_chat_model model{};
  wh::model::fallback_chat_model wrapped{std::vector<wh::model::echo_chat_model>{model},
                                         std::vector<wh::schema::tool_schema_definition>{
                                             {.name = "search", .description = "lookup"},
                                         }};

  wh::model::chat_request request{};
  wh::schema::message user{};
  user.role = wh::schema::message_role::user;
  user.parts.emplace_back(wh::schema::text_part{"hello"});
  request.messages.push_back(user);

  wh::core::run_context context{};
  auto invoked = wrapped.invoke(request, context);
  REQUIRE(invoked.has_value());
  REQUIRE(std::get<wh::schema::text_part>(invoked.value().message.parts.front()).text == "hello");

  auto streamed = wrapped.stream(request, context);
  REQUIRE(streamed.has_value());
  auto collected = wh::schema::stream::collect_stream_reader(std::move(streamed).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 1U);
}

TEST_CASE("fallback chat model reports force-tool failure when no effective tools remain",
          "[UT][wh/model/fallback_chat_model.hpp][fallback_chat_model::stream][branch]") {
  wh::model::echo_chat_model model{};
  wh::model::fallback_chat_model wrapped{std::vector<wh::model::echo_chat_model>{model}};

  wh::model::chat_request request{};
  request.messages.push_back(wh::schema::message{.role = wh::schema::message_role::user,
                                                 .parts = {wh::schema::text_part{"hello"}}});
  request.options.set_base(wh::model::chat_model_common_options{
      .tool_choice = {.mode = wh::schema::tool_call_mode::force},
  });

  wh::core::run_context context{};
  auto invoked = wrapped.invoke(request, context);
  REQUIRE(invoked.has_error());
  REQUIRE(invoked.error() == wh::core::errc::invalid_argument);
}

TEST_CASE(
    "fallback chat model loads catalog tools and surfaces catalog failures",
    "[UT][wh/model/"
    "fallback_chat_model.hpp][fallback_chat_model::bind_tools][condition][branch][boundary]") {
  wh::model::echo_chat_model model{};

  wh::tool::tool_catalog_cache catalog_success{wh::tool::tool_catalog_source{
      .handshake = []() -> wh::core::result<void> { return {}; },
      .fetch_catalog = []() -> wh::core::result<std::vector<wh::schema::tool_schema_definition>> {
        return std::vector<wh::schema::tool_schema_definition>{
            {.name = "search", .description = "lookup"}};
      }}};

  wh::model::fallback_chat_model wrapped_with_catalog{
      std::vector<wh::model::echo_chat_model>{model}, catalog_success};
  REQUIRE(wrapped_with_catalog.descriptor().type_name == "FallbackChatModel");
  REQUIRE(wrapped_with_catalog.candidates().size() == 1U);

  wh::model::chat_request catalog_request{};
  catalog_request.messages.push_back(wh::schema::message{
      .role = wh::schema::message_role::user, .parts = {wh::schema::text_part{"hello"}}});
  catalog_request.options.set_base(wh::model::chat_model_common_options{
      .tool_choice = {.mode = wh::schema::tool_call_mode::force},
  });

  wh::core::run_context context{};
  auto invoked = wrapped_with_catalog.invoke(catalog_request, context);
  REQUIRE(invoked.has_value());

  wh::tool::tool_catalog_cache catalog_failure{wh::tool::tool_catalog_source{
      .handshake = []() -> wh::core::result<void> {
        return wh::core::result<void>::failure(wh::core::errc::timeout);
      },
      .fetch_catalog = []() -> wh::core::result<std::vector<wh::schema::tool_schema_definition>> {
        return std::vector<wh::schema::tool_schema_definition>{};
      }}};

  wh::model::fallback_chat_model wrapped_failure{std::vector<wh::model::echo_chat_model>{model},
                                                 catalog_failure};
  auto failed = wrapped_failure.invoke(catalog_request, context);
  REQUIRE(failed.has_error());
  REQUIRE(failed.error() == wh::core::errc::timeout);
}
