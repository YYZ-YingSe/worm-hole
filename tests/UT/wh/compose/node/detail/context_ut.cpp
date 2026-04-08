#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/node/detail/context.hpp"
#include "wh/compose/runtime/state.hpp"
#include "wh/internal/callbacks.hpp"

namespace {

struct request_options_holder {
  wh::core::component_options storage{};

  [[nodiscard]] auto component_options() noexcept
      -> wh::core::component_options & {
    return storage;
  }

  [[nodiscard]] auto component_options() const noexcept
      -> const wh::core::component_options & {
    return storage;
  }
};

struct request_with_options {
  int value{0};
  request_options_holder options{};
};

struct invalid_request {
  int value{0};
};

struct state_payload {
  int value{0};
};

[[nodiscard]] auto await_graph_sender(wh::compose::graph_sender sender)
    -> wh::core::result<wh::compose::graph_value> {
  auto waited = stdexec::sync_wait(std::move(sender));
  REQUIRE(waited.has_value());
  return std::get<0>(std::move(*waited));
}

[[nodiscard]] auto inline_graph_scheduler()
    -> const wh::core::detail::any_resume_scheduler_t & {
  static const auto scheduler =
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{});
  return scheduler;
}

static_assert(
    wh::compose::detail::component_request_with_options<request_with_options>);
static_assert(
    !wh::compose::detail::component_request_with_options<invalid_request>);

} // namespace

TEST_CASE("node context defaults bindings and callback projection follow runtime observation rules",
          "[UT][wh/compose/node/detail/context.hpp][resolve_node_context_binding][condition][branch]") {
  REQUIRE(std::addressof(wh::compose::detail::default_node_observation()) ==
          std::addressof(wh::compose::detail::default_node_observation()));
  REQUIRE(std::addressof(wh::compose::detail::default_node_trace()) ==
          std::addressof(wh::compose::detail::default_node_trace()));
  REQUIRE(std::addressof(wh::compose::detail::default_node_call_options()) ==
          std::addressof(wh::compose::detail::default_node_call_options()));

  wh::compose::graph_node_trace empty_trace{};
  auto empty_metadata =
      wh::compose::detail::make_callback_metadata(empty_trace);
  REQUIRE(empty_metadata.empty());

  auto path = wh::compose::make_node_path({"root", "leaf"});
  wh::compose::graph_node_trace trace{};
  trace.trace_id = "trace-id";
  trace.span_id = "span-id";
  trace.parent_span_id = "parent-span-id";
  trace.path = &path;
  auto metadata = wh::compose::detail::make_callback_metadata(trace);
  REQUIRE(metadata.trace_id == "trace-id");
  REQUIRE(metadata.span_id == "span-id");
  REQUIRE(metadata.parent_span_id == "parent-span-id");
  REQUIRE(metadata.node_path == path);

  wh::core::run_context parent{};
  wh::compose::graph_resolved_node_observation observation{};

  const auto shared = wh::compose::detail::resolve_node_context_binding(
      parent, observation, empty_trace);
  REQUIRE_FALSE(shared.projects_callbacks());
  REQUIRE_FALSE(shared.forks_execution());

  parent.callbacks.emplace();
  const auto projected = wh::compose::detail::resolve_node_context_binding(
      parent, observation, empty_trace);
  REQUIRE(projected.projects_callbacks());
  REQUIRE_FALSE(projected.forks_execution());

  const auto forked_by_metadata =
      wh::compose::detail::resolve_node_context_binding(parent, observation,
                                                        trace);
  REQUIRE(forked_by_metadata.projects_callbacks());
  REQUIRE(forked_by_metadata.forks_execution());

  observation.callbacks_enabled = false;
  const auto disabled = wh::compose::detail::resolve_node_context_binding(
      parent, observation, trace);
  REQUIRE_FALSE(disabled.projects_callbacks());
  REQUIRE(disabled.forks_execution());

  observation.callbacks_enabled = true;
  std::vector<int> seen{};
  wh::core::stage_callbacks callbacks{};
  callbacks.on_end = [&seen](const wh::core::callback_stage,
                             const wh::core::callback_event_view event,
                             const wh::core::callback_run_info &) {
    const auto *typed = event.get_if<int>();
    REQUIRE(typed != nullptr);
    seen.push_back(*typed);
  };
  observation.local_callbacks.push_back({
      .config = wh::internal::make_callback_config(
          [](const wh::core::callback_stage stage) noexcept {
            return stage == wh::core::callback_stage::end;
          }),
      .callbacks = callbacks,
  });

  wh::core::run_context callback_context{};
  wh::compose::detail::apply_node_callbacks(callback_context, observation, trace);
  REQUIRE(callback_context.callbacks.has_value());
  REQUIRE(callback_context.callbacks->metadata.trace_id == "trace-id");
  REQUIRE(callback_context.callbacks->metadata.span_id == "span-id");
  REQUIRE(callback_context.callbacks->metadata.parent_span_id ==
          "parent-span-id");
  wh::core::inject_callback_event(callback_context, wh::core::callback_stage::end,
                                  7, {});
  REQUIRE(seen == std::vector<int>{7});

  observation.callbacks_enabled = false;
  wh::compose::detail::apply_node_callbacks(callback_context, observation, trace);
  REQUIRE_FALSE(callback_context.callbacks.has_value());
}

