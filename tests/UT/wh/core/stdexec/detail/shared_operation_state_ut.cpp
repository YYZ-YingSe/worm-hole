#include <catch2/catch_test_macros.hpp>

#include <memory>

#include "wh/core/stdexec/detail/shared_operation_state.hpp"

namespace {

struct shared_controller {
  int start_calls{0};

  auto start() noexcept -> void { ++start_calls; }
};

} // namespace

TEST_CASE("shared operation state forwards start and retains controller ownership",
          "[UT][wh/core/stdexec/detail/shared_operation_state.hpp][shared_operation_state::start][condition][branch]") {
  auto controller = std::make_shared<shared_controller>();
  std::weak_ptr<shared_controller> weak = controller;

  {
    wh::core::detail::shared_operation_state<shared_controller> operation{
        controller};
    controller.reset();

    REQUIRE_FALSE(weak.expired());
    operation.start();
    REQUIRE(weak.lock()->start_calls == 1);
  }

  REQUIRE(weak.expired());
}

TEST_CASE("shared operation state keeps shared controller alive across multiple holders",
          "[UT][wh/core/stdexec/detail/shared_operation_state.hpp][shared_operation_state][condition][branch][boundary]") {
  auto controller = std::make_shared<shared_controller>();
  std::weak_ptr<shared_controller> weak = controller;

  {
    wh::core::detail::shared_operation_state<shared_controller> first{controller};
    wh::core::detail::shared_operation_state<shared_controller> second{controller};
    controller.reset();

    REQUIRE_FALSE(weak.expired());
    first.start();
    second.start();
    REQUIRE(weak.lock()->start_calls == 2);
  }

  REQUIRE(weak.expired());
}
