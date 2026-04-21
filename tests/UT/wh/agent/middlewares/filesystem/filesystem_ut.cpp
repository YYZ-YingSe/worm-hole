#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "wh/agent/middlewares/filesystem/filesystem.hpp"

namespace {

[[nodiscard]] auto read_graph_string(wh::compose::graph_value value) -> std::string {
  auto *typed = wh::core::any_cast<std::string>(&value);
  REQUIRE(typed != nullptr);
  return std::move(*typed);
}

[[nodiscard]] auto make_call_scope(wh::core::run_context &context, const std::string_view tool_name,
                                   const std::string_view call_id) -> wh::tool::call_scope {
  return wh::tool::call_scope{
      .run = context,
      .component = "filesystem_ut",
      .implementation = "filesystem_ut",
      .tool_name = tool_name,
      .call_id = call_id,
  };
}

} // namespace

TEST_CASE("filesystem detail helpers parse normalize format and downshift text results",
          "[UT][wh/agent/middlewares/filesystem/"
          "filesystem.hpp][detail::parse_json_object][condition][branch][boundary]") {
  auto parsed = wh::agent::middlewares::filesystem::detail::parse_json_object(
      R"({"path":"src","flag":true,"offset":-1})");
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().IsObject());

  auto invalid_json = wh::agent::middlewares::filesystem::detail::parse_json_object("{");
  REQUIRE(invalid_json.has_error());
  auto wrong_json = wh::agent::middlewares::filesystem::detail::parse_json_object(R"(["x"])");
  REQUIRE(wrong_json.has_error());
  REQUIRE(wrong_json.error() == wh::core::errc::type_mismatch);

  auto optional_path =
      wh::agent::middlewares::filesystem::detail::optional_string_member(parsed.value(), "path");
  REQUIRE(optional_path.has_value());
  REQUIRE(optional_path.value() == std::optional<std::string>{"src"});
  auto missing_path =
      wh::agent::middlewares::filesystem::detail::optional_string_member(parsed.value(), "missing");
  REQUIRE(missing_path.has_value());
  REQUIRE_FALSE(missing_path.value().has_value());
  auto wrong_optional =
      wh::agent::middlewares::filesystem::detail::optional_string_member(parsed.value(), "flag");
  REQUIRE(wrong_optional.has_error());
  REQUIRE(wrong_optional.error() == wh::core::errc::type_mismatch);

  auto required_path =
      wh::agent::middlewares::filesystem::detail::required_string_member(parsed.value(), "path");
  REQUIRE(required_path.has_value());
  REQUIRE(required_path.value() == "src");
  auto missing_required =
      wh::agent::middlewares::filesystem::detail::required_string_member(parsed.value(), "missing");
  REQUIRE(missing_required.has_error());

  auto offset =
      wh::agent::middlewares::filesystem::detail::optional_signed_member(parsed.value(), "offset");
  REQUIRE(offset.has_value());
  REQUIRE(offset.value() == std::optional<std::int64_t>{-1});
  auto no_offset =
      wh::agent::middlewares::filesystem::detail::optional_signed_member(parsed.value(), "none");
  REQUIRE(no_offset.has_value());
  REQUIRE_FALSE(no_offset.value().has_value());
  auto wrong_offset =
      wh::agent::middlewares::filesystem::detail::optional_signed_member(parsed.value(), "path");
  REQUIRE(wrong_offset.has_error());
  REQUIRE(wrong_offset.error() == wh::core::errc::type_mismatch);

  auto flag =
      wh::agent::middlewares::filesystem::detail::optional_bool_member(parsed.value(), "flag");
  REQUIRE(flag.has_value());
  REQUIRE(flag.value() == std::optional<bool>{true});
  auto no_flag =
      wh::agent::middlewares::filesystem::detail::optional_bool_member(parsed.value(), "none");
  REQUIRE(no_flag.has_value());
  REQUIRE_FALSE(no_flag.value().has_value());
  auto wrong_flag =
      wh::agent::middlewares::filesystem::detail::optional_bool_member(parsed.value(), "path");
  REQUIRE(wrong_flag.has_error());
  REQUIRE(wrong_flag.error() == wh::core::errc::type_mismatch);

  REQUIRE(wh::agent::middlewares::filesystem::detail::normalize_directory_path(std::nullopt) ==
          "/");
  REQUIRE(wh::agent::middlewares::filesystem::detail::normalize_directory_path(
              std::optional<std::string>{""}) == "/");
  REQUIRE(wh::agent::middlewares::filesystem::detail::normalize_directory_path(
              std::optional<std::string>{"src"}) == "src");
  REQUIRE(wh::agent::middlewares::filesystem::detail::normalize_offset(std::nullopt) == 0U);
  REQUIRE(wh::agent::middlewares::filesystem::detail::normalize_offset(-3) == 0U);
  REQUIRE(wh::agent::middlewares::filesystem::detail::normalize_offset(5) == 5U);
  REQUIRE(wh::agent::middlewares::filesystem::detail::normalize_limit(std::nullopt) == 4096U);
  REQUIRE(wh::agent::middlewares::filesystem::detail::normalize_limit(0) == 4096U);
  REQUIRE(wh::agent::middlewares::filesystem::detail::normalize_limit(12) == 12U);

  REQUIRE(wh::agent::middlewares::filesystem::detail::parse_grep_mode(std::nullopt) ==
          wh::agent::middlewares::filesystem::filesystem_grep_mode::files_with_matches);
  REQUIRE(wh::agent::middlewares::filesystem::detail::parse_grep_mode("content") ==
          wh::agent::middlewares::filesystem::filesystem_grep_mode::content);
  REQUIRE(wh::agent::middlewares::filesystem::detail::parse_grep_mode("count") ==
          wh::agent::middlewares::filesystem::filesystem_grep_mode::count);
  REQUIRE(wh::agent::middlewares::filesystem::detail::parse_grep_mode("weird") ==
          wh::agent::middlewares::filesystem::filesystem_grep_mode::files_with_matches);

  REQUIRE(wh::agent::middlewares::filesystem::detail::join_lines(
              std::vector<std::string>{"a", "b"}) == "a\nb");
  REQUIRE(wh::agent::middlewares::filesystem::detail::format_ls_entries(
              std::vector<wh::agent::middlewares::filesystem::filesystem_ls_entry>{
                  {.path = "src", .directory = true},
                  {.path = "README.md", .directory = false},
              }) == "src/\nREADME.md");
  REQUIRE(wh::agent::middlewares::filesystem::detail::format_glob_entries(
              std::vector<std::string>{"a.txt", "b.txt"}) == "a.txt\nb.txt");
  const std::vector<wh::agent::middlewares::filesystem::filesystem_grep_match> grep_entries{
      {.path = "a.txt", .line = 2U, .text = "alpha"},
      {.path = "a.txt", .line = 5U, .text = "beta"},
      {.path = "b.txt", .line = 1U, .text = "gamma"},
  };
  REQUIRE(wh::agent::middlewares::filesystem::detail::format_grep_entries(
              grep_entries,
              wh::agent::middlewares::filesystem::filesystem_grep_mode::files_with_matches) ==
          "a.txt\nb.txt");
  REQUIRE(wh::agent::middlewares::filesystem::detail::format_grep_entries(
              grep_entries, wh::agent::middlewares::filesystem::filesystem_grep_mode::content) ==
          "a.txt:2:alpha\na.txt:5:beta\nb.txt:1:gamma");
  REQUIRE(wh::agent::middlewares::filesystem::detail::format_grep_entries(
              grep_entries, wh::agent::middlewares::filesystem::filesystem_grep_mode::count) ==
          "3");

  wh::agent::middlewares::filesystem::filesystem_large_result_options large{};
  large.path_prefix = "/tmp/result";
  REQUIRE(wh::agent::middlewares::filesystem::detail::large_result_path(large, "call-1") ==
          "/tmp/result/call-1");
  REQUIRE(wh::agent::middlewares::filesystem::detail::preview_text("line1\nline2\nline3") ==
          "line1\nline2\nline3");

  wh::agent::middlewares::filesystem::filesystem_backend backend{};
  backend.write_large_result = [](const std::string_view,
                                  const std::string_view) -> wh::core::result<void> { return {}; };
  wh::agent::middlewares::filesystem::filesystem_tool_options options{};
  options.large_result.token_limit = 1U;
  options.large_result.path_prefix = "/tmp/result";
  auto direct = wh::agent::middlewares::filesystem::detail::materialize_text_result(
      "abc", "call-1", backend, options);
  REQUIRE(direct.has_value());
  REQUIRE(direct.value() == "abc");

  auto shifted = wh::agent::middlewares::filesystem::detail::materialize_text_result(
      "abcdefghij", "call-2", backend, options);
  REQUIRE(shifted.has_value());
  REQUIRE(shifted.value().find("Large result saved to /tmp/result/call-2") != std::string::npos);

  wh::agent::middlewares::filesystem::filesystem_backend missing_large_backend{};
  auto missing_large = wh::agent::middlewares::filesystem::detail::materialize_text_result(
      "abcdefghij", "call-3", missing_large_backend, options);
  REQUIRE(missing_large.has_error());
  REQUIRE(missing_large.error() == wh::core::errc::invalid_argument);

  wh::agent::middlewares::filesystem::filesystem_backend failing_large_backend{};
  failing_large_backend.write_large_result = [](const std::string_view,
                                                const std::string_view) -> wh::core::result<void> {
    return wh::core::result<void>::failure(wh::core::errc::unavailable);
  };
  auto failing_large = wh::agent::middlewares::filesystem::detail::materialize_text_result(
      "abcdefghij", "call-4", failing_large_backend, options);
  REQUIRE(failing_large.has_error());
  REQUIRE(failing_large.error() == wh::core::errc::unavailable);

  auto value = wh::agent::middlewares::filesystem::detail::graph_string_value("ok");
  REQUIRE(value.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&value.value()) == "ok");
  REQUIRE(wh::agent::middlewares::filesystem::detail::make_string_parameter("path", "desc", false)
              .required == false);
  REQUIRE(wh::agent::middlewares::filesystem::detail::make_integer_parameter("limit", "desc", true)
              .required);
  REQUIRE(
      wh::agent::middlewares::filesystem::detail::make_bool_parameter("replace_all", "desc", false)
          .type == wh::schema::tool_parameter_type::boolean);
}

