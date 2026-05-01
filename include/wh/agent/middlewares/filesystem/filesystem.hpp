// Defines reusable filesystem tool bindings built on top of the existing
// compose/tool contracts without introducing a second middleware runtime.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/agent/middlewares/surface.hpp"
#include "wh/agent/tool_payload.hpp"
#include "wh/core/any.hpp"
#include "wh/core/json.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/tool/types.hpp"

namespace wh::agent::middlewares::filesystem {

struct filesystem_ls_entry {
  std::string path{};
  bool directory{false};
};

struct filesystem_grep_match {
  std::string path{};
  std::size_t line{0U};
  std::string text{};
};

enum class filesystem_grep_mode {
  files_with_matches = 0U,
  content,
  count,
};

using filesystem_ls_result = wh::core::result<std::vector<filesystem_ls_entry>>;
using filesystem_read_result = wh::core::result<std::string>;
using filesystem_write_result = wh::core::result<void>;
using filesystem_edit_result = wh::core::result<void>;
using filesystem_glob_result = wh::core::result<std::vector<std::string>>;
using filesystem_grep_result = wh::core::result<std::vector<filesystem_grep_match>>;
using filesystem_large_result_write = wh::core::result<void>;

using filesystem_ls_capability =
    wh::agent::middlewares::operation_binding<filesystem_ls_result, std::string>;
using filesystem_read_capability =
    wh::agent::middlewares::operation_binding<filesystem_read_result, std::string, std::size_t,
                                              std::size_t>;
using filesystem_write_capability =
    wh::agent::middlewares::operation_binding<filesystem_write_result, std::string, std::string>;
using filesystem_edit_capability =
    wh::agent::middlewares::operation_binding<filesystem_edit_result, std::string, std::string,
                                              std::string, bool>;
using filesystem_glob_capability =
    wh::agent::middlewares::operation_binding<filesystem_glob_result, std::string, std::string>;
using filesystem_grep_capability =
    wh::agent::middlewares::operation_binding<filesystem_grep_result, std::string, std::string>;
using filesystem_large_result_capability =
    wh::agent::middlewares::operation_binding<filesystem_large_result_write, std::string,
                                              std::string>;

struct filesystem_capabilities {
  filesystem_ls_capability ls{};
  filesystem_read_capability read{};
  filesystem_write_capability write{};
  filesystem_edit_capability edit{};
  filesystem_glob_capability glob{};
  filesystem_grep_capability grep{};
  filesystem_large_result_capability write_large_result{};
};

struct filesystem_large_result_options {
  bool enabled{true};
  std::size_t token_limit{1024U};
  std::string path_prefix{"/large_tool_result"};
};

struct filesystem_tool_options {
  std::string instruction{"Filesystem tools are available for listing, reading, writing, editing, "
                          "globbing, and grepping files."};
  std::string ls_description{"List files below one directory path."};
  std::string read_description{"Read one file slice."};
  std::string write_description{"Write one new file."};
  std::string edit_description{"Edit one file by replacing text."};
  std::string glob_description{"Expand one glob pattern."};
  std::string grep_description{"Search one pattern across files."};
  filesystem_large_result_options large_result{};
};

struct filesystem_ls_arguments {
  std::optional<std::string> path{};
};

struct filesystem_read_arguments {
  std::string path{};
  std::optional<std::int64_t> offset{};
  std::optional<std::int64_t> limit{};
};

struct filesystem_write_arguments {
  std::string path{};
  std::string content{};
};

struct filesystem_edit_arguments {
  std::string path{};
  std::string search{};
  std::string replace{};
  std::optional<bool> replace_all{};
};

struct filesystem_glob_arguments {
  std::optional<std::string> path{};
  std::string pattern{};
};

struct filesystem_grep_arguments {
  std::optional<std::string> path{};
  std::string pattern{};
  std::optional<std::string> mode{};
};

namespace detail {

using graph_value_result = wh::core::result<wh::compose::graph_value>;
using string_result_sender =
    wh::agent::middlewares::operation_sender<wh::core::result<std::string>>;

[[nodiscard]] inline auto normalize_directory_path(const std::optional<std::string> &path)
    -> std::string {
  if (!path.has_value() || path->empty()) {
    return "/";
  }
  return *path;
}

[[nodiscard]] inline auto normalize_offset(const std::optional<std::int64_t> offset) noexcept
    -> std::size_t {
  if (!offset.has_value() || *offset < 0) {
    return 0U;
  }
  return static_cast<std::size_t>(*offset);
}

[[nodiscard]] inline auto normalize_limit(const std::optional<std::int64_t> limit) noexcept
    -> std::size_t {
  if (!limit.has_value() || *limit <= 0) {
    return 4096U;
  }
  return static_cast<std::size_t>(*limit);
}

[[nodiscard]] inline auto parse_grep_mode(const std::optional<std::string> &mode)
    -> filesystem_grep_mode {
  if (!mode.has_value() || mode->empty() || *mode == "files_with_matches") {
    return filesystem_grep_mode::files_with_matches;
  }
  if (*mode == "content") {
    return filesystem_grep_mode::content;
  }
  if (*mode == "count") {
    return filesystem_grep_mode::count;
  }
  return filesystem_grep_mode::files_with_matches;
}

[[nodiscard]] inline auto join_lines(const std::vector<std::string> &lines) -> std::string {
  std::string joined{};
  for (std::size_t index = 0U; index < lines.size(); ++index) {
    if (index != 0U) {
      joined.push_back('\n');
    }
    joined.append(lines[index]);
  }
  return joined;
}

[[nodiscard]] inline auto format_ls_entries(const std::vector<filesystem_ls_entry> &entries)
    -> std::string {
  std::vector<std::string> lines{};
  lines.reserve(entries.size());
  for (const auto &entry : entries) {
    lines.push_back(entry.directory ? entry.path + "/" : entry.path);
  }
  return join_lines(lines);
}

[[nodiscard]] inline auto format_glob_entries(const std::vector<std::string> &entries)
    -> std::string {
  return join_lines(entries);
}

[[nodiscard]] inline auto format_grep_entries(const std::vector<filesystem_grep_match> &entries,
                                              const filesystem_grep_mode mode) -> std::string {
  if (mode == filesystem_grep_mode::count) {
    return std::to_string(entries.size());
  }

  std::vector<std::string> lines{};
  if (mode == filesystem_grep_mode::files_with_matches) {
    std::string last_path{};
    for (const auto &entry : entries) {
      if (entry.path == last_path) {
        continue;
      }
      lines.push_back(entry.path);
      last_path = entry.path;
    }
    return join_lines(lines);
  }

  lines.reserve(entries.size());
  for (const auto &entry : entries) {
    lines.push_back(entry.path + ":" + std::to_string(entry.line) + ":" + entry.text);
  }
  return join_lines(lines);
}

[[nodiscard]] inline auto large_result_path(const filesystem_large_result_options &options,
                                            const std::string_view call_id) -> std::string {
  std::string path = options.path_prefix;
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }
  path.append(call_id);
  return path;
}

[[nodiscard]] inline auto preview_text(const std::string_view text) -> std::string {
  std::vector<std::string> lines{};
  std::size_t start = 0U;
  while (start <= text.size() && lines.size() < 10U) {
    const auto end = text.find('\n', start);
    const auto count = end == std::string_view::npos ? text.size() - start : end - start;
    auto line = std::string{text.substr(start, count)};
    if (line.size() > 1000U) {
      line.resize(1000U);
    }
    lines.push_back(std::move(line));
    if (end == std::string_view::npos) {
      break;
    }
    start = end + 1U;
  }
  return join_lines(lines);
}

[[nodiscard]] inline auto
materialize_text_result(const std::string_view text, const std::string_view call_id,
                        const filesystem_large_result_capability &write_large_result,
                        const filesystem_tool_options &options) -> wh::core::result<std::string> {
  if (!options.large_result.enabled || text.size() <= options.large_result.token_limit * 4U) {
    return std::string{text};
  }
  if (!write_large_result.sync) {
    return wh::core::result<std::string>::failure(wh::core::errc::invalid_argument);
  }

  const auto output_path = large_result_path(options.large_result, call_id);
  auto written = write_large_result.sync(output_path, std::string{text});
  if (written.has_error()) {
    return wh::core::result<std::string>::failure(written.error());
  }

  std::string summary = "Large result saved to ";
  summary.append(output_path);
  summary.append("\nPreview:\n");
  summary.append(preview_text(text));
  return summary;
}

[[nodiscard]] inline auto
materialize_text_result_sender(std::string text, std::string call_id,
                               const filesystem_large_result_capability &write_large_result,
                               const filesystem_tool_options &options) -> string_result_sender {
  if (!options.large_result.enabled || text.size() <= options.large_result.token_limit * 4U) {
    return wh::agent::middlewares::detail::make_operation_ready_sender(
        wh::core::result<std::string>{std::move(text)});
  }
  if (!static_cast<bool>(write_large_result)) {
    return wh::agent::middlewares::detail::make_operation_failure_sender<
        wh::core::result<std::string>>(wh::core::errc::invalid_argument);
  }

  const auto output_path = large_result_path(options.large_result, call_id);
  auto make_summary =
      [path = output_path, text = std::move(text)](
          filesystem_large_result_write written) mutable -> wh::core::result<std::string> {
    if (written.has_error()) {
      return wh::core::result<std::string>::failure(written.error());
    }
    std::string summary = "Large result saved to ";
    summary.append(path);
    summary.append("\nPreview:\n");
    summary.append(preview_text(text));
    return summary;
  };
  if (write_large_result.async) {
    return string_result_sender{wh::agent::middlewares::detail::open_operation_sender(
                                    write_large_result, output_path, std::move(text)) |
                                stdexec::then(std::move(make_summary))};
  }
  return wh::agent::middlewares::detail::make_operation_ready_sender(
      make_summary(write_large_result.sync(output_path, std::move(text))));
}

[[nodiscard]] inline auto graph_string_value(std::string text) -> graph_value_result {
  return wh::compose::graph_value{std::move(text)};
}

[[nodiscard]] inline auto make_string_parameter(std::string name, std::string description,
                                                const bool required = true)
    -> wh::schema::tool_parameter_schema {
  return wh::schema::tool_parameter_schema{
      .name = std::move(name),
      .type = wh::schema::tool_parameter_type::string,
      .description = std::move(description),
      .required = required,
  };
}

[[nodiscard]] inline auto make_integer_parameter(std::string name, std::string description,
                                                 const bool required = false)
    -> wh::schema::tool_parameter_schema {
  return wh::schema::tool_parameter_schema{
      .name = std::move(name),
      .type = wh::schema::tool_parameter_type::integer,
      .description = std::move(description),
      .required = required,
  };
}

[[nodiscard]] inline auto make_bool_parameter(std::string name, std::string description,
                                              const bool required = false)
    -> wh::schema::tool_parameter_schema {
  return wh::schema::tool_parameter_schema{
      .name = std::move(name),
      .type = wh::schema::tool_parameter_type::boolean,
      .description = std::move(description),
      .required = required,
  };
}

template <typename arguments_t>
[[nodiscard]] inline auto require_object(const wh::core::json_value &input)
    -> wh::core::result<void> {
  static_cast<void>(sizeof(arguments_t));
  if (!input.IsObject()) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }
  return {};
}

} // namespace detail

