// Defines parser request contracts and the erased parser handle used by
// concrete document parsers to produce normalized document batches.
#pragma once

#include <concepts>
#include <cstddef>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/core/component/concepts.hpp"
#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/document/document.hpp"
#include "wh/document/parser/option.hpp"

namespace wh::document::parser {

struct parse_request_view {
  std::string_view content{};
  parse_options_view options{};
};

struct parse_request {
  std::string content{};
  parse_options options{};
};

template <typename parser_t>
concept parser_like =
    wh::core::component_descriptor_provider<parser_t> &&
    requires(const parser_t &parser, const parse_request &request, parse_request &&movable_request,
             const parse_request_view request_view) {
      { parser.parse(request) } -> std::same_as<wh::core::result<document_batch>>;
      {
        parser.parse(std::move(movable_request))
      } -> std::same_as<wh::core::result<document_batch>>;
      { parser.parse(request_view) } -> std::same_as<wh::core::result<document_batch>>;
    };

class parser {
public:
  parser() = default;

  template <typename impl_t>
    requires parser_like<std::remove_cvref_t<impl_t>> &&
             (!std::same_as<std::remove_cvref_t<impl_t>, parser>) &&
             std::copy_constructible<std::remove_cvref_t<impl_t>>
  explicit parser(impl_t &&impl) {
    emplace_impl<std::remove_cvref_t<impl_t>>(std::forward<impl_t>(impl));
  }

  parser(const parser &other) { copy_from(other); }

  parser(parser &&other) noexcept { move_from(std::move(other)); }

  auto operator=(const parser &other) -> parser & {
    if (this == &other) {
      return *this;
    }
    parser copy{other};
    reset();
    move_from(std::move(copy));
    return *this;
  }

  auto operator=(parser &&other) noexcept -> parser & {
    if (this == &other) {
      return *this;
    }
    reset();
    move_from(std::move(other));
    return *this;
  }

  ~parser() { reset(); }

  [[nodiscard]] auto has_value() const noexcept -> bool { return vtable_ != nullptr; }

  [[nodiscard]] auto descriptor() const -> wh::core::component_descriptor {
    if (!has_value()) {
      return wh::core::component_descriptor{};
    }
    return vtable_->descriptor(*this);
  }

  [[nodiscard]] auto parse(const parse_request &request) const -> wh::core::result<document_batch> {
    if (!has_value()) {
      return wh::core::result<document_batch>::failure(wh::core::errc::not_supported);
    }
    return vtable_->parse_const(*this, request);
  }

  [[nodiscard]] auto parse(parse_request &&request) const -> wh::core::result<document_batch> {
    if (!has_value()) {
      return wh::core::result<document_batch>::failure(wh::core::errc::not_supported);
    }
    return vtable_->parse_move(*this, std::move(request));
  }

  [[nodiscard]] auto parse(const parse_request_view request) const
      -> wh::core::result<document_batch> {
    if (!has_value()) {
      return wh::core::result<document_batch>::failure(wh::core::errc::not_supported);
    }
    return vtable_->parse_view(*this, request);
  }

private:
  static constexpr std::size_t inline_storage_size_ = sizeof(void *) * 6U;

  struct parser_vtable {
    wh::core::component_descriptor (*descriptor)(const parser &);
    wh::core::result<document_batch> (*parse_const)(const parser &, const parse_request &);
    wh::core::result<document_batch> (*parse_move)(const parser &, parse_request &&);
    wh::core::result<document_batch> (*parse_view)(const parser &, parse_request_view);
    void (*copy_into)(const parser &, parser &);
    void (*move_into)(parser &, parser &) noexcept;
    void (*destroy)(parser &) noexcept;
  };

  template <typename stored_t>
  static constexpr bool use_inline_storage_ =
      sizeof(stored_t) <= inline_storage_size_ && alignof(stored_t) <= alignof(std::max_align_t) &&
      std::is_nothrow_move_constructible_v<stored_t>;

