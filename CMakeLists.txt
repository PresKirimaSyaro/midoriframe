cmake_minimum_required(VERSION 3.20)
project(mdrframe VERSION 0.1.0 LANGUAGES CXX)

add_executable(mdrframe
    src/main.cpp
    src/parser.cpp
    src/lower.cpp
    src/analysis.cpp
    src/print.cpp
)
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
endif()

