#include <catch2/catch_test_macros.hpp>

#include <array>
#include <memory>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/async/completion_tokens.hpp"
#include "wh/core/error.hpp"
#include "wh/core/type_utils.hpp"
#include "wh/output/output_parser.hpp"
#include "wh/scheduler/context_helper.hpp"
#include "wh/schema/message_parser.hpp"
#include "wh/schema/select.hpp"
#include "wh/schema/stream.hpp"

namespace {

struct int_parser final
    : wh::output::output_parser_base<int_parser, std::string, int> {
  std::vector<wh::core::error_code> errors{};

  auto parse_value_impl(const std::string &input,
                        const wh::output::callback_extra_payload &)
      -> wh::core::result<int> {
    try {
      return std::stoi(input);
    } catch (...) {
      return wh::core::result<int>::failure(wh::core::errc::parse_error);
    }
  }

  auto on_error_impl(const wh::core::error_code error,
                     const wh::output::callback_extra_payload &) -> void {
    errors.push_back(error);
  }
};

struct int_parser_view_preferred final
    : wh::output::output_parser_base<int_parser_view_preferred, std::string,
                                     int> {
  bool view_used{false};

  auto parse_value_impl(const std::string &,
                        const wh::output::callback_extra_payload &)
      -> wh::core::result<int> {
    return wh::core::result<int>::failure(wh::core::errc::parse_error);
  }

  auto parse_value_view_impl(const std::string &input,
                             const wh::output::callback_extra_view &)
      -> wh::core::result<int> {
    view_used = true;
    try {
      return std::stoi(input);
    } catch (...) {
      return wh::core::result<int>::failure(wh::core::errc::parse_error);
    }
  }
};

template <typename value_t, typename sender_t>
[[nodiscard]] auto consume_sender(sender_t &&sender) -> value_t {
  auto sync_result = stdexec::sync_wait(std::forward<sender_t>(sender));
  REQUIRE(sync_result.has_value());
  return std::move(std::get<0>(sync_result.value()));
}

struct throwing_reader_state {
  int read_count{0};
  int close_count{0};
  bool closed{false};
};

class throwing_reader final
    : public wh::schema::stream::stream_base<throwing_reader, int> {
public:
  explicit throwing_reader(std::shared_ptr<throwing_reader_state> state)
      : state_(std::move(state)) {}

  [[nodiscard]] auto next_impl()
      -> wh::core::result<wh::schema::stream::stream_chunk<int>> {
    if (!state_ || state_->closed) {
      return wh::core::result<wh::schema::stream::stream_chunk<int>>::failure(
          wh::core::errc::channel_closed);
    }
    ++state_->read_count;
    if (state_->read_count == 1) {
      return wh::schema::stream::stream_chunk<int>::make_value(7);
    }
    throw std::runtime_error("boom");
  }

  auto close_impl() -> wh::core::result<void> {
    if (!state_) {
      return wh::core::result<void>::failure(wh::core::errc::not_found);
    }
    if (!state_->closed) {
      state_->closed = true;
      ++state_->close_count;
    }
    return {};
  }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return !state_ || state_->closed;
  }

private:
  std::shared_ptr<throwing_reader_state> state_{};
};

} // namespace

TEST_CASE("message parser supports content and json modes",
          "[core][l2][message_parser][condition]") {
  const auto plain = wh::schema::parse_message(
      "hello", wh::schema::message_parse_config{
                   .from = wh::schema::message_parse_from::content});
  REQUIRE(plain.has_value());
  REQUIRE(plain.value().parts.size() == 1U);
  REQUIRE(std::get<wh::schema::text_part>(plain.value().parts.front()).text ==
          "hello");

  const auto parsed_json = wh::schema::parse_message(
      R"({"message":{"role":"assistant","content":"json-content"}})",
      wh::schema::message_parse_config{
          .from = wh::schema::message_parse_from::full_json,
          .key_path = "message",
      });
  REQUIRE(parsed_json.has_value());
  REQUIRE(parsed_json.value().parts.size() == 1U);
  REQUIRE(
      std::get<wh::schema::text_part>(parsed_json.value().parts.front()).text ==
      "json-content");
}

