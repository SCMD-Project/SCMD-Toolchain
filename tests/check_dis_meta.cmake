if(NOT DEFINED SCMDSIM OR NOT DEFINED WORKDIR)
    message(FATAL_ERROR "check_dis_meta.cmake missing SCMDSIM/WORKDIR")
endif()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}/root")
file(WRITE "${WORKDIR}/root/demo.cfg" "alias toggle_state \"echoln T\"\ntoggle_state\n")
file(WRITE "${WORKDIR}/run.script" ":dis demo\n:dis toggle_state\n:dis definitely_missing\n")

execute_process(
    COMMAND "${SCMDSIM}" "${WORKDIR}/root" --script "${WORKDIR}/run.script" --no-interactive --no-engine-messages --no-ansi
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "scmdsim failed (${rc})\n${out}\n${err}")
endif()
foreach(needle IN ITEMS "alias_set_i" "dispatch0" "'toggle_state'" "block " "unknown module or alias")
    string(FIND "${out}" "${needle}" pos)
    if(pos EQUAL -1)
        message(FATAL_ERROR "disassembly missing '${needle}':\n${out}\n${err}")
    endif()
endforeach()
message(STATUS ":dis meta PASS")