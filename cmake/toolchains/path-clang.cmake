find_program(WH_PATH_CLANG_C NAMES clang)
find_program(WH_PATH_CLANG_CXX NAMES clang++)

if(NOT WH_PATH_CLANG_C OR NOT WH_PATH_CLANG_CXX)
  message(FATAL_ERROR "path-clang toolchain could not find clang and clang++ in PATH")
endif()

set(CMAKE_C_COMPILER "${WH_PATH_CLANG_C}" CACHE FILEPATH "PATH Clang C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${WH_PATH_CLANG_CXX}" CACHE FILEPATH "PATH Clang C++ compiler" FORCE)
