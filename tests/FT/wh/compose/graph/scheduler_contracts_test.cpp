#include <catch2/catch_test_macros.hpp>

#include <exec/static_thread_pool.hpp>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include <stdexec/execution.hpp>

#include "helper/compose_graph_test_utils.hpp"
#include "wh/compose/graph.hpp"
#include "wh/core/stdexec/resume_scheduler.hpp"

namespace {

template <typename value_t>
[[nodiscard]] auto read_any(const wh::core::any &value)
    -> wh::core::result<value_t> {
  if (const auto *typed = wh::core::any_cast<value_t>(&value);
      typed != nullptr) {
    if constexpr (std::copy_constructible<value_t>) {
      return *typed;
    } else {
      return wh::core::result<value_t>::failure(wh::core::errc::not_supported);
    }
  }
  return wh::core::result<value_t>::failure(wh::core::errc::type_mismatch);
}

struct graph_test_receiver_state {
  std::mutex mutex{};
  std::condition_variable cv{};
  bool done{false};
  int value_count{0};
  int error_count{0};
  int stopped_count{0};
  std::optional<wh::core::result<wh::compose::graph_invoke_result>> status{};
};

template <typename launch_scheduler_t, typename completion_scheduler_t,
          typename stop_token_t = stdexec::never_stop_token>
struct graph_scheduler_receiver {
  using receiver_concept = stdexec::receiver_t;

  struct env {
    stop_token_t stop_token{};
    launch_scheduler_t launch_scheduler{};
    completion_scheduler_t completion_scheduler{};

    [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept
        -> stop_token_t {
      return stop_token;
    }

    [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
        -> launch_scheduler_t {
      return launch_scheduler;
    }

    template <typename cpo_t>
    [[nodiscard]] auto
    query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
        -> completion_scheduler_t {
      return completion_scheduler;
    }
  };

  std::shared_ptr<graph_test_receiver_state> state{};
  env receiver_env{};

  auto set_value(wh::core::result<wh::compose::graph_invoke_result> status) && noexcept
      -> void {
    std::lock_guard lock{state->mutex};
    ++state->value_count;
    state->status.emplace(std::move(status));
    state->done = true;
    state->cv.notify_all();
  }

  auto set_error(std::exception_ptr) && noexcept -> void {
    std::lock_guard lock{state->mutex};
    ++state->error_count;
    state->done = true;
    state->cv.notify_all();
  }

  auto set_stopped() && noexcept -> void {
    std::lock_guard lock{state->mutex};
    ++state->stopped_count;
    state->done = true;
    state->cv.notify_all();
  }

  [[nodiscard]] auto get_env() const noexcept -> env { return receiver_env; }
};

template <typename launch_scheduler_t, typename completion_scheduler_t>
struct dual_scheduler_env {
  launch_scheduler_t launch_scheduler;
  completion_scheduler_t completion_scheduler;

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept
      -> launch_scheduler_t {
    return launch_scheduler;
  }

