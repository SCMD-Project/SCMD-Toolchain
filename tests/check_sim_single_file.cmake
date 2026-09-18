file(REMOVE_RECURSE "${OUTDIR}")
file(MAKE_DIRECTORY "${OUTDIR}")
file(WRITE "${OUTDIR}/hello.v1.cfg" "echoln SINGLE_FILE_PASS\nexec sibling\n")
file(WRITE "${OUTDIR}/hello.cfg" "echoln WRONG_FILE_SELECTED\n")
file(WRITE "${OUTDIR}/sibling.cfg" "echoln SIBLING_OK\n")
file(WRITE "${OUTDIR}/app.v1.cfg" "echoln DOTTED_MODULE_OK\n")
file(WRITE "${OUTDIR}/driver.cfg" "exec app.v1\n")
file(WRITE "${OUTDIR}/notes.txt" "not a cfg root\n")

execute_process(COMMAND "${SCMDSIM}" "${OUTDIR}/hello.v1.cfg"
    --no-interactive --no-engine-messages --no-ansi
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "SINGLE_FILE_PASS" OR NOT out MATCHES "SIBLING_OK" OR
   out MATCHES "WRONG_FILE_SELECTED")
    message(FATAL_ERROR "single .cfg input failed: rc=${rc}\n${out}\n${err}")
endif()

execute_process(COMMAND "${SCMDSIM}" "${OUTDIR}" --exec driver
    --no-interactive --no-engine-messages --no-ansi
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT rc EQUAL 0 OR NOT out MATCHES "DOTTED_MODULE_OK")
    message(FATAL_ERROR "dotted module exec failed: rc=${rc}\n${out}\n${err}")
endif()

execute_process(COMMAND "${SCMDSIM}" "${OUTDIR}/notes.txt"
    --no-interactive --no-engine-messages --no-ansi
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(rc EQUAL 0 OR NOT err MATCHES "cfg root is not a directory")
    message(FATAL_ERROR "non-CFG input error was unclear: rc=${rc}\n${out}\n${err}")
endif()

message(STATUS "SIM_SINGLE_CFG_PASS: exact file input, sibling exec, dotted module paths")
