// Defines tools-node readers that materialize raw payload chunks into
// graph-facing tool_event chunks at explicit stream boundaries.
#pragma once

#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

namespace tool_event_reader_detail {

using chunk_type = wh::schema::stream::stream_chunk<graph_value>;
using result_t = wh::schema::stream::stream_result<chunk_type>;
using try_result_t = wh::schema::stream::stream_try_result<chunk_type>;

struct tool_event_binding {
  tool_call call{};
  std::optional<wh::core::run_context> context{};
};

[[nodiscard]] inline auto make_tool_event_value(const tool_call &call, graph_value value)
    -> graph_value {
  return graph_value{tool_event{
      .call_id = call.call_id,
      .tool_name = call.tool_name,
      .value = std::move(value),
  }};
}

template <typename resolve_binding_t>
[[nodiscard]] inline auto map_value(graph_stream_reader &reader,
                                    wh::schema::stream::detail::stream_adapter_state &adapter_state,
                                    const tool_after_chain_ptr &afters,
                                    resolve_binding_t &&resolve_binding, std::string_view source,
                                    graph_value value) -> result_t {
  auto *binding = std::forward<resolve_binding_t>(resolve_binding)(source);
  if (binding == nullptr) {
    adapter_state.terminal = true;
    adapter_state.close_source_if_enabled(reader);
    return result_t::failure(wh::core::errc::protocol_error);
  }

  if (!has_tool_afters(afters)) {
    auto output_chunk =
        chunk_type::make_value(make_tool_event_value(binding->call, std::move(value)));
    output_chunk.source = std::string{source};
    return result_t{std::move(output_chunk)};
  }

  if (!binding->context.has_value()) {
    adapter_state.terminal = true;
    adapter_state.close_source_if_enabled(reader);
    return result_t::failure(wh::core::errc::protocol_error);
  }

  auto scope = make_scope(binding->call, *binding->context);
  auto status = run_after(afters, binding->call, value, scope);
  if (status.has_error()) {
    adapter_state.terminal = true;
    adapter_state.close_source_if_enabled(reader);
    auto output_chunk = chunk_type{};
    output_chunk.source = std::string{source};
    output_chunk.error = status.error();
    return result_t{std::move(output_chunk)};
  }

  auto output_chunk =
      chunk_type::make_value(make_tool_event_value(binding->call, std::move(value)));
  output_chunk.source = std::string{source};
  return result_t{std::move(output_chunk)};
}

template <typename resolve_binding_t>
[[nodiscard]] inline auto
map_read_owned(graph_stream_reader &reader,
               wh::schema::stream::detail::stream_adapter_state &adapter_state,
               const tool_after_chain_ptr &afters, resolve_binding_t &&resolve_binding,
               result_t next) -> result_t {
  if (next.has_error()) {
    adapter_state.terminal = true;
    adapter_state.close_source_if_enabled(reader);
    return result_t::failure(next.error());
  }

  auto input_chunk = std::move(next).value();
  if (input_chunk.eof) {
    adapter_state.terminal = true;
    adapter_state.close_source_if_enabled(reader);
    auto eof_chunk = chunk_type::make_eof();
    eof_chunk.source = std::move(input_chunk.source);
    return result_t{std::move(eof_chunk)};
  }
  if (input_chunk.error.failed()) {
    adapter_state.terminal = true;
    adapter_state.close_source_if_enabled(reader);
    auto output_chunk = chunk_type{};
    output_chunk.source = std::move(input_chunk.source);
    output_chunk.error = input_chunk.error;
    return result_t{std::move(output_chunk)};
  }
  if (!input_chunk.value.has_value()) {
    adapter_state.terminal = true;
    adapter_state.close_source_if_enabled(reader);
    return result_t::failure(wh::core::errc::protocol_error);
  }

  return map_value(reader, adapter_state, afters, std::forward<resolve_binding_t>(resolve_binding),
                   input_chunk.source, std::move(*input_chunk.value));
}

template <typename resolve_binding_t>
[[nodiscard]] inline auto
map_try_owned(graph_stream_reader &reader,
              wh::schema::stream::detail::stream_adapter_state &adapter_state,
              const tool_after_chain_ptr &afters, resolve_binding_t &&resolve_binding,
              try_result_t next) -> try_result_t {
  if (std::holds_alternative<wh::schema::stream::stream_signal>(next)) {
    return wh::schema::stream::stream_pending;
  }
  return map_read_owned(reader, adapter_state, afters,
                        std::forward<resolve_binding_t>(resolve_binding),
                        std::move(std::get<result_t>(next)));
}

} // namespace tool_event_reader_detail

