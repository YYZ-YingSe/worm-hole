#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <tuple>
#include <utility>

#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/compose/graph.hpp"
#include "wh/core/any.hpp"
#include "wh/core/run_context.hpp"
#include "helper/sender_capture.hpp"

namespace {

[[nodiscard]] auto read_int(wh::compose::graph_value &&value)
    -> wh::core::result<int> {
  if (auto *typed = wh::core::any_cast<int>(&value); typed != nullptr) {
    return std::move(*typed);
  }
  return wh::core::result<int>::failure(wh::core::errc::type_mismatch);
}

template <typename input_t>
[[nodiscard]] auto make_graph_request(input_t &&input)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request request{};
  request.input = wh::compose::graph_value{std::forward<input_t>(input)};
  return request;
}

template <typename input_t>
[[nodiscard]] auto make_graph_request(
    input_t &&input, const wh::compose::graph_call_options &options)
    -> wh::compose::graph_invoke_request {
  auto request = make_graph_request(std::forward<input_t>(input));
  request.controls.call = options;
  return request;
}

struct completion_lifetime_probe {
  bool destroyed{false};
  bool destroyed_before_start_return{false};
};

struct inline_graph_value_probe_sender {
  using result_t = wh::core::result<wh::compose::graph_value>;
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(result_t)>;

  std::shared_ptr<completion_lifetime_probe> probe{};
  int value{0};

  template <stdexec::receiver_of<completion_signatures> receiver_t>
  struct operation {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;
    std::shared_ptr<completion_lifetime_probe> probe{};
    int value{0};

    auto start() & noexcept -> void {
      auto keep_alive = probe;
      stdexec::set_value(std::move(receiver), result_t{wh::core::any(value)});
      if (keep_alive->destroyed) {
        keep_alive->destroyed_before_start_return = true;
      }
    }

    ~operation() { probe->destroyed = true; }
  };

  template <stdexec::receiver_of<completion_signatures> receiver_t>
  auto connect(receiver_t receiver) && -> operation<receiver_t> {
    return operation<receiver_t>{
        .receiver = std::move(receiver),
        .probe = std::move(probe),
        .value = value,
    };
  }
};

} // namespace

