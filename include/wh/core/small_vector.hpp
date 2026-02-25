#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

#include "wh/core/compiler.hpp"
#include "wh/core/result.hpp"

namespace wh::core {

struct default_init_t {
  explicit constexpr default_init_t() = default;
};

inline constexpr default_init_t default_init{};

struct small_vector_default_options {
  using size_type = std::size_t;

  static constexpr std::size_t growth_numerator = 3U;
  static constexpr std::size_t growth_denominator = 2U;
  static constexpr std::size_t minimum_dynamic_capacity = 0U;
  static constexpr bool heap_enabled = true;
  static constexpr bool shrink_to_inline = true;
};

template <
    std::size_t growth_numerator_v = 3U, std::size_t growth_denominator_v = 2U,
    std::size_t minimum_dynamic_capacity_v = 0U, bool heap_enabled_v = true,
    bool shrink_to_inline_v = true, typename size_type_t = std::size_t>
struct small_vector_options {
  using size_type = size_type_t;

  static constexpr std::size_t growth_numerator = growth_numerator_v;
  static constexpr std::size_t growth_denominator = growth_denominator_v;
  static constexpr std::size_t minimum_dynamic_capacity =
      minimum_dynamic_capacity_v;
  static constexpr bool heap_enabled = heap_enabled_v;
  static constexpr bool shrink_to_inline = shrink_to_inline_v;
};

template <typename value_t, std::size_t inline_capacity = 8U,
          typename allocator_t = std::allocator<value_t>,
          typename options_t = small_vector_default_options>
class small_vector_impl {
  static_assert(inline_capacity > 0U,
                "small_vector inline_capacity must be greater than zero");
  static_assert(options_t::growth_denominator > 0U,
                "small_vector growth denominator must be greater than zero");
  static_assert(std::is_unsigned_v<typename options_t::size_type>,
                "small_vector options size_type must be unsigned");

public:
  using value_type = value_t;
  using allocator_type = allocator_t;
  using options_type = options_t;
  using is_partially_propagable = std::true_type;
  using size_type = typename options_t::size_type;
  using difference_type = std::ptrdiff_t;
  using pointer = value_t *;
  using const_pointer = const value_t *;
  using reference = value_t &;
  using const_reference = const value_t &;
  using iterator = pointer;
  using const_iterator = const_pointer;
  using reverse_iterator = std::reverse_iterator<iterator>;
  using const_reverse_iterator = std::reverse_iterator<const_iterator>;

  constexpr small_vector_impl() noexcept
      : data_(inline_data()), size_(0U), capacity_(inline_capacity) {}

  explicit small_vector_impl(const allocator_type &allocator) noexcept
      : data_(inline_data()), size_(0U), capacity_(inline_capacity),
        allocator_(allocator) {}

  explicit small_vector_impl(const size_type count)
    requires std::default_initializable<value_t>
      : small_vector_impl() {
    auto status = reserve(count);
    if (status.has_error()) {
      return;
    }

    status = append_value_initialized_n(static_cast<std::size_t>(count));
    (void)status;
  }

  small_vector_impl(const size_type count, default_init_t)
    requires std::default_initializable<value_t>
      : small_vector_impl() {
    auto status = reserve(count);
    if (status.has_error()) {
      return;
    }

    status = append_default_n(static_cast<std::size_t>(count));
    (void)status;
  }

  small_vector_impl(const size_type count, const value_t &value)
      : small_vector_impl() {
    const auto status = assign(count, value);
    (void)status;
  }

  template <std::input_iterator input_it>
  small_vector_impl(input_it first, input_it last) : small_vector_impl() {
    const auto status = assign(first, last);
    (void)status;
  }

  small_vector_impl(const size_type count, const value_t &value,
                    const allocator_type &allocator)
      : small_vector_impl(allocator) {
    const auto status = assign(count, value);
    (void)status;
  }

  explicit small_vector_impl(const size_type count,
                             const allocator_type &allocator)
    requires std::default_initializable<value_t>
      : small_vector_impl(allocator) {
    auto status = reserve(count);
    if (status.has_error()) {
      return;
    }

    status = append_value_initialized_n(static_cast<std::size_t>(count));
    (void)status;
  }

  small_vector_impl(const size_type count, default_init_t,
                    const allocator_type &allocator)
    requires std::default_initializable<value_t>
      : small_vector_impl(allocator) {
    auto status = reserve(count);
    if (status.has_error()) {
      return;
    }

    status = append_default_n(static_cast<std::size_t>(count));
    (void)status;
  }

  template <std::input_iterator input_it>
  small_vector_impl(input_it first, input_it last,
                    const allocator_type &allocator)
      : small_vector_impl(allocator) {
    const auto status = assign(first, last);
    (void)status;
  }

  small_vector_impl(std::initializer_list<value_t> values,
                    const allocator_type &allocator)
      : small_vector_impl(allocator) {
    if (reserve(static_cast<size_type>(values.size())).has_error()) {
      return;
    }

    const auto appended = append_copy_range(values.begin(), values.end());
    (void)appended;
  }

  small_vector_impl(std::initializer_list<value_t> values)
      : small_vector_impl() {
    if (reserve(static_cast<size_type>(values.size())).has_error()) {
      return;
    }

    const auto appended = append_copy_range(values.begin(), values.end());
    (void)appended;
  }

  small_vector_impl(const small_vector_impl &other)
      : data_(inline_data()), size_(0U), capacity_(inline_capacity),
        allocator_(
            std::allocator_traits<allocator_type>::
                select_on_container_copy_construction(other.allocator_)) {
    if (reserve(other.size()).has_error()) {
      return;
    }

    const auto appended = append_copy_range(other.begin(), other.end());
    (void)appended;
  }

  small_vector_impl(const small_vector_impl &other,
                    const allocator_type &allocator)
      : data_(inline_data()), size_(0U), capacity_(inline_capacity),
        allocator_(allocator) {
    if (reserve(other.size()).has_error()) {
      return;
    }

    const auto appended = append_copy_range(other.begin(), other.end());
    (void)appended;
  }

  small_vector_impl(small_vector_impl &&other)
      : data_(inline_data()), size_(0U), capacity_(inline_capacity),
        allocator_(std::move(other.allocator_)) {
    move_from(std::move(other));
  }

  small_vector_impl(small_vector_impl &&other, const allocator_type &allocator)
      : data_(inline_data()), size_(0U), capacity_(inline_capacity),
        allocator_(allocator) {
    if constexpr (std::allocator_traits<
                      allocator_type>::is_always_equal::value) {
      move_from(std::move(other));
      return;
    }

    if (allocator_equals(other.allocator_)) {
      move_from(std::move(other));
      return;
    }

    if (reserve(other.size()).has_error()) {
      return;
    }

    const auto appended = append_move_range(other.begin(), other.end());
    (void)appended;
    other.clear();
  }

  auto operator=(const small_vector_impl &other) -> small_vector_impl & {
    if (this == &other) {
      return *this;
    }

    if constexpr (std::allocator_traits<allocator_type>::
                      propagate_on_container_copy_assignment::value) {
      if (!using_inline_storage()) {
        clear();
        release_heap_if_needed();
        reset_to_inline();
      } else {
        clear();
      }
      allocator_ = other.allocator_;
    } else {
      clear();
    }

    if (reserve(other.size()).has_error()) {
      return *this;
    }

    const auto appended = append_copy_range(other.begin(), other.end());
    (void)appended;
    return *this;
  }

