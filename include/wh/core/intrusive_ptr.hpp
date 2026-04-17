// Defines intrusive ownership primitives for heap-allocated async state
// objects using a stdexec-style control block.
#pragma once

#include <atomic>
#include <concepts>
#include <cstddef>
#include <memory>
#include <type_traits>
#include <utility>

namespace wh::core::detail {

template <std::size_t reserved_bits_v> struct intrusive_count_and_bits {
  static constexpr std::size_t ref_count_increment = 1UL << reserved_bits_v;

  enum struct bits : std::size_t {};

  friend constexpr auto count(const bits value) noexcept -> std::size_t {
    return static_cast<std::size_t>(value) / ref_count_increment;
  }

  template <std::size_t bit_index_v>
  friend constexpr auto bit(const bits value) noexcept -> bool {
    static_assert(bit_index_v < reserved_bits_v, "bit index out of range");
    return (static_cast<std::size_t>(value) & (1UL << bit_index_v)) != 0U;
  }
};

template <std::size_t reserved_bits_v>
using intrusive_bits_t =
    typename intrusive_count_and_bits<reserved_bits_v>::bits;

template <typename value_t, std::size_t reserved_bits_v = 0U>
class intrusive_enable_from_this;

template <typename value_t, std::size_t reserved_bits_v = 0U>
class intrusive_ptr;

template <typename value_t, std::size_t reserved_bits_v = 0U>
struct make_intrusive_t;

template <typename value_t, std::size_t reserved_bits_v>
struct intrusive_control_block {
  using bits_t = intrusive_bits_t<reserved_bits_v>;
  static constexpr std::size_t ref_count_increment =
      intrusive_count_and_bits<reserved_bits_v>::ref_count_increment;

  union {
    value_t value;
  };
  std::atomic<std::size_t> ref_count_{ref_count_increment};

  template <typename... arg_ts>
    requires std::constructible_from<value_t, arg_ts...>
  explicit intrusive_control_block(arg_ts &&...args)
      noexcept(std::is_nothrow_constructible_v<value_t, arg_ts...>) {
    std::construct_at(std::addressof(value), std::forward<arg_ts>(args)...);
  }

  ~intrusive_control_block() { std::destroy_at(std::addressof(value)); }

  auto inc_ref() noexcept -> bits_t {
    const auto previous =
        ref_count_.fetch_add(ref_count_increment, std::memory_order_relaxed);
    return static_cast<bits_t>(previous);
  }

  auto dec_ref() noexcept -> bits_t {
    const auto previous =
        ref_count_.fetch_sub(ref_count_increment, std::memory_order_acq_rel);
    if (count(static_cast<bits_t>(previous)) == 1U) {
      std::atomic_thread_fence(std::memory_order_acquire);
      delete this;
    }
    return static_cast<bits_t>(previous);
  }

  template <std::size_t bit_index_v>
  [[nodiscard]] auto is_set() const noexcept -> bool {
    const auto current = ref_count_.load(std::memory_order_relaxed);
    return bit<bit_index_v>(static_cast<bits_t>(current));
  }

  template <std::size_t bit_index_v>
  auto set_bit() noexcept -> bits_t {
    static_assert(bit_index_v < reserved_bits_v, "bit index out of range");
    constexpr std::size_t mask = 1UL << bit_index_v;
    const auto previous =
        ref_count_.fetch_or(mask, std::memory_order_acq_rel);
    return static_cast<bits_t>(previous);
  }

  template <std::size_t bit_index_v>
  auto clear_bit() noexcept -> bits_t {
    static_assert(bit_index_v < reserved_bits_v, "bit index out of range");
    constexpr std::size_t mask = 1UL << bit_index_v;
    const auto previous =
        ref_count_.fetch_and(~mask, std::memory_order_acq_rel);
    return static_cast<bits_t>(previous);
  }
};

template <typename value_t, std::size_t reserved_bits_v>
class intrusive_enable_from_this {
public:
  // Types deriving from intrusive_enable_from_this must be created through
  // make_intrusive<T>(...) so `this` is embedded in an intrusive_control_block.
  intrusive_enable_from_this() noexcept = default;
  intrusive_enable_from_this(const intrusive_enable_from_this &) noexcept =
      default;
  auto operator=(const intrusive_enable_from_this &) noexcept
      -> intrusive_enable_from_this & = default;

