// Defines callback injection helpers for start/end/error and stream stages.
#pragma once

#include "wh/core/run_context.hpp"
#include "wh/callbacks/interface.hpp"

namespace wh::callbacks {

template <typename payload_t>
/// Emits one callback event on an explicit callback stage.
inline auto emit(wh::core::run_context &context, const stage current_stage,
                 const payload_t &payload, const run_info &info) -> void {
  wh::core::inject_callback_event(context, current_stage, payload, info);
}

template <typename payload_t>
/// Emits one `start` callback event.
inline auto emit_start(wh::core::run_context &context, const payload_t &payload,
                       const run_info &info) -> void {
  emit(context, stage::start, payload, info);
}

template <typename payload_t>
/// Emits one `end` callback event.
inline auto emit_end(wh::core::run_context &context, const payload_t &payload,
                     const run_info &info) -> void {
  emit(context, stage::end, payload, info);
}

template <typename payload_t>
/// Emits one `error` callback event.
inline auto emit_error(wh::core::run_context &context,
                       const payload_t &payload, const run_info &info) -> void {
  emit(context, stage::error, payload, info);
}

template <typename payload_t>
/// Emits one `stream_start` callback event.
inline auto emit_stream_start(wh::core::run_context &context,
                              const payload_t &payload,
                              const run_info &info) -> void {
  emit(context, stage::stream_start, payload, info);
}

template <typename payload_t>
/// Emits one `stream_end` callback event.
inline auto emit_stream_end(wh::core::run_context &context,
                            const payload_t &payload,
                            const run_info &info) -> void {
  emit(context, stage::stream_end, payload, info);
}

} // namespace wh::callbacks
