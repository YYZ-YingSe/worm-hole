#include <array>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <stdexec/execution.hpp>

#include "wh/model/chat_model.hpp"

namespace {

template <typename text_t>
[[nodiscard]] auto make_user_message(text_t &&text) -> wh::schema::message {
  wh::schema::message message{};
  message.role = wh::schema::message_role::user;
  message.parts.emplace_back(wh::schema::text_part{std::forward<text_t>(text)});
  return message;
}

template <typename reader_t>
[[nodiscard]] auto take_try_chunk(reader_t &reader) ->
    typename std::remove_cvref_t<reader_t>::chunk_result_type {
  auto next = reader.try_read();
  REQUIRE_FALSE(std::holds_alternative<wh::schema::stream::stream_signal>(next));
  return std::move(std::get<typename std::remove_cvref_t<reader_t>::chunk_result_type>(next));
}

struct probe_state {
  std::size_t bind_calls{0U};
  std::size_t invoke_calls{0U};
  std::size_t stream_calls{0U};
  std::size_t bound_tool_count{0U};
};

class sync_probe_model_impl {
public:
  explicit sync_probe_model_impl(std::shared_ptr<probe_state> state,
                                 std::vector<wh::schema::tool_schema_definition> tools = {})
      : state_(std::move(state)), bound_tools_(std::move(tools)) {
    options_.set_base(wh::model::chat_model_common_options{.model_id = "probe"});
  }

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"ProbeModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &) const
      -> wh::model::chat_invoke_result {
    ++state_->invoke_calls;
    state_->bound_tool_count = bound_tools_.size();
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(
        wh::schema::text_part{"bound:" + std::to_string(bound_tools_.size())});
    return wh::model::chat_response{std::move(message), {}};
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &) const
      -> wh::model::chat_message_stream_result {
    ++state_->stream_calls;
    state_->bound_tool_count = bound_tools_.size();
    return wh::model::chat_message_stream_reader{
        wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
            wh::schema::message{.role = wh::schema::message_role::assistant,
                                .parts = {wh::schema::text_part{
                                    "stream:" + std::to_string(bound_tools_.size())}}})};
  }

  [[nodiscard]] auto bind_tools(std::span<const wh::schema::tool_schema_definition> tools) const
      -> sync_probe_model_impl {
    ++state_->bind_calls;
    return sync_probe_model_impl{
        state_, std::vector<wh::schema::tool_schema_definition>{tools.begin(), tools.end()}};
  }

  [[nodiscard]] auto options() const noexcept -> const wh::model::chat_model_options & {
    return options_;
  }

  [[nodiscard]] auto bound_tools() const noexcept
      -> const std::vector<wh::schema::tool_schema_definition> & {
    return bound_tools_;
  }

private:
  std::shared_ptr<probe_state> state_{};
  std::vector<wh::schema::tool_schema_definition> bound_tools_{};
  wh::model::chat_model_options options_{};
};

struct async_probe_model_impl {
  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {"AsyncProbeModel", wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke_sender(const wh::model::chat_request &) const {
    wh::schema::message message{};
    message.role = wh::schema::message_role::assistant;
    message.parts.emplace_back(wh::schema::text_part{"async"});
    return stdexec::just(wh::model::chat_invoke_result{wh::model::chat_response{message, {}}});
  }

  [[nodiscard]] auto stream_sender(const wh::model::chat_request &) const {
    return stdexec::just(
        wh::model::chat_message_stream_result{wh::model::chat_message_stream_reader{
            wh::schema::stream::make_single_value_stream_reader<wh::schema::message>(
                wh::schema::message{.role = wh::schema::message_role::assistant,
                                    .parts = {wh::schema::text_part{"async-stream"}}})}});
  }
};

struct failing_probe_model_impl {
  std::string name{};

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return {name, wh::core::component_kind::model};
  }

  [[nodiscard]] auto invoke(const wh::model::chat_request &) const
      -> wh::model::chat_invoke_result {
    return wh::model::chat_invoke_result::failure(wh::core::errc::timeout);
  }

  [[nodiscard]] auto stream(const wh::model::chat_request &) const
      -> wh::model::chat_message_stream_result {
    return wh::model::chat_message_stream_result::failure(wh::core::errc::timeout);
  }
};

} // namespace

TEST_CASE("chat model wrapper exposes sync surface options and tool binding",
          "[UT][wh/model/chat_model.hpp][chat_model::invoke][branch][boundary]") {
  auto state = std::make_shared<probe_state>();
  wh::model::chat_model wrapped{sync_probe_model_impl{state}};
  REQUIRE(wrapped.descriptor().type_name == "ProbeModel");
  REQUIRE(wrapped.options().resolve_view().model_id == "probe");
  REQUIRE(wrapped.bound_tools().empty());

  std::array<wh::schema::tool_schema_definition, 1> tools{{
      {.name = "search", .description = "lookup"},
  }};
  auto rebound = wrapped.bind_tools(tools);
  REQUIRE(state->bind_calls == 1U);
  REQUIRE(rebound.bound_tools().size() == 1U);

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello"));
  wh::core::run_context context{};

  auto invoked = rebound.invoke(request, context);
  REQUIRE(invoked.has_value());
  REQUIRE(std::get<wh::schema::text_part>(invoked.value().message.parts.front()).text == "bound:1");

  auto streamed = rebound.stream(request, context);
  REQUIRE(streamed.has_value());
  auto reader = std::move(streamed).value();
  auto first = take_try_chunk(reader);
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(first.value().value->parts.front()).text == "stream:1");
  REQUIRE(state->invoke_calls == 1U);
  REQUIRE(state->stream_calls == 1U);
  REQUIRE(state->bound_tool_count == 1U);
}

