if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL "Windows")
  message(FATAL_ERROR "msvc-cl toolchain requires a Windows host")
endif()

find_program(WH_MSVC_CL NAMES cl)

if(NOT WH_MSVC_CL)
  message(FATAL_ERROR "msvc-cl toolchain could not find cl.exe in PATH")
endif()

set(CMAKE_C_COMPILER "${WH_MSVC_CL}" CACHE FILEPATH "MSVC C compiler" FORCE)
set(CMAKE_CXX_COMPILER "${WH_MSVC_CL}" CACHE FILEPATH "MSVC C++ compiler" FORCE)