  auto operator=(small_vector_impl &&other) -> small_vector_impl & {
    if (this == &other) {
      return *this;
    }

    clear();
    release_heap_if_needed();
    reset_to_inline();

    if constexpr (std::allocator_traits<allocator_type>::
                      propagate_on_container_move_assignment::value) {
      allocator_ = std::move(other.allocator_);
      move_from(std::move(other));
      return *this;
    }

    if constexpr (std::allocator_traits<
                      allocator_type>::is_always_equal::value) {
      move_from(std::move(other));
      return *this;
    }

    if (allocator_equals(other.allocator_)) {
      move_from(std::move(other));
      return *this;
    }

    if (reserve(other.size()).has_error()) {
      return *this;
    }

    const auto appended = append_move_range(other.begin(), other.end());
    (void)appended;
    other.clear();
    return *this;
  }

  ~small_vector_impl() {
    clear();
    release_heap_if_needed();
  }

  [[nodiscard]] auto get_allocator() const noexcept -> allocator_type {
    return allocator_;
  }

  [[nodiscard]] auto empty() const noexcept -> bool { return size_ == 0U; }

  [[nodiscard]] auto size() const noexcept -> size_type {
    return static_cast<size_type>(size_);
  }

  [[nodiscard]] auto capacity() const noexcept -> size_type {
    return static_cast<size_type>(capacity_);
  }

  [[nodiscard]] auto max_size() const noexcept -> size_type {
    const auto allocator_max =
        std::allocator_traits<allocator_type>::max_size(allocator_);
    const auto type_max =
        static_cast<std::size_t>((std::numeric_limits<size_type>::max)());
    return static_cast<size_type>((std::min)(allocator_max, type_max));
  }

  [[nodiscard]] auto data() noexcept -> pointer { return data_; }

  [[nodiscard]] auto data() const noexcept -> const_pointer { return data_; }

  [[nodiscard]] auto using_inline_storage() const noexcept -> bool {
    return data_ == inline_data();
  }

  [[nodiscard]] auto internal_storage() noexcept -> pointer {
    return inline_data();
  }

  [[nodiscard]] auto internal_storage() const noexcept -> const_pointer {
    return inline_data();
  }

  [[nodiscard]] auto
  storage_is_unpropagable(const_pointer pointer_value) const noexcept -> bool {
    return pointer_value == internal_storage();
  }

  [[nodiscard]] auto is_small() const noexcept -> bool {
    return using_inline_storage();
  }

  [[nodiscard]] auto operator[](const size_type index) noexcept -> reference {
    return data_[index];
  }

  [[nodiscard]] auto operator[](const size_type index) const noexcept
      -> const_reference {
    return data_[index];
  }

  [[nodiscard]] auto at(const size_type index) noexcept -> result<reference> {
    if (index >= size()) {
      return result<reference>::failure(errc::invalid_argument);
    }

    return data_[index];
  }

  [[nodiscard]] auto at(const size_type index) const noexcept
      -> result<const_reference> {
    if (index >= size()) {
      return result<const_reference>::failure(errc::invalid_argument);
    }

    return data_[index];
  }

  [[nodiscard]] auto front() noexcept -> reference { return data_[0]; }

  [[nodiscard]] auto front() const noexcept -> const_reference {
    return data_[0];
  }

  [[nodiscard]] auto back() noexcept -> reference { return data_[size_ - 1U]; }

  [[nodiscard]] auto back() const noexcept -> const_reference {
    return data_[size_ - 1U];
  }

  [[nodiscard]] auto begin() noexcept -> iterator { return data_; }

  [[nodiscard]] auto begin() const noexcept -> const_iterator { return data_; }

  [[nodiscard]] auto cbegin() const noexcept -> const_iterator { return data_; }

  [[nodiscard]] auto end() noexcept -> iterator { return data_ + size_; }

  [[nodiscard]] auto end() const noexcept -> const_iterator {
    return data_ + size_;
  }

  [[nodiscard]] auto cend() const noexcept -> const_iterator {
    return data_ + size_;
  }

  [[nodiscard]] auto rbegin() noexcept -> reverse_iterator {
    return reverse_iterator(end());
  }

  [[nodiscard]] auto rbegin() const noexcept -> const_reverse_iterator {
    return const_reverse_iterator(end());
  }

  [[nodiscard]] auto crbegin() const noexcept -> const_reverse_iterator {
    return const_reverse_iterator(cend());
  }

  [[nodiscard]] auto rend() noexcept -> reverse_iterator {
    return reverse_iterator(begin());
  }

  [[nodiscard]] auto rend() const noexcept -> const_reverse_iterator {
    return const_reverse_iterator(begin());
  }

  [[nodiscard]] auto crend() const noexcept -> const_reverse_iterator {
    return const_reverse_iterator(cbegin());
  }

  [[nodiscard]] static constexpr auto inline_capacity_value() noexcept
      -> size_type {
    return inline_capacity;
  }

  [[nodiscard]] static constexpr auto internal_capacity() noexcept
      -> size_type {
    return inline_capacity;
  }

  void clear() noexcept {
    if constexpr (!std::is_trivially_destructible_v<value_t>) {
      std::destroy_n(data_, size_);
    }
    size_ = 0U;
  }

  [[nodiscard]] auto assign(const size_type count, const value_t &value)
      -> result<void> {
    const std::size_t count_size_t = static_cast<std::size_t>(count);
    if (count_size_t > max_size()) {
      return result<void>::failure(errc::resource_exhausted);
    }

    clear();
    if (count_size_t == 0U) {
      return result<void>::success();
    }

    auto reserved = reserve(count);
    if (reserved.has_error()) {
      return reserved.error();
    }

    return append_fill_n(count_size_t, value);
  }

  template <std::input_iterator input_it>
  [[nodiscard]] auto assign(input_it first, input_it last) -> result<void> {
    if constexpr (std::forward_iterator<input_it>) {
      const auto count = static_cast<std::size_t>(std::distance(first, last));
      if (count > max_size()) {
        return result<void>::failure(errc::resource_exhausted);
      }

      bool overlaps_self = false;
      if constexpr (std::same_as<input_it, iterator> ||
                    std::same_as<input_it, const_iterator>) {
        overlaps_self = (first >= cbegin() && first <= cend()) &&
                        (last >= cbegin() && last <= cend());
      }

      if (overlaps_self) {
        small_vector_impl staging(allocator_);
        auto stage_reserved = staging.reserve(static_cast<size_type>(count));
        if (stage_reserved.has_error()) {
          return stage_reserved.error();
        }

        auto staged = staging.append_copy_range(first, last);
        if (staged.has_error()) {
          return staged.error();
        }

        clear();
        auto reserved = reserve(static_cast<size_type>(count));
        if (reserved.has_error()) {
          return reserved.error();
        }

        return append_move_range(staging.begin(), staging.end());
      }

      clear();
      auto reserved = reserve(static_cast<size_type>(count));
      if (reserved.has_error()) {
        return reserved.error();
      }

      return append_copy_range(first, last);
    }

    clear();
    for (; first != last; ++first) {
      auto appended = emplace_back(*first);
      if (appended.has_error()) {
        return appended.error();
      }
    }

    return result<void>::success();
  }

  [[nodiscard]] auto assign(std::initializer_list<value_t> values)
      -> result<void> {
    return assign(values.begin(), values.end());
  }

