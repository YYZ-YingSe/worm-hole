#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/cursor_reader/detail/pull_op.hpp"
#include "wh/core/intrusive_ptr.hpp"
#include "wh/core/stdexec/detail/scheduled_resume_turn.hpp"

namespace {

using result_t = wh::core::result<int>;

struct owner_probe : wh::core::detail::intrusive_enable_from_this<owner_probe> {
  using pending_drain_t = void (*)(void *) noexcept;

  int finish_calls{0};
  int stopped_calls{0};
  std::optional<result_t> last_status{};
  bool last_terminal_override{false};
  std::optional<wh::core::error_code> async_failure_code{};
  void *pending_pull{nullptr};
  pending_drain_t pending_drain{nullptr};
  wh::core::detail::scheduled_resume_turn<owner_probe, exec::trampoline_scheduler> resume_turn{
      exec::trampoline_scheduler{}};

  auto finish_source_pull(void *, result_t status, const bool terminal_override) noexcept -> void {
    ++finish_calls;
    last_status = std::move(status);
    last_terminal_override = terminal_override;
  }

  auto finish_source_pull_stopped(void *) noexcept -> void { ++stopped_calls; }

  [[nodiscard]] auto async_failure(const wh::core::error_code code) noexcept -> result_t {
    async_failure_code = code;
    return result_t::failure(code);
  }

  template <typename pull_t> static auto drain_pending_pull(void *raw) noexcept -> void {
    static_cast<pull_t *>(raw)->drain_completion();
  }

  template <typename pull_t> auto schedule_pull_completion(pull_t *pull) noexcept -> void {
    pending_pull = pull;
    pending_drain = &drain_pending_pull<pull_t>;
    resume_turn.request(this);
  }

  [[nodiscard]] auto resume_turn_completed() const noexcept -> bool { return false; }

  auto resume_turn_run() noexcept -> void {
    auto *raw = pending_pull;
    auto drain = pending_drain;
    pending_pull = nullptr;
    pending_drain = nullptr;
    if (drain != nullptr) {
      drain(raw);
    }
  }

  auto resume_turn_idle() noexcept -> void {}
  auto resume_turn_schedule_error(const wh::core::error_code) noexcept -> void {
    resume_turn_run();
  }
  auto resume_turn_add_ref() noexcept -> void {}
  auto resume_turn_arrive() noexcept -> void {}
};

struct base_async_source {
  [[nodiscard]] auto read() -> result_t { return result_t{0}; }
  [[nodiscard]] auto try_read() -> std::optional<result_t> {
    return std::optional<result_t>{result_t{0}};
  }
  [[nodiscard]] auto close() -> wh::core::result<void> { return {}; }
};

struct success_async_source : base_async_source {
  [[nodiscard]] auto read_async() { return stdexec::just(result_t{7}); }
};

struct error_async_source : base_async_source {
  [[nodiscard]] auto read_async() {
    return stdexec::just_error(std::make_exception_ptr(std::runtime_error{"boom"}));
  }
};

struct coded_error_async_source : base_async_source {
  [[nodiscard]] auto read_async() {
    return stdexec::just_error(wh::core::make_error(wh::core::errc::timeout));
  }
};

struct stoppable_sender {
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<stdexec::set_stopped_t()>;

  template <typename receiver_t> struct operation {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;
    stdexec::inplace_stop_token stop_token{};

    struct callback {
      operation *self{nullptr};

      auto operator()() const noexcept -> void {
        if (self->completed.exchange(true, std::memory_order_acq_rel)) {
          return;
        }
        stdexec::set_stopped(std::move(self->receiver));
      }
    };

    std::optional<stdexec::stop_callback_for_t<stdexec::inplace_stop_token, callback>>
        stop_callback{};
    std::atomic<bool> completed{false};

    auto start() & noexcept -> void {
      stop_callback.emplace(stop_token, callback{this});
      if (stop_token.stop_requested()) {
        callback{this}();
      }
    }
  };

  template <typename receiver_t> auto connect(receiver_t receiver) const -> operation<receiver_t> {
    return operation<receiver_t>{
        .receiver = std::move(receiver),
        .stop_token = stdexec::get_stop_token(stdexec::get_env(receiver)),
    };
  }

