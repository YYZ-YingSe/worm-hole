#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "wh/core/stdexec/detail/inline_drive_loop.hpp"

namespace {

struct inline_completion_probe {
  int *completions{nullptr};

  auto complete() && noexcept -> void { ++*completions; }
};

class inline_drive_probe
    : public wh::core::detail::inline_drive_loop<inline_drive_probe> {
public:
  auto request_probe_drive() noexcept -> void { request_drive(); }

  [[nodiscard]] auto finished() const noexcept -> bool { return finished_; }

  [[nodiscard]] auto completion_pending() const noexcept -> bool {
    return completion_.has_value();
  }

  [[nodiscard]] auto take_completion() noexcept
      -> std::optional<inline_completion_probe> {
    if (!completion_.has_value()) {
      return std::nullopt;
    }
    auto completion = std::move(completion_);
    completion_.reset();
    return completion;
  }

  auto drive() noexcept -> void {
    ++drive_calls;
    if (drive_calls == 1 && recursive_request_) {
      request_drive();
    }
    if (drive_calls >= finish_after_) {
      finished_ = true;
      completion_.emplace(&completion_calls);
    }
  }

  int drive_calls{0};
  int completion_calls{0};
  int finish_after_{1};
  bool recursive_request_{false};
  bool finished_{false};
  std::optional<inline_completion_probe> completion_{};
};

} // namespace

TEST_CASE("inline drive loop reruns when nested work arrives during active drive",
          "[UT][wh/core/stdexec/detail/inline_drive_loop.hpp][inline_drive_loop::request_drive][condition][branch][concurrency]") {
  inline_drive_probe probe{};
  probe.recursive_request_ = true;
  probe.finish_after_ = 2;

  probe.request_probe_drive();

  REQUIRE(probe.drive_calls == 2);
  REQUIRE(probe.finished());
  REQUIRE(probe.completion_calls == 1);
  REQUIRE_FALSE(probe.completion_pending());
}

TEST_CASE("inline drive loop skips drive when already finished and only flushes pending completion",
          "[UT][wh/core/stdexec/detail/inline_drive_loop.hpp][inline_drive_loop::request_drive][boundary][branch]") {
  inline_drive_probe probe{};
  probe.finished_ = true;
  probe.completion_.emplace(&probe.completion_calls);

  probe.request_probe_drive();

  REQUIRE(probe.drive_calls == 0);
  REQUIRE(probe.completion_calls == 1);

  probe.request_probe_drive();
  REQUIRE(probe.drive_calls == 0);
  REQUIRE(probe.completion_calls == 1);
}
