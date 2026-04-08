#include <catch2/catch_test_macros.hpp>

#include <array>
#include <atomic>
#include <string>

#include <stdexec/execution.hpp>

#include "helper/component_contract_support.hpp"
#include "helper/test_thread_wait.hpp"
#include "wh/document/document.hpp"
#include "wh/document/processor.hpp"
#include "wh/document/parser/ext_parser.hpp"

namespace {

using wh::testing::helper::document_async_available;
using wh::testing::helper::register_test_callbacks;

} // namespace

TEST_CASE(
    "document parser supports extension routing fallback and explicit missing parser",
    "[core][document][functional]") {
  wh::document::parser::ext_parser parser{};
  auto parsed = parser.parse(
      wh::document::parser::parse_request{"content",
                                          wh::document::parser::parse_options{
                                              "test.unknown",
                                              {{"source", "unit-test"}},
                                              {}}});
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().size() == 1U);
  REQUIRE(parsed.value().front().metadata_or<std::string>("_source", "") ==
          "test.unknown");

  wh::document::parser::ext_parser no_fallback{};
  no_fallback.clear_fallback();
  auto missing = no_fallback.parse(
      wh::document::parser::parse_request{"content",
                                          wh::document::parser::parse_options{
                                              "test.unknown", {}, {}}});
  REQUIRE(missing.has_error());
  REQUIRE(missing.error() == wh::core::errc::not_found);
}

TEST_CASE("document parser rvalue paths keep stable uri for routing and metadata",
          "[core][document][functional]") {
  class tagged_parser final {
  public:
    explicit tagged_parser(const std::string &tag) : tag_(tag) {}
    explicit tagged_parser(std::string &&tag) : tag_(std::move(tag)) {}

    [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
      return wh::core::component_descriptor{"TaggedParser",
                                            wh::core::component_kind::document};
    }

    [[nodiscard]] auto parse(
        const wh::document::parser::parse_request &request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document doc{request.content};
      doc.set_metadata("tag", tag_);
      return wh::document::document_batch{std::move(doc)};
    }

    [[nodiscard]] auto parse(wh::document::parser::parse_request &&request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document doc{std::move(request.content)};
      doc.set_metadata("tag", tag_);
      return wh::document::document_batch{std::move(doc)};
    }

    [[nodiscard]] auto parse(
        const wh::document::parser::parse_request_view request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document doc{std::string{request.content}};
      doc.set_metadata("tag", tag_);
      return wh::document::document_batch{std::move(doc)};
    }

  private:
    std::string tag_{};
  };

  wh::document::parser::ext_parser ext{};
  ext.set_fallback<tagged_parser>("fallback");
  auto registered = ext.register_parser<tagged_parser>(".md", "md");
  REQUIRE(registered.has_value());

  wh::document::parser::parse_request request{};
  request.content = "body";
  request.options.uri = "doc.md";
  auto routed = ext.parse(std::move(request));
  REQUIRE(routed.has_value());
  REQUIRE(routed.value().size() == 1U);
  REQUIRE(routed.value().front().metadata_or<std::string>("tag", "") == "md");

  wh::document::document component{wh::document::document_processor{}};
  wh::core::run_context callback_context{};
  auto parsed = component.process(
      wh::document::document_request{
          .source_kind = wh::document::document_source_kind::uri,
          .source = "in-memory://payload"},
      callback_context);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().size() == 1U);
  REQUIRE(parsed.value().front().metadata_or<std::string>("_source", "") ==
          "in-memory://payload");
}

