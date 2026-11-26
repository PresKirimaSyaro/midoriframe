file(MAKE_DIRECTORY "${OUT}")

function(reject name body diagnostic)
    set(path "${OUT}/${name}.mdr")
    file(WRITE "${path}" "check_stage { match(ins[31:26] == 6'h3e); ${body} }\n")
    execute_process(
        COMMAND "${MDRFRAME}" --emit=ir "${path}"
        RESULT_VARIABLE result ERROR_VARIABLE error)
    if(result EQUAL 0 OR NOT error MATCHES "${diagnostic}")
        message(FATAL_ERROR "Expected stage diagnostic '${diagnostic}', got '${error}'")
    endif()
endfunction()

reject(early_dm "D { jump(dm_read(32'd0, 4)); }" "dm_read is only available in M")
reject(late_jump "M { jump(dm_read(32'd0, 4)); }" "jump is only available in D")

