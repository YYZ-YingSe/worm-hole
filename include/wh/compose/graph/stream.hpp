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
[[nodiscard]] inline auto copy_graph_readers(graph_stream_reader &&reader, const std::size_t count)
    -> wh::core::result<std::vector<graph_stream_reader>> {
  if (count == 0U) {
    return std::vector<graph_stream_reader>{};
  }

  auto copied = wh::schema::stream::make_copy_stream_readers(std::move(reader), count);
  std::vector<graph_stream_reader> wrapped{};
  wrapped.reserve(copied.size());
  for (auto &entry : copied) {
    wrapped.emplace_back(graph_stream_reader{std::move(entry)});
  }
  return wrapped;
}

/// Merges named graph readers into one graph reader.
[[nodiscard]] inline auto make_graph_merge_reader(
    std::vector<wh::schema::stream::named_stream_reader<graph_stream_reader>> readers)
    -> graph_stream_reader {
  return graph_stream_reader{wh::schema::stream::make_merge_stream_reader(std::move(readers))};
}

/// Builds one live merged graph reader shell from source keys.
[[nodiscard]] inline auto make_graph_merge_reader(std::vector<std::string> sources)
    -> graph_stream_reader {
  return graph_stream_reader{
      wh::schema::stream::make_merge_stream_reader<graph_stream_reader>(std::move(sources))};
}

/// Forks one declared stream payload into two readers, mutating `payload` in
/// place.
[[nodiscard]] inline auto fork_graph_reader_payload(graph_value &payload)
    -> wh::core::result<graph_value> {
  auto *reader = wh::core::any_cast<graph_stream_reader>(&payload);
  if (reader == nullptr) {
    return wh::core::result<graph_value>::failure(wh::core::errc::type_mismatch);
  }

  auto copied_readers = copy_graph_readers(std::move(*reader), 2U);
  if (copied_readers.has_error()) {
    return wh::core::result<graph_value>::failure(copied_readers.error());
  }

  auto readers = std::move(copied_readers).value();
  if (readers.size() != 2U) {
    return wh::core::result<graph_value>::failure(wh::core::errc::type_mismatch);
  }

  payload = wh::core::any(std::move(readers[0]));
  return wh::core::any(std::move(readers[1]));
}

[[nodiscard]] inline auto is_reader_value_payload(const graph_value &value) noexcept -> bool {
  return wh::core::any_cast<graph_stream_reader>(&value) != nullptr;
}

[[nodiscard]] inline auto validate_value_boundary_payload(const graph_value &value)
    -> wh::core::result<void> {
  if (is_reader_value_payload(value)) {
    return wh::core::result<void>::failure(wh::core::errc::contract_violation);
  }
  return {};
}

[[nodiscard]] inline auto validate_copyable_value_payload(const graph_value &value)
    -> wh::core::result<void> {
  if (value.copyable()) {
    return {};
  }
  return wh::core::result<void>::failure(wh::core::errc::contract_violation);
}

[[nodiscard]] inline auto validate_value_contract_payload(const graph_value &value)
    -> wh::core::result<void> {
  auto boundary = validate_value_boundary_payload(value);
  if (boundary.has_error()) {
    return boundary;
  }
  return validate_copyable_value_payload(value);
}

[[nodiscard]] inline auto materialize_value_payload(const graph_value &value)
    -> wh::core::result<graph_value> {
  auto boundary = validate_value_boundary_payload(value);
  if (boundary.has_error()) {
    return wh::core::result<graph_value>::failure(boundary.error());
  }
  auto owned = wh::core::into_owned(value);
  if (owned.has_error()) {
    return wh::core::result<graph_value>::failure(owned.error());
  }
  return std::move(owned).value();
}

[[nodiscard]] inline auto materialize_value_payload(graph_value &&value)
    -> wh::core::result<graph_value> {
  auto boundary = validate_value_boundary_payload(value);
  if (boundary.has_error()) {
    return wh::core::result<graph_value>::failure(boundary.error());
  }
  auto owned = wh::core::into_owned(std::move(value));
  if (owned.has_error()) {
    return wh::core::result<graph_value>::failure(owned.error());
  }
  return std::move(owned).value();
}

} // namespace detail

