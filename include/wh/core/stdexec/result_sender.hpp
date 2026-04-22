// Defines reusable result-oriented sender helpers shared by component and
// compose async paths.
#pragma once

#include <concepts>
#include <cstddef>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>

#include <stdexec/execution.hpp>

#include "wh/core/error.hpp"
#include "wh/core/result.hpp"
#include "wh/core/stdexec/erased_receiver_ref.hpp"
#include "wh/core/stdexec/detail/receiver_stop_bridge.hpp"
#include "wh/core/stdexec/manual_lifetime.hpp"

namespace wh::core::detail {

namespace result_sender_detail {

inline constexpr std::size_t erased_inline_size = 6U * sizeof(void *);
inline constexpr std::size_t erased_inline_align = alignof(std::max_align_t);

template <typename value_t>
inline constexpr bool stored_inline_v =
    sizeof(value_t) <= erased_inline_size && alignof(value_t) <= erased_inline_align;

using receiver_policy = wh::core::detail::erased_receiver_scheduler_policy<
    wh::core::detail::missing_scheduler_mode::fallback_inline>;

template <typename result_t>
using receiver_ref = wh::core::detail::erased_receiver_ref<receiver_policy, result_t>;

template <std::size_t InlineSize = erased_inline_size,
          std::size_t InlineAlign = erased_inline_align>
class erased_operation_storage {
public:
  erased_operation_storage() noexcept = default;
  erased_operation_storage(const erased_operation_storage &) = delete;
  auto operator=(const erased_operation_storage &) -> erased_operation_storage & = delete;
  erased_operation_storage(erased_operation_storage &&) = delete;
  auto operator=(erased_operation_storage &&) -> erased_operation_storage & = delete;

  ~erased_operation_storage() { reset(); }

  [[nodiscard]] auto engaged() const noexcept -> bool { return vtable_ != nullptr; }

  auto reset() noexcept -> void {
    if (vtable_ == nullptr) {
      return;
    }
    vtable_->destroy(*this);
  }

  auto start() & noexcept -> void {
    if (vtable_ != nullptr) {
      vtable_->start(*this);
    }
  }

private:
  struct vtable {
    void (*destroy)(erased_operation_storage &) noexcept;
    void (*start)(erased_operation_storage &) noexcept;
  };

  template <typename operation_t>
  [[nodiscard]] auto object() noexcept -> operation_t * {
    return std::launder(static_cast<operation_t *>(object_));
  }

  template <typename operation_t, typename factory_t> auto emplace(factory_t &&factory) -> void {
    if constexpr (stored_inline_v<operation_t>) {
      object_ = inline_buffer_;
      ::new (object_) operation_t{std::forward<factory_t>(factory)()};
      uses_heap_ = false;
    } else {
      object_ = new operation_t(std::forward<factory_t>(factory)());
      uses_heap_ = true;
    }
    vtable_ = make_vtable<operation_t>();
  }

  template <typename operation_t>
  [[nodiscard]] static auto make_vtable() noexcept -> const vtable * {
    static const vtable current{
        .destroy = [](erased_operation_storage &self) noexcept {
          if (self.uses_heap_) {
            delete self.object<operation_t>();
          } else {
            std::destroy_at(self.object<operation_t>());
          }
          self.vtable_ = nullptr;
          self.object_ = nullptr;
          self.uses_heap_ = false;
        },
        .start = [](erased_operation_storage &self) noexcept {
          stdexec::start(*self.object<operation_t>());
        },
    };
    return std::addressof(current);
  }

  const vtable *vtable_{nullptr};
  void *object_{nullptr};
  bool uses_heap_{false};
  alignas(InlineAlign) std::byte inline_buffer_[InlineSize]{};

  template <typename receiver_ref_u, std::size_t sender_inline_size, std::size_t sender_inline_align>
  friend class erased_sender_storage;
};

template <typename receiver_ref_t, std::size_t InlineSize = erased_inline_size,
          std::size_t InlineAlign = erased_inline_align>
class erased_sender_storage {
public:
  erased_sender_storage() noexcept = default;
  erased_sender_storage(const erased_sender_storage &) = delete;
  auto operator=(const erased_sender_storage &) -> erased_sender_storage & = delete;

  erased_sender_storage(erased_sender_storage &&other) { move_from(std::move(other)); }

  auto operator=(erased_sender_storage &&other) -> erased_sender_storage & {
    if (this != std::addressof(other)) {
      reset();
      move_from(std::move(other));
    }
    return *this;
  }

  ~erased_sender_storage() { reset(); }

  template <typename sender_t>
  auto emplace(sender_t &&sender) -> void {
    using stored_sender_t = std::remove_cvref_t<sender_t>;
    if constexpr (stored_inline_v<stored_sender_t>) {
      object_ = inline_buffer_;
      std::construct_at(static_cast<stored_sender_t *>(object_), std::forward<sender_t>(sender));
      uses_heap_ = false;
    } else {
      object_ = new stored_sender_t(std::forward<sender_t>(sender));
      uses_heap_ = true;
    }
    vtable_ = make_vtable<stored_sender_t>();
  }

  auto reset() noexcept -> void {
    if (vtable_ == nullptr) {
      return;
    }
    vtable_->destroy(*this);
  }

  auto connect(receiver_ref_t receiver, erased_operation_storage<> &target) & -> void {
    vtable_->connect(*this, std::move(receiver), target);
  }

private:
  struct vtable {
    void (*destroy)(erased_sender_storage &) noexcept;
    void (*move_construct)(erased_sender_storage &, erased_sender_storage &&);
    void (*connect)(erased_sender_storage &, receiver_ref_t, erased_operation_storage<> &);
  };