TEST_CASE("filesystem middleware builds six tools in stable order and mounts them into toolset",
          "[UT][wh/agent/middlewares/filesystem/"
          "filesystem.hpp][make_filesystem_tool_bindings][condition][branch][boundary]") {
  wh::agent::middlewares::filesystem::filesystem_backend invalid_backend{};
  auto invalid = wh::agent::middlewares::filesystem::make_filesystem_tool_bindings(invalid_backend);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  std::string ls_path{};
  std::string glob_path{};
  std::size_t read_offset = 999U;
  std::size_t read_limit = 999U;
  std::string large_result_path{};
  std::string large_result_text{};
  bool replace_all_seen = false;

  wh::agent::middlewares::filesystem::filesystem_backend backend{
      .ls = [&ls_path](const std::string_view path)
          -> wh::core::result<
              std::vector<wh::agent::middlewares::filesystem::filesystem_ls_entry>> {
        ls_path = std::string{path};
        return std::vector<wh::agent::middlewares::filesystem::filesystem_ls_entry>{
            {.path = "src", .directory = true},
            {.path = "README.md", .directory = false},
        };
      },
      .read = [&read_offset,
               &read_limit](const std::string_view, const std::size_t offset,
                            const std::size_t limit) -> wh::core::result<std::string> {
        read_offset = offset;
        read_limit = limit;
        return std::string(40U, 'x');
      },
      .write = [](const std::string_view, const std::string_view) -> wh::core::result<void> {
        return {};
      },
      .edit = [&replace_all_seen](const std::string_view, const std::string_view,
                                  const std::string_view,
                                  const bool replace_all) -> wh::core::result<void> {
        replace_all_seen = replace_all;
        return {};
      },
      .glob = [&glob_path](const std::string_view path,
                           const std::string_view) -> wh::core::result<std::vector<std::string>> {
        glob_path = std::string{path};
        return std::vector<std::string>{"a.txt", "b.txt"};
      },
      .grep = [](const std::string_view, const std::string_view)
          -> wh::core::result<
              std::vector<wh::agent::middlewares::filesystem::filesystem_grep_match>> {
        return std::vector<wh::agent::middlewares::filesystem::filesystem_grep_match>{
            {.path = "a.txt", .line = 2U, .text = "alpha"},
            {.path = "a.txt", .line = 5U, .text = "beta"},
            {.path = "b.txt", .line = 1U, .text = "gamma"},
        };
      },
      .write_large_result = [&large_result_path, &large_result_text](
                                const std::string_view path,
                                const std::string_view text) -> wh::core::result<void> {
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
  REQUIRE(wh::agent::middlewares::filesystem::make_filesystem_instruction(options) ==
          "filesystem instruction");

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
  REQUIRE(read_text.find("Large result saved to /tmp/result/call-read") != std::string::npos);
  REQUIRE_FALSE(static_cast<bool>(bindings.value()[1].entry.stream));

  auto write_result = bindings.value()[2].entry.invoke(
      wh::compose::tool_call{.call_id = "call-write",
                             .tool_name = "write_file",
                             .arguments = R"({"path":"file.txt","content":"body"})"},
      make_call_scope(context, "write_file", "call-write"));
  REQUIRE(write_result.has_value());
  REQUIRE(read_graph_string(std::move(write_result).value()) == "written");

  auto edit_result = bindings.value()[3].entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-edit",
          .tool_name = "edit_file",
          .arguments = R"({"path":"file.txt","search":"a","replace":"b","replace_all":true})"},
      make_call_scope(context, "edit_file", "call-edit"));
  REQUIRE(edit_result.has_value());
  REQUIRE(replace_all_seen);
  REQUIRE(read_graph_string(std::move(edit_result).value()) == "edited");

  auto glob_result = bindings.value()[4].entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-glob", .tool_name = "glob", .arguments = R"({"pattern":"*.txt"})"},
      make_call_scope(context, "glob", "call-glob"));
  REQUIRE(glob_result.has_value());
  REQUIRE(glob_path == "/");
  REQUIRE(read_graph_string(std::move(glob_result).value()) == "a.txt\nb.txt");

  auto grep_files = bindings.value()[5].entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-grep-files", .tool_name = "grep", .arguments = R"({"pattern":"a"})"},
      make_call_scope(context, "grep", "call-grep-files"));
  REQUIRE(grep_files.has_value());
  REQUIRE(read_graph_string(std::move(grep_files).value()) == "a.txt\nb.txt");
  auto grep_content = bindings.value()[5].entry.invoke(
      wh::compose::tool_call{.call_id = "call-grep-content",
                             .tool_name = "grep",
                             .arguments = R"({"pattern":"a","mode":"content"})"},
      make_call_scope(context, "grep", "call-grep-content"));
  REQUIRE(grep_content.has_value());
  REQUIRE(read_graph_string(std::move(grep_content).value()) ==
          "a.txt:2:alpha\na.txt:5:beta\nb.txt:1:gamma");
  auto grep_count = bindings.value()[5].entry.invoke(
      wh::compose::tool_call{.call_id = "call-grep-count",
                             .tool_name = "grep",
                             .arguments = R"({"pattern":"a","mode":"count"})"},
      make_call_scope(context, "grep", "call-grep-count"));
  REQUIRE(grep_count.has_value());
  REQUIRE(read_graph_string(std::move(grep_count).value()) == "3");

  wh::agent::toolset toolset{};
  auto mounted =
      wh::agent::middlewares::filesystem::mount_filesystem_tools(toolset, backend, options);
  REQUIRE(mounted.has_value());
  REQUIRE(mounted.value() == "filesystem instruction");
  REQUIRE(toolset.size() == 6U);

  auto duplicate_mount =
      wh::agent::middlewares::filesystem::mount_filesystem_tools(toolset, backend, options);
  REQUIRE(duplicate_mount.has_error());
  REQUIRE(duplicate_mount.error() == wh::core::errc::already_exists);
}