  [[nodiscard]] auto intrusive_from_this() noexcept
      -> intrusive_ptr<value_t, reserved_bits_v>;

  [[nodiscard]] auto intrusive_from_this() const noexcept
      -> intrusive_ptr<const value_t, reserved_bits_v>;

protected:
  ~intrusive_enable_from_this() = default;

private:
  using bits_t = intrusive_bits_t<reserved_bits_v>;

  friend value_t;
  template <typename other_t, std::size_t other_reserved_bits_v>
  friend class intrusive_ptr;

  auto inc_ref() noexcept -> bits_t {
    auto *block = reinterpret_cast<intrusive_control_block<value_t, reserved_bits_v> *>(
        static_cast<value_t *>(this));
    return block->inc_ref();
  }

  auto dec_ref() noexcept -> bits_t {
    auto *block = reinterpret_cast<intrusive_control_block<value_t, reserved_bits_v> *>(
        static_cast<value_t *>(this));
    return block->dec_ref();
  }

  template <std::size_t bit_index_v>
  [[nodiscard]] auto is_set() const noexcept -> bool {
    auto *block =
        reinterpret_cast<const intrusive_control_block<value_t, reserved_bits_v> *>(
            static_cast<const value_t *>(this));
    return block->template is_set<bit_index_v>();
  }

  template <std::size_t bit_index_v> auto set_bit() noexcept -> bits_t {
    auto *block = reinterpret_cast<intrusive_control_block<value_t, reserved_bits_v> *>(
        static_cast<value_t *>(this));
    return block->template set_bit<bit_index_v>();
  }

  template <std::size_t bit_index_v> auto clear_bit() noexcept -> bits_t {
    auto *block = reinterpret_cast<intrusive_control_block<value_t, reserved_bits_v> *>(
        static_cast<value_t *>(this));
    return block->template clear_bit<bit_index_v>();
  }
};

template <typename value_t, std::size_t reserved_bits_v>
class intrusive_ptr {
  using raw_value_t = std::remove_const_t<value_t>;
  using control_block_t = intrusive_control_block<raw_value_t, reserved_bits_v>;

public:
  using element_type = value_t;

  intrusive_ptr() noexcept = default;
  intrusive_ptr(std::nullptr_t) noexcept {}

  intrusive_ptr(const intrusive_ptr &other) noexcept : data_(other.data_) {
    inc_ref();
  }

  intrusive_ptr(intrusive_ptr &&other) noexcept
      : data_(std::exchange(other.data_, nullptr)) {}

  template <typename other_t>
    requires (!std::same_as<other_t, value_t> &&
              std::same_as<std::remove_const_t<other_t>, raw_value_t> &&
              std::convertible_to<other_t *, value_t *>)
  intrusive_ptr(const intrusive_ptr<other_t, reserved_bits_v> &other) noexcept
      : data_(other.data_) {
    inc_ref();
  }

  template <typename other_t>
    requires (!std::same_as<other_t, value_t> &&
              std::same_as<std::remove_const_t<other_t>, raw_value_t> &&
              std::convertible_to<other_t *, value_t *>)
  intrusive_ptr(intrusive_ptr<other_t, reserved_bits_v> &&other) noexcept
      : data_(std::exchange(other.data_, nullptr)) {}

  auto operator=(const intrusive_ptr &other) noexcept -> intrusive_ptr & {
    if (this == &other) {
      return *this;
    }
    intrusive_ptr copy{other};
    swap(copy);
    return *this;
  }

  auto operator=(intrusive_ptr &&other) noexcept -> intrusive_ptr & {
    if (this == &other) {
      return *this;
    }
    intrusive_ptr moved{std::move(other)};
    swap(moved);
    return *this;
  }

  template <typename other_t>
    requires (!std::same_as<other_t, value_t> &&
              std::same_as<std::remove_const_t<other_t>, raw_value_t> &&
              std::convertible_to<other_t *, value_t *>)
  auto operator=(const intrusive_ptr<other_t, reserved_bits_v> &other) noexcept
      -> intrusive_ptr & {
    intrusive_ptr copy{other};
    swap(copy);
    return *this;
  }