TEST_CASE("message parser tool-call mode validates missing branches",
          "[core][l2][message_parser][boundary]") {
  const auto parsed = wh::schema::parse_message(
      R"({"tool_calls":[{"function":{"arguments":"{\"k\":1}"}}]})",
      wh::schema::message_parse_config{
          .from = wh::schema::message_parse_from::tool_calls});
  REQUIRE(parsed.has_value());
  REQUIRE(std::get<wh::schema::text_part>(parsed.value().parts.front()).text ==
          R"({"k":1})");

  const auto missing = wh::schema::parse_message(
      R"({"tool_calls":[]})",
      wh::schema::message_parse_config{
          .from = wh::schema::message_parse_from::tool_calls});
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE("pipe stream supports queue and close semantics",
          "[core][l2][stream][condition]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(2U);

  REQUIRE(writer.try_write(1).has_value());
  REQUIRE(writer.try_write(2).has_value());
  const auto full = writer.try_write(3);
  REQUIRE(full.has_error());
  REQUIRE(full.error() == wh::core::errc::queue_full);

  auto first = reader.try_read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(*first.value().value == 1);

  REQUIRE(writer.close().has_value());
  auto second = reader.try_read();
  REQUIRE(second.has_value());
  REQUIRE(*second.value().value == 2);

  auto eof = reader.try_read();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);

  auto after_eof = reader.try_read();
  REQUIRE(after_eof.has_error());
  REQUIRE(after_eof.error() == wh::core::errc::channel_closed);
}

TEST_CASE("pipe stream borrowed read uses transient view contract",
          "[core][l2][stream][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(2U);
  REQUIRE(writer.try_write("alpha").has_value());
  REQUIRE(writer.close().has_value());

  auto first = reader.try_read_borrowed_until_next();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value != nullptr);
  REQUIRE(*first.value().value == "alpha");

  auto eof = reader.try_read_borrowed_until_next();
  REQUIRE(eof.has_value());
  REQUIRE(eof.value().eof);
  REQUIRE(eof.value().value == nullptr);

  auto after_eof = reader.try_read_borrowed_until_next();
  REQUIRE(after_eof.has_error());
  REQUIRE(after_eof.error() == wh::core::errc::channel_closed);
}

TEST_CASE("pipe stream reader close is visible to writer",
          "[core][l2][stream][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);

  REQUIRE(reader.close().has_value());
  const auto write_after_close = writer.try_write(7);
  REQUIRE(write_after_close.has_error());
  REQUIRE(write_after_close.error() == wh::core::errc::channel_closed);
}

TEST_CASE("pipe stream completion token three-mode contracts",
          "[core][l2][stream][condition]") {
  using writer_t = wh::schema::stream::pipe_stream_writer<int>;
  using reader_t = wh::schema::stream::pipe_stream_reader<int>;
  using context_t =
      decltype(wh::core::make_scheduler_context(stdexec::inline_scheduler{}));

  static_assert(
      wh::core::is_sender_v<decltype(std::declval<writer_t &>().write_async(
          std::declval<context_t>(), std::declval<int>(),
          wh::core::use_sender))>);
  static_assert(wh::core::is_sender_v<decltype(std::declval<reader_t &>().recv(
                    std::declval<context_t>(), wh::core::use_sender))>);
  static_assert(
      wh::core::is_sender_v<decltype(std::declval<writer_t &>().write_async(
          std::declval<context_t>(), std::declval<int>(),
          wh::core::use_awaitable))>);
  static_assert(wh::core::is_sender_v<decltype(std::declval<reader_t &>().recv(
                    std::declval<context_t>(), wh::core::use_awaitable))>);

  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(2U);
  auto context = wh::core::make_scheduler_context(stdexec::inline_scheduler{});

  std::binary_semaphore callback_done{0};
  std::optional<wh::core::result<void>> callback_write{};
  writer.write_async(context, 7,
                     wh::core::use_callback([&](wh::core::result<void> status) {
                       callback_write = std::move(status);
                       callback_done.release();
                     }));
  callback_done.acquire();
  REQUIRE(callback_write.has_value());
  REQUIRE(callback_write->has_value());

  auto awaitable_recv = reader.recv(context, wh::core::use_awaitable);
  auto first =
      consume_sender<wh::core::result<wh::schema::stream::stream_chunk<int>>>(
          std::move(awaitable_recv));
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(*first.value().value == 7);

  auto sender_write = writer.write_async(context, 8, wh::core::use_sender);
  auto written =
      consume_sender<wh::core::result<void>>(std::move(sender_write));
  REQUIRE(written.has_value());

  auto sender_recv = reader.recv(context, wh::core::use_sender);
  auto second =
      consume_sender<wh::core::result<wh::schema::stream::stream_chunk<int>>>(
          std::move(sender_recv));
  REQUIRE(second.has_value());
  REQUIRE(second.value().value.has_value());
  REQUIRE(*second.value().value == 8);
}

