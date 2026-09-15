if(NOT DEFINED SCMD OR NOT DEFINED SCMDSIM OR NOT DEFINED SOURCE OR NOT DEFINED OUTDIR OR NOT DEFINED NAME OR NOT DEFINED EXPECT OR NOT DEFINED MODE)
    message(FATAL_ERROR "run_compiled_sim.cmake missing required -D argument")
endif()
if(NOT DEFINED MAX_COMMANDS)
    set(MAX_COMMANDS 5000000)
endif()
file(MAKE_DIRECTORY "${OUTDIR}")
set(cfg "${OUTDIR}/${NAME}.cfg")
execute_process(
    COMMAND "${SCMD}" "${SOURCE}" -o "${cfg}" --console-mode "${MODE}"
    RESULT_VARIABLE compile_rc
    OUTPUT_VARIABLE compile_out
    ERROR_VARIABLE compile_err
)
if(NOT compile_rc EQUAL 0)
    message(FATAL_ERROR "compile failed (${compile_rc})\n${compile_out}\n${compile_err}")
endif()
execute_process(
    COMMAND "${SCMDSIM}" "${OUTDIR}" --exec "${NAME}" --no-interactive --no-engine-messages --no-ansi --strict --max-commands "${MAX_COMMANDS}"
    RESULT_VARIABLE sim_rc
    OUTPUT_VARIABLE sim_out
    ERROR_VARIABLE sim_err
)
if(NOT sim_rc EQUAL 0)
    message(FATAL_ERROR "simulation failed (${sim_rc})\n${sim_out}\n${sim_err}")
endif()
if(NOT sim_out MATCHES "${EXPECT}")
    message(FATAL_ERROR "expected '${EXPECT}' not found\n--- stdout ---\n${sim_out}\n--- stderr ---\n${sim_err}")
endif()
if(sim_out MATCHES "_FAIL")
    message(FATAL_ERROR "runtime reported failure\n${sim_out}")
endif()
