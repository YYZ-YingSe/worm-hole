#include <catch2/catch_test_macros.hpp>

#include "wh/prompt/simple_chat_template.hpp"

TEST_CASE("simple chat template renders messages and records failures under strict mode",
          "[UT][wh/prompt/simple_chat_template.hpp][simple_chat_template::render][branch][boundary]") {
  wh::prompt::simple_chat_template prompt{};
  prompt.add_template({.role = wh::schema::message_role::system,
                       .text = "Hello {{ name }}",
                       .name = "welcome"});
  prompt.add_template({.role = wh::schema::message_role::user,
                       .text = "Question: {{ question }}",
                       .name = "question"});

  wh::prompt::prompt_render_request ok_request{};
  ok_request.context.insert_or_assign("name", "Ada");
  ok_request.context.insert_or_assign("question", "What is this?");
  ok_request.options.set_base(
      wh::prompt::prompt_common_options{.template_name = "chat"});

  wh::prompt::prompt_callback_event ok_event{};
  auto rendered = prompt.impl().render(ok_request, ok_event);
  REQUIRE(rendered.has_value());
  REQUIRE(rendered.value().size() == 2U);
  REQUIRE(std::get<wh::schema::text_part>(rendered.value()[0].parts.front()).text ==
          "Hello Ada");

  wh::prompt::prompt_render_request missing_request{};
  missing_request.context.insert_or_assign("name", "Ada");
  missing_request.options.set_base(
      wh::prompt::prompt_common_options{.strict_missing_variables = true,
                                        .template_name = "chat"});

  wh::prompt::prompt_callback_event missing_event{};
  auto missing = prompt.impl().render(missing_request, missing_event);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
  REQUIRE(missing_event.rendered_message_count == 1U);
  REQUIRE(missing_event.failed_template == "question");
  REQUIRE(missing_event.failed_variable == "question");
}

TEST_CASE("simple chat template keeps raw text when strict missing variables is disabled",
          "[UT][wh/prompt/simple_chat_template.hpp][simple_chat_template::templates][branch]") {
  wh::prompt::simple_chat_template prompt{
      std::vector<wh::prompt::prompt_message_template>{
          {.role = wh::schema::message_role::user, .text = "{{ name }}"},
      }};

  wh::prompt::prompt_render_request request{};
  request.options.set_base(
      wh::prompt::prompt_common_options{.strict_missing_variables = false,
                                        .template_name = "relaxed"});

  wh::prompt::prompt_callback_event event{};
  auto rendered = prompt.impl().render(request, event);
  REQUIRE(rendered.has_value());
  REQUIRE(prompt.templates().size() == 1U);
  REQUIRE(std::get<wh::schema::text_part>(rendered.value().front().parts.front()).text ==
          "{{ name }}");
}

TEST_CASE("simple chat template falls back to template-name when unnamed entry fails",
          "[UT][wh/prompt/simple_chat_template.hpp][simple_chat_template::add_template][condition][branch][boundary]") {
  wh::prompt::simple_chat_template prompt{};
  prompt.add_template({.role = wh::schema::message_role::user, .text = "{{ missing }}"});

  wh::prompt::prompt_render_request request{};
  request.options.set_base(
      wh::prompt::prompt_common_options{.strict_missing_variables = true,
                                        .template_name = "outer-template"});

  wh::prompt::prompt_callback_event event{};
  auto rendered = prompt.impl().render(request, event);
  REQUIRE(rendered.has_error());
  REQUIRE(rendered.error() == wh::core::errc::not_found);
  REQUIRE(event.failed_template == "outer-template");
  REQUIRE(event.failed_variable == "missing");
}
