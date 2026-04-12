#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

#include <stdexec/execution.hpp>

#include "helper/manual_scheduler.hpp"
#include "helper/sender_capture.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/detail/receiver_completion.hpp"
#include "wh/core/stdexec/detail/scheduled_drive_loop.hpp"

namespace {

struct completion_lifetime_probe {
  bool destroyed{false};
  bool destroyed_before_start_return{false};
};

struct activation_probe_scheduler {
  struct schedule_sender {
    using sender_concept = stdexec::sender_t;
    using completion_signatures =
        stdexec::completion_signatures<stdexec::set_value_t()>;

    std::shared_ptr<completion_lifetime_probe> probe{};

    template <typename receiver_t> struct operation {
      using operation_state_concept = stdexec::operation_state_t;

      receiver_t receiver;
      std::shared_ptr<completion_lifetime_probe> probe{};

      auto start() & noexcept -> void {
        auto keep_alive = probe;
        stdexec::set_value(std::move(receiver));
        if (keep_alive->destroyed) {
          keep_alive->destroyed_before_start_return = true;
        }
      }

      ~operation() { probe->destroyed = true; }
    };

    template <typename receiver_t>
    auto connect(receiver_t receiver) const -> operation<receiver_t> {
      return operation<receiver_t>{std::move(receiver), probe};
    }

    template <typename self_t, typename... env_t>
      requires std::same_as<std::remove_cvref_t<self_t>, schedule_sender> &&
               (sizeof...(env_t) >= 1U)
    static consteval auto get_completion_signatures() {
      return completion_signatures{};
    }
  };

  std::shared_ptr<completion_lifetime_probe> probe{};
  using scheduler_concept = stdexec::scheduler_t;

  [[nodiscard]] auto schedule() const noexcept -> schedule_sender {
    return schedule_sender{probe};
  }

  [[nodiscard]] auto operator==(const activation_probe_scheduler &) const noexcept
      -> bool = default;
};

template <typename scheduler_t> class scheduled_probe_controller
    : public std::enable_shared_from_this<scheduled_probe_controller<scheduler_t>>,
      public wh::core::detail::scheduled_drive_loop<
          scheduled_probe_controller<scheduler_t>, scheduler_t> {
  using base_t = wh::core::detail::scheduled_drive_loop<
      scheduled_probe_controller<scheduler_t>, scheduler_t>;
  friend base_t;
  using result_t = wh::core::result<int>;
  using final_completion_t =
      wh::core::detail::receiver_completion<
          wh::testing::helper::sender_capture_receiver<result_t>, result_t>;

public:
  struct block_state {
    std::mutex mutex{};
    std::condition_variable cv{};
    bool first_drive_entered{false};
    bool release_first_drive{false};
  };

  scheduled_probe_controller(
      scheduler_t scheduler,
      wh::testing::helper::sender_capture_receiver<result_t> receiver,
      std::shared_ptr<block_state> block_state = {})
      : base_t(std::move(scheduler)), receiver_(std::move(receiver)),
        block_state_(std::move(block_state)) {}

  auto start() noexcept -> void { base_t::request_drive(); }
  auto request_probe_drive() noexcept -> void { base_t::request_drive(); }

  [[nodiscard]] auto finished() const noexcept -> bool {
    return delivered_.load(std::memory_order_acquire);
  }

  [[nodiscard]] auto completion_pending() const noexcept -> bool {
    return pending_completion_.has_value();
  }

  [[nodiscard]] auto take_completion() noexcept
      -> std::optional<final_completion_t> {
    if (!pending_completion_.has_value()) {
      return std::nullopt;
    }
    auto completion = std::move(pending_completion_);
    pending_completion_.reset();
    return completion;
  }

  auto drive() noexcept -> void {
    const auto drive_no =
        drive_calls_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
    if (block_state_ != nullptr && drive_no == 1U) {
      {
        std::lock_guard lock{block_state_->mutex};
        block_state_->first_drive_entered = true;
      }
      block_state_->cv.notify_all();
      std::unique_lock lock{block_state_->mutex};
      block_state_->cv.wait(lock, [&] { return block_state_->release_first_drive; });
      return;
    }
    if (drive_no == 1U) {
      return;
    }
    finish(result_t{static_cast<int>(drive_no)});
  }

  [[nodiscard]] auto drive_calls() const noexcept -> std::uint32_t {
    return drive_calls_.load(std::memory_order_acquire);
  }

private:
  auto drive_error(const wh::core::error_code error) noexcept -> void {
    finish(result_t::failure(error));
  }

  auto finish(result_t status) noexcept -> void {
    if (delivered_.exchange(true, std::memory_order_acq_rel)) {
      return;
    }
    pending_completion_.emplace(
        final_completion_t::set_value(std::move(receiver_), std::move(status)));
  }

  wh::testing::helper::sender_capture_receiver<result_t> receiver_;
  std::shared_ptr<block_state> block_state_{};
  std::optional<final_completion_t> pending_completion_{};
  std::atomic<bool> delivered_{false};
  std::atomic<std::uint32_t> drive_calls_{0U};
};

template <typename controller_t> class shared_operation {
public:
  using operation_state_concept = stdexec::operation_state_t;

  explicit shared_operation(std::shared_ptr<controller_t> controller) noexcept
      : controller_(std::move(controller)) {}

  auto start() & noexcept -> void { controller_->start(); }
  auto request_probe_drive() noexcept -> void { controller_->request_probe_drive(); }

private:
  std::shared_ptr<controller_t> controller_{};
};

} // namespace