  template <typename cpo_t>
  [[nodiscard]] auto
  query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
      -> completion_scheduler_t {
    return completion_scheduler;
  }
};

[[nodiscard]] auto wait_for_graph_receiver(
    const std::shared_ptr<graph_test_receiver_state> &state,
    const std::chrono::milliseconds timeout = std::chrono::milliseconds{500})
    -> bool {
  std::unique_lock lock{state->mutex};
  return state->cv.wait_for(lock, timeout, [&] { return state->done; });
}

template <stdexec::scheduler scheduler_t>
[[nodiscard]] auto scheduler_thread_id(scheduler_t scheduler) -> std::thread::id {
  auto waited = stdexec::sync_wait(stdexec::schedule(std::move(scheduler)) |
                                   stdexec::then([] {
                                     return std::this_thread::get_id();
                                   }));
  REQUIRE(waited.has_value());
  return std::get<0>(waited.value());
}

template <typename input_t>
[[nodiscard]] auto make_graph_request(input_t &&input)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request request{};
  request.input =
      wh::compose::graph_input::value(std::forward<input_t>(input));
  return request;
}

[[nodiscard]] auto
make_graph_request(wh::compose::graph_value input)
    -> wh::compose::graph_invoke_request {
  wh::compose::graph_invoke_request request{};
  if (auto *reader = wh::core::any_cast<wh::compose::graph_stream_reader>(&input);
      reader != nullptr) {
    request.input = wh::compose::graph_input::stream(std::move(*reader));
  } else {
    request.input = wh::compose::graph_input::value(std::move(input));
  }
  return request;
}

} // namespace

TEST_CASE("compose graph runtime binds launch scheduler instead of completion scheduler",
          "[core][compose][graph][scheduler]") {
  wh::compose::graph graph{};
  std::mutex observed_mutex{};
  std::optional<std::thread::id> observed_thread{};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "worker",
                  [&](const wh::compose::graph_value &, wh::core::run_context &,
                      const wh::compose::graph_call_scope &) {
                    std::lock_guard lock{observed_mutex};
                    observed_thread = std::this_thread::get_id();
                    return stdexec::just(
                        wh::core::result<wh::compose::graph_value>{
                            wh::core::any(7)});
                  })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_exit_edge("worker").has_value());
  REQUIRE(graph.compile().has_value());

  exec::static_thread_pool launch_pool{1U};
  exec::static_thread_pool completion_pool{1U};
  const auto launch_thread = scheduler_thread_id(launch_pool.get_scheduler());
  const auto completion_thread =
      scheduler_thread_id(completion_pool.get_scheduler());
  REQUIRE(launch_thread != completion_thread);

  using launch_scheduler_t =
      std::remove_cvref_t<decltype(launch_pool.get_scheduler())>;
  using completion_scheduler_t =
      std::remove_cvref_t<decltype(completion_pool.get_scheduler())>;

  wh::core::run_context context{};
  auto receiver_state = std::make_shared<graph_test_receiver_state>();
  auto receiver =
      graph_scheduler_receiver<launch_scheduler_t, completion_scheduler_t>{
          .state = receiver_state,
          .receiver_env =
              {
                  .stop_token = stdexec::never_stop_token{},
                  .launch_scheduler = launch_pool.get_scheduler(),
                  .completion_scheduler = completion_pool.get_scheduler(),
              },
  };

  auto op = stdexec::connect(graph.invoke(context, make_graph_request(wh::core::any(1))),
                             std::move(receiver));
  stdexec::start(op);

  REQUIRE(wait_for_graph_receiver(receiver_state));
  REQUIRE(receiver_state->value_count == 1);
  REQUIRE(receiver_state->error_count == 0);
  REQUIRE(receiver_state->stopped_count == 0);
  REQUIRE(receiver_state->status.has_value());
  REQUIRE(receiver_state->status->has_value());
  REQUIRE(receiver_state->status->value().output_status.has_value());

  std::optional<std::thread::id> node_thread{};
  {
    std::lock_guard lock{observed_mutex};
    node_thread = observed_thread;
  }
  REQUIRE(node_thread.has_value());
  REQUIRE(*node_thread == launch_thread);
  REQUIRE(*node_thread != completion_thread);
}

TEST_CASE("scheduler selectors separate launch and completion semantics",
          "[core][stdexec][scheduler]") {
  exec::static_thread_pool completion_pool{1U};
  using env_t = dual_scheduler_env<stdexec::inline_scheduler,
                                   exec::static_thread_pool::scheduler>;
  static_assert(std::same_as<wh::core::detail::resume_scheduler_t<env_t>,
                             exec::static_thread_pool::scheduler>);
  static_assert(std::same_as<wh::core::detail::launch_scheduler_t<env_t>,
                             stdexec::inline_scheduler>);

  const auto completion_thread =
      scheduler_thread_id(completion_pool.get_scheduler());
  const auto test_thread = std::this_thread::get_id();
  const env_t env{
      .launch_scheduler = stdexec::inline_scheduler{},
      .completion_scheduler = completion_pool.get_scheduler(),
  };

  const auto selected_resume_thread =
      scheduler_thread_id(wh::core::detail::get_resume_scheduler(env));
  const auto selected_launch_thread =
      scheduler_thread_id(wh::core::detail::get_launch_scheduler(env));

  REQUIRE(selected_resume_thread == completion_thread);
  REQUIRE(selected_launch_thread == test_thread);
  REQUIRE(selected_launch_thread != selected_resume_thread);
}

