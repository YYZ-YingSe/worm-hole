#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/agent/middlewares/filesystem/filesystem.hpp"
#include "wh/agent/react.hpp"

namespace {

template <typename payload_t> [[nodiscard]] auto encode_payload(const payload_t &payload)
    -> std::string {
  auto encoded = wh::agent::encode_tool_payload(payload);
  REQUIRE(encoded.has_value());
  return std::move(encoded).value();
}

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

TEST_CASE("filesystem detail helpers encode decode normalize format and downshift text results",
          "[UT][wh/agent/middlewares/filesystem/"
          "filesystem.hpp][tool_payload][condition][branch][boundary]") {
  const auto encoded_read =
      wh::agent::encode_tool_payload(wh::agent::middlewares::filesystem::filesystem_read_arguments{
          .path = "src/file.txt",
          .offset = -1,
          .limit = 64,
      });
  REQUIRE(encoded_read.has_value());

  auto decoded_read = wh::agent::decode_tool_payload<
      wh::agent::middlewares::filesystem::filesystem_read_arguments>(encoded_read.value());
  REQUIRE(decoded_read.has_value());
  REQUIRE(decoded_read->path == "src/file.txt");
  REQUIRE(decoded_read->offset == std::optional<std::int64_t>{-1});
  REQUIRE(decoded_read->limit == std::optional<std::int64_t>{64});

  auto decoded_ls = wh::agent::decode_tool_payload<
      wh::agent::middlewares::filesystem::filesystem_ls_arguments>(
      encode_payload(wh::agent::middlewares::filesystem::filesystem_ls_arguments{
          .path = std::optional<std::string>{"src"},
      }));
  REQUIRE(decoded_ls.has_value());
  REQUIRE(decoded_ls->path == std::optional<std::string>{"src"});

  auto invalid_json = wh::agent::decode_tool_payload<
      wh::agent::middlewares::filesystem::filesystem_read_arguments>("{");
  REQUIRE(invalid_json.has_error());
  auto wrong_json = wh::agent::decode_tool_payload<
      wh::agent::middlewares::filesystem::filesystem_read_arguments>(R"(["x"])");
  REQUIRE(wrong_json.has_error());
  REQUIRE(wrong_json.error() == wh::core::errc::type_mismatch);

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

  wh::agent::middlewares::filesystem::filesystem_capabilities backend{};
  backend.write_large_result.sync = [](std::string, std::string) -> wh::core::result<void> {
    return {};
  };
  wh::agent::middlewares::filesystem::filesystem_tool_options options{};
  options.large_result.token_limit = 1U;
  options.large_result.path_prefix = "/tmp/result";
  auto direct = wh::agent::middlewares::filesystem::detail::materialize_text_result(
      "abc", "call-1", backend.write_large_result, options);
  REQUIRE(direct.has_value());
  REQUIRE(direct.value() == "abc");

  auto shifted = wh::agent::middlewares::filesystem::detail::materialize_text_result(
      "abcdefghij", "call-2", backend.write_large_result, options);
  REQUIRE(shifted.has_value());
  REQUIRE(shifted.value().find("Large result saved to /tmp/result/call-2") != std::string::npos);

  wh::agent::middlewares::filesystem::filesystem_capabilities missing_large_backend{};
  auto missing_large = wh::agent::middlewares::filesystem::detail::materialize_text_result(
      "abcdefghij", "call-3", missing_large_backend.write_large_result, options);
  REQUIRE(missing_large.has_error());
  REQUIRE(missing_large.error() == wh::core::errc::invalid_argument);

  wh::agent::middlewares::filesystem::filesystem_capabilities failing_large_backend{};
  failing_large_backend.write_large_result.sync = [](std::string,
                                                     std::string) -> wh::core::result<void> {
    return wh::core::result<void>::failure(wh::core::errc::unavailable);
  };
  auto failing_large = wh::agent::middlewares::filesystem::detail::materialize_text_result(
      "abcdefghij", "call-4", failing_large_backend.write_large_result, options);
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

TEST_CASE("filesystem middleware builds six tools in stable order and exports one surface",
          "[UT][wh/agent/middlewares/filesystem/"
          "filesystem.hpp][make_filesystem_middleware_surface][condition][branch][boundary]") {
  wh::agent::middlewares::filesystem::filesystem_capabilities invalid_backend{};
  auto invalid =
      wh::agent::middlewares::filesystem::make_filesystem_middleware_surface(invalid_backend);
  REQUIRE(invalid.has_error());
  REQUIRE(invalid.error() == wh::core::errc::invalid_argument);

  std::string ls_path{};
  std::string glob_path{};
  std::size_t read_offset = 999U;
  std::size_t read_limit = 999U;
  std::string large_result_path{};
  std::string large_result_text{};
  bool replace_all_seen = false;

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
      .edit = {.sync = [&replace_all_seen](std::string, std::string, std::string,
                                           const bool replace_all) -> wh::core::result<void> {
        replace_all_seen = replace_all;
        return {};
      }},
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
  REQUIRE(surface->instruction_fragments.size() == 1U);
  REQUIRE(surface->instruction_fragments.front() == "filesystem instruction");
  REQUIRE(surface->tool_bindings.size() == 6U);
  REQUIRE(surface->tool_bindings[0].schema.name == "ls");
  REQUIRE(surface->tool_bindings[1].schema.name == "read_file");
  REQUIRE(surface->tool_bindings[2].schema.name == "write_file");
  REQUIRE(surface->tool_bindings[3].schema.name == "edit_file");
  REQUIRE(surface->tool_bindings[4].schema.name == "glob");
  REQUIRE(surface->tool_bindings[5].schema.name == "grep");
  REQUIRE(surface->request_transforms.empty());
  REQUIRE(wh::agent::middlewares::filesystem::make_filesystem_instruction(options) ==
          "filesystem instruction");

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
  REQUIRE_FALSE(static_cast<bool>(surface->tool_bindings[0].entry.async_invoke));
  REQUIRE_FALSE(static_cast<bool>(surface->tool_bindings[1].entry.async_invoke));

  auto write_result = surface->tool_bindings[2].entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-write",
          .tool_name = "write_file",
          .arguments = encode_payload(
              wh::agent::middlewares::filesystem::filesystem_write_arguments{
                  .path = "file.txt",
                  .content = "body",
              }),
      },
      make_call_scope(context, "write_file", "call-write"));
  REQUIRE(write_result.has_value());
  REQUIRE(read_graph_string(std::move(write_result).value()) == "written");