  template <typename self_t, typename... env_t>
    requires std::same_as<std::remove_cvref_t<self_t>, stoppable_sender> && (sizeof...(env_t) >= 1U)
  static consteval auto get_completion_signatures() {
    return completion_signatures{};
  }
};

struct stoppable_async_source : base_async_source {
  [[nodiscard]] auto read_async() { return stoppable_sender{}; }
};

struct lifetime_probe_state {
  std::atomic<bool> child_destroyed{false};
  std::atomic<bool> child_destroyed_before_start_return{false};
};

struct lifetime_probe_sender {
  using sender_concept = stdexec::sender_t;
  using completion_signatures = stdexec::completion_signatures<stdexec::set_value_t(result_t)>;

  template <typename receiver_t> struct operation {
    using operation_state_concept = stdexec::operation_state_t;

    receiver_t receiver;
    std::shared_ptr<lifetime_probe_state> state;

    ~operation() { state->child_destroyed.store(true, std::memory_order_release); }

    auto start() & noexcept -> void {
      auto keep_alive = state;
      stdexec::set_value(std::move(receiver), result_t{11});
      if (keep_alive->child_destroyed.load(std::memory_order_acquire)) {
        keep_alive->child_destroyed_before_start_return.store(true, std::memory_order_release);
      }
    }
  };

  template <typename receiver_t> auto connect(receiver_t receiver) const -> operation<receiver_t> {
    return operation<receiver_t>{std::move(receiver), state};
  }

  template <typename self_t, typename... env_t>
    requires std::same_as<std::remove_cvref_t<self_t>, lifetime_probe_sender> &&
             (sizeof...(env_t) >= 1U)
  static consteval auto get_completion_signatures() {
    return completion_signatures{};
  }

  std::shared_ptr<lifetime_probe_state> state;
};

struct lifetime_probe_async_source : base_async_source {
  std::shared_ptr<lifetime_probe_state> state{std::make_shared<lifetime_probe_state>()};

  [[nodiscard]] auto read_async() { return lifetime_probe_sender{state}; }
};

struct lifetime_owner_probe : wh::core::detail::intrusive_enable_from_this<lifetime_owner_probe> {
  using pending_drain_t = void (*)(void *) noexcept;

  int finish_calls{0};
  void *pending_pull{nullptr};
  pending_drain_t pending_drain{nullptr};
  wh::core::detail::scheduled_resume_turn<lifetime_owner_probe, exec::trampoline_scheduler>
      resume_turn{exec::trampoline_scheduler{}};

  auto finish_source_pull(void *, result_t, const bool) noexcept -> void { ++finish_calls; }

  auto finish_source_pull_stopped(void *) noexcept -> void {}

  [[nodiscard]] auto async_failure(const wh::core::error_code code) noexcept -> result_t {
    return result_t::failure(code);
  }

  template <typename pull_t> static auto drain_pending_pull(void *raw) noexcept -> void {
    static_cast<pull_t *>(raw)->drain_completion();
  }

  template <typename pull_t> auto schedule_pull_completion(pull_t *pull) noexcept -> void {
    pending_pull = pull;
    pending_drain = &drain_pending_pull<pull_t>;
    resume_turn.request(this);
  }

  [[nodiscard]] auto resume_turn_completed() const noexcept -> bool { return false; }

  auto resume_turn_run() noexcept -> void {
    auto *raw = pending_pull;
    auto drain = pending_drain;
    pending_pull = nullptr;
    pending_drain = nullptr;
    if (drain != nullptr) {
      drain(raw);
    }
  }

  auto resume_turn_idle() noexcept -> void {}
  auto resume_turn_schedule_error(const wh::core::error_code) noexcept -> void {
    resume_turn_run();
  }
  auto resume_turn_add_ref() noexcept -> void {}
  auto resume_turn_arrive() noexcept -> void {}
};

} // namespace

namespace {

auto drain_trampoline_turn() -> void {
  auto waited = stdexec::sync_wait(stdexec::schedule(exec::trampoline_scheduler{}));
  REQUIRE(waited.has_value());
}

} // namespace

