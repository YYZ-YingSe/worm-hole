#include <atomic>

#include <catch2/catch_test_macros.hpp>

#include "helper/component_contract_support.hpp"
#include "wh/indexer/indexer.hpp"

namespace {

using wh::testing::helper::register_test_callbacks;
using wh::testing::helper::sync_indexer_single_impl;

} // namespace

TEST_CASE("indexer callbacks and embedding-combined config stay consistent",
          "[core][indexer][functional]") {
  std::atomic<bool> started{false};
  std::atomic<bool> ended{false};
  std::atomic<bool> failed{false};
  std::atomic<bool> payload_seen{false};

  wh::indexer::indexer_options options{};
  wh::indexer::indexer_common_options common{};
  common.failure_policy = wh::indexer::write_failure_policy::skip;
  common.combine_with_embedding = true;
  common.embedding_model = "embed-model";
  options.set_base(common);

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context), [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage, const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        const auto *typed = event.get_if<wh::indexer::indexer_callback_event>();
        REQUIRE(typed != nullptr);
        if (stage == wh::core::callback_stage::start) {
          REQUIRE(typed->batch_size >= 1U);
          started.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::end) {
          REQUIRE(typed->success_count == 1U);
          REQUIRE(typed->failure_count == 1U);
          ended.store(true, std::memory_order_release);
          payload_seen.store(true, std::memory_order_release);
          return;
        }
        if (stage == wh::core::callback_stage::error) {
          failed.store(true, std::memory_order_release);
        }
      },
      "indexer-events");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  wh::indexer::indexer component{sync_indexer_single_impl{
      [](const wh::schema::document &doc,
         const wh::indexer::indexer_options &) -> wh::core::result<std::string> {
        if (doc.content() == "bad") {
          return wh::core::result<std::string>::failure(wh::core::errc::network_error);
        }
        return std::string{"id-" + doc.content()};
      }}};

  auto missing_embedding =
      component.write({{wh::schema::document{"ok"}}, {}, options}, callback_context);
  REQUIRE(missing_embedding.has_error());
  REQUIRE(missing_embedding.error() == wh::core::errc::invalid_argument);

  auto status = component.write(
      {{wh::schema::document{"ok"}, wh::schema::document{"bad"}}, {0.5, 0.25}, options},
      callback_context);
  REQUIRE(status.has_value());
  REQUIRE(status.value().success_count == 1U);
  REQUIRE(status.value().failure_count == 1U);
  REQUIRE(started.load(std::memory_order_acquire));
  REQUIRE(ended.load(std::memory_order_acquire));
  REQUIRE(failed.load(std::memory_order_acquire));
  REQUIRE(payload_seen.load(std::memory_order_acquire));
}

TEST_CASE("indexer skip policy keeps successes and reports failures",
          "[core][indexer][functional]") {
  wh::indexer::indexer_options options{};
  wh::indexer::indexer_common_options common{};
  common.failure_policy = wh::indexer::write_failure_policy::skip;
  common.max_retries = 1U;
  options.set_base(common);

  wh::indexer::indexer component{sync_indexer_single_impl{
      [](const wh::schema::document &doc,
         const wh::indexer::indexer_options &) -> wh::core::result<std::string> {
        if (doc.content() == "bad") {
          return wh::core::result<std::string>::failure(wh::core::errc::network_error);
        }
        return std::string{"id-" + doc.content()};
      }}};

  wh::core::run_context context{};
  auto indexed = component.write(
      {{wh::schema::document{"ok"}, wh::schema::document{"bad"}}, {}, options}, context);
  REQUIRE(indexed.has_value());
  REQUIRE(indexed.value().success_count == 1U);
  REQUIRE(indexed.value().failure_count == 1U);
  REQUIRE(indexed.value().document_ids.size() == 1U);
  REQUIRE(indexed.value().document_ids.front() == "id-ok");
}
