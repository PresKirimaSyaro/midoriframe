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


compile(ranges [=[
test {
    match(ins[31:26] == 6'h3e);
    E {
        acc: 32 = 32'd0;
        for (i = 7; i >= 0; i = i - 2) acc = acc * 10 + i;
        for (i = 1; i <= 7; i = i + 3) acc = acc * 10 + i;
        for (i = 6; i > 1; i = i - 3) acc = acc * 10 + i;
        for (i = 0; i >= 1; i = i + 1) acc = acc * 10 + i;
        update 32'd1 << 5'd1;
        yield acc;
    }
}
]=])

string(REGEX MATCHALL [=[literal "32'sd[0-9]+"]=] indices "${ranges}")
string(REGEX REPLACE [=[literal "32'sd([0-9]+)"]=] "\\1" indices "${indices}")
if(NOT indices STREQUAL "7;5;3;1;1;4;7;6;3")
    message(FATAL_ERROR "Unexpected indices from stepped loops: ${indices}")
endif()

set(path "${OUT}/bad_direction.mdr")
file(WRITE "${path}" "test { match(ins[31:26] == 6'h3e); E { for (i = 31; i >= 0; i = i + 1) update 32'd0; } }\n")
execute_process(
    COMMAND "${MDRFRAME}" --emit=deps "${path}"
    RESULT_VARIABLE result ERROR_VARIABLE error
)
if(result EQUAL 0 OR NOT error MATCHES "moves away from its termination bound")
    message(FATAL_ERROR "Expected direction diagnostic: ${error}")
endif()


set(lwxsel [=[
lwxsel {
    match(ins[31-:6] == 6'b11_0111);
    base: 5 = ins[25:21];
    rt: 5 = ins[20:16];
    off: 16 = ins[15:0];
    D {
        update (32'd1 << rt) | 32'h0000_ffff;
    }

    addr: 32;
    scale: 32 = {{14{off[15]}}, off, 2'b00};

    E {
        addr = grf_read(base) + scale;
    }
    M {
        word: 32 = dm_read(addr, 4);
        mixed: 32 = word ^ grf_read(rt);
        yield word;
        if (mixed[31]) update 32'd1 << rt;
        else update 32'd1 << mixed[3:0];
    }
}
]=])
compile(interleaved "${lwxsel}")
set(declarations "    addr: 32;\n    scale: 32 = {{14{off[15]}}, off, 2'b00};\n")
string(REPLACE "${declarations}" "" before_stages "${lwxsel}")
string(REPLACE "    D {" "${declarations}    D {" before_stages "${before_stages}")
compile(hoisted "${before_stages}")
if(NOT interleaved STREQUAL hoisted)
    message(FATAL_ERROR "Moving declarations before D changed a program that first uses them in E")
endif()

set(path "${OUT}/use_before_declaration.mdr")
file(WRITE "${path}" "test { match(ins[31:26] == 6'h3e); D { addr = 32'd4; } addr: 32; }\n")
execute_process(
    COMMAND "${MDRFRAME}" --emit=deps "${path}"
    RESULT_VARIABLE result ERROR_VARIABLE error
)
if(result EQUAL 0 OR NOT error MATCHES "unknown variable 'addr'")
    message(FATAL_ERROR "Expected use-before-declaration diagnostic: ${error}")
endif()
