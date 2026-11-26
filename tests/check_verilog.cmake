file(MAKE_DIRECTORY "${OUT}")
if(DYNAMIC)
    file(READ "${EXAMPLES}/conditional.mdr" conditional)
    file(READ "${EXAMPLES}/lrm.mdr" lrm)
    file(READ "${EXAMPLES}/p5.mdr" base)
    set(input "${OUT}/combined.mdr")
    file(WRITE "${input}" "${conditional}\n${lrm}\n${base}")
    file(APPEND "${input}" [=[
bit_write {
    match(ins[31:26] == 6'h3f);
    E {
        result: 32 = grf_read(5'd1);
        result[0] = 1'b1;
        update 32'd1 << 5'd2;
        yield result;
    }
}
]=])
else()
    set(input "${EXAMPLES}/p5.mdr")
endif()

execute_process(
    COMMAND "${MDRFRAME}" -o "${OUT}" --program "${IMAGE}" "${input}"
    RESULT_VARIABLE result ERROR_VARIABLE error
)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Verilog generation failed: ${error}")
endif()

file(SHA256 "${TB}" original)
file(SHA256 "${OUT}/mips_tb.v" generated)
if(NOT original STREQUAL generated)
    message(FATAL_ERROR "Generated testbench differs from the P5 testbench")
endif()

file(READ "${OUT}/mips.v" top)
foreach(stage IN ITEMS FDreg DEreg EMreg MWreg)
    if(NOT EXISTS "${OUT}/${stage}.v" OR NOT top MATCHES "${stage}[^;]*${stage}\\(")
        message(FATAL_ERROR "A dedicated ${stage} instance is required")
    endif()
endforeach()
file(READ "${OUT}/Controller.v" control)
if(NOT top MATCHES "Controller Controller\\(" OR NOT control MATCHES "wire ins_add"
        OR NOT control MATCHES "case \\(1'b1\\)" OR control MATCHES "always @\\(posedge")
    message(FATAL_ERROR "Expected shared combinational Controller and named decode flags")
endif()

file(REMOVE "${OUT}/generated-test.vvp" "${OUT}/wave.vcd")
execute_process(
    COMMAND "${IVERILOG}" -g2005 -s mips_tb -o generated-test.vvp -f files.f
    WORKING_DIRECTORY "${OUT}"
    RESULT_VARIABLE result ERROR_VARIABLE error
)
# The installed Windows Icarus driver can exit with an access violation
# after emitting a complete image. Require a fresh image and a clean vvp run.
if(NOT result EQUAL 0)
    if(NOT (WIN32 AND result MATCHES "^(-1073741819|3221225477|Exit code 0xc0000005|Access violation)$"
            AND error STREQUAL "" AND EXISTS "${OUT}/generated-test.vvp"))
        message(FATAL_ERROR "Icarus compilation failed (${result}): ${error}")
    endif()
endif()
execute_process(
    COMMAND "${VVP}" generated-test.vvp
    WORKING_DIRECTORY "${OUT}"
    RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE error
    TIMEOUT 20
)
if(NOT result EQUAL 0 OR NOT EXISTS "${OUT}/wave.vcd")
    message(FATAL_ERROR "Generated project did not finish (${result}): ${error}\n${log}")
endif()

function(expect_line line)
    string(FIND "${log}" "${line}" position)
    if(position EQUAL -1)
        message(FATAL_ERROR "Missing write '${line}'\n${log}")
    endif()
endfunction()

function(expect_delta first second cycles)
    string(REGEX MATCH "([0-9]+)@${first}:" from "${log}")
    set(start "${CMAKE_MATCH_1}")
    string(REGEX MATCH "([0-9]+)@${second}:" to "${log}")
    if(NOT from OR NOT to)
        message(FATAL_ERROR "Missing time for ${first} or ${second}\n${log}")
    endif()
    math(EXPR delta "${CMAKE_MATCH_1} - ${start}")
    math(EXPR expected "${cycles} * 2")
    if(NOT delta EQUAL expected)
        message(FATAL_ERROR "Unexpected time delta (${delta}, expected ${expected})\n${log}")
    endif()
endfunction()

if(DYNAMIC)
    expect_line("@0000300c: $14 <= 000000f0")
    expect_line("@00003010: $ 3 <= 000000fe")
    expect_line("@0000301c: $ 6 <= 00000002")
    expect_line("@00003024: $ 8 <= 0000000f")
    expect_line("@00003028: *00000004 <= 0000000f")
    expect_line("@0000302c: $ 9 <= 0000000f")
    expect_delta("0000300c" "00003010" 2)
    expect_delta("00003018" "0000301c" 1)
    expect_delta("00003020" "00003024" 2)
else()
    expect_line("@00003008: $ 3 <= 00000033")
    expect_line("@0000300c: *00000000 <= 00000033")
    expect_line("@00003014: $ 5 <= 00000044")
    expect_line("@00003020: *00000004 <= 00000044")
    expect_line("@0000302c: $31 <= 00003034")
    expect_line("@00003030: $ 9 <= 00003034")
    expect_line("@0000303c: $11 <= 000000bb")
    if(log MATCHES "@00003024:|@00003034:")
        message(FATAL_ERROR "Skipped code executed\n${log}")
    endif()
    expect_delta("00003010" "00003014" 2)
endif()