  [[nodiscard]] auto resize(const size_type requested_size) -> result<void>
    requires std::default_initializable<value_t>
  {
    const std::size_t new_size = static_cast<std::size_t>(requested_size);
    if (new_size > max_size()) {
      return result<void>::failure(errc::resource_exhausted);
    }

    if (new_size <= size_) {
      if constexpr (!std::is_trivially_destructible_v<value_t>) {
        std::destroy_n(data_ + new_size, size_ - new_size);
      }
      size_ = new_size;
      return result<void>::success();
    }

    auto reserved = reserve(requested_size);
    if (reserved.has_error()) {
      return reserved.error();
    }

    return append_value_initialized_n(new_size - size_);
  }

  [[nodiscard]] auto resize(const size_type requested_size, default_init_t)
      -> result<void>
    requires std::default_initializable<value_t>
  {
    const std::size_t new_size = static_cast<std::size_t>(requested_size);
    if (new_size > max_size()) {
      return result<void>::failure(errc::resource_exhausted);
    }

    if (new_size <= size_) {
      if constexpr (!std::is_trivially_destructible_v<value_t>) {
        std::destroy_n(data_ + new_size, size_ - new_size);
      }
      size_ = new_size;
      return result<void>::success();
    }

    auto reserved = reserve(requested_size);
    if (reserved.has_error()) {
      return reserved.error();
    }

    return append_default_n(new_size - size_);
  }

  [[nodiscard]] auto resize(const size_type requested_size,
                            const value_t &value) -> result<void> {
    const std::size_t new_size = static_cast<std::size_t>(requested_size);
    if (new_size > max_size()) {
      return result<void>::failure(errc::resource_exhausted);
    }

    if (new_size <= size_) {
      if constexpr (!std::is_trivially_destructible_v<value_t>) {
        std::destroy_n(data_ + new_size, size_ - new_size);
      }
      size_ = new_size;
      return result<void>::success();
    }

    auto reserved = reserve(requested_size);
    if (reserved.has_error()) {
      return reserved.error();
    }

    return append_fill_n(new_size - size_, value);
  }

  [[nodiscard]] auto swap(small_vector_impl &other) -> result<void> {
    if (this == &other) {
      return result<void>::success();
    }

    if constexpr (!std::allocator_traits<
                      allocator_type>::propagate_on_container_swap::value &&
                  !std::allocator_traits<
                      allocator_type>::is_always_equal::value) {
      if (!allocator_equals(other.allocator_)) {
        return result<void>::failure(errc::contract_violation);
      }
    }

    if constexpr (std::allocator_traits<
                      allocator_type>::propagate_on_container_swap::value) {
      std::swap(allocator_, other.allocator_);
    }

    const bool this_inline = using_inline_storage();
    const bool other_inline = other.using_inline_storage();

    if (!this_inline && !other_inline) {
      std::swap(data_, other.data_);
      std::swap(size_, other.size_);
      std::swap(capacity_, other.capacity_);
      return result<void>::success();
    }

    if (this_inline && !other_inline) {
      pointer other_heap = other.data_;
      const std::size_t other_size = other.size_;
      const std::size_t other_capacity = other.capacity_;

      auto moved = relocate_into(data_, size_, other.inline_data());
      if (moved.has_error()) {
        return moved.error();
      }

      std::destroy_n(data_, size_);

      other.data_ = other.inline_data();
      other.capacity_ = inline_capacity;
      other.size_ = size_;

      data_ = other_heap;
      size_ = other_size;
      capacity_ = other_capacity;
      return result<void>::success();
    }

    if (!this_inline && other_inline) {
      pointer this_heap = data_;
      const std::size_t this_size = size_;
      const std::size_t this_capacity = capacity_;

      auto moved = relocate_into(other.data_, other.size_, inline_data());
      if (moved.has_error()) {
        return moved.error();
      }

      std::destroy_n(other.data_, other.size_);

      data_ = inline_data();
      capacity_ = inline_capacity;
      size_ = other.size_;

      other.data_ = this_heap;
      other.size_ = this_size;
      other.capacity_ = this_capacity;
      return result<void>::success();
    }

    if constexpr (std::is_nothrow_swappable_v<value_t> &&
                  std::is_nothrow_move_constructible_v<value_t>) {
      return swap_inline_nothrow(other);
    }

    small_vector_impl this_snapshot(allocator_);
    small_vector_impl other_snapshot(other.allocator_);
    auto this_snapshot_status = this_snapshot.assign(begin(), end());
    if (this_snapshot_status.has_error()) {
      return this_snapshot_status.error();
    }

    auto other_snapshot_status =
        other_snapshot.assign(other.begin(), other.end());
    if (other_snapshot_status.has_error()) {
      return other_snapshot_status.error();
    }

    auto this_assign = assign(other_snapshot.begin(), other_snapshot.end());
    if (this_assign.has_error()) {
      return this_assign.error();
    }

    auto other_assign =
        other.assign(this_snapshot.begin(), this_snapshot.end());
    if (other_assign.has_error()) {
      auto restore = assign(this_snapshot.begin(), this_snapshot.end());
      if (restore.has_error()) {
        return restore.error();
      }
      return other_assign.error();
    }

    return result<void>::success();
  }

  [[nodiscard]] auto reserve(const size_type requested_capacity)
      -> result<void> {
    const std::size_t requested = static_cast<std::size_t>(requested_capacity);

    if (requested <= capacity_) {
      return result<void>::success();
    }

    if (requested > max_size()) {
      return result<void>::failure(errc::resource_exhausted);
    }

    if constexpr (!options_type::heap_enabled) {
      return result<void>::failure(errc::resource_exhausted);
    }

    auto grown = reallocate(requested);
    if (grown.has_error()) {
      return grown.error();
    }

    return result<void>::success();
  }

  [[nodiscard]] auto shrink_to_fit() -> result<void> {
    if (size_ == capacity_) {
      return result<void>::success();
    }

    if constexpr (options_type::shrink_to_inline) {
      if (size_ <= inline_capacity && !using_inline_storage()) {
        pointer inline_target = inline_data();
        auto moved = relocate_into(data_, size_, inline_target);
        if (moved.has_error()) {
          return moved.error();
        }
        std::destroy_n(data_, size_);
        std::allocator_traits<allocator_type>::deallocate(allocator_, data_,
                                                          capacity_);
        data_ = inline_target;
        capacity_ = inline_capacity;
        return result<void>::success();
      }
    }

    if (size_ > inline_capacity && size_ < capacity_) {
      return reallocate(size_);
    }

    return result<void>::success();
  }

  [[nodiscard]] auto push_back(const value_t &value) noexcept -> result<void> {
    return append_one(value);
  }

  [[nodiscard]] auto push_back(value_t &&value) noexcept -> result<void> {
    return append_one(std::move(value));
  }

  template <typename... args_t>
  [[nodiscard]] auto emplace_back(args_t &&...args) -> result<reference> {
    auto appended = append_one(std::forward<args_t>(args)...);
    if (appended.has_error()) {
      return result<reference>::failure(appended.error());
    }

    return data_[size_ - 1U];
  }

  [[nodiscard]] auto insert(const_iterator position, const value_t &value)
      -> result<iterator> {
    return emplace(position, value);
  }

  [[nodiscard]] auto insert(const_iterator position, value_t &&value)
      -> result<iterator> {
    return emplace(position, std::move(value));
  }

  [[nodiscard]] auto insert(const_iterator position, const size_type count,
                            const value_t &value) -> result<iterator> {
    const std::size_t index = static_cast<std::size_t>(position - cbegin());
    if (index > size_) {
      return result<iterator>::failure(errc::invalid_argument);
    }

    if (count == 0U) {
      return data_ + index;
    }

    const std::size_t count_size_t = static_cast<std::size_t>(count);
    if (count_size_t > max_size() - size_) {
      return result<iterator>::failure(errc::resource_exhausted);
    }

    if constexpr (std::is_nothrow_copy_constructible_v<value_t>) {
      value_t value_copy(value);
      return insert_fill_n(index, count_size_t, value_copy);
    } else {
      try {
        value_t value_copy(value);
        return insert_fill_n(index, count_size_t, value_copy);
      } catch (...) {
        return result<iterator>::failure(errc::internal_error);
      }
    }
  }

