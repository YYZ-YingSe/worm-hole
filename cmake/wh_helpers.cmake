include_guard(GLOBAL)

function(wh_require_cmake_source_dir path label hint)
  if(EXISTS "${path}/CMakeLists.txt")
    return()
  endif()

  message(FATAL_ERROR
          "${label} source is required at ${path}\n"
          "Hint: ${hint}")
endfunction()
