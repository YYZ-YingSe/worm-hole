set(_wh_gcc_search_paths
    /opt/homebrew/bin
    /usr/local/bin
    /usr/bin)

find_program(
  WH_GCC_C
  NAMES gcc-15 gcc-14 gcc-13 gcc
  PATHS ${_wh_gcc_search_paths})
find_program(
  WH_GCC_CXX
  NAMES g++-15 g++-14 g++-13 g++
  PATHS ${_wh_gcc_search_paths})

if(NOT WH_GCC_C OR NOT WH_GCC_CXX)
  message(FATAL_ERROR "gcc toolchain could not find gcc and g++")
endif()

set(CMAKE_C_COMPILER "${WH_GCC_C}" CACHE FILEPATH "GCC C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${WH_GCC_CXX}" CACHE FILEPATH "GCC C++ compiler" FORCE)
