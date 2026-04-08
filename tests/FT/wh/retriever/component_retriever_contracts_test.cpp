#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "helper/component_contract_support.hpp"
#include "wh/retriever/retriever.hpp"

namespace {

using wh::testing::helper::register_test_callbacks;
using wh::testing::helper::sync_retriever_impl;

} // namespace

TEST_CASE("retriever callbacks bridge typed and payload paths",
          "[core][retriever][functional]") {
  std::atomic<bool> started{false};
  std::atomic<bool> ended{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> payload_seen{false};

  wh::retriever::retriever_options options{};
  wh::retriever::retriever_common_options common{};
  common.top_k = 1U;
  common.fail_fast_on_route_error = false;
  options.set_base(common);

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context),
      [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage,
          const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::retriever::retriever_callback_event>();
        REQUIRE(typed != nullptr);
        if (stage == wh::core::callback_stage::start) {
          REQUIRE(typed->extra == "query");
          started.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          REQUIRE(typed->extra == "1");
          ended.store(true, std::memory_order_release);
          payload_seen.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          failed.store(true, std::memory_order_release);
        }
      },
      "retriever-events");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  wh::retriever::retriever component{sync_retriever_impl{
      [](const wh::retriever::retriever_request &)
          -> wh::core::result<wh::retriever::retriever_response> {
        return wh::retriever::retriever_response{wh::schema::document{"ok"}};
      }}};
  auto status = component.retrieve({"query", "idx", "", {}, options},
                                   callback_context);
  REQUIRE(status.has_value());
  REQUIRE(status.value().size() == 1U);
  REQUIRE(started.load(std::memory_order_acquire));
  REQUIRE(ended.load(std::memory_order_acquire));
  REQUIRE_FALSE(failed.load(std::memory_order_acquire));
  REQUIRE(payload_seen.load(std::memory_order_acquire));
}

TEST_CASE("retriever applies score threshold filter dsl and sub-index constraints",
          "[core][retriever][functional]") {
  wh::retriever::retriever component{sync_retriever_impl{
      [](const wh::retriever::retriever_request &)
          -> wh::core::result<wh::retriever::retriever_response> {
        wh::schema::document first{"keep-a"};
        first.with_score(0.91).with_dsl("dsl-a").with_sub_index("s1");

        wh::schema::document second{"drop-score"};
        second.with_score(0.20).with_dsl("dsl-a").with_sub_index("s1");

        wh::schema::document third{"drop-dsl"};
        third.with_score(0.95).with_dsl("dsl-b").with_sub_index("s1");

        wh::schema::document fourth{"keep-b"};
        fourth.with_score(0.99).with_dsl("dsl-a").with_sub_index("s1");

        wh::schema::document fifth{"drop-sub-index"};
        fifth.with_score(0.99).with_dsl("dsl-a").with_sub_index("s2");

        return wh::retriever::retriever_response{
            std::move(first), std::move(second), std::move(third),
            std::move(fourth), std::move(fifth)};
      }}};

  wh::retriever::retriever_options options{};
  wh::retriever::retriever_common_options common{};
  common.top_k = 2U;
  common.score_threshold = 0.9;
  common.filter = "content=keep-a";
  common.dsl = "dsl-a";
  options.set_base(common);
  wh::core::run_context callback_context{};

  auto status =
      component.retrieve({"q", "idx", "s1", {}, options}, callback_context);
  REQUIRE(status.has_value());
  REQUIRE(status.value().size() == 1U);
  REQUIRE(status.value().front().content() == "keep-a");

  common.filter = "keep";
  common.top_k = 1U;
  options.set_base(common);
  auto relaxed =
      component.retrieve({"q", "idx", "s1", {}, options}, callback_context);
  REQUIRE(relaxed.has_value());
  REQUIRE(relaxed.value().size() == 1U);
}

TEST_CASE("retriever strategy policies dedupe by content before top-k cut",
          "[core][retriever][functional]") {
  wh::retriever::retriever_options retriever_options{};
  wh::retriever::retriever_common_options retriever_common{};
  retriever_common.top_k = 2U;
  retriever_common.merge_policy =
      wh::retriever::recall_merge_policy::dedupe_by_content;
  retriever_options.set_base(retriever_common);

  wh::retriever::retriever merged_retriever{sync_retriever_impl{
      [](const wh::retriever::retriever_request &)
          -> wh::core::result<wh::retriever::retriever_response> {
        return wh::retriever::retriever_response{wh::schema::document{"a"},
                                                 wh::schema::document{"b"},
                                                 wh::schema::document{"a"},
                                                 wh::schema::document{"c"}};
      }}};
  wh::core::run_context retriever_context{};
  auto merged = merged_retriever.retrieve({"q", "idx", "", {}, retriever_options},
                                          retriever_context);
  REQUIRE(merged.has_value());
  REQUIRE(merged.value().size() == 2U);
  REQUIRE(merged.value()[0].content() == "a");
  REQUIRE(merged.value()[1].content() == "b");
}
