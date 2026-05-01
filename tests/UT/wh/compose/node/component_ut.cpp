#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <exec/task.hpp>
#include <stdexec/execution.hpp>

#include "helper/static_thread_scheduler.hpp"
#include "wh/compose/graph.hpp"
#include "wh/compose/graph/call_options.hpp"
#include "wh/compose/graph/detail/build.hpp"
#include "wh/compose/graph/detail/compile.hpp"
#include "wh/compose/graph/detail/graph_class.hpp"
#include "wh/compose/graph/detail/invoke.hpp"
#include "wh/compose/node.hpp"
#include "wh/compose/node/component.hpp"
#include "wh/model/retry.hpp"

namespace {

struct request_options_holder {
  wh::core::component_options storage{};

  [[nodiscard]] auto component_options() noexcept -> wh::core::component_options & {
    return storage;
  }

  [[nodiscard]] auto component_options() const noexcept -> const wh::core::component_options & {
    return storage;
  }
};

struct request_with_options {
  int value{0};
  request_options_holder options{};
};

struct custom_sync_component {
  auto invoke(int value, wh::core::run_context &) const -> wh::core::result<int> {
    return value + 1;
  }
};

struct custom_async_component {
  auto async_invoke(int value, wh::core::run_context &) const {
    return stdexec::just(wh::core::result<int>{value + 2});
  }
};

struct custom_stream_component {
  auto stream(int value, wh::core::run_context &) const
      -> wh::core::result<wh::compose::graph_stream_reader> {
    return wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
        wh::compose::graph_value{value},
        wh::compose::graph_value{value + 1},
    });
  }
};

template <typename value_t>
[[nodiscard]] auto read_any(wh::compose::graph_value &&value) -> wh::core::result<value_t>;

struct task_value_value_component {
  auto async_invoke(const int &value, wh::core::run_context &) const
      -> exec::task<wh::core::result<int>> {
    co_return wh::core::result<int>{value + 1};
  }
};

struct task_value_stream_component {
  auto async_stream(const int &value, wh::core::run_context &) const
      -> exec::task<wh::core::result<wh::compose::graph_stream_reader>> {
    co_return wh::compose::make_values_stream_reader(std::vector<wh::compose::graph_value>{
        wh::compose::graph_value{value},
        wh::compose::graph_value{value * 2},
    });
  }
};

struct task_stream_stream_component {
  auto async_stream(wh::compose::graph_stream_reader reader, wh::core::run_context &) const
      -> exec::task<wh::core::result<wh::compose::graph_stream_reader>> {
    auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
    if (chunks.has_error()) {
      co_return wh::core::result<wh::compose::graph_stream_reader>::failure(chunks.error());
    }

    std::vector<wh::compose::graph_value> outputs{};
    outputs.reserve(chunks.value().size());
    for (auto &chunk : chunks.value()) {
      auto typed = read_any<int>(std::move(chunk));
      if (typed.has_error()) {
        co_return wh::core::result<wh::compose::graph_stream_reader>::failure(typed.error());
      }
      outputs.emplace_back(typed.value() + 10);
    }
    co_return wh::compose::make_values_stream_reader(std::move(outputs));
  }
};

struct task_stream_value_component {
  auto async_invoke(wh::compose::graph_stream_reader reader, wh::core::run_context &) const
      -> exec::task<wh::core::result<int>> {
    auto chunks = wh::compose::collect_graph_stream_reader(std::move(reader));
    if (chunks.has_error()) {
      co_return wh::core::result<int>::failure(chunks.error());
    }

    int total = 0;
    for (auto &chunk : chunks.value()) {
      auto typed = read_any<int>(std::move(chunk));
      if (typed.has_error()) {
        co_return wh::core::result<int>::failure(typed.error());
      }
      total += typed.value();
    }
    co_return wh::core::result<int>{total};
  }
};

struct model_component_stub {
  auto invoke(wh::model::chat_request, wh::core::run_context &) const
      -> wh::model::chat_invoke_result {
    return wh::model::chat_invoke_result::failure(wh::core::errc::not_supported);
  }

