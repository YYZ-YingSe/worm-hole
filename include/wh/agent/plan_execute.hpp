// Defines the authored plan-execute shell that binds planner, executor, and
// replanner roles without introducing runtime behavior.
#pragma once

#include <concepts>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/agent/role_binding.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/message.hpp"

namespace wh::agent {

/// Ordered execution plan produced by the planner or replanner role.
struct plan_execute_plan {
  /// Ordered steps that remain to be executed.
  std::vector<std::string> steps{};
};

/// One executed step plus its normalized result text.
struct plan_execute_executed_step {
  /// Stable step text that was executed.
  std::string step{};
  /// Normalized executor result kept for replanning context.
  std::string result{};
};

/// Shared authored context passed into plan/execute/replan request builders.
struct plan_execute_context {
  /// Original input conversation forwarded into the scenario.
  std::vector<wh::schema::message> input_messages{};
  /// Current plan before the next role runs, when any.
  std::optional<plan_execute_plan> current_plan{};
  /// Ordered executor outputs already completed in this scenario.
  std::vector<plan_execute_executed_step> executed_steps{};
};

/// Decision emitted by the replanner role.
enum class plan_execute_decision_kind {
  /// Continue the loop with a replacement plan.
  plan = 0U,
  /// Stop the loop and return the final response.
  respond,
};

/// Parsed replanner output used by the plan-execute loop.
struct plan_execute_decision {
  /// Selected branch after the replanner role completes.
  plan_execute_decision_kind kind{plan_execute_decision_kind::plan};
  /// Replacement plan used when `kind == plan`.
  plan_execute_plan next_plan{};
  /// Final response used when `kind == respond`.
  wh::schema::message response{};
};

using plan_execute_request_builder =
    wh::core::callback_function<wh::core::result<std::vector<wh::schema::message>>(
        const plan_execute_context &, wh::core::run_context &) const>;
using plan_execute_plan_reader = wh::agent::output_reader<plan_execute_plan>;
using plan_execute_step_reader = wh::agent::output_reader<std::string>;
using plan_execute_decision_reader = wh::agent::output_reader<plan_execute_decision>;

/// Thin authored plan-execute shell that binds planner, executor, and optional
/// replanner roles.
class plan_execute {
public:
  /// Creates one authored plan-execute shell.
  explicit plan_execute(std::string name) noexcept : name_(std::move(name)) {}

  plan_execute(const plan_execute &) = delete;
  auto operator=(const plan_execute &) -> plan_execute & = delete;
  plan_execute(plan_execute &&) noexcept = default;
  auto operator=(plan_execute &&) noexcept -> plan_execute & = default;
  ~plan_execute() = default;

  /// Returns the authored shell name.
  [[nodiscard]] auto name() const noexcept -> std::string_view { return name_; }

  /// Returns true after all required roles freeze successfully.
  [[nodiscard]] auto frozen() const noexcept -> bool { return frozen_; }

  /// Replaces the maximum execute-replan iterations. Zero falls back to one.
  auto set_max_iterations(const std::size_t max_iterations) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    max_iterations_ = max_iterations == 0U ? 1U : max_iterations;
    return {};
  }

