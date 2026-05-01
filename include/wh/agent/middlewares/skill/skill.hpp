// Defines local-skill tool bindings and request transforms that reuse the
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

#include "wh/agent/middlewares/surface.hpp"
#include "wh/agent/tool_payload.hpp"
#include "wh/compose/node/tools_contract.hpp"
#include "wh/core/any.hpp"
#include "wh/core/function.hpp"
#include "wh/core/result.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/tool/types.hpp"

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

using skill_list_result = wh::core::result<std::vector<skill_info>>;
using skill_load_result = wh::core::result<loaded_skill>;
using skill_list_capability = wh::agent::middlewares::operation_binding<skill_list_result>;
using skill_load_capability =
    wh::agent::middlewares::operation_binding<skill_load_result, std::string>;

/// Runtime capabilities used by the mounted skill tool.
struct skill_capabilities {
  /// Lists the current visible skill set.
  skill_list_capability list{};
  /// Loads one skill document by its public skill name.
  skill_load_capability load{};
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

struct skill_load_arguments {
  std::string name{};
};

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

[[nodiscard]] inline auto default_tool_description(const skill_language language) -> std::string {
  if (language == skill_language::chinese) {
    return "按名称读取一个本地技能指南。";
  }
  return "Load one local skill guide by name.";
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

inline auto wh_to_json(const skill_load_arguments &input, wh::core::json_value &output,
                       wh::core::json_allocator &allocator) -> wh::core::result<void> {
  output.SetObject();
  return wh::agent::detail::write_json_member(output, "name", input.name, allocator);
}

inline auto wh_from_json(const wh::core::json_value &input, skill_load_arguments &output)
    -> wh::core::result<void> {
  if (!input.IsObject()) {
    return wh::core::result<void>::failure(wh::core::errc::type_mismatch);
  }
  auto name = wh::agent::detail::read_required_json_member<std::string>(input, "name");
  if (name.has_error()) {
    return wh::core::result<void>::failure(name.error());
  }
  output.name = std::move(name).value();
  return {};
}

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

  /// Projects the local backend into the generic skill-capability contract.
  [[nodiscard]] auto to_capabilities() const -> skill_capabilities {
    return skill_capabilities{
        .list =
            skill_list_capability{
                .sync = [backend = *this]() -> wh::core::result<std::vector<skill_info>> {
                  return backend.list();
                }},
        .load = skill_load_capability{.sync = [backend = *this](std::string skill_name)
                                          -> wh::core::result<loaded_skill> {
          return backend.load(skill_name);
        }},
    };
  }

private:
  /// Absolute directory containing one level of skill subdirectories.
  std::filesystem::path base_directory_{};
};

/// Renders the current skill-tool description from the latest backend list.
[[nodiscard]] inline auto render_skill_tool_description(const skill_capabilities &backend,
                                                        const skill_tool_options &options = {})
    -> wh::core::result<std::string> {
  if (!backend.list.sync) {
    return wh::core::result<std::string>::failure(wh::core::errc::invalid_argument);
  }
  auto listed = backend.list.sync();
  if (listed.has_error()) {
    return wh::core::result<std::string>::failure(listed.error());
  }
  return detail::render_skill_list(listed.value(), options.language);
}

[[nodiscard]] inline auto
render_skill_tool_description_sender(const skill_capabilities &backend,
                                     const skill_tool_options &options = {})
    -> wh::agent::middlewares::operation_sender<wh::core::result<std::string>> {
  if (!static_cast<bool>(backend.list)) {
    return wh::agent::middlewares::detail::make_operation_failure_sender<
        wh::core::result<std::string>>(wh::core::errc::invalid_argument);
  }
  return wh::agent::middlewares::operation_sender<wh::core::result<std::string>>{
      wh::agent::middlewares::detail::open_operation_sender(backend.list) |
      stdexec::then(
          [language = options.language](skill_list_result listed) -> wh::core::result<std::string> {
            if (listed.has_error()) {
              return wh::core::result<std::string>::failure(listed.error());
            }
            return detail::render_skill_list(listed.value(), language);
          })};
}

/// Returns the instruction fragment that documents the mounted skill tool.
[[nodiscard]] inline auto make_skill_instruction(const skill_tool_options &options) -> std::string {
  if (!options.instruction.empty()) {
    return options.instruction;
  }
  return detail::default_instruction(options.language);
}

/// Creates a request transform that refreshes the skill-tool description and
/// prepends the configured instruction on every model turn.
[[nodiscard]] inline auto make_skill_request_transform(const skill_capabilities &backend,
                                                       const skill_tool_options &options = {})
    -> wh::core::result<wh::agent::middlewares::request_transform_binding> {
  if (!static_cast<bool>(backend.list)) {
    return wh::core::result<wh::agent::middlewares::request_transform_binding>::failure(
        wh::core::errc::invalid_argument);
  }
  const auto tool_name = options.tool_name;
  if (tool_name.empty()) {
    return wh::core::result<wh::agent::middlewares::request_transform_binding>::failure(
        wh::core::errc::invalid_argument);
  }

  return wh::agent::middlewares::request_transform_binding{
      .sync = backend.list.sync
                  ? wh::agent::middlewares::sync_operation<
                        wh::agent::middlewares::request_transform_result, wh::model::chat_request,
                        wh::core::run_context
                            &>{[backend, options, tool_name](wh::model::chat_request request,
                                                             wh::core::run_context &)
                                   -> wh::agent::middlewares::request_transform_result {
                      auto description = render_skill_tool_description(backend, options);
                      if (description.has_error()) {
                        return wh::agent::middlewares::request_transform_result::failure(
                            description.error());
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
                      return request;
                    }}
                  : nullptr,
      .async = backend.list.async
                   ? wh::agent::middlewares::async_operation<
                         wh::agent::middlewares::request_transform_result, wh::model::chat_request,
                         wh::core::run_context
                             &>{[backend,
                                 options, tool_name](wh::model::chat_request request,
                                                     wh::core::run_context &)
                                    -> wh::agent::middlewares::request_transform_sender {
                       return wh::agent::middlewares::request_transform_sender{
                           render_skill_tool_description_sender(backend, options) |
                           stdexec::then([request = std::move(request), tool_name,
                                          instruction = make_skill_instruction(options)](
                                             wh::core::result<std::string> description) mutable
                                             -> wh::agent::middlewares::request_transform_result {
                             if (description.has_error()) {
                               return wh::agent::middlewares::request_transform_result::failure(
                                   description.error());
                             }
                             for (auto &tool : request.tools) {
                               if (tool.name == tool_name) {
                                 tool.description = description.value();
                               }
                             }
                             if (!instruction.empty()) {
                               wh::schema::message message{};
                               message.role = wh::schema::message_role::system;
                               message.parts.emplace_back(
                                   wh::schema::text_part{std::move(instruction)});
                               request.messages.insert(request.messages.begin(),
                                                       std::move(message));
                             }
                             return request;
                           })};
                     }}
                   : nullptr};
}

/// Builds one skill middleware surface.
[[nodiscard]] inline auto make_skill_middleware_surface(const skill_capabilities &backend,
                                                        const skill_tool_options &options = {})
    -> wh::core::result<wh::agent::middlewares::middleware_surface> {
  if (!static_cast<bool>(backend.list) || !static_cast<bool>(backend.load) ||
      options.tool_name.empty()) {
    return wh::core::result<wh::agent::middlewares::middleware_surface>::failure(
        wh::core::errc::invalid_argument);
  }

  auto request_transform = make_skill_request_transform(backend, options);
  if (request_transform.has_error()) {
    return wh::core::result<wh::agent::middlewares::middleware_surface>::failure(
        request_transform.error());
  }

  wh::agent::middlewares::middleware_surface surface{};
  surface.instruction_fragments.push_back(make_skill_instruction(options));
  surface.request_transforms.push_back(std::move(request_transform).value());
  surface.tool_bindings.emplace_back();
  auto &binding = surface.tool_bindings.back();
  binding.schema.name = options.tool_name;
  auto description = render_skill_tool_description(backend, options);
  binding.schema.description = description.has_value()
                                   ? std::move(description).value()
                                   : detail::default_tool_description(options.language);
  binding.schema.parameters.push_back(wh::schema::tool_parameter_schema{
      .name = "name",
      .type = wh::schema::tool_parameter_type::string,
      .description = options.language == skill_language::english ? "Skill name to load."
                                                                 : "要加载的技能名称。",
      .required = true,
  });
  binding.entry = wh::agent::make_value_tool_entry<skill_load_arguments>({
      .sync = backend.load.sync
                  ? wh::agent::sync_value_tool_handler<skill_load_arguments>{
                        [backend, language = options.language](
                            const wh::compose::tool_call &,
                            skill_load_arguments args) -> wh::agent::tool_text_result {
                          auto loaded = backend.load.sync(std::move(args.name));
                          if (loaded.has_error()) {
                            return wh::agent::tool_text_result::failure(loaded.error());
                          }
                          return detail::render_loaded_skill(loaded.value(), language);
                        }}
                  : nullptr,
      .async =
          backend.load.async
              ? wh::agent::async_value_tool_handler<skill_load_arguments>{
                    [backend, language = options.language](
                        wh::compose::tool_call,
                        skill_load_arguments args) -> wh::agent::tool_text_sender {
                      return wh::agent::tool_text_sender{
                          wh::agent::middlewares::detail::open_operation_sender(
                              backend.load, std::move(args.name)) |
                          stdexec::then([language](
                                            skill_load_result loaded)
                                            -> wh::agent::tool_text_result {
                            if (loaded.has_error()) {
                              return wh::agent::tool_text_result::failure(loaded.error());
                            }
                            return detail::render_loaded_skill(loaded.value(), language);
                          })};
                    }}
              : nullptr,
  });
  return surface;
}

} // namespace wh::agent::middlewares::skill
