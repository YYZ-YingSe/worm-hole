// Defines reusable manual-lifetime storage for move-only sender op-states.
#pragma once

#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace wh::core::detail {

template <typename value_t> class manual_lifetime_box {
public:
  manual_lifetime_box() = default;
  manual_lifetime_box(const manual_lifetime_box &) = delete;
  auto operator=(const manual_lifetime_box &) -> manual_lifetime_box & = delete;

  ~manual_lifetime_box() { reset(); }

  template <typename... args_t>
  auto emplace(args_t &&...args) -> value_t & {
    reset();
    auto *value = std::construct_at(reinterpret_cast<value_t *>(storage_),
                                    std::forward<args_t>(args)...);
    engaged_ = true;
    return *value;
  }

  template <typename factory_t, typename... args_t>
  auto emplace_from(factory_t &&factory, args_t &&...args) -> value_t & {
    reset();
    auto *value = ::new (static_cast<void *>(storage_)) value_t(std::invoke(
        std::forward<factory_t>(factory), std::forward<args_t>(args)...));
    engaged_ = true;
    return *value;
  }

  auto reset() noexcept -> void {
    if (!engaged_) {
      return;
    }
    std::destroy_at(ptr());
    engaged_ = false;
  }

  [[nodiscard]] auto has_value() const noexcept -> bool { return engaged_; }

  [[nodiscard]] auto get() noexcept -> value_t & { return *ptr(); }

private:
  [[nodiscard]] auto ptr() noexcept -> value_t * {
    return std::launder(reinterpret_cast<value_t *>(storage_));
  }

  alignas(value_t) std::byte storage_[sizeof(value_t)];
  bool engaged_{false};
};

} // namespace wh::core::detail
