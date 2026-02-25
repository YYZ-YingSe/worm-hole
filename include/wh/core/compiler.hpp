#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#include <immintrin.h>
#endif

namespace wh::core {

enum class compiler_id : std::uint8_t {
  unknown = 0,
  clang = 1,
  gcc = 2,
  msvc = 3,
};

#if defined(__clang__)
inline constexpr compiler_id active_compiler = compiler_id::clang;
inline constexpr int compiler_version_major = __clang_major__;
inline constexpr int compiler_version_minor = __clang_minor__;
inline constexpr int compiler_version_patch = __clang_patchlevel__;
#elif defined(__GNUC__)
inline constexpr compiler_id active_compiler = compiler_id::gcc;
inline constexpr int compiler_version_major = __GNUC__;
inline constexpr int compiler_version_minor = __GNUC_MINOR__;
inline constexpr int compiler_version_patch = __GNUC_PATCHLEVEL__;
#elif defined(_MSC_VER)
inline constexpr compiler_id active_compiler = compiler_id::msvc;
inline constexpr int compiler_version_major = _MSC_VER / 100;
inline constexpr int compiler_version_minor = _MSC_VER % 100;
inline constexpr int compiler_version_patch = 0;
#else
inline constexpr compiler_id active_compiler = compiler_id::unknown;
inline constexpr int compiler_version_major = 0;
inline constexpr int compiler_version_minor = 0;
inline constexpr int compiler_version_patch = 0;
#endif

inline constexpr bool compiler_is_clang = active_compiler == compiler_id::clang;
inline constexpr bool compiler_is_gcc = active_compiler == compiler_id::gcc;
inline constexpr bool compiler_is_msvc = active_compiler == compiler_id::msvc;

#if defined(__cpp_lib_hardware_interference_size)
inline constexpr std::size_t default_cacheline_size =
    std::hardware_destructive_interference_size > 0
        ? std::hardware_destructive_interference_size
        : 64U;
#else
inline constexpr std::size_t default_cacheline_size = 64U;
#endif

template <typename t>
concept trivially_relocatable = std::is_trivially_move_constructible_v<t> &&
                                std::is_trivially_destructible_v<t>;

template <typename t>
concept trivially_copyable_value = std::is_trivially_copyable_v<t>;

template <typename condition_t>
  requires std::convertible_to<condition_t, bool>
[[nodiscard]] inline constexpr bool
predict_likely(condition_t &&value) noexcept {
  const bool raw = static_cast<bool>(std::forward<condition_t>(value));
#if defined(__has_builtin)
#if __has_builtin(__builtin_expect)
  return __builtin_expect(raw, true);
#else
  return raw;
#endif
#elif defined(__GNUC__) || defined(__clang__)
  return __builtin_expect(raw, true);
#else
  return raw;
#endif
}

template <typename condition_t>
  requires std::convertible_to<condition_t, bool>
[[nodiscard]] inline constexpr bool
predict_unlikely(condition_t &&value) noexcept {
  const bool raw = static_cast<bool>(std::forward<condition_t>(value));
#if defined(__has_builtin)
#if __has_builtin(__builtin_expect)
  return __builtin_expect(raw, false);
#else
  return raw;
#endif
#elif defined(__GNUC__) || defined(__clang__)
  return __builtin_expect(raw, false);
#else
  return raw;
#endif
}

[[nodiscard]] constexpr bool is_power_of_two(const std::size_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

[[nodiscard]] constexpr std::size_t
align_up(const std::size_t value, const std::size_t alignment) noexcept {
  if (!is_power_of_two(alignment)) {
    return value;
  }
  return (value + alignment - 1U) & ~(alignment - 1U);
}

[[nodiscard]] constexpr std::size_t
next_power_of_two(std::size_t value) noexcept {
  if (value <= 1U) {
    return 1U;
  }
  value -= 1U;
  for (std::size_t shift = 1U; shift < (sizeof(std::size_t) * 8U);
       shift <<= 1U) {
    value |= value >> shift;
  }
  return value + 1U;
}

[[noreturn]] inline void unreachable() noexcept {
#if defined(__has_builtin)
#if __has_builtin(__builtin_unreachable)
  __builtin_unreachable();
#endif
#elif defined(__GNUC__) || defined(__clang__)
  __builtin_unreachable();
#elif defined(_MSC_VER)
  __assume(false);
#endif
  std::abort();
}

template <typename condition_t>
  requires std::convertible_to<condition_t, bool>
inline void assume(condition_t &&condition) noexcept {
  const bool raw = static_cast<bool>(std::forward<condition_t>(condition));
#if defined(_MSC_VER)
  __assume(raw);
#elif defined(__has_builtin)
#if __has_builtin(__builtin_assume)
  __builtin_assume(raw);
#else
  if (!raw) {
    unreachable();
  }
#endif
#else
  if (!raw) {
    unreachable();
  }
#endif
}

inline void spin_pause() noexcept {
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) ||             \
    defined(_M_IX86)
#if defined(_MSC_VER)
  _mm_pause();
#else
  __asm__ __volatile__("pause" ::: "memory");
#endif
#elif defined(__aarch64__) || defined(__arm__)
  __asm__ __volatile__("yield" ::: "memory");
#endif
}

[[noreturn]] inline void contract_violation(const char *kind,
                                            const char *expression,
                                            const char *file,
                                            const int line) noexcept {
  std::fprintf(stderr, "[wh-contract] %s failed: %s at %s:%d\n", kind,
               expression, file, line);
  std::abort();
}

} // namespace wh::core

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(likely)
#define wh_likely [[likely]]
#else
#define wh_likely
#endif

#if __has_cpp_attribute(unlikely)
#define wh_unlikely [[unlikely]]
#else
#define wh_unlikely
#endif
#else
#define wh_likely
#define wh_unlikely
#endif

#if defined(__GNUC__) || defined(__clang__)
#define wh_force_inline [[gnu::always_inline]] inline
#define wh_noinline [[gnu::noinline]]
#define wh_hot [[gnu::hot]]
#define wh_cold [[gnu::cold]]
#define wh_cacheline_align alignas(::wh::core::default_cacheline_size)
#else
#define wh_force_inline inline
#define wh_noinline
#define wh_hot
#define wh_cold
#define wh_cacheline_align
#endif

#define wh_precondition(expr)                                                  \
  do {                                                                         \
    if (!(expr))                                                               \
      wh_unlikely {                                                            \
        ::wh::core::contract_violation("precondition", #expr, __FILE__,        \
                                       __LINE__);                              \
      }                                                                        \
  } while (false)

#define wh_postcondition(expr)                                                 \
  do {                                                                         \
    if (!(expr))                                                               \
      wh_unlikely {                                                            \
        ::wh::core::contract_violation("postcondition", #expr, __FILE__,       \
                                       __LINE__);                              \
      }                                                                        \
  } while (false)

#define wh_invariant(expr)                                                     \
  do {                                                                         \
    if (!(expr))                                                               \
      wh_unlikely {                                                            \
        ::wh::core::contract_violation("invariant", #expr, __FILE__,           \
                                       __LINE__);                              \
      }                                                                        \
  } while (false)