TEST_CASE("compose graph typed invoke request returns structured result and cleans context",
          "[core][compose][invoke][condition]") {
  wh::compose::graph graph{};
  auto added = graph.add_lambda<wh::compose::node_contract::value,
                                wh::compose::node_contract::value>(
      "increment",
      [](const wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        auto value = read_int(wh::compose::graph_value{input});
        if (value.has_error()) {
          return wh::core::result<wh::compose::graph_value>::failure(
              value.error());
        }
        return wh::core::any(value.value() + 1);
      });
  REQUIRE(added.has_value());
  REQUIRE(graph.add_entry_edge("increment").has_value());
  REQUIRE(graph.add_exit_edge("increment").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::checkpoint_store store{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = &store;

  wh::compose::graph_invoke_request request{};
  request.input = wh::core::any(7);
  request.services = &services;
  request.controls.call.record_transition_log = true;
  request.controls.call.stream_subscriptions.push_back(
      wh::compose::graph_stream_subscription{
          .kind = wh::compose::graph_stream_channel_kind::debug,
          .enabled = true,
      });
  request.controls.checkpoint.save = wh::compose::checkpoint_save_options{
      .thread_key = "invoke-request",
      .branch = "main",
  };
  wh::core::run_context context{};
  auto invoke_status = wh::testing::helper::wait_value_on_test_thread(
      graph.invoke(context, std::move(request)));
  REQUIRE(invoke_status.has_value());
  auto invoke_result = std::move(invoke_status).value();
  REQUIRE(invoke_result.output_status.has_value());

  auto output = read_int(std::move(invoke_result.output_status).value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 8);

  const auto &report = invoke_result.report;
  REQUIRE_FALSE(report.transition_log.empty());
  REQUIRE(context.session_values.empty());
}

TEST_CASE("compose graph typed invoke request preserves graph failure inside structured result",
          "[core][compose][invoke][boundary]") {
  wh::compose::graph graph{};
  auto added = graph.add_lambda<wh::compose::node_contract::value,
                                wh::compose::node_contract::value>(
      "fail",
      [](const wh::compose::graph_value &, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return wh::core::result<wh::compose::graph_value>::failure(
            wh::core::errc::not_supported);
      });
  REQUIRE(added.has_value());
  REQUIRE(graph.add_entry_edge("fail").has_value());
  REQUIRE(graph.add_exit_edge("fail").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::core::any(1);
  request.controls.call.record_transition_log = true;

  wh::core::run_context context{};
  auto invoke_status = wh::testing::helper::wait_value_on_test_thread(
      graph.invoke(context, std::move(request)));
  REQUIRE(invoke_status.has_value());
  REQUIRE(invoke_status.value().output_status.has_error());
  REQUIRE(invoke_status.value().output_status.error() ==
          wh::core::errc::not_supported);
  REQUIRE(invoke_status.value().report.graph_run_error.has_value());
  REQUIRE(context.session_values.empty());
}

TEST_CASE("compose graph typed invoke request rejects conflicting checkpoint services",
          "[core][compose][invoke][boundary]") {
  wh::compose::graph graph{};
  auto added = graph.add_lambda<wh::compose::node_contract::value,
                                wh::compose::node_contract::value>(
      "passthrough",
      [](const wh::compose::graph_value &input, wh::core::run_context &,
         const wh::compose::graph_call_scope &)
          -> wh::core::result<wh::compose::graph_value> {
        return input;
      });
  REQUIRE(added.has_value());
  REQUIRE(graph.add_entry_edge("passthrough").has_value());
  REQUIRE(graph.add_exit_edge("passthrough").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::core::any(3);
  wh::compose::checkpoint_store store{};
  wh::compose::checkpoint_backend backend{};
  wh::compose::graph_runtime_services services{};
  services.checkpoint.store = &store;
  services.checkpoint.backend = &backend;
  request.services = &services;

  wh::core::run_context context{};
  auto invoke_status = wh::testing::helper::wait_value_on_test_thread(
      graph.invoke(context, std::move(request)));
  REQUIRE(invoke_status.has_value());
  REQUIRE(invoke_status.value().output_status.has_error());
  REQUIRE(invoke_status.value().output_status.error() ==
          wh::core::errc::invalid_argument);
  REQUIRE(invoke_status.value().report.checkpoint_error.has_value());
  REQUIRE(invoke_status.value().report.graph_run_error.has_value());
}

TEST_CASE("compose graph async invoke does not destroy node sender op before callback returns",
          "[core][compose][invoke][runtime]") {
  auto probe = std::make_shared<completion_lifetime_probe>();

  wh::compose::graph graph{};
  auto added = graph.add_lambda<wh::compose::node_contract::value,
                                wh::compose::node_contract::value,
                                wh::compose::node_exec_mode::async>(
      "worker",
      [probe](const wh::compose::graph_value &, wh::core::run_context &,
              const wh::compose::graph_call_scope &) {
        return inline_graph_value_probe_sender{
            .probe = probe,
            .value = 9,
        };
      });
  REQUIRE(added.has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  wh::compose::graph_invoke_request request{};
  request.input = wh::core::any(1);

  wh::core::run_context context{};
  wh::testing::helper::sender_capture<
      wh::core::result<wh::compose::graph_invoke_result>>
      state{};
  auto operation = stdexec::connect(
      graph.invoke(context, std::move(request)),
      wh::testing::helper::sender_capture_receiver{
          &state,
          wh::testing::helper::make_scheduler_env(stdexec::inline_scheduler{}),
      });
  stdexec::start(operation);
  REQUIRE(state.ready.try_acquire_for(std::chrono::milliseconds(100)));
  REQUIRE(state.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(state.value.has_value());
  REQUIRE(state.value->has_value());
  REQUIRE(state.value->value().output_status.has_value());
  auto output = read_int(std::move(state.value->value().output_status).value());
  REQUIRE(output.has_value());
  REQUIRE(output.value() == 9);
  REQUIRE(probe->destroyed);
  REQUIRE_FALSE(probe->destroyed_before_start_return);
}

TEST_CASE("compose graph async invoke supports sender callback and awaitable tokens",
          "[core][compose][graph][token]") {
  wh::compose::graph graph{};
  REQUIRE(graph
              .add_lambda("inc", [](const wh::compose::graph_value &input,
                                    wh::core::run_context &,
                                    const wh::compose::graph_call_scope &)
                            -> wh::core::result<wh::compose::graph_value> {
                auto typed = read_int(wh::compose::graph_value{input});
                if (typed.has_error()) {
                  return wh::core::result<wh::compose::graph_value>::failure(
                      typed.error());
                }
                return wh::core::any(typed.value() + 1);
              })
              .has_value());
  REQUIRE(graph.add_entry_edge("inc").has_value());
  REQUIRE(graph.add_exit_edge("inc").has_value());
  REQUIRE(graph.compile().has_value());

  wh::core::run_context context{};

  auto sender_token =
      graph.invoke(context, make_graph_request(wh::core::any(1)));
  auto sender_waited = stdexec::sync_wait(std::move(sender_token));
  REQUIRE(sender_waited.has_value());
  auto sender_result = std::get<0>(std::move(sender_waited).value());
  REQUIRE(sender_result.has_value());
  REQUIRE(sender_result.value().output_status.has_value());
  auto sender_typed =
      read_int(std::move(sender_result.value().output_status).value());
  REQUIRE(sender_typed.has_value());
  REQUIRE(sender_typed.value() == 2);

  wh::compose::graph_call_options sender_call_options{};
  sender_call_options.trace = wh::compose::graph_trace_context{
      .trace_id = "token-run",
      .parent_span_id = "token-parent",
  };
  auto sender_with_options =
      graph.invoke(context,
                   make_graph_request(wh::core::any(2), sender_call_options));
  auto sender_with_options_waited =
      stdexec::sync_wait(std::move(sender_with_options));
  REQUIRE(sender_with_options_waited.has_value());
  auto sender_with_options_result =
      std::get<0>(std::move(sender_with_options_waited).value());
  REQUIRE(sender_with_options_result.has_value());
  REQUIRE(sender_with_options_result.value().output_status.has_value());
  auto sender_with_options_typed =
      read_int(std::move(sender_with_options_result.value().output_status).value());
  REQUIRE(sender_with_options_typed.has_value());
  REQUIRE(sender_with_options_typed.value() == 3);

  auto second_sender =
      graph.invoke(context, make_graph_request(wh::core::any(9)));
  auto awaitable_waited = stdexec::sync_wait(std::move(second_sender));
  REQUIRE(awaitable_waited.has_value());
  auto awaitable_result = std::get<0>(std::move(awaitable_waited).value());
  REQUIRE(awaitable_result.has_value());
  REQUIRE(awaitable_result.value().output_status.has_value());
  auto awaitable_typed =
      read_int(std::move(awaitable_result.value().output_status).value());
  REQUIRE(awaitable_typed.has_value());
  REQUIRE(awaitable_typed.value() == 10);
}
