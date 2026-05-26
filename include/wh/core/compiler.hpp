// Defines compiler-level portability helpers such as branch prediction,
// alignment utilities, and constexpr numeric helpers.
#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__has_builtin)
#define WH_HAS_BUILTIN(feature) __has_builtin(feature)
#else
#define WH_HAS_BUILTIN(feature) 0
#endif

#if defined(__has_cpp_attribute)
#define WH_HAS_CPP_ATTRIBUTE(attribute) __has_cpp_attribute(attribute)
#else
#define WH_HAS_CPP_ATTRIBUTE(attribute) 0
#endif

#if defined(__has_attribute)
#define WH_HAS_ATTRIBUTE(attribute) __has_attribute(attribute)
#else
#define WH_HAS_ATTRIBUTE(attribute) 0
#endif

#if defined(__clang__)
#define WH_COMPILER_CLANG 1
#define WH_COMPILER_GCC 0
#define WH_COMPILER_MSVC 0
#elif defined(__GNUC__)
#define WH_COMPILER_CLANG 0
#define WH_COMPILER_GCC 1
#define WH_COMPILER_MSVC 0
#elif defined(_MSC_VER)
#define WH_COMPILER_CLANG 0
#define WH_COMPILER_GCC 0
#define WH_COMPILER_MSVC 1
#else
#define WH_COMPILER_CLANG 0
#define WH_COMPILER_GCC 0
#define WH_COMPILER_MSVC 0
#endif

#if WH_COMPILER_CLANG || WH_COMPILER_GCC
#define WH_COMPILER_GNU_LIKE 1
#else
#define WH_COMPILER_GNU_LIKE 0
#endif

#if defined(_MSC_VER)
#define WH_COMPILER_MSVC_HEADERS 1
#else
#define WH_COMPILER_MSVC_HEADERS 0
#endif

#if defined(_M_ARM64EC)
#define WH_CPU_ARM64EC 1
#else
#define WH_CPU_ARM64EC 0
#endif

#if !WH_CPU_ARM64EC && (defined(_M_X64) || defined(__x86_64__) || defined(__amd64__))
#define WH_CPU_X64 1
#else
#define WH_CPU_X64 0
#endif

#if defined(_M_IX86) || defined(__i386__)
#define WH_CPU_X86 1
#else
#define WH_CPU_X86 0
#endif

#if defined(_M_ARM64) || defined(__aarch64__)
#define WH_CPU_ARM64 1
#else
#define WH_CPU_ARM64 0
#endif

#if defined(_M_ARM) || defined(__arm__)
#define WH_CPU_ARM 1
#else
#define WH_CPU_ARM 0
#endif

#if defined(__powerpc64__)
#define WH_CPU_POWERPC64 1
#else
#define WH_CPU_POWERPC64 0
#endif

#if WH_CPU_X86 || WH_CPU_X64
#define WH_CPU_X86_FAMILY 1
#else
#define WH_CPU_X86_FAMILY 0
#endif

#if WH_CPU_ARM || WH_CPU_ARM64 || WH_CPU_ARM64EC
#define WH_CPU_ARM_FAMILY 1
#else
#define WH_CPU_ARM_FAMILY 0
#endif

#if WH_CPU_X86_FAMILY || WH_CPU_ARM_FAMILY || WH_CPU_POWERPC64
#define WH_CPU_KNOWN 1
#else
#define WH_CPU_KNOWN 0
#endif

#if defined(_WIN32)
#define WH_OS_WINDOWS 1
#else
#define WH_OS_WINDOWS 0
#endif

#if defined(__APPLE__) && defined(__MACH__)
#define WH_OS_DARWIN 1
#else
#define WH_OS_DARWIN 0
#endif

#if defined(__unix__) && !WH_OS_DARWIN
#define WH_OS_UNIX 1
#else
#define WH_OS_UNIX 0
#endif

#if WH_OS_UNIX || WH_OS_DARWIN
#define WH_OS_POSIX_LIKE 1
#else
#define WH_OS_POSIX_LIKE 0
#endif

#define WH_HAS_WINDOWS_STACKTRACE_CAPTURE WH_OS_WINDOWS
#define WH_HAS_EXECINFO_STACKTRACE_CAPTURE WH_OS_POSIX_LIKE
#define WH_SUPPORTS_NATIVE_STACKTRACE_CAPTURE                                                        \
  (WH_HAS_WINDOWS_STACKTRACE_CAPTURE || WH_HAS_EXECINFO_STACKTRACE_CAPTURE)

