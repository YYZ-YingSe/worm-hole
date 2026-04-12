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

#include "wh/agent/toolset.hpp"
#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/core/any.hpp"
#include "wh/core/function.hpp"
#include "wh/core/json.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/stream/reader/values_stream_reader.hpp"
#include "wh/schema/tool.hpp"

namespace wh::agent::middlewares::filesystem {

/// One filesystem listing row returned by the host backend.
struct filesystem_ls_entry {
  /// Stable path reported by the backend.
  std::string path{};
  /// True when the listed path is a directory.
  bool directory{false};
};

/// One grep match returned by the host backend.
struct filesystem_grep_match {
  /// Matched file path.
  std::string path{};
  /// 1-based line number when available.
  std::size_t line{0U};
  /// Matched line text.
  std::string text{};
};

/// Output projection mode for grep results.
enum class filesystem_grep_mode {
  /// Return one deduplicated file list.
  files_with_matches = 0U,
  /// Return one `path:line:text` line list.
  content,
  /// Return only the match count.
  count,
};

/// Host filesystem operations injected by the caller.
struct filesystem_backend {
  /// Lists entries below one directory path.
  wh::core::callback_function<wh::core::result<
      std::vector<filesystem_ls_entry>>(std::string_view) const>
      ls{nullptr};
  /// Reads one file slice from `offset` with the supplied `limit`.
  wh::core::callback_function<wh::core::result<std::string>(
      std::string_view, std::size_t, std::size_t) const>
      read{nullptr};
  /// Writes one new file payload.
  wh::core::callback_function<wh::core::result<void>(std::string_view,
                                                     std::string_view) const>
      write{nullptr};
  /// Edits one file by replacing one search string.
  wh::core::callback_function<wh::core::result<void>(
      std::string_view, std::string_view, std::string_view, bool) const>
      edit{nullptr};
  /// Expands one glob pattern below the supplied base path.
  wh::core::callback_function<wh::core::result<std::vector<std::string>>(
      std::string_view, std::string_view) const>
      glob{nullptr};
  /// Greps one pattern below the supplied base path.
  wh::core::callback_function<
      wh::core::result<std::vector<filesystem_grep_match>>(
          std::string_view, std::string_view) const>
      grep{nullptr};
  /// Persists one oversized tool result at the supplied path.
  wh::core::callback_function<wh::core::result<void>(std::string_view,
                                                     std::string_view) const>
      write_large_result{nullptr};
};

/// Oversized-result handling options shared by all filesystem tools.
struct filesystem_large_result_options {
  /// True enables large-result downshift through `write_large_result`.
  bool enabled{true};
  /// Token-like limit used as `chars > token_limit * 4`.
  std::size_t token_limit{1024U};
  /// Path prefix used when persisting oversized results.
  std::string path_prefix{"/large_tool_result"};
};

/// Public configuration for the generated filesystem tools.
struct filesystem_tool_options {
  /// Optional instruction fragment describing the mounted tools.
  std::string instruction{
      "Filesystem tools are available for listing, reading, writing, editing, "
      "globbing, and grepping files."};
  /// Public description for `ls`.
  std::string ls_description{"List files below one directory path."};
  /// Public description for `read_file`.
  std::string read_description{"Read one file slice."};
  /// Public description for `write_file`.
  std::string write_description{"Write one new file."};
  /// Public description for `edit_file`.
  std::string edit_description{"Edit one file by replacing text."};
  /// Public description for `glob`.
  std::string glob_description{"Expand one glob pattern."};
  /// Public description for `grep`.
  std::string grep_description{"Search one pattern across files."};
  /// Oversized-result handling knobs shared by all generated tools.
  filesystem_large_result_options large_result{};
};

/// One generated filesystem tool binding.
struct filesystem_tool_binding {
  /// Public tool schema visible to the model.
  wh::schema::tool_schema_definition schema{};
  /// Compose dispatch entry implementing the tool.
  wh::compose::tool_entry entry{};
};

namespace detail {

using graph_value_result = wh::core::result<wh::compose::graph_value>;
using invoke_handler = wh::core::callback_function<graph_value_result(
    const wh::compose::tool_call &, wh::tool::call_scope) const>;

[[nodiscard]] inline auto parse_json_object(const std::string_view input_json)
    -> wh::core::result<wh::core::json_document> {
  auto parsed = wh::core::parse_json(input_json);
  if (parsed.has_error()) {
    return wh::core::result<wh::core::json_document>::failure(parsed.error());
  }
  if (!parsed.value().IsObject()) {
    return wh::core::result<wh::core::json_document>::failure(
        wh::core::errc::type_mismatch);
  }
  return parsed;
}

[[nodiscard]] inline auto
optional_string_member(const wh::core::json_value &value,
                       const std::string_view key)
    -> wh::core::result<std::optional<std::string>> {
  auto member = wh::core::json_find_member(value, key);
  if (member.has_error()) {
    if (member.error() == wh::core::errc::not_found) {
      return std::optional<std::string>{};
    }
    return wh::core::result<std::optional<std::string>>::failure(
        member.error());
  }
  if (!member.value()->IsString()) {
    return wh::core::result<std::optional<std::string>>::failure(
        wh::core::errc::type_mismatch);
  }
  return std::optional<std::string>{
      std::string{member.value()->GetString(),
                  static_cast<std::size_t>(member.value()->GetStringLength())}};
}

[[nodiscard]] inline auto
required_string_member(const wh::core::json_value &value,
                       const std::string_view key)
    -> wh::core::result<std::string> {
  auto member = wh::core::json_find_member(value, key);
  if (member.has_error()) {
    return wh::core::result<std::string>::failure(member.error());
  }
  if (!member.value()->IsString()) {
    return wh::core::result<std::string>::failure(
        wh::core::errc::type_mismatch);
  }
  return std::string{
      member.value()->GetString(),
      static_cast<std::size_t>(member.value()->GetStringLength())};
}

[[nodiscard]] inline auto
optional_signed_member(const wh::core::json_value &value,
                       const std::string_view key)
    -> wh::core::result<std::optional<std::int64_t>> {
  auto member = wh::core::json_find_member(value, key);
  if (member.has_error()) {
    if (member.error() == wh::core::errc::not_found) {
      return std::optional<std::int64_t>{};
    }
    return wh::core::result<std::optional<std::int64_t>>::failure(
        member.error());
  }
  if (!member.value()->IsInt64()) {
    return wh::core::result<std::optional<std::int64_t>>::failure(
        wh::core::errc::type_mismatch);
  }
  return std::optional<std::int64_t>{member.value()->GetInt64()};
}

[[nodiscard]] inline auto
optional_bool_member(const wh::core::json_value &value,
                     const std::string_view key)
    -> wh::core::result<std::optional<bool>> {
  auto member = wh::core::json_find_member(value, key);
  if (member.has_error()) {
    if (member.error() == wh::core::errc::not_found) {
      return std::optional<bool>{};
    }
    return wh::core::result<std::optional<bool>>::failure(member.error());
  }
  if (!member.value()->IsBool()) {
    return wh::core::result<std::optional<bool>>::failure(
        wh::core::errc::type_mismatch);
  }
  return std::optional<bool>{member.value()->GetBool()};
}

[[nodiscard]] inline auto
normalize_directory_path(const std::optional<std::string> &path)
    -> std::string {
  if (!path.has_value() || path->empty()) {
    return "/";
  }
  return *path;
}

[[nodiscard]] inline auto
normalize_offset(const std::optional<std::int64_t> offset) noexcept
    -> std::size_t {
  if (!offset.has_value() || *offset < 0) {
    return 0U;
  }
  return static_cast<std::size_t>(*offset);
}

[[nodiscard]] inline auto
normalize_limit(const std::optional<std::int64_t> limit) noexcept
    -> std::size_t {
  if (!limit.has_value() || *limit <= 0) {
    return 4096U;
  }
  return static_cast<std::size_t>(*limit);
}

[[nodiscard]] inline auto
parse_grep_mode(const std::optional<std::string> &mode)
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

[[nodiscard]] inline auto join_lines(const std::vector<std::string> &lines)
    -> std::string {
  std::string joined{};
  for (std::size_t index = 0U; index < lines.size(); ++index) {
    if (index != 0U) {
      joined.push_back('\n');
    }
    joined.append(lines[index]);
  }
  return joined;
}

[[nodiscard]] inline auto
format_ls_entries(const std::vector<filesystem_ls_entry> &entries)
    -> std::string {
  std::vector<std::string> lines{};
  lines.reserve(entries.size());
  for (const auto &entry : entries) {
    lines.push_back(entry.directory ? entry.path + "/" : entry.path);
  }
  return join_lines(lines);
}

[[nodiscard]] inline auto
format_glob_entries(const std::vector<std::string> &entries) -> std::string {
  return join_lines(entries);
}

[[nodiscard]] inline auto
format_grep_entries(const std::vector<filesystem_grep_match> &entries,
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
    lines.push_back(entry.path + ":" + std::to_string(entry.line) + ":" +
                    entry.text);
  }
  return join_lines(lines);
}

[[nodiscard]] inline auto
large_result_path(const filesystem_large_result_options &options,
                  const std::string_view call_id) -> std::string {
  std::string path = options.path_prefix;
  if (path.empty() || path.back() != '/') {
    path.push_back('/');
  }
  path.append(call_id);
  return path;
}

[[nodiscard]] inline auto preview_text(const std::string_view text)
    -> std::string {
  std::vector<std::string> lines{};
  std::size_t start = 0U;
  while (start <= text.size() && lines.size() < 10U) {
    const auto end = text.find('\n', start);
    const auto count =
        end == std::string_view::npos ? text.size() - start : end - start;
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

[[nodiscard]] inline auto materialize_text_result(
    const std::string_view text, const std::string_view call_id,
    const filesystem_backend &backend, const filesystem_tool_options &options)
    -> wh::core::result<std::string> {
  if (!options.large_result.enabled ||
      text.size() <= options.large_result.token_limit * 4U) {
    return std::string{text};
  }
  if (!static_cast<bool>(backend.write_large_result)) {
    return wh::core::result<std::string>::failure(
        wh::core::errc::invalid_argument);
  }

  const auto output_path = large_result_path(options.large_result, call_id);
  auto written = backend.write_large_result(output_path, text);
  if (written.has_error()) {
    return wh::core::result<std::string>::failure(written.error());
  }

  std::string summary = "Large result saved to ";
  summary.append(output_path);
  summary.append("\nPreview:\n");
  summary.append(preview_text(text));
  return summary;
}

[[nodiscard]] inline auto graph_string_value(std::string text)
    -> graph_value_result {
  return wh::compose::graph_value{std::move(text)};
}

[[nodiscard]] inline auto wrap_text_invoke(invoke_handler invoke)
    -> wh::compose::tool_entry {
  wh::compose::tool_entry entry{};
  entry.invoke = wh::compose::tool_invoke{invoke};
  return entry;
}

[[nodiscard]] inline auto make_string_parameter(std::string name,
                                                std::string description,
                                                const bool required = true)
    -> wh::schema::tool_parameter_schema {
  return wh::schema::tool_parameter_schema{
      .name = std::move(name),
      .type = wh::schema::tool_parameter_type::string,
      .description = std::move(description),
      .required = required,
  };
}

[[nodiscard]] inline auto make_integer_parameter(std::string name,
                                                 std::string description,
                                                 const bool required = false)
    -> wh::schema::tool_parameter_schema {
  return wh::schema::tool_parameter_schema{
      .name = std::move(name),
      .type = wh::schema::tool_parameter_type::integer,
      .description = std::move(description),
      .required = required,
  };
}

[[nodiscard]] inline auto make_bool_parameter(std::string name,
                                              std::string description,
                                              const bool required = false)
    -> wh::schema::tool_parameter_schema {
  return wh::schema::tool_parameter_schema{
      .name = std::move(name),
      .type = wh::schema::tool_parameter_type::boolean,
      .description = std::move(description),
      .required = required,
  };
}

} // namespace detail

/// Returns the instruction fragment that documents the mounted filesystem
/// tools.
[[nodiscard]] inline auto
make_filesystem_instruction(const filesystem_tool_options &options)
    -> std::string {
  return options.instruction;
}

/// Builds the six default filesystem tools in stable order.
[[nodiscard]] inline auto
make_filesystem_tool_bindings(const filesystem_backend &backend,
                              filesystem_tool_options options = {})
    -> wh::core::result<std::vector<filesystem_tool_binding>> {
  if (!static_cast<bool>(backend.ls) || !static_cast<bool>(backend.read) ||
      !static_cast<bool>(backend.write) || !static_cast<bool>(backend.edit) ||
      !static_cast<bool>(backend.glob) || !static_cast<bool>(backend.grep)) {
    return wh::core::result<std::vector<filesystem_tool_binding>>::failure(
        wh::core::errc::invalid_argument);
  }
  if (options.large_result.enabled &&
      !static_cast<bool>(backend.write_large_result)) {
    return wh::core::result<std::vector<filesystem_tool_binding>>::failure(
        wh::core::errc::invalid_argument);
  }

  std::vector<filesystem_tool_binding> bindings{};
  bindings.reserve(6U);

  filesystem_tool_binding ls{};
  ls.schema.name = "ls";
  ls.schema.description = options.ls_description;
  ls.schema.parameters.push_back(
      detail::make_string_parameter("path", "Directory path to list.", false));
  ls.entry = detail::wrap_text_invoke(detail::invoke_handler{
      [backend](const wh::compose::tool_call &call,
                wh::tool::call_scope) -> detail::graph_value_result {
        auto parsed = detail::parse_json_object(call.arguments);
        if (parsed.has_error()) {
          return detail::graph_value_result::failure(parsed.error());
        }
        auto path = detail::optional_string_member(parsed.value(), "path");
        if (path.has_error()) {
          return detail::graph_value_result::failure(path.error());
        }
        auto listed =
            backend.ls(detail::normalize_directory_path(path.value()));
        if (listed.has_error()) {
          return detail::graph_value_result::failure(listed.error());
        }
        return detail::graph_string_value(
            detail::format_ls_entries(listed.value()));
      }});
  bindings.push_back(std::move(ls));

  filesystem_tool_binding read{};
  read.schema.name = "read_file";
  read.schema.description = options.read_description;
  read.schema.parameters.push_back(
      detail::make_string_parameter("path", "File path to read."));
  read.schema.parameters.push_back(detail::make_integer_parameter(
      "offset", "Start offset; negative values clamp to 0."));
  read.schema.parameters.push_back(detail::make_integer_parameter(
      "limit", "Maximum bytes to read; non-positive values use the default."));
  read.entry = detail::wrap_text_invoke(detail::invoke_handler{
      [backend, options](const wh::compose::tool_call &call,
                         wh::tool::call_scope) -> detail::graph_value_result {
        auto parsed = detail::parse_json_object(call.arguments);
        if (parsed.has_error()) {
          return detail::graph_value_result::failure(parsed.error());
        }
        auto path = detail::required_string_member(parsed.value(), "path");
        auto offset = detail::optional_signed_member(parsed.value(), "offset");
        auto limit = detail::optional_signed_member(parsed.value(), "limit");
        if (path.has_error()) {
          return detail::graph_value_result::failure(path.error());
        }
        if (offset.has_error()) {
          return detail::graph_value_result::failure(offset.error());
        }
        if (limit.has_error()) {
          return detail::graph_value_result::failure(limit.error());
        }
        auto content =
            backend.read(path.value(), detail::normalize_offset(offset.value()),
                         detail::normalize_limit(limit.value()));
        if (content.has_error()) {
          return detail::graph_value_result::failure(content.error());
        }
        auto materialized = detail::materialize_text_result(
            content.value(), call.call_id, backend, options);
        if (materialized.has_error()) {
          return detail::graph_value_result::failure(materialized.error());
        }
        return detail::graph_string_value(std::move(materialized).value());
      }});
  bindings.push_back(std::move(read));

  filesystem_tool_binding write{};
  write.schema.name = "write_file";
  write.schema.description = options.write_description;
  write.schema.parameters.push_back(
      detail::make_string_parameter("path", "File path to create."));
  write.schema.parameters.push_back(
      detail::make_string_parameter("content", "Text to write."));
  write.entry = detail::wrap_text_invoke(detail::invoke_handler{
      [backend](const wh::compose::tool_call &call,
                wh::tool::call_scope) -> detail::graph_value_result {
        auto parsed = detail::parse_json_object(call.arguments);
        if (parsed.has_error()) {
          return detail::graph_value_result::failure(parsed.error());
        }
        auto path = detail::required_string_member(parsed.value(), "path");
        auto content =
            detail::required_string_member(parsed.value(), "content");
        if (path.has_error()) {
          return detail::graph_value_result::failure(path.error());
        }
        if (content.has_error()) {
          return detail::graph_value_result::failure(content.error());
        }
        auto written = backend.write(path.value(), content.value());
        if (written.has_error()) {
          return detail::graph_value_result::failure(written.error());
        }
        return detail::graph_string_value("written");
      }});
  bindings.push_back(std::move(write));

  filesystem_tool_binding edit{};
  edit.schema.name = "edit_file";
  edit.schema.description = options.edit_description;
  edit.schema.parameters.push_back(
      detail::make_string_parameter("path", "File path to edit."));
  edit.schema.parameters.push_back(
      detail::make_string_parameter("search", "Search text."));
  edit.schema.parameters.push_back(
      detail::make_string_parameter("replace", "Replacement text."));
  edit.schema.parameters.push_back(detail::make_bool_parameter(
      "replace_all", "True replaces all matches instead of one."));
  edit.entry = detail::wrap_text_invoke(detail::invoke_handler{
      [backend](const wh::compose::tool_call &call,
                wh::tool::call_scope) -> detail::graph_value_result {
        auto parsed = detail::parse_json_object(call.arguments);
        if (parsed.has_error()) {
          return detail::graph_value_result::failure(parsed.error());
        }
        auto path = detail::required_string_member(parsed.value(), "path");
        auto search = detail::required_string_member(parsed.value(), "search");
        auto replace =
            detail::required_string_member(parsed.value(), "replace");
        auto replace_all =
            detail::optional_bool_member(parsed.value(), "replace_all");
        if (path.has_error()) {
          return detail::graph_value_result::failure(path.error());
        }
        if (search.has_error()) {
          return detail::graph_value_result::failure(search.error());
        }
        if (replace.has_error()) {
          return detail::graph_value_result::failure(replace.error());
        }
        if (replace_all.has_error()) {
          return detail::graph_value_result::failure(replace_all.error());
        }
        auto edited =
            backend.edit(path.value(), search.value(), replace.value(),
                         replace_all.value().value_or(false));
        if (edited.has_error()) {
          return detail::graph_value_result::failure(edited.error());
        }
        return detail::graph_string_value("edited");
      }});
  bindings.push_back(std::move(edit));

  filesystem_tool_binding glob{};
  glob.schema.name = "glob";
  glob.schema.description = options.glob_description;
  glob.schema.parameters.push_back(
      detail::make_string_parameter("path", "Base directory path.", false));
  glob.schema.parameters.push_back(
      detail::make_string_parameter("pattern", "Glob pattern."));
  glob.entry = detail::wrap_text_invoke(detail::invoke_handler{
      [backend](const wh::compose::tool_call &call,
                wh::tool::call_scope) -> detail::graph_value_result {
        auto parsed = detail::parse_json_object(call.arguments);
        if (parsed.has_error()) {
          return detail::graph_value_result::failure(parsed.error());
        }
        auto path = detail::optional_string_member(parsed.value(), "path");
        auto pattern =
            detail::required_string_member(parsed.value(), "pattern");
        if (path.has_error()) {
          return detail::graph_value_result::failure(path.error());
        }
        if (pattern.has_error()) {
          return detail::graph_value_result::failure(pattern.error());
        }
        auto matched = backend.glob(
            detail::normalize_directory_path(path.value()), pattern.value());
        if (matched.has_error()) {
          return detail::graph_value_result::failure(matched.error());
        }
        return detail::graph_string_value(
            detail::format_glob_entries(matched.value()));
      }});
  bindings.push_back(std::move(glob));

  filesystem_tool_binding grep{};
  grep.schema.name = "grep";
  grep.schema.description = options.grep_description;
  grep.schema.parameters.push_back(
      detail::make_string_parameter("path", "Base directory path.", false));
  grep.schema.parameters.push_back(
      detail::make_string_parameter("pattern", "Search pattern."));
  grep.schema.parameters.push_back(detail::make_string_parameter(
      "mode", "files_with_matches, content, or count.", false));
  grep.entry = detail::wrap_text_invoke(detail::invoke_handler{
      [backend](const wh::compose::tool_call &call,
                wh::tool::call_scope) -> detail::graph_value_result {
        auto parsed = detail::parse_json_object(call.arguments);
        if (parsed.has_error()) {
          return detail::graph_value_result::failure(parsed.error());
        }
        auto path = detail::optional_string_member(parsed.value(), "path");
        auto pattern =
            detail::required_string_member(parsed.value(), "pattern");
        auto mode = detail::optional_string_member(parsed.value(), "mode");
        if (path.has_error()) {
          return detail::graph_value_result::failure(path.error());
        }
        if (pattern.has_error()) {
          return detail::graph_value_result::failure(pattern.error());
        }
        if (mode.has_error()) {
          return detail::graph_value_result::failure(mode.error());
        }
        auto matched = backend.grep(
            detail::normalize_directory_path(path.value()), pattern.value());
        if (matched.has_error()) {
          return detail::graph_value_result::failure(matched.error());
        }
        return detail::graph_string_value(detail::format_grep_entries(
            matched.value(), detail::parse_grep_mode(mode.value())));
      }});
  bindings.push_back(std::move(grep));

  return bindings;
}

/// Mounts the six default filesystem tools into one authored toolset and
/// returns the instruction fragment that should be appended to the agent.
[[nodiscard]] inline auto mount_filesystem_tools(
    wh::agent::toolset &toolset, const filesystem_backend &backend,
    filesystem_tool_options options = {}) -> wh::core::result<std::string> {
  auto bindings = make_filesystem_tool_bindings(backend, options);
  if (bindings.has_error()) {
    return wh::core::result<std::string>::failure(bindings.error());
  }
  for (auto &binding : bindings.value()) {
    auto added =
        toolset.add_entry(std::move(binding.schema), std::move(binding.entry));
    if (added.has_error()) {
      return wh::core::result<std::string>::failure(added.error());
    }
  }
  return make_filesystem_instruction(options);
}

} // namespace wh::agent::middlewares::filesystem
