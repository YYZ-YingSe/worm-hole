#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <exec/static_thread_pool.hpp>
#include <stdexec/execution.hpp>

#include "helper/component_contract_support.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/prompt/simple_chat_template.hpp"

namespace {

using wh::testing::helper::make_user_message;
using wh::testing::helper::register_test_callbacks;

} // namespace

TEST_CASE("component async wrappers enter sender-only path on caller scheduler",
          "[core][prompt][functional]") {
  struct scheduler_observing_prompt_impl {
    std::shared_ptr<std::thread::id> observed_thread{};

    [[nodiscard]] auto render_sender(wh::prompt::prompt_render_request) const {
      return stdexec::just() |
             stdexec::then([observed_thread = observed_thread]() {
               *observed_thread = std::this_thread::get_id();
               std::vector<wh::schema::message> messages{};
               messages.push_back(make_user_message("scheduled"));
               return wh::core::result<std::vector<wh::schema::message>>{
                   std::move(messages)};
             });
    }
  };

  auto observed_thread = std::make_shared<std::thread::id>();
  wh::prompt::chat_template component{
      scheduler_observing_prompt_impl{observed_thread}};
  exec::static_thread_pool pool{1U};

  std::thread::id scheduler_thread{};
  auto scheduler_waited =
      stdexec::sync_wait(stdexec::schedule(pool.get_scheduler()) |
                         stdexec::then([&]() {
                           scheduler_thread = std::this_thread::get_id();
                           return 0;
                         }));
  REQUIRE(scheduler_waited.has_value());

  wh::core::run_context callback_context{};
  wh::prompt::prompt_render_request request{};
  std::thread::id completion_thread{};
  auto waited = stdexec::sync_wait(
      stdexec::starts_on(pool.get_scheduler(),
                         component.async_render(std::move(request),
                                                callback_context)) |
      stdexec::then(
          [&](wh::core::result<std::vector<wh::schema::message>> status) {
            completion_thread = std::this_thread::get_id();
            return status;
          }));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(waited).value());
  REQUIRE(status.has_value());
  REQUIRE(status.value().size() == 1U);
  REQUIRE(std::get<wh::schema::text_part>(status.value().front().parts.front())
              .text == "scheduled");
  REQUIRE(*observed_thread == scheduler_thread);
  REQUIRE(completion_thread == scheduler_thread);
}

TEST_CASE("component async impl can hop to worker scheduler and explicitly resume",
          "[core][prompt][functional]") {
  struct explicit_resume_prompt_impl {
    exec::static_thread_pool::scheduler worker_scheduler;
    std::shared_ptr<std::thread::id> worker_thread{};
    std::shared_ptr<std::thread::id> resumed_thread{};

    [[nodiscard]] auto render_sender(wh::prompt::prompt_render_request) const {
      return wh::core::read_resume_scheduler(
          [worker_scheduler = worker_scheduler, worker_thread = worker_thread,
           resumed_thread = resumed_thread](auto resume_scheduler) mutable {
            auto worker = stdexec::starts_on(
                worker_scheduler,
                stdexec::just() | stdexec::then([worker_thread]() {
                  *worker_thread = std::this_thread::get_id();
                  std::vector<wh::schema::message> messages{};
                  messages.push_back(make_user_message("resumed"));
                  return wh::core::result<std::vector<wh::schema::message>>{
                      std::move(messages)};
                }));
            return wh::core::resume_on(std::move(worker),
                                       std::move(resume_scheduler)) |
                   stdexec::then([resumed_thread](
                                     wh::core::result<
                                         std::vector<wh::schema::message>>
                                         status) {
                     *resumed_thread = std::this_thread::get_id();
                     return status;
                   });
          });
    }
  };

  exec::static_thread_pool caller_pool{1U};
  exec::static_thread_pool worker_pool{1U};
  auto worker_thread = std::make_shared<std::thread::id>();
  auto resumed_thread = std::make_shared<std::thread::id>();
  wh::prompt::chat_template component{explicit_resume_prompt_impl{
      worker_pool.get_scheduler(), worker_thread, resumed_thread}};

  std::thread::id caller_scheduler_thread{};
  auto caller_ready = stdexec::sync_wait(
      stdexec::schedule(caller_pool.get_scheduler()) | stdexec::then([&]() {
        caller_scheduler_thread = std::this_thread::get_id();
        return 0;
      }));
  REQUIRE(caller_ready.has_value());

  std::thread::id worker_scheduler_thread{};
  auto worker_ready = stdexec::sync_wait(
      stdexec::schedule(worker_pool.get_scheduler()) | stdexec::then([&]() {
        worker_scheduler_thread = std::this_thread::get_id();
        return 0;
      }));
  REQUIRE(worker_ready.has_value());

  wh::core::run_context context{};
  wh::prompt::prompt_render_request request{};
  std::thread::id completion_thread{};
  auto waited = stdexec::sync_wait(
      stdexec::starts_on(caller_pool.get_scheduler(),
                         component.async_render(std::move(request), context)) |
      stdexec::then(
          [&](wh::core::result<std::vector<wh::schema::message>> status) {
            completion_thread = std::this_thread::get_id();
            return status;
          }));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(waited).value());
  REQUIRE(status.has_value());
  REQUIRE(status.value().size() == 1U);
  REQUIRE(std::get<wh::schema::text_part>(status.value().front().parts.front())
              .text == "resumed");
  REQUIRE(*worker_thread == worker_scheduler_thread);
  REQUIRE(*resumed_thread == caller_scheduler_thread);
  REQUIRE(completion_thread == caller_scheduler_thread);
}