TEST_CASE("named stream merge emits source eof markers",
          "[core][l2][stream][branch]") {
  auto [writer_a, reader_a] =
      wh::schema::stream::make_pipe_stream<std::string>();
  auto [writer_b, reader_b] =
      wh::schema::stream::make_pipe_stream<std::string>();

  REQUIRE(writer_a.try_write("a1").has_value());
  REQUIRE(writer_b.try_write("b1").has_value());
  REQUIRE(writer_a.close().has_value());
  REQUIRE(writer_b.close().has_value());

  auto merged = wh::schema::stream::merge_named<std::string>({
      wh::schema::stream::named_stream_reader<std::string>{"A", reader_a},
      wh::schema::stream::named_stream_reader<std::string>{"B", reader_b},
  });

  std::vector<std::string> payloads{};
  std::vector<std::string> eofs{};
  for (int i = 0; i < 6; ++i) {
    auto chunk = merged.next();
    if (chunk.has_error()) {
      if (chunk.error() == wh::core::errc::queue_empty) {
        continue;
      }
      FAIL("unexpected stream error");
    }
    if (chunk.value().eof) {
      if (!chunk.value().source.empty()) {
        eofs.push_back(chunk.value().source);
      } else {
        break;
      }
      continue;
    }
    payloads.push_back(*chunk.value().value);
  }

  REQUIRE(payloads.size() == 2U);
  REQUIRE(eofs.size() == 2U);
}

TEST_CASE("stream select switches fixed and dynamic paths",
          "[core][l2][stream][condition]") {
  {
    std::vector<wh::schema::stream::pipe_stream_reader<int>> readers{};
    readers.reserve(5U);
    std::vector<wh::schema::stream::pipe_stream_writer<int>> writers{};
    writers.reserve(5U);
    for (int i = 0; i < 5; ++i) {
      auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
      REQUIRE(writer.try_write(i).has_value());
      REQUIRE(writer.close().has_value());
      writers.push_back(std::move(writer));
      readers.push_back(std::move(reader));
    }

    auto selected = wh::schema::stream::select<int>(std::move(readers));
    REQUIRE(selected.uses_fixed_select_path());
    std::size_t value_count = 0U;
    std::size_t source_eof_count = 0U;
    for (int i = 0; i < 32; ++i) {
      auto chunk = selected.try_read();
      if (chunk.has_error()) {
        if (chunk.error() == wh::core::errc::queue_empty) {
          continue;
        }
        FAIL("unexpected error in fixed path");
      }
      if (chunk.value().eof) {
        if (chunk.value().source.empty()) {
          break;
        }
        ++source_eof_count;
      } else {
        ++value_count;
      }
    }
    REQUIRE(value_count == 5U);
    REQUIRE(source_eof_count == 5U);
  }

  {
    std::vector<wh::schema::stream::pipe_stream_reader<int>> readers{};
    readers.reserve(6U);
    std::vector<wh::schema::stream::pipe_stream_writer<int>> writers{};
    writers.reserve(6U);
    for (int i = 0; i < 6; ++i) {
      auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(1U);
      REQUIRE(writer.try_write(i).has_value());
      REQUIRE(writer.close().has_value());
      writers.push_back(std::move(writer));
      readers.push_back(std::move(reader));
    }

    auto selected = wh::schema::stream::select<int>(std::move(readers));
    REQUIRE(!selected.uses_fixed_select_path());
    std::size_t value_count = 0U;
    std::size_t source_eof_count = 0U;
    for (int i = 0; i < 40; ++i) {
      auto chunk = selected.try_read();
      if (chunk.has_error()) {
        if (chunk.error() == wh::core::errc::queue_empty) {
          continue;
        }
        FAIL("unexpected error in dynamic path");
      }
      if (chunk.value().eof) {
        if (chunk.value().source.empty()) {
          break;
        }
        ++source_eof_count;
      } else {
        ++value_count;
      }
    }
    REQUIRE(value_count == 6U);
    REQUIRE(source_eof_count == 6U);
  }
}