#define WH_SUPPORTS_INTRINSIC_SPIN_PAUSE                                                             \
  (WH_COMPILER_MSVC_HEADERS && (WH_CPU_X86_FAMILY || WH_CPU_ARM_FAMILY))
#define WH_SUPPORTS_ASM_SPIN_PAUSE                                                                   \
  (!WH_COMPILER_MSVC_HEADERS && (WH_CPU_X86_FAMILY || WH_CPU_ARM_FAMILY || WH_CPU_POWERPC64))
#define WH_SUPPORTS_NATIVE_SPIN_PAUSE                                                                \
  (WH_SUPPORTS_INTRINSIC_SPIN_PAUSE || WH_SUPPORTS_ASM_SPIN_PAUSE)

#if WH_SUPPORTS_INTRINSIC_SPIN_PAUSE
#include <intrin.h>
#endif

#if WH_HAS_CPP_ATTRIBUTE(likely)
#define wh_likely [[likely]]
#else
#define wh_likely
#endif

#if WH_HAS_CPP_ATTRIBUTE(unlikely)
#define wh_unlikely [[unlikely]]
#else
#define wh_unlikely
#endif

#if WH_HAS_CPP_ATTRIBUTE(no_unique_address)
#define wh_no_unique_address [[no_unique_address]]
#elif WH_COMPILER_MSVC_HEADERS && WH_HAS_CPP_ATTRIBUTE(msvc::no_unique_address)
#define wh_no_unique_address [[msvc::no_unique_address]]
#else
#define wh_no_unique_address
#endif

#if WH_COMPILER_MSVC_HEADERS
#define wh_empty_bases __declspec(empty_bases)
#else
#define wh_empty_bases
#endif

#if WH_COMPILER_GNU_LIKE && WH_HAS_CPP_ATTRIBUTE(gnu::always_inline)
#define WH_ATTRIBUTE_ALWAYS_INLINE [[gnu::always_inline]]
#elif WH_COMPILER_GNU_LIKE && WH_HAS_ATTRIBUTE(always_inline)
#define WH_ATTRIBUTE_ALWAYS_INLINE __attribute__((always_inline))
#else
#define WH_ATTRIBUTE_ALWAYS_INLINE
#endif

#if WH_COMPILER_GNU_LIKE && WH_HAS_CPP_ATTRIBUTE(gnu::noinline)
#define WH_ATTRIBUTE_NOINLINE [[gnu::noinline]]
#elif WH_COMPILER_GNU_LIKE && WH_HAS_ATTRIBUTE(noinline)
#define WH_ATTRIBUTE_NOINLINE __attribute__((noinline))
#else
#define WH_ATTRIBUTE_NOINLINE
#endif

#if WH_COMPILER_GNU_LIKE && WH_HAS_CPP_ATTRIBUTE(gnu::hot)
#define WH_ATTRIBUTE_HOT [[gnu::hot]]
#elif WH_COMPILER_GNU_LIKE && WH_HAS_ATTRIBUTE(hot)
#define WH_ATTRIBUTE_HOT __attribute__((hot))
#else
#define WH_ATTRIBUTE_HOT
#endif

#if WH_COMPILER_GNU_LIKE && WH_HAS_CPP_ATTRIBUTE(gnu::cold)
#define WH_ATTRIBUTE_COLD [[gnu::cold]]
#elif WH_COMPILER_GNU_LIKE && WH_HAS_ATTRIBUTE(cold)
#define WH_ATTRIBUTE_COLD __attribute__((cold))
#else
#define WH_ATTRIBUTE_COLD
#endif

#if WH_COMPILER_GNU_LIKE && WH_HAS_CPP_ATTRIBUTE(gnu::pure)
#define WH_ATTRIBUTE_PURE [[gnu::pure]]
#elif WH_COMPILER_GNU_LIKE && WH_HAS_ATTRIBUTE(pure)
#define WH_ATTRIBUTE_PURE __attribute__((pure))
#else
#define WH_ATTRIBUTE_PURE
#endif

#if WH_COMPILER_GNU_LIKE && WH_HAS_CPP_ATTRIBUTE(gnu::const)
#define WH_ATTRIBUTE_CONST [[gnu::const]]
#elif WH_COMPILER_GNU_LIKE && WH_HAS_ATTRIBUTE(const)
#define WH_ATTRIBUTE_CONST __attribute__((const))
#else
#define WH_ATTRIBUTE_CONST
#endif