TEST_CASE("component wrapper keeps worker completion when resume mode is unchanged",
          "[core][prompt][functional]") {
  struct worker_prompt_impl {
    exec::static_thread_pool::scheduler worker_scheduler;
    std::shared_ptr<std::thread::id> worker_thread{};

    [[nodiscard]] auto render_sender(wh::prompt::prompt_render_request) const {
      return stdexec::starts_on(
          worker_scheduler, stdexec::just() |
                                stdexec::then([worker_thread = worker_thread]() {
                                  *worker_thread = std::this_thread::get_id();
                                  std::vector<wh::schema::message> messages{};
                                  messages.push_back(make_user_message("worker"));
                                  return wh::core::result<
                                      std::vector<wh::schema::message>>{
                                      std::move(messages)};
                                }));
    }
  };

  // The worker scheduler must outlive the caller pool because caller-side
  // teardown can still enqueue work onto the worker scheduler.
  exec::static_thread_pool worker_pool{1U};
  exec::static_thread_pool caller_pool{1U};
  auto worker_thread = std::make_shared<std::thread::id>();
  wh::prompt::chat_template<worker_prompt_impl,
                            wh::core::resume_mode::unchanged>
      component{worker_prompt_impl{worker_pool.get_scheduler(), worker_thread}};

  std::thread::id caller_scheduler_thread{};
  auto caller_ready = stdexec::sync_wait(
      stdexec::schedule(caller_pool.get_scheduler()) | stdexec::then([&]() {
        caller_scheduler_thread = std::this_thread::get_id();
        return 0;
      }));
  REQUIRE(caller_ready.has_value());

  std::thread::id worker_scheduler_thread{};
  auto worker_ready = stdexec::sync_wait(
      stdexec::schedule(worker_pool.get_scheduler()) | stdexec::then([&]() {
        worker_scheduler_thread = std::this_thread::get_id();
        return 0;
      }));
  REQUIRE(worker_ready.has_value());

  wh::core::run_context context{};
  wh::prompt::prompt_render_request request{};
  std::thread::id completion_thread{};
  auto waited = stdexec::sync_wait(
      stdexec::starts_on(caller_pool.get_scheduler(),
                         component.async_render(std::move(request), context)) |
      stdexec::then(
          [&](wh::core::result<std::vector<wh::schema::message>> status) {
            completion_thread = std::this_thread::get_id();
            return status;
          }));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(waited).value());
  REQUIRE(status.has_value());
  REQUIRE(*worker_thread == worker_scheduler_thread);
  REQUIRE(worker_scheduler_thread != caller_scheduler_thread);
  REQUIRE(completion_thread == worker_scheduler_thread);
}