TEST_CASE("node context helpers expose defaults process state request patching and scoped callback restoration",
          "[UT][wh/compose/node/detail/context.hpp][scoped_node_callbacks][branch][boundary]") {
  wh::compose::node_runtime runtime{};
  REQUIRE(wh::compose::node_observation(runtime).callbacks_enabled);
  REQUIRE(wh::compose::node_trace(runtime).trace_id.empty());
  REQUIRE(wh::compose::node_call_options(runtime).prefix().empty());

  wh::compose::graph_call_options options{};
  wh::compose::graph_call_scope scope{options};
  auto path = wh::compose::make_node_path({"graph", "worker"});
  wh::compose::graph_resolved_node_observation observation{};
  observation.callbacks_enabled = true;
  wh::compose::graph_node_trace trace{};
  trace.trace_id = "trace";
  trace.span_id = "span";
  trace.parent_span_id = "parent";
  trace.path = &path;
  wh::compose::graph_process_state process_state{};

  runtime.set_call_options(&scope)
      .set_observation(&observation)
      .set_trace(&trace)
      .set_process_state(&process_state);

  REQUIRE(wh::compose::node_observation(runtime).callbacks_enabled);
  REQUIRE(wh::compose::node_trace(runtime).trace_id == "trace");
  REQUIRE(wh::compose::node_call_options(runtime).prefix().empty());

  wh::compose::node_runtime missing_runtime{};
  auto missing_state =
      wh::compose::node_process_state_ref<state_payload>(missing_runtime);
  REQUIRE(missing_state.has_error());
  REQUIRE(missing_state.error() == wh::core::errc::not_found);

  auto emplaced =
      wh::compose::emplace_node_process_state<state_payload>(runtime, 41);
  REQUIRE(emplaced.has_value());
  REQUIRE(emplaced.value().get().value == 41);

  auto mutable_state = wh::compose::node_process_state_ref<state_payload>(runtime);
  REQUIRE(mutable_state.has_value());
  REQUIRE(mutable_state.value().get().value == 41);

  auto const_state =
      wh::compose::node_const_process_state_ref<state_payload>(runtime);
  REQUIRE(const_state.has_value());
  REQUIRE(const_state.value().get().value == 41);

  request_with_options request{};
  request.options.component_options().set_base(
      wh::core::component_common_options{
          .callbacks_enabled = true,
          .trace_id = "old-trace",
          .span_id = "old-span",
      });
  wh::compose::patch_component_request(request, observation, trace);
  const auto view = request.options.component_options().resolve_view();
  REQUIRE(view.callbacks_enabled);
  REQUIRE(view.trace_id == "trace");
  REQUIRE(view.span_id == "span");

  wh::core::run_context parent{};
  parent.callbacks.emplace();
  parent.callbacks->metadata.trace_id = "old";
  {
    wh::compose::scoped_node_callbacks scoped{parent, observation, trace};
    REQUIRE(parent.callbacks.has_value());
    REQUIRE(parent.callbacks->metadata.trace_id == "trace");
    REQUIRE(parent.callbacks->metadata.span_id == "span");
  }
  REQUIRE(parent.callbacks.has_value());
  REQUIRE(parent.callbacks->metadata.trace_id == "old");

  auto callback_context =
      wh::compose::make_node_callback_context(parent, observation, trace);
  REQUIRE(callback_context.has_value());
  REQUIRE(callback_context->callbacks.has_value());
  REQUIRE(callback_context->callbacks->metadata.trace_id == "trace");

  auto execution_context =
      wh::compose::make_node_context(parent, observation, trace);
  REQUIRE(execution_context.has_value());
  REQUIRE(execution_context->callbacks.has_value());
  REQUIRE(execution_context->callbacks->metadata.trace_id == "trace");

  observation.callbacks_enabled = false;
  REQUIRE_FALSE(
      wh::compose::make_node_callback_context(parent, observation, trace)
          .has_value());
  REQUIRE(
      wh::compose::make_node_context(parent, observation, trace).has_value());
}