TEST_CASE("chat model wrapper normalizes async invoke and stream senders",
          "[UT][wh/model/chat_model.hpp][chat_model::async_invoke][branch]") {
  wh::model::chat_model wrapped{async_probe_model_impl{}};
  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("hello"));
  wh::core::run_context context{};

  auto invoked = stdexec::sync_wait(wrapped.async_invoke(request, context));
  REQUIRE(invoked.has_value());
  REQUIRE(std::get<0>(*invoked).has_value());
  REQUIRE(
      std::get<wh::schema::text_part>(std::get<0>(*invoked).value().message.parts.front()).text ==
      "async");

  auto streamed = stdexec::sync_wait(wrapped.async_stream(request, context));
  REQUIRE(streamed.has_value());
  REQUIRE(std::get<0>(*streamed).has_value());
  auto reader = std::move(std::get<0>(*streamed)).value();
  auto first = take_try_chunk(reader);
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(first.value().value->parts.front()).text ==
          "async-stream");
}

TEST_CASE("chat model fallback helpers report invoke and stream selections",
          "[UT][wh/model/chat_model.hpp][invoke_with_fallback_report_only][branch][boundary]") {
  auto state = std::make_shared<probe_state>();
  std::array<wh::model::chat_model<sync_probe_model_impl>, 1> models{
      wh::model::chat_model{sync_probe_model_impl{state}}};

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("fallback"));
  request.tools.push_back({.name = "search", .description = "lookup"});

  auto invoke_report = wh::model::invoke_with_fallback_report_only(
      std::span<const wh::model::chat_model<sync_probe_model_impl>>{models}, request);
  REQUIRE_FALSE(invoke_report.final_error.has_value());
  REQUIRE(invoke_report.frozen_candidates == std::vector<std::string>{"ProbeModel"});
  REQUIRE(invoke_report.response.message.role == wh::schema::message_role::assistant);
  REQUIRE(state->bind_calls == 1U);

  auto stream_report = wh::model::stream_with_fallback_report_only(
      std::span<const wh::model::chat_model<sync_probe_model_impl>>{models}, request);
  REQUIRE_FALSE(stream_report.final_error.has_value());
  REQUIRE(stream_report.selected_model == "ProbeModel");
  auto reader = std::move(stream_report.reader);
  auto first = take_try_chunk(reader);
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(std::get<wh::schema::text_part>(first.value().value->parts.front()).text == "stream:1");

  wh::model::chat_model_common_options forced{};
  forced.tool_choice.mode = wh::schema::tool_call_mode::force;
  forced.allowed_tool_names = {"missing"};
  request.options.set_base(forced);
  auto missing = wh::model::invoke_with_fallback_report_only(
      std::span<const wh::model::chat_model<sync_probe_model_impl>>{models}, request);
  REQUIRE(missing.final_error.has_value());
  REQUIRE(*missing.final_error == wh::core::errc::not_found);

  wh::model::chat_request empty_request{};
  auto no_models = wh::model::invoke_with_fallback_report_only(
      std::span<const wh::model::chat_model<sync_probe_model_impl>>{}, empty_request);
  REQUIRE(no_models.final_error.has_value());
  REQUIRE(*no_models.final_error == wh::core::errc::not_found);
}

TEST_CASE("chat model fallback helpers honor keep-failure-reasons when all candidates fail",
          "[UT][wh/model/"
          "chat_model.hpp][stream_with_fallback_report_only][condition][branch][boundary]") {
  std::array<wh::model::chat_model<failing_probe_model_impl>, 2> models{
      wh::model::chat_model{failing_probe_model_impl{"slow"}},
      wh::model::chat_model{failing_probe_model_impl{"fast"}}};

  wh::model::chat_request request{};
  request.messages.push_back(make_user_message("fail"));

  wh::model::chat_model_common_options keep{};
  keep.fallback.keep_failure_reasons = true;
  request.options.set_base(keep);
  auto kept = wh::model::invoke_with_fallback_report_only(
      std::span<const wh::model::chat_model<failing_probe_model_impl>>{models}, request);
  REQUIRE(kept.final_error.has_value());
  REQUIRE(*kept.final_error == wh::core::errc::timeout);
  REQUIRE(kept.attempts.size() == 2U);
  REQUIRE(kept.frozen_candidates == std::vector<std::string>{"slow", "fast"});

  wh::model::chat_model_common_options drop{};
  drop.fallback.keep_failure_reasons = false;
  request.options.set_base(drop);
  auto dropped = wh::model::stream_with_fallback_report_only(
      std::span<const wh::model::chat_model<failing_probe_model_impl>>{models}, request);
  REQUIRE(dropped.final_error.has_value());
  REQUIRE(*dropped.final_error == wh::core::errc::timeout);
  REQUIRE(dropped.attempts.empty());
}
