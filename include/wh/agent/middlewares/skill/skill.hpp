// Defines local-skill tool bindings and request mutators that reuse the
// existing toolset/model contracts instead of introducing a new runtime.
#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
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
#include "wh/model/chat_model.hpp"
#include "wh/schema/tool.hpp"

namespace wh::agent::middlewares::skill {

/// Prompt language used by the rendered skill instruction and tool description.
enum class skill_language {
  /// Render English-facing guidance.
  english = 0U,
  /// Render Chinese-facing guidance.
  chinese,
};

/// Stable metadata header extracted from one local skill document.
struct skill_info {
  /// Public skill name.
  std::string name{};
  /// Short skill description.
  std::string description{};
  /// Absolute skill directory that owns `SKILL.md`.
  std::string directory{};
};

/// Fully loaded skill document returned by the backend.
struct loaded_skill {
  /// Parsed skill metadata header.
  skill_info info{};
  /// Full `SKILL.md` body after metadata header stripping.
  std::string content{};
};

/// Runtime backend used by the mounted skill tool.
struct skill_backend {
  /// Lists the current visible skill set.
  wh::core::callback_function<wh::core::result<std::vector<skill_info>>() const> list{nullptr};
  /// Loads one skill document by its public skill name.
  wh::core::callback_function<wh::core::result<loaded_skill>(std::string_view) const> load{nullptr};
};

/// Public configuration for the generated skill tool.
struct skill_tool_options {
  /// Public tool name exposed to the model.
  std::string tool_name{"skill"};
  /// Optional instruction text that should be appended to the agent.
  std::string instruction{};
  /// Render language used by the description and default instruction.
  skill_language language{skill_language::english};
};

/// One generated skill tool binding.
struct skill_tool_binding {
  /// Public tool schema visible to the model.
  wh::schema::tool_schema_definition schema{};
  /// Compose dispatch entry implementing the tool.
  wh::compose::tool_entry entry{};
};

/// Request mutator used by callers to refresh skill visibility before the
/// model turn begins.
using skill_request_middleware =
    wh::core::callback_function<wh::core::result<void>(wh::model::chat_request &) const>;

namespace detail {

[[nodiscard]] inline auto trim_copy(std::string value) -> std::string {
  const auto not_space = [](const unsigned char ch) noexcept -> bool { return !std::isspace(ch); };
  auto begin = std::find_if(value.begin(), value.end(), not_space);
  auto end = std::find_if(value.rbegin(), value.rend(), not_space).base();
  if (begin >= end) {
    return {};
  }
  return std::string{begin, end};
}

[[nodiscard]] inline auto strip_quotes(std::string value) -> std::string {
  if (value.size() >= 2U && ((value.front() == '"' && value.back() == '"') ||
                             (value.front() == '\'' && value.back() == '\''))) {
    value.erase(value.begin());
    value.pop_back();
  }
  return value;
}

[[nodiscard]] inline auto parse_skill_document(const std::filesystem::path &directory,
                                               const std::filesystem::path &skill_file)
    -> wh::core::result<loaded_skill> {
  std::ifstream input{skill_file};
  if (!input.is_open()) {
    return wh::core::result<loaded_skill>::failure(wh::core::errc::not_found);
  }

  std::ostringstream buffer{};
  buffer << input.rdbuf();
  auto content = buffer.str();
  if (content.rfind("---", 0U) != 0U) {
    return wh::core::result<loaded_skill>::failure(wh::core::errc::parse_error);
  }

  const auto second = content.find("\n---", 3U);
  if (second == std::string::npos) {
    return wh::core::result<loaded_skill>::failure(wh::core::errc::parse_error);
  }
  const auto header = content.substr(4U, second - 4U);
  auto body_offset = second + 4U;
  if (body_offset < content.size() && content[body_offset] == '\n') {
    ++body_offset;
  }

  loaded_skill parsed{};
  parsed.info.directory = directory.string();
  parsed.content = content.substr(body_offset);

  std::istringstream header_stream{header};
  std::string line{};
  while (std::getline(header_stream, line)) {
    auto colon = line.find(':');
    if (colon == std::string::npos) {
      return wh::core::result<loaded_skill>::failure(wh::core::errc::parse_error);
    }
    auto key = trim_copy(line.substr(0U, colon));
    auto value = strip_quotes(trim_copy(line.substr(colon + 1U)));
    if (key == "name") {
      parsed.info.name = std::move(value);
      continue;
    }
    if (key == "description") {
      parsed.info.description = std::move(value);
      continue;
    }
  }

  if (parsed.info.name.empty() || parsed.info.description.empty() ||
      parsed.info.directory.empty()) {
    return wh::core::result<loaded_skill>::failure(wh::core::errc::parse_error);
  }
  return parsed;
}

[[nodiscard]] inline auto render_skill_list(const std::vector<skill_info> &skills,
                                            const skill_language language) -> std::string {
  std::string rendered = language == skill_language::english
                             ? "Load one local skill guide by name. Available skills:\n"
                             : "按名称读取一个本地技能指南。可用技能：\n";
  if (skills.empty()) {
    rendered.append(language == skill_language::english ? "(none)" : "（无）");
    return rendered;
  }
  for (const auto &skill : skills) {
    rendered.append("- ");
    rendered.append(skill.name);
    rendered.append(": ");
    rendered.append(skill.description);
    rendered.push_back('\n');
  }
  rendered.pop_back();
  return rendered;
}

[[nodiscard]] inline auto default_instruction(const skill_language language) -> std::string {
  if (language == skill_language::chinese) {
    return "当你需要仓库内的专用工作流或约束时，可以使用 skill "
           "工具读取本地技能说明。";
  }
  return "When repository-specific workflow or constraints are needed, use the "
         "skill tool to read one local skill guide.";
}

[[nodiscard]] inline auto read_skill_name(const std::string_view input_json)
    -> wh::core::result<std::string> {
  auto parsed = wh::core::parse_json(input_json);
  if (parsed.has_error()) {
    return wh::core::result<std::string>::failure(parsed.error());
  }
  if (!parsed.value().IsObject()) {
    return wh::core::result<std::string>::failure(wh::core::errc::type_mismatch);
  }
  auto name = wh::core::json_find_member(parsed.value(), "name");
  if (name.has_error()) {
    return wh::core::result<std::string>::failure(name.error());
  }
  if (!name.value()->IsString()) {
    return wh::core::result<std::string>::failure(wh::core::errc::type_mismatch);
  }
  return std::string{name.value()->GetString(),
                     static_cast<std::size_t>(name.value()->GetStringLength())};
}

[[nodiscard]] inline auto render_loaded_skill(const loaded_skill &skill,
                                              const skill_language language) -> std::string {
  std::string rendered{};
  if (language == skill_language::english) {
    rendered.append("Skill: ");
    rendered.append(skill.info.name);
    rendered.append("\nDescription: ");
    rendered.append(skill.info.description);
    rendered.append("\nDirectory: ");
    rendered.append(skill.info.directory);
    rendered.append("\nPath: ");
    rendered.append(skill.info.directory);
    rendered.append("/SKILL.md\n\n");
  } else {
    rendered.append("技能：");
    rendered.append(skill.info.name);
    rendered.append("\n说明：");
    rendered.append(skill.info.description);
    rendered.append("\n目录：");
    rendered.append(skill.info.directory);
    rendered.append("\n路径：");
    rendered.append(skill.info.directory);
    rendered.append("/SKILL.md\n\n");
  }
  rendered.append(skill.content);
  return rendered;
}

[[nodiscard]] inline auto graph_string_value(std::string text)
    -> wh::core::result<wh::compose::graph_value> {
  return wh::compose::graph_value{std::move(text)};
}

} // namespace detail

/// Local backend that scans one absolute base directory for first-level
/// `SKILL.md` documents.
class skill_local_backend {
public:
  /// Creates one local backend rooted at an absolute directory path.
  explicit skill_local_backend(std::filesystem::path base_directory) noexcept
      : base_directory_(std::move(base_directory)) {}

