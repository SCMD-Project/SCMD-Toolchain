if(NOT DEFINED SCMDC OR NOT DEFINED SCMDSIM OR NOT DEFINED SOURCE OR NOT DEFINED OUTDIR)
    message(FATAL_ERROR "SCMDC, SCMDSIM, SOURCE and OUTDIR are required")
endif()
file(REMOVE_RECURSE "${OUTDIR}")
file(MAKE_DIRECTORY "${OUTDIR}")
set(OUTCFG "${OUTDIR}/output.cfg")
execute_process(
    COMMAND "${SCMDC}" "${SOURCE}" -o "${OUTCFG}" --console-mode sync
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err
)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "compile failed (${rc})\n${out}\n${err}")
endif()
file(GLOB_RECURSE cfg_files "${OUTDIR}/*.cfg")
set(cfg_text "")
foreach(f IN LISTS cfg_files)
    file(READ "${f}" t)
    string(APPEND cfg_text "\n${t}")
endforeach()
# Bit variables hold the strings __scmd_true/__scmd_false, and external cfg
# (input frontends) write those sentinels too. The optimizer must keep the
# sentinel dispatch chain intact instead of folding it away.
foreach(sentinel
        "alias __scmd_true \"__scmd_branch_true\""
        "alias __scmd_false \"__scmd_branch_false\""
        "alias __scmd_branch_true \"\""
        "alias __scmd_branch_false \"\"")
    if(NOT cfg_text MATCHES "${sentinel}")
        message(FATAL_ERROR "optimized cfg dropped sentinel alias: ${sentinel}")
    endif()
endforeach()
execute_process(
    COMMAND "${SCMDSIM}" "${OUTDIR}" --exec output --no-interactive --no-engine-messages --no-ansi --max-commands 100000
    RESULT_VARIABLE sim_rc OUTPUT_VARIABLE sim_out ERROR_VARIABLE sim_err
)
if(NOT sim_rc EQUAL 0 OR NOT sim_out MATCHES "CFG_SENTINEL_PASS")
    message(FATAL_ERROR "simulation failed (${sim_rc})\n${sim_out}\n${sim_err}")
endif()