inline auto wh_to_json(const filesystem_ls_arguments &input, wh::core::json_value &output,
                       wh::core::json_allocator &allocator) -> wh::core::result<void> {
  output.SetObject();
  return wh::agent::detail::write_optional_json_member(output, "path", input.path, allocator);
}

inline auto wh_from_json(const wh::core::json_value &input, filesystem_ls_arguments &output)
    -> wh::core::result<void> {
  auto object = detail::require_object<filesystem_ls_arguments>(input);
  if (object.has_error()) {
    return object;
  }
  auto path = wh::agent::detail::read_optional_json_member<std::string>(input, "path");
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  output.path = std::move(path).value();
  return {};
}

inline auto wh_to_json(const filesystem_read_arguments &input, wh::core::json_value &output,
                       wh::core::json_allocator &allocator) -> wh::core::result<void> {
  output.SetObject();
  auto path = wh::agent::detail::write_json_member(output, "path", input.path, allocator);
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  auto offset = wh::agent::detail::write_optional_json_member(output, "offset", input.offset,
                                                              allocator);
  if (offset.has_error()) {
    return wh::core::result<void>::failure(offset.error());
  }
  return wh::agent::detail::write_optional_json_member(output, "limit", input.limit, allocator);
}

inline auto wh_from_json(const wh::core::json_value &input, filesystem_read_arguments &output)
    -> wh::core::result<void> {
  auto object = detail::require_object<filesystem_read_arguments>(input);
  if (object.has_error()) {
    return object;
  }
  auto path = wh::agent::detail::read_required_json_member<std::string>(input, "path");
  auto offset = wh::agent::detail::read_optional_json_member<std::int64_t>(input, "offset");
  auto limit = wh::agent::detail::read_optional_json_member<std::int64_t>(input, "limit");
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  if (offset.has_error()) {
    return wh::core::result<void>::failure(offset.error());
  }
  if (limit.has_error()) {
    return wh::core::result<void>::failure(limit.error());
  }
  output.path = std::move(path).value();
  output.offset = std::move(offset).value();
  output.limit = std::move(limit).value();
  return {};
}

