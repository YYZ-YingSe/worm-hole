#pragma once

#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "helper/thread_support.hpp"

namespace wh::testing::helper {

template <typename scheduler_t, typename stop_token_t = stdexec::never_stop_token>
struct scheduler_env {
  scheduler_t scheduler;
  stop_token_t stop_token{};

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept -> scheduler_t {
    return scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept -> scheduler_t {
    return scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept -> stop_token_t {
    return stop_token;
  }
};

struct no_scheduler_env {
  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept -> stdexec::never_stop_token {
    return {};
  }
};

template <typename scheduler_t, typename stop_token_t = stdexec::never_stop_token>
struct completion_scheduler_env {
  scheduler_t scheduler;
  stop_token_t stop_token{};

  template <typename cpo_t>
  [[nodiscard]] auto query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
      -> scheduler_t {
    return scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept -> scheduler_t {
    return scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept -> stop_token_t {
    return stop_token;
  }
};

template <typename launch_scheduler_t, typename completion_scheduler_t,
          typename stop_token_t = stdexec::never_stop_token>
struct dual_scheduler_env {
  launch_scheduler_t launch_scheduler;
  completion_scheduler_t completion_scheduler;
  stop_token_t stop_token{};

  [[nodiscard]] auto query(stdexec::get_scheduler_t) const noexcept -> launch_scheduler_t {
    return launch_scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_delegation_scheduler_t) const noexcept
      -> launch_scheduler_t {
    return launch_scheduler;
  }

  template <typename cpo_t>
  [[nodiscard]] auto query(stdexec::get_completion_scheduler_t<cpo_t>) const noexcept
      -> completion_scheduler_t {
    return completion_scheduler;
  }

  [[nodiscard]] auto query(stdexec::get_stop_token_t) const noexcept -> stop_token_t {
    return stop_token;
  }
};

template <typename scheduler_t, typename stop_token_t = stdexec::never_stop_token>
[[nodiscard]] auto make_scheduler_env(scheduler_t scheduler, stop_token_t stop_token = {})
    -> scheduler_env<std::remove_cvref_t<scheduler_t>, stop_token_t> {
  return {std::move(scheduler), std::move(stop_token)};
}

template <typename scheduler_t, typename stop_token_t = stdexec::never_stop_token>
[[nodiscard]] auto make_completion_scheduler_env(scheduler_t scheduler,
                                                 stop_token_t stop_token = {})
    -> completion_scheduler_env<std::remove_cvref_t<scheduler_t>, stop_token_t> {
  return {std::move(scheduler), std::move(stop_token)};
}

template <typename launch_scheduler_t, typename completion_scheduler_t,
          typename stop_token_t = stdexec::never_stop_token>
[[nodiscard]] auto make_dual_scheduler_env(launch_scheduler_t launch_scheduler,
                                           completion_scheduler_t completion_scheduler,
                                           stop_token_t stop_token = {})
    -> dual_scheduler_env<std::remove_cvref_t<launch_scheduler_t>,
                          std::remove_cvref_t<completion_scheduler_t>, stop_token_t> {
  return {
      std::move(launch_scheduler),
      std::move(completion_scheduler),
      std::move(stop_token),
  };
}

} // namespace wh::testing::helper
