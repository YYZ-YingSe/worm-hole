#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

#include "helper/component_contract_support.hpp"
#include "helper/compose_graph_test_utils.hpp"
#include "wh/core/run_context.hpp"
#include "wh/embedding/embedding.hpp"

namespace {

using wh::testing::helper::invoke_value_sync;
using wh::testing::helper::register_test_callbacks;
using wh::testing::helper::sync_embedding_impl;

struct observed_callback_entry {
  std::string source{};
  wh::core::callback_run_info run_info{};
};

[[nodiscard]] auto make_observed_callback_registration(
    std::string source, std::vector<observed_callback_entry> &records)
    -> wh::compose::graph_node_callback_registration {
  const auto stored_source = std::move(source);
  wh::core::stage_callbacks callbacks{};
  callbacks.on_end = wh::core::stage_view_callback{
      [&records, source_copy = stored_source](
          const wh::core::callback_stage stage,
          const wh::core::callback_event_view event,
          const wh::core::callback_run_info &run_info) {
        REQUIRE(stage == wh::core::callback_stage::end);
        REQUIRE(event.get_if<int>() != nullptr);
        records.push_back(observed_callback_entry{
            .source = source_copy,
            .run_info = run_info,
        });
      }};
  return wh::compose::graph_node_callback_registration{
      .config =
          wh::core::callback_config{
              .timing_checker = wh::core::callback_timing_checker{
                  [](const wh::core::callback_stage stage) noexcept {
                    return stage == wh::core::callback_stage::end;
                  }},
              .name = stored_source,
          },
      .callbacks = std::move(callbacks),
  };
}

inline auto emit_observed_callback(wh::core::run_context &context,
                                   const int payload = 1) -> void {
  wh::core::callback_run_info run_info{};
  run_info.name = "observed";
  run_info.type = "Observed";
  run_info.component = wh::core::component_kind::custom;
  wh::core::inject_callback_event(context, wh::core::callback_stage::end, payload,
                                  run_info);
}

} // namespace

TEST_CASE("compose graph observation appends local callbacks and patches trace metadata",
          "[core][compose][graph][condition]") {
  std::vector<observed_callback_entry> callback_entries{};

  wh::compose::graph_add_node_options node_options{};
  node_options.observation.local_callbacks.push_back(
      make_observed_callback_registration("default", callback_entries));

  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input,
                     wh::core::run_context &context,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    emit_observed_callback(context);
                    return std::move(input);
                  },
                  std::move(node_options))
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(context),
      [](const wh::core::callback_stage stage) noexcept {
        return stage == wh::core::callback_stage::end;
      },
      [&callback_entries](const wh::core::callback_stage stage,
                          const wh::core::callback_event_view event,
                          const wh::core::callback_run_info &run_info) {
        REQUIRE(stage == wh::core::callback_stage::end);
        REQUIRE(event.get_if<int>() != nullptr);
        callback_entries.push_back(observed_callback_entry{
            .source = "outer",
            .run_info = run_info,
        });
      },
      "outer");
  REQUIRE(registered.has_value());
  context = std::move(registered).value();

  wh::compose::graph_call_options call_options{};
  call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "trace-run",
      .parent_span_id = "trace-parent",
  };
  call_options.node_observations.push_back(
      wh::compose::graph_node_observation_override{
          .path = wh::compose::make_node_path({"worker"}),
          .local_callbacks = wh::compose::graph_node_callback_plan{
              make_observed_callback_registration("override", callback_entries),
          },
      });

  auto invoked = invoke_value_sync(graph, wh::core::any(9), context,
                                   std::move(call_options));
  REQUIRE(invoked.has_value());
  REQUIRE(callback_entries.size() == 3U);
  REQUIRE(callback_entries[0].source == "outer");
  REQUIRE(callback_entries[1].source == "default");
  REQUIRE(callback_entries[2].source == "override");
  for (const auto &entry : callback_entries) {
    REQUIRE(entry.run_info.trace_id == "trace-run");
    REQUIRE_FALSE(entry.run_info.span_id.empty());
    REQUIRE_FALSE(entry.run_info.parent_span_id.empty());
    REQUIRE(entry.run_info.node_path.to_string() == "graph/worker");
  }
}

TEST_CASE("compose graph observation uses last matching enablement override",
          "[core][compose][graph][condition]") {
  int callback_count = 0;

  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input,
                     wh::core::run_context &context,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    emit_observed_callback(context);
                    return std::move(input);
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(context),
      [](const wh::core::callback_stage stage) noexcept {
        return stage == wh::core::callback_stage::end;
      },
      [&callback_count](const wh::core::callback_stage,
                        const wh::core::callback_event_view event,
                        const wh::core::callback_run_info &) {
        REQUIRE(event.get_if<int>() != nullptr);
        ++callback_count;
      },
      "override-last-wins");
  REQUIRE(registered.has_value());
  context = std::move(registered).value();

  wh::compose::graph_call_options call_options{};
  call_options.node_observations.push_back(
      wh::compose::graph_node_observation_override{
          .path = wh::compose::make_node_path({"worker"}),
          .callbacks_enabled = true,
      });
  call_options.node_observations.push_back(
      wh::compose::graph_node_observation_override{
          .path = wh::compose::make_node_path({"worker"}),
          .callbacks_enabled = false,
      });

  auto invoked = invoke_value_sync(graph, wh::core::any(3), context,
                                   std::move(call_options));
  REQUIRE(invoked.has_value());
  REQUIRE(callback_count == 0);
}

