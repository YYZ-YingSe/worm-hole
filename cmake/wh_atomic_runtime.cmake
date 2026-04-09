include_guard(GLOBAL)

include(CheckCXXSourceCompiles)
include(CMakePushCheckState)

set(
  WH_ATOMIC_RUNTIME_PROBE_SOURCE
  [[
#include <atomic>

static std::atomic<unsigned long> wh_atomic_runtime_probe_value{0U};

auto main() -> int {
  return wh_atomic_runtime_probe_value.is_lock_free() ? 0 : 1;
}
]])

function(wh_setup_atomic_runtime_target)
  if(TARGET wh_atomic_runtime)
    return()
  endif()

  add_library(wh_atomic_runtime INTERFACE)
  add_library(wh::atomic_runtime ALIAS wh_atomic_runtime)

  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_QUIET ON)
  check_cxx_source_compiles("${WH_ATOMIC_RUNTIME_PROBE_SOURCE}"
                            WH_HAVE_ATOMIC_RUNTIME_WITHOUT_LIBATOMIC)
  cmake_pop_check_state()

  if(WH_HAVE_ATOMIC_RUNTIME_WITHOUT_LIBATOMIC)
    message(STATUS "wh atomic runtime: no extra runtime library required")
    return()
  endif()

  cmake_push_check_state(RESET)
  set(CMAKE_REQUIRED_QUIET ON)
  set(CMAKE_REQUIRED_LIBRARIES atomic)
  check_cxx_source_compiles("${WH_ATOMIC_RUNTIME_PROBE_SOURCE}"
                            WH_HAVE_ATOMIC_RUNTIME_WITH_LIBATOMIC)
  cmake_pop_check_state()

  if(WH_HAVE_ATOMIC_RUNTIME_WITH_LIBATOMIC)
    target_link_libraries(wh_atomic_runtime INTERFACE atomic)
    message(STATUS "wh atomic runtime: linking libatomic")
    return()
  endif()

  message(
    FATAL_ERROR
      "wh atomic runtime probe failed both without and with libatomic; "
      "the current C++ toolchain cannot satisfy the required atomic runtime support.")
endfunction()