  template <std::input_iterator input_it>
  [[nodiscard]] auto insert(const_iterator position, input_it first,
                            input_it last) -> result<iterator> {
    const std::size_t index = static_cast<std::size_t>(position - cbegin());
    if (index > size_) {
      return result<iterator>::failure(errc::invalid_argument);
    }

    if (first == last) {
      return data_ + index;
    }

    if constexpr (std::forward_iterator<input_it>) {
      const auto count = static_cast<std::size_t>(std::distance(first, last));
      if (count > max_size() - size_) {
        return result<iterator>::failure(errc::resource_exhausted);
      }

      try {
        return insert_forward_range(index, first, last, count);
      } catch (...) {
        return result<iterator>::failure(errc::internal_error);
      }
    }

    std::size_t inserted = 0U;
    for (; first != last; ++first, ++inserted) {
      auto inserted_it = emplace(
          cbegin() + static_cast<difference_type>(index + inserted), *first);
      if (inserted_it.has_error()) {
        return inserted_it.error();
      }
    }

    return data_ + index;
  }

  [[nodiscard]] auto insert(const_iterator position,
                            std::initializer_list<value_t> values)
      -> result<iterator> {
    return insert(position, values.begin(), values.end());
  }

  template <typename... args_t>
  [[nodiscard]] auto emplace(const_iterator position, args_t &&...args)
      -> result<iterator> {
    const std::size_t index = static_cast<std::size_t>(position - cbegin());
    if (index > size_) {
      return result<iterator>::failure(errc::invalid_argument);
    }

    if (index == size_) {
      auto appended = append_one(std::forward<args_t>(args)...);
      if (appended.has_error()) {
        return result<iterator>::failure(appended.error());
      }

      return data_ + index;
    }

    try {
      auto inserted_value = value_t(std::forward<args_t>(args)...);

      auto growth = ensure_capacity_for(1U);
      if (growth.has_error()) {
        return result<iterator>::failure(growth.error());
      }

      if (shift_right_one(index).has_error()) {
        return result<iterator>::failure(errc::internal_error);
      }
      data_[index] = std::move(inserted_value);
      return data_ + index;
    } catch (...) {
      return result<iterator>::failure(errc::internal_error);
    }
  }

  auto erase(const_iterator position) -> iterator {
    const auto index_offset = position - cbegin();
    if (index_offset < 0) {
      return end();
    }

    const std::size_t index = static_cast<std::size_t>(index_offset);
    if (index >= size_) {
      return end();
    }

    shift_left_one(index);
    return data_ + index;
  }

  auto erase(const_iterator first, const_iterator last) -> iterator {
    const auto first_offset = first - cbegin();
    const auto last_offset = last - cbegin();

    if (first_offset < 0) {
      return end();
    }

    const std::size_t first_index = static_cast<std::size_t>(first_offset);
    if (first_index >= size_) {
      return end();
    }

    std::size_t last_index = first_index;
    if (last_offset > first_offset) {
      last_index = static_cast<std::size_t>(last_offset);
      if (last_index > size_) {
        last_index = size_;
      }
    }

    if (last_index == first_index) {
      return data_ + first_index;
    }

    const std::size_t erase_count = last_index - first_index;
    const std::size_t move_count = size_ - last_index;

    if constexpr (std::is_trivially_copyable_v<value_t>) {
      if (move_count > 0U) {
        std::memmove(data_ + first_index, data_ + last_index,
                     sizeof(value_t) * move_count);
      }
    } else {
      std::move(data_ + last_index, data_ + size_, data_ + first_index);
    }

    const std::size_t new_size = size_ - erase_count;
    if constexpr (!std::is_trivially_destructible_v<value_t>) {
      std::destroy_n(data_ + new_size, erase_count);
    }
    size_ = new_size;
    return data_ + first_index;
  }

  void pop_back() {
    if (size_ == 0U) {
      return;
    }

    --size_;
    if constexpr (!std::is_trivially_destructible_v<value_t>) {
      std::destroy_at(data_ + size_);
    }
  }

  [[nodiscard]] auto to_std_vector() const -> std::vector<value_t> {
    return std::vector<value_t>(data_, data_ + size_);
  }

  [[nodiscard]] static auto
  from_std_vector_impl(const std::vector<value_t> &values)
      -> result<small_vector_impl> {
    small_vector_impl output;
    const std::size_t count = values.size();

    if (output.reserve(static_cast<size_type>(count)).has_error()) {
      return result<small_vector_impl>::failure(errc::resource_exhausted);
    }

    if (count == 0U) {
      return output;
    }

    if constexpr (std::is_trivially_copyable_v<value_t>) {
      std::memcpy(output.data_, values.data(), sizeof(value_t) * count);
      output.size_ = count;
      return output;
    }

    auto appended = output.append_copy_range(values.begin(), values.end());
    if (appended.has_error()) {
      return result<small_vector_impl>::failure(appended.error());
    }

    return output;
  }

private:
  static constexpr bool should_trivially_relocate =
      std::is_trivially_copyable_v<value_t> &&
      std::is_move_constructible_v<value_t>;

  alignas(value_t)
      std::array<std::byte, sizeof(value_t) * inline_capacity> inline_buffer_{};
  pointer data_;
  std::size_t size_;
  std::size_t capacity_;
  wh_no_unique_address allocator_type allocator_{};

  [[nodiscard]] auto allocator_equals(const allocator_type &other) const
      -> bool {
    if constexpr (requires(const allocator_type &lhs,
                           const allocator_type &rhs) {
                    { lhs == rhs } -> std::convertible_to<bool>;
                  }) {
      return allocator_ == other;
    }

    return true;
  }

  [[nodiscard]] auto inline_data() noexcept -> pointer {
    return std::launder(reinterpret_cast<pointer>(inline_buffer_.data()));
  }

  [[nodiscard]] auto inline_data() const noexcept -> const_pointer {
    return std::launder(reinterpret_cast<const_pointer>(inline_buffer_.data()));
  }

  void reset_to_inline() noexcept {
    data_ = inline_data();
    size_ = 0U;
    capacity_ = inline_capacity;
  }

  [[nodiscard]] auto shift_right_one(const std::size_t index) -> result<void> {
    if constexpr (std::is_trivially_copyable_v<value_t>) {
      const std::size_t tail_count = size_ - index;
      std::memmove(data_ + index + 1U, data_ + index,
                   sizeof(value_t) * tail_count);
      ++size_;
      return result<void>::success();
    }

    if constexpr (std::is_nothrow_move_constructible_v<value_t> &&
                  std::is_nothrow_move_assignable_v<value_t>) {
      std::construct_at(data_ + size_, std::move(data_[size_ - 1U]));
      std::move_backward(data_ + index, data_ + size_ - 1U, data_ + size_);
    } else {
      try {
        std::construct_at(data_ + size_,
                          std::move_if_noexcept(data_[size_ - 1U]));
      } catch (...) {
        return result<void>::failure(errc::internal_error);
      }

      try {
        std::move_backward(data_ + index, data_ + size_ - 1U, data_ + size_);
      } catch (...) {
        std::destroy_at(data_ + size_);
        return result<void>::failure(errc::internal_error);
      }
    }

    ++size_;
    return result<void>::success();
  }

