file(MAKE_DIRECTORY "${OUT}")

function(compile name source)
    set(path "${OUT}/${name}.mdr")
    file(WRITE "${path}" "${source}")
    execute_process(
        COMMAND "${MDRFRAME}" --emit=deps "${path}"
        RESULT_VARIABLE result OUTPUT_VARIABLE ir ERROR_VARIABLE error
    )
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "${name}: ${error}")
    endif()
    set("${name}" "${ir}" PARENT_SCOPE)
endfunction()

compile(unbraced [=[
test {
    match(ins[31:26] == 6'h3e);
    E {
        acc: 32 = 32'd0;
        if (ins[0])
            if (ins[1])
                acc = 32'd1;
            else if (ins[2])
                acc = 32'd2;
            else
                acc = 32'd3;
        for (i = 0; i < 2; i = i + 1)
            if (ins[i])
                acc = acc + 32'd4;
        update 32'd1 << 5'd1;
        yield acc;
    }
}
]=])

compile(braced [=[
test {
    match(ins[31:26] == 6'h3e);
    E {
        acc: 32 = 32'd0;
        if (ins[0]) {
            if (ins[1]) {
                acc = 32'd1;
            } else {
                if (ins[2]) {
                    acc = 32'd2;
                } else {
                    acc = 32'd3;
                }
            }
        }
        for (i = 0; i < 2; i = i + 1) {
            if (ins[i]) {
                acc = acc + 32'd4;
            }
        }
        update 32'd1 << 5'd1;
        yield acc;
    }
}
]=])

if(NOT unbraced STREQUAL braced)
    message(FATAL_ERROR "Single-statement bodies differ from their braced equivalent")
endif()

foreach(stage IN ITEMS D E M)
    set(path "${OUT}/missing_${stage}_braces.mdr")
    file(WRITE "${path}" "test { match(ins[31:26] == 6'h3e); ${stage} update 32'd0; }\n")
    execute_process(
        COMMAND "${MDRFRAME}" --emit=deps "${path}"
        RESULT_VARIABLE result ERROR_VARIABLE error
    )
    string(FIND "${error}" "expected '{'" diagnostic)
    if(result EQUAL 0 OR diagnostic EQUAL -1)
        message(FATAL_ERROR "${stage} must require braces: ${error}")
    endif()
endforeach()
