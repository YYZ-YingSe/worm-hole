#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>

#include <stdexec/execution.hpp>

#include "wh/core/cursor_reader/detail/pull_op.hpp"

namespace {

using result_t = wh::core::result<int>;

struct owner_probe {
  int finish_calls{0};
  int stopped_calls{0};
  int reset_calls{0};
  std::optional<result_t> last_status{};
  bool last_terminal_override{false};
  std::optional<wh::core::error_code> async_failure_code{};

  auto finish_source_pull(void *, result_t status,
                          const bool terminal_override) noexcept -> void {
    ++finish_calls;
    last_status = std::move(status);
    last_terminal_override = terminal_override;
  }

  auto finish_source_pull_stopped(void *) noexcept -> void { ++stopped_calls; }

  auto reset_active_pull(void *) noexcept -> void { ++reset_calls; }

  [[nodiscard]] auto async_failure(const wh::core::error_code code) noexcept
      -> result_t {
    async_failure_code = code;
    return result_t::failure(code);
  }
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
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_stopped_t()>;

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

  template <typename receiver_t>
  auto connect(receiver_t receiver) const -> operation<receiver_t> {
    return operation<receiver_t>{
        .receiver = std::move(receiver),
        .stop_token = stdexec::get_stop_token(stdexec::get_env(receiver)),
    };
  }

  template <typename self_t, typename... env_t>
    requires std::same_as<std::remove_cvref_t<self_t>, stoppable_sender> &&
             (sizeof...(env_t) >= 1U)
  static consteval auto get_completion_signatures() {
    return completion_signatures{};
  }
};

struct stoppable_async_source : base_async_source {
  [[nodiscard]] auto read_async() { return stoppable_sender{}; }
};

} // namespace

TEST_CASE("pull op forwards successful async source completion to owner and resets storage",
          "[UT][wh/core/cursor_reader/detail/pull_op.hpp][pull_op::start][branch]") {
  owner_probe owner{};
  success_async_source source{};
  wh::core::cursor_reader_detail::pull_op<owner_probe, success_async_source, result_t>
      pull{&owner};

  pull.start(source, wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
             std::make_shared<stdexec::inplace_stop_source>());

  REQUIRE(owner.finish_calls == 1);
  REQUIRE(owner.last_status.has_value());
  REQUIRE(owner.last_status->has_value());
  REQUIRE(owner.last_status->value() == 7);
  REQUIRE_FALSE(owner.last_terminal_override);
  REQUIRE(owner.reset_calls == 1);
}

TEST_CASE("pull op maps async source errors through owner async failure path",
          "[UT][wh/core/cursor_reader/detail/pull_op.hpp][pull_op::receiver::set_error][branch]") {
  owner_probe owner{};
  error_async_source source{};
  wh::core::cursor_reader_detail::pull_op<owner_probe, error_async_source, result_t>
      pull{&owner};

  pull.start(source, wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
             std::make_shared<stdexec::inplace_stop_source>());

  REQUIRE(owner.finish_calls == 1);
  REQUIRE(owner.last_status.has_value());
  REQUIRE(owner.last_status->has_error());
  REQUIRE(owner.async_failure_code.has_value());
  REQUIRE(*owner.async_failure_code == wh::core::errc::internal_error);
  REQUIRE(owner.last_terminal_override);
  REQUIRE(owner.reset_calls == 1);
}

TEST_CASE("pull op request_stop propagates stop to async sender and reports stopped once",
          "[UT][wh/core/cursor_reader/detail/pull_op.hpp][pull_op::request_stop][concurrency][branch]") {
  owner_probe owner{};
  stoppable_async_source source{};
  auto stop_source = std::make_shared<stdexec::inplace_stop_source>();
  wh::core::cursor_reader_detail::pull_op<owner_probe, stoppable_async_source,
                                          result_t>
      pull{&owner};

  pull.start(source, wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
             stop_source);
  REQUIRE(owner.finish_calls == 0);
  REQUIRE(owner.stopped_calls == 0);

  pull.request_stop();

  REQUIRE(owner.stopped_calls == 1);
  REQUIRE(owner.reset_calls == 1);
  REQUIRE(owner.finish_calls == 0);
}

TEST_CASE("pull op maps direct error codes and ignores request_stop without a stop source",
          "[UT][wh/core/cursor_reader/detail/pull_op.hpp][pull_op::receiver::set_error][condition][branch][boundary]") {
  owner_probe owner{};
  coded_error_async_source source{};
  wh::core::cursor_reader_detail::pull_op<owner_probe, coded_error_async_source,
                                          result_t>
      pull{&owner};

  pull.request_stop();
  pull.start(source,
             wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}),
             std::shared_ptr<stdexec::inplace_stop_source>{});

  REQUIRE(owner.finish_calls == 1);
  REQUIRE(owner.last_status.has_value());
  REQUIRE(owner.last_status->has_error());
  REQUIRE(owner.last_status->error() == wh::core::errc::timeout);
  REQUIRE(owner.async_failure_code.has_value());
  REQUIRE(*owner.async_failure_code == wh::core::errc::timeout);
  REQUIRE(owner.last_terminal_override);
  REQUIRE(owner.reset_calls == 1);
}