class tool_event_stream_reader final
    : public wh::schema::stream::stream_base<tool_event_stream_reader, graph_value> {
private:
  using result_t = tool_event_reader_detail::result_t;
  using try_result_t = tool_event_reader_detail::try_result_t;

public:
  using value_type = graph_value;

  tool_event_stream_reader() = default;
  tool_event_stream_reader(const tool_event_stream_reader &) = delete;
  auto operator=(const tool_event_stream_reader &) -> tool_event_stream_reader & = delete;
  tool_event_stream_reader(tool_event_stream_reader &&) noexcept = default;
  auto operator=(tool_event_stream_reader &&) noexcept -> tool_event_stream_reader & = default;
  ~tool_event_stream_reader() = default;

  tool_event_stream_reader(graph_stream_reader reader, tool_call call, tool_after_chain_ptr afters,
                           std::optional<wh::core::run_context> context = std::nullopt)
      : state_(wh::core::detail::make_intrusive<state>(std::move(reader), std::move(call),
                                                       std::move(afters), std::move(context))) {}

  [[nodiscard]] auto read_impl() -> result_t {
    try {
      return map_read_owned(state_, state_->reader.read());
    } catch (...) {
      state_->adapter_state.terminal = true;
      state_->adapter_state.close_source_if_enabled(state_->reader);
      return result_t{
          wh::schema::stream::detail::make_error_chunk<tool_event_reader_detail::chunk_type>(
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
          wh::schema::stream::detail::make_error_chunk<tool_event_reader_detail::chunk_type>(
              wh::core::map_current_exception())};
    }
  }

  [[nodiscard]] auto read_async() const {
    using input_result_t =
        wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<graph_value>>;
    auto shared_state = state_;
    return shared_state->reader.read_async() |
           stdexec::then([](auto status) { return input_result_t{std::move(status)}; }) |
           stdexec::upon_error([](auto &&) noexcept {
             return input_result_t::failure(wh::core::errc::internal_error);
           }) |
           stdexec::then([shared_state = std::move(shared_state)](input_result_t next) {
             return map_read_owned(shared_state, std::move(next));
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
    return wh::schema::stream::detail::is_source_closed_if_supported(state_->reader);
  }

  auto set_automatic_close(const wh::schema::stream::auto_close_options &options) -> void {
    state_->adapter_state.automatic_close = options.enabled;
    wh::schema::stream::detail::set_automatic_close_if_supported(state_->reader, options);
  }

private:
  struct state : wh::core::detail::intrusive_enable_from_this<state> {
    graph_stream_reader reader{};
    tool_event_reader_detail::tool_event_binding binding{};
    tool_after_chain_ptr afters{};
    wh::schema::stream::detail::stream_adapter_state adapter_state{};

    state() = default;

    state(graph_stream_reader reader_value, tool_call call_value, tool_after_chain_ptr afters_value,
          std::optional<wh::core::run_context> context_value)
        : reader(std::move(reader_value)),
          binding{.call = std::move(call_value), .context = std::move(context_value)},
          afters(std::move(afters_value)), adapter_state{} {}
  };

  [[nodiscard]] static auto map_read_owned(const wh::core::detail::intrusive_ptr<state> &state,
                                           result_t next) -> result_t {
    return tool_event_reader_detail::map_read_owned(
        state->reader, state->adapter_state, state->afters,
        [&](std::string_view) noexcept { return std::addressof(state->binding); }, std::move(next));
  }

  [[nodiscard]] static auto map_try_owned(const wh::core::detail::intrusive_ptr<state> &state,
                                          try_result_t next) -> try_result_t {
    return tool_event_reader_detail::map_try_owned(
        state->reader, state->adapter_state, state->afters,
        [&](std::string_view) noexcept { return std::addressof(state->binding); }, std::move(next));
  }

  wh::core::detail::intrusive_ptr<state> state_{wh::core::detail::make_intrusive<state>()};
};

class tools_output_stream_reader final
    : public wh::schema::stream::stream_base<tools_output_stream_reader, graph_value> {
private:
  using result_t = tool_event_reader_detail::result_t;
  using try_result_t = tool_event_reader_detail::try_result_t;
  using binding_t = tool_event_reader_detail::tool_event_binding;

public:
  using value_type = graph_value;

  tools_output_stream_reader() = default;
  tools_output_stream_reader(const tools_output_stream_reader &) = delete;
  auto operator=(const tools_output_stream_reader &) -> tools_output_stream_reader & = delete;
  tools_output_stream_reader(tools_output_stream_reader &&) noexcept = default;
  auto operator=(tools_output_stream_reader &&) noexcept -> tools_output_stream_reader & = default;
  ~tools_output_stream_reader() = default;

  tools_output_stream_reader(graph_stream_reader reader, tool_after_chain_ptr afters,
                             std::vector<binding_t> bindings)
      : state_(wh::core::detail::make_intrusive<state>(std::move(reader), std::move(afters),
                                                       std::move(bindings))) {}

  [[nodiscard]] auto read_impl() -> result_t {
    try {
      return map_read_owned(state_, state_->reader.read());
    } catch (...) {
      state_->adapter_state.terminal = true;
      state_->adapter_state.close_source_if_enabled(state_->reader);
      return result_t{
          wh::schema::stream::detail::make_error_chunk<tool_event_reader_detail::chunk_type>(
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
          wh::schema::stream::detail::make_error_chunk<tool_event_reader_detail::chunk_type>(
              wh::core::map_current_exception())};
    }
  }

  [[nodiscard]] auto read_async() const {
    using input_result_t =
        wh::schema::stream::stream_result<wh::schema::stream::stream_chunk<graph_value>>;
    auto shared_state = state_;
    return shared_state->reader.read_async() |
           stdexec::then([](auto status) { return input_result_t{std::move(status)}; }) |
           stdexec::upon_error([](auto &&) noexcept {
             return input_result_t::failure(wh::core::errc::internal_error);
           }) |
           stdexec::then([shared_state = std::move(shared_state)](input_result_t next) {
             return map_read_owned(shared_state, std::move(next));
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
    return wh::schema::stream::detail::is_source_closed_if_supported(state_->reader);
  }

  auto set_automatic_close(const wh::schema::stream::auto_close_options &options) -> void {
    state_->adapter_state.automatic_close = options.enabled;
    wh::schema::stream::detail::set_automatic_close_if_supported(state_->reader, options);
  }

private:
  struct state : wh::core::detail::intrusive_enable_from_this<state> {
    graph_stream_reader reader{};
    tool_after_chain_ptr afters{};
    std::vector<binding_t> bindings{};
    wh::schema::stream::detail::stream_adapter_state adapter_state{};

    state() = default;

    state(graph_stream_reader reader_value, tool_after_chain_ptr afters_value,
          std::vector<binding_t> bindings_value)
        : reader(std::move(reader_value)), afters(std::move(afters_value)),
          bindings(std::move(bindings_value)), adapter_state{} {}

    [[nodiscard]] auto find_binding(const std::string_view source) noexcept -> binding_t * {
      auto iter = std::ranges::find_if(
          bindings, [&](binding_t &binding) { return binding.call.call_id == source; });
      return iter == bindings.end() ? nullptr : std::addressof(*iter);
    }
  };

  [[nodiscard]] static auto map_read_owned(const wh::core::detail::intrusive_ptr<state> &state,
                                           result_t next) -> result_t {
    return tool_event_reader_detail::map_read_owned(
        state->reader, state->adapter_state, state->afters,
        [&](const std::string_view source) noexcept { return state->find_binding(source); },
        std::move(next));
  }

  [[nodiscard]] static auto map_try_owned(const wh::core::detail::intrusive_ptr<state> &state,
                                          try_result_t next) -> try_result_t {
    return tool_event_reader_detail::map_try_owned(
        state->reader, state->adapter_state, state->afters,
        [&](const std::string_view source) noexcept { return state->find_binding(source); },
        std::move(next));
  }

  wh::core::detail::intrusive_ptr<state> state_{wh::core::detail::make_intrusive<state>()};
};

[[nodiscard]] inline auto make_tool_event_stream_reader(
    graph_stream_reader reader, tool_call call, tool_after_chain_ptr afters,
    std::optional<wh::core::run_context> context = std::nullopt) -> graph_stream_reader {
  return graph_stream_reader{tool_event_stream_reader{std::move(reader), std::move(call),
                                                      std::move(afters), std::move(context)}};
}

[[nodiscard]] inline auto
make_tools_output_stream_reader(graph_stream_reader reader, tool_after_chain_ptr afters,
                                std::vector<tool_event_reader_detail::tool_event_binding> bindings)
    -> graph_stream_reader {
  return graph_stream_reader{
      tools_output_stream_reader{std::move(reader), std::move(afters), std::move(bindings)}};
}

} // namespace detail
} // namespace wh::compose
