// Defines the internal plan-execute graph lowerer that maps the public
// authored shell onto one compose graph without introducing a second runtime.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "wh/adk/detail/agent_graph_view.hpp"
#include "wh/adk/detail/shared_state.hpp"
#include "wh/adk/deterministic_transfer.hpp"
#include "wh/agent/agent.hpp"
#include "wh/agent/plan_execute.hpp"
#include "wh/compose/authored/value_branch.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/message.hpp"

namespace wh::adk::detail {

namespace plan_execute_detail {

struct runtime_state {
  std::vector<wh::schema::message> input_messages{};
  std::optional<wh::agent::plan_execute_plan> current_plan{};
  std::vector<wh::agent::plan_execute_executed_step> executed_steps{};
  std::size_t remaining_iterations{0U};
  std::optional<wh::agent::agent_output> final_output{};
};

struct replanner_result {
  wh::agent::plan_execute_decision decision{};
};

[[nodiscard]] inline auto role_request_node_key(const std::string_view role_name) -> std::string {
  return std::string{role_name} + "_request_adapter";
}

[[nodiscard]] inline auto role_result_node_key(const std::string_view role_name) -> std::string {
  return std::string{role_name} + "_result_adapter";
}

[[nodiscard]] inline auto read_state(wh::compose::graph_process_state &process_state)
    -> wh::core::result<std::reference_wrapper<runtime_state>> {
  return wh::adk::detail::shared_state_ref<runtime_state>(process_state);
}

[[nodiscard]] inline auto make_context(const runtime_state &state)
    -> wh::agent::plan_execute_context {
  return wh::agent::plan_execute_context{
      .input_messages = state.input_messages,
      .current_plan = state.current_plan,
      .executed_steps = state.executed_steps,
  };
}

[[nodiscard]] inline auto read_messages_payload(wh::compose::graph_value &payload)
    -> wh::core::result<std::vector<wh::schema::message>> {
  if (auto *typed = wh::core::any_cast<std::vector<wh::schema::message>>(&payload);
      typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<std::vector<wh::schema::message>>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto read_agent_output(const wh::compose::graph_value &payload)
    -> wh::core::result<std::reference_wrapper<const wh::agent::agent_output>> {
  if (const auto *typed = wh::core::any_cast<wh::agent::agent_output>(&payload); typed != nullptr) {
    return std::cref(*typed);
  }
  return wh::core::result<std::reference_wrapper<const wh::agent::agent_output>>::failure(
      wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto validate_role_graph(const wh::compose::graph &graph)
    -> wh::core::result<void> {
  const auto boundary = graph.boundary();
  if (boundary.input != wh::compose::node_contract::value &&
      boundary.input != wh::compose::node_contract::stream) {
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  if (boundary.output != wh::compose::node_contract::value &&
      boundary.output != wh::compose::node_contract::stream) {
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  return {};
}

[[nodiscard]] inline auto make_role_agent_output(std::vector<wh::schema::message> messages,
                                                 wh::core::run_context &)
    -> wh::core::result<wh::agent::agent_output> {
  if (messages.empty()) {
    return wh::core::result<wh::agent::agent_output>::failure(wh::core::errc::not_found);
  }
  auto final_message = messages.back();
  return wh::agent::agent_output{
      .final_message = final_message,
      .history_messages = std::move(messages),
      .transfer = wh::adk::extract_transfer_from_message(final_message),
  };
}

[[nodiscard]] inline auto add_role_request_adapter(wh::compose::graph &graph,
                                                   const std::string &role_name,
                                                   const wh::compose::node_contract input_contract)
    -> wh::core::result<void> {
  const auto node_key = role_request_node_key(role_name);
  if (input_contract == wh::compose::node_contract::value) {
    auto node = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                              wh::compose::node_contract::value>(
        node_key,
        [](wh::compose::graph_value &payload, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          auto messages = read_messages_payload(payload);
          if (messages.has_error()) {
            return wh::core::result<wh::compose::graph_value>::failure(messages.error());
          }
          return wh::compose::graph_value{std::move(messages).value()};
        });
    return graph.add_lambda(std::move(node));
  }

  auto node = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                            wh::compose::node_contract::stream>(
      node_key,
      [](wh::compose::graph_value &payload, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_stream_reader> {
        auto messages = read_messages_payload(payload);
        if (messages.has_error()) {
          return wh::core::result<wh::compose::graph_stream_reader>::failure(messages.error());
        }
        return wh::adk::detail::make_message_stream_reader(std::move(messages).value());
      });
  return graph.add_lambda(std::move(node));
}

[[nodiscard]] inline auto add_role_result_adapter(wh::compose::graph &graph,
                                                  const std::string &role_name,
                                                  const wh::compose::node_contract)
    -> wh::core::result<void> {
  const auto node_key = role_result_node_key(role_name);
  const auto build_output =
      wh::adk::detail::agent_output_from_messages{plan_execute_detail::make_role_agent_output};
  auto node = wh::compose::make_lambda_node<wh::compose::node_contract::value,
                                            wh::compose::node_contract::value>(
      node_key,
      [build_output](
          wh::compose::graph_value &payload, wh::core::run_context &context,
          const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
        return wh::adk::detail::make_agent_output_value(payload, build_output, context);
      });
  return graph.add_lambda(std::move(node));
}

[[nodiscard]] inline auto make_bootstrap_options(const std::size_t max_iterations)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre([max_iterations](const wh::compose::graph_state_cause &,
                                          wh::compose::graph_process_state &process_state,
                                          wh::compose::graph_value &payload,
                                          wh::core::run_context &) -> wh::core::result<void> {
    auto messages = read_messages_payload(payload);
    if (messages.has_error()) {
      return wh::core::result<void>::failure(messages.error());
    }
    auto inserted = wh::adk::detail::emplace_shared_state<runtime_state>(
        process_state, runtime_state{
                           .input_messages = std::move(messages).value(),
                           .remaining_iterations = max_iterations,
                       });
    if (inserted.has_error()) {
      return wh::core::result<void>::failure(inserted.error());
    }
    payload = wh::core::any(std::monostate{});
    return {};
  });
  return options;
}

[[nodiscard]] inline auto make_request_options(wh::agent::plan_execute_request_builder builder,
                                               const bool consume_iteration = false)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre([builder = std::move(builder), consume_iteration](
                             const wh::compose::graph_state_cause &,
                             wh::compose::graph_process_state &process_state,
                             wh::compose::graph_value &payload,
                             wh::core::run_context &context) -> wh::core::result<void> {
    auto state = read_state(process_state);
    if (state.has_error()) {
      return wh::core::result<void>::failure(state.error());
    }
    auto &runtime_state = state.value().get();
    if (consume_iteration) {
      if (runtime_state.remaining_iterations == 0U) {
        return wh::core::result<void>::failure(wh::core::errc::resource_exhausted);
      }
      runtime_state.remaining_iterations -= 1U;
    }
    auto request = builder(make_context(runtime_state), context);
    if (request.has_error()) {
      return wh::core::result<void>::failure(request.error());
    }
    payload = wh::core::any(std::move(request).value());
    return {};
  });
  return options;
}

[[nodiscard]] inline auto make_parse_plan_options(wh::agent::plan_execute_plan_reader reader)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [reader = std::move(reader)](const wh::compose::graph_state_cause &,
                                   wh::compose::graph_process_state &process_state,
                                   wh::compose::graph_value &payload,
                                   wh::core::run_context &context) -> wh::core::result<void> {
        auto state = read_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto output = read_agent_output(payload);
        if (output.has_error()) {
          return wh::core::result<void>::failure(output.error());
        }
        auto parsed = reader(output.value().get(), context);
        if (parsed.has_error()) {
          return wh::core::result<void>::failure(parsed.error());
        }
        if (parsed.value().steps.empty()) {
          return wh::core::result<void>::failure(wh::core::errc::contract_violation);
        }
        state.value().get().current_plan = std::move(parsed).value();
        payload = wh::core::any(std::monostate{});
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_capture_step_options(wh::agent::plan_execute_step_reader reader)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [reader = std::move(reader)](const wh::compose::graph_state_cause &,
                                   wh::compose::graph_process_state &process_state,
                                   wh::compose::graph_value &payload,
                                   wh::core::run_context &context) -> wh::core::result<void> {
        auto state = read_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &runtime_state = state.value().get();
        if (!runtime_state.current_plan.has_value() || runtime_state.current_plan->steps.empty()) {
          return wh::core::result<void>::failure(wh::core::errc::contract_violation);
        }

        auto output = read_agent_output(payload);
        if (output.has_error()) {
          return wh::core::result<void>::failure(output.error());
        }
        auto parsed = reader(output.value().get(), context);
        if (parsed.has_error()) {
          return wh::core::result<void>::failure(parsed.error());
        }

        runtime_state.executed_steps.push_back(wh::agent::plan_execute_executed_step{
            .step = runtime_state.current_plan->steps.front(),
            .result = std::move(parsed).value(),
        });
        runtime_state.current_plan->steps.erase(runtime_state.current_plan->steps.begin());
        payload = wh::core::any(std::monostate{});
        return {};
      });
  return options;
}

[[nodiscard]] inline auto
make_parse_replanner_options(wh::agent::plan_execute_decision_reader reader, std::string output_key)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post([reader = std::move(reader), output_key = std::move(output_key)](
                              const wh::compose::graph_state_cause &,
                              wh::compose::graph_process_state &process_state,
                              wh::compose::graph_value &payload,
                              wh::core::run_context &context) -> wh::core::result<void> {
    auto state = read_state(process_state);
    if (state.has_error()) {
      return wh::core::result<void>::failure(state.error());
    }
    auto &runtime_state = state.value().get();
    auto output = read_agent_output(payload);
    if (output.has_error()) {
      return wh::core::result<void>::failure(output.error());
    }
    auto parsed = reader(output.value().get(), context);
    if (parsed.has_error()) {
      return wh::core::result<void>::failure(parsed.error());
    }

    auto decision = std::move(parsed).value();
    if (decision.kind == wh::agent::plan_execute_decision_kind::plan) {
      if (decision.next_plan.steps.empty()) {
        return wh::core::result<void>::failure(wh::core::errc::contract_violation);
      }
      runtime_state.current_plan = decision.next_plan;
    } else {
      wh::agent::agent_output final_output{};
      final_output.final_message = decision.response;
      final_output.history_messages.push_back(decision.response);
      final_output.transfer = wh::adk::extract_transfer_from_message(final_output.final_message);
      if (!output_key.empty()) {
        final_output.output_values.insert_or_assign(output_key, wh::core::any{decision.response});
      }
      runtime_state.final_output = final_output;
    }
    payload = wh::core::any(replanner_result{
        .decision = std::move(decision),
    });
    return {};
  });
  return options;
}

[[nodiscard]] inline auto make_emit_final_options() -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [](const wh::compose::graph_state_cause &, wh::compose::graph_process_state &process_state,
         wh::compose::graph_value &payload, wh::core::run_context &) -> wh::core::result<void> {
        auto state = read_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        const auto &runtime_state = state.value().get();
        if (!runtime_state.final_output.has_value()) {
          return wh::core::result<void>::failure(wh::core::errc::not_found);
        }
        payload = wh::core::any(*runtime_state.final_output);
        return {};
      });
  return options;
}

} // namespace plan_execute_detail

/// Internal plan-execute lowering shell that produces one real compose graph.
class plan_execute_graph {
public:
  explicit plan_execute_graph(wh::agent::plan_execute &authored) noexcept
      : authored_(std::addressof(authored)) {}