  auto stream(wh::model::chat_request, wh::core::run_context &) const
      -> wh::model::chat_message_stream_result {
    return wh::model::chat_message_stream_result::failure(wh::core::errc::not_supported);
  }

  auto async_invoke(wh::model::chat_request, wh::core::run_context &) const {
    return stdexec::just(wh::model::chat_invoke_result::failure(wh::core::errc::not_supported));
  }

  auto async_stream(wh::model::chat_request, wh::core::run_context &) const {
    return stdexec::just(
        wh::model::chat_message_stream_result::failure(wh::core::errc::not_supported));
  }
};

[[nodiscard]] auto make_user_message(const std::string &text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

struct retry_async_model_state {
  std::size_t invoke_calls{0U};
  std::size_t stream_calls{0U};
};

struct retry_async_model_impl {
  std::shared_ptr<retry_async_model_state> state{};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"RetryAsyncModelComponent", wh::core::component_kind::model};
  }

  [[nodiscard]] auto async_invoke(wh::model::chat_request, wh::core::run_context &) const {
    ++state->invoke_calls;
    if (state->invoke_calls == 1U) {
      return stdexec::just(
          wh::model::chat_invoke_result::failure(wh::core::errc::unavailable));
    }

    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"retry-async-vv"});
    return stdexec::just(
        wh::model::chat_invoke_result{wh::model::chat_response{.message = std::move(message)}});
  }

  [[nodiscard]] auto async_stream(wh::model::chat_request, wh::core::run_context &) const {
    ++state->stream_calls;
    if (state->stream_calls == 1U) {
      return stdexec::just(
          wh::model::chat_message_stream_result::failure(wh::core::errc::unavailable));
    }

    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"retry-async-vs"});
    return stdexec::just(wh::model::chat_message_stream_result{
        wh::model::chat_message_stream_reader{
            wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
                std::move(message))}});
  }

  [[nodiscard]] auto bind_tools(const std::span<const wh::schema::tool_schema_definition>) const
      -> retry_async_model_impl {
    return *this;
  }
};

