#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/agent/middlewares/filesystem/filesystem.hpp"
#include "wh/agent/middlewares/reduction/clear_tool_result.hpp"
#include "wh/agent/middlewares/skill/skill.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/message.hpp"

namespace {

[[nodiscard]] auto make_call_scope(wh::core::run_context &context,
                                   const std::string_view tool_name,
                                   const std::string_view call_id)
    -> wh::tool::call_scope {
  return wh::tool::call_scope{
      .run = context,
      .component = "middleware_test",
      .implementation = "middleware_test_impl",
      .tool_name = tool_name,
      .call_id = call_id,
  };
}

[[nodiscard]] auto read_graph_string(wh::compose::graph_value value) -> std::string {
  auto *typed = wh::core::any_cast<std::string>(&value);
  REQUIRE(typed != nullptr);
  return std::move(*typed);
}

[[nodiscard]] auto make_text_message(const wh::schema::message_role role,
                                     std::string text,
                                     std::string tool_name = {})
    -> wh::schema::message {
  wh::schema::message message{};
  message.role = role;
  message.tool_name = std::move(tool_name);
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

struct scoped_directory {
  std::filesystem::path path{};

  ~scoped_directory() { std::error_code ignored{}; std::filesystem::remove_all(path, ignored); }
};

} // namespace

TEST_CASE("filesystem middleware mounts six tools in stable order and applies normalization plus large-result downshift",
          "[core][adk][middleware][filesystem]") {
  std::string ls_path{};
  std::string glob_path{};
  std::size_t read_offset = 999U;
  std::size_t read_limit = 999U;
  std::string large_result_path{};
  std::string large_result_text{};

  wh::agent::middlewares::filesystem::filesystem_backend backend{
      .ls =
          [&ls_path](const std::string_view path)
              -> wh::core::result<std::vector<wh::agent::middlewares::filesystem::filesystem_ls_entry>> {
        ls_path = std::string{path};
        return std::vector<wh::agent::middlewares::filesystem::filesystem_ls_entry>{
            {.path = "src", .directory = true},
            {.path = "README.md", .directory = false},
        };
      },
      .read =
          [&read_offset, &read_limit](const std::string_view, const std::size_t offset,
                                      const std::size_t limit)
              -> wh::core::result<std::string> {
        read_offset = offset;
        read_limit = limit;
        return std::string(40U, 'x');
      },
      .write =
          [](const std::string_view, const std::string_view)
              -> wh::core::result<void> { return {}; },
      .edit =
          [](const std::string_view, const std::string_view,
             const std::string_view, const bool) -> wh::core::result<void> {
        return {};
      },
      .glob =
          [&glob_path](const std::string_view path, const std::string_view)
              -> wh::core::result<std::vector<std::string>> {
        glob_path = std::string{path};
        return std::vector<std::string>{"a.txt", "b.txt"};
      },
      .grep =
          [](const std::string_view, const std::string_view)
              -> wh::core::result<std::vector<wh::agent::middlewares::filesystem::filesystem_grep_match>> {
        return std::vector<wh::agent::middlewares::filesystem::filesystem_grep_match>{
            {.path = "a.txt", .line = 2U, .text = "alpha"},
            {.path = "a.txt", .line = 5U, .text = "beta"},
            {.path = "b.txt", .line = 1U, .text = "gamma"},
        };
      },
      .write_large_result =
          [&large_result_path, &large_result_text](const std::string_view path,
                                                   const std::string_view text)
              -> wh::core::result<void> {
        large_result_path = std::string{path};
        large_result_text = std::string{text};
        return {};
      },
  };
  wh::agent::middlewares::filesystem::filesystem_tool_options options{};
  options.instruction = "filesystem instruction";
  options.large_result.token_limit = 4U;
  options.large_result.path_prefix = "/tmp/result";

  auto bindings =
      wh::agent::middlewares::filesystem::make_filesystem_tool_bindings(backend, options);
  REQUIRE(bindings.has_value());
  REQUIRE(bindings.value().size() == 6U);
  REQUIRE(bindings.value()[0].schema.name == "ls");
  REQUIRE(bindings.value()[1].schema.name == "read_file");
  REQUIRE(bindings.value()[2].schema.name == "write_file");
  REQUIRE(bindings.value()[3].schema.name == "edit_file");
  REQUIRE(bindings.value()[4].schema.name == "glob");
  REQUIRE(bindings.value()[5].schema.name == "grep");

  wh::core::run_context context{};
  auto ls_result = bindings.value()[0].entry.invoke(
      wh::compose::tool_call{.call_id = "call-ls", .tool_name = "ls", .arguments = "{}"},
      make_call_scope(context, "ls", "call-ls"));
  REQUIRE(ls_result.has_value());
  REQUIRE(ls_path == "/");
  REQUIRE(read_graph_string(std::move(ls_result).value()) == "src/\nREADME.md");

  auto read_result = bindings.value()[1].entry.invoke(
      wh::compose::tool_call{.call_id = "call-read",
                             .tool_name = "read_file",
                             .arguments = R"({"path":"file.txt","offset":-3,"limit":0})"},
      make_call_scope(context, "read_file", "call-read"));
  REQUIRE(read_result.has_value());
  REQUIRE(read_offset == 0U);
  REQUIRE(read_limit == 4096U);
  auto read_text = read_graph_string(std::move(read_result).value());
  REQUIRE(large_result_path == "/tmp/result/call-read");
  REQUIRE(large_result_text == std::string(40U, 'x'));
  REQUIRE(read_text.find("Large result saved to /tmp/result/call-read") !=
          std::string::npos);
  REQUIRE_FALSE(static_cast<bool>(bindings.value()[1].entry.stream));

  auto glob_result = bindings.value()[4].entry.invoke(
      wh::compose::tool_call{.call_id = "call-glob",
                             .tool_name = "glob",
                             .arguments = R"({"pattern":"*.txt"})"},
      make_call_scope(context, "glob", "call-glob"));
  REQUIRE(glob_result.has_value());
  REQUIRE(glob_path == "/");
  REQUIRE(read_graph_string(std::move(glob_result).value()) == "a.txt\nb.txt");

  auto grep_result = bindings.value()[5].entry.invoke(
      wh::compose::tool_call{.call_id = "call-grep",
                             .tool_name = "grep",
                             .arguments = R"({"pattern":"a"})"},
      make_call_scope(context, "grep", "call-grep"));
  REQUIRE(grep_result.has_value());
  REQUIRE(read_graph_string(std::move(grep_result).value()) == "a.txt\nb.txt");
}

TEST_CASE("skill middleware scans local skills, renders dynamic descriptions, and loads documents by name",
          "[core][adk][middleware][skill]") {
  scoped_directory temp{
      .path = std::filesystem::temp_directory_path() /
              "worm_hole_skill_middleware_contracts"};
  std::filesystem::create_directories(temp.path / "brainstorming");
  std::filesystem::create_directories(temp.path / "review");

  {
    std::ofstream out{temp.path / "brainstorming" / "SKILL.md"};
    out << "---\nname: brainstorming\ndescription: Explore design options\n---\nUse this to design before implementation.\n";
  }
  {
    std::ofstream out{temp.path / "review" / "SKILL.md"};
    out << "---\nname: review\ndescription: Review implemented work\n---\nUse this to review a finished change.\n";
  }

  const auto local = wh::agent::middlewares::skill::skill_local_backend{temp.path};
  const auto backend = local.to_backend();

  auto description = wh::agent::middlewares::skill::render_skill_tool_description(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .language = wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(description.has_value());
  REQUIRE(description.value().find("brainstorming") != std::string::npos);
  REQUIRE(description.value().find("Explore design options") != std::string::npos);

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
  REQUIRE(loaded_text.find("Use this to design before implementation.") !=
          std::string::npos);

  auto middleware =
      wh::agent::middlewares::skill::make_skill_request_middleware(
          backend, wh::agent::middlewares::skill::skill_tool_options{
                       .tool_name = "load_skill",
                       .instruction = "Read local skills before acting.",
                       .language =
                           wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(middleware.has_value());

  std::filesystem::create_directories(temp.path / "dynamic");
  {
    std::ofstream out{temp.path / "dynamic" / "SKILL.md"};
    out << "---\nname: dynamic\ndescription: Added after middleware creation\n---\nFresh skill body.\n";
  }

  wh::model::chat_request request{};
  request.messages.push_back(
      make_text_message(wh::schema::message_role::user, "hello"));
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
}

TEST_CASE("clear tool result middleware only trims old tool messages outside the protected window",
          "[core][adk][middleware][reduction]") {
  wh::model::chat_request request{};
  request.messages.push_back(
      make_text_message(wh::schema::message_role::system, "system"));

  wh::schema::message assistant_call{};
  assistant_call.role = wh::schema::message_role::assistant;
  assistant_call.parts.emplace_back(wh::schema::tool_call_part{
      .id = "call-1",
      .name = "search",
      .arguments = std::string(120U, 'a'),
  });
  request.messages.push_back(std::move(assistant_call));
  request.messages.push_back(
      make_text_message(wh::schema::message_role::tool, "old tool payload", "search"));
  request.messages.push_back(
      make_text_message(wh::schema::message_role::tool, "excluded payload", "keep"));
  request.messages.push_back(
      make_text_message(wh::schema::message_role::user, "recent user text"));
  request.messages.push_back(
      make_text_message(wh::schema::message_role::tool, "recent tool payload", "search"));

  auto middleware =
      wh::agent::middlewares::reduction::make_clear_tool_result_middleware(
          wh::agent::middlewares::reduction::clear_tool_result_options{
              .max_history_tokens = 20U,
              .protected_recent_tokens = 8U,
              .placeholder = "[[trimmed]]",
              .excluded_tool_names = {"keep"},
          });

  auto reduced = middleware(request);
  REQUIRE(reduced.has_value());
  REQUIRE(std::get<wh::schema::text_part>(request.messages[2].parts.front()).text ==
          "[[trimmed]]");
  REQUIRE(std::get<wh::schema::text_part>(request.messages[3].parts.front()).text ==
          "excluded payload");
  REQUIRE(std::get<wh::schema::text_part>(request.messages.back().parts.front()).text ==
          "recent tool payload");
}