TEST_CASE("compose graph returns to graph scheduler at node boundary after impl switches away",
          "[core][compose][graph][scheduler][boundary]") {
  wh::compose::graph graph{};
  std::mutex observed_mutex{};
  std::optional<std::thread::id> worker_thread{};
  std::optional<std::thread::id> downstream_thread{};

  exec::static_thread_pool worker_pool{1U};
  const auto worker_scheduler_thread =
      scheduler_thread_id(worker_pool.get_scheduler());

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "worker",
                  [&](const wh::compose::graph_value &, wh::core::run_context &,
                      const wh::compose::graph_call_scope &) {
                    return wh::compose::detail::normalize_graph_sender(
                        stdexec::starts_on(
                            worker_pool.get_scheduler(),
                            stdexec::just() |
                                stdexec::then([&]() {
                                  std::lock_guard lock{observed_mutex};
                                  worker_thread = std::this_thread::get_id();
                                  return wh::core::result<wh::compose::graph_value>{
                                      wh::core::any(7)};
                                })));
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda("downstream",
                          [&](const wh::compose::graph_value &input,
                              wh::core::run_context &,
                              const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            std::lock_guard lock{observed_mutex};
                            downstream_thread = std::this_thread::get_id();
                            return input;
                          })
              .has_value());
  REQUIRE(graph.add_entry_edge("worker").has_value());
  REQUIRE(graph.add_edge("worker", "downstream").has_value());
  REQUIRE(graph.add_exit_edge("downstream").has_value());
  REQUIRE(graph.compile().has_value());

  exec::static_thread_pool launch_pool{1U};
  exec::static_thread_pool completion_pool{1U};
  const auto launch_thread = scheduler_thread_id(launch_pool.get_scheduler());
  const auto completion_thread =
      scheduler_thread_id(completion_pool.get_scheduler());
  REQUIRE(launch_thread != completion_thread);
  REQUIRE(launch_thread != worker_scheduler_thread);

  using launch_scheduler_t =
      std::remove_cvref_t<decltype(launch_pool.get_scheduler())>;
  using completion_scheduler_t =
      std::remove_cvref_t<decltype(completion_pool.get_scheduler())>;

  wh::core::run_context context{};
  auto receiver_state = std::make_shared<graph_test_receiver_state>();
  auto receiver =
      graph_scheduler_receiver<launch_scheduler_t, completion_scheduler_t>{
          .state = receiver_state,
          .receiver_env =
              {
                  .stop_token = stdexec::never_stop_token{},
                  .launch_scheduler = launch_pool.get_scheduler(),
                  .completion_scheduler = completion_pool.get_scheduler(),
              },
  };

  auto op = stdexec::connect(graph.invoke(context, make_graph_request(wh::core::any(1))),
                             std::move(receiver));
  stdexec::start(op);

  REQUIRE(wait_for_graph_receiver(receiver_state));
  REQUIRE(receiver_state->value_count == 1);
  REQUIRE(receiver_state->error_count == 0);
  REQUIRE(receiver_state->stopped_count == 0);
  REQUIRE(receiver_state->status.has_value());
  REQUIRE(receiver_state->status->has_value());
  REQUIRE(receiver_state->status->value().output_status.has_value());

  std::optional<std::thread::id> observed_worker_thread{};
  std::optional<std::thread::id> observed_downstream_thread{};
  {
    std::lock_guard lock{observed_mutex};
    observed_worker_thread = worker_thread;
    observed_downstream_thread = downstream_thread;
  }
  REQUIRE(observed_worker_thread.has_value());
  REQUIRE(observed_downstream_thread.has_value());
  REQUIRE(*observed_worker_thread == worker_scheduler_thread);
  REQUIRE(*observed_worker_thread != launch_thread);
  REQUIRE(*observed_worker_thread != completion_thread);
  REQUIRE(*observed_downstream_thread == launch_thread);
  REQUIRE(*observed_downstream_thread != worker_scheduler_thread);
  REQUIRE(*observed_downstream_thread != completion_thread);
}

