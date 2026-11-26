cmake_minimum_required(VERSION 3.20)
project(midoriframe VERSION 0.1.0 LANGUAGES CXX)

add_executable(mdrframe
    src/main.cpp
    src/parser.cpp
    src/lower.cpp
    src/analysis.cpp
    src/print.cpp
    src/verilog.cpp
    src/verilog_controller.cpp
    src/verilog_top.cpp
)
set(rtl_resources "")
foreach(file IN ITEMS IM.v DM.v RF.v STALL.v FDreg.v DEreg.v EMreg.v MWreg.v Promise.vh mips_tb.v)
    set(path "${CMAKE_CURRENT_SOURCE_DIR}/rtl/${file}")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    file(READ "${path}" bytes HEX)
    string(REGEX REPLACE "(..)" "0x\\1," bytes "${bytes}")
    string(MAKE_C_IDENTIFIER "${file}" name)
    string(APPEND rtl_resources "inline constexpr unsigned char ${name}[] = {${bytes}};\n")
endforeach()
file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/rtl_resources.hpp"
    CONTENT "#pragma once\nnamespace mdr::rtl {\n@rtl_resources@}\n" @ONLY)
target_include_directories(mdrframe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(mdrframe PRIVATE cxx_std_20)
set_target_properties(mdrframe PROPERTIES CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(mdrframe PRIVATE /W4 /permissive-)
else()
    target_compile_options(mdrframe PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(MINGW)
    # Keep the CLI runnable outside the MSYS2 environment used to build it.
    target_link_options(mdrframe PRIVATE -static)
endif()

include(CTest)
if(BUILD_TESTING)
    add_test(NAME cli_help COMMAND mdrframe --help)
    set_tests_properties(cli_help PROPERTIES PASS_REGULAR_EXPRESSION "Usage: mdrframe")
    if(MINGW)
        set_tests_properties(cli_help PROPERTIES ENVIRONMENT "PATH=$ENV{SystemRoot}/System32")
    endif()

    function(check_output name example)
        add_test(NAME "${name}" COMMAND "${CMAKE_COMMAND}"
            "-DMDRFRAME=$<TARGET_FILE:mdrframe>"
            "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/examples/${example}.mdr"
            "-DEXPECT=${ARGN}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_output.cmake"
        )
    endfunction()

    check_output(p5_decode p5 "instructions=16" "decode=first-match" "dm.write.4")
    check_output(dynamic_destination lrm "initial_raw_upper=0xfffffffe"
        "tuse=2" "rf-address+candidate" "ready_by=M->W")
    check_output(conditional_ready conditional "tuse=1" "decode_enable=!"
        "ready_by=E->M" "ready_by=M->W")

    add_test(NAME control_bodies COMMAND "${CMAKE_COMMAND}"
        "-DMDRFRAME=$<TARGET_FILE:mdrframe>"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/control-bodies"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_control_bodies.cmake"
    )

    add_test(NAME memory_stage COMMAND "${CMAKE_COMMAND}"
        "-DMDRFRAME=$<TARGET_FILE:mdrframe>"
        "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/memory-stage"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_memory_stage.cmake"
    )

    find_program(MDR_IVERILOG iverilog)
    find_program(MDR_VVP vvp)
    if(MDR_IVERILOG AND MDR_VVP)
        function(check_verilog name image dynamic)
            add_test(NAME "${name}" COMMAND "${CMAKE_COMMAND}"
                "-DMDRFRAME=$<TARGET_FILE:mdrframe>"
                "-DIVERILOG=${MDR_IVERILOG}" "-DVVP=${MDR_VVP}"
                "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/${name}"
                "-DEXAMPLES=${CMAKE_CURRENT_SOURCE_DIR}/examples"
                "-DIMAGE=${CMAKE_CURRENT_SOURCE_DIR}/tests/${image}.hex"
                "-DTB=${CMAKE_CURRENT_SOURCE_DIR}/rtl/mips_tb.v"
                "-DDYNAMIC=${dynamic}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_verilog.cmake"
            )
        endfunction()
        check_verilog(rtl_p5 p5_smoke OFF)
        check_verilog(rtl_dynamic dynamic_promise ON)
    endif()
endif()

