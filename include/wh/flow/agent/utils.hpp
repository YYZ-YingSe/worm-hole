// Defines shared message reader/result bridge helpers used by flow-level agent
// facades.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/model/chat_model.hpp"
#include "wh/schema/message.hpp"
#include "wh/schema/stream/algorithm.hpp"
#include "wh/schema/stream/reader.hpp"

namespace wh::flow::agent {

/// One externally visible item yielded by a flow message reader.
struct message_reader_item {
  /// Message payload emitted by the flow, when any.
  std::optional<wh::schema::message> message{};
  /// Error surfaced as one visible item instead of a silent EOF.
  std::optional<wh::core::error_code> error{};
};

/// Reader boundary exposed by flow-level streaming APIs.
using message_reader =
    wh::schema::stream::any_stream_reader<message_reader_item>;

/// Single-message terminal result exposed by flow-level value APIs.
using message_result = wh::core::result<wh::schema::message>;

/// Renders the plain-text projection of one message.
[[nodiscard]] inline auto render_message_text(const wh::schema::message &message)
    -> std::string {
  std::string text{};
  for (const auto &part : message.parts) {
    if (const auto *typed = std::get_if<wh::schema::text_part>(&part);
        typed != nullptr) {
      text.append(typed->text);
    }
  }
  return text;
}

/// Builds one reader from explicitly materialized reader items.
[[nodiscard]] inline auto make_message_reader(
    std::vector<message_reader_item> items)
    -> wh::core::result<message_reader> {
  return message_reader{
      wh::schema::stream::make_values_stream_reader(std::move(items))};
}

/// Builds one reader from one terminal message result.
[[nodiscard]] inline auto message_result_to_reader(message_result status)
    -> wh::core::result<message_reader> {
  std::vector<message_reader_item> items{};
  if (status.has_error()) {
    items.push_back(message_reader_item{.error = status.error()});
  } else {
    items.push_back(message_reader_item{.message = std::move(status).value()});
  }
  return make_message_reader(std::move(items));
}

/// Consumes one model message stream and materializes all emitted values plus
/// the terminal error item, when any.
[[nodiscard]] inline auto message_stream_to_reader(
    wh::model::chat_message_stream_reader reader)
    -> wh::core::result<message_reader> {
  std::vector<message_reader_item> items{};
  while (true) {
    auto next = reader.read();
    if (next.has_error()) {
      items.push_back(message_reader_item{.error = next.error()});
      break;
    }
    if (next.value().error.failed()) {
      items.push_back(message_reader_item{.error = next.value().error});
      break;
    }
    if (next.value().eof) {
      break;
    }
    if (next.value().value.has_value()) {
      items.push_back(message_reader_item{.message = std::move(*next.value().value)});
    }
  }
  return make_message_reader(std::move(items));
}

/// Collects one flow message reader into owned reader items.
[[nodiscard]] inline auto collect_message_reader_items(message_reader reader)
    -> wh::core::result<std::vector<message_reader_item>> {
  return wh::schema::stream::collect_stream_reader(std::move(reader));
}

/// Reduces one flow message reader to the last visible message or the first
/// surfaced error item.
[[nodiscard]] inline auto message_reader_to_result(message_reader reader)
    -> message_result {
  auto items = collect_message_reader_items(std::move(reader));
  if (items.has_error()) {
    return message_result::failure(items.error());
  }

  std::optional<wh::schema::message> last_message{};
  for (auto &item : items.value()) {
    if (item.error.has_value()) {
      return message_result::failure(*item.error);
    }
    if (item.message.has_value()) {
      last_message = std::move(item.message);
    }
  }
  if (!last_message.has_value()) {
    return message_result::failure(wh::core::errc::not_found);
  }
  return std::move(*last_message);
}

/// Reduces one model message stream to the last visible message or the first
/// surfaced error item.
[[nodiscard]] inline auto message_stream_to_result(
    wh::model::chat_message_stream_reader reader) -> message_result {
  auto bridged = message_stream_to_reader(std::move(reader));
  if (bridged.has_error()) {
    return message_result::failure(bridged.error());
  }
  return message_reader_to_result(std::move(bridged).value());
}

} // namespace wh::flow::agent
