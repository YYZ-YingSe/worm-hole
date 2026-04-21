// Defines document request/batch types and the public document component.
#pragma once

#include <concepts>
#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <stdexec/execution.hpp>

#include "wh/core/compiler.hpp"
#include "wh/core/component.hpp"
#include "wh/core/result.hpp"
#include "wh/core/run_context.hpp"
#include "wh/core/stdexec.hpp"
#include "wh/document/options.hpp"
#include "wh/schema/document.hpp"

namespace wh::document {

/// Document collection type used across document processing interfaces.
using document_batch = std::vector<wh::schema::document>;

enum class document_source_kind : std::uint8_t {
  content = 0U,
  uri,
};

struct document_request {
  document_source_kind source_kind{document_source_kind::content};
  std::string source{};
  loader_options options{};
};

namespace detail {

using document_result = wh::core::result<document_batch>;
template <typename impl_t>
concept sync_document_handler_with_context = requires(
    const impl_t &impl, const document_request &request, wh::core::run_context &callback_context) {
  { impl.process(request, callback_context) } -> std::same_as<wh::core::result<document_batch>>;
};

template <typename impl_t>
concept sender_document_handler_const =
    requires(const impl_t &impl, const document_request &request) {
      { impl.process_sender(request) } -> stdexec::sender;
    };

template <typename impl_t>
concept sender_document_handler_move = requires(const impl_t &impl, document_request &&request) {
  { impl.process_sender(std::move(request)) } -> stdexec::sender;
};

template <typename impl_t>
concept async_document_handler =
    sender_document_handler_const<impl_t> || sender_document_handler_move<impl_t>;

template <typename impl_t>
concept sender_document_handler = async_document_handler<impl_t>;

template <typename impl_t>
concept document_impl =
    sync_document_handler_with_context<impl_t> || async_document_handler<impl_t>;

template <typename impl_t>
[[nodiscard]] inline auto describe_impl(const impl_t &impl) -> wh::core::component_descriptor {
  if constexpr (requires {
                  { impl.descriptor() } -> std::same_as<wh::core::component_descriptor>;
                }) {
    return impl.descriptor();
  } else {
    return wh::core::component_descriptor{"Document", wh::core::component_kind::document};
  }
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_document_impl(const impl_t &impl,
                                                 const document_request &request,
                                                 wh::core::run_context &callback_context)
    -> wh::core::result<document_batch> {
  return impl.process(request, callback_context);
}

template <typename impl_t>
[[nodiscard]] inline auto run_sync_document_impl(const impl_t &impl, document_request &&request,
                                                 wh::core::run_context &callback_context)
    -> wh::core::result<document_batch> {
  if constexpr (requires {
                  {
                    impl.process(std::move(request), callback_context)
                  } -> std::same_as<wh::core::result<document_batch>>;
                }) {
    return impl.process(std::move(request), callback_context);
  } else {
    return run_sync_document_impl(impl, request, callback_context);
  }
}

} // namespace detail

/// Public interface for one document component.
template <detail::document_impl impl_t> class document {
public:
  explicit document(const impl_t &impl)
    requires std::copy_constructible<impl_t>
      : impl_(impl) {}

  explicit document(impl_t &&impl) noexcept(std::is_nothrow_move_constructible_v<impl_t>)
      : impl_(std::move(impl)) {}

  document(const document &) = default;
  document(document &&) noexcept = default;
  auto operator=(const document &) -> document & = default;
  auto operator=(document &&) noexcept -> document & = default;
  ~document() = default;

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    return detail::describe_impl(impl_);
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, document_request> &&
             detail::sync_document_handler_with_context<impl_t>
  [[nodiscard]] auto process(request_t &&request, wh::core::run_context &callback_context) const
      -> wh::core::result<document_batch> {
    return detail::run_sync_document_impl(impl_, std::forward<request_t>(request),
                                          callback_context);
  }

  template <typename request_t>
    requires std::same_as<std::remove_cvref_t<request_t>, document_request> &&
             detail::async_document_handler<impl_t>
  [[nodiscard]] auto async_process(request_t &&request, wh::core::run_context &) const -> auto {
    return wh::core::detail::defer_request_result_sender<detail::document_result>(
        document_request{std::forward<request_t>(request)},
        [this](auto &&forwarded_request) -> decltype(auto) {
          return impl_.process_sender(std::forward<decltype(forwarded_request)>(forwarded_request));
        });
  }

  wh_no_unique_address impl_t impl_;
};

template <typename impl_t> document(impl_t &&) -> document<std::remove_cvref_t<impl_t>>;

} // namespace wh::document