  /// Returns the absolute base directory used for scanning.
  [[nodiscard]] auto base_directory() const noexcept -> const std::filesystem::path & {
    return base_directory_;
  }

  /// Lists all visible local skills in stable directory order.
  [[nodiscard]] auto list() const -> wh::core::result<std::vector<skill_info>> {
    if (base_directory_.empty() || !base_directory_.is_absolute()) {
      return wh::core::result<std::vector<skill_info>>::failure(wh::core::errc::invalid_argument);
    }
    if (!std::filesystem::exists(base_directory_) ||
        !std::filesystem::is_directory(base_directory_)) {
      return wh::core::result<std::vector<skill_info>>::failure(wh::core::errc::not_found);
    }

    std::vector<std::filesystem::path> directories{};
    for (const auto &entry : std::filesystem::directory_iterator(base_directory_)) {
      if (entry.is_directory()) {
        directories.push_back(entry.path());
      }
    }
    std::sort(directories.begin(), directories.end());

    std::vector<skill_info> skills{};
    for (const auto &directory : directories) {
      const auto skill_file = directory / "SKILL.md";
      if (!std::filesystem::exists(skill_file)) {
        continue;
      }
      auto parsed = detail::parse_skill_document(directory, skill_file);
      if (parsed.has_error()) {
        return wh::core::result<std::vector<skill_info>>::failure(parsed.error());
      }
      skills.push_back(std::move(parsed).value().info);
    }
    return skills;
  }

  /// Loads one local skill document by its public skill name.
  [[nodiscard]] auto load(const std::string_view skill_name) const
      -> wh::core::result<loaded_skill> {
    auto listed = list();
    if (listed.has_error()) {
      return wh::core::result<loaded_skill>::failure(listed.error());
    }
    for (const auto &skill : listed.value()) {
      if (skill.name != skill_name) {
        continue;
      }
      return detail::parse_skill_document(skill.directory,
                                          std::filesystem::path{skill.directory} / "SKILL.md");
    }
    return wh::core::result<loaded_skill>::failure(wh::core::errc::not_found);
  }

