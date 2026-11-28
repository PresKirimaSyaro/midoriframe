file(MAKE_DIRECTORY "${OUT}")
file(READ "${EXAMPLES}/p5.mdr" base)
file(READ "${EXAMPLES}/multistage_jump.mdr" branches)
file(WRITE "${OUT}/combined.mdr" "${base}\n${branches}")
file(APPEND "${OUT}/combined.mdr" [=[
overlapping_jumps {
    match(ins[31:26] == 6'h3b);
    D {
        if (ins[0]) jump(pc + 32'd20);
        if (ins[1]) jump(pc + 32'd24);
    }
}

conditional_jm {
    match(ins[31:26] == 6'h3a);
    E {
        if (ins[1]) jump(pc + 32'd20);
    }
    M {
        if (ins[0]) jump(dm_read(32'd0, 4));
        update 32'd1 << 5'd12;
        yield 32'd42;
    }
}
]=])

execute_process(
    COMMAND "${MDRFRAME}" -o "${OUT}" "${OUT}/combined.mdr"
    RESULT_VARIABLE result ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Verilog generation failed: ${error}")
endif()

file(REMOVE "${OUT}/jump-test.vvp")
execute_process(
    COMMAND "${IVERILOG}" -g2005 -s multistage_jump_tb -o jump-test.vvp -f files.f "${TB}"
    WORKING_DIRECTORY "${OUT}"
    RESULT_VARIABLE result ERROR_VARIABLE error
)
# Some Windows Icarus builds exit after emitting a usable image.
if(NOT result EQUAL 0)
    if(NOT (WIN32 AND result MATCHES "^(-1073741819|3221225477|Exit code 0xc0000005|Access violation)$"
            AND error STREQUAL "" AND EXISTS "${OUT}/jump-test.vvp"))
        message(FATAL_ERROR "Icarus compilation failed (${result}): ${error}")
    endif()
endif()

execute_process(
    COMMAND "${VVP}" jump-test.vvp
    WORKING_DIRECTORY "${OUT}"
    RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE error
    TIMEOUT 20
)
if(NOT result EQUAL 0 OR NOT log MATCHES "Multistage jump tests passed")
    message(FATAL_ERROR "Jump simulation failed (${result}): ${error}\n${log}")
endif()