TEST_CASE("scheduled drive loop keeps inline schedule turn alive across quiescent rerun",
          "[UT][wh/core/stdexec/detail/scheduled_drive_loop.hpp][scheduled_drive_loop::request_drive][branch][boundary]") {
  using controller_t = scheduled_probe_controller<activation_probe_scheduler>;
  using result_t = wh::core::result<int>;

  auto probe = std::make_shared<completion_lifetime_probe>();
  wh::testing::helper::sender_capture<result_t> capture{};
  auto controller = std::make_shared<controller_t>(
      activation_probe_scheduler{probe},
      wh::testing::helper::sender_capture_receiver<result_t>{&capture});
  auto operation = shared_operation<controller_t>{controller};

  operation.start();
  REQUIRE_FALSE(probe->destroyed_before_start_return);
  REQUIRE_FALSE(probe->destroyed);

  operation.request_probe_drive();

  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(capture.value->has_value());
  REQUIRE(capture.value->value() == 2);
  REQUIRE(probe->destroyed);
  REQUIRE_FALSE(probe->destroyed_before_start_return);
}

TEST_CASE("scheduled drive loop reruns when concurrent request arrives during active drive",
          "[UT][wh/core/stdexec/detail/scheduled_drive_loop.hpp][scheduled_drive_loop::request_drive][condition][concurrency][branch]") {
  using scheduler_t = wh::testing::helper::manual_scheduler<void>;
  using controller_t = scheduled_probe_controller<scheduler_t>;
  using result_t = wh::core::result<int>;

  wh::testing::helper::manual_scheduler_state scheduler_state{};
  auto block_state = std::make_shared<typename controller_t::block_state>();
  wh::testing::helper::sender_capture<result_t> capture{};
  auto controller = std::make_shared<controller_t>(
      scheduler_t{&scheduler_state},
      wh::testing::helper::sender_capture_receiver<result_t>{&capture},
      block_state);
  auto operation = shared_operation<controller_t>{controller};

  operation.start();
  REQUIRE(scheduler_state.pending_count() == 1U);

  std::thread worker([&] { REQUIRE(scheduler_state.run_one()); });

  {
    std::unique_lock lock{block_state->mutex};
    block_state->cv.wait(lock, [&] { return block_state->first_drive_entered; });
  }

  std::vector<std::thread> requesters{};
  requesters.reserve(4U);
  for (int index = 0; index < 4; ++index) {
    requesters.emplace_back([&] { operation.request_probe_drive(); });
  }
  for (auto &requester : requesters) {
    requester.join();
  }

  {
    std::lock_guard lock{block_state->mutex};
    block_state->release_first_drive = true;
  }
  block_state->cv.notify_all();
  worker.join();

  REQUIRE(capture.ready.try_acquire());
  REQUIRE(capture.terminal == wh::testing::helper::sender_terminal_kind::value);
  REQUIRE(capture.value.has_value());
  REQUIRE(capture.value->has_value());
  REQUIRE(capture.value->value() == 2);
  REQUIRE(controller->drive_calls() == 2U);
}