  /// Sets the optional output slot written when the scenario completes.
  auto set_output_key(std::string output_key) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    output_key_ = std::move(output_key);
    return {};
  }

  /// Installs the planner-request builder before freeze.
  auto set_planner_request_builder(plan_execute_request_builder builder) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    planner_request_builder_ = std::move(builder);
    return {};
  }

  /// Installs the executor-request builder before freeze.
  auto set_executor_request_builder(plan_execute_request_builder builder)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    executor_request_builder_ = std::move(builder);
    return {};
  }

  /// Installs the replanner-request builder before freeze.
  auto set_replanner_request_builder(plan_execute_request_builder builder)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!builder) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    replanner_request_builder_ = std::move(builder);
    return {};
  }

  /// Installs the planner output reader before freeze.
  auto set_planner_plan_reader(plan_execute_plan_reader reader) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!reader) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    planner_plan_reader_ = std::move(reader);
    return {};
  }

  /// Installs the executor output reader before freeze.
  auto set_executor_step_reader(plan_execute_step_reader reader) -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!reader) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    executor_step_reader_ = std::move(reader);
    return {};
  }

  /// Installs the replanner output reader before freeze.
  auto set_replanner_decision_reader(plan_execute_decision_reader reader)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (!reader) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    replanner_decision_reader_ = std::move(reader);
    return {};
  }

  /// Installs the planner role before freeze.
  auto set_planner(wh::agent::role_binding planner) -> wh::core::result<void> {
    return set_role(planner_, std::move(planner));
  }

  template <typename role_t>
    requires(!std::same_as<std::remove_cvref_t<role_t>, wh::agent::role_binding>)
  auto set_planner(role_t &&planner) -> wh::core::result<void> {
    return set_planner(wh::agent::make_role_binding(std::forward<role_t>(planner)));
  }

  /// Installs the executor role before freeze.
  auto set_executor(wh::agent::role_binding executor) -> wh::core::result<void> {
    return set_role(executor_, std::move(executor));
  }

  template <typename role_t>
    requires(!std::same_as<std::remove_cvref_t<role_t>, wh::agent::role_binding>)
  auto set_executor(role_t &&executor) -> wh::core::result<void> {
    return set_executor(wh::agent::make_role_binding(std::forward<role_t>(executor)));
  }

  /// Installs the optional replanner role before freeze.
  auto set_replanner(wh::agent::role_binding replanner) -> wh::core::result<void> {
    return set_role(replanner_, std::move(replanner));
  }

  template <typename role_t>
    requires(!std::same_as<std::remove_cvref_t<role_t>, wh::agent::role_binding>)
  auto set_replanner(role_t &&replanner) -> wh::core::result<void> {
    return set_replanner(wh::agent::make_role_binding(std::forward<role_t>(replanner)));
  }

  /// Returns the frozen planner role.
  [[nodiscard]] auto planner()
      -> wh::core::result<std::reference_wrapper<wh::agent::role_binding>> {
    return role_ref(planner_);
  }

  /// Returns the frozen planner role.
  [[nodiscard]] auto planner() const
      -> wh::core::result<std::reference_wrapper<const wh::agent::role_binding>> {
    return role_ref(planner_);
  }

  /// Returns the frozen executor role.
  [[nodiscard]] auto executor()
      -> wh::core::result<std::reference_wrapper<wh::agent::role_binding>> {
    return role_ref(executor_);
  }

  /// Returns the frozen executor role.
  [[nodiscard]] auto executor() const
      -> wh::core::result<std::reference_wrapper<const wh::agent::role_binding>> {
    return role_ref(executor_);
  }

  /// Returns the optional replanner role when present.
  [[nodiscard]] auto replanner()
      -> wh::core::result<std::reference_wrapper<wh::agent::role_binding>> {
    return role_ref(replanner_);
  }

  /// Returns the optional replanner role when present.
  [[nodiscard]] auto replanner() const
      -> wh::core::result<std::reference_wrapper<const wh::agent::role_binding>> {
    return role_ref(replanner_);
  }

  /// Returns the effective replanner role, falling back to planner.
  [[nodiscard]] auto effective_replanner()
      -> wh::core::result<std::reference_wrapper<wh::agent::role_binding>> {
    if (replanner_.has_value()) {
      return std::ref(*replanner_);
    }
    return planner();
  }

  /// Returns the effective replanner role, falling back to planner.
  [[nodiscard]] auto effective_replanner() const
      -> wh::core::result<std::reference_wrapper<const wh::agent::role_binding>> {
    if (replanner_.has_value()) {
      return std::cref(*replanner_);
    }
    return planner();
  }

  /// Returns the effective replanner name. When no explicit replanner is
  /// installed, the planner role is reused.
  [[nodiscard]] auto effective_replanner_name() const -> wh::core::result<std::string_view> {
    if (replanner_.has_value()) {
      return std::string_view{replanner_->name()};
    }
    auto planner_role = planner();
    if (planner_role.has_error()) {
      return wh::core::result<std::string_view>::failure(planner_role.error());
    }
    return std::string_view{planner_role.value().get().name()};
  }

  /// Returns the configured iteration budget.
  [[nodiscard]] auto max_iterations() const noexcept -> std::size_t { return max_iterations_; }

  /// Returns the configured output slot name.
  [[nodiscard]] auto output_key() const noexcept -> std::string_view { return output_key_; }

  /// Returns the planner-request builder.
  [[nodiscard]] auto planner_request_builder() const noexcept
      -> const plan_execute_request_builder & {
    return planner_request_builder_;
  }

  /// Returns the executor-request builder.
  [[nodiscard]] auto executor_request_builder() const noexcept
      -> const plan_execute_request_builder & {
    return executor_request_builder_;
  }

  /// Returns the replanner-request builder.
  [[nodiscard]] auto replanner_request_builder() const noexcept
      -> const plan_execute_request_builder & {
    return replanner_request_builder_;
  }

  /// Returns the planner output reader.
  [[nodiscard]] auto planner_plan_reader() const noexcept -> const plan_execute_plan_reader & {
    return planner_plan_reader_;
  }

  /// Returns the executor output reader.
  [[nodiscard]] auto executor_step_reader() const noexcept -> const plan_execute_step_reader & {
    return executor_step_reader_;
  }

  /// Returns the replanner output reader.
  [[nodiscard]] auto replanner_decision_reader() const noexcept
      -> const plan_execute_decision_reader & {
    return replanner_decision_reader_;
  }

  /// Validates role completeness, executable bindings, and freezes all
  /// installed roles.
  auto freeze() -> wh::core::result<void> {
    if (frozen_) {
      return {};
    }
    if (name_.empty() || !planner_.has_value() || !executor_.has_value()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    if (!planner_request_builder_ || !executor_request_builder_ || !replanner_request_builder_ ||
        !planner_plan_reader_ || !executor_step_reader_ || !replanner_decision_reader_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (planner_->name() == executor_->name()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (!planner_->executable() || !executor_->executable()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    if (replanner_.has_value() && !replanner_->executable()) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    auto planner_frozen = planner_->freeze();
    if (planner_frozen.has_error()) {
      return planner_frozen;
    }
    auto executor_frozen = executor_->freeze();
    if (executor_frozen.has_error()) {
      return executor_frozen;
    }
    if (replanner_.has_value()) {
      auto replanner_frozen = replanner_->freeze();
      if (replanner_frozen.has_error()) {
        return replanner_frozen;
      }
    }
    frozen_ = true;
    return {};
  }

  /// Converts this frozen authored shell into the common executable agent surface.
  [[nodiscard]] auto into_agent() && -> wh::core::result<wh::agent::agent>;

private:
  /// Installs one role before freeze.
  auto set_role(std::optional<wh::agent::role_binding> &slot, wh::agent::role_binding value)
      -> wh::core::result<void> {
    auto mutable_status = ensure_mutable();
    if (mutable_status.has_error()) {
      return mutable_status;
    }
    if (value.name().empty()) {
      return wh::core::result<void>::failure(wh::core::errc::invalid_argument);
    }
    slot.emplace(std::move(value));
    return {};
  }

  /// Returns one installed role by mutable reference.
  [[nodiscard]] static auto role_ref(std::optional<wh::agent::role_binding> &slot)
      -> wh::core::result<std::reference_wrapper<wh::agent::role_binding>> {
    if (!slot.has_value()) {
      return wh::core::result<std::reference_wrapper<wh::agent::role_binding>>::failure(
          wh::core::errc::not_found);
    }
    return std::ref(*slot);
  }

  /// Returns one installed role by reference.
  [[nodiscard]] static auto role_ref(const std::optional<wh::agent::role_binding> &slot)
      -> wh::core::result<std::reference_wrapper<const wh::agent::role_binding>> {
    if (!slot.has_value()) {
      return wh::core::result<std::reference_wrapper<const wh::agent::role_binding>>::failure(
          wh::core::errc::not_found);
    }
    return std::cref(*slot);
  }

  /// Rejects role mutation after freeze.
  [[nodiscard]] auto ensure_mutable() const -> wh::core::result<void> {
    if (frozen_) {
      return wh::core::result<void>::failure(wh::core::errc::contract_violation);
    }
    return {};
  }

  /// Stable authored shell name.
  std::string name_{};
  /// Planner role used to author planning turns.
  std::optional<wh::agent::role_binding> planner_{};
  /// Executor role used to author execution turns.
  std::optional<wh::agent::role_binding> executor_{};
  /// Optional replanner role used after execution feedback.
  std::optional<wh::agent::role_binding> replanner_{};
  /// Maximum execute-replan iterations allowed for the authored shell.
  std::size_t max_iterations_{8U};
  /// Optional output slot written when the scenario converges.
  std::string output_key_{};
  /// Planner input builder for the initial planning turn.
  plan_execute_request_builder planner_request_builder_{nullptr};
  /// Executor input builder for each plan step.
  plan_execute_request_builder executor_request_builder_{nullptr};
  /// Replanner input builder after each execution turn.
  plan_execute_request_builder replanner_request_builder_{nullptr};
  /// Planner output reader that normalizes one plan from agent output.
  plan_execute_plan_reader planner_plan_reader_{nullptr};
  /// Executor output reader that normalizes one step result from agent output.
  plan_execute_step_reader executor_step_reader_{nullptr};
  /// Replanner output reader that selects continue/respond.
  plan_execute_decision_reader replanner_decision_reader_{nullptr};
  /// True after all authored roles have been frozen successfully.
  bool frozen_{false};
};

} // namespace wh::agent
