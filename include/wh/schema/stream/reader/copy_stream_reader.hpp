// Defines retained fixed-cardinality fan-out readers.
#pragma once

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/core/cursor_reader.hpp"
#include "wh/core/type_traits.hpp"
#include "wh/schema/stream/adapter/detail/adapter_support.hpp"
#include "wh/schema/stream/core/concepts.hpp"
#include "wh/schema/stream/core/stream_base.hpp"

namespace wh::schema::stream {

namespace detail {

template <typename reader_t> struct copy_stream_policy {
  using result_type =
      std::remove_cvref_t<decltype(std::declval<reader_t &>().read())>;
  using source_try_type =
      std::remove_cvref_t<decltype(std::declval<reader_t &>().try_read())>;
  using try_result_type = source_try_type;

  [[nodiscard]] static auto is_terminal(const result_type &status) noexcept
      -> bool {
    if (status.has_error()) {
      return true;
    }
    return status.value().eof || status.value().error.failed();
  }

  [[nodiscard]] static auto is_pending(const source_try_type &status) noexcept
      -> bool {
    return std::holds_alternative<stream_signal>(status);
  }

  [[nodiscard]] static auto project_try(source_try_type status) noexcept
      -> result_type {
    return std::move(std::get<result_type>(status));
  }

  [[nodiscard]] static auto pending() noexcept -> try_result_type {
    return stream_pending;
  }

  [[nodiscard]] static auto ready(result_type status) noexcept
      -> try_result_type {
    return try_result_type{std::move(status)};
  }

  [[nodiscard]] static auto closed_result() noexcept -> result_type {
    return result_type{result_type::value_type::make_eof()};
  }

  [[nodiscard]] static auto internal_result() noexcept -> result_type {
    return result_type::failure(wh::core::errc::internal_error);
  }

  static auto set_automatic_close(reader_t &reader, const bool enabled)
      -> void {
    set_automatic_close_if_supported(reader, auto_close_options{enabled});
  }
};

} // namespace detail

template <stream_reader reader_t>
class copy_stream_reader final
    : public stream_base<copy_stream_reader<reader_t>,
                         typename reader_t::value_type> {
private:
  using core_reader_t =
      ::wh::core::cursor_reader<reader_t, detail::copy_stream_policy<reader_t>>;

public:
  using value_type = typename reader_t::value_type;
  using chunk_type = stream_chunk<value_type>;

  copy_stream_reader() = default;
  copy_stream_reader(const copy_stream_reader &) = delete;
  auto operator=(const copy_stream_reader &) -> copy_stream_reader & = delete;
  copy_stream_reader(copy_stream_reader &&) noexcept = default;
  auto operator=(copy_stream_reader &&) noexcept
      -> copy_stream_reader & = default;
  ~copy_stream_reader() = default;

  template <typename source_reader_t>
    requires(!std::same_as<std::remove_cvref_t<source_reader_t>,
                           copy_stream_reader>) &&
            std::constructible_from<reader_t, source_reader_t &&> &&
            std::copy_constructible<std::remove_cvref_t<
                decltype(std::declval<reader_t &>().read())>>
  explicit copy_stream_reader(source_reader_t &&reader) {
    auto readers = core_reader_t::make_readers(
        reader_t{std::forward<source_reader_t>(reader)}, 1U);
    reader_ = std::move(readers.front());
  }

  template <typename source_reader_t>
    requires(!std::same_as<std::remove_cvref_t<source_reader_t>,
                           copy_stream_reader>) &&
            std::constructible_from<reader_t, source_reader_t &&> &&
            std::copy_constructible<std::remove_cvref_t<
                decltype(std::declval<reader_t &>().read())>>
  [[nodiscard]] static auto make_copies(source_reader_t &&reader,
                                        const std::size_t count)
      -> std::vector<copy_stream_reader> {
    auto readers = core_reader_t::make_readers(
        reader_t{std::forward<source_reader_t>(reader)}, count);
    std::vector<copy_stream_reader> copies{};
    copies.reserve(readers.size());
    for (auto &entry : readers) {
      copy_stream_reader current{};
      current.reader_ = std::move(entry);
      copies.push_back(std::move(current));
    }
    return copies;
  }

  [[nodiscard]] auto try_read_impl() -> stream_try_result<chunk_type> {
    return reader_.try_read();
  }

  [[nodiscard]] auto read_impl() -> stream_result<chunk_type> {
    return reader_.read();
  }

  [[nodiscard]] auto read_async()
    requires detail::async_stream_reader<reader_t>
  {
    return reader_.read_async();
  }

  auto close_impl() -> ::wh::core::result<void> { return reader_.close(); }

  [[nodiscard]] auto is_closed_impl() const noexcept -> bool {
    return reader_.is_closed();
  }

  [[nodiscard]] auto is_source_closed() const noexcept -> bool {
    return reader_.is_source_closed();
  }

  auto set_automatic_close(const auto_close_options &options) -> void {
    reader_.set_automatic_close(options.enabled);
  }

private:
  core_reader_t reader_{};
};

template <stream_reader reader_t>
copy_stream_reader(reader_t &&)
    -> copy_stream_reader<::wh::core::remove_cvref_t<reader_t>>;

template <stream_reader reader_t>
[[nodiscard]] inline auto make_copy_stream_readers(reader_t &&reader,
                                                   const std::size_t count)
    -> std::vector<copy_stream_reader<::wh::core::remove_cvref_t<reader_t>>>
  requires std::copy_constructible<
      std::remove_cvref_t<decltype(std::declval<reader_t &>().read())>>
{
  using reader_type = copy_stream_reader<::wh::core::remove_cvref_t<reader_t>>;
  return reader_type::make_copies(std::forward<reader_t>(reader), count);
}

} // namespace wh::schema::stream