TEST_CASE("compose graph observation downscopes subtree overrides into nested graphs",
          "[core][compose][graph][condition]") {
  std::vector<observed_callback_entry> callback_entries{};

  wh::compose::graph child{};
  REQUIRE(child
              .add_lambda(
                  "leaf",
                  [](wh::compose::graph_value &input,
                     wh::core::run_context &context,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    emit_observed_callback(context);
                    return std::move(input);
                  })
              .has_value());
  REQUIRE(child.add_entry_edge("leaf").has_value());
  REQUIRE(child.add_exit_edge("leaf").has_value());
  REQUIRE(child.compile().has_value());

  wh::compose::graph parent{};
  REQUIRE(parent.add_subgraph("child", std::move(child)).has_value());
  REQUIRE(parent.add_entry_edge("child").has_value());
  REQUIRE(parent.add_exit_edge("child").has_value());
  REQUIRE(parent.compile().has_value());

  wh::compose::graph_call_options call_options{};
  call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "trace-nested",
      .parent_span_id = "trace-root",
  };
  call_options.node_observations.push_back(
      wh::compose::graph_node_observation_override{
          .path = wh::compose::make_node_path({"child"}),
          .include_descendants = true,
          .local_callbacks = wh::compose::graph_node_callback_plan{
              make_observed_callback_registration("subtree", callback_entries),
          },
      });

  wh::core::run_context context{};
  auto invoked =
      invoke_value_sync(parent, wh::core::any(5), context,
                        std::move(call_options));
  REQUIRE(invoked.has_value());
  REQUIRE(callback_entries.size() == 1U);
  REQUIRE(callback_entries.front().source == "subtree");
  REQUIRE(callback_entries.front().run_info.trace_id == "trace-nested");
  REQUIRE(callback_entries.front().run_info.node_path.to_string() ==
          "graph/child/leaf");
}

TEST_CASE("compose graph standard component callbacks carry graph trace metadata",
          "[core][compose][graph][condition]") {
  std::vector<wh::core::callback_run_info> callback_infos{};

  wh::compose::graph graph{};
  REQUIRE(graph
              .add_component<wh::core::component_kind::embedding,
                             wh::compose::node_contract::value,
                             wh::compose::node_contract::value>(
                  "embed",
                  wh::embedding::embedding{sync_embedding_impl{
                      [](const wh::embedding::embedding_request &request)
                          -> wh::core::result<wh::embedding::embedding_response> {
                        return wh::embedding::embedding_response{
                            std::vector<double>{
                                static_cast<double>(request.inputs.size())}};
                      }}})
              .has_value());
  REQUIRE(graph.add_entry_edge("embed").has_value());
  REQUIRE(graph.add_exit_edge("embed").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};
  context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(context),
      [](const wh::core::callback_stage stage) noexcept {
        return stage == wh::core::callback_stage::end;
      },
      [&callback_infos](const wh::core::callback_stage,
                        const wh::core::callback_event_view event,
                        const wh::core::callback_run_info &run_info) {
        REQUIRE(
            event.get_if<wh::embedding::embedding_callback_event>() != nullptr);
        callback_infos.push_back(run_info);
      },
      "component-observation");
  REQUIRE(registered.has_value());
  context = std::move(registered).value();

  wh::compose::graph_call_options call_options{};
  call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "trace-component",
      .parent_span_id = "trace-parent",
  };
  auto invoked = invoke_value_sync(
      graph,
      wh::core::any(
          wh::embedding::embedding_request{.inputs = {"abc", "def"}}),
      context, std::move(call_options));
  REQUIRE(invoked.has_value());
  REQUIRE(callback_infos.size() == 1U);
  REQUIRE(callback_infos.front().trace_id == "trace-component");
  REQUIRE_FALSE(callback_infos.front().span_id.empty());
  REQUIRE_FALSE(callback_infos.front().parent_span_id.empty());
  REQUIRE(callback_infos.front().node_path.to_string() == "graph/embed");
}

TEST_CASE("compose graph runtime-aware nodes only fork run_context when local callbacks are injected",
          "[core][compose][graph][condition]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda(
                  "worker",
                  [](wh::compose::graph_value &input,
                     wh::core::run_context &context,
                     const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    auto stored =
                        wh::core::set_session_value(context, "node-mutated", 7);
                    if (stored.has_error()) {
                      return wh::core::result<wh::compose::graph_value>::failure(stored.error());
                    }
                    return std::move(input);
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context shared_context{};
  auto shared_invoked =
      invoke_value_sync(graph, wh::core::any(1), shared_context);
  REQUIRE(shared_invoked.has_value());
  auto shared_value =
      wh::core::session_value_ref<int>(shared_context, "node-mutated");
  REQUIRE(shared_value.has_value());
  REQUIRE(shared_value.value().get() == 7);

  wh::core::run_context forked_context{};
  wh::compose::graph_call_options call_options{};
  call_options.node_observations.push_back(
      wh::compose::graph_node_observation_override{
          .path = wh::compose::make_node_path({"worker"}),
          .local_callbacks = wh::compose::graph_node_callback_plan{
              wh::compose::graph_node_callback_registration{},
          },
      });
  auto forked_invoked =
      invoke_value_sync(graph, wh::core::any(1), forked_context,
                        std::move(call_options));
  REQUIRE(forked_invoked.has_value());
  auto forked_value =
      wh::core::session_value_ref<int>(forked_context, "node-mutated");
  REQUIRE(forked_value.has_error());
  REQUIRE(forked_value.error() == wh::core::errc::not_found);
}