TEST_CASE("compose graph explicit invoke schedulers split control and work execution",
          "[core][compose][graph][scheduler][split]") {
  wh::compose::graph graph{};
  std::mutex observed_mutex{};
  std::optional<std::thread::id> async_thread{};
  std::optional<std::thread::id> sync_work_thread{};
  std::optional<std::thread::id> sync_control_thread{};

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "async_work",
                  [&](const wh::compose::graph_value &input,
                      wh::core::run_context &,
                      const wh::compose::graph_call_scope &) {
                    std::lock_guard lock{observed_mutex};
                    async_thread = std::this_thread::get_id();
                    return stdexec::just(
                        wh::core::result<wh::compose::graph_value>{input});
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "sync_work",
                  [&](const wh::compose::graph_value &input,
                      wh::core::run_context &,
                      const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    std::lock_guard lock{observed_mutex};
                    sync_work_thread = std::this_thread::get_id();
                    return input;
                  })
              .has_value());

  wh::compose::graph_add_node_options inline_control{};
  inline_control.sync_dispatch = wh::compose::sync_dispatch::inline_control;
  REQUIRE(graph
              .add_lambda(
                  "sync_control",
                  [&](const wh::compose::graph_value &input,
                      wh::core::run_context &,
                      const wh::compose::graph_call_scope &)
                      -> wh::core::result<wh::compose::graph_value> {
                    std::lock_guard lock{observed_mutex};
                    sync_control_thread = std::this_thread::get_id();
                    return input;
                  },
                  inline_control)
              .has_value());

  REQUIRE(graph.add_entry_edge("async_work").has_value());
  REQUIRE(graph.add_edge("async_work", "sync_work").has_value());
  REQUIRE(graph.add_edge("sync_work", "sync_control").has_value());
  REQUIRE(graph.add_exit_edge("sync_control").has_value());
  REQUIRE(graph.compile().has_value());

  exec::static_thread_pool control_pool{1U};
  exec::static_thread_pool work_pool{1U};
  exec::static_thread_pool launch_pool{1U};
  exec::static_thread_pool completion_pool{1U};
  const auto control_thread = scheduler_thread_id(control_pool.get_scheduler());
  const auto work_thread = scheduler_thread_id(work_pool.get_scheduler());
  const auto launch_thread = scheduler_thread_id(launch_pool.get_scheduler());
  const auto completion_thread =
      scheduler_thread_id(completion_pool.get_scheduler());
  REQUIRE(control_thread != work_thread);
  REQUIRE(control_thread != launch_thread);
  REQUIRE(control_thread != completion_thread);
  REQUIRE(work_thread != launch_thread);
  REQUIRE(work_thread != completion_thread);

  using launch_scheduler_t =
      std::remove_cvref_t<decltype(launch_pool.get_scheduler())>;
  using completion_scheduler_t =
      std::remove_cvref_t<decltype(completion_pool.get_scheduler())>;

  wh::core::run_context context{};
  wh::compose::graph_invoke_schedulers schedulers{};
  schedulers.set_control_scheduler(control_pool.get_scheduler())
      .set_work_scheduler(work_pool.get_scheduler());

  auto receiver_state = std::make_shared<graph_test_receiver_state>();
  auto receiver =
      graph_scheduler_receiver<launch_scheduler_t, completion_scheduler_t>{
          .state = receiver_state,
          .receiver_env =
              {
                  .stop_token = stdexec::never_stop_token{},
                  .launch_scheduler = launch_pool.get_scheduler(),
                  .completion_scheduler = completion_pool.get_scheduler(),
              },
      };

  auto op = stdexec::connect(
      graph.invoke(context, make_graph_request(wh::core::any(1)),
                   std::move(schedulers)),
      std::move(receiver));
  stdexec::start(op);

  REQUIRE(wait_for_graph_receiver(receiver_state));
  REQUIRE(receiver_state->value_count == 1);
  REQUIRE(receiver_state->error_count == 0);
  REQUIRE(receiver_state->stopped_count == 0);
  REQUIRE(receiver_state->status.has_value());
  REQUIRE(receiver_state->status->has_value());
  REQUIRE(receiver_state->status->value().output_status.has_value());

  std::optional<std::thread::id> observed_async_thread{};
  std::optional<std::thread::id> observed_sync_work_thread{};
  std::optional<std::thread::id> observed_sync_control_thread{};
  {
    std::lock_guard lock{observed_mutex};
    observed_async_thread = async_thread;
    observed_sync_work_thread = sync_work_thread;
    observed_sync_control_thread = sync_control_thread;
  }
  REQUIRE(observed_async_thread.has_value());
  REQUIRE(observed_sync_work_thread.has_value());
  REQUIRE(observed_sync_control_thread.has_value());
  REQUIRE(*observed_async_thread == work_thread);
  REQUIRE(*observed_sync_work_thread == work_thread);
  REQUIRE(*observed_sync_control_thread == control_thread);
  REQUIRE(*observed_async_thread != launch_thread);
  REQUIRE(*observed_async_thread != completion_thread);
  REQUIRE(*observed_sync_control_thread != launch_thread);
  REQUIRE(*observed_sync_control_thread != completion_thread);
}

