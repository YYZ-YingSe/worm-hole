#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/agent/middlewares/filesystem/filesystem.hpp"
#include "wh/agent/middlewares/reduction/clear_tool_result.hpp"
#include "wh/agent/middlewares/skill/skill.hpp"
#include "wh/core/any.hpp"
#include "wh/core/error.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/message/types.hpp"

namespace {

template <typename payload_t> [[nodiscard]] auto encode_payload(const payload_t &payload)
    -> std::string {
  auto encoded = wh::agent::encode_tool_payload(payload);
  REQUIRE(encoded.has_value());
  return std::move(encoded).value();
}

[[nodiscard]] auto make_call_scope(wh::core::run_context &context, const std::string_view tool_name,
                                   const std::string_view call_id) -> wh::tool::call_scope {
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

[[nodiscard]] auto make_text_message(const wh::schema::message_role role, std::string text,
                                     std::string tool_name = {}) -> wh::schema::message {
  wh::schema::message message{};
  message.role = role;
  message.tool_name = std::move(tool_name);
  message.parts.emplace_back(wh::schema::text_part{std::move(text)});
  return message;
}

struct scoped_directory {
  std::filesystem::path path{};

  ~scoped_directory() {
    std::error_code ignored{};
    std::filesystem::remove_all(path, ignored);
  }
};

} // namespace

TEST_CASE("filesystem middleware mounts six tools in stable order and applies normalization plus "
          "large-result downshift",
          "[core][adk][middleware][filesystem]") {
  std::string ls_path{};
  std::string glob_path{};
  std::size_t read_offset = 999U;
  std::size_t read_limit = 999U;
  std::string large_result_path{};
  std::string large_result_text{};

  wh::agent::middlewares::filesystem::filesystem_capabilities backend{
      .ls = {.sync = [&ls_path](std::string path)
                 -> wh::core::result<
                     std::vector<wh::agent::middlewares::filesystem::filesystem_ls_entry>> {
        ls_path = std::move(path);
        return std::vector<wh::agent::middlewares::filesystem::filesystem_ls_entry>{
            {.path = "src", .directory = true},
            {.path = "README.md", .directory = false},
        };
      }},
      .read = {.sync = [&read_offset,
                        &read_limit](std::string, const std::size_t offset,
                                     const std::size_t limit) -> wh::core::result<std::string> {
        read_offset = offset;
        read_limit = limit;
        return std::string(40U, 'x');
      }},
      .write = {.sync = [](std::string, std::string) -> wh::core::result<void> { return {}; }},
      .edit = {.sync = [](std::string, std::string, std::string,
                          const bool) -> wh::core::result<void> { return {}; }},
      .glob = {.sync = [&glob_path](std::string path,
                                    std::string) -> wh::core::result<std::vector<std::string>> {
        glob_path = std::move(path);
        return std::vector<std::string>{"a.txt", "b.txt"};
      }},
      .grep = {.sync = [](std::string, std::string)
                   -> wh::core::result<
                       std::vector<wh::agent::middlewares::filesystem::filesystem_grep_match>> {
        return std::vector<wh::agent::middlewares::filesystem::filesystem_grep_match>{
            {.path = "a.txt", .line = 2U, .text = "alpha"},
            {.path = "a.txt", .line = 5U, .text = "beta"},
            {.path = "b.txt", .line = 1U, .text = "gamma"},
        };
      }},
      .write_large_result = {.sync = [&large_result_path, &large_result_text](
                                         std::string path,
                                         std::string text) -> wh::core::result<void> {
        large_result_path = std::move(path);
        large_result_text = std::move(text);
        return {};
      }},
  };
  wh::agent::middlewares::filesystem::filesystem_tool_options options{};
  options.instruction = "filesystem instruction";
  options.large_result.token_limit = 4U;
  options.large_result.path_prefix = "/tmp/result";

  auto surface =
      wh::agent::middlewares::filesystem::make_filesystem_middleware_surface(backend, options);
  REQUIRE(surface.has_value());
  REQUIRE(surface->tool_bindings.size() == 6U);
  REQUIRE(surface->tool_bindings[0].schema.name == "ls");
  REQUIRE(surface->tool_bindings[1].schema.name == "read_file");
  REQUIRE(surface->tool_bindings[2].schema.name == "write_file");
  REQUIRE(surface->tool_bindings[3].schema.name == "edit_file");
  REQUIRE(surface->tool_bindings[4].schema.name == "glob");
  REQUIRE(surface->tool_bindings[5].schema.name == "grep");
  REQUIRE(surface->instruction_fragments == std::vector<std::string>{"filesystem instruction"});
  REQUIRE(surface->request_transforms.empty());

  wh::core::run_context context{};
  auto ls_result = surface->tool_bindings[0].entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-ls",
          .tool_name = "ls",
          .arguments = encode_payload(
              wh::agent::middlewares::filesystem::filesystem_ls_arguments{}),
      },
      make_call_scope(context, "ls", "call-ls"));
  REQUIRE(ls_result.has_value());
  REQUIRE(ls_path == "/");
  REQUIRE(read_graph_string(std::move(ls_result).value()) == "src/\nREADME.md");

  auto read_result = surface->tool_bindings[1].entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-read",
          .tool_name = "read_file",
          .arguments = encode_payload(
              wh::agent::middlewares::filesystem::filesystem_read_arguments{
                  .path = "file.txt",
                  .offset = -3,
                  .limit = 0,
              }),
      },
      make_call_scope(context, "read_file", "call-read"));
  REQUIRE(read_result.has_value());
  REQUIRE(read_offset == 0U);
  REQUIRE(read_limit == 4096U);
  auto read_text = read_graph_string(std::move(read_result).value());
  REQUIRE(large_result_path == "/tmp/result/call-read");
  REQUIRE(large_result_text == std::string(40U, 'x'));
  REQUIRE(read_text.find("Large result saved to /tmp/result/call-read") != std::string::npos);
  REQUIRE_FALSE(static_cast<bool>(surface->tool_bindings[1].entry.stream));

  auto glob_result = surface->tool_bindings[4].entry.invoke(
      wh::compose::tool_call{.call_id = "call-glob",
                             .tool_name = "glob",
                             .arguments = encode_payload(
                                 wh::agent::middlewares::filesystem::filesystem_glob_arguments{
                                     .pattern = "*.txt",
                                 })},
      make_call_scope(context, "glob", "call-glob"));
  REQUIRE(glob_result.has_value());
  REQUIRE(glob_path == "/");
  REQUIRE(read_graph_string(std::move(glob_result).value()) == "a.txt\nb.txt");

  auto grep_result = surface->tool_bindings[5].entry.invoke(
      wh::compose::tool_call{.call_id = "call-grep",
                             .tool_name = "grep",
                             .arguments = encode_payload(
                                 wh::agent::middlewares::filesystem::filesystem_grep_arguments{
                                     .pattern = "a",
                                 })},
      make_call_scope(context, "grep", "call-grep"));
  REQUIRE(grep_result.has_value());
  REQUIRE(read_graph_string(std::move(grep_result).value()) == "a.txt\nb.txt");
}