  template <typename sender_t>
  [[nodiscard]] auto object() noexcept -> sender_t * {
    return std::launder(static_cast<sender_t *>(object_));
  }

  template <typename sender_t>
  [[nodiscard]] static auto make_vtable() noexcept -> const vtable * {
    static const vtable current{
        .destroy = [](erased_sender_storage &self) noexcept {
          if (self.uses_heap_) {
            delete self.object<sender_t>();
          } else {
            std::destroy_at(self.object<sender_t>());
          }
          self.vtable_ = nullptr;
          self.object_ = nullptr;
          self.uses_heap_ = false;
        },
        .move_construct = [](erased_sender_storage &target, erased_sender_storage &&source) {
          target.vtable_ = source.vtable_;
          target.uses_heap_ = source.uses_heap_;
          if (source.uses_heap_) {
            target.object_ = source.object_;
          } else {
            target.object_ = target.inline_buffer_;
            std::construct_at(static_cast<sender_t *>(target.object_),
                              std::move(*source.object<sender_t>()));
            std::destroy_at(source.object<sender_t>());
          }
          source.vtable_ = nullptr;
          source.object_ = nullptr;
          source.uses_heap_ = false;
        },
        .connect = [](erased_sender_storage &self, receiver_ref_t receiver,
                      erased_operation_storage<> &target) -> void {
          using operation_t = stdexec::connect_result_t<sender_t, receiver_ref_t>;
          target.template emplace<operation_t>(
              [&self, receiver = std::move(receiver)]() mutable -> operation_t {
                return stdexec::connect(std::move(*self.object<sender_t>()), std::move(receiver));
              });
        },
    };
    return std::addressof(current);
  }

  auto move_from(erased_sender_storage &&other) -> void {
    if (other.vtable_ == nullptr) {
      return;
    }
    other.vtable_->move_construct(*this, std::move(other));
  }

  const vtable *vtable_{nullptr};
  void *object_{nullptr};
  bool uses_heap_{false};
  alignas(InlineAlign) std::byte inline_buffer_[InlineSize]{};
};

} // namespace result_sender_detail

template <typename result_t, stdexec::sender sender_t>
[[nodiscard]] constexpr auto normalize_result_sender(sender_t &&sender) {
  return std::forward<sender_t>(sender) | stdexec::upon_error([](auto &&) noexcept {
           return result_t::failure(wh::core::errc::internal_error);
         });
}

template <typename result_t> class result_sender final {
  using inner_completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(result_t),
                                     stdexec::set_error_t(std::exception_ptr),
                                     stdexec::set_stopped_t()>;
  using receiver_ref_t = result_sender_detail::receiver_ref<result_t>;
  using sender_storage_t = result_sender_detail::erased_sender_storage<receiver_ref_t>;
  using operation_storage_t = result_sender_detail::erased_operation_storage<>;

public:
  using sender_concept = stdexec::sender_t;
  using completion_signatures =
      stdexec::completion_signatures<stdexec::set_value_t(result_t), stdexec::set_stopped_t()>;

  template <typename... env_t> static consteval auto get_completion_signatures() noexcept {
    return completion_signatures{};
  }

  template <typename sender_t>
    requires(!std::same_as<std::remove_cvref_t<sender_t>, result_sender>)
  /*implicit*/ result_sender(sender_t &&sender) {
    auto normalized = normalize_result_sender<result_t>(std::forward<sender_t>(sender));
    sender_.emplace(std::move(normalized));
  }

  result_sender(const result_sender &) = delete;
  auto operator=(const result_sender &) -> result_sender & = delete;
  result_sender(result_sender &&) = default;
  auto operator=(result_sender &&) -> result_sender & = default;
  ~result_sender() = default;

  template <stdexec::receiver_of<completion_signatures> receiver_t> class operation {
    using bridge_t = wh::core::detail::receiver_stop_bridge<receiver_t>;

    struct child_receiver {
      using receiver_concept = stdexec::receiver_t;

      bridge_t *bridge{nullptr};

      auto set_value(result_t status) && noexcept -> void { bridge->set_value(std::move(status)); }

      auto set_error(std::exception_ptr) && noexcept -> void {
        bridge->set_value(result_t::failure(wh::core::errc::internal_error));
      }

      auto set_stopped() && noexcept -> void { bridge->set_stopped(); }

      [[nodiscard]] auto get_env() const noexcept -> typename bridge_t::stop_env_t {
        return bridge->env();
      }
    };

  public:
    using operation_state_concept = stdexec::operation_state_t;

    operation(sender_storage_t sender, receiver_t receiver)
        : sender_(std::move(sender)), receiver_(std::move(receiver)) {}

    ~operation() = default;

    auto start() & noexcept -> void {
      try {
        bridge_.emplace(receiver_);
      } catch (...) {
        stdexec::set_value(std::move(receiver_), result_t::failure(wh::core::errc::internal_error));
        return;
      }

      if (bridge_->stop_requested()) {
        bridge_->set_stopped();
        return;
      }

      try {
        child_receiver_.bridge = std::addressof(*bridge_);
        sender_.connect(receiver_ref_t{child_receiver_}, child_op_);
        child_op_.start();
      } catch (...) {
        bridge_->set_value(result_t::failure(wh::core::errc::internal_error));
      }
    }

  private:
    sender_storage_t sender_;
    receiver_t receiver_;
    std::optional<bridge_t> bridge_{};
    child_receiver child_receiver_{};
    operation_storage_t child_op_{};
  };

  template <stdexec::receiver_of<completion_signatures> receiver_t>
  [[nodiscard]] auto connect(receiver_t receiver) && -> operation<receiver_t> {
    return operation<receiver_t>{std::move(sender_), std::move(receiver)};
  }

private:
  sender_storage_t sender_{};
};

} // namespace wh::core::detail
