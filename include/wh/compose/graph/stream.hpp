// Defines graph-value stream aliases and graph-specific stream factories.
#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/compose/types.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/type_traits.hpp"

namespace wh::compose {

namespace detail {

/// Copies one graph reader into fixed-count readers.
[[nodiscard]] inline auto copy_graph_readers(
    graph_stream_reader &&reader, const std::size_t count)
    -> wh::core::result<std::vector<graph_stream_reader>> {
  if (count == 0U) {
    return std::vector<graph_stream_reader>{};
  }

  auto copied =
      wh::schema::stream::make_copy_stream_readers(std::move(reader), count);
  std::vector<graph_stream_reader> wrapped{};
  wrapped.reserve(copied.size());
  for (auto &entry : copied) {
    wrapped.emplace_back(graph_stream_reader{std::move(entry)});
  }
  return wrapped;
}

/// Merges named graph readers into one graph reader.
[[nodiscard]] inline auto make_graph_merge_reader(
    std::vector<wh::schema::stream::named_stream_reader<graph_stream_reader>>
        readers) -> graph_stream_reader {
  return graph_stream_reader{
      wh::schema::stream::make_merge_stream_reader(std::move(readers))};
}

/// Builds one live merged graph reader shell from source keys.
[[nodiscard]] inline auto make_graph_merge_reader(
    std::vector<std::string> sources) -> graph_stream_reader {
  return graph_stream_reader{
      wh::schema::stream::make_merge_stream_reader<graph_stream_reader>(
          std::move(sources))};
}

} // namespace detail

/// Creates one graph stream writer/reader pair.
[[nodiscard]] inline auto make_graph_stream(const std::size_t capacity = 16U)
    -> std::pair<graph_stream_writer, graph_stream_reader> {
  auto [writer, reader] =
      wh::schema::stream::make_pipe_stream<graph_value>(capacity);
  return {graph_stream_writer{std::move(writer)},
          graph_stream_reader{std::move(reader)}};
}

template <typename value_t>
  requires std::constructible_from<graph_value, value_t &&>
/// Creates one graph stream containing exactly one graph payload.
[[nodiscard]] inline auto make_single_value_stream_reader(value_t &&value)
    -> wh::core::result<graph_stream_reader> {
  graph_value payload{};
  if constexpr (std::same_as<std::remove_cvref_t<value_t>, graph_value>) {
    payload = std::forward<value_t>(value);
  } else {
    payload = graph_value{std::forward<value_t>(value)};
  }
  return graph_stream_reader{
      wh::schema::stream::make_single_value_stream_reader<graph_value>(
          std::move(payload))};
}

/// Collects one graph stream into owned graph values.
[[nodiscard]] inline auto collect_graph_stream_reader(graph_stream_reader &&reader)
    -> wh::core::result<std::vector<graph_value>> {
  return wh::schema::stream::collect_stream_reader(std::move(reader));
}

/// Creates one graph stream from copied graph values.
[[nodiscard]] inline auto
make_values_stream_reader(const std::vector<graph_value> &values)
    -> wh::core::result<graph_stream_reader> {
  return graph_stream_reader{wh::schema::stream::make_values_stream_reader(values)};
}

/// Creates one graph stream from movable graph values.
[[nodiscard]] inline auto make_values_stream_reader(std::vector<graph_value> &&values)
    -> wh::core::result<graph_stream_reader> {
  return graph_stream_reader{
      wh::schema::stream::make_values_stream_reader(std::move(values))};
}

/// Canonicalizes one compose reader into the public graph reader boundary.
[[nodiscard]] inline auto to_graph_stream_reader(graph_stream_reader reader)
    -> wh::core::result<graph_stream_reader> {
  return reader;
}

template <typename reader_t>
  requires (!std::same_as<std::remove_cvref_t<reader_t>, graph_stream_reader>) &&
           wh::schema::stream::stream_reader<std::remove_cvref_t<reader_t>> &&
           std::same_as<typename std::remove_cvref_t<reader_t>::value_type,
                        graph_value>
[[nodiscard]] inline auto to_graph_stream_reader(reader_t &&reader)
    -> wh::core::result<graph_stream_reader> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  return graph_stream_reader{stored_reader_t{std::forward<reader_t>(reader)}};
}

template <typename reader_t>
  requires (!std::same_as<std::remove_cvref_t<reader_t>, graph_stream_reader>) &&
           wh::schema::stream::stream_reader<std::remove_cvref_t<reader_t>> &&
           (!std::same_as<typename std::remove_cvref_t<reader_t>::value_type,
                          graph_value>) &&
           std::constructible_from<
               graph_value,
               const typename std::remove_cvref_t<reader_t>::value_type &>
[[nodiscard]] inline auto to_graph_stream_reader(reader_t &&reader)
    -> wh::core::result<graph_stream_reader> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  auto mapped = wh::schema::stream::make_transform_stream_reader(
      stored_reader_t{std::forward<reader_t>(reader)},
      [](const typename stored_reader_t::value_type &value)
          -> wh::core::result<graph_value> { return graph_value{value}; });
  return graph_stream_reader{std::move(mapped)};
}

template <typename status_t>
concept graph_stream_status =
    wh::core::result_like<std::remove_cvref_t<status_t>> &&
    requires(typename std::remove_cvref_t<status_t>::value_type value) {
      { to_graph_stream_reader(std::move(value)) }
      -> std::same_as<wh::core::result<graph_stream_reader>>;
    };

/// Converts one payload to a graph reader, preserving movable reader payloads.
[[nodiscard]] inline auto payload_to_reader(graph_value &&payload)
    -> wh::core::result<graph_stream_reader> {
  if (auto *reader = wh::core::any_cast<graph_stream_reader>(&payload);
      reader != nullptr) {
    return std::move(*reader);
  }
  return make_single_value_stream_reader(std::move(payload));
}

/// Forks one retained graph payload into an execution-safe sibling payload.
[[nodiscard]] inline auto fork_graph_value(graph_value &value)
    -> wh::core::result<graph_value> {
  if (auto *reader = wh::core::any_cast<graph_stream_reader>(&value);
      reader != nullptr) {
    auto copies = detail::copy_graph_readers(std::move(*reader), 2U);
    if (copies.has_error()) {
      return wh::core::result<graph_value>::failure(copies.error());
    }
    auto readers = std::move(copies).value();
    value = wh::core::any(std::move(readers[0]));
    return wh::core::any(std::move(readers[1]));
  }
  if (!value.copyable()) {
    return wh::core::result<graph_value>::failure(wh::core::errc::not_supported);
  }
  return graph_value{value};
}

} // namespace wh::compose
