#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/cursor_reader/detail/source.hpp"
#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/stdexec.hpp"

namespace wh::core::cursor_reader_detail {

template <typename owner_t, typename source_t, typename result_t>
class pull_op {
public:
  using scheduler_t = wh::core::detail::any_resume_scheduler_t;
  using sender_t = decltype(stdexec::starts_on(
      std::declval<scheduler_t>(), std::declval<source_t &>().read_async()));

  struct receiver {
    using receiver_concept = stdexec::receiver_t;

    pull_op *pull{nullptr};

    auto set_value(result_t status) noexcept -> void {
      auto *owner = pull->owner_;
      owner->finish_source_pull(pull, std::move(status), false);
      owner->reset_active_pull(pull);
    }

    template <typename error_t>
    auto set_error(error_t &&error) noexcept -> void {
      auto *owner = pull->owner_;
      owner->finish_source_pull(
          pull, owner->async_failure(map_error_code(std::forward<error_t>(error))),
          true);
      owner->reset_active_pull(pull);
    }

    auto set_stopped() noexcept -> void {
      auto *owner = pull->owner_;
      owner->finish_source_pull(pull, owner->async_failure(wh::core::errc::internal_error), true);
      owner->reset_active_pull(pull);
    }

    [[nodiscard]] auto get_env() const noexcept -> stdexec::env<> {
      return {};
    }
  };

  using op_state_t = stdexec::connect_result_t<sender_t, receiver>;

  explicit pull_op(owner_t *owner) noexcept : owner_(owner) {}

  auto start(source_t &source, scheduler_t scheduler) -> void {
    op_state_.emplace_from(stdexec::connect,
                           stdexec::starts_on(std::move(scheduler),
                                              source.read_async()),
                           receiver{this});
    stdexec::start(op_state_.get());
  }

private:
  template <typename error_t>
  [[nodiscard]] static auto map_error_code(error_t &&error) noexcept
      -> wh::core::error_code {
    if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                               wh::core::error_code>) {
      return std::forward<error_t>(error);
    } else if constexpr (std::same_as<std::remove_cvref_t<error_t>,
                                      std::exception_ptr>) {
      try {
        std::rethrow_exception(std::forward<error_t>(error));
      } catch (const std::exception &exception) {
        return wh::core::map_exception(exception);
      } catch (...) {
        return wh::core::errc::internal_error;
      }
    } else {
      return wh::core::errc::internal_error;
    }
  }

  owner_t *owner_{nullptr};
  wh::core::detail::manual_lifetime_box<op_state_t> op_state_{};
};

} // namespace wh::core::cursor_reader_detail
