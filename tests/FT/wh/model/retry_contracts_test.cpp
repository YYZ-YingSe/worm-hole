#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/core/any.hpp"
#include "wh/core/run_context.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/model/retry.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/stream/core/types.hpp"
#include "wh/schema/stream/reader.hpp"

namespace {

template <typename timing_checker_t, typename callback_t, typename name_t>
  requires std::constructible_from<wh::core::callback_timing_checker, timing_checker_t &&> &&
           std::constructible_from<wh::core::stage_view_callback, callback_t &&> &&
           std::constructible_from<std::string, name_t &&>
[[nodiscard]] auto register_test_callbacks(wh::core::run_context &&context,
                                           timing_checker_t &&timing_checker, callback_t &&callback,
                                           name_t &&name = {})
    -> wh::core::result<wh::core::run_context> {
  wh::core::stage_view_callback stage_callback{std::forward<callback_t>(callback)};
  wh::core::stage_callbacks callbacks{};
  callbacks.on_start = stage_callback;
  callbacks.on_end = stage_callback;
  callbacks.on_error = stage_callback;
  callbacks.on_stream_start = stage_callback;
  callbacks.on_stream_end = std::move(stage_callback);
  return wh::core::register_local_callbacks(
      std::move(context), std::forward<timing_checker_t>(timing_checker), std::move(callbacks),
      std::string{std::forward<name_t>(name)});
}

[[nodiscard]] auto make_assistant_message(const std::string &text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::assistant;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

[[nodiscard]] auto make_user_message(const std::string &text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

[[nodiscard]] auto collect_stream_text(wh::model::chat_message_stream_reader &reader)
    -> std::vector<std::string> {
  std::vector<std::string> output{};
  while (true) {
    auto next = reader.read();
    REQUIRE(next.has_value());
    REQUIRE_FALSE(next.value().error.failed());
    if (next.value().eof) {
      break;
    }
    REQUIRE(next.value().value.has_value());
    output.push_back(std::get<wh::schema::text_part>(next.value().value->parts.front()).text);
  }
  return output;
}

struct invoke_retry_state {
  std::size_t invoke_calls{0U};
  std::size_t stream_calls{0U};
  std::size_t bind_calls{0U};
  std::size_t bound_tool_count{0U};
  std::size_t fail_until{0U};
  std::string success_text{"ok"};
};

struct flaky_invoke_model_impl {
  invoke_retry_state *state{nullptr};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"FlakyInvokeModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &) const
      -> wh::core::result<wh::model::chat_response> {
    ++state->invoke_calls;
    if (state->invoke_calls <= state->fail_until) {
      return wh::core::result<wh::model::chat_response>::failure(wh::core::errc::unavailable);
    }
    return wh::model::chat_response{.message = make_assistant_message(state->success_text)};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &) const
      -> wh::core::result<wh::model::chat_message_stream_reader> {
    ++state->stream_calls;
    return wh::model::chat_message_stream_reader{
        wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
            make_assistant_message(state->success_text))};
  }

  [[nodiscard]] auto
  bind_tools(const std::span<const wh::schema::tool_schema_definition> tools) const
      -> flaky_invoke_model_impl {
    ++state->bind_calls;
    state->bound_tool_count = tools.size();
    return *this;
  }
};

struct scripted_message_reader {
  using value_type = wh::schema::message;
  using chunk_type = wh::schema::stream::stream_chunk<value_type>;
  using chunk_result_type = wh::schema::stream::stream_result<chunk_type>;
  using chunk_try_result_type = wh::schema::stream::stream_try_result<chunk_type>;

  scripted_message_reader() = default;

  explicit scripted_message_reader(std::vector<chunk_result_type> chunks) noexcept
      : chunks_(std::move(chunks)) {}

  [[nodiscard]] auto read() -> chunk_result_type {
    if (closed_) {
      return chunk_type::make_eof();
    }
    if (index_ >= chunks_.size()) {
      closed_ = true;
      return chunk_type::make_eof();
    }
    auto next = chunks_[index_++];
    if (next.has_value() && (next.value().eof || next.value().error.failed())) {
      closed_ = true;
    }
    if (next.has_error()) {
      closed_ = true;
    }
    return next;
  }

  [[nodiscard]] auto try_read() -> chunk_try_result_type { return read(); }

  [[nodiscard]] auto read_async() { return stdexec::just(read()); }

  auto close() -> wh::core::result<void> {
    closed_ = true;
    return {};
  }

  [[nodiscard]] auto is_closed() const noexcept -> bool { return closed_; }

private:
  std::vector<chunk_result_type> chunks_{};
  std::size_t index_{0U};
  bool closed_{false};
};

struct flaky_stream_state {
  std::size_t invoke_calls{0U};
  std::size_t stream_calls{0U};
  std::vector<scripted_message_reader::chunk_result_type> first_attempt{};
  std::vector<scripted_message_reader::chunk_result_type> second_attempt{};
};

struct flaky_stream_model_impl {
  flaky_stream_state *state{nullptr};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return wh::core::component_descriptor{"FlakyStreamModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &) const
      -> wh::core::result<wh::model::chat_response> {
    ++state->invoke_calls;
    return wh::model::chat_response{.message = make_assistant_message("invoke")};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &) const
      -> wh::core::result<wh::model::chat_message_stream_reader> {
    ++state->stream_calls;
    if (state->stream_calls == 1U) {
      return wh::model::chat_message_stream_reader{scripted_message_reader{state->first_attempt}};
    }
    return wh::model::chat_message_stream_reader{scripted_message_reader{state->second_attempt}};
  }

  [[nodiscard]] auto bind_tools(const std::span<const wh::schema::tool_schema_definition>) const
      -> flaky_stream_model_impl {
    return *this;
  }
};

using invoke_model = wh::model::chat_model<flaky_invoke_model_impl>;
using stream_model = wh::model::chat_model<flaky_stream_model_impl>;

} // namespace

TEST_CASE("retry chat model retries invoke failures and preserves tool binding",
          "[core][adk][condition]") {
  invoke_retry_state state{};
  state.fail_until = 2U;
  invoke_model model{flaky_invoke_model_impl{.state = &state}};
  wh::model::retry_chat_model<invoke_model> retry{
      std::move(model), wh::model::retry_chat_model_options{.max_attempts = 4U}};

  wh::schema::tool_schema_definition tool{};
  tool.name = "lookup";
  tool.description = "lookup";
  auto bound = retry.bind_tools(std::span<const wh::schema::tool_schema_definition>{&tool, 1U});

  std::vector<wh::model::will_retry_error> retry_events{};
  wh::core::run_context context{};
  context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(context), [](const wh::core::callback_stage) noexcept -> bool { return true; },
      [&retry_events](const wh::core::callback_stage stage,
                      const wh::core::callback_event_view event,
                      const wh::core::callback_run_info &) {
        if (stage != wh::core::callback_stage::error) {
          return;
        }
        if (const auto *typed = wh::core::any_cast<wh::model::will_retry_error>(&event);
            typed != nullptr) {
          retry_events.push_back(*typed);
        }
      },
      "retry-invoke");
  REQUIRE(registered.has_value());
  context = std::move(registered).value();

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello"));
  auto status = bound.invoke(request, context);
  REQUIRE(status.has_value());
  REQUIRE(std::get<wh::schema::text_part>(status.value().message.parts.front()).text == "ok");
  REQUIRE(state.invoke_calls == 3U);
  REQUIRE(state.bind_calls == 1U);
  REQUIRE(state.bound_tool_count == 1U);
  REQUIRE(retry_events.size() == 2U);
  REQUIRE(retry_events.front().attempt == 1U);
  REQUIRE(retry_events.back().attempt == 2U);
}

TEST_CASE("retry chat model returns retry_exhausted after final retryable failure",
          "[core][adk][boundary]") {
  invoke_retry_state state{};
  state.fail_until = 5U;
  invoke_model model{flaky_invoke_model_impl{.state = &state}};
  wh::model::retry_chat_model<invoke_model> retry{
      std::move(model), wh::model::retry_chat_model_options{.max_attempts = 2U}};

  std::vector<wh::model::retry_exhausted_error> exhausted{};
  wh::core::run_context context{};
  context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(context), [](const wh::core::callback_stage) noexcept -> bool { return true; },
      [&exhausted](const wh::core::callback_stage stage, const wh::core::callback_event_view event,
                   const wh::core::callback_run_info &) {
        if (stage != wh::core::callback_stage::error) {
          return;
        }
        if (const auto *typed = wh::core::any_cast<wh::model::retry_exhausted_error>(&event);
            typed != nullptr) {
          exhausted.push_back(*typed);
        }
      },
      "retry-exhausted");
  REQUIRE(registered.has_value());
  context = std::move(registered).value();

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello"));
  auto status = retry.invoke(request, context);
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::retry_exhausted);
  REQUIRE(state.invoke_calls == 2U);
  REQUIRE(exhausted.size() == 1U);
  REQUIRE(exhausted.front().attempts == 2U);
  REQUIRE(exhausted.front().last_error == wh::core::errc::unavailable);
}