  template <typename stored_t>
  [[nodiscard]] static auto storage_ptr(parser &self) noexcept -> stored_t * {
    if constexpr (use_inline_storage_<stored_t>) {
      return std::launder(reinterpret_cast<stored_t *>(self.inline_storage_));
    }
    return static_cast<stored_t *>(self.heap_storage_);
  }

  template <typename stored_t>
  [[nodiscard]] static auto storage_ptr(const parser &self) noexcept -> const stored_t * {
    if constexpr (use_inline_storage_<stored_t>) {
      return std::launder(reinterpret_cast<const stored_t *>(self.inline_storage_));
    }
    return static_cast<const stored_t *>(self.heap_storage_);
  }

  template <typename stored_t> static const parser_vtable vtable_for;

  template <typename stored_t, typename impl_t> auto emplace_impl(impl_t &&impl) -> void {
    if constexpr (use_inline_storage_<stored_t>) {
      new (inline_storage_) stored_t(std::forward<impl_t>(impl));
      inline_storage_used_ = true;
      heap_storage_ = nullptr;
    } else {
      heap_storage_ = new stored_t(std::forward<impl_t>(impl));
      inline_storage_used_ = false;
    }
    vtable_ = &vtable_for<stored_t>;
  }

  auto copy_from(const parser &other) -> void {
    if (!other.has_value()) {
      return;
    }
    other.vtable_->copy_into(other, *this);
  }

  auto move_from(parser &&other) noexcept -> void {
    if (!other.has_value()) {
      return;
    }
    other.vtable_->move_into(other, *this);
  }

  auto reset() noexcept -> void {
    if (vtable_ == nullptr) {
      return;
    }
    vtable_->destroy(*this);
  }

  alignas(std::max_align_t) std::byte inline_storage_[inline_storage_size_]{};
  void *heap_storage_{nullptr};
  bool inline_storage_used_{false};
  const parser_vtable *vtable_{nullptr};
};

template <typename stored_t>
inline constexpr parser::parser_vtable parser::vtable_for{
    [](const parser &self) -> wh::core::component_descriptor {
      return storage_ptr<stored_t>(self)->descriptor();
    },
    [](const parser &self, const parse_request &request) -> wh::core::result<document_batch> {
      return storage_ptr<stored_t>(self)->parse(request);
    },
    [](const parser &self, parse_request &&request) -> wh::core::result<document_batch> {
      return storage_ptr<stored_t>(self)->parse(std::move(request));
    },
    [](const parser &self, const parse_request_view request) -> wh::core::result<document_batch> {
      return storage_ptr<stored_t>(self)->parse(request);
    },
    [](const parser &from, parser &to) {
      if constexpr (use_inline_storage_<stored_t>) {
        new (to.inline_storage_) stored_t(*storage_ptr<stored_t>(from));
        to.inline_storage_used_ = true;
        to.heap_storage_ = nullptr;
      } else {
        to.heap_storage_ = new stored_t(*storage_ptr<stored_t>(from));
        to.inline_storage_used_ = false;
      }
      to.vtable_ = &vtable_for<stored_t>;
    },
    [](parser &from, parser &to) noexcept {
      if constexpr (use_inline_storage_<stored_t>) {
        new (to.inline_storage_) stored_t(std::move(*storage_ptr<stored_t>(from)));
        std::destroy_at(storage_ptr<stored_t>(from));
        to.inline_storage_used_ = true;
        to.heap_storage_ = nullptr;
      } else {
        to.heap_storage_ = from.heap_storage_;
        to.inline_storage_used_ = false;
        from.heap_storage_ = nullptr;
      }
      to.vtable_ = &vtable_for<stored_t>;
      from.inline_storage_used_ = false;
      from.vtable_ = nullptr;
    },
    [](parser &self) noexcept {
      if constexpr (use_inline_storage_<stored_t>) {
        std::destroy_at(storage_ptr<stored_t>(self));
      } else {
        delete storage_ptr<stored_t>(self);
        self.heap_storage_ = nullptr;
      }
      self.inline_storage_used_ = false;
      self.vtable_ = nullptr;
    }};

} // namespace wh::document::parser