TEST_CASE("component wrapper can restore caller completion when configured",
          "[core][prompt][functional]") {
  struct worker_prompt_impl {
    exec::static_thread_pool::scheduler worker_scheduler;
    std::shared_ptr<std::thread::id> worker_thread{};

    [[nodiscard]] auto render_sender(wh::prompt::prompt_render_request) const {
      return stdexec::starts_on(
          worker_scheduler, stdexec::just() |
                                stdexec::then([worker_thread = worker_thread]() {
                                  *worker_thread = std::this_thread::get_id();
                                  std::vector<wh::schema::message> messages{};
                                  messages.push_back(
                                      make_user_message("restore"));
                                  return wh::core::result<
                                      std::vector<wh::schema::message>>{
                                      std::move(messages)};
                                }));
    }
  };

  exec::static_thread_pool caller_pool{1U};
  exec::static_thread_pool worker_pool{1U};
  auto worker_thread = std::make_shared<std::thread::id>();
  wh::prompt::chat_template<worker_prompt_impl, wh::core::resume_mode::restore>
      component{worker_prompt_impl{worker_pool.get_scheduler(), worker_thread}};

  std::thread::id caller_scheduler_thread{};
  auto caller_ready = stdexec::sync_wait(
      stdexec::schedule(caller_pool.get_scheduler()) | stdexec::then([&]() {
        caller_scheduler_thread = std::this_thread::get_id();
        return 0;
      }));
  REQUIRE(caller_ready.has_value());

  std::thread::id worker_scheduler_thread{};
  auto worker_ready = stdexec::sync_wait(
      stdexec::schedule(worker_pool.get_scheduler()) | stdexec::then([&]() {
        worker_scheduler_thread = std::this_thread::get_id();
        return 0;
      }));
  REQUIRE(worker_ready.has_value());

  wh::core::run_context context{};
  wh::prompt::prompt_render_request request{};
  std::thread::id completion_thread{};
  auto waited = stdexec::sync_wait(
      stdexec::starts_on(caller_pool.get_scheduler(),
                         component.async_render(std::move(request), context)) |
      stdexec::then(
          [&](wh::core::result<std::vector<wh::schema::message>> status) {
            completion_thread = std::this_thread::get_id();
            return status;
          }));
  REQUIRE(waited.has_value());
  auto status = std::get<0>(std::move(waited).value());
  REQUIRE(status.has_value());
  REQUIRE(*worker_thread == worker_scheduler_thread);
  REQUIRE(worker_scheduler_thread != caller_scheduler_thread);
  REQUIRE(completion_thread == caller_scheduler_thread);
}

TEST_CASE(
    "simple chat template renders in order and enforces strict missing variables",
    "[core][prompt][functional]") {
  wh::prompt::simple_chat_template tpl({
      {wh::schema::message_role::system, "You are {{role}}", "sys"},
      {wh::schema::message_role::user, "Hi {{name}}", "usr"},
  });

  wh::prompt::template_context context{};
  context.emplace("role", wh::prompt::template_value{"assistant"});
  context.emplace("name", wh::prompt::template_value{"alice"});
  wh::core::run_context callback_context{};

  auto rendered = tpl.render({context, {}}, callback_context);
  REQUIRE(rendered.has_value());
  REQUIRE(rendered.value().size() == 2U);
  REQUIRE(std::get<wh::schema::text_part>(rendered.value()[0].parts.front())
              .text == "You are assistant");
  REQUIRE(std::get<wh::schema::text_part>(rendered.value()[1].parts.front())
              .text == "Hi alice");

  wh::prompt::template_context missing_context{};
  missing_context.emplace("role", wh::prompt::template_value{"assistant"});
  auto missing = tpl.render({missing_context, {}}, callback_context);
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE("prompt callbacks run start end and error lifecycle",
          "[core][prompt][functional]") {
  wh::prompt::simple_chat_template tpl({
      {wh::schema::message_role::user, "Hello {{name}}", "usr"},
  });

  std::atomic<bool> started{false};
  std::atomic<bool> ended{false};
  std::atomic<bool> failed{false};

  wh::prompt::prompt_options options{};
  wh::prompt::prompt_common_options common{};
  common.template_name = "tmpl";
  options.set_base(common);

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context),
      [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage,
          const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *extra = event.get_if<wh::prompt::prompt_callback_event>();
        REQUIRE(extra != nullptr);
        if (stage == wh::core::callback_stage::start) {
          REQUIRE(extra->template_name == "tmpl");
          started.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          REQUIRE(extra->rendered_message_count == 1U);
          ended.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          REQUIRE(extra->failed_template == "usr");
          REQUIRE(extra->failed_variable == "name");
          failed.store(true, std::memory_order_release);
        }
      },
      "prompt-lifecycle");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  wh::prompt::template_context placeholder_values{};
  placeholder_values.emplace("name", wh::prompt::template_value{"bob"});
  auto ok = tpl.render({placeholder_values, options}, callback_context);
  REQUIRE(ok.has_value());
  REQUIRE(started.load(std::memory_order_acquire));
  REQUIRE(ended.load(std::memory_order_acquire));
  REQUIRE_FALSE(failed.load(std::memory_order_acquire));

  wh::prompt::template_context missing{};
  started.store(false, std::memory_order_release);
  ended.store(false, std::memory_order_release);
  failed.store(false, std::memory_order_release);
  auto err = tpl.render({missing, options}, callback_context);
  REQUIRE(err.has_error());
  REQUIRE(started.load(std::memory_order_acquire));
  REQUIRE_FALSE(ended.load(std::memory_order_acquire));
  REQUIRE(failed.load(std::memory_order_acquire));
}

TEST_CASE("prompt descriptor exposes stable prompt type name",
          "[core][prompt][functional]") {
  wh::prompt::simple_chat_template tpl{};
  REQUIRE(tpl.descriptor().type_name == "Prompt");
}
