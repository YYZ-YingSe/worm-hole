#pragma once

#include <cassert>
#include <concepts>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

namespace wh::core::cursor_reader_detail {

template <typename value_t, typename allocator_t = std::allocator<value_t>>
class retained_ring_storage {
public:
  using value_type = value_t;
  using allocator_type = allocator_t;
  using allocator_traits = std::allocator_traits<allocator_type>;

  explicit retained_ring_storage(
      const std::size_t capacity,
      const allocator_type &allocator = allocator_type{})
      : allocator_(allocator), capacity_(capacity) {
    if (capacity_ != 0U) {
      storage_ = allocator_traits::allocate(allocator_, capacity_);
    }
  }

  retained_ring_storage(const retained_ring_storage &) = delete;
  auto operator=(const retained_ring_storage &) -> retained_ring_storage & =
      delete;

  retained_ring_storage(retained_ring_storage &&other) noexcept
      : allocator_(std::move(other.allocator_)), capacity_(other.capacity_),
        storage_(other.storage_) {
    other.capacity_ = 0U;
    other.storage_ = nullptr;
  }

  auto operator=(retained_ring_storage &&other) noexcept
      -> retained_ring_storage & {
    if (this == &other) {
      return *this;
    }

    release();
    allocator_ = std::move(other.allocator_);
    capacity_ = other.capacity_;
    storage_ = other.storage_;

    other.capacity_ = 0U;
    other.storage_ = nullptr;
    return *this;
  }

  ~retained_ring_storage() { release(); }

  [[nodiscard]] auto capacity() const noexcept -> std::size_t {
    return capacity_;
  }

  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
    return allocator_;
  }

  auto reserve(std::size_t new_capacity, const std::uint64_t first_sequence,
               const std::uint64_t last_sequence) -> void
    requires std::copy_constructible<value_type>
  {
    if (new_capacity <= capacity_) {
      return;
    }

    value_type *new_storage = allocator_traits::allocate(allocator_, new_capacity);
    std::size_t constructed = 0U;
    try {
      for (auto sequence = first_sequence; sequence < last_sequence; ++sequence) {
        allocator_traits::construct(
            allocator_,
            new_storage + static_cast<std::size_t>(sequence % new_capacity),
            value_at_sequence(sequence));
        ++constructed;
      }
    } catch (...) {
      for (auto sequence = first_sequence;
           sequence < first_sequence + constructed; ++sequence) {
        allocator_traits::destroy(
            allocator_,
            new_storage + static_cast<std::size_t>(sequence % new_capacity));
      }
      allocator_traits::deallocate(allocator_, new_storage, new_capacity);
      throw;
    }

    for (auto sequence = first_sequence; sequence < last_sequence; ++sequence) {
      destroy_at_sequence(sequence);
    }

    if (storage_ != nullptr) {
      allocator_traits::deallocate(allocator_, storage_, capacity_);
    }
    storage_ = new_storage;
    capacity_ = new_capacity;
  }

  template <typename... args_t>
    requires std::constructible_from<value_type, args_t &&...>
  auto construct_at_sequence(const std::uint64_t sequence,
                             args_t &&...args) -> void {
    assert(capacity_ != 0U);
    allocator_traits::construct(allocator_, slot(sequence),
                                std::forward<args_t>(args)...);
  }

  auto destroy_at_sequence(const std::uint64_t sequence) noexcept -> void {
    assert(capacity_ != 0U);
    allocator_traits::destroy(allocator_, slot(sequence));
  }

  [[nodiscard]] auto value_at_sequence(const std::uint64_t sequence) noexcept
      -> value_type & {
    assert(capacity_ != 0U);
    return *slot(sequence);
  }

  [[nodiscard]] auto value_at_sequence(const std::uint64_t sequence) const
      noexcept -> const value_type & {
    assert(capacity_ != 0U);
    return *slot(sequence);
  }

private:
  [[nodiscard]] auto slot_index(const std::uint64_t sequence) const noexcept
      -> std::size_t {
    return static_cast<std::size_t>(sequence % capacity_);
  }

  [[nodiscard]] auto slot(const std::uint64_t sequence) noexcept
      -> value_type * {
    return storage_ + slot_index(sequence);
  }

  [[nodiscard]] auto slot(const std::uint64_t sequence) const noexcept
      -> const value_type * {
    return storage_ + slot_index(sequence);
  }

  auto release() noexcept -> void {
    if (storage_ == nullptr) {
      return;
    }
    allocator_traits::deallocate(allocator_, storage_, capacity_);
    storage_ = nullptr;
    capacity_ = 0U;
  }

  [[no_unique_address]] allocator_type allocator_{};
  std::size_t capacity_{0U};
  value_type *storage_{nullptr};
};

} // namespace wh::core::cursor_reader_detail
