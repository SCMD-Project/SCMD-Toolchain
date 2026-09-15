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
    bool real_time;      /* sleep also waits real milliseconds (demo pacing) */
    uint64_t max_commands;
} ScmdSimOptions;

int scmd_simulator_run(const ScmdSimOptions *options);

#ifdef __cplusplus
}
#endif

#endif
