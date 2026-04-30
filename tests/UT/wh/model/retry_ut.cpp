#include <array>
#include <chrono>
#include <memory>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "helper/test_thread_wait.hpp"
#include "wh/model/echo_chat_model.hpp"
#include "wh/model/retry.hpp"

namespace {

struct flaky_model {
  mutable std::size_t invoke_calls{0U};
  mutable std::size_t stream_calls{0U};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"FlakyModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &, wh::core::run_context &) const
      -> wh::model::chat_invoke_result {
    ++invoke_calls;
    if (invoke_calls == 1U) {
      return wh::core::result<wh::model::chat_response>::failure(wh::core::errc::unavailable);
    }
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"ok"});
    return wh::model::chat_response{message, message.meta};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &, wh::core::run_context &) const
      -> wh::model::chat_message_stream_result {
    ++stream_calls;
    if (stream_calls == 1U) {
      return wh::core::result<wh::model::chat_message_stream_reader>::failure(
          wh::core::errc::unavailable);
    }
    return wh::model::chat_message_stream_reader{
        wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
            wh::schema::message{.role = wh::schema::message_role::assistant,
                                .parts = {wh::schema::text_part{"stream-ok"}}})};
  }

  [[nodiscard]] auto bind_tools(const std::span<const wh::schema::tool_schema_definition>) const
      -> flaky_model {
    return *this;
  }
};

[[nodiscard]] auto make_user_message(const std::string &text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{text});
  return message;
}

struct async_retry_state {
  std::size_t invoke_calls{0U};
  std::size_t stream_calls{0U};
};

struct async_retry_model_impl {
  std::shared_ptr<async_retry_state> state{};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"AsyncRetryModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke_sender(const wh::model::chat_request &) const {
    ++state->invoke_calls;
    if (state->invoke_calls == 1U) {
      return stdexec::just(
          wh::model::chat_invoke_result::failure(wh::core::errc::unavailable));
    }
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"async-ok"});
    return stdexec::just(wh::model::chat_invoke_result{
        wh::model::chat_response{.message = std::move(message)}});
  }

  [[nodiscard]] auto stream_sender(const wh::model::chat_request &) const {
    ++state->stream_calls;
    if (state->stream_calls == 1U) {
      return stdexec::just(
          wh::model::chat_message_stream_result::failure(wh::core::errc::unavailable));
    }
    return stdexec::just(wh::model::chat_message_stream_result{
        wh::model::chat_message_stream_reader{
            wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
                wh::schema::message{.role = wh::schema::message_role::assistant,
                                    .parts = {wh::schema::text_part{"async-stream-ok"}}})}});
  }

  [[nodiscard]] auto bind_tools(const std::span<const wh::schema::tool_schema_definition>) const
      -> async_retry_model_impl {
    return *this;
  }
};

struct scripted_async_message_reader {
  using value_type = wh::schema::message;
  using chunk_type = wh::schema::stream::stream_chunk<value_type>;
  using chunk_result_type = wh::schema::stream::stream_result<chunk_type>;
  using chunk_try_result_type = wh::schema::stream::stream_try_result<chunk_type>;

  scripted_async_message_reader() = default;

