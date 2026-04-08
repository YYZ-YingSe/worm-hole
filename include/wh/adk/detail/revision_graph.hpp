// Defines the shared revision-loop graph lowerer used by self-refine,
// reviewer-executor, and reflexion authored shells.
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wh/adk/detail/shared_state.hpp"
#include "wh/agent/agent.hpp"
#include "wh/agent/reflexion.hpp"
#include "wh/agent/reviewer_executor.hpp"
#include "wh/agent/self_refine.hpp"
#include "wh/compose/authored/value_branch.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/add_node_options.hpp"
#include "wh/compose/node/lambda.hpp"
#include "wh/compose/node/subgraph.hpp"
#include "wh/core/any.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/message.hpp"

namespace wh::adk::detail {

namespace revision_detail {

struct runtime_state {
  std::vector<wh::schema::message> input_messages{};
  std::vector<wh::agent::agent_output> draft_history{};
  std::optional<wh::agent::agent_output> current_draft{};
  std::vector<wh::agent::agent_output> review_history{};
  std::optional<wh::agent::agent_output> current_review{};
  std::size_t remaining_iterations{0U};
};

struct review_result {
  wh::agent::review_decision decision{};
};

struct graph_config {
  std::string graph_name{};
  std::size_t max_iterations{1U};
  wh::agent::revision_request_builder draft_request_builder{nullptr};
  wh::agent::revision_request_builder review_request_builder{nullptr};
  wh::agent::review_decision_reader review_decision_reader{nullptr};
  wh::compose::graph draft_graph{};
  wh::compose::graph review_graph{};
  std::optional<wh::agent::revision_request_builder> memory_request_builder{};
  std::optional<wh::compose::graph> memory_graph{};
};

[[nodiscard]] inline auto read_state(
    wh::compose::graph_process_state &process_state)
    -> wh::core::result<std::reference_wrapper<runtime_state>> {
  return wh::adk::detail::shared_state_ref<runtime_state>(process_state);
}

[[nodiscard]] inline auto make_context(const runtime_state &state)
    -> wh::agent::revision_context {
  return wh::agent::revision_context{
      .input_messages = state.input_messages,
      .draft_history = state.draft_history,
      .current_draft =
          state.current_draft.has_value() ? std::addressof(*state.current_draft)
                                          : nullptr,
      .review_history = state.review_history,
      .current_review =
          state.current_review.has_value() ? std::addressof(*state.current_review)
                                           : nullptr,
      .remaining_iterations = state.remaining_iterations,
  };
}

[[nodiscard]] inline auto read_messages_payload(wh::compose::graph_value &payload)
    -> wh::core::result<std::vector<wh::schema::message>> {
  if (auto *typed =
          wh::core::any_cast<std::vector<wh::schema::message>>(&payload);
      typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<std::vector<wh::schema::message>>::failure(
      wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto move_agent_output(wh::compose::graph_value &payload)
    -> wh::core::result<wh::agent::agent_output> {
  if (auto *typed = wh::core::any_cast<wh::agent::agent_output>(&payload);
      typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<wh::agent::agent_output>::failure(
      wh::core::errc::type_mismatch);
}

[[nodiscard]] inline auto make_bootstrap_options(const std::size_t max_iterations)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre(
      [max_iterations](const wh::compose::graph_state_cause &,
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

[[nodiscard]] inline auto make_request_options(
    wh::agent::revision_request_builder builder, const bool consume_iteration)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_pre(
      [builder = std::move(builder), consume_iteration](
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
            return wh::core::result<void>::failure(
                wh::core::errc::resource_exhausted);
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

[[nodiscard]] inline auto make_capture_draft_options()
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [](const wh::compose::graph_state_cause &,
         wh::compose::graph_process_state &process_state,
         wh::compose::graph_value &payload,
         wh::core::run_context &) -> wh::core::result<void> {
        auto state = read_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto draft = move_agent_output(payload);
        if (draft.has_error()) {
          return wh::core::result<void>::failure(draft.error());
        }
        auto &runtime_state = state.value().get();
        if (runtime_state.current_draft.has_value()) {
          runtime_state.draft_history.push_back(
              std::move(*runtime_state.current_draft));
        }
        runtime_state.current_draft = std::move(draft).value();
        payload = wh::core::any(std::monostate{});
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_parse_review_options(
    wh::agent::review_decision_reader reader)
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [reader = std::move(reader)](const wh::compose::graph_state_cause &,
                                   wh::compose::graph_process_state &process_state,
                                   wh::compose::graph_value &payload,
                                   wh::core::run_context &context)
          -> wh::core::result<void> {
        auto state = read_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto review = move_agent_output(payload);
        if (review.has_error()) {
          return wh::core::result<void>::failure(review.error());
        }
        auto &runtime_state = state.value().get();
        if (runtime_state.current_review.has_value()) {
          runtime_state.review_history.push_back(
              std::move(*runtime_state.current_review));
        }
        runtime_state.current_review = std::move(review).value();
        auto decision = reader(*runtime_state.current_review, context);
        if (decision.has_error()) {
          return wh::core::result<void>::failure(decision.error());
        }
        payload = wh::core::any(review_result{
            .decision = std::move(decision).value(),
        });
        return {};
      });
  return options;
}

[[nodiscard]] inline auto make_emit_final_options()
    -> wh::compose::graph_add_node_options {
  wh::compose::graph_add_node_options options{};
  options.state.bind_post(
      [](const wh::compose::graph_state_cause &,
         wh::compose::graph_process_state &process_state,
         wh::compose::graph_value &payload,
         wh::core::run_context &) -> wh::core::result<void> {
        auto state = read_state(process_state);
        if (state.has_error()) {
          return wh::core::result<void>::failure(state.error());
        }
        auto &runtime_state = state.value().get();
        if (!runtime_state.current_draft.has_value()) {
          return wh::core::result<void>::failure(wh::core::errc::not_found);
        }
        payload = wh::core::any(std::move(*runtime_state.current_draft));
        return {};
      });
  return options;
}

[[nodiscard]] inline auto lower_graph(graph_config config)
    -> wh::core::result<wh::compose::graph> {
  const bool has_memory =
      config.memory_request_builder.has_value() && config.memory_graph.has_value();

  wh::compose::graph_compile_options compile_options{};
  compile_options.name = std::move(config.graph_name);
  compile_options.mode = wh::compose::graph_runtime_mode::pregel;
  compile_options.max_steps =
      config.max_iterations * (has_memory ? 5U : 4U) + 4U;
  compile_options.max_parallel_nodes = 1U;
  compile_options.max_parallel_per_node = 1U;
  wh::compose::graph lowered{std::move(compile_options)};

  auto bootstrap = wh::compose::make_lambda_node(
      "bootstrap",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      },
      make_bootstrap_options(config.max_iterations));
  auto bootstrap_added = lowered.add_lambda(std::move(bootstrap));
  if (bootstrap_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(
        bootstrap_added.error());
  }

  auto draft_request = wh::compose::make_lambda_node(
      "draft_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      },
      make_request_options(std::move(config.draft_request_builder), true));
  auto draft_request_added = lowered.add_lambda(std::move(draft_request));
  if (draft_request_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(
        draft_request_added.error());
  }

  auto draft_node =
      wh::compose::make_subgraph_node("draft", std::move(config.draft_graph));
  auto draft_added = lowered.add_subgraph(std::move(draft_node));
  if (draft_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(draft_added.error());
  }

  auto capture_draft = wh::compose::make_lambda_node(
      "capture_draft",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      },
      make_capture_draft_options());
  auto capture_draft_added = lowered.add_lambda(std::move(capture_draft));
  if (capture_draft_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(
        capture_draft_added.error());
  }

  auto review_request = wh::compose::make_lambda_node(
      "review_request",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      },
      make_request_options(std::move(config.review_request_builder), false));
  auto review_request_added = lowered.add_lambda(std::move(review_request));
  if (review_request_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(
        review_request_added.error());
  }

  auto review_node =
      wh::compose::make_subgraph_node("review", std::move(config.review_graph));
  auto review_added = lowered.add_subgraph(std::move(review_node));
  if (review_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(review_added.error());
  }

  auto parse_review = wh::compose::make_lambda_node(
      "parse_review",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      },
      make_parse_review_options(std::move(config.review_decision_reader)));
  auto parse_review_added = lowered.add_lambda(std::move(parse_review));
  if (parse_review_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(
        parse_review_added.error());
  }

  if (has_memory) {
    auto memory_request = wh::compose::make_lambda_node(
        "memory_request",
        [](wh::compose::graph_value &input, wh::core::run_context &,
           const wh::compose::graph_call_scope &)
            -> wh::core::result<wh::compose::graph_value> {
          return std::move(input);
        },
        make_request_options(std::move(*config.memory_request_builder), false));
    auto memory_request_added = lowered.add_lambda(std::move(memory_request));
    if (memory_request_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          memory_request_added.error());
    }

    auto memory_node = wh::compose::make_subgraph_node(
        "memory", std::move(*config.memory_graph));
    auto memory_added = lowered.add_subgraph(std::move(memory_node));
    if (memory_added.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(memory_added.error());
    }
  }

  auto emit_final = wh::compose::make_lambda_node(
      "emit_final",
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return std::move(input);
      },
      make_emit_final_options());
  auto emit_final_added = lowered.add_lambda(std::move(emit_final));
  if (emit_final_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(
        emit_final_added.error());
  }

  const auto add_edge = [&lowered](const char *from,
                                   const char *to) -> wh::core::result<void> {
    return lowered.add_edge(from, to);
  };
  auto start_edge = lowered.add_entry_edge("bootstrap");
  if (start_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(start_edge.error());
  }
  auto bootstrap_edge = add_edge("bootstrap", "draft_request");
  if (bootstrap_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(
        bootstrap_edge.error());
  }
  auto draft_edge = add_edge("draft_request", "draft");
  if (draft_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(draft_edge.error());
  }
  auto capture_edge = add_edge("draft", "capture_draft");
  if (capture_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(capture_edge.error());
  }
  auto review_request_edge = add_edge("capture_draft", "review_request");
  if (review_request_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(
        review_request_edge.error());
  }
  auto review_edge = add_edge("review_request", "review");
  if (review_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(review_edge.error());
  }
  auto parse_edge = add_edge("review", "parse_review");
  if (parse_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(parse_edge.error());
  }
  if (has_memory) {
    auto memory_request_edge = add_edge("memory_request", "memory");
    if (memory_request_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          memory_request_edge.error());
    }
    auto memory_loop_edge = add_edge("memory", "draft_request");
    if (memory_loop_edge.has_error()) {
      return wh::core::result<wh::compose::graph>::failure(
          memory_loop_edge.error());
    }
  }
  auto final_edge = lowered.add_exit_edge("emit_final");
  if (final_edge.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(final_edge.error());
  }

  wh::compose::value_branch review_branch{};
  auto accept_case = review_branch.add_case(
      "emit_final",
      [](const wh::compose::graph_value &payload, wh::core::run_context &)
          -> wh::core::result<bool> {
        const auto *typed =
            wh::core::any_cast<revision_detail::review_result>(&payload);
        if (typed == nullptr) {
          return wh::core::result<bool>::failure(
              wh::core::errc::type_mismatch);
        }
        return typed->decision.kind == wh::agent::review_decision_kind::accept;
      });
  if (accept_case.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(accept_case.error());
  }

  auto revise_case = review_branch.add_case(
      has_memory ? "memory_request" : "draft_request",
      [](const wh::compose::graph_value &payload, wh::core::run_context &)
          -> wh::core::result<bool> {
        const auto *typed =
            wh::core::any_cast<revision_detail::review_result>(&payload);
        if (typed == nullptr) {
          return wh::core::result<bool>::failure(
              wh::core::errc::type_mismatch);
        }
        return typed->decision.kind == wh::agent::review_decision_kind::revise;
      });
  if (revise_case.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(revise_case.error());
  }

  auto branch_added = review_branch.apply(lowered, "parse_review");
  if (branch_added.has_error()) {
    return wh::core::result<wh::compose::graph>::failure(branch_added.error());
  }

  return lowered;
}

} // namespace revision_detail

[[nodiscard]] inline auto bind_self_refine_agent(wh::agent::self_refine authored)
    -> wh::core::result<wh::agent::agent> {
  auto frozen = authored.freeze();
  if (frozen.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(frozen.error());
  }

  wh::agent::agent exported{std::string{authored.name()}};
  auto shell = std::make_unique<wh::agent::self_refine>(std::move(authored));
  auto bound = exported.bind_execution(
      nullptr,
      [shell = std::move(shell)]() mutable
          -> wh::core::result<wh::compose::graph> {
        auto worker = shell->worker();
        if (worker.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(worker.error());
        }
        auto reviewer = shell->effective_reviewer();
        if (reviewer.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(
              reviewer.error());
        }
        auto worker_graph = worker.value().get().lower_graph();
        if (worker_graph.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(
              worker_graph.error());
        }
        auto reviewer_graph = reviewer.value().get().lower_graph();
        if (reviewer_graph.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(
              reviewer_graph.error());
        }
        return revision_detail::lower_graph(revision_detail::graph_config{
            .graph_name = std::string{shell->name()},
            .max_iterations = shell->max_iterations(),
            .draft_request_builder = shell->worker_request_builder(),
            .review_request_builder = shell->reviewer_request_builder(),
            .review_decision_reader = shell->review_decision_reader(),
            .draft_graph = std::move(worker_graph).value(),
            .review_graph = std::move(reviewer_graph).value(),
        });
      });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  return exported;
}

[[nodiscard]] inline auto bind_reviewer_executor_agent(
    wh::agent::reviewer_executor authored)
    -> wh::core::result<wh::agent::agent> {
  auto frozen = authored.freeze();
  if (frozen.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(frozen.error());
  }

  wh::agent::agent exported{std::string{authored.name()}};
  auto shell =
      std::make_unique<wh::agent::reviewer_executor>(std::move(authored));
  auto bound = exported.bind_execution(
      nullptr,
      [shell = std::move(shell)]() mutable
          -> wh::core::result<wh::compose::graph> {
        auto executor = shell->executor();
        if (executor.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(
              executor.error());
        }
        auto reviewer = shell->reviewer();
        if (reviewer.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(
              reviewer.error());
        }
        auto executor_graph = executor.value().get().lower_graph();
        if (executor_graph.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(
              executor_graph.error());
        }
        auto reviewer_graph = reviewer.value().get().lower_graph();
        if (reviewer_graph.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(
              reviewer_graph.error());
        }
        return revision_detail::lower_graph(revision_detail::graph_config{
            .graph_name = std::string{shell->name()},
            .max_iterations = shell->max_iterations(),
            .draft_request_builder = shell->executor_request_builder(),
            .review_request_builder = shell->reviewer_request_builder(),
            .review_decision_reader = shell->review_decision_reader(),
            .draft_graph = std::move(executor_graph).value(),
            .review_graph = std::move(reviewer_graph).value(),
        });
      });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  return exported;
}

[[nodiscard]] inline auto bind_reflexion_agent(wh::agent::reflexion authored)
    -> wh::core::result<wh::agent::agent> {
  auto frozen = authored.freeze();
  if (frozen.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(frozen.error());
  }

  wh::agent::agent exported{std::string{authored.name()}};
  auto shell = std::make_unique<wh::agent::reflexion>(std::move(authored));
  auto bound = exported.bind_execution(
      nullptr,
      [shell = std::move(shell)]() mutable
          -> wh::core::result<wh::compose::graph> {
        auto actor = shell->actor();
        if (actor.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(actor.error());
        }
        auto critic = shell->critic();
        if (critic.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(critic.error());
        }
        auto actor_graph = actor.value().get().lower_graph();
        if (actor_graph.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(
              actor_graph.error());
        }
        auto critic_graph = critic.value().get().lower_graph();
        if (critic_graph.has_error()) {
          return wh::core::result<wh::compose::graph>::failure(
              critic_graph.error());
        }

        revision_detail::graph_config config{
            .graph_name = std::string{shell->name()},
            .max_iterations = shell->max_iterations(),
            .draft_request_builder = shell->actor_request_builder(),
            .review_request_builder = shell->critic_request_builder(),
            .review_decision_reader = shell->review_decision_reader(),
            .draft_graph = std::move(actor_graph).value(),
            .review_graph = std::move(critic_graph).value(),
        };
        auto memory_writer = shell->memory_writer();
        if (memory_writer.has_value()) {
          auto memory_graph = memory_writer.value().get().lower_graph();
          if (memory_graph.has_error()) {
            return wh::core::result<wh::compose::graph>::failure(
                memory_graph.error());
          }
          config.memory_request_builder = shell->memory_writer_request_builder();
          config.memory_graph = std::move(memory_graph).value();
        }
        return revision_detail::lower_graph(std::move(config));
      });
  if (bound.has_error()) {
    return wh::core::result<wh::agent::agent>::failure(bound.error());
  }
  return exported;
}

} // namespace wh::adk::detail
