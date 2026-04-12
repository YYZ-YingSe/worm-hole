// Defines tools-node stream event wrapping over merged raw tool readers.
#pragma once

#include <concepts>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/stream.hpp"
#include "wh/compose/node/detail/tools/state.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::compose {
namespace detail {

using tool_after = wh::core::callback_function<wh::core::result<void>(
    const tool_call &, graph_value &, const wh::tool::call_scope &) const>;

struct tool_stream_binding {
  tool_call call{};
  wh::core::run_context context{};
};

using tool_stream_binding_map =
    std::unordered_map<std::string, tool_stream_binding,
                       wh::core::transparent_string_hash,
                       wh::core::transparent_string_equal>;

class tool_event_stream_reader final
    : public wh::schema::stream::stream_base<tool_event_stream_reader,
                                             graph_value> {
public:
  using value_type = graph_value;
  using chunk_type = wh::schema::stream::stream_chunk<graph_value>;

  tool_event_stream_reader() = default;
  tool_event_stream_reader(const tool_event_stream_reader &) = delete;
  auto operator=(const tool_event_stream_reader &)
      -> tool_event_stream_reader & = delete;
  tool_event_stream_reader(tool_event_stream_reader &&) noexcept = default;
  auto operator=(tool_event_stream_reader &&) noexcept
      -> tool_event_stream_reader & = default;
  ~tool_event_stream_reader() = default;

  tool_event_stream_reader(graph_stream_reader reader,
                           tool_stream_binding_map bindings,
                           std::vector<tool_after> afters)
      : reader_(std::move(reader)), bindings_(std::move(bindings)),
        afters_(std::move(afters)) {}

  [[nodiscard]] auto read_impl()
      -> wh::schema::stream::stream_result<chunk_type> {
    return map(reader_.read());
  }

  [[nodiscard]] auto try_read_impl()
      -> wh::schema::stream::stream_try_result<chunk_type> {
    return map(reader_.try_read());
  }

  [[nodiscard]] auto read_async() & {
    return reader_.read_async() | stdexec::then([this](result_t status) {
             return map(std::move(status));
           });
  }

  auto close_impl() -> wh::core::result<void> {
    closed_ = true;
    return reader_.close();
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return closed_ || reader_.is_closed();
  }

  [[nodiscard]] auto is_source_closed() const noexcept -> bool {
    return reader_.is_source_closed();
  }

  auto
  set_automatic_close(const wh::schema::stream::auto_close_options &options)
      -> void {
    reader_.set_automatic_close(options);
  }

private:
  using result_t = wh::schema::stream::stream_result<chunk_type>;
  using try_result_t = wh::schema::stream::stream_try_result<chunk_type>;

  [[nodiscard]] auto map(wh::schema::stream::stream_result<chunk_type> status)
      -> wh::schema::stream::stream_result<chunk_type> {
    if (status.has_error()) {
      return wh::schema::stream::stream_result<chunk_type>::failure(
          status.error());
    }

    auto chunk = std::move(status).value();
    if (chunk.eof || chunk.error.failed()) {
      return chunk;
    }
    if (!chunk.value.has_value()) {
      return wh::schema::stream::stream_result<chunk_type>::failure(
          wh::core::errc::protocol_error);
    }

    auto iter = bindings_.find(chunk.source);
    if (iter == bindings_.end()) {
      return wh::schema::stream::stream_result<chunk_type>::failure(
          wh::core::errc::not_found);
    }

    auto &binding = iter->second;
    graph_value value = std::move(*chunk.value);
    auto scope = make_scope(binding.call, binding.context);
    for (auto &after : afters_) {
      auto after_status = after(binding.call, value, scope);
      if (after_status.has_error()) {
        auto output = chunk_type{};
        output.error = after_status.error();
        return output;
      }
    }

    auto output = chunk_type::make_value(tool_event{
        .call_id = binding.call.call_id,
        .tool_name = binding.call.tool_name,
        .value = std::move(value),
    });
    output.source = std::move(chunk.source);
    return output;
  }

  [[nodiscard]] auto map(try_result_t status) -> try_result_t {
    if (std::holds_alternative<wh::schema::stream::stream_signal>(status)) {
      return wh::schema::stream::stream_pending;
    }
    return map(std::move(std::get<result_t>(status)));
  }

  graph_stream_reader reader_{};
  tool_stream_binding_map bindings_{};
  std::vector<tool_after> afters_{};
  bool closed_{false};
};

[[nodiscard]] inline auto collect_tool_afters(const tools_options &options)
    -> std::vector<tool_after> {
  std::vector<tool_after> afters{};
  afters.reserve(options.middleware.size());
  for (const auto &middleware : options.middleware) {
    if (!static_cast<bool>(middleware.after)) {
      continue;
    }
    afters.push_back(middleware.after);
  }
  return afters;
}

[[nodiscard]] inline auto make_tool_event_stream_reader(
    graph_stream_reader reader, tool_stream_binding_map bindings,
    std::vector<tool_after> afters) -> graph_stream_reader {
  return graph_stream_reader{tool_event_stream_reader{
      std::move(reader), std::move(bindings), std::move(afters)}};
}

} // namespace detail
} // namespace wh::compose
