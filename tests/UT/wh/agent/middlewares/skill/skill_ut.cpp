#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <catch2/catch_test_macros.hpp>

#include "helper/agent_authoring_support.hpp"
#include "wh/agent/middlewares/skill/skill.hpp"

namespace {

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

  auto skill_name =
      wh::agent::middlewares::skill::detail::read_skill_name(R"({"name":"brainstorming"})");
  REQUIRE(skill_name.has_value());
  REQUIRE(skill_name.value() == "brainstorming");
  auto missing_name = wh::agent::middlewares::skill::detail::read_skill_name(R"({"other":"x"})");
  REQUIRE(missing_name.has_error());
  auto wrong_name = wh::agent::middlewares::skill::detail::read_skill_name(R"({"name":1})");
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

TEST_CASE("skill middleware lists loads renders mounts and refreshes skill tools",
          "[UT][wh/agent/middlewares/skill/"
          "skill.hpp][make_skill_tool_binding][condition][branch][boundary]") {
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

  const auto backend = local.to_backend();
  auto description = wh::agent::middlewares::skill::render_skill_tool_description(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .language = wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(description.has_value());
  REQUIRE(description.value().find("brainstorming") != std::string::npos);

  wh::agent::middlewares::skill::skill_backend invalid_backend{};
  REQUIRE(
      wh::agent::middlewares::skill::render_skill_tool_description(invalid_backend).has_error());
  REQUIRE(
      wh::agent::middlewares::skill::make_skill_request_middleware(invalid_backend).has_error());
  REQUIRE(wh::agent::middlewares::skill::make_skill_tool_binding(invalid_backend).has_error());
  REQUIRE(wh::agent::middlewares::skill::make_skill_instruction({.instruction = "custom"}) ==
          "custom");
  REQUIRE(wh::agent::middlewares::skill::make_skill_instruction(
              {.language = wh::agent::middlewares::skill::skill_language::english})
              .find("skill tool") != std::string::npos);

  auto binding = wh::agent::middlewares::skill::make_skill_tool_binding(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .tool_name = "load_skill",
                   .language = wh::agent::middlewares::skill::skill_language::chinese});
  REQUIRE(binding.has_value());
  REQUIRE(binding.value().schema.name == "load_skill");
  REQUIRE(binding.value().schema.description.find("可用技能") != std::string::npos);

  wh::core::run_context context{};
  auto loaded = binding.value().entry.invoke(
      wh::compose::tool_call{.call_id = "call-skill",
                             .tool_name = "load_skill",
                             .arguments = R"({"name":"brainstorming"})"},
      make_call_scope(context, "load_skill", "call-skill"));
  REQUIRE(loaded.has_value());
  auto loaded_text = read_graph_string(std::move(loaded).value());
  REQUIRE(loaded_text.find("brainstorming") != std::string::npos);
  REQUIRE(loaded_text.find("SKILL.md") != std::string::npos);

  auto bad_call = binding.value().entry.invoke(wh::compose::tool_call{.call_id = "call-bad",
                                                                      .tool_name = "load_skill",
                                                                      .arguments = R"({"name":1})"},
                                               make_call_scope(context, "load_skill", "call-bad"));
  REQUIRE(bad_call.has_error());
  REQUIRE(bad_call.error() == wh::core::errc::type_mismatch);

  auto middleware = wh::agent::middlewares::skill::make_skill_request_middleware(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .tool_name = "load_skill",
                   .instruction = "Read local skills before acting.",
                   .language = wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(middleware.has_value());

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
  auto mutated = middleware.value()(request);
  REQUIRE(mutated.has_value());
  REQUIRE(request.messages.front().role == wh::schema::message_role::system);
  REQUIRE(std::get<wh::schema::text_part>(request.messages.front().parts.front()).text ==
          "Read local skills before acting.");
  REQUIRE(request.tools.front().description.find("dynamic") != std::string::npos);

  wh::agent::toolset toolset{};
  auto mounted = wh::agent::middlewares::skill::mount_skill_tool(
      toolset, backend,
      wh::agent::middlewares::skill::skill_tool_options{
          .tool_name = "load_skill",
      });
  REQUIRE(mounted.has_value());
  REQUIRE(toolset.size() == 1U);
  auto duplicate = wh::agent::middlewares::skill::mount_skill_tool(
      toolset, backend,
      wh::agent::middlewares::skill::skill_tool_options{
          .tool_name = "load_skill",
      });
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);
}