TEST_CASE("document parser handle keeps value semantics for small and large impls",
          "[core][document][functional]") {
  class small_parser final {
  public:
    explicit small_parser(std::string tag) : tag_(std::move(tag)) {}

    [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
      return wh::core::component_descriptor{"SmallParser",
                                            wh::core::component_kind::document};
    }

    [[nodiscard]] auto parse(
        const wh::document::parser::parse_request &request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document doc{request.content};
      doc.set_metadata("tag", tag_);
      return wh::document::document_batch{std::move(doc)};
    }

    [[nodiscard]] auto parse(wh::document::parser::parse_request &&request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document doc{std::move(request.content)};
      doc.set_metadata("tag", tag_);
      return wh::document::document_batch{std::move(doc)};
    }

    [[nodiscard]] auto parse(
        const wh::document::parser::parse_request_view request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document doc{std::string{request.content}};
      doc.set_metadata("tag", tag_);
      return wh::document::document_batch{std::move(doc)};
    }

  private:
    std::string tag_{};
  };

  class large_parser final {
  public:
    explicit large_parser(std::string tag) : tag_(std::move(tag)) {
      padding_.fill(0x2A);
    }

    [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
      return wh::core::component_descriptor{"LargeParser",
                                            wh::core::component_kind::document};
    }

    [[nodiscard]] auto parse(
        const wh::document::parser::parse_request &request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document doc{request.content};
      doc.set_metadata("tag", tag_);
      doc.set_metadata("padding",
                       std::to_string(static_cast<int>(padding_.front())));
      return wh::document::document_batch{std::move(doc)};
    }

    [[nodiscard]] auto parse(wh::document::parser::parse_request &&request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document doc{std::move(request.content)};
      doc.set_metadata("tag", tag_);
      doc.set_metadata("padding",
                       std::to_string(static_cast<int>(padding_.front())));
      return wh::document::document_batch{std::move(doc)};
    }

    [[nodiscard]] auto parse(
        const wh::document::parser::parse_request_view request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document doc{std::string{request.content}};
      doc.set_metadata("tag", tag_);
      doc.set_metadata("padding",
                       std::to_string(static_cast<int>(padding_.front())));
      return wh::document::document_batch{std::move(doc)};
    }

  private:
    std::array<unsigned char, 128> padding_{};
    std::string tag_{};
  };

  auto parse_tag = [](const wh::document::parser::parser &parser_handle,
                      std::string_view expected_tag) {
    auto parsed = parser_handle.parse(
        wh::document::parser::parse_request{"body", {}});
    REQUIRE(parsed.has_value());
    REQUIRE(parsed.value().size() == 1U);
    REQUIRE(parsed.value().front().metadata_or<std::string>("tag", "") ==
            expected_tag);
  };

  wh::document::parser::parser small{small_parser{"small"}};
  wh::document::parser::parser small_copy = small;
  wh::document::parser::parser small_move = std::move(small_copy);
  parse_tag(small, "small");
  parse_tag(small_move, "small");

  wh::document::parser::parser large{large_parser{"large"}};
  wh::document::parser::parser large_copy = large;
  wh::document::parser::parser large_move = std::move(large_copy);
  parse_tag(large, "large");
  parse_tag(large_move, "large");
}

TEST_CASE("document processor parser callbacks and metadata injection stay consistent",
          "[core][document][functional]") {
  class passthrough_parser final {
  public:
    [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
      return wh::core::component_descriptor{
          "PassthroughParser", wh::core::component_kind::document};
    }

    [[nodiscard]] auto parse(
        const wh::document::parser::parse_request &request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document non_empty{request.content};
      non_empty.set_metadata("existing", std::string{"kept"});
      wh::schema::document empty{""};
      return wh::document::document_batch{std::move(non_empty), std::move(empty)};
    }

    [[nodiscard]] auto parse(wh::document::parser::parse_request &&request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document non_empty{std::move(request.content)};
      non_empty.set_metadata("existing", std::string{"kept"});
      wh::schema::document empty{""};
      return wh::document::document_batch{std::move(non_empty), std::move(empty)};
    }

    [[nodiscard]] auto parse(
        const wh::document::parser::parse_request_view request) const
        -> wh::core::result<wh::document::document_batch> {
      wh::schema::document non_empty{std::string{request.content}};
      non_empty.set_metadata("existing", std::string{"kept"});
      wh::schema::document empty{""};
      return wh::document::document_batch{std::move(non_empty), std::move(empty)};
    }
  };

  std::atomic<bool> parser_extra_called{false};
  std::atomic<bool> parser_payload_called{false};

  wh::document::loader_options options{};
  wh::document::loader_common_options common{};
  common.parser.uri = "override://uri";
  common.parser.extra_meta.insert_or_assign("tenant", "acme");
  options.set_base(common);

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context),
      [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage,
          const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        if (stage != wh::core::callback_stage::end) {
          return;
        }
        const auto *typed = event.get_if<wh::document::parser_callback_event>();
        if (typed == nullptr) {
          return;
        }
        REQUIRE(typed->uri == "override://uri");
        REQUIRE(typed->input_bytes == 4U);
        REQUIRE(typed->output_count == 2U);
        parser_extra_called.store(true, std::memory_order_release);
        parser_payload_called.store(true, std::memory_order_release);
      },
      "document-parser-events");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  wh::document::document_processor processor{passthrough_parser{}};
  processor.set_loader([](const std::string &, const wh::document::loader_options &)
                           -> wh::core::result<std::string> { return "body"; });
  wh::document::document component{std::move(processor)};
  auto parsed = component.process(
      wh::document::document_request{
          .source_kind = wh::document::document_source_kind::uri,
          .source = "input://uri",
          .options = options},
      callback_context);
  REQUIRE(parsed.has_value());
  REQUIRE(parsed.value().size() == 2U);
  REQUIRE(parsed.value()[0].metadata_or<std::string>("_source", "") ==
          "override://uri");
  REQUIRE(parsed.value()[0].metadata_or<std::string>("tenant", "") == "acme");
  REQUIRE(parsed.value()[0].metadata_or<std::string>("existing", "") == "kept");
  REQUIRE(parsed.value()[1].metadata() == nullptr);
  REQUIRE(parser_extra_called.load(std::memory_order_acquire));
  REQUIRE(parser_payload_called.load(std::memory_order_acquire));
}

