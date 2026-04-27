#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/middlewares/skill/skill.hpp"
#include "wh/agent/react.hpp"

namespace {

template <typename payload_t> [[nodiscard]] auto encode_payload(const payload_t &payload)
    -> std::string {
  auto encoded = wh::agent::encode_tool_payload(payload);
  REQUIRE(encoded.has_value());
  return std::move(encoded).value();
}

struct scoped_directory {
  std::filesystem::path path{};

  ~scoped_directory() {
    std::error_code ignored{};
    std::filesystem::remove_all(path, ignored);
  }
};

[[nodiscard]] auto read_graph_string(wh::compose::graph_value value) -> std::string {
  auto *typed = wh::core::any_cast<std::string>(&value);
  REQUIRE(typed != nullptr);
  return std::move(*typed);
}

[[nodiscard]] auto make_call_scope(wh::core::run_context &context, const std::string_view tool_name,
                                   const std::string_view call_id) -> wh::tool::call_scope {
  return wh::tool::call_scope{
      .run = context,
      .component = "skill_ut",
      .implementation = "skill_ut",
      .tool_name = tool_name,
      .call_id = call_id,
  };
}

} // namespace

TEST_CASE("skill detail helpers trim parse render and decode local skill documents",
          "[UT][wh/agent/middlewares/skill/"
          "skill.hpp][detail::parse_skill_document][condition][branch][boundary]") {
  REQUIRE(wh::agent::middlewares::skill::detail::trim_copy("  abc \n") == "abc");
  REQUIRE(wh::agent::middlewares::skill::detail::trim_copy("   ") == "");
  REQUIRE(wh::agent::middlewares::skill::detail::strip_quotes("\"abc\"") == "abc");
  REQUIRE(wh::agent::middlewares::skill::detail::strip_quotes("'abc'") == "abc");
  REQUIRE(wh::agent::middlewares::skill::detail::strip_quotes("abc") == "abc");

  scoped_directory temp{.path =
                            std::filesystem::temp_directory_path() / "worm_hole_skill_detail_ut"};
  std::filesystem::create_directories(temp.path / "good");
  std::filesystem::create_directories(temp.path / "bad");
  std::filesystem::create_directories(temp.path / "missing_fields");

  {
    std::ofstream out{temp.path / "good" / "SKILL.md"};
    out << "---\nname: brainstorming\ndescription: Explore design options\n---\nBody\n";
  }
  {
    std::ofstream out{temp.path / "bad" / "SKILL.md"};
    out << "not yaml";
  }
  {
    std::ofstream out{temp.path / "missing_fields" / "SKILL.md"};
    out << "---\nname: only_name\n---\nBody\n";
  }

  auto parsed = wh::agent::middlewares::skill::detail::parse_skill_document(
      temp.path / "good", temp.path / "good" / "SKILL.md");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().info.name == "brainstorming");
  REQUIRE(parsed.value().info.description == "Explore design options");
  REQUIRE(parsed.value().content == "Body\n");

  auto missing = wh::agent::middlewares::skill::detail::parse_skill_document(
      temp.path / "none", temp.path / "none" / "SKILL.md");
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
  auto bad = wh::agent::middlewares::skill::detail::parse_skill_document(
      temp.path / "bad", temp.path / "bad" / "SKILL.md");
  REQUIRE(bad.has_error());
  REQUIRE(bad.error() == wh::core::errc::parse_error);
  auto missing_fields = wh::agent::middlewares::skill::detail::parse_skill_document(
      temp.path / "missing_fields", temp.path / "missing_fields" / "SKILL.md");
  REQUIRE(missing_fields.has_error());
  REQUIRE(missing_fields.error() == wh::core::errc::parse_error);

  REQUIRE(wh::agent::middlewares::skill::detail::render_skill_list(
              {}, wh::agent::middlewares::skill::skill_language::english) ==
          "Load one local skill guide by name. Available skills:\n(none)");
  REQUIRE(wh::agent::middlewares::skill::detail::render_skill_list(
              {}, wh::agent::middlewares::skill::skill_language::chinese) ==
          "按名称读取一个本地技能指南。可用技能：\n（无）");
  REQUIRE(wh::agent::middlewares::skill::detail::render_skill_list(
              {parsed.value().info}, wh::agent::middlewares::skill::skill_language::english)
              .find("brainstorming") != std::string::npos);
  REQUIRE(wh::agent::middlewares::skill::detail::default_instruction(
              wh::agent::middlewares::skill::skill_language::chinese)
              .find("skill 工具") != std::string::npos);

  auto encoded_name =
      wh::agent::encode_tool_payload(wh::agent::middlewares::skill::skill_load_arguments{
          .name = "brainstorming",
      });
  REQUIRE(encoded_name.has_value());
  auto skill_name = wh::agent::decode_tool_payload<
      wh::agent::middlewares::skill::skill_load_arguments>(encoded_name.value());
  REQUIRE(skill_name.has_value());
  REQUIRE(skill_name->name == "brainstorming");
  auto missing_name = wh::agent::decode_tool_payload<
      wh::agent::middlewares::skill::skill_load_arguments>(R"({"other":"x"})");
  REQUIRE(missing_name.has_error());
  auto wrong_name = wh::agent::decode_tool_payload<
      wh::agent::middlewares::skill::skill_load_arguments>(R"({"name":1})");
  REQUIRE(wrong_name.has_error());
  REQUIRE(wrong_name.error() == wh::core::errc::type_mismatch);

  REQUIRE(wh::agent::middlewares::skill::detail::render_loaded_skill(
              parsed.value(), wh::agent::middlewares::skill::skill_language::english)
              .find("Path:") != std::string::npos);
  REQUIRE(wh::agent::middlewares::skill::detail::render_loaded_skill(
              parsed.value(), wh::agent::middlewares::skill::skill_language::chinese)
              .find("技能：") == 0U);
  auto graph_value = wh::agent::middlewares::skill::detail::graph_string_value("hello");
  REQUIRE(graph_value.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&graph_value.value()) == "hello");
}