  void shift_left_one(const std::size_t index) {
    if constexpr (std::is_trivially_copyable_v<value_t>) {
      const std::size_t move_count = size_ - index - 1U;
      if (move_count > 0U) {
        std::memmove(data_ + index, data_ + index + 1U,
                     sizeof(value_t) * move_count);
      }
    } else {
      std::move(data_ + index + 1U, data_ + size_, data_ + index);
    }

    --size_;
    if constexpr (!std::is_trivially_destructible_v<value_t>) {
      std::destroy_at(data_ + size_);
    }
  }

  auto insert_fill_n(const std::size_t index, const std::size_t count,
                     const value_t &value) -> result<iterator> {
    if (size_ + count > capacity_) {
      const std::size_t new_capacity = next_capacity(size_ + count);
      if (new_capacity <= capacity_) {
        return result<iterator>::failure(errc::resource_exhausted);
      }
      pointer new_data = nullptr;
      try {
        new_data = std::allocator_traits<allocator_type>::allocate(
            allocator_, new_capacity);
      } catch (...) {
        return result<iterator>::failure(errc::resource_exhausted);
      }

      if constexpr (std::is_trivially_copyable_v<value_t>) {
        if (index > 0U) {
          std::memcpy(new_data, data_, sizeof(value_t) * index);
        }

        for (std::size_t item = 0U; item < count; ++item) {
          std::construct_at(new_data + index + item, value);
        }

        const std::size_t suffix_count = size_ - index;
        if (suffix_count > 0U) {
          std::memcpy(new_data + index + count, data_ + index,
                      sizeof(value_t) * suffix_count);
        }
      } else {
        pointer constructed_end = new_data;

        try {
          if constexpr (std::is_nothrow_move_constructible_v<value_t> ||
                        !std::is_copy_constructible_v<value_t>) {
            constructed_end =
                std::uninitialized_move_n(data_, index, constructed_end).second;
            constructed_end =
                std::uninitialized_move_n(
                    data_ + index, size_ - index,
                    std::uninitialized_fill_n(constructed_end, count, value))
                    .second;
          } else {
            constructed_end =
                std::uninitialized_copy_n(data_, index, constructed_end);
            constructed_end = std::uninitialized_copy_n(
                data_ + index, size_ - index,
                std::uninitialized_fill_n(constructed_end, count, value));
          }
        } catch (...) {
          std::destroy(new_data, constructed_end);
          std::allocator_traits<allocator_type>::deallocate(
              allocator_, new_data, new_capacity);
          return result<iterator>::failure(errc::internal_error);
        }
      }

      if constexpr (!std::is_trivially_destructible_v<value_t>) {
        std::destroy_n(data_, size_);
      }
      release_heap_if_needed();

      data_ = new_data;
      size_ += count;
      capacity_ = new_capacity;
      return data_ + index;
    }

    const std::size_t old_size = size_;
    const std::size_t tail_count = old_size - index;

    if (index == old_size) {
      auto appended = append_fill_n(count, value);
      if (appended.has_error()) {
        return result<iterator>::failure(appended.error());
      }

      return data_ + index;
    }

    if constexpr (std::is_trivially_copyable_v<value_t>) {
      std::memmove(data_ + index + count, data_ + index,
                   sizeof(value_t) * tail_count);

      const std::size_t overlap_count = (std::min)(count, tail_count);
      std::fill_n(data_ + index, overlap_count, value);
      for (std::size_t item = overlap_count; item < count; ++item) {
        std::construct_at(data_ + index + item, value);
      }

      size_ = old_size + count;
      return data_ + index;
    }

    if (count <= tail_count) {
      try {
        if constexpr (std::is_nothrow_move_constructible_v<value_t> ||
                      !std::is_copy_constructible_v<value_t>) {
          (void)std::uninitialized_move_n(data_ + old_size - count, count,
                                          data_ + old_size);
        } else {
          (void)std::uninitialized_copy_n(data_ + old_size - count, count,
                                          data_ + old_size);
        }
      } catch (...) {
        return result<iterator>::failure(errc::internal_error);
      }

      try {
        std::move_backward(data_ + index, data_ + old_size - count,
                           data_ + old_size);
        std::fill_n(data_ + index, count, value);
      } catch (...) {
        std::destroy_n(data_ + old_size, count);
        return result<iterator>::failure(errc::internal_error);
      }

      size_ = old_size + count;
      return data_ + index;
    }

    const std::size_t appended_inserted = count - tail_count;

    try {
      (void)std::uninitialized_fill_n(data_ + old_size, appended_inserted,
                                      value);
    } catch (...) {
      return result<iterator>::failure(errc::internal_error);
    }

    try {
      if constexpr (std::is_nothrow_move_constructible_v<value_t> ||
                    !std::is_copy_constructible_v<value_t>) {
        (void)std::uninitialized_move_n(data_ + index, tail_count,
                                        data_ + old_size + appended_inserted);
      } else {
        (void)std::uninitialized_copy_n(data_ + index, tail_count,
                                        data_ + old_size + appended_inserted);
      }
    } catch (...) {
      std::destroy_n(data_ + old_size, appended_inserted);
      return result<iterator>::failure(errc::internal_error);
    }

    try {
      std::fill(data_ + index, data_ + old_size, value);
    } catch (...) {
      std::destroy_n(data_ + old_size, count);
      return result<iterator>::failure(errc::internal_error);
    }

    size_ = old_size + count;
    return data_ + index;
  }