template <typename value_t>
[[nodiscard]] auto read_any(wh::compose::graph_value &&value) -> wh::core::result<value_t> {
  if (auto *typed = wh::core::any_cast<value_t>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

[[nodiscard]] auto await_graph_sender(wh::compose::graph_sender sender)
    -> wh::core::result<wh::compose::graph_value> {
  auto awaited = stdexec::sync_wait(std::move(sender));
  REQUIRE(awaited.has_value());
  return std::get<0>(std::move(*awaited));
}

[[nodiscard]] auto compile_component_node(const wh::compose::component_node &node)
    -> wh::core::result<wh::compose::compiled_node> {
  wh::compose::graph graph{};
  auto added = graph.add_component(node);
  if (added.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(added.error());
  }
  if (auto entry = graph.add_entry_edge(std::string{node.key()}); entry.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(entry.error());
  }
  if (auto exit = graph.add_exit_edge(std::string{node.key()}); exit.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(exit.error());
  }
  auto compiled = graph.compile();
  if (compiled.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(compiled.error());
  }
  auto lowered = graph.compiled_node_by_key(node.key());
  if (lowered.has_error()) {
    return wh::core::result<wh::compose::compiled_node>::failure(lowered.error());
  }
  return lowered.value().get();
}

} // namespace

TEST_CASE(
    "component helpers cover request resolution context projection and payload bridging",
    "[UT][wh/compose/node/component.hpp][resolve_component_request][condition][branch][boundary]") {
  REQUIRE(wh::compose::detail::component_kind_name(wh::compose::component_kind::model) == "model");
  REQUIRE(wh::compose::detail::component_kind_name(wh::compose::component_kind::custom) ==
          "custom");

  auto decorated = wh::compose::detail::decorate_component_options(
      "worker", wh::compose::component_kind::custom, wh::compose::graph_add_node_options{});
  REQUIRE(decorated.name == "worker");
  REQUIRE(decorated.type == "custom");
  REQUIRE(decorated.label == "custom");

  wh::compose::graph_value const_input{request_with_options{.value = 7}};
  auto const_request =
      wh::compose::detail::read_request<request_with_options>(std::as_const(const_input));
  REQUIRE(const_request.has_value());
  REQUIRE(const_request.value().get().value == 7);

  wh::compose::graph_value mutable_input{9};
  auto *mutable_request = wh::compose::detail::read_request<int>(mutable_input);
  REQUIRE(mutable_request != nullptr);
  REQUIRE(*mutable_request == 9);
  REQUIRE(wh::compose::detail::read_request<std::string>(mutable_input) == nullptr);

  int borrowed_source = 4;
  auto borrowed = wh::compose::detail::component_request_state<int>::borrow(borrowed_source);
  REQUIRE(std::move(borrowed).apply([](auto &&value) { return value + 1; }) == 5);

  auto owned = wh::compose::detail::component_request_state<int>::own(6);
  REQUIRE(std::move(owned).into_owned() == 6);

  request_with_options request{};
  request.value = 11;
  request.options.component_options().set_base(wh::core::component_common_options{
      .callbacks_enabled = true,
      .trace_id = "old-trace",
      .span_id = "old-span",
  });

  wh::compose::graph_resolved_node_observation observation{};
  observation.callbacks_enabled = false;
  auto path = wh::compose::make_node_path({"graph", "node"});
  wh::compose::graph_node_trace trace{};
  trace.trace_id = "trace-1";
  trace.span_id = "span-1";
  trace.parent_span_id = "parent-1";
  trace.path = &path;

  wh::compose::node_runtime runtime{};
  runtime.set_observation(&observation).set_trace(&trace);

  REQUIRE_FALSE(wh::compose::detail::request_matches_runtime(request, runtime));
  auto resolved =
      wh::compose::detail::resolve_component_request<request_with_options>(const_input, runtime);
  REQUIRE(resolved.has_value());
  REQUIRE(resolved.value().owned.has_value());
  const auto resolved_view = resolved.value().owned->options.component_options().resolve_view();
  REQUIRE_FALSE(resolved_view.callbacks_enabled);
  REQUIRE(resolved_view.trace_id == "trace-1");
  REQUIRE(resolved_view.span_id == "span-1");

  request.options.component_options().set_call_override(wh::core::component_override_options{
      .callbacks_enabled = false,
      .trace_id = std::string{"trace-1"},
      .span_id = std::string{"span-1"},
  });
  REQUIRE(wh::compose::detail::request_matches_runtime(request, runtime));

  wh::core::run_context parent{};
  parent.callbacks.emplace();
  observation.callbacks_enabled = true;
  auto callback_context = wh::compose::detail::make_component_context(parent, runtime);
  REQUIRE(callback_context.has_value());
  REQUIRE(callback_context->callbacks.has_value());
  REQUIRE(callback_context->callbacks->metadata.trace_id == "trace-1");
  REQUIRE(callback_context->callbacks->metadata.span_id == "span-1");

  observation.callbacks_enabled = false;
  REQUIRE_FALSE(wh::compose::detail::make_component_context(parent, runtime).has_value());

  auto value_output = wh::compose::detail::make_graph_output(std::string{"ok"});
  REQUIRE(value_output.has_value());
  REQUIRE(*wh::core::any_cast<std::string>(&value_output.value()) == "ok");

  auto reader = wh::compose::make_single_value_stream_reader(3);
  REQUIRE(reader.has_value());
  auto stream_output = wh::compose::detail::make_graph_stream_output(std::move(reader).value());
  REQUIRE(stream_output.has_value());
  auto extracted_reader =
      read_any<wh::compose::graph_stream_reader>(std::move(stream_output).value());
  REQUIRE(extracted_reader.has_value());

  auto success_output = wh::compose::detail::to_graph_output(wh::core::result<int>{12});
  REQUIRE(success_output.has_value());
  REQUIRE(*wh::core::any_cast<int>(&success_output.value()) == 12);

  auto failed_output =
      wh::compose::detail::to_graph_output(wh::core::result<int>::failure(wh::core::errc::timeout));
  REQUIRE(failed_output.has_error());
  REQUIRE(failed_output.error() == wh::core::errc::timeout);
}

TEST_CASE("component node builders cover custom sync async stream and explicit model bindings",
          "[UT][wh/compose/node/component.hpp][make_component_node][condition][branch]") {
  auto sync_node = wh::compose::make_component_node<wh::compose::component_kind::custom,
                                                    wh::compose::node_contract::value,
                                                    wh::compose::node_contract::value, int, int>(
      "sync", custom_sync_component{});
  REQUIRE(sync_node.key() == "sync");
  REQUIRE(sync_node.input_gate().kind == wh::compose::input_gate_kind::value_exact);
  REQUIRE(sync_node.output_gate().kind == wh::compose::output_gate_kind::value_exact);
  auto compiled_sync = compile_component_node(sync_node);
  REQUIRE(compiled_sync.has_value());

  wh::compose::graph_value sync_input{5};
  wh::core::run_context context{};
  auto sync_status = wh::compose::run_compiled_sync_node(compiled_sync.value(), sync_input, context,
                                                         wh::compose::node_runtime{});
  REQUIRE(sync_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&sync_status.value()) == 6);

  wh::compose::graph_value wrong_input{std::string{"bad"}};
  auto wrong_status = wh::compose::run_compiled_sync_node(compiled_sync.value(), wrong_input,
                                                          context, wh::compose::node_runtime{});
  REQUIRE(wrong_status.has_error());
  REQUIRE(wrong_status.error() == wh::core::errc::type_mismatch);

  auto async_node = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::value, int, int, wh::compose::node_exec_mode::async>(
      "async", custom_async_component{});
  auto compiled_async = compile_component_node(async_node);
  REQUIRE(compiled_async.has_value());

  wh::testing::helper::static_thread_scheduler_helper scheduler_helper{1U};
  auto scheduler = wh::core::detail::erase_resume_scheduler(scheduler_helper.scheduler());
  wh::compose::node_runtime runtime{};
  runtime.set_control_scheduler(&scheduler);

  wh::compose::graph_value async_input{4};
  auto async_status = await_graph_sender(
      wh::compose::run_compiled_async_node(compiled_async.value(), async_input, context, runtime));
  REQUIRE(async_status.has_value());
  REQUIRE(*wh::core::any_cast<int>(&async_status.value()) == 6);

  auto stream_node = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::stream, int, wh::compose::graph_stream_reader>(
      "stream", custom_stream_component{});
  auto compiled_stream = compile_component_node(stream_node);
  REQUIRE(compiled_stream.has_value());
  wh::compose::graph_value stream_input{2};
  auto stream_status = wh::compose::run_compiled_sync_node(compiled_stream.value(), stream_input,
                                                           context, wh::compose::node_runtime{});
  REQUIRE(stream_status.has_value());
  auto stream_reader = read_any<wh::compose::graph_stream_reader>(std::move(stream_status).value());
  REQUIRE(stream_reader.has_value());
  auto collected = wh::compose::collect_graph_stream_reader(std::move(stream_reader).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 2U);
  REQUIRE(*wh::core::any_cast<int>(&collected.value()[0]) == 2);
  REQUIRE(*wh::core::any_cast<int>(&collected.value()[1]) == 3);

  auto model_node = wh::compose::make_component_node<wh::compose::component_kind::model,
                                                     wh::compose::node_contract::value,
                                                     wh::compose::node_contract::value>(
      "model", model_component_stub{});
  auto compiled_model = compile_component_node(model_node);
  REQUIRE(compiled_model.has_value());
  wh::compose::graph_value model_input{wh::model::chat_request{}};
  auto model_status = wh::compose::run_compiled_sync_node(compiled_model.value(), model_input,
                                                          context, wh::compose::node_runtime{});
  REQUIRE(model_status.has_error());
  REQUIRE(model_status.error() == wh::core::errc::not_supported);
}