  template <typename other_t>
    requires (!std::same_as<other_t, value_t> &&
              std::same_as<std::remove_const_t<other_t>, raw_value_t> &&
              std::convertible_to<other_t *, value_t *>)
  auto operator=(intrusive_ptr<other_t, reserved_bits_v> &&other) noexcept
      -> intrusive_ptr & {
    intrusive_ptr moved{std::move(other)};
    swap(moved);
    return *this;
  }

  ~intrusive_ptr() { dec_ref(); }

  auto reset() noexcept -> void { *this = intrusive_ptr{}; }

  auto swap(intrusive_ptr &other) noexcept -> void {
    std::swap(data_, other.data_);
  }

  [[nodiscard]] auto get() const noexcept -> value_t * {
    return data_ == nullptr ? nullptr : std::addressof(data_->value);
  }

  [[nodiscard]] auto operator->() const noexcept -> value_t * { return get(); }

  [[nodiscard]] auto operator*() const noexcept -> value_t & { return *get(); }

  [[nodiscard]] explicit operator bool() const noexcept {
    return data_ != nullptr;
  }

  [[nodiscard]] auto operator!() const noexcept -> bool {
    return data_ == nullptr;
  }

  [[nodiscard]] auto operator==(const intrusive_ptr &) const noexcept -> bool =
      default;

  [[nodiscard]] auto operator==(std::nullptr_t) const noexcept -> bool {
    return data_ == nullptr;
  }

private:
  template <typename other_t, std::size_t other_reserved_bits_v>
  friend class intrusive_ptr;

  template <typename other_t, std::size_t other_reserved_bits_v>
  friend class intrusive_enable_from_this;

  template <typename other_t, std::size_t other_reserved_bits_v>
  friend struct make_intrusive_t;

  explicit intrusive_ptr(control_block_t *data) noexcept : data_(data) {}

  auto inc_ref() noexcept -> void {
    if (data_ != nullptr) {
      data_->inc_ref();
    }
  }

  auto dec_ref() noexcept -> void {
    if (data_ != nullptr) {
      data_->dec_ref();
      data_ = nullptr;
    }
  }

  control_block_t *data_{nullptr};
};

template <typename value_t, std::size_t reserved_bits_v>
[[nodiscard]] auto
intrusive_enable_from_this<value_t, reserved_bits_v>::intrusive_from_this()
    noexcept -> intrusive_ptr<value_t, reserved_bits_v> {
  auto *block = reinterpret_cast<intrusive_control_block<value_t, reserved_bits_v> *>(
      static_cast<value_t *>(this));
  block->inc_ref();
  return intrusive_ptr<value_t, reserved_bits_v>{block};
}

template <typename value_t, std::size_t reserved_bits_v>
[[nodiscard]] auto intrusive_enable_from_this<
    value_t, reserved_bits_v>::intrusive_from_this() const noexcept
    -> intrusive_ptr<const value_t, reserved_bits_v> {
  auto *block =
      const_cast<intrusive_control_block<value_t, reserved_bits_v> *>(
          reinterpret_cast<
              const intrusive_control_block<value_t, reserved_bits_v> *>(
              static_cast<const value_t *>(this)));
  block->inc_ref();
  return intrusive_ptr<const value_t, reserved_bits_v>{block};
}

template <typename value_t, std::size_t reserved_bits_v>
struct make_intrusive_t {
  template <typename... arg_ts>
    requires std::constructible_from<value_t, arg_ts...>
  [[nodiscard]] auto operator()(arg_ts &&...args) const
      -> intrusive_ptr<value_t, reserved_bits_v> {
    static_assert(!std::is_const_v<value_t>,
                  "make_intrusive requires a non-const value type");
    return intrusive_ptr<value_t, reserved_bits_v>{
        ::new intrusive_control_block<value_t, reserved_bits_v>(
            std::forward<arg_ts>(args)...)};
  }
};

template <typename value_t, std::size_t reserved_bits_v = 0U>
inline constexpr make_intrusive_t<value_t, reserved_bits_v> make_intrusive{};

} // namespace wh::core::detail