TEST_CASE("pull op forwards successful async source completion to owner and resets storage",
          "[UT][wh/core/cursor_reader/detail/pull_op.hpp][pull_op::start][branch]") {
  auto owner = wh::core::detail::make_intrusive<owner_probe>();
  success_async_source source{};
  auto pull = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::pull_op<owner_probe, success_async_source, result_t>>(*owner);

  pull->start(source, wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));
  drain_trampoline_turn();

  REQUIRE(owner->finish_calls == 1);
  REQUIRE(owner->last_status.has_value());
  REQUIRE(owner->last_status->has_value());
  REQUIRE(owner->last_status->value() == 7);
  REQUIRE_FALSE(owner->last_terminal_override);
}

TEST_CASE("pull op maps async source errors through owner async failure path",
          "[UT][wh/core/cursor_reader/detail/pull_op.hpp][pull_op::receiver::set_error][branch]") {
  auto owner = wh::core::detail::make_intrusive<owner_probe>();
  error_async_source source{};
  auto pull = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::pull_op<owner_probe, error_async_source, result_t>>(*owner);

  pull->start(source, wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));
  drain_trampoline_turn();

  REQUIRE(owner->finish_calls == 1);
  REQUIRE(owner->last_status.has_value());
  REQUIRE(owner->last_status->has_error());
  REQUIRE(owner->async_failure_code.has_value());
  REQUIRE(*owner->async_failure_code == wh::core::errc::internal_error);
  REQUIRE(owner->last_terminal_override);
}

TEST_CASE(
    "pull op request_stop propagates stop to async sender and reports stopped once",
    "[UT][wh/core/cursor_reader/detail/pull_op.hpp][pull_op::request_stop][concurrency][branch]") {
  auto owner = wh::core::detail::make_intrusive<owner_probe>();
  stoppable_async_source source{};
  auto pull = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::pull_op<owner_probe, stoppable_async_source, result_t>>(
      *owner);

  pull->start(source, wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));
  REQUIRE(owner->finish_calls == 0);
  REQUIRE(owner->stopped_calls == 0);

  pull->request_stop();
  drain_trampoline_turn();

  REQUIRE(owner->stopped_calls == 1);
  REQUIRE(owner->finish_calls == 0);
}

TEST_CASE("pull op maps direct error codes and ignores request_stop without a stop source",
          "[UT][wh/core/cursor_reader/detail/"
          "pull_op.hpp][pull_op::receiver::set_error][condition][branch][boundary]") {
  auto owner = wh::core::detail::make_intrusive<owner_probe>();
  coded_error_async_source source{};
  auto pull = wh::core::detail::make_intrusive<
      wh::core::cursor_reader_detail::pull_op<owner_probe, coded_error_async_source, result_t>>(
      *owner);

  pull->request_stop();
  pull->start(source, wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));
  drain_trampoline_turn();

  REQUIRE(owner->finish_calls == 1);
  REQUIRE(owner->last_status.has_value());
  REQUIRE(owner->last_status->has_error());
  REQUIRE(owner->last_status->error() == wh::core::errc::timeout);
  REQUIRE(owner->async_failure_code.has_value());
  REQUIRE(*owner->async_failure_code == wh::core::errc::timeout);
  REQUIRE(owner->last_terminal_override);
}

TEST_CASE("pull op keeps child operation alive until inline completion callback returns",
          "[UT][wh/core/cursor_reader/detail/pull_op.hpp][pull_op::start][lifetime][regression]") {
  auto owner = wh::core::detail::make_intrusive<lifetime_owner_probe>();
  lifetime_probe_async_source source{};
  auto pull = wh::core::detail::make_intrusive<wh::core::cursor_reader_detail::pull_op<
      lifetime_owner_probe, lifetime_probe_async_source, result_t>>(*owner);

  pull->start(source, wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));
  drain_trampoline_turn();

  REQUIRE(owner->finish_calls == 1);
  REQUIRE_FALSE(source.state->child_destroyed_before_start_return.load(std::memory_order_acquire));
  REQUIRE(source.state->child_destroyed.load(std::memory_order_acquire));
}