TEST_CASE("skill middleware lists loads renders surfaces and refreshes skill tools",
          "[UT][wh/agent/middlewares/skill/"
          "skill.hpp][make_skill_middleware_surface][condition][branch][boundary]") {
  scoped_directory temp{.path =
                            std::filesystem::temp_directory_path() / "worm_hole_skill_public_ut"};
  std::filesystem::create_directories(temp.path / "brainstorming");
  std::filesystem::create_directories(temp.path / "review");
  {
    std::ofstream out{temp.path / "brainstorming" / "SKILL.md"};
    out << "---\nname: brainstorming\ndescription: Explore design options\n---\nUse this to design "
           "before implementation.\n";
  }
  {
    std::ofstream out{temp.path / "review" / "SKILL.md"};
    out << "---\nname: review\ndescription: Review implemented work\n---\nUse this to review a "
           "finished change.\n";
  }

  wh::agent::middlewares::skill::skill_local_backend invalid_relative{"relative"};
  auto invalid_list = invalid_relative.list();
  REQUIRE(invalid_list.has_error());
  REQUIRE(invalid_list.error() == wh::core::errc::invalid_argument);

  wh::agent::middlewares::skill::skill_local_backend local{temp.path};
  auto listed = local.list();
  REQUIRE(listed.has_value());
  REQUIRE(listed.value().size() == 2U);
  REQUIRE(listed.value().front().name == "brainstorming");
  REQUIRE(local.load("brainstorming").has_value());
  auto missing_load = local.load("missing");
  REQUIRE(missing_load.has_error());
  REQUIRE(missing_load.error() == wh::core::errc::not_found);

  const auto backend = local.to_capabilities();
  auto description = wh::agent::middlewares::skill::render_skill_tool_description(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .language = wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(description.has_value());
  REQUIRE(description.value().find("brainstorming") != std::string::npos);

  wh::agent::middlewares::skill::skill_capabilities invalid_backend{};
  REQUIRE(
      wh::agent::middlewares::skill::render_skill_tool_description(invalid_backend).has_error());
  REQUIRE(wh::agent::middlewares::skill::make_skill_request_transform(invalid_backend).has_error());
  REQUIRE(
      wh::agent::middlewares::skill::make_skill_middleware_surface(invalid_backend).has_error());
  REQUIRE(wh::agent::middlewares::skill::make_skill_instruction({.instruction = "custom"}) ==
          "custom");
  REQUIRE(wh::agent::middlewares::skill::make_skill_instruction(
              {.language = wh::agent::middlewares::skill::skill_language::english})
              .find("skill tool") != std::string::npos);

  auto surface = wh::agent::middlewares::skill::make_skill_middleware_surface(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .tool_name = "load_skill",
                   .language = wh::agent::middlewares::skill::skill_language::chinese});
  REQUIRE(surface.has_value());
  REQUIRE(surface->instruction_fragments.size() == 1U);
  REQUIRE(surface->tool_bindings.size() == 1U);
  REQUIRE(surface->request_transforms.size() == 1U);
  REQUIRE(surface->tool_bindings.front().schema.name == "load_skill");
  REQUIRE(surface->tool_bindings.front().schema.description.find("可用技能") != std::string::npos);
  REQUIRE_FALSE(static_cast<bool>(surface->tool_bindings.front().entry.async_invoke));

  wh::core::run_context context{};
  auto loaded = surface->tool_bindings.front().entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-skill",
          .tool_name = "load_skill",
          .arguments = encode_payload(wh::agent::middlewares::skill::skill_load_arguments{
              .name = "brainstorming",
          }),
      },
      make_call_scope(context, "load_skill", "call-skill"));
  REQUIRE(loaded.has_value());
  auto loaded_text = read_graph_string(std::move(loaded).value());
  REQUIRE(loaded_text.find("brainstorming") != std::string::npos);
  REQUIRE(loaded_text.find("SKILL.md") != std::string::npos);

  auto bad_call = surface->tool_bindings.front().entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-bad", .tool_name = "load_skill", .arguments = R"({"name":1})"},
      make_call_scope(context, "load_skill", "call-bad"));
  REQUIRE(bad_call.has_error());
  REQUIRE(bad_call.error() == wh::core::errc::type_mismatch);

  auto request_transform = wh::agent::middlewares::skill::make_skill_request_transform(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .tool_name = "load_skill",
                   .instruction = "Read local skills before acting.",
                   .language = wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(request_transform.has_value());
  REQUIRE(static_cast<bool>(request_transform->sync));
  REQUIRE_FALSE(static_cast<bool>(request_transform->async));

  std::filesystem::create_directories(temp.path / "dynamic");
  {
    std::ofstream out{temp.path / "dynamic" / "SKILL.md"};
    out << "---\nname: dynamic\ndescription: Added after middleware creation\n---\nFresh skill "
           "body.\n";
  }

  wh::model::chat_request request{};
  request.messages.push_back(
      wh::testing::helper::make_text_message(wh::schema::message_role::user, "hello"));
  request.tools.push_back(wh::schema::tool_schema_definition{
      .name = "load_skill",
      .description = "stale",
  });
  auto mutated = request_transform->sync(std::move(request), context);
  REQUIRE(mutated.has_value());
  REQUIRE(mutated->messages.front().role == wh::schema::message_role::system);
  REQUIRE(std::get<wh::schema::text_part>(mutated->messages.front().parts.front()).text ==
          "Read local skills before acting.");
  REQUIRE(mutated->tools.front().description.find("dynamic") != std::string::npos);

  wh::agent::react authored{"react", "assistant"};
  REQUIRE(authored.add_middleware_surface(std::move(surface).value()).has_value());
  REQUIRE(authored.tools().size() == 1U);
  REQUIRE(authored.render_instruction().find("skill 工具") != std::string::npos);

  auto duplicate_surface = wh::agent::middlewares::skill::make_skill_middleware_surface(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .tool_name = "load_skill",
                   .language = wh::agent::middlewares::skill::skill_language::chinese});
  REQUIRE(duplicate_surface.has_value());
  auto duplicate = authored.add_middleware_surface(std::move(duplicate_surface).value());
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);
}