/// Creates one graph stream writer/reader pair.
[[nodiscard]] inline auto make_graph_stream(const std::size_t capacity = 16U)
    -> std::pair<graph_stream_writer, graph_stream_reader> {
  auto [writer, reader] = wh::schema::stream::make_pipe_stream<graph_value>(capacity);
  return {graph_stream_writer{std::move(writer)}, graph_stream_reader{std::move(reader)}};
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
      wh::schema::stream::make_single_value_stream_reader<graph_value>(std::move(payload))};
}

/// Collects one graph stream into owned graph values.
[[nodiscard]] inline auto collect_graph_stream_reader(graph_stream_reader &&reader)
    -> wh::core::result<std::vector<graph_value>> {
  return wh::schema::stream::collect_stream_reader(std::move(reader));
}

/// Creates one graph stream from copied graph values.
[[nodiscard]] inline auto make_values_stream_reader(const std::vector<graph_value> &values)
    -> wh::core::result<graph_stream_reader> {
  return graph_stream_reader{wh::schema::stream::make_values_stream_reader(values)};
}

/// Creates one graph stream from movable graph values.
[[nodiscard]] inline auto make_values_stream_reader(std::vector<graph_value> &&values)
    -> wh::core::result<graph_stream_reader> {
  return graph_stream_reader{wh::schema::stream::make_values_stream_reader(std::move(values))};
}

/// Canonicalizes one compose reader into the public graph reader boundary.
[[nodiscard]] inline auto to_graph_stream_reader(graph_stream_reader reader)
    -> wh::core::result<graph_stream_reader> {
  return reader;
}

template <typename reader_t>
  requires(!std::same_as<std::remove_cvref_t<reader_t>, graph_stream_reader>) &&
          wh::schema::stream::stream_reader<std::remove_cvref_t<reader_t>> &&
          std::same_as<typename std::remove_cvref_t<reader_t>::value_type, graph_value>
[[nodiscard]] inline auto to_graph_stream_reader(reader_t &&reader)
    -> wh::core::result<graph_stream_reader> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  return graph_stream_reader{stored_reader_t{std::forward<reader_t>(reader)}};
}

template <typename reader_t>
  requires(!std::same_as<std::remove_cvref_t<reader_t>, graph_stream_reader>) &&
          wh::schema::stream::stream_reader<std::remove_cvref_t<reader_t>> &&
          (!std::same_as<typename std::remove_cvref_t<reader_t>::value_type, graph_value>) &&
          std::constructible_from<graph_value,
                                  const typename std::remove_cvref_t<reader_t>::value_type &>
[[nodiscard]] inline auto to_graph_stream_reader(reader_t &&reader)
    -> wh::core::result<graph_stream_reader> {
  using stored_reader_t = std::remove_cvref_t<reader_t>;
  auto mapped = wh::schema::stream::make_transform_stream_reader(
      stored_reader_t{std::forward<reader_t>(reader)},
      [](const typename stored_reader_t::value_type &value) -> wh::core::result<graph_value> {
        return graph_value{value};
      });
  return graph_stream_reader{std::move(mapped)};
}

template <typename status_t>
concept graph_stream_status = wh::core::detail::result_like<std::remove_cvref_t<status_t>> &&
                              requires(typename std::remove_cvref_t<status_t>::value_type value) {
                                {
                                  to_graph_stream_reader(std::move(value))
                                } -> std::same_as<wh::core::result<graph_stream_reader>>;
                              };

/// Forks one declared value payload into an execution-safe sibling payload.
///
/// This helper is intentionally value-only: it does not reinterpret a stored
/// `graph_stream_reader` as stream contract. Stream payload duplication must go
/// through explicit stream helpers on declared stream boundaries.
[[nodiscard]] inline auto fork_graph_value(graph_value &value) -> wh::core::result<graph_value> {
  return detail::materialize_value_payload(value);
}

} // namespace wh::compose
