if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  message(FATAL_ERROR "apple-clang toolchain requires a Darwin host")
endif()

find_program(WH_APPLE_CLANG_C NAMES clang PATHS /usr/bin NO_DEFAULT_PATH)
find_program(WH_APPLE_CLANG_CXX NAMES clang++ PATHS /usr/bin NO_DEFAULT_PATH)

if(NOT WH_APPLE_CLANG_C OR NOT WH_APPLE_CLANG_CXX)
  message(FATAL_ERROR "apple-clang toolchain could not find /usr/bin/clang and /usr/bin/clang++")
endif()

set(CMAKE_C_COMPILER "${WH_APPLE_CLANG_C}" CACHE FILEPATH "Apple Clang C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${WH_APPLE_CLANG_CXX}" CACHE FILEPATH "Apple Clang C++ compiler" FORCE)
