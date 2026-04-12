#include <catch2/catch_test_macros.hpp>

#include <tuple>
#include <vector>
#include <variant>

#include <stdexec/execution.hpp>

#include "wh/compose/graph/detail/rewrite_policy.hpp"

TEST_CASE("rewrite policy rewrites stream chunks and closes the writer on terminal eof",
          "[UT][wh/compose/graph/detail/rewrite_policy.hpp][rewrite_policy::handle_completion][condition][branch][boundary]") {
  std::vector<wh::compose::graph_value> values{};
  values.emplace_back(1);
  auto source = wh::compose::make_values_stream_reader(values);
  REQUIRE(source.has_value());
  auto [writer, rewritten] = wh::compose::make_graph_stream();

  wh::compose::detail::rewrite_policy handler_policy{
      .reader = std::move(source).value(),
      .writer = std::move(writer),
      .handler =
          [](wh::compose::graph_value &value) -> wh::core::result<void> {
        auto *typed = wh::core::any_cast<int>(&value);
        REQUIRE(typed != nullptr);
        *typed += 5;
        return {};
      },
  };

  auto step = handler_policy.next_step();
  REQUIRE(step.has_value());
  REQUIRE(step->sender.has_value());

  auto first = handler_policy.reader.read();
  REQUIRE(first.has_value());
  auto first_status = handler_policy.handle_completion(std::move(first).value());
  REQUIRE_FALSE(first_status.has_value());

  auto rewritten_value = rewritten.read();
  REQUIRE(rewritten_value.has_value());
  REQUIRE(*wh::core::any_cast<int>(&rewritten_value->value.value()) == 6);

  auto eof = handler_policy.reader.read();
  REQUIRE(eof.has_value());
  auto finished = handler_policy.handle_completion(std::move(eof).value());
  REQUIRE(finished.has_value());
  REQUIRE(finished->has_value());
  REQUIRE(wh::core::any_cast<std::monostate>(&finished->value()) != nullptr);
}

TEST_CASE("rewrite policy sender propagates handler failures",
          "[UT][wh/compose/graph/detail/rewrite_policy.hpp][make_rewrite_stream_sender][branch][error]") {
  auto source = wh::compose::make_single_value_stream_reader(3);
  REQUIRE(source.has_value());
  auto [writer, rewritten] = wh::compose::make_graph_stream();

  auto sender = wh::compose::detail::make_rewrite_stream_sender(
      std::move(source).value(), std::move(writer),
      [](wh::compose::graph_value &) -> wh::core::result<void> {
        return wh::core::result<void>::failure(
            wh::core::errc::invalid_argument);
      },
      wh::core::detail::erase_resume_scheduler(stdexec::inline_scheduler{}));
  auto waited = stdexec::sync_wait(std::move(sender));
  REQUIRE(waited.has_value());
  REQUIRE(std::get<0>(*waited).has_error());
  REQUIRE(std::get<0>(*waited).error() == wh::core::errc::invalid_argument);

  auto downstream = rewritten.try_read();
  REQUIRE(std::holds_alternative<wh::schema::stream::stream_signal>(downstream));
  REQUIRE(std::get<wh::schema::stream::stream_signal>(downstream) ==
          wh::schema::stream::stream_pending);
  REQUIRE(rewritten.close().has_value());
}