  /// Projects the local backend into the generic skill-backend contract.
  [[nodiscard]] auto to_backend() const -> skill_backend {
    return skill_backend{
        .list = [backend = *this]() -> wh::core::result<std::vector<skill_info>> {
          return backend.list();
        },
        .load = [backend = *this](const std::string_view skill_name)
            -> wh::core::result<loaded_skill> { return backend.load(skill_name); },
    };
  }

private:
  /// Absolute directory containing one level of skill subdirectories.
  std::filesystem::path base_directory_{};
};

/// Renders the current skill-tool description from the latest backend list.
[[nodiscard]] inline auto render_skill_tool_description(const skill_backend &backend,
                                                        const skill_tool_options &options = {})
    -> wh::core::result<std::string> {
  if (!static_cast<bool>(backend.list)) {
    return wh::core::result<std::string>::failure(wh::core::errc::invalid_argument);
  }
  auto listed = backend.list();
  if (listed.has_error()) {
    return wh::core::result<std::string>::failure(listed.error());
  }
  return detail::render_skill_list(listed.value(), options.language);
}

/// Returns the instruction fragment that documents the mounted skill tool.
[[nodiscard]] inline auto make_skill_instruction(const skill_tool_options &options) -> std::string {
  if (!options.instruction.empty()) {
    return options.instruction;
  }
  return detail::default_instruction(options.language);
}

/// Creates a request mutator that refreshes the skill-tool description and
/// prepends the configured instruction on every model turn.
[[nodiscard]] inline auto make_skill_request_middleware(const skill_backend &backend,
                                                        const skill_tool_options &options = {})
    -> wh::core::result<skill_request_middleware> {
  if (!static_cast<bool>(backend.list)) {
    return wh::core::result<skill_request_middleware>::failure(wh::core::errc::invalid_argument);
  }
  const auto tool_name = options.tool_name;
  if (tool_name.empty()) {
    return wh::core::result<skill_request_middleware>::failure(wh::core::errc::invalid_argument);
  }

  return skill_request_middleware{
      [backend, options, tool_name](wh::model::chat_request &request) -> wh::core::result<void> {
        auto description = render_skill_tool_description(backend, options);
        if (description.has_error()) {
          return wh::core::result<void>::failure(description.error());
        }
        for (auto &tool : request.tools) {
          if (tool.name == tool_name) {
            tool.description = description.value();
          }
        }
        auto instruction = make_skill_instruction(options);
        if (!instruction.empty()) {
          wh::schema::message message{};
          message.role = wh::schema::message_role::system;
          message.parts.emplace_back(wh::schema::text_part{std::move(instruction)});
          request.messages.insert(request.messages.begin(), std::move(message));
        }
        return {};
      }};
}

/// Creates one mounted skill tool binding.
[[nodiscard]] inline auto make_skill_tool_binding(const skill_backend &backend,
                                                  const skill_tool_options &options = {})
    -> wh::core::result<skill_tool_binding> {
  if (!static_cast<bool>(backend.list) || !static_cast<bool>(backend.load) ||
      options.tool_name.empty()) {
    return wh::core::result<skill_tool_binding>::failure(wh::core::errc::invalid_argument);
  }

  auto description = render_skill_tool_description(backend, options);
  if (description.has_error()) {
    return wh::core::result<skill_tool_binding>::failure(description.error());
  }

  skill_tool_binding binding{};
  binding.schema.name = options.tool_name;
  binding.schema.description = description.value();
  binding.schema.parameters.push_back(wh::schema::tool_parameter_schema{
      .name = "name",
      .type = wh::schema::tool_parameter_type::string,
      .description = options.language == skill_language::english ? "Skill name to load."
                                                                 : "要加载的技能名称。",
      .required = true,
  });
  binding.entry.invoke = wh::compose::tool_invoke{
      [backend, language = options.language](
          const wh::compose::tool_call &call,
          wh::tool::call_scope) -> wh::core::result<wh::compose::graph_value> {
        auto skill_name = detail::read_skill_name(call.arguments);
        if (skill_name.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(skill_name.error());
        }
        auto loaded = backend.load(skill_name.value());
        if (loaded.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(loaded.error());
        }
        return detail::graph_string_value(detail::render_loaded_skill(loaded.value(), language));
      }};
  return binding;
}

/// Mounts the skill tool into one authored toolset and returns the instruction
/// fragment that should be appended to the agent.
[[nodiscard]] inline auto mount_skill_tool(wh::agent::toolset &toolset,
                                           const skill_backend &backend,
                                           const skill_tool_options &options = {})
    -> wh::core::result<std::string> {
  auto binding = make_skill_tool_binding(backend, options);
  if (binding.has_error()) {
    return wh::core::result<std::string>::failure(binding.error());
  }
  auto added =
      toolset.add_entry(std::move(binding.value().schema), std::move(binding.value().entry));
  if (added.has_error()) {
    return wh::core::result<std::string>::failure(added.error());
  }
  return make_skill_instruction(options);
}

} // namespace wh::agent::middlewares::skill