inline auto wh_to_json(const filesystem_write_arguments &input, wh::core::json_value &output,
                       wh::core::json_allocator &allocator) -> wh::core::result<void> {
  output.SetObject();
  auto path = wh::agent::detail::write_json_member(output, "path", input.path, allocator);
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  return wh::agent::detail::write_json_member(output, "content", input.content, allocator);
}

inline auto wh_from_json(const wh::core::json_value &input, filesystem_write_arguments &output)
    -> wh::core::result<void> {
  auto object = detail::require_object<filesystem_write_arguments>(input);
  if (object.has_error()) {
    return object;
  }
  auto path = wh::agent::detail::read_required_json_member<std::string>(input, "path");
  auto content = wh::agent::detail::read_required_json_member<std::string>(input, "content");
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  if (content.has_error()) {
    return wh::core::result<void>::failure(content.error());
  }
  output.path = std::move(path).value();
  output.content = std::move(content).value();
  return {};
}

inline auto wh_to_json(const filesystem_edit_arguments &input, wh::core::json_value &output,
                       wh::core::json_allocator &allocator) -> wh::core::result<void> {
  output.SetObject();
  auto path = wh::agent::detail::write_json_member(output, "path", input.path, allocator);
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  auto search = wh::agent::detail::write_json_member(output, "search", input.search, allocator);
  if (search.has_error()) {
    return wh::core::result<void>::failure(search.error());
  }
  auto replace = wh::agent::detail::write_json_member(output, "replace", input.replace, allocator);
  if (replace.has_error()) {
    return wh::core::result<void>::failure(replace.error());
  }
  return wh::agent::detail::write_optional_json_member(output, "replace_all", input.replace_all,
                                                       allocator);
}

