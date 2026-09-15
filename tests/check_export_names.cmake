file(MAKE_DIRECTORY "${OUTDIR}")
foreach(name long case builtin_case)
    execute_process(COMMAND "${SCMDC}" "${ROOT}/export_${name}_invalid.scmd" -o "${OUTDIR}/${name}.cfg"
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(rc EQUAL 0 OR NOT err MATCHES "exported")
        message(FATAL_ERROR "invalid exported console name accepted: ${name}: ${out} ${err}")
    endif()
endforeach()