  auto edit_result = surface->tool_bindings[3].entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-edit",
          .tool_name = "edit_file",
          .arguments = encode_payload(
              wh::agent::middlewares::filesystem::filesystem_edit_arguments{
                  .path = "file.txt",
                  .search = "a",
                  .replace = "b",
                  .replace_all = true,
              })},
      make_call_scope(context, "edit_file", "call-edit"));
  REQUIRE(edit_result.has_value());
  REQUIRE(replace_all_seen);
  REQUIRE(read_graph_string(std::move(edit_result).value()) == "edited");

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

  auto grep_files = surface->tool_bindings[5].entry.invoke(
      wh::compose::tool_call{.call_id = "call-grep-files",
                             .tool_name = "grep",
                             .arguments = encode_payload(
                                 wh::agent::middlewares::filesystem::filesystem_grep_arguments{
                                     .pattern = "a",
                                 })},
      make_call_scope(context, "grep", "call-grep-files"));
  REQUIRE(grep_files.has_value());
  REQUIRE(read_graph_string(std::move(grep_files).value()) == "a.txt\nb.txt");
  auto grep_content = surface->tool_bindings[5].entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-grep-content",
          .tool_name = "grep",
          .arguments = encode_payload(
              wh::agent::middlewares::filesystem::filesystem_grep_arguments{
                  .pattern = "a",
                  .mode = std::optional<std::string>{"content"},
              }),
      },
      make_call_scope(context, "grep", "call-grep-content"));
  REQUIRE(grep_content.has_value());
  REQUIRE(read_graph_string(std::move(grep_content).value()) ==
          "a.txt:2:alpha\na.txt:5:beta\nb.txt:1:gamma");
  auto grep_count = surface->tool_bindings[5].entry.invoke(
      wh::compose::tool_call{
          .call_id = "call-grep-count",
          .tool_name = "grep",
          .arguments = encode_payload(
              wh::agent::middlewares::filesystem::filesystem_grep_arguments{
                  .pattern = "a",
                  .mode = std::optional<std::string>{"count"},
              }),
      },
      make_call_scope(context, "grep", "call-grep-count"));
  REQUIRE(grep_count.has_value());
  REQUIRE(read_graph_string(std::move(grep_count).value()) == "3");

  wh::agent::react authored{"react", "assistant"};
  REQUIRE(authored.add_middleware_surface(std::move(surface).value()).has_value());
  REQUIRE(authored.tools().size() == 6U);
  REQUIRE(authored.render_instruction().find("filesystem instruction") != std::string::npos);

  auto duplicate_surface =
      wh::agent::middlewares::filesystem::make_filesystem_middleware_surface(backend, options);
  REQUIRE(duplicate_surface.has_value());
  auto duplicate = authored.add_middleware_surface(std::move(duplicate_surface).value());
  REQUIRE(duplicate.has_error());
  REQUIRE(duplicate.error() == wh::core::errc::already_exists);
}