TEST_CASE("skill middleware scans local skills, renders dynamic descriptions, and loads documents "
          "by name",
          "[core][adk][middleware][skill]") {
  scoped_directory temp{.path = std::filesystem::temp_directory_path() /
                                "worm_hole_skill_middleware_contracts"};
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

  const auto local = wh::agent::middlewares::skill::skill_local_backend{temp.path};
  const auto backend = local.to_capabilities();

  auto description = wh::agent::middlewares::skill::render_skill_tool_description(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .language = wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(description.has_value());
  REQUIRE(description.value().find("brainstorming") != std::string::npos);
  REQUIRE(description.value().find("Explore design options") != std::string::npos);

  auto surface = wh::agent::middlewares::skill::make_skill_middleware_surface(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .tool_name = "load_skill",
                   .language = wh::agent::middlewares::skill::skill_language::chinese});
  REQUIRE(surface.has_value());
  REQUIRE(surface->tool_bindings.size() == 1U);
  REQUIRE(surface->request_transforms.size() == 1U);
  REQUIRE(surface->tool_bindings.front().schema.name == "load_skill");
  REQUIRE(surface->tool_bindings.front().schema.description.find("可用技能") != std::string::npos);

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
  REQUIRE(loaded_text.find("Use this to design before implementation.") != std::string::npos);

  auto request_transform = wh::agent::middlewares::skill::make_skill_request_transform(
      backend, wh::agent::middlewares::skill::skill_tool_options{
                   .tool_name = "load_skill",
                   .instruction = "Read local skills before acting.",
                   .language = wh::agent::middlewares::skill::skill_language::english});
  REQUIRE(request_transform.has_value());
  REQUIRE(static_cast<bool>(request_transform->sync));

  std::filesystem::create_directories(temp.path / "dynamic");
  {
    std::ofstream out{temp.path / "dynamic" / "SKILL.md"};
    out << "---\nname: dynamic\ndescription: Added after middleware creation\n---\nFresh skill "
           "body.\n";
  }

  wh::model::chat_request request{};
  request.messages.push_back(make_text_message(wh::schema::message_role::user, "hello"));
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
}

TEST_CASE("clear tool result surface only trims old tool messages outside the protected window",
          "[core][adk][middleware][reduction]") {
  wh::model::chat_request request{};
  request.messages.push_back(make_text_message(wh::schema::message_role::system, "system"));

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
  request.messages.push_back(make_text_message(wh::schema::message_role::user, "recent user text"));
  request.messages.push_back(
      make_text_message(wh::schema::message_role::tool, "recent tool payload", "search"));

  auto transform = wh::agent::middlewares::reduction::make_clear_tool_result_transform(
      wh::agent::middlewares::reduction::clear_tool_result_options{
          .max_history_tokens = 20U,
          .protected_recent_tokens = 8U,
          .placeholder = "[[trimmed]]",
          .excluded_tool_names = {"keep"},
      });
  wh::core::run_context context{};
  auto reduced = transform.sync(std::move(request), context);
  REQUIRE(reduced.has_value());
  REQUIRE(std::get<wh::schema::text_part>(reduced->messages[2].parts.front()).text ==
          "[[trimmed]]");
  REQUIRE(std::get<wh::schema::text_part>(reduced->messages[3].parts.front()).text ==
          "excluded payload");
  REQUIRE(std::get<wh::schema::text_part>(reduced->messages.back().parts.front()).text ==
          "recent tool payload");
}
