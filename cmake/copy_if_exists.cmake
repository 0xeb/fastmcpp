if(NOT DEFINED SRC OR NOT DEFINED DST)
  message(FATAL_ERROR "copy_if_exists.cmake requires -DSRC=... and -DDST=...")
endif()

if(EXISTS "${SRC}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${SRC}" "${DST}"
    RESULT_VARIABLE _copy_result
  )
  if(NOT _copy_result EQUAL 0)
    message(FATAL_ERROR "copy_if_exists.cmake failed copying '${SRC}' -> '${DST}' (code ${_copy_result})")
  endif()
endif()
