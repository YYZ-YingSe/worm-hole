// Defines the shared host-mediated multi-agent graph lowerer used by
// supervisor, swarm, and research shells without reviving a second runtime.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "wh/adk/call_options.hpp"
#include "wh/adk/detail/shared_state.hpp"
#include "wh/adk/deterministic_transfer.hpp"
#include "wh/agent/agent.hpp"
#include "wh/agent/research.hpp"
#include "wh/agent/supervisor.hpp"
#include "wh/agent/swarm.hpp"
#include "wh/compose/authored/value_branch.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/message.hpp"

namespace wh::adk::detail {

namespace host_graph_detail {

struct host_member_definition {
  std::string name{};
  std::string description{};
  std::optional<std::string> parent_name{};
  bool allow_transfer_to_parent{false};
  std::vector<std::string> allowed_children{};
  wh::compose::graph graph{};
};

struct host_graph_definition {
  host_member_definition root{};
  std::vector<host_member_definition> children{};
};

struct host_request {
  std::string agent_name{};
  std::vector<wh::schema::message> messages{};
};

enum class host_step_kind : std::uint8_t {
  invoke_next = 0U,
  emit_final,
};

struct host_step_decision {
  host_step_kind kind{host_step_kind::invoke_next};
};

struct runtime_state {
  std::vector<wh::schema::message> visible_history{};
  std::size_t input_message_count{0U};
  std::string active_agent_name{};
  std::vector<wh::schema::message> active_request_messages{};
  std::optional<wh::agent::agent_output> final_output{};
  std::unordered_map<std::string, wh::adk::deterministic_transfer_state,
                     wh::core::transparent_string_hash, wh::core::transparent_string_equal>
      transfer_states{};
};

[[nodiscard]] inline auto equal_message_part(const wh::schema::message_part &left,
                                             const wh::schema::message_part &right) -> bool {
  if (left.index() != right.index()) {
    return false;
  }
  if (const auto *lhs = std::get_if<wh::schema::text_part>(&left); lhs != nullptr) {
    const auto *rhs = std::get_if<wh::schema::text_part>(&right);
    return rhs != nullptr && lhs->text == rhs->text;
  }
  if (const auto *lhs = std::get_if<wh::schema::image_part>(&left); lhs != nullptr) {
    const auto *rhs = std::get_if<wh::schema::image_part>(&right);
    return rhs != nullptr && lhs->uri == rhs->uri;
  }
  if (const auto *lhs = std::get_if<wh::schema::audio_part>(&left); lhs != nullptr) {
    const auto *rhs = std::get_if<wh::schema::audio_part>(&right);
    return rhs != nullptr && lhs->base64 == rhs->base64 && lhs->uri == rhs->uri;
  }
  if (const auto *lhs = std::get_if<wh::schema::video_part>(&left); lhs != nullptr) {
    const auto *rhs = std::get_if<wh::schema::video_part>(&right);
    return rhs != nullptr && lhs->uri == rhs->uri;
  }
  if (const auto *lhs = std::get_if<wh::schema::file_part>(&left); lhs != nullptr) {
    const auto *rhs = std::get_if<wh::schema::file_part>(&right);
    return rhs != nullptr && lhs->uri == rhs->uri && lhs->mime_type == rhs->mime_type;
  }
  if (const auto *lhs = std::get_if<wh::schema::tool_call_part>(&left); lhs != nullptr) {
    const auto *rhs = std::get_if<wh::schema::tool_call_part>(&right);
    return rhs != nullptr && lhs->index == rhs->index && lhs->id == rhs->id &&
           lhs->type == rhs->type && lhs->name == rhs->name && lhs->arguments == rhs->arguments &&
           lhs->complete == rhs->complete;
  }
  return false;
}

[[nodiscard]] inline auto equal_message(const wh::schema::message &left,
                                        const wh::schema::message &right) -> bool {
  if (left.message_id != right.message_id || left.role != right.role || left.name != right.name ||
      left.tool_call_id != right.tool_call_id || left.tool_name != right.tool_name ||
      left.parts.size() != right.parts.size() ||
      left.meta.finish_reason != right.meta.finish_reason ||
      left.meta.usage.prompt_tokens != right.meta.usage.prompt_tokens ||
      left.meta.usage.completion_tokens != right.meta.usage.completion_tokens ||
      left.meta.usage.total_tokens != right.meta.usage.total_tokens ||
      left.meta.logprobs.size() != right.meta.logprobs.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.parts.size(); ++index) {
    if (!equal_message_part(left.parts[index], right.parts[index])) {
      return false;
    }
  }
  for (std::size_t index = 0U; index < left.meta.logprobs.size(); ++index) {
    if (left.meta.logprobs[index].token != right.meta.logprobs[index].token ||
        left.meta.logprobs[index].logprob != right.meta.logprobs[index].logprob) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline auto message_has_payload(const wh::schema::message &message) -> bool {
  return !message.parts.empty() || !message.tool_call_id.empty() || !message.tool_name.empty() ||
         !message.message_id.empty();
}

inline auto stamp_agent_message(wh::schema::message &message, const std::string_view agent_name)
    -> void {
  if (message.role == wh::schema::message_role::assistant ||
      message.role == wh::schema::message_role::tool) {
    message.name = std::string{agent_name};
  }
}

[[nodiscard]] inline auto matches_history_prefix(const std::vector<wh::schema::message> &history,
                                                 const std::vector<wh::schema::message> &request)
    -> bool {
  if (history.size() < request.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < request.size(); ++index) {
    if (!equal_message(history[index], request[index])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] inline auto
normalize_history_delta(const wh::agent::agent_output &output,
                        const std::vector<wh::schema::message> &request_messages,
                        const std::string_view agent_name) -> std::vector<wh::schema::message> {
  std::vector<wh::schema::message> delta{};
  if (matches_history_prefix(output.history_messages, request_messages)) {
    delta.assign(std::next(output.history_messages.begin(),
                           static_cast<std::ptrdiff_t>(request_messages.size())),
                 output.history_messages.end());
  } else {
    delta = output.history_messages;
  }

  for (auto &message : delta) {
    stamp_agent_message(message, agent_name);
  }

  if (delta.empty() && message_has_payload(output.final_message)) {
    auto fallback = output.final_message;
    stamp_agent_message(fallback, agent_name);
    delta.push_back(std::move(fallback));
  }
  return delta;
}

[[nodiscard]] inline auto filter_transfer_messages(std::vector<wh::schema::message> messages,
                                                   const std::string_view tool_call_id)
    -> std::vector<wh::schema::message> {
  if (tool_call_id.empty()) {
    return messages;
  }

  std::erase_if(messages, [tool_call_id](const wh::schema::message &message) {
    if (const auto assistant_call_id =
            wh::adk::detail::transfer::transfer_assistant_call_id(message);
        assistant_call_id.has_value() && *assistant_call_id == tool_call_id) {
      return true;
    }

    const auto parsed = wh::adk::parse_transfer_tool_message(message);
    return parsed.has_value() && parsed.value().tool_call_id == tool_call_id;
  });
  return messages;
}

[[nodiscard]] inline auto make_transfer_state(const std::string_view agent_name)
    -> wh::adk::deterministic_transfer_state {
  wh::adk::deterministic_transfer_state state{};
  if (!agent_name.empty()) {
    state.visited_agents.insert(std::string{agent_name});
  }
  return state;
}

[[nodiscard]] inline auto member_node_key(const std::string_view agent_name) -> std::string {
  return std::string{"agent/"} + std::string{agent_name};
}

[[nodiscard]] inline auto member_ref(const host_graph_definition &definition,
                                     const std::string_view agent_name)
    -> wh::core::result<std::reference_wrapper<const host_member_definition>> {
  if (definition.root.name == agent_name) {
    return std::cref(definition.root);
  }
  for (const auto &child : definition.children) {
    if (child.name == agent_name) {
      return std::cref(child);
    }
  }
  return wh::core::result<std::reference_wrapper<const host_member_definition>>::failure(
      wh::core::errc::not_found);
}

[[nodiscard]] inline auto has_member(const host_graph_definition &definition,
                                     const std::string_view agent_name) -> bool {
  return member_ref(definition, agent_name).has_value();
}

[[nodiscard]] inline auto resolve_transfer_target(const host_graph_definition &definition,
                                                  const host_member_definition &current,
                                                  const std::string_view target_agent_name)
    -> wh::core::result<wh::adk::transfer_target> {
  if (target_agent_name.empty()) {
    return wh::core::result<wh::adk::transfer_target>::failure(wh::core::errc::invalid_argument);
  }

  if (target_agent_name == current.name) {
    return wh::adk::transfer_target{
        .kind = wh::adk::transfer_target_kind::current,
    };
  }

  if (current.parent_name.has_value() && target_agent_name == *current.parent_name) {
    if (!current.allow_transfer_to_parent) {
      return wh::core::result<wh::adk::transfer_target>::failure(
          wh::core::errc::contract_violation);
    }
    return wh::adk::transfer_target{
        .kind = wh::adk::transfer_target_kind::parent,
    };
  }

  if (std::ranges::find(current.allowed_children, target_agent_name) !=
      current.allowed_children.end()) {
    return wh::adk::transfer_target{
        .kind = wh::adk::transfer_target_kind::child,
        .agent_name = std::string{target_agent_name},
    };
  }

  if (has_member(definition, target_agent_name)) {
    return wh::core::result<wh::adk::transfer_target>::failure(wh::core::errc::contract_violation);
  }

  return wh::core::result<wh::adk::transfer_target>::failure(wh::core::errc::not_found);
}

inline auto append_visible_history(runtime_state &state,
                                   const std::vector<wh::schema::message> &messages) -> void {
  state.visible_history.insert(state.visible_history.end(), messages.begin(), messages.end());
}

inline auto append_visible_history(runtime_state &state,
                                   std::vector<wh::schema::message> &&messages) -> void {
  state.visible_history.insert(state.visible_history.end(),
                               std::make_move_iterator(messages.begin()),
                               std::make_move_iterator(messages.end()));
}

[[nodiscard]] inline auto build_final_output(runtime_state &state, wh::agent::agent_output output,
                                             const std::string_view agent_name)
    -> wh::agent::agent_output {
  stamp_agent_message(output.final_message, agent_name);
  output.transfer.reset();
  output.history_messages.assign(state.visible_history.begin() +
                                     static_cast<std::ptrdiff_t>(state.input_message_count),
                                 state.visible_history.end());
  return output;
}

[[nodiscard]] inline auto read_state(wh::compose::graph_process_state &process_state)
    -> wh::core::result<std::reference_wrapper<runtime_state>> {
  return wh::adk::detail::shared_state_ref<runtime_state>(process_state);
}

[[nodiscard]] inline auto move_messages_payload(wh::compose::graph_value &payload)
    -> wh::core::result<std::vector<wh::schema::message>> {
  if (auto *typed = wh::core::any_cast<std::vector<wh::schema::message>>(&payload);
      typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<std::vector<wh::schema::message>>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto read_host_request(wh::compose::graph_value &payload)
    -> wh::core::result<std::reference_wrapper<host_request>> {
  if (auto *typed = wh::core::any_cast<host_request>(&payload); typed != nullptr) {
    return std::ref(*typed);
  }
  return wh::core::result<std::reference_wrapper<host_request>>::failure(
      wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto move_agent_output(wh::compose::graph_value &payload)
    -> wh::core::result<wh::agent::agent_output> {
  if (auto *typed = wh::core::any_cast<wh::agent::agent_output>(&payload); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<wh::agent::agent_output>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto make_bootstrap_options(const host_graph_definition &definition)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre([&definition](const wh::compose::graph_state_cause &,
                                       wh::compose::graph_process_state &process_state,
                                       wh::compose::graph_value &payload,
                                       wh::core::run_context &) -> wh::core::result<void> {
    auto messages = move_messages_payload(payload);
    if (messages.has_error()) {
      return wh::core::result<void>::failure(messages.error());
    }

    runtime_state state{};
    state.input_message_count = messages.value().size();
    state.visible_history = std::move(messages).value();
    state.active_agent_name = definition.root.name;
    state.transfer_states.emplace(definition.root.name, make_transfer_state(definition.root.name));
    for (const auto &child : definition.children) {
      state.transfer_states.emplace(child.name, make_transfer_state(child.name));
    }

    auto inserted =
        wh::adk::detail::emplace_shared_state<runtime_state>(process_state, std::move(state));
    if (inserted.has_error()) {
      return wh::core::result<void>::failure(inserted.error());
    }

    payload = wh::core::any(std::monostate{});
    return {};
  });
  return options;
}

[[nodiscard]] inline auto make_prepare_request_options() -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &process_state,
         wh::compose::graph_value &payload, wh::core::run_context &) -> wh::core::result<void> {
        auto state = read_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }

        auto &runtime_state = state.value().get();
        if (runtime_state.active_agent_name.empty()) {
          return wh::core::result<void>::failure(wh::core::errc::not_found);
        }

        auto request = wh::adk::rewrite_transfer_history(
            runtime_state.visible_history, runtime_state.active_agent_name,
            wh::adk::resolved_transfer_trim_options{
                .trim_assistant_transfer_message = true,
                .trim_tool_transfer_pair = true,
            });
        runtime_state.active_request_messages = request;
        payload = wh::core::any(host_request{
            .agent_name = runtime_state.active_agent_name,
            .messages = std::move(request),
        });
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_role_input_options(std::string agent_name)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre([agent_name = std::move(agent_name)](
                             const wh::compose::graph_state_cause &,
                             wh::compose::graph_process_state &, wh::compose::graph_value &payload,
                             wh::core::run_context &) -> wh::core::result<void> {
    auto request = read_host_request(payload);
    if (request.has_error()) {
      return wh::core::result<void>::failure(request.error());
    }
    if (request.value().get().agent_name != agent_name) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    payload = wh::core::any(std::move(request.value().get().messages));
    return {};
  });
  return options;
}

[[nodiscard]] inline auto make_capture_options(const host_graph_definition &definition)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post([&definition](const wh::compose::graph_state_cause &,
                                        wh::compose::graph_process_state &process_state,
                                        wh::compose::graph_value &payload,
                                        wh::core::run_context &) -> wh::core::result<void> {
    auto state = read_state(process_state);
    if (state.has_error()) {
      return wh::core::result<void>::failure(state.error());
    }
    auto &runtime_state = state.value().get();
    auto member = member_ref(definition, runtime_state.active_agent_name);
    if (member.has_error()) {
      return wh::core::result<void>::failure(member.error());
    }

    auto output = move_agent_output(payload);
    if (output.has_error()) {
      return wh::core::result<void>::failure(output.error());
    }

    auto emitted = normalize_history_delta(output.value(), runtime_state.active_request_messages,
                                           member.value().get().name);
    if (output.value().transfer.has_value()) {
      auto target = resolve_transfer_target(definition, member.value().get(),
                                            output.value().transfer->target_agent_name);
      if (target.has_error()) {
        return wh::core::result<void>::failure(target.error());
      }

      auto resolved_target = wh::adk::begin_deterministic_transfer(
          runtime_state.transfer_states[member.value().get().name],
          target.value().kind == wh::adk::transfer_target_kind::child
              ? target.value().agent_name
              : (target.value().kind == wh::adk::transfer_target_kind::parent
                     ? *member.value().get().parent_name
                     : member.value().get().name));
      if (resolved_target.has_error()) {
        return wh::core::result<void>::failure(resolved_target.error());
      }

      append_visible_history(
          runtime_state,
          filter_transfer_messages(std::move(emitted), output.value().transfer->tool_call_id));

      const auto original_size = runtime_state.visible_history.size();
      auto appended = wh::adk::append_transfer_messages_once(
          runtime_state.visible_history, runtime_state.transfer_states[member.value().get().name],
          resolved_target.value(), output.value().transfer->tool_call_id,
          wh::adk::transfer_completion_kind::normal);
      if (appended.has_error()) {
        return wh::core::result<void>::failure(appended.error());
      }
      if (runtime_state.visible_history.size() >= original_size + 2U) {
        stamp_agent_message(runtime_state.visible_history[original_size],
                            member.value().get().name);
        stamp_agent_message(runtime_state.visible_history[original_size + 1U],
                            member.value().get().name);
      }

      runtime_state.active_agent_name = resolved_target.value();
      runtime_state.final_output.reset();
      payload = wh::core::any(host_step_decision{
          .kind = host_step_kind::invoke_next,
      });
      return {};
    }

    append_visible_history(runtime_state, std::move(emitted));
    runtime_state.final_output =
        build_final_output(runtime_state, std::move(output).value(), member.value().get().name);
    payload = wh::core::any(host_step_decision{
        .kind = host_step_kind::emit_final,
    });
    return {};
  });
  return options;
}

[[nodiscard]] inline auto make_emit_final_options() -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &process_state,
         wh::compose::graph_value &payload, wh::core::run_context &) -> wh::core::result<void> {
        auto state = read_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &runtime_state = state.value().get();
        if (!runtime_state.final_output.has_value()) {
          return wh::core::result<void>::failure(wh::core::errc::not_found);
        }
        payload = wh::core::any(std::move(*runtime_state.final_output));
        return {};
      });
  return options;
}

[[nodiscard]] inline auto validate_member_graph(const host_member_definition &member)
    -> wh::core::result<void> {
  const auto &boundary = member.graph.boundary();
  if (boundary.input != wh::compose::node_contract::value ||
      boundary.output != wh::compose::node_contract::value) {
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  return {};
}

[[nodiscard]] inline auto lower_member_definition(wh::agent::agent &member)
    -> wh::core::result<host_member_definition> {
  auto graph = member.lower();
  if (graph.has_error()) {
    return wh::core::result<host_member_definition>::failure(graph.error());
  }

  host_member_definition definition{
      .name = std::string{member.name()},
      .description = std::string{member.description()},
      .parent_name = member.parent_name().has_value()
                         ? std::optional<std::string>{std::string{*member.parent_name()}}
                         : std::nullopt,
      .allow_transfer_to_parent = member.allows_transfer_to_parent(),
      .allowed_children = member.allowed_transfer_children(),
      .graph = std::move(graph).value(),
  };
  auto valid = validate_member_graph(definition);
  if (valid.has_error()) {
    return wh::core::result<host_member_definition>::failure(valid.error());
  }
  return definition;
}

template <typename children_t>
[[nodiscard]] inline auto build_definition(wh::agent::agent &root, children_t &children)
    -> wh::core::result<std::unique_ptr<host_graph_definition>> {
  auto root_definition = lower_member_definition(root);
  if (root_definition.has_error()) {
    return wh::core::result<std::unique_ptr<host_graph_definition>>::failure(
        root_definition.error());
  }

  auto definition = std::make_unique<host_graph_definition>();
  definition->root = std::move(root_definition).value();
  definition->root.parent_name.reset();
  definition->root.allow_transfer_to_parent = false;
  definition->root.allowed_children.clear();
  definition->children.reserve(children.size());
  for (auto &child : children) {
    auto lowered = lower_member_definition(child);
    if (lowered.has_error()) {
      return wh::core::result<std::unique_ptr<host_graph_definition>>::failure(lowered.error());
    }
    auto child_definition = std::move(lowered).value();
    child_definition.parent_name = definition->root.name;
    child_definition.allow_transfer_to_parent = true;
    child_definition.allowed_children.clear();
    definition->root.allowed_children.push_back(child_definition.name);
    definition->children.push_back(std::move(child_definition));
  }
  return definition;
}

[[nodiscard]] inline auto populate_exported_topology(const host_graph_definition &definition)
    -> wh::core::result<wh::agent::agent> {
  wh::agent::agent exported{definition.root.name};
  if (!definition.root.description.empty()) {
    auto described = exported.set_description(definition.root.description);
    if (described.has_error()) {
      return wh::core::result<wh::agent::agent>::failure(described.error());
    }
  }

  for (const auto &child : definition.children) {
    wh::agent::agent placeholder{child.name};
    if (!child.description.empty()) {
      auto described = placeholder.set_description(child.description);
      if (described.has_error()) {
        return wh::core::result<wh::agent::agent>::failure(described.error());
      }
    }
    if (child.allow_transfer_to_parent) {
      auto upward = placeholder.allow_transfer_to_parent();
      if (upward.has_error()) {
        return wh::core::result<wh::agent::agent>::failure(upward.error());
      }
    }
    auto added = exported.add_child(std::move(placeholder));
    if (added.has_error()) {
      return wh::core::result<wh::agent::agent>::failure(added.error());
    }
    auto routed = exported.allow_transfer_to_child(child.name);
    if (routed.has_error()) {
      return wh::core::result<wh::agent::agent>::failure(routed.error());
    }
  }

  return exported;
}

class host_graph {
public:
  explicit host_graph(const host_graph_definition &definition) noexcept
      : definition_(std::addressof(definition)) {}

  [[nodiscard]] auto lower() const -> wh::core::result<wh::compose::graph> {
    if (definition_ == nullptr) {
      return wh::core::result<wh::compose::graph>::failure(wh::core::errc::invalid_argument);
    }

    wh::compose::graph_compile_options compile_options{};
    compile_options.name = definition_->root.name;
    compile_options.mode = wh::compose::graph_runtime_mode::pregel;
    compile_options.max_parallel_nodes = 1U;
    compile_options.max_parallel_per_node = 1U;
    wh::compose::graph lowered{std::move(compile_options)};

    auto bootstrap = wh::compose::make_lambda_node(
        "bootstrap",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        make_bootstrap_options(*definition_));
    auto bootstrap_added = lowered.add_lambda(std::move(bootstrap));
    if (bootstrap_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(bootstrap_added.error());
    }

    auto prepare_request = wh::compose::make_lambda_node(
        "prepare_request",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        make_prepare_request_options());
    auto prepare_request_added = lowered.add_lambda(std::move(prepare_request));
    if (prepare_request_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(prepare_request_added.error());
    }

    const auto root_node_key = member_node_key(definition_->root.name);
    auto root_node = wh::compose::make_subgraph_node(
        root_node_key, definition_->root.graph, make_role_input_options(definition_->root.name));
    auto root_added = lowered.add_subgraph(std::move(root_node));
    if (root_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(root_added.error());
    }

    for (const auto &child : definition_->children) {
      const auto child_node_key = member_node_key(child.name);
      auto child_node = wh::compose::make_subgraph_node(child_node_key, child.graph,
                                                        make_role_input_options(child.name));
      auto child_added = lowered.add_subgraph(std::move(child_node));
      if (child_added.has_error()) {
        return wh::core::result<wh::compose::graph>::failure(child_added.error());
      }
    }

    auto capture_result = wh::compose::make_lambda_node(
        "capture_result",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        make_capture_options(*definition_));
    auto capture_added = lowered.add_lambda(std::move(capture_result));
    if (capture_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(capture_added.error());
    }

    auto emit_final = wh::compose::make_lambda_node(
        "emit_final",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        make_emit_final_options());
    auto emit_final_added = lowered.add_lambda(std::move(emit_final));
    if (emit_final_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(emit_final_added.error());
    }

    const auto add_edge = [&lowered](const std::string &from,
                                     const std::string &to) -> wh::core::result<void> {
      return lowered.add_edge(from, to);
    };
    auto start_edge = lowered.add_entry_edge("bootstrap");
    if (start_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(start_edge.error());
    }
    auto bootstrap_edge = add_edge(std::string{"bootstrap"}, std::string{"prepare_request"});
    if (bootstrap_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(bootstrap_edge.error());
    }
    for (const auto &child : definition_->children) {
      auto child_capture = add_edge(member_node_key(child.name), std::string{"capture_result"});
      if (child_capture.has_error()) {
        return wh::core::result<wh::compose::graph>::failure(child_capture.error());
      }
    }
    auto root_capture = add_edge(root_node_key, std::string{"capture_result"});
    if (root_capture.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(root_capture.error());
    }
    auto final_edge = lowered.add_exit_edge("emit_final");
    if (final_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(final_edge.error());
    }

    wh::compose::value_branch request_branch{};
    auto add_route_case = [&request_branch](const std::string &target) -> wh::core::result<void> {
      return request_branch.add_case(
          member_node_key(target),
          [target](const wh::compose::graph_value &payload,
                   wh::core::run_context &) -> wh::core::result<bool> {
            const auto *request = wh::core::any_cast<host_request>(&payload);
            if (request == nullptr) {
              return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
            }
            return request->agent_name == target;
          });
    };

    auto root_route = add_route_case(definition_->root.name);
    if (root_route.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(root_route.error());
    }
    for (const auto &child : definition_->children) {
      auto child_route = add_route_case(child.name);
      if (child_route.has_error()) {
        return wh::core::result<wh::compose::graph>::failure(child_route.error());
      }
    }
    auto request_branch_added = request_branch.apply(lowered, "prepare_request");
    if (request_branch_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(request_branch_added.error());
    }

    wh::compose::value_branch decision_branch{};
    auto invoke_case = decision_branch.add_case(
        "prepare_request",
        [](const wh::compose::graph_value &payload,
           wh::core::run_context &) -> wh::core::result<bool> {
          const auto *decision = wh::core::any_cast<host_step_decision>(&payload);
          if (decision == nullptr) {
            return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
          }
          return decision->kind == host_step_kind::invoke_next;
        });
    if (invoke_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(invoke_case.error());
    }
    auto finalize_case = decision_branch.add_case(
        "emit_final",
        [](const wh::compose::graph_value &payload,
           wh::core::run_context &) -> wh::core::result<bool> {
          const auto *decision = wh::core::any_cast<host_step_decision>(&payload);
          if (decision == nullptr) {
            return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
          }
          return decision->kind == host_step_kind::emit_final;
        });
    if (finalize_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(finalize_case.error());
    }
    auto decision_branch_added = decision_branch.apply(lowered, "capture_result");
    if (decision_branch_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(decision_branch_added.error());
    }

    auto compiled = lowered.compile();
    if (compiled.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(compiled.error());
    }

    return lowered;
  }

private:
  const host_graph_definition *definition_{nullptr};
};

template <typename children_t>
[[nodiscard]] inline auto bind_host_agent(wh::agent::agent &root, children_t &children)
    -> wh::core::result<wh::agent::agent> {
  if (!root.frozen()) {
    return wh::core::result<wh::agent::agent>::failure(wh::core::errc::contract_violation);
  }
  auto definition = build_definition(root, children);
  if (definition.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(definition.error());
  }

  auto exported = populate_exported_topology(*definition.value());
  if (exported.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(exported.error());
  }

  auto lowered_definition = std::move(definition).value();
  auto bound = exported.value().bind_execution(
      nullptr,
      [definition = std::move(lowered_definition)]() mutable
          -> wh::core::result<wh::compose::graph> { return host_graph{*definition}.lower(); });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  auto exported_value = std::move(exported).value();
  auto exported_frozen = exported_value.freeze();
  if (exported_frozen.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(exported_frozen.error());
  }
  return exported_value;
}

} // namespace host_graph_detail

/// Lowers one frozen supervisor shell into the executable agent surface by
/// materializing one shared host-mediated compose graph.
[[nodiscard]] inline auto bind_supervisor_agent(wh::agent::supervisor authored)
    -> wh::core::result<wh::agent::agent> {
  if (!authored.frozen()) {
    return wh::core::result<wh::agent::agent>::failure(wh::core::errc::contract_violation);
  }
  auto supervisor = authored.supervisor_agent();
  if (supervisor.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(supervisor.error());
  }
  return host_graph_detail::bind_host_agent(supervisor.value().get(), authored.workers());
}

/// Lowers one frozen swarm shell into the executable agent surface by
/// materializing one host-mediated peer graph.
[[nodiscard]] inline auto bind_swarm_agent(wh::agent::swarm authored)
    -> wh::core::result<wh::agent::agent> {
  if (!authored.frozen()) {
    return wh::core::result<wh::agent::agent>::failure(wh::core::errc::contract_violation);
  }
  auto host = authored.host_agent();
  if (host.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(host.error());
  }
  return host_graph_detail::bind_host_agent(host.value().get(), authored.peers());
}

/// Lowers one frozen research shell into the executable agent surface by
/// materializing one lead-and-specialist host graph.
[[nodiscard]] inline auto bind_research_agent(wh::agent::research authored)
    -> wh::core::result<wh::agent::agent> {
  if (!authored.frozen()) {
    return wh::core::result<wh::agent::agent>::failure(wh::core::errc::contract_violation);
  }
  auto lead = authored.lead();
  if (lead.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(lead.error());
  }
  return host_graph_detail::bind_host_agent(lead.value().get(), authored.specialists());
}

} // namespace wh::adk::detail