TEST_CASE("named merge sorts source names for stable replay",
          "[core][l2][stream][branch]") {
  auto [writer_z, reader_z] = wh::schema::stream::make_pipe_stream<int>(1U);
  auto [writer_a, reader_a] = wh::schema::stream::make_pipe_stream<int>(1U);
  REQUIRE(writer_z.try_write(1).has_value());
  REQUIRE(writer_a.try_write(2).has_value());
  REQUIRE(writer_z.close().has_value());
  REQUIRE(writer_a.close().has_value());

  auto merged = wh::schema::stream::merge_named<int>({
      wh::schema::stream::named_stream_reader<int>{"z", reader_z},
      wh::schema::stream::named_stream_reader<int>{"a", reader_a},
  });

  auto first = merged.try_read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(first.value().source == "a");
}

TEST_CASE("stream copy readers keep independent cursors",
          "[core][l2][stream][branch]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(4U);
  REQUIRE(writer.try_write("alpha").has_value());
  REQUIRE(writer.try_write("beta").has_value());
  REQUIRE(writer.close().has_value());

  auto copies_result = wh::schema::stream::copy_readers(std::move(reader), 2U);
  REQUIRE(std::holds_alternative<std::vector<
          wh::schema::stream::copied_stream_reader<
              wh::schema::stream::pipe_stream_reader<std::string>>>>(
      copies_result));
  auto copies = std::get<std::vector<
      wh::schema::stream::copied_stream_reader<
          wh::schema::stream::pipe_stream_reader<std::string>>>>(
      std::move(copies_result));
  REQUIRE(copies.size() == 2U);

  auto drain = [](auto &copy_reader) {
    std::vector<std::string> values{};
    for (int i = 0; i < 16; ++i) {
      auto chunk = copy_reader.try_read();
      if (chunk.has_error()) {
        if (chunk.error() == wh::core::errc::queue_empty) {
          continue;
        }
        FAIL("unexpected error while draining copied reader");
      }
      if (chunk.value().eof) {
        break;
      }
      values.push_back(*chunk.value().value);
    }
    return values;
  };

  auto first_values = drain(copies[0]);
  auto second_values = drain(copies[1]);
  REQUIRE(first_values == std::vector<std::string>{"alpha", "beta"});
  REQUIRE(second_values == std::vector<std::string>{"alpha", "beta"});
}

TEST_CASE("stream copy readers returns original for n less than two",
          "[core][l2][stream][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<int>(2U);
  REQUIRE(writer.try_write(1).has_value());
  REQUIRE(writer.close().has_value());

  auto copied = wh::schema::stream::copy_readers(std::move(reader), 1U);
  REQUIRE(
      std::holds_alternative<wh::schema::stream::pipe_stream_reader<int>>(
          copied));

  auto original = std::get<wh::schema::stream::pipe_stream_reader<int>>(
      std::move(copied));
  auto first = original.try_read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(*first.value().value == 1);
}

TEST_CASE("stream convert supports skip-current-chunk behavior",
          "[core][l2][stream][boundary]") {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(4U);
  REQUIRE(writer.try_write("1").has_value());
  REQUIRE(writer.try_write("skip").has_value());
  REQUIRE(writer.try_write("2").has_value());
  REQUIRE(writer.close().has_value());

  auto converted = wh::schema::stream::convert(
      std::move(reader), [](const std::string &input) -> wh::core::result<int> {
        if (input == "skip") {
          return wh::core::result<int>::failure(wh::core::errc::queue_empty);
        }
        try {
          return std::stoi(input);
        } catch (...) {
          return wh::core::result<int>::failure(wh::core::errc::parse_error);
        }
      });

  std::vector<int> parsed_values{};
  for (int i = 0; i < 16; ++i) {
    auto chunk = converted.try_read();
    if (chunk.has_error()) {
      if (chunk.error() == wh::core::errc::queue_empty) {
        continue;
      }
      FAIL("unexpected error from converted reader");
    }
    if (chunk.value().eof) {
      break;
    }
    parsed_values.push_back(*chunk.value().value);
  }

  REQUIRE(parsed_values == std::vector<int>{1, 2});
}

TEST_CASE("to-stream bridge captures exceptions as error chunks",
          "[core][l2][stream][condition]") {
  auto state = std::make_shared<throwing_reader_state>();
  auto bridged = wh::schema::stream::to_stream(throwing_reader{state});

  auto first = bridged.try_read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(*first.value().value == 7);

  auto second = bridged.try_read();
  REQUIRE(second.has_value());
  REQUIRE(second.value().error.failed());
  REQUIRE(second.value().error.code() == wh::core::errc::internal_error);
  REQUIRE(state->close_count == 1);

  auto third = bridged.try_read();
  REQUIRE(third.has_error());
  REQUIRE(third.error() == wh::core::errc::channel_closed);
}

