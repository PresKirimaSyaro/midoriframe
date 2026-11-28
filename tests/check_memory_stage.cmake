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
reject(early_dm_e "E { jump(dm_read(32'd0, 4)); }" "dm_read is only available in M")
reject(jump_before_annul "D { if (ins[0]) jump(pc); } E { if (!ins[0]) annul_delay_slot(); }"
    "jump in D precedes the last annul_delay_slot in E")
reject(jump_before_last_annul "E { annul_delay_slot(); jump(pc); } M { if (ins[0]) annul_delay_slot(); }"
    "jump in E precedes the last annul_delay_slot in M")
reject(jump_before_dead_annul "D { jump(pc); } M { if (0) annul_delay_slot(); }"
    "jump in D precedes the last annul_delay_slot in M")

