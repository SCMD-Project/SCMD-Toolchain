cmake_minimum_required(VERSION 3.22)
project(scmdc VERSION 0.11.1 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SCMD_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)
option(SCMD_ENABLE_SANITIZERS "Enable AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

# Keep CMake/Ninja internals under out/, and place user-facing executables in
# dist/<flavor>/.  This avoids mixing tools with object files, caches, and test
# scratch data.  Presets set SCMD_DIST_FLAVOR explicitly; the fallback keeps
# ad-hoc CMake builds useful as well.
set(SCMD_DIST_FLAVOR "" CACHE STRING "Distribution flavor directory (debug/release/sanitize)")
if(NOT SCMD_DIST_FLAVOR)
    if(CMAKE_BUILD_TYPE)
        string(TOLOWER "${CMAKE_BUILD_TYPE}" SCMD_DIST_FLAVOR)
    else()
        set(SCMD_DIST_FLAVOR "default")
    endif()
endif()
set(SCMD_DIST_DIR "${CMAKE_SOURCE_DIR}/dist/${SCMD_DIST_FLAVOR}" CACHE PATH
    "Directory for user-facing SCMD executables")

add_library(scmd_core STATIC
    src/frontend/ast.c
    src/common/common.c
    src/frontend/lexer.c
    src/frontend/keywords.c
    src/frontend/parser.c
    src/frontend/comptime.c
    src/frontend/sema.c
    src/compiler/codegen.c
    src/compiler/loader.c
    src/compiler/project.c
)
target_include_directories(scmd_core PUBLIC include)

add_library(scmd_bytecode_core STATIC
    src/scb/bytecode.cpp
)
target_include_directories(scmd_bytecode_core PUBLIC include src/scb)

add_library(scmd_sim_core STATIC
    src/simulator/console_sim.cpp
    src/simulator/sos_sim.cpp
)
target_include_directories(scmd_sim_core PUBLIC include src/scb)
target_link_libraries(scmd_sim_core PUBLIC scmd_bytecode_core)

# vCS-16/2 is a CFG-friendly virtual execution architecture for host-side
# tooling and downstream runtimes. Keep the implementation in a library; CLI tools are
# deliberately thin front-ends over this API.
add_library(vcs16 STATIC
    src/vcs16/vcs16.cpp
)
target_include_directories(vcs16 PUBLIC include)

add_library(vcs16_scmd STATIC
    src/vcs16/scmd_codegen.cpp
)
target_include_directories(vcs16_scmd PUBLIC include)
target_link_libraries(vcs16_scmd PUBLIC vcs16)

add_executable(vcs16as tools/vcs16/vcs16as.cpp)
target_link_libraries(vcs16as PRIVATE vcs16)
add_executable(vcs16run tools/vcs16/vcs16run.cpp)
target_link_libraries(vcs16run PRIVATE vcs16)
add_executable(vcs16dump tools/vcs16/vcs16dump.cpp)
target_link_libraries(vcs16dump PRIVATE vcs16)
add_executable(vcs16scmd tools/vcs16/vcs16scmd.cpp)
target_link_libraries(vcs16scmd PRIVATE vcs16_scmd)

# windows.h defines min/max macros unless NOMINMAX is set. They collide with
# std::numeric_limits<T>::max() and other standard C++ APIs. Keep those legacy
# macros out of the simulator on Windows and reduce Win32 header pollution.
if(WIN32)
    target_compile_definitions(scmd_sim_core PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

add_executable(scmdc src/compiler/main.c)
target_link_libraries(scmdc PRIVATE scmd_core scmd_bytecode_core)

add_executable(scmdsim src/simulator/main.cpp)
target_link_libraries(scmdsim PRIVATE scmd_sim_core)

# Both public tools live in dist/. Static libraries and every other build
# artifact remain in the CMake binary tree under out/. The per-config
# properties also make this deterministic if someone uses a multi-config
# generator instead of the supported Ninja presets.
foreach(tool scmdc scmdsim vcs16as vcs16run vcs16dump vcs16scmd)
    set_target_properties(${tool} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${SCMD_DIST_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${SCMD_DIST_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${SCMD_DIST_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${SCMD_DIST_DIR}"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${SCMD_DIST_DIR}"
    )
endforeach()

# An explicit aggregate target makes the intended public build products clear
# and lets build scripts fail if either tool cannot be produced.
add_custom_target(scmd_tools ALL DEPENDS scmdc scmdsim vcs16as vcs16run vcs16dump vcs16scmd)


function(scmd_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(SCMD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
        if(SCMD_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

foreach(target scmd_core scmd_bytecode_core scmd_sim_core vcs16 vcs16_scmd scmdc scmdsim vcs16as vcs16run vcs16dump vcs16scmd)
    scmd_apply_warnings(${target})
endforeach()

if(SCMD_ENABLE_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "SCMD_ENABLE_SANITIZERS currently requires Clang/GCC-style flags")
    endif()
    foreach(target scmd_core scmd_bytecode_core scmd_sim_core vcs16 vcs16_scmd scmdc scmdsim vcs16as vcs16run vcs16dump vcs16scmd)
        target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    endforeach()
    target_link_options(scmdc PRIVATE -fsanitize=address,undefined)
    target_link_options(scmdsim PRIVATE -fsanitize=address,undefined)
    target_link_options(vcs16as PRIVATE -fsanitize=address,undefined)
    target_link_options(vcs16run PRIVATE -fsanitize=address,undefined)
    target_link_options(vcs16dump PRIVATE -fsanitize=address,undefined)
    target_link_options(vcs16scmd PRIVATE -fsanitize=address,undefined)
endif()

include(CheckIPOSupported)
check_ipo_supported(RESULT SCMD_IPO_SUPPORTED OUTPUT SCMD_IPO_ERROR)
if(SCMD_IPO_SUPPORTED)
    set_property(TARGET scmdc scmdsim vcs16as vcs16run vcs16dump vcs16scmd scmd_core scmd_bytecode_core scmd_sim_core vcs16 vcs16_scmd PROPERTY INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(vcs16_c_api_test tests/vcs16/c_api.c)
    target_link_libraries(vcs16_c_api_test PRIVATE vcs16)
    add_dependencies(scmd_tools vcs16_c_api_test)
    set_target_properties(vcs16_c_api_test PROPERTIES LINKER_LANGUAGE CXX)
    scmd_apply_warnings(vcs16_c_api_test)
    if(SCMD_ENABLE_SANITIZERS)
        target_compile_options(vcs16_c_api_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(vcs16_c_api_test PRIVATE -fsanitize=address,undefined)
    endif()

    add_test(NAME vcs16_c_api COMMAND $<TARGET_FILE:vcs16_c_api_test>)
    set_tests_properties(vcs16_c_api PROPERTIES PASS_REGULAR_EXPRESSION "VCS16_C_API_PASS")

    add_test(NAME vcs16_v2_smoke
        COMMAND $<TARGET_FILE:vcs16run> ${CMAKE_CURRENT_SOURCE_DIR}/tests/vcs16/smoke.vcs)
    set_tests_properties(vcs16_v2_smoke PROPERTIES PASS_REGULAR_EXPRESSION "55")

    add_test(NAME vcs16_vxe_roundtrip
        COMMAND ${CMAKE_COMMAND}
                -DAS=$<TARGET_FILE:vcs16as>
                -DRUN=$<TARGET_FILE:vcs16run>
                -DDUMP=$<TARGET_FILE:vcs16dump>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/vcs16/smoke.vcs
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/vcs16-vxe-roundtrip
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vcs16_vxe_roundtrip.cmake)

    add_test(NAME vcs16_v2_call_memory
        COMMAND $<TARGET_FILE:vcs16run> ${CMAKE_CURRENT_SOURCE_DIR}/tests/vcs16/call_memory.vcs)
    set_tests_properties(vcs16_v2_call_memory PROPERTIES PASS_REGULAR_EXPRESSION "42")

    add_test(NAME vcs16_v2_scmd_aot
        COMMAND ${CMAKE_COMMAND}
                -DVCS16SCMD=$<TARGET_FILE:vcs16scmd>
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/vcs16/aot_all.vcs
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/vcs16-scmd-aot-check
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_vcs16_scmd_aot.cmake)

    add_test(NAME scmdc_cli_help COMMAND $<TARGET_FILE:scmdc> --help)
    set_tests_properties(scmdc_cli_help PROPERTIES
        PASS_REGULAR_EXPRESSION "separate scmdsim tool"
        FAIL_REGULAR_EXPRESSION "console <cfg-root>"
    )

    add_test(NAME scmdsim_cli_help COMMAND $<TARGET_FILE:scmdsim> --help)
    set_tests_properties(scmdsim_cli_help PROPERTIES
        PASS_REGULAR_EXPRESSION "quit or exit to leave"
    )

    add_test(NAME scmd_tools_version_match
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DEXPECTED=${PROJECT_VERSION}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_versions.cmake
    )

    add_test(
        NAME scmdsim_profile_cs2_2026
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --profile cs2-2026
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/case.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(scmdsim_profile_cs2_2026 PROPERTIES PASS_REGULAR_EXPRESSION "CASE_PASS")

    add_test(
        NAME scmdsim_sos_compat
        COMMAND ${CMAKE_COMMAND}
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/sos.script
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_sos_compat.cmake
    )

    add_test(
        NAME scmdsim_echo_ui
        COMMAND ${CMAKE_COMMAND}
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_echo_ui.cmake
    )

    add_test(
        NAME scmdsim_unknown_profile_rejected
        COMMAND ${CMAKE_COMMAND}
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_bad_profile.cmake
    )

    add_test(
        NAME scmdsim_cs2_command_length
        COMMAND ${CMAKE_COMMAND}
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_sim_command_limit.cmake
    )

    function(scmd_compile_test name source)
        add_test(
            NAME ${name}
            COMMAND $<TARGET_FILE:scmdc>
                    ${CMAKE_CURRENT_SOURCE_DIR}/${source}
                    -o ${CMAKE_CURRENT_BINARY_DIR}/${name}.cfg
                    --console-mode sync
        )
    endfunction()

    scmd_compile_test(scmd_core tests/v06_core.scmd)
    scmd_compile_test(scmd_arithmetic tests/arithmetic.scmd)
    scmd_compile_test(scmd_ripple_runtime tests/ripple_runtime.scmd)
    scmd_compile_test(scmd_loops tests/loops.scmd)
    scmd_compile_test(scmd_block tests/block.scmd)
    scmd_compile_test(scmd_same_line_brace tests/style_same_line.scmd)
    scmd_compile_test(scmd_multifile tests/multifile/main.scmd)
    scmd_compile_test(scmd_unicode examples/unicode.scmd)
    scmd_compile_test(scmd_adder8 examples/adder8.scmd)

    add_test(
        NAME scmd_optimizer
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/optimizer.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/optimizer-check
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_optimizer.cmake
    )

    add_test(
        NAME scmd_cs2_command_length
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/optimizer_command_limit.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/command-limit-check
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_command_limit.cmake
    )

    add_test(
        NAME scmd_state_storage_narrowing
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/state_narrowing.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/state-narrowing-check
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_state_narrowing.cmake
    )

    add_test(
        NAME scmd_mandatory_function_demand_loading
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/lazy_functions.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/lazy-function-check
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_lazy_functions.cmake
    )

    add_test(
        NAME scmd_resident_function
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/resident_function.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/resident-function-check
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_resident_function.cmake
    )

    add_test(
        NAME scmd_static_main_bootstrap
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/static_main_bootstrap.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/static-main-check
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_static_main_bootstrap.cmake
    )

    add_test(
        NAME scmd_missing_if_parentheses_rejected
        COMMAND $<TARGET_FILE:scmdc>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/invalid_no_paren.scmd
                -o ${CMAKE_CURRENT_BINARY_DIR}/invalid.cfg
                --console-mode sync
    )
    set_tests_properties(scmd_missing_if_parentheses_rejected PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME scmd_recursion_rejected
        COMMAND $<TARGET_FILE:scmdc>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/recursive.scmd
                -o ${CMAKE_CURRENT_BINARY_DIR}/recursive.cfg
                --console-mode sync
    )
    set_tests_properties(scmd_recursion_rejected PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME sim_alias_trailing_args
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --exec alias
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/alias.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_alias_trailing_args PROPERTIES
        PASS_REGULAR_EXPRESSION "\\[ARG\\]"
        FAIL_REGULAR_EXPRESSION "\\[ARG\\] 12345"
    )

    add_test(
        NAME sim_pipe_is_literal
        COMMAND ${CMAKE_COMMAND}
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/pipe.script
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_pipe_literal.cmake
    )

    add_test(
        NAME scmd_export_function
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/export_function.scmd
                -DSCRIPT=${CMAKE_CURRENT_SOURCE_DIR}/tests/export_function.script
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/export-function-check
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_export_function.cmake
    )

    add_test(
        NAME scmd_export_reserved_kill_rejected
        COMMAND $<TARGET_FILE:scmdc>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/reserved_export_kill.scmd
                -o ${CMAKE_CURRENT_BINARY_DIR}/reserved-kill.cfg
                --console-mode sync
    )
    set_tests_properties(scmd_export_reserved_kill_rejected PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME scmd_export_reserved_version_rejected
        COMMAND $<TARGET_FILE:scmdc>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/reserved_export_version.scmd
                -o ${CMAKE_CURRENT_BINARY_DIR}/reserved-version.cfg
                --console-mode sync
    )
    set_tests_properties(scmd_export_reserved_version_rejected PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME sim_cs2_builtin_precedes_alias
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --exec builtin_collision
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_cs2_builtin_precedes_alias PROPERTIES
        PASS_REGULAR_EXPRESSION "BUILTIN_COLLISION_OK"
        FAIL_REGULAR_EXPRESSION "BAD_KILL_ALIAS|BAD_HELP_ALIAS|BAD_VERSION_ALIAS"
    )


    add_test(
        NAME sim_async_scheduler
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --exec async
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_async_scheduler PROPERTIES
        PASS_REGULAR_EXPRESSION "A\nB\nC"
    )

    add_test(
        NAME sim_incrementvar_wrap
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/increment.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_incrementvar_wrap PROPERTIES
        PASS_REGULAR_EXPRESSION "tv_window_size = 0"
    )

    add_test(
        NAME sim_loop_budget
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/loop.script
                --no-interactive --no-engine-messages --no-ansi --max-commands 1000
    )
    set_tests_properties(sim_loop_budget PROPERTIES WILL_FAIL TRUE)

    add_test(
        NAME scmd_runtime_smoke
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_smoke.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/runtime-smoke
                -DNAME=runtime_smoke
                -DEXPECT=RUNTIME_SMOKE_PASS
                -DMODE=sync
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_compiled_sim.cmake
    )

    add_test(
        NAME scmd_runtime_async_smoke
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_async.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/runtime-async
                -DNAME=runtime_async
                -DEXPECT=ASYNC_RUNTIME_PASS
                -DMODE=async
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_compiled_sim.cmake
    )

    add_test(
        NAME scmd_project_run_smoke
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/examples/adder8-project/adder8.scmdproj
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/project-smoke-adder8
                -DEXEC_NAME=adder8
                -DEXPECT=200.*100.*44.*PASS
                -DMAX_COMMANDS=5000000
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_project_sim.cmake
    )

    add_test(
        NAME sim_alias_state_rebinding
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --exec alias_state
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_alias_state_rebinding PROPERTIES
        PASS_REGULAR_EXPRESSION "ON\nOFF\nON"
    )

    add_test(
        NAME sim_quoted_semicolon
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --exec quote
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_quoted_semicolon PROPERTIES
        PASS_REGULAR_EXPRESSION "X;Y\nA\nB"
    )

    add_test(
        NAME sim_nested_exec
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --exec nested_root
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_nested_exec PROPERTIES
        PASS_REGULAR_EXPRESSION "ROOT_A\nCHILD\nROOT_B"
    )

    add_test(
        NAME sim_async_virtual_time_order
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --exec async_order
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_async_virtual_time_order PROPERTIES
        PASS_REGULAR_EXPRESSION "ROOT_A\nROOT_B\nFAST\nSLOW"
    )

    add_test(
        NAME sim_path_traversal_rejected
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/traversal.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_path_traversal_rejected PROPERTIES
        PASS_REGULAR_EXPRESSION "invalid cfg path"
        FAIL_REGULAR_EXPRESSION "TRAVERSAL_BAD"
    )

    add_test(
        NAME sim_case_insensitive_commands_aliases
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/case.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_case_insensitive_commands_aliases PROPERTIES
        PASS_REGULAR_EXPRESSION "CASE_PASS"
    )

    add_test(
        NAME sim_execifexists_missing_is_nonfatal
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/execifexists.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_execifexists_missing_is_nonfatal PROPERTIES
        PASS_REGULAR_EXPRESSION "EXECIFEXISTS_PASS"
        FAIL_REGULAR_EXPRESSION "couldn't exec"
    )

    add_test(
        NAME sim_script_meta_quit
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/meta_quit.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(sim_script_meta_quit PROPERTIES
        PASS_REGULAR_EXPRESSION "BEFORE_QUIT"
        FAIL_REGULAR_EXPRESSION "AFTER_QUIT_BAD"
    )

    add_test(
        NAME scmd_const_edge_semantics
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/const_edges.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/const-edges
                -DNAME=const_edges
                -DEXPECT=CONST_EDGES_PASS
                -DMODE=sync
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_compiled_sim.cmake
    )

    add_test(
        NAME scmd_project_clean_rebuild
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/project-clean-rebuild
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_project_clean_rebuild.cmake
    )

    add_test(
        NAME scmd_array_load_controlflow
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/array_load_controlflow.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/array-load-controlflow
                -DNAME=array_load_controlflow
                -DEXPECT=ARRAY_LOAD_CONTROLFLOW_PASS
                -DMODE=sync
                -DMAX_COMMANDS=2000000
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_compiled_sim.cmake
    )

    add_test(
        NAME scmd_arithmetic_matrix
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/arithmetic_matrix.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/arithmetic-matrix
                -DNAME=arithmetic_matrix
                -DEXPECT=ARITH_MATRIX_PASS
                -DMODE=sync
                -DMAX_COMMANDS=20000000
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_compiled_sim.cmake
    )
    set_tests_properties(scmd_arithmetic_matrix PROPERTIES TIMEOUT 60)

    add_test(
        NAME vcs16_a1_simulator_bringup
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/examples/vcs16-a1/vcs16-a1.scmdproj
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/project-smoke-vcs16
                -DEXEC_NAME=vcs16_a1
                -DEXPECT=vCS-16.A1.CORE:.PASS
                -DMAX_COMMANDS=2000000
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_project_sim.cmake
    )
    set_tests_properties(vcs16_a1_simulator_bringup PROPERTIES TIMEOUT 20)
endif()

install(TARGETS scmdc scmdsim vcs16as vcs16run vcs16dump vcs16scmd
    RUNTIME DESTINATION bin)
install(TARGETS vcs16 vcs16_scmd
    ARCHIVE DESTINATION lib
    LIBRARY DESTINATION lib
    RUNTIME DESTINATION bin)
install(DIRECTORY include/vcs16 DESTINATION include)

if(BUILD_TESTING)
    add_test(
        NAME scmd_bytecode_roundtrip
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DROOT=${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/roundtrip.scb
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bytecode_roundtrip.cmake
    )

    add_test(
        NAME scmd_bytecode_invalid_rejected
        COMMAND ${CMAKE_COMMAND}
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DBAD=${CMAKE_CURRENT_BINARY_DIR}/bad.scb
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/bad_scb.cmake
    )

    add_test(
        NAME scmdsim_exec_path_completion
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/complete_exec.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(scmdsim_exec_path_completion PROPERTIES
        PASS_REGULAR_EXPRESSION "nested/child"
    )

    add_test(
        NAME scmdsim_default_current_directory
        COMMAND $<TARGET_FILE:scmdsim>
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/case.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(scmdsim_default_current_directory PROPERTIES
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
        PASS_REGULAR_EXPRESSION "CASE_PASS"
    )

    add_test(
        NAME vcs16_a1_bytecode_bringup
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DPROJECT=${CMAKE_CURRENT_SOURCE_DIR}/examples/vcs16-a1/vcs16-a1.scmdproj
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/project-bytecode-vcs16
                -DEXEC_NAME=vcs16_a1
                -DEXPECT=vCS-16.A1.CORE:.PASS
                -DMAX_COMMANDS=2000000
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_project_scb.cmake
    )
    set_tests_properties(vcs16_a1_bytecode_bringup PROPERTIES TIMEOUT 20)

    add_test(
        NAME scmdsim_plain_quit
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/quit_plain.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(scmdsim_plain_quit PROPERTIES
        FAIL_REGULAR_EXPRESSION "SHOULD_NOT_RUN"
    )

    add_test(
        NAME scmdsim_plain_exit
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/exit_plain.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(scmdsim_plain_exit PROPERTIES
        FAIL_REGULAR_EXPRESSION "SHOULD_NOT_RUN"
    )

    add_test(
        NAME scmdsim_lazy_cfg_mode
        COMMAND $<TARGET_FILE:scmdsim>
                ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/cfg
                --script ${CMAKE_CURRENT_SOURCE_DIR}/tests/sim/lazy_stats.script
                --no-interactive --no-engine-messages --no-ansi
    )
    set_tests_properties(scmdsim_lazy_cfg_mode PROPERTIES
        PASS_REGULAR_EXPRESSION "source=cfg-lazy.*lazy_compiles=2"
    )

    add_test(
        NAME scmdsim_lazy_cache_reload
        COMMAND ${CMAKE_COMMAND}
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/lazy-cache-test
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/lazy_cache.cmake
    )

    add_test(
        NAME scmdsim_single_file_input
        COMMAND ${CMAKE_COMMAND}
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/single-file-test
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_single_file_input.cmake
    )

    add_test(
        NAME scmd_comptime_arrays
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/comptime_arrays.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/comptime-arrays
                -DNAME=comptime_arrays
                -DEXPECT=COMPTIME_ARRAY_PASS
                -DMODE=sync
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_compiled_sim.cmake
    )

    add_test(
        NAME scmd_volatile_noopt
        COMMAND ${CMAKE_COMMAND}
                -DSCMD=$<TARGET_FILE:scmdc>
                -DSCMDSIM=$<TARGET_FILE:scmdsim>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/volatile_noopt.scmd
                -DOUTDIR=${CMAKE_CURRENT_BINARY_DIR}/volatile-noopt
                -DNAME=volatile_noopt
                -DEXPECT=VOLATILE_NOOPT_PASS
                -DMODE=sync
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/run_compiled_sim.cmake
    )

    add_test(
        NAME scmd_volatile_noopt_shape
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/volatile_noopt.scmd
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/volatile-noopt-shape
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_volatile_noopt_shape.cmake
    )

    add_test(
        NAME scmd_array_compound_rejected
        COMMAND ${CMAKE_COMMAND}
                -DSCMDC=$<TARGET_FILE:scmdc>
                -DSOURCE=${CMAKE_CURRENT_SOURCE_DIR}/tests/array_compound_invalid.scmd
                -DOUT=${CMAKE_CURRENT_BINARY_DIR}/array-compound-invalid.cfg
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_array_compound_rejected.cmake
    )

endif()