TEST_CASE("to-stream bridge supports disabling automatic close",
          "[core][l2][stream][boundary]") {
  auto state = std::make_shared<throwing_reader_state>();
  auto bridged = wh::schema::stream::to_stream(throwing_reader{state});
  bridged.set_automatic_close(wh::schema::stream::auto_close_disabled);

  auto first = bridged.try_read();
  REQUIRE(first.has_value());
  auto second = bridged.try_read();
  REQUIRE(second.has_value());
  REQUIRE(second.value().error.failed());
  REQUIRE(state->close_count == 0);

  auto closed = bridged.close();
  REQUIRE(closed.has_value());
  REQUIRE(state->close_count == 1);
}

TEST_CASE("output parser base handles value and stream chunk paths",
          "[core][l2][output][condition]") {
  int_parser parser{};

  auto value = parser.parse_value("42");
  REQUIRE(value.has_value());
  REQUIRE(value.value() == 42);

  auto bad_value = parser.parse_value("bad");
  REQUIRE(bad_value.has_error());
  REQUIRE(bad_value.error() == wh::core::errc::parse_error);

  wh::schema::stream::stream_chunk<std::string> input_chunk =
      wh::schema::stream::stream_chunk<std::string>::make_value("7");
  auto parsed_chunk = parser.parse_stream_chunk(input_chunk);
  REQUIRE(parsed_chunk.has_value());
  REQUIRE(parsed_chunk.value().value.has_value());
  REQUIRE(*parsed_chunk.value().value == 7);

  wh::schema::stream::stream_chunk<std::string> eof_chunk =
      wh::schema::stream::stream_chunk<std::string>::make_eof();
  auto parsed_eof = parser.parse_stream_chunk(eof_chunk);
  REQUIRE(parsed_eof.has_value());
  REQUIRE(parsed_eof.value().eof);

  wh::schema::stream::stream_chunk<std::string> bad_chunk =
      wh::schema::stream::stream_chunk<std::string>::make_value("x");
  auto parsed_bad = parser.parse_stream_chunk(bad_chunk);
  REQUIRE(parsed_bad.has_error());
  REQUIRE(parsed_bad.error() == wh::core::errc::parse_error);
  REQUIRE(!parser.errors.empty());
}

TEST_CASE("callback extra payload supports cref and pointer fast path",
          "[core][l2][output][boundary]") {
  const auto extra = wh::output::make_callback_extra_payload(
      std::string{"payload-text"});

  const auto *raw = wh::output::callback_extra_get_if<std::string>(extra);
  REQUIRE(raw != nullptr);
  REQUIRE(*raw == "payload-text");

  const auto cref = wh::output::callback_extra_cref_as<std::string>(extra);
  REQUIRE(cref.has_value());
  REQUIRE(cref.value().get() == "payload-text");

  const auto mismatch = wh::output::callback_extra_cref_as<int>(extra);
  REQUIRE(mismatch.has_error());
  REQUIRE(mismatch.error() == wh::core::errc::type_mismatch);
}

TEST_CASE("output parser parse_stream closes upstream on parse failure",
          "[core][l2][output][boundary]") {
  int_parser parser{};
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(4U);
  REQUIRE(writer.try_write("42").has_value());
  REQUIRE(writer.try_write("bad").has_value());

  auto parsed_reader = parser.parse_stream(std::move(reader));
  auto first = parsed_reader.try_read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(*first.value().value == 42);

  auto second = parsed_reader.try_read();
  REQUIRE(second.has_error());
  REQUIRE(second.error() == wh::core::errc::parse_error);
  REQUIRE(!parser.errors.empty());
  REQUIRE(parser.errors.back().code() == wh::core::errc::parse_error);

  auto write_after_error = writer.try_write("99");
  REQUIRE(write_after_error.has_error());
  REQUIRE(write_after_error.error() == wh::core::errc::channel_closed);
}

TEST_CASE("output parser parse_stream_view prefers view implementation",
          "[core][l2][output][condition]") {
  int_parser_view_preferred parser{};
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<std::string>(2U);
  REQUIRE(writer.try_write("11").has_value());
  REQUIRE(writer.close().has_value());

  auto parsed_reader = parser.parse_stream_view(std::move(reader));
  auto first = parsed_reader.try_read();
  REQUIRE(first.has_value());
  REQUIRE(first.value().value.has_value());
  REQUIRE(*first.value().value == 11);
  REQUIRE(parser.view_used);
}