TEST_CASE("retry chat model custom predicate can stop retries immediately",
          "[core][adk][boundary]") {
  invoke_retry_state state{};
  state.fail_until = 3U;
  invoke_model model{flaky_invoke_model_impl{.state = &state}};
  wh::model::retry_chat_model<invoke_model> retry{
      std::move(model),
      wh::model::retry_chat_model_options{
          .max_attempts = 4U,
          .should_retry =
              wh::model::retry_predicate{[](const wh::core::error_code error) noexcept -> bool {
                return error != wh::core::errc::unavailable;
              }},
      }};

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello"));
  wh::core::run_context context{};
  auto status = retry.invoke(request, context);
  REQUIRE(status.has_error());
  REQUIRE(status.error() == wh::core::errc::unavailable);
  REQUIRE(state.invoke_calls == 1U);
}

TEST_CASE("retry chat model stream probe hides failed attempt fragments from success path",
          "[core][adk][condition]") {
  flaky_stream_state state{};
  scripted_message_reader::chunk_type first_value{};
  first_value.value = make_assistant_message("bad");
  scripted_message_reader::chunk_type first_error{};
  first_error.error = wh::core::make_error(wh::core::errc::unavailable);
  scripted_message_reader::chunk_type second_value{};
  second_value.value = make_assistant_message("good");
  state.first_attempt = {
      scripted_message_reader::chunk_result_type{first_value},
      scripted_message_reader::chunk_result_type{first_error},
  };
  state.second_attempt = {
      scripted_message_reader::chunk_result_type{second_value},
      scripted_message_reader::chunk_result_type{scripted_message_reader::chunk_type::make_eof()},
  };

  stream_model model{flaky_stream_model_impl{.state = &state}};
  wh::model::retry_chat_model<stream_model> retry{
      std::move(model), wh::model::retry_chat_model_options{.max_attempts = 3U}};

  std::vector<wh::model::will_retry_error> retry_events{};
  wh::core::run_context context{};
  context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(context), [](const wh::core::callback_stage) noexcept -> bool { return true; },
      [&retry_events](const wh::core::callback_stage stage,
                      const wh::core::callback_event_view event,
                      const wh::core::callback_run_info &) {
        if (stage != wh::core::callback_stage::error) {
          return;
        }
        if (const auto *typed = wh::core::any_cast<wh::model::will_retry_error>(&event);
            typed != nullptr) {
          retry_events.push_back(*typed);
        }
      },
      "retry-stream");
  REQUIRE(registered.has_value());
  context = std::move(registered).value();

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello"));
  auto status = retry.stream(request, context);
  REQUIRE(status.has_value());
  auto texts = collect_stream_text(status.value());
  REQUIRE(texts == std::vector<std::string>{"good"});
  REQUIRE(state.stream_calls == 2U);
  REQUIRE(retry_events.size() == 1U);
  REQUIRE(retry_events.front().attempt == 1U);
}