inline auto wh_from_json(const wh::core::json_value &input, filesystem_edit_arguments &output)
    -> wh::core::result<void> {
  auto object = detail::require_object<filesystem_edit_arguments>(input);
  if (object.has_error()) {
    return object;
  }
  auto path = wh::agent::detail::read_required_json_member<std::string>(input, "path");
  auto search = wh::agent::detail::read_required_json_member<std::string>(input, "search");
  auto replace = wh::agent::detail::read_required_json_member<std::string>(input, "replace");
  auto replace_all = wh::agent::detail::read_optional_json_member<bool>(input, "replace_all");
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  if (search.has_error()) {
    return wh::core::result<void>::failure(search.error());
  }
  if (replace.has_error()) {
    return wh::core::result<void>::failure(replace.error());
  }
  if (replace_all.has_error()) {
    return wh::core::result<void>::failure(replace_all.error());
  }
  output.path = std::move(path).value();
  output.search = std::move(search).value();
  output.replace = std::move(replace).value();
  output.replace_all = std::move(replace_all).value();
  return {};
}

inline auto wh_to_json(const filesystem_glob_arguments &input, wh::core::json_value &output,
                       wh::core::json_allocator &allocator) -> wh::core::result<void> {
  output.SetObject();
  auto path = wh::agent::detail::write_optional_json_member(output, "path", input.path, allocator);
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  return wh::agent::detail::write_json_member(output, "pattern", input.pattern, allocator);
}

