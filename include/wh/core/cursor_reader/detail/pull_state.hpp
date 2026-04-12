#pragma once

#include <cstdint>

namespace wh::core::cursor_reader_detail {

enum class pull_state : std::uint8_t {
  idle = 0U,
  try_reading,
  blocking_reading,
  async_reading,
  closing,
  terminal,
};

} // namespace wh::core::cursor_reader_detail