#if WH_COMPILER_GNU_LIKE
#define wh_force_inline WH_ATTRIBUTE_ALWAYS_INLINE inline
#define wh_noinline WH_ATTRIBUTE_NOINLINE
#define wh_hot WH_ATTRIBUTE_HOT
#define wh_cold WH_ATTRIBUTE_COLD
#define wh_pure WH_ATTRIBUTE_PURE
#define wh_const WH_ATTRIBUTE_CONST
#define wh_restrict __restrict__
#elif WH_COMPILER_MSVC_HEADERS
#define wh_force_inline __forceinline
#define wh_noinline __declspec(noinline)
#define wh_hot
#define wh_cold
#define wh_pure
#define wh_const
#define wh_restrict __restrict
#else
#define wh_force_inline inline
#define wh_noinline
#define wh_hot
#define wh_cold
#define wh_pure
#define wh_const
#define wh_restrict
#endif

namespace wh::core {

/// Identifies the active compiler for compile-time feature branching.
enum class compiler_id : std::uint8_t {
  /// Compiler could not be identified from predefined macros.
  unknown = 0,
  /// LLVM/Clang toolchain.
  clang = 1,
  /// GNU Compiler Collection.
  gcc = 2,
  /// Microsoft Visual C++ toolchain.
  msvc = 3,
};

#if WH_COMPILER_CLANG
inline constexpr compiler_id active_compiler = compiler_id::clang;
inline constexpr int compiler_version_major = __clang_major__;
inline constexpr int compiler_version_minor = __clang_minor__;
inline constexpr int compiler_version_patch = __clang_patchlevel__;
#elif WH_COMPILER_GCC
inline constexpr compiler_id active_compiler = compiler_id::gcc;
inline constexpr int compiler_version_major = __GNUC__;
inline constexpr int compiler_version_minor = __GNUC_MINOR__;
inline constexpr int compiler_version_patch = __GNUC_PATCHLEVEL__;
#elif WH_COMPILER_MSVC
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

inline constexpr bool compiler_uses_msvc_headers = WH_COMPILER_MSVC_HEADERS != 0;

/// Identifies the active target CPU family for architecture-specific helpers.
enum class cpu_architecture : std::uint8_t {
  /// Target CPU family could not be identified from predefined macros.
  unknown = 0U,
  /// 32-bit x86 target.
  x86 = 1U,
  /// 64-bit x86 target.
  x64 = 2U,
  /// 32-bit ARM target.
  arm = 3U,
  /// 64-bit ARM target, including Windows ARM64.
  arm64 = 4U,
  /// Windows ARM64EC target: ARM64 instructions with x64-compatible ABI conventions.
  arm64ec = 5U,
  /// 64-bit PowerPC target.
  powerpc64 = 6U,
};

#if WH_CPU_ARM64EC
inline constexpr cpu_architecture active_cpu_architecture = cpu_architecture::arm64ec;
#elif WH_CPU_X64
inline constexpr cpu_architecture active_cpu_architecture = cpu_architecture::x64;
#elif WH_CPU_X86
inline constexpr cpu_architecture active_cpu_architecture = cpu_architecture::x86;
#elif WH_CPU_ARM64
inline constexpr cpu_architecture active_cpu_architecture = cpu_architecture::arm64;
#elif WH_CPU_ARM
inline constexpr cpu_architecture active_cpu_architecture = cpu_architecture::arm;
#elif WH_CPU_POWERPC64
inline constexpr cpu_architecture active_cpu_architecture = cpu_architecture::powerpc64;
#else
inline constexpr cpu_architecture active_cpu_architecture = cpu_architecture::unknown;
#endif

inline constexpr bool cpu_is_x86 = active_cpu_architecture == cpu_architecture::x86 ||
                                   active_cpu_architecture == cpu_architecture::x64;
inline constexpr bool cpu_is_arm = active_cpu_architecture == cpu_architecture::arm ||
                                   active_cpu_architecture == cpu_architecture::arm64 ||
                                   active_cpu_architecture == cpu_architecture::arm64ec;
inline constexpr bool cpu_is_powerpc64 =
    active_cpu_architecture == cpu_architecture::powerpc64;
inline constexpr bool cpu_is_known = active_cpu_architecture != cpu_architecture::unknown;

/// Identifies the active operating-system family for platform feature branching.
enum class operating_system_id : std::uint8_t {
  /// Target operating system could not be identified from predefined macros.
  unknown = 0U,
  /// Microsoft Windows target.
  windows = 1U,
  /// Apple Darwin target, including macOS/iOS-like environments.
  darwin = 2U,
  /// Generic Unix/POSIX-like target that is not Darwin.
  unix = 3U,
};

#if WH_OS_WINDOWS
inline constexpr operating_system_id active_operating_system = operating_system_id::windows;
#elif WH_OS_DARWIN
inline constexpr operating_system_id active_operating_system = operating_system_id::darwin;
#elif WH_OS_UNIX
inline constexpr operating_system_id active_operating_system = operating_system_id::unix;
#else
inline constexpr operating_system_id active_operating_system = operating_system_id::unknown;
#endif

inline constexpr bool operating_system_is_windows =
    active_operating_system == operating_system_id::windows;
inline constexpr bool operating_system_is_darwin =
    active_operating_system == operating_system_id::darwin;
inline constexpr bool operating_system_is_unix =
    active_operating_system == operating_system_id::unix;
inline constexpr bool operating_system_is_posix_like =
    operating_system_is_darwin || operating_system_is_unix;

inline constexpr bool supports_native_stacktrace_capture =
    operating_system_is_windows || operating_system_is_posix_like;

inline constexpr std::size_t default_cacheline_size = 64U;
inline constexpr std::size_t pointer_alignment = alignof(void *);
inline constexpr std::size_t default_inline_storage_alignment = alignof(std::max_align_t);

/// Reports whether the active platform headers provide a CPU pause/yield intrinsic.
inline constexpr bool supports_intrinsic_spin_pause =
    compiler_uses_msvc_headers && (cpu_is_x86 || cpu_is_arm);

/// Reports whether the active target supports an inline assembly pause/yield instruction here.
inline constexpr bool supports_asm_spin_pause =
    !compiler_uses_msvc_headers && (cpu_is_x86 || cpu_is_arm || cpu_is_powerpc64);

/// Reports whether `spin_pause()` emits an architecture-specific instruction.
inline constexpr bool supports_native_spin_pause =
    supports_intrinsic_spin_pause || supports_asm_spin_pause;

/// True when a value can be moved by raw relocation safely.
template <typename t>
concept trivially_relocatable =
    std::is_trivially_copyable_v<t> && std::is_move_constructible_v<t>;

/// True when a value is bitwise copyable.
template <typename t>
concept trivially_copyable_value = std::is_trivially_copyable_v<t>;

/// True when destroying a value has no observable side effect.
template <typename t>
concept trivially_destructible_value = std::is_trivially_destructible_v<t>;

/// True when a value can be copied with byte-wise memory operations.
template <typename t>
inline constexpr bool can_memcpy_value_v = trivially_copyable_value<t>;

/// True when a value can be moved across overlapping storage with `memmove`.
template <typename t>
inline constexpr bool can_memmove_value_v = trivially_copyable_value<t>;

/// True when a value can be relocated by copying bytes and discarding the source storage.
template <typename t>
inline constexpr bool can_trivially_relocate_value_v = trivially_relocatable<t>;

/// True when explicit destructor calls can be skipped.
template <typename t>
inline constexpr bool can_skip_destroy_v = trivially_destructible_value<t>;

/// Returns whether `value` is a non-zero power of two.
[[nodiscard]] constexpr bool is_power_of_two(const std::size_t value) noexcept {
  return value != 0U && (value & (value - 1U)) == 0U;
}

/// Aligns `value` upward to `alignment` when alignment is power-of-two.
[[nodiscard]] constexpr std::size_t align_up(const std::size_t value,
                                             const std::size_t alignment) noexcept {
  if (!is_power_of_two(alignment)) {
    return value;
  }
  return (value + alignment - 1U) & ~(alignment - 1U);
}

/// Aligns `value` upward to the compile-time alignment value.
template <std::size_t alignment>
[[nodiscard]] constexpr std::size_t align_up_to(const std::size_t value) noexcept {
  return align_up(value, alignment);
}

/// Aligns `value` upward to pointer alignment.
[[nodiscard]] constexpr std::size_t align_up_to_pointer(const std::size_t value) noexcept {
  return align_up(value, pointer_alignment);
}

/// Returns whether `value_alignment` fits inside storage aligned to `storage_alignment`.
[[nodiscard]] constexpr bool fits_storage_alignment(const std::size_t storage_alignment,
                                                   const std::size_t value_alignment) noexcept {
  return value_alignment != 0U && storage_alignment != 0U &&
         value_alignment <= storage_alignment && (storage_alignment % value_alignment) == 0U;
}

/// Returns whether `alignment` is compatible with pointer-aligned inline storage.
[[nodiscard]] constexpr bool fits_pointer_alignment(const std::size_t alignment) noexcept {
  return fits_storage_alignment(pointer_alignment, alignment);
}

/// Returns whether a type fits in an inline storage buffer with the requested alignment.
template <typename type_t>
[[nodiscard]] constexpr bool fits_inline_storage(const std::size_t size,
                                                const std::size_t alignment) noexcept {
  return sizeof(type_t) <= size && fits_storage_alignment(alignment, alignof(type_t));
}

/// Returns whether a type fits in a pointer-aligned inline storage buffer.
template <typename type_t>
[[nodiscard]] constexpr bool fits_pointer_aligned_storage(const std::size_t size) noexcept {
  return fits_inline_storage<type_t>(size, pointer_alignment);
}

/// Computes the next power of two greater than or equal to `value`.
[[nodiscard]] constexpr std::size_t next_power_of_two(std::size_t value) noexcept {
  if (value <= 1U) {
    return 1U;
  }
  value -= 1U;
  for (std::size_t shift = 1U; shift < (sizeof(std::size_t) * 8U); shift <<= 1U) {
    value |= value >> shift;
  }
  return value + 1U;
}

/// Marks unreachable control flow and aborts as a fallback.
[[noreturn]] wh_cold inline void unreachable() noexcept {
#if WH_HAS_BUILTIN(__builtin_unreachable)
  __builtin_unreachable();
#elif WH_COMPILER_GNU_LIKE
  __builtin_unreachable();
#elif WH_COMPILER_MSVC_HEADERS
  __assume(false);
#endif
  std::abort();
}

/// Hints the optimizer that `condition` is expected to hold.
template <typename condition_t>
  requires std::convertible_to<condition_t, bool>
wh_force_inline void assume(condition_t &&condition) noexcept {
  const bool raw = static_cast<bool>(std::forward<condition_t>(condition));
#if WH_COMPILER_MSVC_HEADERS
  __assume(raw);
#elif WH_HAS_BUILTIN(__builtin_assume)
  __builtin_assume(raw);
#else
  if (!raw) {
    unreachable();
  }
#endif
}

/// Emits a short CPU pause/yield for spin-wait loops.
wh_force_inline void spin_pause() noexcept {
#if WH_SUPPORTS_INTRINSIC_SPIN_PAUSE && WH_CPU_ARM_FAMILY
  __yield();
#elif WH_SUPPORTS_INTRINSIC_SPIN_PAUSE && WH_CPU_X86_FAMILY
  _mm_pause();
#elif WH_SUPPORTS_ASM_SPIN_PAUSE && WH_CPU_X86_FAMILY
  __asm__ __volatile__("pause" ::: "memory");
#elif WH_SUPPORTS_ASM_SPIN_PAUSE && WH_CPU_ARM_FAMILY
  __asm__ __volatile__("yield" ::: "memory");
#elif WH_SUPPORTS_ASM_SPIN_PAUSE && WH_CPU_POWERPC64
  __asm__ __volatile__("or 27,27,27" ::: "memory");
#endif
}

enum class contract_kind : std::uint8_t {
  precondition = 0U,
  postcondition,
  invariant,
};

[[nodiscard]] inline auto contract_kind_name(const contract_kind kind) noexcept
    -> std::string_view {
  switch (kind) {
  case contract_kind::precondition:
    return "precondition";
  case contract_kind::postcondition:
    return "postcondition";
  case contract_kind::invariant:
    return "invariant";
  }
  return "contract";
}

/// Reports contract violation and terminates immediately.
[[noreturn]] inline void
contract_violation(const contract_kind kind, const std::string_view expression,
                   const std::source_location where = std::source_location::current()) noexcept {
  const auto kind_name = contract_kind_name(kind);
  std::fprintf(stderr, "[wh-contract] %.*s failed: %.*s at %s:%u\n",
               static_cast<int>(kind_name.size()), kind_name.data(),
               static_cast<int>(expression.size()), expression.data(), where.file_name(),
               where.line());
  std::abort();
}

} // namespace wh::core

#define wh_cacheline_align alignas(::wh::core::default_cacheline_size)

#ifndef NDEBUG
#define wh_precondition(expr)                                                                      \
  (static_cast<bool>(expr) ||                                                                      \
   (::wh::core::contract_violation(::wh::core::contract_kind::precondition, #expr,                 \
                                   std::source_location::current()),                               \
    false))

#define wh_postcondition(expr)                                                                     \
  (static_cast<bool>(expr) ||                                                                      \
   (::wh::core::contract_violation(::wh::core::contract_kind::postcondition, #expr,                \
                                   std::source_location::current()),                               \
    false))

#define wh_invariant(expr)                                                                         \
  (static_cast<bool>(expr) ||                                                                      \
   (::wh::core::contract_violation(::wh::core::contract_kind::invariant, #expr,                    \
                                   std::source_location::current()),                               \
    false))
#else
#define wh_precondition(expr) ((void)0)
#define wh_postcondition(expr) ((void)0)
#define wh_invariant(expr) ((void)0)
#endif
