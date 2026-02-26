#pragma once

#include <cstdint>
#include <utility>

#include "wh/core/resume_state.hpp"

namespace wh::testing {

class revision_checkpoint_fixture {
public:
  struct checkpoint_snapshot {
    std::uint64_t revision{0U};
    wh::core::resume_state state{};
  };

  auto set_revision(const std::uint64_t revision) noexcept -> void {
    current_revision_ = revision;
  }

  [[nodiscard]] auto revision() const noexcept -> std::uint64_t {
    return current_revision_;
  }

  [[nodiscard]] auto capture(const wh::core::resume_state &state) const
      -> checkpoint_snapshot {
    return checkpoint_snapshot{state.revision(), state};
  }

  [[nodiscard]] auto restore(const checkpoint_snapshot &snapshot)
      -> wh::core::resume_state {
    current_revision_ = snapshot.revision;
    return snapshot.state;
  }

private:
  std::uint64_t current_revision_{0U};
};

} // namespace wh::testing
