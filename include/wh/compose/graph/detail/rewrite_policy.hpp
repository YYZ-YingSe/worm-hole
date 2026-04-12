// Defines graph stream rewrite policy used by graph state-stream rewriting.
#pragma once

#include "wh/core/compiler.hpp"
#include "wh/compose/graph/detail/child_pump.hpp"

namespace wh::compose::detail {

template <typename handler_t> struct rewrite_policy {
  using child_sender_type =
      decltype(std::declval<graph_stream_reader &>().read_async());
  using completion_type = graph_stream_reader::chunk_result_type;

  graph_stream_reader reader{};
  graph_stream_writer writer{};
  wh_no_unique_address handler_t handler;

  [[nodiscard]] auto next_step()
      -> wh::core::result<child_pump_step<child_sender_type>> {
    return child_pump_step<child_sender_type>::launch(reader.read_async());
  }

  [[nodiscard]] auto handle_completion(completion_type next)
      -> std::optional<wh::core::result<graph_value>> {
    if (next.has_error()) {
      return wh::core::result<graph_value>::failure(next.error());
    }

    auto chunk = std::move(next).value();
    if (chunk.is_terminal_eof()) {
      auto closed = writer.close();
      if (closed.has_error()) {
        return wh::core::result<graph_value>::failure(closed.error());
      }
      return wh::core::result<graph_value>{make_graph_unit_value()};
    }
    if (chunk.is_source_eof()) {
      return std::nullopt;
    }
    if (chunk.error != wh::core::errc::ok) {
      return wh::core::result<graph_value>::failure(chunk.error);
    }

    if (!chunk.value.has_value()) {
      return std::nullopt;
    }

    auto chunk_payload = std::move(*chunk.value);
    auto handled = handler(chunk_payload);
    if (handled.has_error()) {
      return wh::core::result<graph_value>::failure(handled.error());
    }
    auto wrote = writer.try_write(std::move(chunk_payload));
    if (wrote.has_error()) {
      return wh::core::result<graph_value>::failure(wrote.error());
    }
    return std::nullopt;
  }
};

template <typename handler_t>
[[nodiscard]] inline auto make_rewrite_stream_sender(
    graph_stream_reader source, graph_stream_writer writer, handler_t &&handler,
    const wh::core::detail::any_resume_scheduler_t &graph_scheduler) {
  return make_child_pump_sender(
      rewrite_policy<std::remove_cvref_t<handler_t>>{
          .reader = std::move(source),
          .writer = std::move(writer),
          .handler = std::forward<handler_t>(handler),
      },
      graph_scheduler);
}

} // namespace wh::compose::detail