TEST_CASE("async component nodes require graph scheduler when invoked directly",
          "[UT][wh/compose/node/component.hpp][make_component_node][condition][error]") {
  auto async_node = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::value, int, int, wh::compose::node_exec_mode::async>(
      "async", custom_async_component{});
  auto compiled_async = compile_component_node(async_node);
  REQUIRE(compiled_async.has_value());

  wh::compose::graph_value input{9};
  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(wh::compose::run_compiled_async_node(
      compiled_async.value(), input, context, wh::compose::node_runtime{}));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::contract_violation);
}

TEST_CASE("async custom component exec task impls compose through graph boundaries",
          "[UT][wh/compose/node/component.hpp][make_component_node][async][exec::task][graph]") {
  auto task_value_value = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::value, int, int, wh::compose::node_exec_mode::async>(
      "task-vv", task_value_value_component{});
  auto task_value_stream = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::value,
      wh::compose::node_contract::stream, int, wh::compose::graph_stream_reader,
      wh::compose::node_exec_mode::async>("task-vs", task_value_stream_component{});
  auto task_stream_stream = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::stream,
      wh::compose::node_contract::stream, wh::compose::graph_stream_reader,
      wh::compose::graph_stream_reader, wh::compose::node_exec_mode::async>(
      "task-ss", task_stream_stream_component{});
  auto task_stream_value = wh::compose::make_component_node<
      wh::compose::component_kind::custom, wh::compose::node_contract::stream,
      wh::compose::node_contract::value, wh::compose::graph_stream_reader, int,
      wh::compose::node_exec_mode::async>("task-sv", task_stream_value_component{});

  wh::compose::graph graph{};
  REQUIRE(graph.add_component(task_value_value).has_value());
  REQUIRE(graph.add_component(task_value_stream).has_value());
  REQUIRE(graph.add_component(task_stream_stream).has_value());
  REQUIRE(graph.add_component(task_stream_value).has_value());

  // This serial path forces the graph runtime through all async custom
  // boundary shapes backed by exec::task.
  REQUIRE(graph.add_entry_edge("task-vv").has_value());
  REQUIRE(graph.add_edge("task-vv", "task-vs").has_value());
  REQUIRE(graph.add_edge("task-vs", "task-ss").has_value());
  REQUIRE(graph.add_edge("task-ss", "task-sv").has_value());
  REQUIRE(graph.add_exit_edge("task-sv").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_input::value(3);

  wh::core::run_context context{};
  auto waited = stdexec::sync_wait(graph.invoke(context, std::move(request)));
  REQUIRE(waited.has_value());

  auto status = std::get<0>(std::move(*waited));
  REQUIRE(status.has_value());
  auto invoke_result = std::move(status).value();
  REQUIRE(invoke_result.output_status.has_value());

  auto output = read_any<int>(std::move(invoke_result.output_status).value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 32);
}

TEST_CASE("retry chat model async surfaces compose through model graph nodes",
          "[UT][wh/compose/node/component.hpp][model][async][retry][graph]") {
  auto make_request = []() {
    wh::model::chat_request request{};
    request.messages.push_back(make_user_message("hello"));
    return request;
  };

  auto invoke_state = std::make_shared<retry_async_model_state>();
  wh::model::retry_chat_model retry_invoke{
      retry_async_model_impl{invoke_state},
      wh::model::retry_chat_model_options{.max_attempts = 3U}};

  auto model_vv = wh::compose::make_component_node<wh::compose::component_kind::model,
                                                   wh::compose::node_contract::value,
                                                   wh::compose::node_contract::value,
                                                   wh::compose::node_exec_mode::async>(
      "retry-vv", retry_invoke);

  wh::compose::graph value_graph{};
  REQUIRE(value_graph.add_component(model_vv).has_value());
  REQUIRE(value_graph.add_entry_edge("retry-vv").has_value());
  REQUIRE(value_graph.add_exit_edge("retry-vv").has_value());
  REQUIRE(value_graph.compile().has_value());

  wh::compose::graph_invoke_request value_request{};
  value_request.input = wh::compose::graph_input::value(make_request());
  wh::core::run_context value_context{};
  auto value_waited = stdexec::sync_wait(value_graph.invoke(value_context, std::move(value_request)));
  REQUIRE(value_waited.has_value());
  auto value_status = std::get<0>(std::move(*value_waited));
  REQUIRE(value_status.has_value());
  auto value_result = std::move(value_status).value();
  REQUIRE(value_result.output_status.has_value());
  auto response = read_any<wh::model::chat_response>(std::move(value_result.output_status).value());
  REQUIRE(response.has_value());
  REQUIRE(std::get<wh::schema::text_part>(response.value().message.parts.front()).text ==
          "retry-async-vv");
  REQUIRE(invoke_state->invoke_calls == 2U);

  auto stream_state = std::make_shared<retry_async_model_state>();
  wh::model::retry_chat_model retry_stream{
      retry_async_model_impl{stream_state},
      wh::model::retry_chat_model_options{.max_attempts = 3U}};

  auto model_vs = wh::compose::make_component_node<wh::compose::component_kind::model,
                                                   wh::compose::node_contract::value,
                                                   wh::compose::node_contract::stream,
                                                   wh::compose::node_exec_mode::async>(
      "retry-vs", retry_stream);

  wh::compose::graph stream_graph{
      wh::compose::graph_boundary{.output = wh::compose::node_contract::stream}};
  REQUIRE(stream_graph.add_component(model_vs).has_value());
  REQUIRE(stream_graph.add_entry_edge("retry-vs").has_value());
  REQUIRE(stream_graph.add_exit_edge("retry-vs").has_value());
  REQUIRE(stream_graph.compile().has_value());

  wh::compose::graph_invoke_request stream_request{};
  stream_request.input = wh::compose::graph_input::value(make_request());
  wh::core::run_context stream_context{};
  auto stream_waited =
      stdexec::sync_wait(stream_graph.invoke(stream_context, std::move(stream_request)));
  REQUIRE(stream_waited.has_value());
  auto stream_status = std::get<0>(std::move(*stream_waited));
  REQUIRE(stream_status.has_value());
  auto stream_result = std::move(stream_status).value();
  REQUIRE(stream_result.output_status.has_value());
  auto reader = read_any<wh::compose::graph_stream_reader>(std::move(stream_result.output_status).value());
  REQUIRE(reader.has_value());
  auto collected = wh::compose::collect_graph_stream_reader(std::move(reader).value());
  REQUIRE(collected.has_value());
  REQUIRE(collected.value().size() == 1U);
  auto message = read_any<wh::schema::message>(std::move(collected.value().front()));
  REQUIRE(message.has_value());
  REQUIRE(std::get<wh::schema::text_part>(message.value().parts.front()).text == "retry-async-vs");
  REQUIRE(stream_state->stream_calls == 2U);

  auto collected_state = std::make_shared<retry_async_model_state>();
  wh::model::retry_chat_model retry_stream_collected{
      retry_async_model_impl{collected_state},
      wh::model::retry_chat_model_options{.max_attempts = 3U}};

  auto model_vs_collected = wh::compose::make_component_node<wh::compose::component_kind::model,
                                                             wh::compose::node_contract::value,
                                                             wh::compose::node_contract::stream,
                                                             wh::compose::node_exec_mode::async>(
      "retry-vs-collected", retry_stream_collected);

  wh::compose::graph collected_graph{};
  REQUIRE(collected_graph.add_component(model_vs_collected).has_value());
  REQUIRE(collected_graph.add_entry_edge("retry-vs-collected").has_value());
  REQUIRE(collected_graph.add_exit_edge("retry-vs-collected").has_value());
  REQUIRE(collected_graph.compile().has_value());

  wh::compose::graph_invoke_request collected_request{};
  collected_request.input = wh::compose::graph_input::value(make_request());
  wh::core::run_context collected_context{};
  auto collected_waited =
      stdexec::sync_wait(collected_graph.invoke(collected_context, std::move(collected_request)));
  REQUIRE(collected_waited.has_value());
  auto collected_status = std::get<0>(std::move(*collected_waited));
  REQUIRE(collected_status.has_value());
  auto collected_result = std::move(collected_status).value();
  REQUIRE(collected_result.output_status.has_value());
  auto values =
      read_any<std::vector<wh::compose::graph_value>>(std::move(collected_result.output_status).value());
  REQUIRE(values.has_value());
  REQUIRE(values.value().size() == 1U);
  auto collected_message = read_any<wh::schema::message>(std::move(values.value().front()));
  REQUIRE(collected_message.has_value());
  REQUIRE(std::get<wh::schema::text_part>(collected_message.value().parts.front()).text ==
          "retry-async-vs");
  REQUIRE(collected_state->stream_calls == 2U);
}