  explicit scripted_async_message_reader(std::vector<chunk_result_type> chunks) noexcept
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
    if (next.has_error()) {
      closed_ = true;
      return next;
    }
    if (next.value().eof || next.value().error.failed()) {
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

struct async_midstream_retry_state {
  std::size_t invoke_calls{0U};
  std::size_t stream_calls{0U};
  std::vector<scripted_async_message_reader::chunk_result_type> first_attempt{};
  std::vector<scripted_async_message_reader::chunk_result_type> second_attempt{};
};

struct async_midstream_retry_model_impl {
  std::shared_ptr<async_midstream_retry_state> state{};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"AsyncMidstreamRetryModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke_sender(const wh::model::chat_request &) const {
    ++state->invoke_calls;
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"invoke"});
    return stdexec::just(wh::model::chat_invoke_result{
        wh::model::chat_response{.message = std::move(message)}});
  }

  [[nodiscard]] auto stream_sender(const wh::model::chat_request &) const {
    ++state->stream_calls;
    if (state->stream_calls == 1U) {
      return stdexec::just(wh::model::chat_message_stream_result{
          wh::model::chat_message_stream_reader{
              scripted_async_message_reader{state->first_attempt}}});
    }
    return stdexec::just(wh::model::chat_message_stream_result{
        wh::model::chat_message_stream_reader{
            scripted_async_message_reader{state->second_attempt}}});
  }

  [[nodiscard]] auto bind_tools(const std::span<const wh::schema::tool_schema_definition>) const
      -> async_midstream_retry_model_impl {
    return *this;
  }
};

} // namespace

TEST_CASE("retry chat model retries invoke and stream before succeeding",
          "[UT][wh/model/retry.hpp][retry_chat_model::invoke][branch][concurrency]") {
  wh::model::retry_chat_model wrapped{
      flaky_model{},
      wh::model::retry_chat_model_options{
          .max_attempts = 3U,
          .should_retry =
              [](const wh::core::error_code error) { return error == wh::core::errc::unavailable; },
      }};

  wh::model::chat_request request{};
  request.messages.push_back(wh::schema::message{.role = wh::schema::message_role::user,
                                                 .parts = {wh::schema::text_part{"hello"}}});

  wh::core::run_context context{};
  auto invoked = wrapped.invoke(request, context);
  REQUIRE(invoked.has_value());
  REQUIRE(std::get<wh::schema::text_part>(invoked.value().message.parts.front()).text == "ok");

  auto streamed = wrapped.stream(request, context);
  REQUIRE(streamed.has_value());
  REQUIRE(wrapped.wrapped_model().stream_calls == 2U);
  auto collected = wh::schema::stream::collect_stream_reader(std::move(streamed).value());
  REQUIRE(collected.has_value());
  REQUIRE(std::get<wh::schema::text_part>(collected.value().front().parts.front()).text ==
          "stream-ok");
}

TEST_CASE("retry chat model reports exhausted retries and rebinds tools",
          "[UT][wh/model/retry.hpp][retry_chat_model::bind_tools][branch]") {
  wh::model::retry_chat_model wrapped{wh::model::echo_chat_model{},
                                      wh::model::retry_chat_model_options{
                                          .max_attempts = 1U,
                                      }};
  auto rebound = wrapped.bind_tools(std::array<wh::schema::tool_schema_definition, 1>{{
      {.name = "search", .description = "lookup"},
  }});
  REQUIRE(rebound.options().max_attempts == 1U);
}

TEST_CASE("retry helpers cap backoff and resolve terminal versus exhausted failures",
          "[UT][wh/model/retry.hpp][detail::resolve_retry_failure][condition][branch][boundary]") {
  using namespace std::chrono_literals;

  const auto capped = wh::model::detail::default_retry_backoff(99U);
  REQUIRE(capped <= 10s);

  wh::core::run_context context{};
  const auto descriptor = wh::model::retry_chat_model{wh::model::echo_chat_model{}}.descriptor();
  const auto max_attempts = wh::model::detail::max_retry_attempts({});

  wh::model::retry_chat_model_options terminal_options{};
  terminal_options.should_retry =
      wh::model::retry_predicate{[](const wh::core::error_code) noexcept { return false; }};
  auto terminal = wh::model::detail::resolve_retry_failure(
      context, descriptor, terminal_options, 1U, max_attempts, wh::core::errc::timeout);
  REQUIRE(terminal.has_value());
  REQUIRE(*terminal == wh::core::errc::timeout);

  wh::model::retry_chat_model_options exhausted_options{};
  exhausted_options.max_attempts = 3U;
  auto exhausted = wh::model::detail::resolve_retry_failure(
      context, descriptor, exhausted_options, 3U, wh::model::detail::max_retry_attempts(exhausted_options),
      wh::core::errc::timeout);
  REQUIRE(exhausted.has_value());
  REQUIRE(*exhausted == wh::core::errc::retry_exhausted);
}