inline auto wh_from_json(const wh::core::json_value &input, filesystem_glob_arguments &output)
    -> wh::core::result<void> {
  auto object = detail::require_object<filesystem_glob_arguments>(input);
  if (object.has_error()) {
    return object;
  }
  auto path = wh::agent::detail::read_optional_json_member<std::string>(input, "path");
  auto pattern = wh::agent::detail::read_required_json_member<std::string>(input, "pattern");
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  if (pattern.has_error()) {
    return wh::core::result<void>::failure(pattern.error());
  }
  output.path = std::move(path).value();
  output.pattern = std::move(pattern).value();
  return {};
}

inline auto wh_to_json(const filesystem_grep_arguments &input, wh::core::json_value &output,
                       wh::core::json_allocator &allocator) -> wh::core::result<void> {
  output.SetObject();
  auto path = wh::agent::detail::write_optional_json_member(output, "path", input.path, allocator);
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  auto pattern = wh::agent::detail::write_json_member(output, "pattern", input.pattern, allocator);
  if (pattern.has_error()) {
    return wh::core::result<void>::failure(pattern.error());
  }
  return wh::agent::detail::write_optional_json_member(output, "mode", input.mode, allocator);
}

inline auto wh_from_json(const wh::core::json_value &input, filesystem_grep_arguments &output)
    -> wh::core::result<void> {
  auto object = detail::require_object<filesystem_grep_arguments>(input);
  if (object.has_error()) {
    return object;
  }
  auto path = wh::agent::detail::read_optional_json_member<std::string>(input, "path");
  auto pattern = wh::agent::detail::read_required_json_member<std::string>(input, "pattern");
  auto mode = wh::agent::detail::read_optional_json_member<std::string>(input, "mode");
  if (path.has_error()) {
    return wh::core::result<void>::failure(path.error());
  }
  if (pattern.has_error()) {
    return wh::core::result<void>::failure(pattern.error());
  }
  if (mode.has_error()) {
    return wh::core::result<void>::failure(mode.error());
  }
  output.path = std::move(path).value();
  output.pattern = std::move(pattern).value();
  output.mode = std::move(mode).value();
  return {};
}

[[nodiscard]] inline auto make_filesystem_instruction(const filesystem_tool_options &options)
    -> std::string {
  return options.instruction;
}

