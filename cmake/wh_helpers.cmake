include_guard(GLOBAL)

function(wh_require_source_path path label hint)
  if(EXISTS "${path}")
    return()
  endif()

  message(FATAL_ERROR
          "${label} is required at ${path}\n"
          "Hint: ${hint}")
endfunction()
