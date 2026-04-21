#include <catch2/catch_test_macros.hpp>

#include <atomic>

#include "helper/component_contract_support.hpp"
#include "wh/embedding/embedding.hpp"

namespace {

using wh::testing::helper::register_test_callbacks;
using wh::testing::helper::sync_embedding_impl;

} // namespace

TEST_CASE("embedding callbacks keep partial-success batches on end path",
          "[core][embedding][functional]") {
  std::atomic<bool> started{false};
  std::atomic<bool> ended{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> payload_seen{false};

  wh::embedding::embedding_options options{};
  wh::embedding::embedding_common_options common{};
  common.model_id = "embed";
  common.failure_policy = wh::embedding::batch_failure_policy::partial_success;
  options.set_base(common);

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context),
      [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage,
          const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::embedding::embedding_callback_event>();
        REQUIRE(typed != nullptr);
        if (stage == wh::core::callback_stage::start) {
          REQUIRE(typed->batch_size == 2U);
          REQUIRE(typed->usage.prompt_tokens == 2);
          REQUIRE(typed->usage.total_tokens == 2);
          started.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          REQUIRE(typed->usage.prompt_tokens == 2);
          REQUIRE(typed->usage.total_tokens >= 2);
          ended.store(true, std::memory_order_release);
          payload_seen.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          failed.store(true, std::memory_order_release);
        }
      },
      "embedding-events");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  wh::embedding::embedding component{sync_embedding_impl{
      [](const wh::embedding::embedding_request &request)
          -> wh::core::result<wh::embedding::embedding_response> {
        wh::embedding::embedding_response output{};
        output.reserve(request.inputs.size());
        const auto request_options = request.options.resolve_view();
        for (const auto &input : request.inputs) {
          if (input == "bad") {
            if (request_options.failure_policy ==
                wh::embedding::batch_failure_policy::fail_fast) {
              return wh::core::result<wh::embedding::embedding_response>::failure(
                  wh::core::errc::invalid_argument);
            }
            output.emplace_back();
            continue;
          }
          output.push_back({1.0});
        }
        return output;
      }}};
  auto status = component.embed({{"ok", "bad"}, options}, callback_context);
  REQUIRE(status.has_value());
  REQUIRE(status.value().size() == 2U);
  REQUIRE(status.value()[1].empty());
  REQUIRE(started.load(std::memory_order_acquire));
  REQUIRE(ended.load(std::memory_order_acquire));
  REQUIRE_FALSE(failed.load(std::memory_order_acquire));
  REQUIRE(payload_seen.load(std::memory_order_acquire));
}

TEST_CASE("embedding batch function path reports usage in callbacks",
          "[core][embedding][functional]") {
  std::atomic<bool> ended{false};

  wh::embedding::embedding_options options{};
  wh::embedding::embedding_common_options common{};
  common.model_id = "batch-embed";
  options.set_base(common);

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context),
      [](const wh::core::callback_stage stage) noexcept {
        return stage == wh::core::callback_stage::end;
      },
      [&](const wh::core::callback_stage,
          const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::embedding::embedding_callback_event>();
        if (typed == nullptr) {
          return;
        }
        REQUIRE(typed->model_id == "batch-embed");
        REQUIRE(typed->batch_size == 3U);
        REQUIRE(typed->usage.prompt_tokens == 3);
        REQUIRE(typed->usage.completion_tokens == 3);
        REQUIRE(typed->usage.total_tokens == 6);
        ended.store(true, std::memory_order_release);
      },
      "embedding-batch-end");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  wh::embedding::embedding component{sync_embedding_impl{
      [](const wh::embedding::embedding_request &request)
          -> wh::core::result<wh::embedding::embedding_response> {
        wh::embedding::embedding_response output{};
        output.reserve(request.inputs.size());
        for (const auto &input : request.inputs) {
          output.push_back({static_cast<double>(input.size())});
        }
        return output;
      }}};
  auto status = component.embed({{"a", "bb", "ccc"}, options}, callback_context);
  REQUIRE(status.has_value());
  REQUIRE(status.value().size() == 3U);
  REQUIRE(ended.load(std::memory_order_acquire));
}