TEST_CASE("document async path only exists for true async implementations",
          "[core][document][functional]") {
  struct sender_document_impl {
    [[nodiscard]] auto process_sender(wh::document::document_request request) const {
      wh::schema::document document{"async:" + request.source};
      return stdexec::just(wh::core::result<wh::document::document_batch>{
          wh::document::document_batch{std::move(document)}});
    }
  };

  static_assert(!document_async_available<
                wh::document::document<wh::document::document_processor>>);

  wh::document::document component{sender_document_impl{}};
  wh::document::document_request request{};
  request.source_kind = wh::document::document_source_kind::content;
  request.source = "payload";

  wh::core::run_context callback_context{};
  auto async_result = wh::testing::helper::wait_value_on_test_thread(
      component.async_process(std::move(request), callback_context));
  REQUIRE(async_result.has_value());
  REQUIRE(async_result.value().size() == 1U);
  REQUIRE(async_result.value().front().content() == "async:payload");
}

TEST_CASE("document loader callbacks report loaded bytes for uri sources",
          "[core][document][functional]") {
  std::atomic<bool> loader_extra_called{false};
  std::atomic<bool> loader_payload_called{false};
  wh::document::loader_options loader_options{};

  wh::core::run_context callback_context{};
  callback_context.callbacks.emplace();
  auto registered = register_test_callbacks(
      std::move(callback_context),
      [](const wh::core::callback_stage) noexcept { return true; },
      [&](const wh::core::callback_stage stage,
          const wh::core::callback_event_view event,
          const wh::core::callback_run_info &) {
        if (stage != wh::core::callback_stage::end) {
          return;
        }
        const auto *typed = event.get_if<wh::document::loader_callback_event>();
        if (typed == nullptr) {
          return;
        }
        REQUIRE(typed->loaded_bytes > 0U);
        loader_extra_called.store(true, std::memory_order_release);
        loader_payload_called.store(true, std::memory_order_release);
      },
      "document-loader-events");
  REQUIRE(registered.has_value());
  callback_context = std::move(registered).value();

  wh::document::document_processor processor{};
  processor.set_loader([](const std::string &uri, const wh::document::loader_options &)
                           -> wh::core::result<std::string> { return uri; });
  wh::document::document component{std::move(processor)};
  auto parsed = component.process(
      wh::document::document_request{
          .source_kind = wh::document::document_source_kind::uri,
          .source = "doc-content",
          .options = loader_options},
      callback_context);
  REQUIRE(parsed.has_value());
  REQUIRE(loader_extra_called.load(std::memory_order_acquire));
  REQUIRE(loader_payload_called.load(std::memory_order_acquire));
}
