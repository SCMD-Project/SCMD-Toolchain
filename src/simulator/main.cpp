#include "scmd/simulator.h"
#include "scmd/version.h"

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace {

void usage(const char *argv0) {
    std::cout << "scmdsim " << SCMD_VERSION << "\n\n"
              << "Usage:\n"
              << "  " << argv0 << " [cfg-root|package.scb] [options]\n\n"
              << "If the input is omitted, the current directory is opened lazily.\n"
              << "A single .cfg file is also accepted: its directory becomes the CFG root\n"
              << "and the file itself is executed on startup.\n\n"
              << "CFG modules compile on first exec and hot-reload when their source changes.\n\n"
              << "Options:\n"
              << "  --exec NAME             execute NAME.cfg before entering the console\n"
              << "  --script FILE           replay Console input from FILE\n"
              << "  --save-scb FILE         precompile the whole CFG root and save an SCB package\n"
              << "  --precompile            precompile every CFG module before entering the console\n"
              << "  --cache                 enable persistent per-module SCB cache\n"
              << "  --cache-dir DIR         cache directory (implies --cache; default .scmdcache)\n"
              << "  --no-cache              disable persistent per-module bytecode cache\n"
              << "  --no-interactive        exit after startup/script work completes\n"
              << "  --trace                 trace SCB VM instructions to stderr\n"
              << "  --no-engine-messages    hide modeled CS2 engine messages\n"
              << "  --no-ansi               do not translate clear to terminal ANSI clear\n"
              << "  --max-commands N        Console command budget (default 10000000)\n"
              << "  --profile " << SCMD_CS2_PROFILE << "    select the verified compatibility profile\n"
              << "  --help                  show this help\n"
              << "  --version               print version\n\n"
              << "Interactive mode supports Tab completion. `exec`, `execifexists`, and\n"
              << "`exec_async` use virtual CFG-path completion from the compiled package.\n"
              << "Type quit or exit to leave. Meta-commands such as :stats remain available.\n";
}

bool parse_u64(const char *name, const char *text, uint64_t &out) {
    if (!text || !*text || *text == '-') {
        std::cerr << "error: " << name << " expects an unsigned integer\n";
        return false;
    }
    errno = 0;
    char *end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || value > std::numeric_limits<uint64_t>::max()) {
        std::cerr << "error: " << name << " expects an unsigned integer, got '" << text << "'\n";
        return false;
    }
    out = static_cast<uint64_t>(value);
    return true;
}

} // namespace

int main(int argc, char **argv) {
    ScmdSimOptions opts{};
    opts.cfg_root = ".";
    opts.startup_exec = nullptr;
    opts.script_path = nullptr;
    opts.profile = SCMD_CS2_PROFILE;
    opts.save_scb_path = nullptr;
    opts.cache_dir = nullptr;
    opts.interactive = true;
    opts.use_cache = false;
    opts.precompile = false;
    opts.trace = false;
    opts.ansi_clear = true;
    opts.engine_messages = true;
    opts.max_commands = 10000000ULL;

    bool input_seen = false;
    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
            usage(argv[0]);
            return 0;
        }
        if (std::strcmp(arg, "--version") == 0) {
            std::cout << SCMD_VERSION << '\n';
            return 0;
        }
        if (std::strcmp(arg, "--exec") == 0) {
            if (++i >= argc) { std::cerr << "error: --exec requires a cfg name\n"; return 2; }
            opts.startup_exec = argv[i];
        } else if (std::strcmp(arg, "--script") == 0) {
            if (++i >= argc) { std::cerr << "error: --script requires a file\n"; return 2; }
            opts.script_path = argv[i];
        } else if (std::strcmp(arg, "--save-scb") == 0) {
            if (++i >= argc) { std::cerr << "error: --save-scb requires a file\n"; return 2; }
            opts.save_scb_path = argv[i];
        } else if (std::strcmp(arg, "--precompile") == 0) {
            opts.precompile = true;
        } else if (std::strcmp(arg, "--cache") == 0) {
            opts.use_cache = true;
        } else if (std::strcmp(arg, "--cache-dir") == 0) {
            if (++i >= argc) { std::cerr << "error: --cache-dir requires a directory\n"; return 2; }
            opts.cache_dir = argv[i];
            opts.use_cache = true;
        } else if (std::strcmp(arg, "--no-cache") == 0) {
            opts.use_cache = false;
        } else if (std::strcmp(arg, "--no-interactive") == 0) {
            opts.interactive = false;
        } else if (std::strcmp(arg, "--trace") == 0) {
            opts.trace = true;
        } else if (std::strcmp(arg, "--no-engine-messages") == 0) {
            opts.engine_messages = false;
        } else if (std::strcmp(arg, "--no-ansi") == 0) {
            opts.ansi_clear = false;
        } else if (std::strcmp(arg, "--max-commands") == 0) {
            if (++i >= argc) { std::cerr << "error: --max-commands requires a number\n"; return 2; }
            if (!parse_u64("--max-commands", argv[i], opts.max_commands) || opts.max_commands == 0) return 2;
        } else if (std::strcmp(arg, "--profile") == 0) {
            if (++i >= argc) { std::cerr << "error: --profile requires a name\n"; return 2; }
            if (std::strcmp(argv[i], SCMD_CS2_PROFILE) != 0) {
                std::cerr << "error: unsupported compatibility profile '" << argv[i]
                          << "' (supported: " << SCMD_CS2_PROFILE << ")\n";
                return 2;
            }
            opts.profile = argv[i];
        } else if (arg[0] == '-') {
            std::cerr << "error: unknown option '" << arg << "'\n";
            return 2;
        } else {
            if (input_seen) {
                std::cerr << "error: multiple simulator inputs specified ('" << opts.cfg_root << "', '" << arg << "')\n";
                return 2;
            }
            opts.cfg_root = arg;
            input_seen = true;
        }
    }

    return scmd_simulator_run(&opts);
}
