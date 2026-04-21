set(_wh_llvm_clang_search_paths
    /opt/homebrew/opt/llvm/bin
    /usr/local/opt/llvm/bin
    /usr/lib/llvm-20/bin
    /usr/lib/llvm-19/bin
    /usr/lib/llvm-18/bin)

find_program(
  WH_LLVM_CLANG_C
  NAMES clang-20 clang-19 clang-18 clang
  PATHS ${_wh_llvm_clang_search_paths})
find_program(
  WH_LLVM_CLANG_CXX
  NAMES clang++-20 clang++-19 clang++-18 clang++
  PATHS ${_wh_llvm_clang_search_paths})

if(NOT WH_LLVM_CLANG_C OR NOT WH_LLVM_CLANG_CXX)
  message(FATAL_ERROR "llvm-clang toolchain could not find clang and clang++")
endif()

set(CMAKE_C_COMPILER "${WH_LLVM_CLANG_C}" CACHE FILEPATH "LLVM Clang C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${WH_LLVM_CLANG_CXX}" CACHE FILEPATH "LLVM Clang C++ compiler" FORCE)