TEST_CASE("node sender binders preserve shared or forked context and adapt value reader inputs",
          "[UT][wh/compose/node/detail/context.hpp][bind_reader_sender][condition][branch]") {
  wh::core::run_context shared_context{};
  wh::compose::node_runtime shared_runtime{};
  shared_runtime.set_graph_scheduler(&inline_graph_scheduler());

  const auto shared_result = wh::compose::with_node_context(
      shared_context, shared_runtime,
      [](wh::core::run_context &node_context) -> int {
        wh::core::set_session_value(node_context, "shared", 1);
        return 7;
      });
  REQUIRE(shared_result == 7);
  REQUIRE(wh::core::session_value_ref<int>(shared_context, "shared").has_value());

  wh::core::run_context fork_parent{};
  fork_parent.callbacks.emplace();
  wh::compose::graph_resolved_node_observation fork_observation{};
  fork_observation.callbacks_enabled = true;
  wh::compose::graph_node_trace fork_trace{};
  fork_trace.trace_id = "fork-trace";
  wh::compose::node_runtime fork_runtime{};
  fork_runtime.set_graph_scheduler(&inline_graph_scheduler())
      .set_observation(&fork_observation)
      .set_trace(&fork_trace);

  const auto fork_result = wh::compose::with_node_context(
      fork_parent, fork_runtime,
      [](wh::core::run_context &node_context) -> int {
        wh::core::set_session_value(node_context, "forked", 9);
        return 11;
      });
  REQUIRE(fork_result == 11);
  REQUIRE(wh::core::session_value_ref<int>(fork_parent, "forked").has_error());

  wh::compose::graph_call_options call_options_storage{};
  auto scoped = wh::compose::graph_call_scope::root(call_options_storage);
  fork_runtime.set_call_options(&scoped);
  const auto prefix_result = wh::compose::with_node_call(
      fork_parent, fork_runtime,
      [](wh::core::run_context &, const wh::compose::graph_call_scope &call_scope)
          -> bool { return call_scope.prefix().empty(); });
  REQUIRE(prefix_result);

  auto copyable_input = wh::compose::graph_value{13};
  auto copied = wh::compose::capture_node_input(copyable_input);
  REQUIRE(*wh::core::any_cast<int>(&copied) == 13);
  REQUIRE(*wh::core::any_cast<int>(&copyable_input) == 13);

  auto unique_input = wh::compose::graph_value{std::make_unique<int>(17)};
  auto moved = wh::compose::capture_node_input(unique_input);
  auto *moved_ptr = wh::core::any_cast<std::unique_ptr<int>>(&moved);
  REQUIRE(moved_ptr != nullptr);
  REQUIRE(**moved_ptr == 17);

  wh::core::run_context input_context{};
  auto with_call_options = [](int value, wh::core::run_context &,
                              const wh::compose::graph_call_scope &) {
    return value + 1;
  };
  auto without_call_options = [](int value, wh::core::run_context &) {
    return value + 2;
  };
  REQUIRE(wh::compose::detail::invoke_input_sender(with_call_options, 3,
                                                   input_context, scoped) == 4);
  REQUIRE(wh::compose::detail::invoke_input_sender(without_call_options, 3,
                                                   input_context, scoped) == 5);

  auto missing_scheduler = await_graph_sender(wh::compose::bind_node_sender(
      shared_context, wh::compose::node_runtime{},
      [](wh::core::run_context &) {
        return stdexec::just(wh::core::result<wh::compose::graph_value>{
            wh::compose::graph_value{1}});
      }));
  REQUIRE(missing_scheduler.has_error());
  REQUIRE(missing_scheduler.error() == wh::core::errc::contract_violation);

  auto bound_sender = await_graph_sender(wh::compose::bind_node_sender(
      shared_context, shared_runtime, [](wh::core::run_context &) {
        return stdexec::just(wh::core::result<wh::compose::graph_value>{
            wh::compose::graph_value{21}});
      }));
  REQUIRE(bound_sender.has_value());
  REQUIRE(*wh::core::any_cast<int>(&bound_sender.value()) == 21);

  auto bound_call_sender = await_graph_sender(wh::compose::bind_node_call_sender(
      shared_context, shared_runtime,
      [](wh::core::run_context &, const wh::compose::graph_call_scope &call_scope) {
        return stdexec::just(wh::core::result<wh::compose::graph_value>{
            wh::compose::graph_value{call_scope.prefix().size()}});
      }));
  REQUIRE(bound_call_sender.has_value());
  REQUIRE(*wh::core::any_cast<std::size_t>(&bound_call_sender.value()) == 0U);

  auto value_input = wh::compose::graph_value{5};
  auto bound_value = await_graph_sender(wh::compose::bind_value_sender(
      value_input, shared_context, shared_runtime,
      [](wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &) {
        auto *typed = wh::core::any_cast<int>(&input);
        REQUIRE(typed != nullptr);
        return stdexec::just(wh::core::result<wh::compose::graph_value>{
            wh::compose::graph_value{*typed + 1}});
      }));
  REQUIRE(bound_value.has_value());
  REQUIRE(*wh::core::any_cast<int>(&bound_value.value()) == 6);

  auto reader_status = wh::compose::make_single_value_stream_reader(9);
  REQUIRE(reader_status.has_value());
  auto reader_input = wh::compose::graph_value{std::move(reader_status).value()};
  auto bound_reader = await_graph_sender(wh::compose::bind_reader_sender(
      reader_input, shared_context, shared_runtime,
      [](wh::compose::graph_stream_reader reader, wh::core::run_context &,
         const wh::compose::graph_call_scope &) {
        auto collected =
            wh::compose::collect_graph_stream_reader(std::move(reader));
        if (collected.has_error()) {
          return stdexec::just(
              wh::core::result<wh::compose::graph_value>::failure(
                  collected.error()));
        }
        return stdexec::just(wh::core::result<wh::compose::graph_value>{
            wh::compose::graph_value{collected.value().size()}});
      }));
  REQUIRE(bound_reader.has_value());
  REQUIRE(*wh::core::any_cast<std::size_t>(&bound_reader.value()) == 1U);

  auto wrong_reader_input = wh::compose::graph_value{3};
  auto wrong_reader = await_graph_sender(wh::compose::bind_reader_sender(
      wrong_reader_input, shared_context, shared_runtime,
      [](wh::compose::graph_stream_reader, wh::core::run_context &,
         const wh::compose::graph_call_scope &) {
        return stdexec::just(wh::core::result<wh::compose::graph_value>{
            wh::compose::graph_value{0}});
      }));
  REQUIRE(wrong_reader.has_error());
  REQUIRE(wrong_reader.error() == wh::core::errc::type_mismatch);
}