TEST_CASE("retry chat model exposes async invoke and stream for async-only models",
          "[UT][wh/model/retry.hpp][retry_chat_model::async_stream][async]") {
  auto state = std::make_shared<async_retry_state>();
  wh::model::chat_model async_model{async_retry_model_impl{state}};
  wh::model::retry_chat_model wrapped{
      std::move(async_model),
      wh::model::retry_chat_model_options{
          .max_attempts = 3U,
          .should_retry =
              [](const wh::core::error_code error) { return error == wh::core::errc::unavailable; },
      }};

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello"));
  wh::core::run_context context{};

  auto invoked = wh::testing::helper::wait_value_on_test_thread(wrapped.async_invoke(request, context));
  REQUIRE(invoked.has_value());
  REQUIRE(std::get<wh::schema::text_part>(invoked.value().message.parts.front()).text == "async-ok");
  REQUIRE(state->invoke_calls == 2U);

  auto streamed = wh::testing::helper::wait_value_on_test_thread(wrapped.async_stream(request, context));
  REQUIRE(streamed.has_value());
  REQUIRE(state->stream_calls == 2U);
  auto reader = std::move(streamed).value();
  auto collected = wh::schema::stream::collect_stream_reader(std::move(reader));
  REQUIRE(collected.has_value());
  REQUIRE(std::get<wh::schema::text_part>(collected.value().front().parts.front()).text ==
          "async-stream-ok");
}

TEST_CASE("retry chat model read_async retries mid-stream for async models",
          "[UT][wh/model/retry.hpp][retry_stream_reader::read_async][async][stream]") {
  auto state = std::make_shared<async_midstream_retry_state>();
  scripted_async_message_reader::chunk_type first_value{};
  first_value.value = make_user_message("bad");
  first_value.value->role = wh::schema::message_role::assistant;
  scripted_async_message_reader::chunk_type first_error{};
  first_error.error = wh::core::make_error(wh::core::errc::unavailable);
  scripted_async_message_reader::chunk_type second_value{};
  second_value.value = make_user_message("good");
  second_value.value->role = wh::schema::message_role::assistant;
  state->first_attempt = {
      scripted_async_message_reader::chunk_result_type{first_value},
      scripted_async_message_reader::chunk_result_type{first_error},
  };
  state->second_attempt = {
      scripted_async_message_reader::chunk_result_type{second_value},
      scripted_async_message_reader::chunk_result_type{
          scripted_async_message_reader::chunk_type::make_eof()},
  };

  wh::model::chat_model async_model{async_midstream_retry_model_impl{state}};
  wh::model::retry_chat_model wrapped{
      std::move(async_model), wh::model::retry_chat_model_options{.max_attempts = 3U}};

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello"));
  wh::core::run_context context{};

  auto streamed = wh::testing::helper::wait_value_on_test_thread(wrapped.async_stream(request, context));
  REQUIRE(streamed.has_value());
  REQUIRE(state->stream_calls == 1U);

  auto reader = std::move(streamed).value();
  auto first = wh::testing::helper::wait_value_on_test_thread(reader.read_async());
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(first.value().value->parts.front()).text == "bad");
  REQUIRE(state->stream_calls == 1U);

  auto second = wh::testing::helper::wait_value_on_test_thread(reader.read_async());
  REQUIRE(second.has_value());
  REQUIRE(second.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(second.value().value->parts.front()).text == "good");
  REQUIRE(state->stream_calls == 2U);

  auto eof = wh::testing::helper::wait_value_on_test_thread(reader.read_async());
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
}