  [[nodiscard]] auto lower() -> wh::core::result<wh::compose::graph> {
    if (authored_ == nullptr) {
      return wh::core::result<wh::compose::graph>::failure(wh::core::errc::invalid_argument);
    }

    auto planner = authored_->planner();
    if (planner.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner.error());
    }
    auto executor = authored_->executor();
    if (executor.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(executor.error());
    }
    auto replanner = authored_->effective_replanner();
    if (replanner.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner.error());
    }

    auto planner_native = planner.value().get().lower();
    if (planner_native.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner_native.error());
    }
    auto planner_graph = std::move(planner_native).value();
    auto planner_valid = plan_execute_detail::validate_role_graph(planner_graph);
    if (planner_valid.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner_valid.error());
    }
    auto executor_native = executor.value().get().lower();
    if (executor_native.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(executor_native.error());
    }
    auto executor_graph = std::move(executor_native).value();
    auto executor_valid = plan_execute_detail::validate_role_graph(executor_graph);
    if (executor_valid.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(executor_valid.error());
    }
    auto replanner_native = replanner.value().get().lower();
    if (replanner_native.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_native.error());
    }
    auto replanner_graph = std::move(replanner_native).value();
    auto replanner_valid = plan_execute_detail::validate_role_graph(replanner_graph);
    if (replanner_valid.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_valid.error());
    }

    wh::compose::graph_compile_options compile_options{};
    compile_options.name = std::string{authored_->name()};
    compile_options.mode = wh::compose::graph_runtime_mode::pregel;
    compile_options.max_steps = authored_->max_iterations() * 10U + 8U;
    compile_options.max_parallel_nodes = 1U;
    compile_options.max_parallel_per_node = 1U;
    wh::compose::graph lowered{std::move(compile_options)};

    auto bootstrap = wh::compose::make_lambda_node(
        "bootstrap",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        plan_execute_detail::make_bootstrap_options(authored_->max_iterations()));
    auto bootstrap_added = lowered.add_lambda(std::move(bootstrap));
    if (bootstrap_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(bootstrap_added.error());
    }

    auto plan_request = wh::compose::make_lambda_node(
        "plan_request",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        plan_execute_detail::make_request_options(authored_->planner_request_builder(), false));
    auto plan_request_added = lowered.add_lambda(std::move(plan_request));
    if (plan_request_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(plan_request_added.error());
    }

    const auto planner_boundary = planner_graph.boundary();
    auto planner_request_adapter = plan_execute_detail::add_role_request_adapter(
        lowered, std::string{"planner"}, planner_boundary.input);
    if (planner_request_adapter.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner_request_adapter.error());
    }
    auto planner_node = wh::compose::make_subgraph_node("planner", std::move(planner_graph));
    auto planner_added = lowered.add_subgraph(std::move(planner_node));
    if (planner_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner_added.error());
    }
    auto planner_result_adapter = plan_execute_detail::add_role_result_adapter(
        lowered, std::string{"planner"}, planner_boundary.output);
    if (planner_result_adapter.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner_result_adapter.error());
    }

    auto parse_plan = wh::compose::make_lambda_node(
        "parse_plan",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        plan_execute_detail::make_parse_plan_options(authored_->planner_plan_reader()));
    auto parse_plan_added = lowered.add_lambda(std::move(parse_plan));
    if (parse_plan_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(parse_plan_added.error());
    }

    auto execute_request = wh::compose::make_lambda_node(
        "execute_request",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        plan_execute_detail::make_request_options(authored_->executor_request_builder(), true));
    auto execute_request_added = lowered.add_lambda(std::move(execute_request));
    if (execute_request_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(execute_request_added.error());
    }

    const auto executor_boundary = executor_graph.boundary();
    auto executor_request_adapter = plan_execute_detail::add_role_request_adapter(
        lowered, std::string{"executor"}, executor_boundary.input);
    if (executor_request_adapter.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(executor_request_adapter.error());
    }
    auto executor_node = wh::compose::make_subgraph_node("executor", std::move(executor_graph));
    auto executor_added = lowered.add_subgraph(std::move(executor_node));
    if (executor_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(executor_added.error());
    }
    auto executor_result_adapter = plan_execute_detail::add_role_result_adapter(
        lowered, std::string{"executor"}, executor_boundary.output);
    if (executor_result_adapter.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(executor_result_adapter.error());
    }

    auto capture_step = wh::compose::make_lambda_node(
        "capture_step",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        plan_execute_detail::make_capture_step_options(authored_->executor_step_reader()));
    auto capture_step_added = lowered.add_lambda(std::move(capture_step));
    if (capture_step_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(capture_step_added.error());
    }

    auto replan_request = wh::compose::make_lambda_node(
        "replan_request",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        plan_execute_detail::make_request_options(authored_->replanner_request_builder(), false));
    auto replan_request_added = lowered.add_lambda(std::move(replan_request));
    if (replan_request_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replan_request_added.error());
    }

    const auto replanner_boundary = replanner_graph.boundary();
    auto replanner_request_adapter = plan_execute_detail::add_role_request_adapter(
        lowered, std::string{"replanner"}, replanner_boundary.input);
    if (replanner_request_adapter.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_request_adapter.error());
    }
    auto replanner_node = wh::compose::make_subgraph_node("replanner", std::move(replanner_graph));
    auto replanner_added = lowered.add_subgraph(std::move(replanner_node));
    if (replanner_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_added.error());
    }
    auto replanner_result_adapter = plan_execute_detail::add_role_result_adapter(
        lowered, std::string{"replanner"}, replanner_boundary.output);
    if (replanner_result_adapter.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_result_adapter.error());
    }

    auto parse_replanner = wh::compose::make_lambda_node(
        "parse_replanner",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        plan_execute_detail::make_parse_replanner_options(authored_->replanner_decision_reader(),
                                                          std::string{authored_->output_key()}));
    auto parse_replanner_added = lowered.add_lambda(std::move(parse_replanner));
    if (parse_replanner_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(parse_replanner_added.error());
    }

    auto emit_final = wh::compose::make_lambda_node(
        "emit_final",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &) -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        plan_execute_detail::make_emit_final_options());
    auto emit_final_added = lowered.add_lambda(std::move(emit_final));
    if (emit_final_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(emit_final_added.error());
    }

    const auto add_edge = [&lowered](const char *from, const char *to) -> wh::core::result<void> {
      return lowered.add_edge(from, to);
    };
    auto start_edge = lowered.add_entry_edge("bootstrap");
    if (start_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(start_edge.error());
    }
    auto bootstrap_edge = add_edge("bootstrap", "plan_request");
    if (bootstrap_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(bootstrap_edge.error());
    }
    auto planner_request_edge = add_edge("plan_request", "planner_request_adapter");
    if (planner_request_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner_request_edge.error());
    }
    auto planner_role_edge = add_edge("planner_request_adapter", "planner");
    if (planner_role_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner_role_edge.error());
    }
    auto planner_parse_edge = add_edge("planner_result_adapter", "parse_plan");
    if (planner_parse_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner_parse_edge.error());
    }
    auto planner_result_edge = add_edge("planner", "planner_result_adapter");
    if (planner_result_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(planner_result_edge.error());
    }
    auto execute_request_edge = add_edge("execute_request", "executor_request_adapter");
    if (execute_request_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(execute_request_edge.error());
    }
    auto executor_role_edge = add_edge("executor_request_adapter", "executor");
    if (executor_role_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(executor_role_edge.error());
    }
    auto executor_capture_edge = add_edge("executor_result_adapter", "capture_step");
    if (executor_capture_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(executor_capture_edge.error());
    }
    auto executor_result_edge = add_edge("executor", "executor_result_adapter");
    if (executor_result_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(executor_result_edge.error());
    }
    auto capture_replan_edge = add_edge("capture_step", "replan_request");
    if (capture_replan_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(capture_replan_edge.error());
    }
    auto replanner_request_edge = add_edge("replan_request", "replanner_request_adapter");
    if (replanner_request_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_request_edge.error());
    }
    auto replanner_role_edge = add_edge("replanner_request_adapter", "replanner");
    if (replanner_role_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_role_edge.error());
    }
    auto replanner_parse_edge = add_edge("replanner_result_adapter", "parse_replanner");
    if (replanner_parse_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_parse_edge.error());
    }
    auto replanner_result_edge = add_edge("replanner", "replanner_result_adapter");
    if (replanner_result_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_result_edge.error());
    }
    auto emit_final_edge = lowered.add_exit_edge("emit_final");
    if (emit_final_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(emit_final_edge.error());
    }

    auto parse_plan_route = lowered.add_edge("parse_plan", "execute_request");
    if (parse_plan_route.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(parse_plan_route.error());
    }

    wh::compose::value_branch replanner_branch{};
    auto continue_case = replanner_branch.add_case(
        "execute_request",
        [](const wh::compose::graph_value &payload,
           wh::core::run_context &) -> wh::core::result<bool> {
          const auto *typed = wh::core::any_cast<plan_execute_detail::replanner_result>(&payload);
          if (typed == nullptr) {
            return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
          }
          return typed->decision.kind == wh::agent::plan_execute_decision_kind::plan;
        });
    if (continue_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(continue_case.error());
    }
    auto respond_case = replanner_branch.add_case(
        "emit_final",
        [](const wh::compose::graph_value &payload,
           wh::core::run_context &) -> wh::core::result<bool> {
          const auto *typed = wh::core::any_cast<plan_execute_detail::replanner_result>(&payload);
          if (typed == nullptr) {
            return wh::core::result<bool>::failure(wh::core::errc::type_mismatch);
          }
          return typed->decision.kind == wh::agent::plan_execute_decision_kind::respond;
        });
    if (respond_case.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(respond_case.error());
    }
    auto replanner_branch_added = replanner_branch.apply(lowered, "parse_replanner");
    if (replanner_branch_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(replanner_branch_added.error());
    }

    return lowered;
  }

private:
  const wh::agent::plan_execute *authored_{nullptr};
};

/// Wraps one frozen plan-execute shell as the common executable agent surface.
[[nodiscard]] inline auto bind_plan_execute_agent(wh::agent::plan_execute authored)
    -> wh::core::result<wh::agent::agent> {
  if (!authored.frozen()) {
    return wh::core::result<wh::agent::agent>::failure(wh::core::errc::contract_violation);
  }

  wh::agent::agent exported{std::string{authored.name()}};
  auto shell = std::make_unique<wh::agent::plan_execute>(std::move(authored));
  auto bound = exported.bind_execution(
      nullptr, [shell = std::move(shell)]() mutable -> wh::core::result<wh::compose::graph> {
        return plan_execute_graph{*shell}.lower();
      });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  auto exported_frozen = exported.freeze();
  if (exported_frozen.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(exported_frozen.error());
  }
  return exported;
}

} // namespace wh::adk::detail
