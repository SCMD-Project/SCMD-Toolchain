if(NOT DEFINED SCMDSIM OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "check_single_file_input.cmake missing SCMDSIM/WORKDIR")
endif()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/root/nested")
file(WRITE "${WORKDIR}/root/nested/hello.cfg" "echoln SINGLE_FILE_PASS\nexec sibling\n")
file(WRITE "${WORKDIR}/root/nested/sibling.cfg" "echoln SIBLING_OK\n")
file(WRITE "${WORKDIR}/plain.txt" "not a cfg root")

execute_process(
    COMMAND "${SCMDSIM}" "${WORKDIR}/root/nested/hello.cfg" --no-interactive --no-engine-messages --no-ansi
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "scmdsim failed (${rc})\n${out}\n${err}")
endif()
foreach(needle IN ITEMS "SINGLE_FILE_PASS" "SIBLING_OK")
    string(FIND "${out}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "missing '${needle}':\n${out}\n${err}")
    endif()
endforeach()

execute_process(
    COMMAND "${SCMDSIM}" "${WORKDIR}/plain.txt" --no-interactive
    RESULT_VARIABLE rc2 OUTPUT_VARIABLE out2 ERROR_VARIABLE err2
)
if(rc2 EQUAL 0 OR NOT err2 MATCHES "not a directory")
    message(FATAL_ERROR "non-cfg file input was not rejected clearly:\n${out2}\n${err2}")
endif()
message(STATUS "single-file input PASS")