  template <std::forward_iterator forward_it>
  auto insert_forward_range(const std::size_t index, forward_it first,
                            forward_it last, const std::size_t count)
      -> result<iterator> {
    if (count == 0U) {
      return data_ + index;
    }

    if (size_ + count > capacity_) {
      const std::size_t new_capacity = next_capacity(size_ + count);
      if (new_capacity <= capacity_) {
        return result<iterator>::failure(errc::resource_exhausted);
      }
      pointer new_data = nullptr;
      try {
        new_data = std::allocator_traits<allocator_type>::allocate(
            allocator_, new_capacity);
      } catch (...) {
        return result<iterator>::failure(errc::resource_exhausted);
      }

      if constexpr (std::contiguous_iterator<forward_it> &&
                    std::is_trivially_copyable_v<value_t> &&
                    std::same_as<
                        std::remove_cv_t<std::iter_value_t<forward_it>>,
                        value_t>) {
        if (index > 0U) {
          std::memcpy(new_data, data_, sizeof(value_t) * index);
        }

        std::memcpy(new_data + index, std::to_address(first),
                    sizeof(value_t) * count);

        const std::size_t suffix_count = size_ - index;
        if (suffix_count > 0U) {
          std::memcpy(new_data + index + count, data_ + index,
                      sizeof(value_t) * suffix_count);
        }
      } else {
        pointer constructed_end = new_data;

        try {
          if constexpr (std::is_nothrow_move_constructible_v<value_t> ||
                        !std::is_copy_constructible_v<value_t>) {
            constructed_end =
                std::uninitialized_move_n(data_, index, constructed_end).second;
            constructed_end =
                std::uninitialized_copy(first, last, constructed_end);
            constructed_end = std::uninitialized_move_n(
                                  data_ + index, size_ - index, constructed_end)
                                  .second;
          } else {
            constructed_end =
                std::uninitialized_copy_n(data_, index, constructed_end);
            constructed_end =
                std::uninitialized_copy(first, last, constructed_end);
            constructed_end = std::uninitialized_copy_n(
                data_ + index, size_ - index, constructed_end);
          }
        } catch (...) {
          std::destroy(new_data, constructed_end);
          std::allocator_traits<allocator_type>::deallocate(
              allocator_, new_data, new_capacity);
          return result<iterator>::failure(errc::internal_error);
        }
      }

      if constexpr (!std::is_trivially_destructible_v<value_t>) {
        std::destroy_n(data_, size_);
      }
      release_heap_if_needed();

      data_ = new_data;
      size_ += count;
      capacity_ = new_capacity;
      return data_ + index;
    }

    const std::size_t old_size = size_;
    const std::size_t tail_count = old_size - index;

    if (index == old_size) {
      auto appended = append_copy_range(first, last);
      if (appended.has_error()) {
        return result<iterator>::failure(appended.error());
      }

      return data_ + index;
    }

    if constexpr (std::contiguous_iterator<forward_it> &&
                  std::is_trivially_copyable_v<value_t> &&
                  std::same_as<std::remove_cv_t<std::iter_value_t<forward_it>>,
                               value_t>) {
      const value_t *const source_begin = std::to_address(first);
      const value_t *const source_end = source_begin + count;
      const value_t *const storage_begin = data_;
      const value_t *const storage_end = data_ + old_size;
      const bool source_overlaps_storage =
          source_begin < storage_end && source_end > storage_begin;

      if (!source_overlaps_storage) {
        std::memmove(data_ + index + count, data_ + index,
                     sizeof(value_t) * tail_count);
        std::memcpy(data_ + index, source_begin, sizeof(value_t) * count);
        size_ = old_size + count;
        return data_ + index;
      }
    }

    if (count <= tail_count) {
      try {
        if constexpr (std::is_nothrow_move_constructible_v<value_t> ||
                      !std::is_copy_constructible_v<value_t>) {
          (void)std::uninitialized_move_n(data_ + old_size - count, count,
                                          data_ + old_size);
        } else {
          (void)std::uninitialized_copy_n(data_ + old_size - count, count,
                                          data_ + old_size);
        }
      } catch (...) {
        return result<iterator>::failure(errc::internal_error);
      }

      try {
        std::move_backward(data_ + index, data_ + old_size - count,
                           data_ + old_size);
        (void)std::copy_n(first, count, data_ + index);
      } catch (...) {
        std::destroy_n(data_ + old_size, count);
        return result<iterator>::failure(errc::internal_error);
      }

      size_ = old_size + count;
      return data_ + index;
    }

    const std::size_t appended_inserted = count - tail_count;
    auto split = first;
    std::advance(split, static_cast<difference_type>(tail_count));

    pointer constructed_end = data_ + old_size;
    try {
      constructed_end = std::uninitialized_copy(split, last, data_ + old_size);
    } catch (...) {
      return result<iterator>::failure(errc::internal_error);
    }

    try {
      if constexpr (std::is_nothrow_move_constructible_v<value_t> ||
                    !std::is_copy_constructible_v<value_t>) {
        constructed_end =
            std::uninitialized_move_n(data_ + index, tail_count,
                                      data_ + old_size + appended_inserted)
                .second;
      } else {
        constructed_end = std::uninitialized_copy_n(
            data_ + index, tail_count, data_ + old_size + appended_inserted);
      }
    } catch (...) {
      std::destroy(data_ + old_size, data_ + old_size + appended_inserted);
      return result<iterator>::failure(errc::internal_error);
    }

    try {
      (void)std::copy_n(first, tail_count, data_ + index);
    } catch (...) {
      std::destroy(data_ + old_size, constructed_end);
      return result<iterator>::failure(errc::internal_error);
    }

    size_ = old_size + count;
    return data_ + index;
  }

  void release_heap_if_needed() noexcept {
    if (!using_inline_storage()) {
      std::allocator_traits<allocator_type>::deallocate(allocator_, data_,
                                                        capacity_);
    }
  }

  [[nodiscard]] auto next_capacity(const std::size_t required) const
      -> std::size_t {
    if constexpr (!options_type::heap_enabled) {
      return required;
    }

    const std::size_t minimum_dynamic_capacity =
        (std::max)(inline_capacity, options_type::minimum_dynamic_capacity);
    const std::size_t growth_floor = capacity_ < minimum_dynamic_capacity
                                         ? minimum_dynamic_capacity
                                         : capacity_;
    const std::size_t max_capacity = static_cast<std::size_t>(max_size());

    if (growth_floor >= max_capacity) {
      return max_capacity;
    }

    const std::size_t remaining_capacity = max_capacity - growth_floor;
    const std::size_t min_additional_capacity =
        required > growth_floor ? (required - growth_floor) : 0U;

    if (remaining_capacity < min_additional_capacity) {
      return max_capacity;
    }

    const std::size_t grown =
        grow_capacity_with_ratio(growth_floor, max_capacity);

    const std::size_t clamped_grown = (std::min)(max_capacity, grown);
    const std::size_t requested_floor = (std::max)(required, growth_floor);

    return (std::max)(requested_floor, clamped_grown);
  }

  [[nodiscard]] auto ensure_capacity_for(const std::size_t additional)
      -> result<void> {
    if (additional <= (capacity_ - size_))
      wh_likely { return result<void>::success(); }

    const std::size_t max_items = static_cast<std::size_t>(max_size());
    if (size_ > max_items || additional > (max_items - size_)) {
      return result<void>::failure(errc::resource_exhausted);
    }

    const std::size_t required = size_ + additional;

    const std::size_t target_capacity = next_capacity(required);
    if (target_capacity < required || target_capacity <= capacity_) {
      return result<void>::failure(errc::resource_exhausted);
    }

    return reserve(static_cast<size_type>(target_capacity));
  }

  [[nodiscard]] static auto relocate_into(pointer source,
                                          const std::size_t count,
                                          pointer destination) -> result<void> {
    if constexpr (should_trivially_relocate) {
      std::memcpy(destination, source, sizeof(value_t) * count);
      return result<void>::success();
    }

    std::size_t constructed = 0U;
    try {
      for (; constructed < count; ++constructed) {
        std::construct_at(destination + constructed,
                          std::move_if_noexcept(source[constructed]));
      }
    } catch (...) {
      std::destroy_n(destination, constructed);
      return result<void>::failure(errc::internal_error);
    }

    return result<void>::success();
  }

  [[nodiscard]] auto reallocate(const std::size_t new_capacity)
      -> result<void> {
    pointer new_data = nullptr;
    try {
      new_data = std::allocator_traits<allocator_type>::allocate(allocator_,
                                                                 new_capacity);
    } catch (...) {
      return result<void>::failure(errc::resource_exhausted);
    }

    auto relocated = relocate_into(data_, size_, new_data);
    if (relocated.has_error()) {
      std::allocator_traits<allocator_type>::deallocate(allocator_, new_data,
                                                        new_capacity);
      return relocated.error();
    }

    if constexpr (!std::is_trivially_destructible_v<value_t>) {
      std::destroy_n(data_, size_);
    }

    release_heap_if_needed();
    data_ = new_data;
    capacity_ = new_capacity;

    return result<void>::success();
  }

  void move_from(small_vector_impl &&other) {
    if (other.using_inline_storage()) {
      auto relocated = relocate_into(other.data_, other.size_, data_);
      if (relocated.has_error()) {
        return;
      }

      size_ = other.size_;
      other.clear();
      return;
    }

    data_ = other.data_;
    size_ = other.size_;
    capacity_ = other.capacity_;

    other.reset_to_inline();
  }

