// Defines the private tools-node reader that shapes raw tool payload chunks
// into graph-facing tool_event chunks.
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/compose/node/detail/tools/state.hpp"
#include "wh/core/error.hpp"
#include "wh/core/error_domain.hpp"
#include "wh/core/result.hpp"
#include "wh/schema/stream/adapter/detail/adapter_support.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/status.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::compose {
namespace detail {

class tool_event_stream_reader final
    : public wh::schema::stream::stream_base<tool_event_stream_reader,
                                             graph_value> {
private:
  using chunk_type = wh::schema::stream::stream_chunk<graph_value>;
  using result_t = wh::schema::stream::stream_result<chunk_type>;
  using try_result_t = wh::schema::stream::stream_try_result<chunk_type>;

public:
  using value_type = graph_value;

  tool_event_stream_reader() = default;
  tool_event_stream_reader(const tool_event_stream_reader &) = delete;
  auto operator=(const tool_event_stream_reader &)
      -> tool_event_stream_reader & = delete;
  tool_event_stream_reader(tool_event_stream_reader &&) noexcept = default;
  auto operator=(tool_event_stream_reader &&) noexcept
      -> tool_event_stream_reader & = default;
  ~tool_event_stream_reader() = default;

  tool_event_stream_reader(graph_stream_reader reader, tool_call call,
                           tool_after_chain_ptr afters,
                           std::optional<wh::core::run_context> context = std::nullopt)
      : state_(wh::core::detail::make_intrusive<state>(
            std::move(reader), std::move(call), std::move(afters),
            std::move(context))) {}

  [[nodiscard]] auto read_impl() -> result_t {
    try {
      return map_read_owned(state_, state_->reader.read());
    } catch (...) {
      state_->adapter_state.terminal = true;
      state_->adapter_state.close_source_if_enabled(state_->reader);
      return result_t{
          wh::schema::stream::detail::make_error_chunk<chunk_type>(
              wh::core::map_current_exception())};
    }
  }

  [[nodiscard]] auto try_read_impl() -> try_result_t {
    try {
      return map_try_owned(state_, state_->reader.try_read());
    } catch (...) {
      state_->adapter_state.terminal = true;
      state_->adapter_state.close_source_if_enabled(state_->reader);
      return result_t{
          wh::schema::stream::detail::make_error_chunk<chunk_type>(
              wh::core::map_current_exception())};
    }
  }

  [[nodiscard]] auto read_async() const {
    using input_result_t = wh::schema::stream::stream_result<
        wh::schema::stream::stream_chunk<graph_value>>;
    auto state = state_;
    return state->reader.read_async() |
           stdexec::then([](auto status) {
             return input_result_t{std::move(status)};
           }) |
           stdexec::upon_error([](auto &&) noexcept {
             return input_result_t::failure(wh::core::errc::internal_error);
           }) |
           stdexec::then([state = std::move(state)](input_result_t next) {
             return map_read_owned(state, std::move(next));
           });
  }

  auto close_impl() -> wh::core::result<void> {
    state_->adapter_state.terminal = true;
    return state_->adapter_state.close_source(state_->reader);
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return state_->adapter_state.terminal || state_->reader.is_closed();
  }

  [[nodiscard]] auto is_source_closed() const noexcept -> bool {
    return wh::schema::stream::detail::is_source_closed_if_supported(
        state_->reader);
  }

  auto set_automatic_close(
      const wh::schema::stream::auto_close_options &options) -> void {
    state_->adapter_state.automatic_close = options.enabled;
    wh::schema::stream::detail::set_automatic_close_if_supported(state_->reader,
                                                                 options);
  }

private:
  struct state : wh::core::detail::intrusive_enable_from_this<state> {
    graph_stream_reader reader{};
    tool_call call{};
    tool_after_chain_ptr afters{};
    std::optional<wh::core::run_context> context{};
    wh::schema::stream::detail::stream_adapter_state adapter_state{};

    state() = default;

    state(graph_stream_reader reader_value, tool_call call_value,
          tool_after_chain_ptr afters_value,
          std::optional<wh::core::run_context> context_value)
        : reader(std::move(reader_value)),
          call(std::move(call_value)),
          afters(std::move(afters_value)),
          context(std::move(context_value)) {}
  };

  [[nodiscard]] static auto make_tool_event_value(const tool_call &call,
                                                  graph_value value)
      -> graph_value {
    return graph_value{tool_event{
        .call_id = call.call_id,
        .tool_name = call.tool_name,
        .value = std::move(value),
    }};
  }

  [[nodiscard]] static auto
  map_read_owned(const wh::core::detail::intrusive_ptr<state> &state,
                 result_t next) -> result_t {
    if (next.has_error()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      return result_t::failure(next.error());
    }

    auto input_chunk = std::move(next).value();
    if (input_chunk.eof) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      auto eof_chunk = chunk_type::make_eof();
      eof_chunk.source = std::move(input_chunk.source);
      return result_t{std::move(eof_chunk)};
    }
    if (input_chunk.error.failed()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      auto output_chunk = chunk_type{};
      output_chunk.source = std::move(input_chunk.source);
      output_chunk.error = input_chunk.error;
      return result_t{std::move(output_chunk)};
    }
    if (!input_chunk.value.has_value()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      return result_t::failure(wh::core::errc::protocol_error);
    }
    return map_value(state, input_chunk.source, std::move(*input_chunk.value));
  }

  [[nodiscard]] static auto
  map_try_owned(const wh::core::detail::intrusive_ptr<state> &state,
                try_result_t next) -> try_result_t {
    if (std::holds_alternative<wh::schema::stream::stream_signal>(next)) {
      return wh::schema::stream::stream_pending;
    }
    return map_read_owned(state, std::move(std::get<result_t>(next)));
  }

  [[nodiscard]] static auto
  map_value(const wh::core::detail::intrusive_ptr<state> &state,
            std::string_view source, graph_value value) -> result_t {
    if (!has_tool_afters(state->afters)) {
      auto output_chunk =
          chunk_type::make_value(make_tool_event_value(state->call,
                                                       std::move(value)));
      output_chunk.source = std::string{source};
      return result_t{std::move(output_chunk)};
    }

    if (!state->context.has_value()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      return result_t::failure(wh::core::errc::protocol_error);
    }
    auto scope = wh::tool::call_scope{
        .run = *state->context,
        .component = "tools_node",
        .implementation = "tool",
        .tool_name = state->call.tool_name,
        .call_id = state->call.call_id,
    };
    auto status = run_after(state->afters, state->call, value, scope);
    if (status.has_error()) {
      state->adapter_state.terminal = true;
      state->adapter_state.close_source_if_enabled(state->reader);
      auto output_chunk = chunk_type{};
      output_chunk.source = std::string{source};
      output_chunk.error = status.error();
      return result_t{std::move(output_chunk)};
    }

    auto output_chunk =
        chunk_type::make_value(make_tool_event_value(state->call,
                                                     std::move(value)));
    output_chunk.source = std::string{source};
    return result_t{std::move(output_chunk)};
  }

  wh::core::detail::intrusive_ptr<state> state_{
      wh::core::detail::make_intrusive<state>()};
};

[[nodiscard]] inline auto
make_tool_event_stream_reader(graph_stream_reader reader, tool_call call,
                              tool_after_chain_ptr afters,
                              std::optional<wh::core::run_context> context = std::nullopt)
    -> graph_stream_reader {
  return graph_stream_reader{tool_event_stream_reader{
      std::move(reader), std::move(call), std::move(afters),
      std::move(context)}};
}

} // namespace detail
} // namespace wh::compose