TEST_CASE("filesystem middleware preserves async-only capabilities without fabricating sync entry",
          "[UT][wh/agent/middlewares/filesystem/"
          "filesystem.hpp][async-capabilities][condition][branch][boundary]") {
  wh::agent::middlewares::filesystem::filesystem_capabilities backend{
      .ls = {.async = [](std::string path)
                 -> wh::agent::middlewares::operation_sender<
                     wh::agent::middlewares::filesystem::filesystem_ls_result> {
        return wh::agent::middlewares::operation_sender<
            wh::agent::middlewares::filesystem::filesystem_ls_result>{
            stdexec::just(wh::agent::middlewares::filesystem::filesystem_ls_result{
                std::vector<wh::agent::middlewares::filesystem::filesystem_ls_entry>{
                    {.path = std::move(path), .directory = true}}})};
      }},
      .read = {.async = [](std::string, std::size_t offset, std::size_t limit)
                   -> wh::agent::middlewares::operation_sender<
                       wh::agent::middlewares::filesystem::filesystem_read_result> {
        return wh::agent::middlewares::operation_sender<
            wh::agent::middlewares::filesystem::filesystem_read_result>{
            stdexec::just(wh::agent::middlewares::filesystem::filesystem_read_result{
                "offset=" + std::to_string(offset) + ",limit=" + std::to_string(limit)})};
      }},
      .write = {.async = [](std::string, std::string)
                    -> wh::agent::middlewares::operation_sender<
                        wh::agent::middlewares::filesystem::filesystem_write_result> {
        return wh::agent::middlewares::operation_sender<
            wh::agent::middlewares::filesystem::filesystem_write_result>{
            stdexec::just(wh::agent::middlewares::filesystem::filesystem_write_result{})};
      }},
      .edit = {.async = [](std::string, std::string, std::string, bool)
                   -> wh::agent::middlewares::operation_sender<
                       wh::agent::middlewares::filesystem::filesystem_edit_result> {
        return wh::agent::middlewares::operation_sender<
            wh::agent::middlewares::filesystem::filesystem_edit_result>{
            stdexec::just(wh::agent::middlewares::filesystem::filesystem_edit_result{})};
      }},
      .glob = {.async = [](std::string, std::string pattern)
                   -> wh::agent::middlewares::operation_sender<
                       wh::agent::middlewares::filesystem::filesystem_glob_result> {
        return wh::agent::middlewares::operation_sender<
            wh::agent::middlewares::filesystem::filesystem_glob_result>{
            stdexec::just(wh::agent::middlewares::filesystem::filesystem_glob_result{
                std::vector<std::string>{std::move(pattern)}})};
      }},
      .grep = {.async = [](std::string, std::string pattern)
                   -> wh::agent::middlewares::operation_sender<
                       wh::agent::middlewares::filesystem::filesystem_grep_result> {
        return wh::agent::middlewares::operation_sender<
            wh::agent::middlewares::filesystem::filesystem_grep_result>{
            stdexec::just(wh::agent::middlewares::filesystem::filesystem_grep_result{
                std::vector<wh::agent::middlewares::filesystem::filesystem_grep_match>{
                    {.path = std::move(pattern), .line = 1U, .text = "match"}}})};
      }},
  };
  auto surface = wh::agent::middlewares::filesystem::make_filesystem_middleware_surface(
      backend, {.large_result = {.enabled = false}});
  REQUIRE(surface.has_value());
  REQUIRE_FALSE(static_cast<bool>(surface->tool_bindings[0].entry.invoke));
  REQUIRE(static_cast<bool>(surface->tool_bindings[0].entry.async_invoke));

  wh::core::run_context context{};
  auto ls_status = stdexec::sync_wait(surface->tool_bindings[0].entry.async_invoke(
      wh::compose::tool_call{
          .call_id = "call-ls",
          .tool_name = "ls",
          .arguments = encode_payload(
              wh::agent::middlewares::filesystem::filesystem_ls_arguments{}),
      },
      make_call_scope(context, "ls", "call-ls")));
  REQUIRE(ls_status.has_value());
  REQUIRE(std::get<0>(*ls_status).has_value());
  REQUIRE(read_graph_string(std::move(std::get<0>(*ls_status)).value()) == "//");

  auto read_status = stdexec::sync_wait(surface->tool_bindings[1].entry.async_invoke(
      wh::compose::tool_call{
          .call_id = "call-read",
          .tool_name = "read_file",
          .arguments = encode_payload(
              wh::agent::middlewares::filesystem::filesystem_read_arguments{
                  .path = "file.txt",
                  .offset = 2,
                  .limit = 7,
              }),
      },
      make_call_scope(context, "read_file", "call-read")));
  REQUIRE(read_status.has_value());
  REQUIRE(std::get<0>(*read_status).has_value());
  REQUIRE(read_graph_string(std::move(std::get<0>(*read_status)).value()) == "offset=2,limit=7");
}
