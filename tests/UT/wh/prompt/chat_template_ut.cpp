#include <catch2/catch_test_macros.hpp>

#include <tuple>

#include <stdexec/execution.hpp>

#include "wh/prompt/chat_template.hpp"

namespace {

struct sync_prompt_impl {
  [[nodiscard]] auto render(const wh::prompt::prompt_render_request &,
                            wh::prompt::prompt_callback_event &event) const
      -> wh::core::result<std::vector<wh::schema::message>> {
    event.template_name = "sync";
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"sync"});
    return std::vector<wh::schema::message>{message};
  }
};

struct async_prompt_impl {
  [[nodiscard]] auto render_sender(const wh::prompt::prompt_render_request &) const {
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"async"});
    return stdexec::just(
        wh::core::result<std::vector<wh::schema::message>>{
            std::vector<wh::schema::message>{message}});
  }
};

} // namespace

TEST_CASE("chat template wrapper forwards sync implementations",
          "[UT][wh/prompt/chat_template.hpp][chat_template::render][branch][boundary]") {
  wh::prompt::chat_template wrapped{sync_prompt_impl{}};
  REQUIRE(wrapped.descriptor().kind == wh::core::component_kind::prompt);

  wh::prompt::prompt_render_request request{};
  wh::core::run_context context{};
  auto result = wrapped.render(request, context);
  REQUIRE(result.has_value());
  REQUIRE(result.value().size() == 1U);
  REQUIRE(std::get<wh::schema::text_part>(result.value().front().parts.front()).text ==
          "sync");
}

TEST_CASE("chat template wrapper normalizes async sender outputs",
          "[UT][wh/prompt/chat_template.hpp][chat_template::async_render][branch]") {
  wh::prompt::chat_template wrapped{async_prompt_impl{}};
  wh::prompt::prompt_render_request request{};
  wh::core::run_context context{};

  auto awaited = stdexec::sync_wait(wrapped.async_render(request, context));
  REQUIRE(awaited.has_value());
  REQUIRE(std::get<0>(*awaited).has_value());
  REQUIRE(std::get<wh::schema::text_part>(
              std::get<0>(*awaited).value().front().parts.front())
              .text == "async");
}

TEST_CASE("chat template detail callback state resolves template name and variable count",
          "[UT][wh/prompt/chat_template.hpp][detail::make_callback_state][condition][branch][boundary]") {
  wh::prompt::prompt_render_request request{};
  request.context.insert_or_assign("name", "Ada");
  request.context.insert_or_assign("question", "What?");
  request.options.set_base(
      wh::prompt::prompt_common_options{.template_name = "base-name"});
  request.options.set_call_override(
      wh::prompt::prompt_common_options{.template_name = "override-name"});

  const auto state = wh::prompt::detail::make_callback_state(request);
  REQUIRE(state.run_info.name == "Prompt");
  REQUIRE(state.event.template_name == "override-name");
  REQUIRE(state.event.variable_count == 2U);
}
