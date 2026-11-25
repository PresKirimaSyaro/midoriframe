execute_process(
    COMMAND "${MDRFRAME}" --emit=deps "${INPUT}"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE output
    ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "mdrframe failed: ${error}")
endif()
foreach(required IN LISTS EXPECT)
    string(FIND "${output}" "${required}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Missing '${required}' in dependency output")
    endif()
endforeach()

