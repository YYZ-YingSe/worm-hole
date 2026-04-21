// Defines the public pipe stream family facade.
#pragma once

#include <cstddef>
#include <memory>
#include <utility>

#include "wh/schema/stream/detail/pipe_stream.hpp"
#include "wh/schema/stream/reader/pipe_stream_reader.hpp"
#include "wh/schema/stream/writer/pipe_stream_writer.hpp"

namespace wh::schema::stream {

template <typename value_t>
[[nodiscard]] inline auto make_pipe_stream(const std::size_t capacity = 64U)
    -> std::pair<pipe_stream_writer<value_t>, pipe_stream_reader<value_t>> {
  auto state = std::make_shared<detail::pipe_stream_state<value_t>>(capacity);
  return {pipe_stream_writer<value_t>{state}, pipe_stream_reader<value_t>{state}};
}

} // namespace wh::schema::stream