TEST_CASE("skill middleware preserves async-only capabilities without fabricating sync entry",
          "[UT][wh/agent/middlewares/skill/"
          "skill.hpp][async-capabilities][condition][branch][boundary]") {
  wh::agent::middlewares::skill::skill_capabilities backend{
      .list = {.async = []() -> wh::agent::middlewares::operation_sender<
                                 wh::agent::middlewares::skill::skill_list_result> {
        return wh::agent::middlewares::operation_sender<
            wh::agent::middlewares::skill::skill_list_result>{
            stdexec::just(wh::agent::middlewares::skill::skill_list_result{
                std::vector<wh::agent::middlewares::skill::skill_info>{
                    {.name = "async-skill",
                     .description = "Async description",
                     .directory = "/tmp/async-skill"}}})};
      }},
      .load = {.async = [](std::string skill_name)
                   -> wh::agent::middlewares::operation_sender<
                       wh::agent::middlewares::skill::skill_load_result> {
        return wh::agent::middlewares::operation_sender<
            wh::agent::middlewares::skill::skill_load_result>{
            stdexec::just(wh::agent::middlewares::skill::skill_load_result{
                wh::agent::middlewares::skill::loaded_skill{
                    .info = {.name = std::move(skill_name),
                             .description = "Async description",
                             .directory = "/tmp/async-skill"},
                    .content = "Async body\n"}})};
      }},
  };

  auto request_transform = wh::agent::middlewares::skill::make_skill_request_transform(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .tool_name = "load_skill",
                   .instruction = "Use async skills.",
                   .language = wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(request_transform.has_value());
  REQUIRE_FALSE(static_cast<bool>(request_transform->sync));
  REQUIRE(static_cast<bool>(request_transform->async));

  wh::model::chat_request request{};
  request.tools.push_back(wh::schema::tool_schema_definition{
      .name = "load_skill",
      .description = "stale",
  });
  wh::core::run_context context{};
  auto transformed = stdexec::sync_wait(request_transform->async(std::move(request), context));
  REQUIRE(transformed.has_value());
  REQUIRE(std::get<0>(*transformed).has_value());
  auto transformed_request = std::move(std::get<0>(*transformed)).value();
  REQUIRE(transformed_request.tools.front().description.find("async-skill") != std::string::npos);
  REQUIRE(transformed_request.messages.front().role == wh::schema::message_role::system);

  auto surface = wh::agent::middlewares::skill::make_skill_middleware_surface(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .tool_name = "load_skill",
                   .language = wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(surface.has_value());
  REQUIRE_FALSE(static_cast<bool>(surface->tool_bindings.front().entry.invoke));
  REQUIRE(static_cast<bool>(surface->tool_bindings.front().entry.async_invoke));

  auto loaded = stdexec::sync_wait(surface->tool_bindings.front().entry.async_invoke(
      wh::compose::tool_call{
          .call_id = "call-skill",
          .tool_name = "load_skill",
          .arguments = encode_payload(wh::agent::middlewares::skill::skill_load_arguments{
              .name = "async-skill",
          }),
      },
      make_call_scope(context, "load_skill", "call-skill")));
  REQUIRE(loaded.has_value());
  REQUIRE(std::get<0>(*loaded).has_value());
  auto loaded_text = read_graph_string(std::move(std::get<0>(*loaded)).value());
  REQUIRE(loaded_text.find("async-skill") != std::string::npos);
  REQUIRE(loaded_text.find("Async body") != std::string::npos);
}