  template <typename... args_t>
  [[nodiscard]] auto append_one(args_t &&...args) noexcept -> result<void> {
    if (size_ == capacity_)
      wh_unlikely {
        return append_one_slow_path(std::forward<args_t>(args)...);
      }

    if constexpr (std::is_nothrow_constructible_v<value_t, decltype(args)...>) {
      std::construct_at(data_ + size_, std::forward<args_t>(args)...);
    } else {
      try {
        std::construct_at(data_ + size_, std::forward<args_t>(args)...);
      } catch (...) {
        return result<void>::failure(errc::internal_error);
      }
    }

    ++size_;
    return result<void>::success();
  }

  template <typename... args_t>
  [[nodiscard]] auto append_one_slow_path(args_t &&...args) noexcept
      -> result<void> {
    auto growth = ensure_capacity_for(1U);
    if (growth.has_error()) {
      return growth.error();
    }

    if constexpr (std::is_nothrow_constructible_v<value_t, decltype(args)...>) {
      std::construct_at(data_ + size_, std::forward<args_t>(args)...);
    } else {
      try {
        std::construct_at(data_ + size_, std::forward<args_t>(args)...);
      } catch (...) {
        return result<void>::failure(errc::internal_error);
      }
    }

    ++size_;
    return result<void>::success();
  }

  [[nodiscard]] static auto
  grow_capacity_with_ratio(const std::size_t current_capacity,
                           const std::size_t max_capacity) noexcept
      -> std::size_t {
    if constexpr (options_type::growth_numerator == 0U) {
      return current_capacity + 1U;
    }

    const std::size_t overflow_limit =
        max_capacity / options_type::growth_numerator;

    if (current_capacity <= overflow_limit) {
      const std::size_t multiplied =
          (current_capacity * options_type::growth_numerator) /
          options_type::growth_denominator;
      return multiplied > current_capacity ? multiplied
                                           : (current_capacity + 1U);
    }

    if constexpr (options_type::growth_denominator == 1U) {
      return max_capacity;
    }

    if ((current_capacity / options_type::growth_denominator) >
        overflow_limit) {
      return max_capacity;
    }

    const std::size_t scaled_capacity =
        current_capacity / options_type::growth_denominator;
    const std::size_t multiplied =
        scaled_capacity * options_type::growth_numerator;
    return multiplied > current_capacity ? multiplied : max_capacity;
  }

  [[nodiscard]] auto append_default_n(const std::size_t count) -> result<void>
    requires std::default_initializable<value_t>
  {
    if (count == 0U) {
      return result<void>::success();
    }

    if constexpr (std::is_nothrow_default_constructible_v<value_t>) {
      std::uninitialized_default_construct_n(data_ + size_, count);
    } else {
      std::size_t constructed = 0U;
      try {
        for (; constructed < count; ++constructed) {
          std::construct_at(data_ + size_ + constructed);
        }
      } catch (...) {
        std::destroy_n(data_ + size_, constructed);
        return result<void>::failure(errc::internal_error);
      }
    }

    size_ += count;
    return result<void>::success();
  }

  [[nodiscard]] auto append_value_initialized_n(const std::size_t count)
      -> result<void>
    requires std::default_initializable<value_t>
  {
    if (count == 0U) {
      return result<void>::success();
    }

    if constexpr (std::is_nothrow_default_constructible_v<value_t>) {
      std::uninitialized_value_construct_n(data_ + size_, count);
    } else {
      std::size_t constructed = 0U;
      try {
        for (; constructed < count; ++constructed) {
          std::construct_at(data_ + size_ + constructed, value_t{});
        }
      } catch (...) {
        std::destroy_n(data_ + size_, constructed);
        return result<void>::failure(errc::internal_error);
      }
    }

    size_ += count;
    return result<void>::success();
  }

  [[nodiscard]] auto append_fill_n(const std::size_t count,
                                   const value_t &value) -> result<void> {
    if (count == 0U) {
      return result<void>::success();
    }

    if constexpr (std::is_nothrow_copy_constructible_v<value_t>) {
      (void)std::uninitialized_fill_n(data_ + size_, count, value);
    } else {
      std::size_t constructed = 0U;
      try {
        for (; constructed < count; ++constructed) {
          std::construct_at(data_ + size_ + constructed, value);
        }
      } catch (...) {
        std::destroy_n(data_ + size_, constructed);
        return result<void>::failure(errc::internal_error);
      }
    }

    size_ += count;
    return result<void>::success();
  }

  template <std::input_iterator input_it>
  [[nodiscard]] auto append_copy_range(input_it first, input_it last)
      -> result<void> {
    if constexpr (std::contiguous_iterator<input_it> &&
                  std::is_trivially_copyable_v<value_t> &&
                  std::same_as<std::remove_cv_t<std::iter_value_t<input_it>>,
                               value_t>) {
      const std::size_t count = static_cast<std::size_t>(last - first);
      if (count > 0U) {
        std::memcpy(data_ + size_, std::to_address(first),
                    sizeof(value_t) * count);
      }
      size_ += count;
      return result<void>::success();
    }

    if constexpr (std::forward_iterator<input_it>) {
      if constexpr (std::is_nothrow_copy_constructible_v<value_t>) {
        const pointer inserted_end =
            std::uninitialized_copy(first, last, data_ + size_);
        size_ += static_cast<std::size_t>(inserted_end - (data_ + size_));
        return result<void>::success();
      }

      pointer inserted_end = data_ + size_;
      try {
        inserted_end = std::uninitialized_copy(first, last, data_ + size_);
      } catch (...) {
        return result<void>::failure(errc::internal_error);
      }

      size_ += static_cast<std::size_t>(inserted_end - (data_ + size_));
      return result<void>::success();
    }

    if constexpr (std::is_nothrow_copy_constructible_v<value_t>) {
      std::size_t constructed = 0U;
      for (; first != last; ++first, ++constructed) {
        std::construct_at(data_ + size_ + constructed, *first);
      }

      size_ += constructed;
      return result<void>::success();
    }

    std::size_t constructed = 0U;
    try {
      for (; first != last; ++first, ++constructed) {
        std::construct_at(data_ + size_ + constructed, *first);
      }
    } catch (...) {
      std::destroy_n(data_ + size_, constructed);
      return result<void>::failure(errc::internal_error);
    }

    size_ += constructed;
    return result<void>::success();
  }

  template <std::input_iterator input_it>
  [[nodiscard]] auto append_move_range(input_it first, input_it last)
      -> result<void> {
    if constexpr (std::contiguous_iterator<input_it> &&
                  std::is_trivially_copyable_v<value_t> &&
                  std::same_as<std::remove_cv_t<std::iter_value_t<input_it>>,
                               value_t>) {
      const std::size_t count = static_cast<std::size_t>(last - first);
      if (count > 0U) {
        std::memmove(data_ + size_, std::to_address(first),
                     sizeof(value_t) * count);
      }
      size_ += count;
      return result<void>::success();
    }

    if constexpr (std::forward_iterator<input_it>) {
      if constexpr (std::is_nothrow_move_constructible_v<value_t>) {
        const pointer inserted_end =
            std::uninitialized_move(first, last, data_ + size_);
        size_ += static_cast<std::size_t>(inserted_end - (data_ + size_));
        return result<void>::success();
      }

      pointer inserted_end = data_ + size_;
      try {
        inserted_end = std::uninitialized_move(first, last, data_ + size_);
      } catch (...) {
        return result<void>::failure(errc::internal_error);
      }

      size_ += static_cast<std::size_t>(inserted_end - (data_ + size_));
      return result<void>::success();
    }

    if constexpr (std::is_nothrow_move_constructible_v<value_t>) {
      std::size_t constructed = 0U;
      for (; first != last; ++first, ++constructed) {
        std::construct_at(data_ + size_ + constructed, std::move(*first));
      }

      size_ += constructed;
      return result<void>::success();
    }

    std::size_t constructed = 0U;
    try {
      for (; first != last; ++first, ++constructed) {
        std::construct_at(data_ + size_ + constructed, std::move(*first));
      }
    } catch (...) {
      std::destroy_n(data_ + size_, constructed);
      return result<void>::failure(errc::internal_error);
    }

    size_ += constructed;
    return result<void>::success();
  }