[[nodiscard]] inline auto make_filesystem_middleware_surface(const filesystem_capabilities &backend,
                                                             filesystem_tool_options options = {})
    -> wh::core::result<wh::agent::middlewares::middleware_surface> {
  if (!static_cast<bool>(backend.ls) || !static_cast<bool>(backend.read) ||
      !static_cast<bool>(backend.write) || !static_cast<bool>(backend.edit) ||
      !static_cast<bool>(backend.glob) || !static_cast<bool>(backend.grep)) {
    return wh::core::result<wh::agent::middlewares::middleware_surface>::failure(
        wh::core::errc::invalid_argument);
  }
  if (options.large_result.enabled && !static_cast<bool>(backend.write_large_result)) {
    return wh::core::result<wh::agent::middlewares::middleware_surface>::failure(
        wh::core::errc::invalid_argument);
  }

  wh::agent::middlewares::middleware_surface surface{};
  surface.instruction_fragments.push_back(make_filesystem_instruction(options));
  surface.tool_bindings.reserve(6U);

  wh::agent::tool_binding_pair ls{};
  ls.schema.name = "ls";
  ls.schema.description = options.ls_description;
  ls.schema.parameters.push_back(
      detail::make_string_parameter("path", "Directory path to list.", false));
  ls.entry = wh::agent::make_value_tool_entry<filesystem_ls_arguments>({
      .sync = backend.ls.sync
                  ? wh::agent::sync_value_tool_handler<filesystem_ls_arguments>{
                        [backend](const wh::compose::tool_call &,
                                  filesystem_ls_arguments args) -> wh::agent::tool_text_result {
                          auto listed =
                              backend.ls.sync(detail::normalize_directory_path(args.path));
                          if (listed.has_error()) {
                            return wh::agent::tool_text_result::failure(listed.error());
                          }
                          return detail::format_ls_entries(listed.value());
                        }}
                  : nullptr,
      .async =
          backend.ls.async
              ? wh::agent::async_value_tool_handler<filesystem_ls_arguments>{
                    [backend](wh::compose::tool_call,
                              filesystem_ls_arguments args) -> wh::agent::tool_text_sender {
                      return wh::agent::tool_text_sender{
                          wh::agent::middlewares::detail::open_operation_sender(
                              backend.ls, detail::normalize_directory_path(args.path)) |
                          stdexec::then([](filesystem_ls_result listed)
                                            -> wh::agent::tool_text_result {
                            if (listed.has_error()) {
                              return wh::agent::tool_text_result::failure(listed.error());
                            }
                            return detail::format_ls_entries(listed.value());
                          })};
                    }}
              : nullptr,
  });
  surface.tool_bindings.push_back(std::move(ls));

  wh::agent::tool_binding_pair read{};
  read.schema.name = "read_file";
  read.schema.description = options.read_description;
  read.schema.parameters.push_back(detail::make_string_parameter("path", "File path to read."));
  read.schema.parameters.push_back(
      detail::make_integer_parameter("offset", "Start offset; negative values clamp to 0."));
  read.schema.parameters.push_back(detail::make_integer_parameter(
      "limit", "Maximum bytes to read; non-positive values use the default."));
  read.entry = wh::agent::make_value_tool_entry<filesystem_read_arguments>({
      .sync = backend.read.sync
                  ? wh::agent::sync_value_tool_handler<filesystem_read_arguments>{
                        [backend, options](const wh::compose::tool_call &call,
                                           filesystem_read_arguments args)
                            -> wh::agent::tool_text_result {
                          auto content = backend.read.sync(
                              args.path, detail::normalize_offset(args.offset),
                              detail::normalize_limit(args.limit));
                          if (content.has_error()) {
                            return wh::agent::tool_text_result::failure(content.error());
                          }
                          return detail::materialize_text_result(content.value(), call.call_id,
                                                                 backend.write_large_result,
                                                                 options);
                        }}
                  : nullptr,
      .async = backend.read.async
                   ? wh::agent::async_value_tool_handler<filesystem_read_arguments>{
                         [backend, options](wh::compose::tool_call call,
                                            filesystem_read_arguments args)
                             -> wh::agent::tool_text_sender {
                           return wh::agent::tool_text_sender{
                               wh::agent::middlewares::detail::open_operation_sender(
                                   backend.read, args.path, detail::normalize_offset(args.offset),
                                   detail::normalize_limit(args.limit)) |
                               stdexec::let_value([call_id = std::move(call.call_id), backend,
                                                   options](filesystem_read_result content)
                                                      -> detail::string_result_sender {
                                 if (content.has_error()) {
                                   return wh::agent::middlewares::detail::make_operation_failure_sender<
                                       wh::core::result<std::string>>(content.error());
                                 }
                                 return detail::materialize_text_result_sender(
                                     std::move(content).value(), std::move(call_id),
                                     backend.write_large_result, options);
                               })};
                         }}
                   : nullptr,
  });
  surface.tool_bindings.push_back(std::move(read));

  wh::agent::tool_binding_pair write{};
  write.schema.name = "write_file";
  write.schema.description = options.write_description;
  write.schema.parameters.push_back(detail::make_string_parameter("path", "File path to create."));
  write.schema.parameters.push_back(detail::make_string_parameter("content", "Text to write."));
  write.entry = wh::agent::make_value_tool_entry<filesystem_write_arguments>({
      .sync = backend.write.sync
                  ? wh::agent::sync_value_tool_handler<filesystem_write_arguments>{
                        [backend](const wh::compose::tool_call &,
                                  filesystem_write_arguments args) -> wh::agent::tool_text_result {
                          auto written = backend.write.sync(args.path, args.content);
                          if (written.has_error()) {
                            return wh::agent::tool_text_result::failure(written.error());
                          }
                          return std::string{"written"};
                        }}
                  : nullptr,
      .async =
          backend.write.async
              ? wh::agent::async_value_tool_handler<filesystem_write_arguments>{
                    [backend](wh::compose::tool_call,
                              filesystem_write_arguments args) -> wh::agent::tool_text_sender {
                      return wh::agent::tool_text_sender{
                          wh::agent::middlewares::detail::open_operation_sender(
                              backend.write, args.path, args.content) |
                          stdexec::then([](filesystem_write_result written)
                                            -> wh::agent::tool_text_result {
                            if (written.has_error()) {
                              return wh::agent::tool_text_result::failure(written.error());
                            }
                            return std::string{"written"};
                          })};
                    }}
              : nullptr,
  });
  surface.tool_bindings.push_back(std::move(write));

  wh::agent::tool_binding_pair edit{};
  edit.schema.name = "edit_file";
  edit.schema.description = options.edit_description;
  edit.schema.parameters.push_back(detail::make_string_parameter("path", "File path to edit."));
  edit.schema.parameters.push_back(detail::make_string_parameter("search", "Search text."));
  edit.schema.parameters.push_back(detail::make_string_parameter("replace", "Replacement text."));
  edit.schema.parameters.push_back(
      detail::make_bool_parameter("replace_all", "True replaces all matches instead of one."));
  edit.entry = wh::agent::make_value_tool_entry<filesystem_edit_arguments>({
      .sync = backend.edit.sync
                  ? wh::agent::sync_value_tool_handler<filesystem_edit_arguments>{
                        [backend](const wh::compose::tool_call &,
                                  filesystem_edit_arguments args) -> wh::agent::tool_text_result {
                          auto edited = backend.edit.sync(args.path, args.search, args.replace,
                                                          args.replace_all.value_or(false));
                          if (edited.has_error()) {
                            return wh::agent::tool_text_result::failure(edited.error());
                          }
                          return std::string{"edited"};
                        }}
                  : nullptr,
      .async =
          backend.edit.async
              ? wh::agent::async_value_tool_handler<filesystem_edit_arguments>{
                    [backend](wh::compose::tool_call,
                              filesystem_edit_arguments args) -> wh::agent::tool_text_sender {
                      return wh::agent::tool_text_sender{
                          wh::agent::middlewares::detail::open_operation_sender(
                              backend.edit, args.path, args.search, args.replace,
                              args.replace_all.value_or(false)) |
                          stdexec::then([](filesystem_edit_result edited)
                                            -> wh::agent::tool_text_result {
                            if (edited.has_error()) {
                              return wh::agent::tool_text_result::failure(edited.error());
                            }
                            return std::string{"edited"};
                          })};
                    }}
              : nullptr,
  });
  surface.tool_bindings.push_back(std::move(edit));

  wh::agent::tool_binding_pair glob{};
  glob.schema.name = "glob";
  glob.schema.description = options.glob_description;
  glob.schema.parameters.push_back(
      detail::make_string_parameter("path", "Base directory path.", false));
  glob.schema.parameters.push_back(detail::make_string_parameter("pattern", "Glob pattern."));
  glob.entry = wh::agent::make_value_tool_entry<filesystem_glob_arguments>({
      .sync = backend.glob.sync
                  ? wh::agent::sync_value_tool_handler<filesystem_glob_arguments>{
                        [backend](const wh::compose::tool_call &,
                                  filesystem_glob_arguments args) -> wh::agent::tool_text_result {
                          auto matched = backend.glob.sync(
                              detail::normalize_directory_path(args.path), args.pattern);
                          if (matched.has_error()) {
                            return wh::agent::tool_text_result::failure(matched.error());
                          }
                          return detail::format_glob_entries(matched.value());
                        }}
                  : nullptr,
      .async =
          backend.glob.async
              ? wh::agent::async_value_tool_handler<filesystem_glob_arguments>{
                    [backend](wh::compose::tool_call,
                              filesystem_glob_arguments args) -> wh::agent::tool_text_sender {
                      return wh::agent::tool_text_sender{
                          wh::agent::middlewares::detail::open_operation_sender(
                              backend.glob, detail::normalize_directory_path(args.path),
                              args.pattern) |
                          stdexec::then([](filesystem_glob_result matched)
                                            -> wh::agent::tool_text_result {
                            if (matched.has_error()) {
                              return wh::agent::tool_text_result::failure(matched.error());
                            }
                            return detail::format_glob_entries(matched.value());
                          })};
                    }}
              : nullptr,
  });
  surface.tool_bindings.push_back(std::move(glob));

  wh::agent::tool_binding_pair grep{};
  grep.schema.name = "grep";
  grep.schema.description = options.grep_description;
  grep.schema.parameters.push_back(
      detail::make_string_parameter("path", "Base directory path.", false));
  grep.schema.parameters.push_back(detail::make_string_parameter("pattern", "Search pattern."));
  grep.schema.parameters.push_back(
      detail::make_string_parameter("mode", "files_with_matches, content, or count.", false));
  grep.entry = wh::agent::make_value_tool_entry<filesystem_grep_arguments>({
      .sync = backend.grep.sync
                  ? wh::agent::sync_value_tool_handler<filesystem_grep_arguments>{
                        [backend](const wh::compose::tool_call &,
                                  filesystem_grep_arguments args) -> wh::agent::tool_text_result {
                          auto matched = backend.grep.sync(
                              detail::normalize_directory_path(args.path), args.pattern);
                          if (matched.has_error()) {
                            return wh::agent::tool_text_result::failure(matched.error());
                          }
                          return detail::format_grep_entries(matched.value(),
                                                             detail::parse_grep_mode(args.mode));
                        }}
                  : nullptr,
      .async =
          backend.grep.async
              ? wh::agent::async_value_tool_handler<filesystem_grep_arguments>{
                    [backend](wh::compose::tool_call,
                              filesystem_grep_arguments args) -> wh::agent::tool_text_sender {
                      return wh::agent::tool_text_sender{
                          wh::agent::middlewares::detail::open_operation_sender(
                              backend.grep, detail::normalize_directory_path(args.path),
                              args.pattern) |
                          stdexec::then(
                              [mode = detail::parse_grep_mode(args.mode)](
                                  filesystem_grep_result matched) -> wh::agent::tool_text_result {
                                if (matched.has_error()) {
                                  return wh::agent::tool_text_result::failure(matched.error());
                                }
                                return detail::format_grep_entries(matched.value(), mode);
                              })};
                    }}
              : nullptr,
  });
  surface.tool_bindings.push_back(std::move(grep));

  return surface;
}

} // namespace wh::agent::middlewares::filesystem
