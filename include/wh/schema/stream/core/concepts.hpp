// Defines the common stream reader concepts shared by stream adapters,
// merge utilities, and copy/fork helpers.
#pragma once

#include <concepts>

#include <stdexec/execution.hpp>

namespace wh::schema::stream {

/// Reader concept used by stream combinators and adapters.
template <typename reader_t>
concept stream_reader = requires(reader_t reader) {
  typename reader_t::value_type;
  reader.read();
  reader.try_read();
  reader.close();
  reader.is_closed();
};

/// Reader concept for readers that also expose borrowed-view reads.
template <typename reader_t>
concept borrowed_stream_reader =
    stream_reader<reader_t> && requires(reader_t reader) {
      reader.read_borrowed();
      reader.try_read_borrowed();
    };

namespace detail {

/// Reader concept for readers that also expose sender-based async reads.
template <typename reader_t>
concept async_stream_reader =
    stream_reader<reader_t> && requires(reader_t &reader) {
      { reader.read_async() } -> stdexec::sender;
    };

} // namespace detail

} // namespace wh::schema::stream
