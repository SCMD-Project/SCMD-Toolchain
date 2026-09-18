#ifndef SCMD_SIMULATOR_H
#define SCMD_SIMULATOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ScmdSimOptions {
    /* A CFG root directory or a precompiled .scb package. NULL/empty means '.'. */
    const char *cfg_root;
    const char *startup_exec;
    const char *script_path;
    const char *profile;
    const char *save_scb_path;
    const char *cache_dir;
    bool interactive;
    bool use_cache;
    bool precompile;
    bool trace;
    bool ansi_clear;
    bool engine_messages;
    uint64_t max_commands;
    /* Deterministic stress knobs, not measurements of real CS2 timings. */
    uint64_t echo_delay_ms;
    uint64_t exec_latency_ms;
    /* Pace virtual-time advances against wall time. Disabled by default. */
    bool real_time;
    bool strict; /* fail on unknown commands, missing execs and rejected aliases */
} ScmdSimOptions;

int scmd_simulator_run(const ScmdSimOptions *options);

#ifdef __cplusplus
}
#endif

#endif