TEST_CASE("compose graph keeps async node internal resume on work scheduler",
          "[core][compose][graph][scheduler][restore]") {
  wh::compose::graph graph{};
  std::mutex observed_mutex{};
  std::optional<std::thread::id> worker_thread{};
  std::optional<std::thread::id> resumed_thread{};
  std::optional<std::thread::id> downstream_thread{};

  exec::static_thread_pool worker_pool{1U};
  const auto worker_scheduler_thread =
      scheduler_thread_id(worker_pool.get_scheduler());

  REQUIRE(graph
              .add_lambda<wh::compose::node_contract::value,
                          wh::compose::node_contract::value,
                          wh::compose::node_exec_mode::async>(
                  "restore",
                  [&](const wh::compose::graph_value &, wh::core::run_context &,
                      const wh::compose::graph_call_scope &) {
                    return wh::core::read_resume_scheduler(
                        [&](auto resume_scheduler) {
                          auto worker = stdexec::starts_on(
                              worker_pool.get_scheduler(),
                              stdexec::just() |
                                  stdexec::then([&]() {
                                    std::lock_guard lock{observed_mutex};
                                    worker_thread = std::this_thread::get_id();
                                    return wh::core::result<wh::compose::graph_value>{
                                        wh::core::any(7)};
                                  }));
                          return wh::core::resume_on(std::move(worker),
                                                     std::move(resume_scheduler)) |
                                 stdexec::then(
                                     [&](wh::core::result<wh::compose::graph_value>
                                             status) {
                                       std::lock_guard lock{observed_mutex};
                                       resumed_thread =
                                           std::this_thread::get_id();
                                       return status;
                                     });
                        });
                  })
              .has_value());
  REQUIRE(graph
              .add_lambda(
                  "downstream",
                          [&](const wh::compose::graph_value &input,
                              wh::core::run_context &,
                              const wh::compose::graph_call_scope &)
                              -> wh::core::result<wh::compose::graph_value> {
                            std::lock_guard lock{observed_mutex};
                            downstream_thread = std::this_thread::get_id();
                            return input;
                          },
                  wh::compose::graph_add_node_options{
                      .sync_dispatch =
                          wh::compose::sync_dispatch::inline_control})
              .has_value());
  REQUIRE(graph.add_entry_edge("restore").has_value());
  REQUIRE(graph.add_edge("restore", "downstream").has_value());
  REQUIRE(graph.add_exit_edge("downstream").has_value());
  REQUIRE(graph.compile().has_value());

  exec::static_thread_pool control_pool{1U};
  exec::static_thread_pool work_pool{1U};
  exec::static_thread_pool launch_pool{1U};
  exec::static_thread_pool completion_pool{1U};
  const auto control_thread = scheduler_thread_id(control_pool.get_scheduler());
  const auto work_thread = scheduler_thread_id(work_pool.get_scheduler());
  const auto launch_thread = scheduler_thread_id(launch_pool.get_scheduler());
  const auto completion_thread =
      scheduler_thread_id(completion_pool.get_scheduler());
  REQUIRE(launch_thread != completion_thread);
  REQUIRE(control_thread != completion_thread);
  REQUIRE(control_thread != launch_thread);
  REQUIRE(control_thread != worker_scheduler_thread);
  REQUIRE(work_thread != control_thread);

  using launch_scheduler_t =
      std::remove_cvref_t<decltype(launch_pool.get_scheduler())>;
  using completion_scheduler_t =
      std::remove_cvref_t<decltype(completion_pool.get_scheduler())>;

  wh::core::run_context context{};
  wh::compose::graph_invoke_schedulers schedulers{};
  schedulers.set_control_scheduler(control_pool.get_scheduler())
      .set_work_scheduler(work_pool.get_scheduler());
  auto receiver_state = std::make_shared<graph_test_receiver_state>();
  auto receiver =
      graph_scheduler_receiver<launch_scheduler_t, completion_scheduler_t>{
          .state = receiver_state,
          .receiver_env =
              {
                  .stop_token = stdexec::never_stop_token{},
                  .launch_scheduler = launch_pool.get_scheduler(),
                  .completion_scheduler = completion_pool.get_scheduler(),
              },
  };

  auto op = stdexec::connect(
      graph.invoke(context, make_graph_request(wh::core::any(1)),
                   std::move(schedulers)),
                             std::move(receiver));
  stdexec::start(op);

  REQUIRE(wait_for_graph_receiver(receiver_state));
  REQUIRE(receiver_state->value_count == 1);
  REQUIRE(receiver_state->error_count == 0);
  REQUIRE(receiver_state->stopped_count == 0);
  REQUIRE(receiver_state->status.has_value());
  REQUIRE(receiver_state->status->has_value());
  REQUIRE(receiver_state->status->value().output_status.has_value());

  std::optional<std::thread::id> observed_worker_thread{};
  std::optional<std::thread::id> observed_resumed_thread{};
  std::optional<std::thread::id> observed_downstream_thread{};
  {
    std::lock_guard lock{observed_mutex};
    observed_worker_thread = worker_thread;
    observed_resumed_thread = resumed_thread;
    observed_downstream_thread = downstream_thread;
  }
  REQUIRE(observed_worker_thread.has_value());
  REQUIRE(observed_resumed_thread.has_value());
  REQUIRE(observed_downstream_thread.has_value());
  REQUIRE(*observed_worker_thread == worker_scheduler_thread);
  REQUIRE(*observed_worker_thread != work_thread);
  REQUIRE(*observed_worker_thread != control_thread);
  REQUIRE(*observed_worker_thread != launch_thread);
  REQUIRE(*observed_resumed_thread == work_thread);
  REQUIRE(*observed_resumed_thread != worker_scheduler_thread);
  REQUIRE(*observed_resumed_thread != control_thread);
  REQUIRE(*observed_resumed_thread != completion_thread);
  REQUIRE(*observed_downstream_thread == control_thread);
}