  [[nodiscard]] auto swap_inline_nothrow(small_vector_impl &other) noexcept
      -> result<void> {
    const bool this_smaller = size_ < other.size_;
    small_vector_impl &smaller = this_smaller ? *this : other;
    small_vector_impl &bigger = this_smaller ? other : *this;

    const std::size_t common = smaller.size_;
    const std::size_t bigger_size = bigger.size_;

    for (std::size_t index = 0U; index < common; ++index) {
      std::swap(smaller.data_[index], bigger.data_[index]);
    }

    const std::size_t tail_count = bigger_size - common;
    for (std::size_t index = 0U; index < tail_count; ++index) {
      std::construct_at(smaller.data_ + common + index,
                        std::move(bigger.data_[common + index]));
    }

    std::destroy_n(bigger.data_ + common, tail_count);
    smaller.size_ = bigger_size;
    bigger.size_ = common;
    return result<void>::success();
  }
};

template <typename value_t, typename allocator_t = std::allocator<value_t>,
          typename options_t = small_vector_default_options>
class small_vector_base {
public:
  using value_type = value_t;
  using allocator_type = allocator_t;
  using options_type = options_t;
  using size_type = typename options_t::size_type;
  using pointer = value_t *;
  using const_pointer = const value_t *;

  virtual ~small_vector_base() = default;

  [[nodiscard]] virtual auto empty() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto size() const noexcept -> size_type = 0;
  [[nodiscard]] virtual auto capacity() const noexcept -> size_type = 0;
  [[nodiscard]] virtual auto data() noexcept -> pointer = 0;
  [[nodiscard]] virtual auto data() const noexcept -> const_pointer = 0;
  [[nodiscard]] virtual auto using_inline_storage() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto is_small() const noexcept -> bool = 0;
  [[nodiscard]] virtual auto internal_storage() noexcept -> pointer = 0;
  [[nodiscard]] virtual auto internal_storage() const noexcept
      -> const_pointer = 0;
  [[nodiscard]] virtual auto
  storage_is_unpropagable(const_pointer pointer_value) const noexcept
      -> bool = 0;
};

template <typename value_t, std::size_t inline_capacity = 8U,
          typename allocator_t = std::allocator<value_t>,
          typename options_t = small_vector_default_options>
class small_vector : public small_vector_base<value_t, allocator_t, options_t>,
                     public small_vector_impl<value_t, inline_capacity,
                                              allocator_t, options_t> {
public:
  using interface_type = small_vector_base<value_t, allocator_t, options_t>;
  using impl_type =
      small_vector_impl<value_t, inline_capacity, allocator_t, options_t>;
  using allocator_type = allocator_t;

  using impl_type::impl_type;
  using impl_type::operator=;

  small_vector() = default;
  small_vector(const small_vector &) = default;
  small_vector(small_vector &&) noexcept = default;
  auto operator=(const small_vector &) -> small_vector & = default;
  auto operator=(small_vector &&) noexcept -> small_vector & = default;
  ~small_vector() = default;

  small_vector(const impl_type &other) : impl_type(other) {}

  small_vector(impl_type &&other) noexcept : impl_type(std::move(other)) {}

  small_vector(const impl_type &other, const allocator_type &allocator)
      : impl_type(other, allocator) {}

  small_vector(impl_type &&other, const allocator_type &allocator)
      : impl_type(std::move(other), allocator) {}

  auto operator=(const impl_type &other) -> small_vector & {
    impl_type::operator=(other);
    return *this;
  }

  auto operator=(impl_type &&other) -> small_vector & {
    impl_type::operator=(std::move(other));
    return *this;
  }

  [[nodiscard]] auto empty() const noexcept -> bool override {
    return impl_type::empty();
  }

  [[nodiscard]] auto size() const noexcept ->
      typename interface_type::size_type override {
    return impl_type::size();
  }

  [[nodiscard]] auto capacity() const noexcept ->
      typename interface_type::size_type override {
    return impl_type::capacity();
  }

  [[nodiscard]] auto data() noexcept ->
      typename interface_type::pointer override {
    return impl_type::data();
  }

  [[nodiscard]] auto data() const noexcept ->
      typename interface_type::const_pointer override {
    return impl_type::data();
  }

  [[nodiscard]] auto using_inline_storage() const noexcept -> bool override {
    return impl_type::using_inline_storage();
  }

  [[nodiscard]] auto is_small() const noexcept -> bool override {
    return impl_type::is_small();
  }

  [[nodiscard]] auto internal_storage() noexcept ->
      typename interface_type::pointer override {
    return impl_type::internal_storage();
  }

  [[nodiscard]] auto internal_storage() const noexcept ->
      typename interface_type::const_pointer override {
    return impl_type::internal_storage();
  }

  [[nodiscard]] auto storage_is_unpropagable(
      typename interface_type::const_pointer pointer_value) const noexcept
      -> bool override {
    return impl_type::storage_is_unpropagable(pointer_value);
  }

  [[nodiscard]] static auto from_std_vector(const std::vector<value_t> &values)
      -> result<small_vector> {
    auto converted = impl_type::from_std_vector_impl(values);
    if (converted.has_error()) {
      return result<small_vector>::failure(converted.error());
    }

    return small_vector(std::move(converted.value()));
  }
};

template <typename value_t, std::size_t inline_capacity, typename allocator_t,
          typename options_t, typename equal_t>
[[nodiscard]] auto
erase(small_vector<value_t, inline_capacity, allocator_t, options_t> &container,
      const equal_t &value) ->
    typename small_vector<value_t, inline_capacity, allocator_t,
                          options_t>::size_type {
  const auto old_size = container.size();
  container.erase(std::remove(container.begin(), container.end(), value),
                  container.end());
  return static_cast<typename small_vector<value_t, inline_capacity,
                                           allocator_t, options_t>::size_type>(
      old_size - container.size());
}

template <typename value_t, std::size_t inline_capacity, typename allocator_t,
          typename options_t, typename predicate_t>
[[nodiscard]] auto erase_if(
    small_vector<value_t, inline_capacity, allocator_t, options_t> &container,
    predicate_t predicate) ->
    typename small_vector<value_t, inline_capacity, allocator_t,
                          options_t>::size_type {
  const auto old_size = container.size();
  container.erase(std::remove_if(container.begin(), container.end(), predicate),
                  container.end());
  return static_cast<typename small_vector<value_t, inline_capacity,
                                           allocator_t, options_t>::size_type>(
      old_size - container.size());
}

template <typename value_t, std::size_t inline_capacity, typename allocator_t,
          typename options_t>
[[nodiscard]] auto
swap(small_vector<value_t, inline_capacity, allocator_t, options_t> &lhs,
     small_vector<value_t, inline_capacity, allocator_t, options_t> &rhs)
    -> result<void> {
  return lhs.swap(rhs);
}

} // namespace wh::core
