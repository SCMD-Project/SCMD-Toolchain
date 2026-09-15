#include "scmd/simulator.h"
#include "scmd/version.h"
#include "bytecode_internal.hpp"
#include "sos_sim.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <queue>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <conio.h>
#include <io.h>
#include <windows.h>
#else
#include <termios.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using scmd::bc::Block;
using scmd::bc::Instruction;
using scmd::bc::Op;
using scmd::bc::Package;
using scmd::bc::join_tokens;
using scmd::bc::lower_ascii;
using scmd::bc::tokenize;
using scmd::bc::trim;

namespace {

bool parse_double(std::string_view sv, double &out) {
    std::string s(sv);
    char *end = nullptr;
    errno = 0;
    out = std::strtod(s.c_str(), &end);
    return errno == 0 && end && *end == '\0' && std::isfinite(out);
}

std::string format_number(double x) {
    if (std::abs(x - std::round(x)) < 1e-12 && std::abs(x) <= 9.0e15) {
        return std::to_string(static_cast<long long>(std::llround(x)));
    }
    std::ostringstream oss;
    oss << std::setprecision(12) << x;
    std::string s = oss.str();
    if (s.find_first_of("eE") == std::string::npos && s.find('.') != std::string::npos) {
        while (!s.empty() && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
    }
    return s;
}

struct Alias {
    uint32_t block = 0;
    uint32_t body_sid = 0;
};

struct Frame {
    uint32_t block = 0;
    uint32_t pc = 0;
    uint32_t end = 0;
};

struct Stream {
    uint64_t id = 0;
    uint64_t ready_ms = 0;
    bool async = false;
    std::array<uint64_t, scmd::bc::kRegisterCount> regs{};
    std::vector<Frame> stack;
};

struct StreamLater {
    bool operator()(const std::shared_ptr<Stream> &lhs, const std::shared_ptr<Stream> &rhs) const noexcept {
        if (lhs->ready_ms != rhs->ready_ms) return lhs->ready_ms > rhs->ready_ms;
        return lhs->id > rhs->id;
    }
};

enum class StepResult {
    Continue,
    CommandBoundary,
    Sleep,
    Finished,
    Failed,
};


#ifdef _WIN32
std::string utf8_from_wchars(const wchar_t *chars, int count) {
    if (!chars || count <= 0) return {};
    const int needed = WideCharToMultiByte(CP_UTF8, 0, chars, count, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<size_t>(needed), '\0');
    const int written = WideCharToMultiByte(CP_UTF8, 0, chars, count, out.data(), needed, nullptr, nullptr);
    if (written != needed) return {};
    return out;
}
#endif

void pop_utf8_codepoint(std::string &text) {
    if (text.empty()) return;
    size_t pos = text.size() - 1u;
    while (pos > 0u && (static_cast<unsigned char>(text[pos]) & 0xC0u) == 0x80u) --pos;
    text.resize(pos);
}

std::string longest_common_prefix(const std::vector<std::string> &items) {
    if (items.empty()) return {};
    std::string prefix = items.front();
    for (size_t i = 1; i < items.size(); ++i) {
        size_t n = 0;
        while (n < prefix.size() && n < items[i].size()) {
            const unsigned char a = static_cast<unsigned char>(prefix[n]);
            const unsigned char b = static_cast<unsigned char>(items[i][n]);
            const char al = (a >= 'A' && a <= 'Z') ? static_cast<char>(a - 'A' + 'a') : static_cast<char>(a);
            const char bl = (b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : static_cast<char>(b);
            if (al != bl) break;
            ++n;
        }
        prefix.resize(n);
        if (prefix.empty()) break;
    }
    return prefix;
}

class Simulator {
public:
    explicit Simulator(const ScmdSimOptions &options)
        : options_(options), max_commands_(options.max_commands ? options.max_commands : 10000000ULL) {
        aliases_.reserve(262144);
        cvars_.reserve(256);
        cvars_["sv_cheats"] = "1";
        cvars_["tv_window_size"] = "0";
        sos_ = std::make_unique<scmd::sim::SosSimulator>(cvars_);
    }

    int run() {
        if (options_.profile && *options_.profile && std::string_view(options_.profile) != SCMD_CS2_PROFILE) {
            std::cerr << "scmdsim: unsupported compatibility profile '" << options_.profile
                      << "' (supported: " << SCMD_CS2_PROFILE << ")\n";
            return 2;
        }

        const char *input_raw = (options_.cfg_root && *options_.cfg_root) ? options_.cfg_root : ".";
        input_path_ = fs::path(input_raw);
        const bool is_scb = lower_ascii(input_path_.extension().string()) == ".scb";
        std::string error;
        const auto begin = std::chrono::steady_clock::now();

        if (is_scb) {
            if (!package_.load(input_path_, error)) {
                std::cerr << "scmdsim: " << error << '\n';
                return 1;
            }
            package_source_ = "scb";
        } else {
            std::error_code ec;
            source_root_ = fs::absolute(input_path_, ec);
            if (ec || !fs::exists(source_root_) || !fs::is_directory(source_root_)) {
                std::cerr << "scmdsim: cfg root does not exist: " << input_path_.string() << '\n';
                return 1;
            }
            source_mode_ = true;
            package_source_ = "cfg-lazy";
            package_.profile = SCMD_CS2_PROFILE;
            package_.intern(package_.profile);
            if (options_.cache_dir && *options_.cache_dir) cache_root_ = fs::path(options_.cache_dir);
            else cache_root_ = source_root_ / ".scmdcache";

            if (options_.precompile || (options_.save_scb_path && *options_.save_scb_path)) {
                if (!precompile_all_sources()) return 1;
                package_source_ = "cfg-aot";
            }
            if (options_.save_scb_path && *options_.save_scb_path) {
                if (!package_.save(options_.save_scb_path, error)) {
                    std::cerr << "scmdsim: cannot save bytecode package: " << error << '\n';
                    return 1;
                }
            }
        }

        const auto end = std::chrono::steady_clock::now();
        startup_us_ = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());

        if (package_.profile != SCMD_CS2_PROFILE) {
            std::cerr << "scmdsim: SCB profile mismatch: package='" << package_.profile
                      << "' simulator='" << SCMD_CS2_PROFILE << "'\n";
            return 1;
        }
        if (options_.startup_exec && *options_.startup_exec) {
            const std::string ref = options_.startup_exec;
            if (!safe_exec_ref(ref)) {
                std::cerr << "exec: invalid cfg path '" << ref << "'\n";
                return 1;
            }
            const uint32_t block = resolve_module(ref);
            if (block == (std::numeric_limits<uint32_t>::max)()) {
                std::cerr << "exec: couldn't exec '" << ref << "'\n";
                return 1;
            }
            note_exec(ref);
            submit_block(block, false, options_.exec_latency_ms);
            if (!drain()) return 1;
        }
        if (options_.script_path && *options_.script_path) {
            if (!run_script(options_.script_path)) return 1;
        }
        if (options_.interactive) repl();
        return failed_ ? 1 : 0;
    }

private:
    ScmdSimOptions options_{};
    fs::path input_path_;
    fs::path source_root_;
    fs::path cache_root_;
    Package package_;
    std::string package_source_;
    bool source_mode_ = false;

    struct SourceStamp {
        uintmax_t size = 0;
        fs::file_time_type mtime{};
        uint32_t block = (std::numeric_limits<uint32_t>::max)();
    };
    std::unordered_map<std::string, SourceStamp> source_stamps_;
    std::unordered_map<std::string, Alias> aliases_;
    std::unordered_map<std::string, std::string> cvars_;
    std::unique_ptr<scmd::sim::SosSimulator> sos_;
    std::priority_queue<std::shared_ptr<Stream>, std::vector<std::shared_ptr<Stream>>, StreamLater> ready_;
    uint64_t next_stream_id_ = 1;
    uint64_t now_ms_ = 0;
    uint64_t commands_executed_ = 0;
    uint64_t instructions_executed_ = 0;
    uint64_t max_commands_ = 10000000ULL;
    uint64_t exec_calls_ = 0;
    uint64_t alias_calls_ = 0;
    uint64_t startup_us_ = 0;
    uint64_t compile_us_ = 0;
    uint64_t cache_hits_ = 0;
    uint64_t cache_misses_ = 0;
    uint64_t lazy_compiles_ = 0;
    uint64_t alias_generation_ = 0;
    uint64_t completion_alias_generation_ = (std::numeric_limits<uint64_t>::max)();
    std::vector<std::string> completion_aliases_;
    struct DelayedLine {
        uint64_t at, order;
        std::string text;
    };
    struct LineLater {
        bool operator()(const DelayedLine &a, const DelayedLine &b) const {
            return a.at != b.at ? a.at > b.at : a.order > b.order;
        }
    };
    std::priority_queue<DelayedLine, std::vector<DelayedLine>, LineLater> delayed_lines_;
    uint64_t next_line_id_ = 0;
    uint64_t unknown_commands_ = 0;
    uint64_t rejected_aliases_ = 0;
    std::unordered_map<std::string, uint64_t> module_loads_;
    bool console_visible_ = true;
    std::vector<std::string> screen_lines_;
    std::string screen_partial_;
    bool failed_ = false;

    static uint64_t fnv1a64_text(std::string_view a, std::string_view b) {
        uint64_t h = 1469598103934665603ULL;
        auto feed = [&](std::string_view sv) {
            for (char raw : sv) {
                const unsigned char c = static_cast<unsigned char>(raw);
                h ^= static_cast<uint64_t>(c);
                h *= 1099511628211ULL;
            }
        };
        feed(a);
        const char sep = '\0';
        feed(std::string_view(&sep, 1));
        feed(b);
        return h;
    }

    static std::string hex64(uint64_t value) {
        std::ostringstream oss;
        oss << std::hex << std::setfill('0') << std::setw(16) << value;
        return oss.str();
    }

    bool source_path_for_ref(std::string_view ref_view, fs::path &path, std::string &display) const {
        if (!source_mode_) return false;
        std::string ref(ref_view);
        if (!safe_exec_ref(ref)) return false;
        std::replace(ref.begin(), ref.end(), '\\', '/');
        while (ref.rfind("./", 0) == 0) ref.erase(0, 2);
        if (ref.size() >= 4u && lower_ascii(ref.substr(ref.size() - 4u)) == ".cfg") ref.resize(ref.size() - 4u);
        fs::path rel(ref);
        rel = rel.lexically_normal();
        if (rel.empty() || rel == ".") return false;
        display = rel.generic_string();
        path = source_root_ / rel;
        path.replace_extension(".cfg");
        std::error_code ec;
        if (fs::is_regular_file(path, ec) && !ec) return true;

        /* Source console paths are case-insensitive. On case-sensitive hosts,
         * resolve by scanning names only when the direct path misses. */
        const std::string wanted = scmd::bc::normalize_exec_ref(display);
        for (const std::string &name : source_module_names()) {
            if (scmd::bc::normalize_exec_ref(name) != wanted) continue;
            fs::path actual = source_root_ / fs::path(name);
            actual.replace_extension(".cfg");
            if (fs::is_regular_file(actual, ec) && !ec) {
                path = actual;
                display = name;
                return true;
            }
        }
        return false;
    }

    std::vector<std::string> source_module_names() const {
        std::vector<std::string> names;
        if (!source_mode_) return names;
        std::error_code ec;
        fs::recursive_directory_iterator it(source_root_, fs::directory_options::skip_permission_denied, ec), end;
        for (; it != end; it.increment(ec)) {
            if (ec) { ec.clear(); continue; }
            if (it->is_directory(ec)) {
                const std::string leaf = lower_ascii(it->path().filename().string());
                if (leaf == ".scmdcache" || leaf == ".git") it.disable_recursion_pending();
                continue;
            }
            if (!it->is_regular_file(ec) || ec) { ec.clear(); continue; }
            if (lower_ascii(it->path().extension().string()) != ".cfg") continue;
            fs::path rel = fs::relative(it->path(), source_root_, ec);
            if (ec) { ec.clear(); continue; }
            rel.replace_extension();
            names.push_back(rel.generic_string());
        }
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
            return lower_ascii(a) == lower_ascii(b);
        }), names.end());
        return names;
    }

    std::vector<std::string> complete_exec_dynamic(std::string_view prefix_view) const {
        if (!source_mode_) return package_.complete_exec(prefix_view);
        std::vector<std::string> names = source_module_names();
        for (const std::string &name : package_.module_names) names.push_back(name);
        std::sort(names.begin(), names.end());
        names.erase(std::unique(names.begin(), names.end(), [](const std::string &a, const std::string &b) {
            return lower_ascii(a) == lower_ascii(b);
        }), names.end());

        std::string prefix(prefix_view);
        std::replace(prefix.begin(), prefix.end(), '\\', '/');
        const size_t slash = prefix.find_last_of('/');
        const std::string dir = slash == std::string::npos ? "" : prefix.substr(0, slash + 1u);
        const std::string leaf = slash == std::string::npos ? prefix : prefix.substr(slash + 1u);
        const std::string lower_dir = lower_ascii(dir);
        const std::string lower_leaf = lower_ascii(leaf);
        std::vector<std::string> out;
        std::unordered_map<std::string, bool> seen;
        for (std::string name : names) {
            std::replace(name.begin(), name.end(), '\\', '/');
            const std::string lname = lower_ascii(name);
            if (!lower_dir.empty() && lname.rfind(lower_dir, 0) != 0) continue;
            if (lower_dir.empty() && name.find('/') != std::string::npos) {
                const size_t first_slash = name.find('/');
                const std::string child = name.substr(0, first_slash + 1u);
                const std::string k = lower_ascii(child);
                if (k.rfind(lower_leaf, 0) == 0 && !seen[k]) { seen[k] = true; out.push_back(child); }
                continue;
            }
            if (dir.size() > name.size()) continue;
            const std::string rest = name.substr(dir.size());
            if (lower_ascii(rest).rfind(lower_leaf, 0) != 0) continue;
            const size_t next_slash = rest.find('/');
            const std::string candidate = next_slash == std::string::npos ? dir + rest : dir + rest.substr(0, next_slash + 1u);
            const std::string k = lower_ascii(candidate);
            if (!seen[k]) { seen[k] = true; out.push_back(candidate); }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    uint32_t compile_source_module(std::string_view ref_view) {
        fs::path path;
        std::string display;
        if (!source_path_for_ref(ref_view, path, display)) return (std::numeric_limits<uint32_t>::max)();
        const std::string key = scmd::bc::normalize_exec_ref(display);
        std::error_code ec;
        const uintmax_t size = fs::file_size(path, ec);
        if (ec) return (std::numeric_limits<uint32_t>::max)();
        const fs::file_time_type mtime = fs::last_write_time(path, ec);
        if (ec) return (std::numeric_limits<uint32_t>::max)();
        const auto sit = source_stamps_.find(key);
        if (sit != source_stamps_.end() && sit->second.size == size && sit->second.mtime == mtime) {
            const uint32_t existing = package_.find_module(key);
            if (existing != (std::numeric_limits<uint32_t>::max)()) return existing;
        }

        std::ifstream f(path, std::ios::binary);
        if (!f) return (std::numeric_limits<uint32_t>::max)();
        std::ostringstream ss;
        ss << f.rdbuf();
        const std::string text = ss.str();
        if (!validate_text(text, display)) return (std::numeric_limits<uint32_t>::max)();
        const auto begin = std::chrono::steady_clock::now();
        std::string error;

        if (!options_.use_cache) {
            if (!package_.compile_cfg_module(path, display, error)) {
                std::cerr << "scmdsim: lazy compile failed for '" << display << "': " << error << '\n';
                return (std::numeric_limits<uint32_t>::max)();
            }
            ++lazy_compiles_;
            const auto end = std::chrono::steady_clock::now();
            compile_us_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
            const uint32_t block = package_.find_module(key);
            source_stamps_[key] = SourceStamp{size, mtime, block};
            return block;
        }

        Package module;
        module.profile = SCMD_CS2_PROFILE;
        module.intern(module.profile);
        bool loaded_cache = false;

        fs::path cache_path;
        if (options_.use_cache) {
            const uint64_t hash = fnv1a64_text(std::string(SCMD_VERSION) + ":" + key, text);
            cache_path = cache_root_ / "modules" / (hex64(hash) + ".scb");
            if (fs::is_regular_file(cache_path, ec) && !ec) {
                if (module.load(cache_path, error) && module.find_module(key) != (std::numeric_limits<uint32_t>::max)()) {
                    loaded_cache = true;
                    ++cache_hits_;
                } else {
                    error.clear();
                }
            }
        }

        if (!loaded_cache) {
            module = Package{};
            module.profile = SCMD_CS2_PROFILE;
            module.intern(module.profile);
            if (!module.compile_cfg_module(path, display, error) || !module.verify(error)) {
                std::cerr << "scmdsim: lazy compile failed for '" << display << "': " << error << '\n';
                return (std::numeric_limits<uint32_t>::max)();
            }
            ++lazy_compiles_;
            ++cache_misses_;
            if (options_.use_cache) {
                fs::create_directories(cache_path.parent_path(), ec);
                if (!ec) {
                    std::string save_error;
                    if (!module.save(cache_path, save_error) && options_.trace) {
                        std::cerr << "scmdsim: cache write skipped: " << save_error << '\n';
                    }
                }
            }
        }

        if (!package_.merge_from(module, error)) {
            std::cerr << "scmdsim: cannot merge module '" << display << "': " << error << '\n';
            return (std::numeric_limits<uint32_t>::max)();
        }
        const auto end = std::chrono::steady_clock::now();
        compile_us_ += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count());
        const uint32_t block = package_.find_module(key);
        source_stamps_[key] = SourceStamp{size, mtime, block};
        return block;
    }

    uint32_t resolve_module(std::string_view ref) {
        if (source_mode_) return compile_source_module(ref);
        return package_.find_module(ref);
    }

    bool precompile_all_sources() {
        if (!source_mode_) return true;
        const auto names = source_module_names();
        const auto begin = std::chrono::steady_clock::now();
        for (const std::string &name : names) {
            if (compile_source_module(name) == (std::numeric_limits<uint32_t>::max)()) return false;
        }
        const auto end = std::chrono::steady_clock::now();
        if (options_.trace || options_.interactive) {
            const double ms = static_cast<double>(std::chrono::duration_cast<std::chrono::microseconds>(end - begin).count()) / 1000.0;
            std::cout << "[scmdsim] precompiled " << names.size() << " module(s) in " << ms << " ms\n";
        }
        return true;
    }

    void screen_write(const std::string &text, bool newline) {
        // Closing the console hides the view, not its retained log.
        if (console_visible_) std::cout << text;
        screen_partial_ += text;
        if (newline) {
            if (console_visible_) std::cout << '\n';
            screen_lines_.push_back(screen_partial_);
            screen_partial_.clear();
            if (screen_lines_.size() > 512u) screen_lines_.erase(screen_lines_.begin(), screen_lines_.begin() + 256);
        }
        std::cout.flush();
    }

    void screen_line(const std::string &text) { screen_write(text, true); }

    void engine_line(const std::string &text) { screen_line(text); }

    void screen_clear() {
        screen_lines_.clear();
        screen_partial_.clear();
    }

    static bool is_builtin(std::string_view name) {
        const std::string n = lower_ascii(name);
        return n == "alias" || n == "exec" || n == "execifexists" || n == "exec_async" || n == "sleep" ||
               n == "clear" || n == "clearall" || n == "hideconsole" || n == "showconsole" || n == "kill" || n == "help" || n == "version" || n == "echo" || n == "echoln" || n == "incrementvar" || n == "multvar" ||
               n == "say" || n == "say_team" || n == "setinfo" || n == "toggle" || n == "ent_create" || n == "ent_fire" ||
               n == "cl_sos_test_set_opvar" || n == "cl_sos_test_get_opvar" || n.rfind("snd_sos_", 0) == 0;
    }

    bool validate_text(std::string_view text, std::string_view origin) {
        if (!options_.strict) return true;
        for (const std::string &command : scmd::bc::split_commands(text)) {
            if (command.size() > SCMD_CS2_MAX_COMMAND_BYTES) {
                std::cerr << "scmdsim: overlong command (" << command.size() << " bytes) in " << origin << '\n';
                failed_ = true;
                return false;
            }
        }
        return true;
    }

    bool valid_alias_name(const std::string &name) {
        if (name.empty() || name.size() > 31u) {
            std::cerr << "alias: name must contain 1..31 bytes: '" << name << "'\n";
        } else if (is_builtin(name) || cvars_.find(name) != cvars_.end()) {
            std::cerr << "alias: cannot shadow built-in command or cvar '" << name << "'\n";
        } else {
            return true;
        }
        ++rejected_aliases_;
        if (options_.strict) failed_ = true;
        return false;
    }

    void note_exec(std::string_view ref) {
        ++exec_calls_;
        ++module_loads_[scmd::bc::normalize_exec_ref(std::string(ref))];
    }

    bool time_after(uint64_t delay, uint64_t &result) {
        if (delay > (std::numeric_limits<uint64_t>::max)() - now_ms_) {
            std::cerr << "scmdsim: virtual time overflow\n";
            failed_ = true;
            return false;
        }
        result = now_ms_ + delay;
        return true;
    }

    const std::string &reg_string(const Stream &stream, uint8_t reg) const {
        const uint64_t raw = stream.regs[reg];
        if (raw >= package_.strings.size()) throw std::runtime_error("VM register contains invalid string id");
        return package_.strings[static_cast<size_t>(raw)];
    }

    bool push_block(Stream &stream, uint32_t block_id) {
        if (block_id >= package_.blocks.size()) {
            std::cerr << "scmdsim: VM block id out of range\n";
            failed_ = true;
            return false;
        }
        const Block &block = package_.blocks[block_id];
        Frame frame{};
        frame.block = block_id;
        frame.pc = block.first;
        frame.end = block.first + block.count;
        stream.stack.push_back(frame);
        return true;
    }

    void submit_block(uint32_t block_id, bool async = false, uint64_t delay = 0) {
        auto stream = std::make_shared<Stream>();
        stream->id = next_stream_id_++;
        if (!time_after(delay, stream->ready_ms)) return;
        stream->async = async;
        if (push_block(*stream, block_id)) ready_.push(std::move(stream));
    }

    void submit_console(std::string_view text) {
        if (!validate_text(text, "<interactive>")) return;
        const uint32_t block = package_.compile_text(text, "<interactive>");
        submit_block(block, false);
    }

    static bool safe_exec_ref(std::string ref) {
        if (ref.empty()) return false;
        std::replace(ref.begin(), ref.end(), '\\', '/');
        if (!ref.empty() && (ref.front() == '/' || ref.front() == '\\')) return false;
        if (ref.find(':') != std::string::npos) return false;
        const fs::path path(ref);
        if (path.is_absolute()) return false;
        for (const auto &part : path) if (part == "..") return false;
        return true;
    }

    bool report_exec_missing(std::string_view ref, bool report) {
        if (report) {
            std::cerr << "exec: couldn't exec '" << ref << "'\n";
            if (options_.strict) failed_ = true;
        }
        return false;
    }

    StepResult execute_instruction(Stream &stream, const Instruction &ins) {
        const Op op = static_cast<Op>(ins.op);
        switch (op) {
        case Op::Nop:
            return StepResult::Continue;
        case Op::KStr:
            stream.regs[ins.dst] = ins.x;
            return StepResult::Continue;
        case Op::KImm:
            stream.regs[ins.dst] = static_cast<uint64_t>(ins.x) | (static_cast<uint64_t>(ins.y) << 32u);
            return StepResult::Continue;
        case Op::AliasList: {
            std::vector<std::string> names;
            names.reserve(aliases_.size());
            for (const auto &kv : aliases_) names.push_back(kv.first);
            std::sort(names.begin(), names.end());
            for (const std::string &name : names) std::cout << name << " = " << package_.str(aliases_.at(name).body_sid) << '\n';
            return StepResult::CommandBoundary;
        }
        case Op::AliasQuery: {
            const std::string name = lower_ascii(reg_string(stream, ins.a));
            const auto it = aliases_.find(name);
            if (it == aliases_.end()) std::cout << "Unknown alias: " << name << '\n';
            else std::cout << name << " = " << package_.str(it->second.body_sid) << '\n';
            return StepResult::CommandBoundary;
        }
        case Op::AliasQueryI: {
            const std::string name = lower_ascii(package_.str(ins.x));
            const auto it = aliases_.find(name);
            if (it == aliases_.end()) std::cout << "Unknown alias: " << name << '\n';
            else std::cout << name << " = " << package_.str(it->second.body_sid) << '\n';
            return StepResult::CommandBoundary;
        }
        case Op::AliasSet: {
            const std::string name = lower_ascii(reg_string(stream, ins.a));
            if (valid_alias_name(name)) {
                aliases_[name] = Alias{ins.x, ins.y};
                ++alias_generation_;
            }
            return StepResult::CommandBoundary;
        }
        case Op::AliasSetI: {
            const std::string name = lower_ascii(package_.str(ins.x));
            if (valid_alias_name(name)) {
                aliases_[name] = Alias{ins.y, ins.z};
                ++alias_generation_;
            }
            return StepResult::CommandBoundary;
        }
        case Op::Exec:
        case Op::ExecIfExists:
        case Op::ExecAsync:
        case Op::ExecI:
        case Op::ExecIfExistsI:
        case Op::ExecAsyncI: {
            const bool immediate = op == Op::ExecI || op == Op::ExecIfExistsI || op == Op::ExecAsyncI;
            const std::string ref = immediate ? package_.str(ins.x) : reg_string(stream, ins.a);
            const bool report = op != Op::ExecIfExists && op != Op::ExecIfExistsI;
            if (!safe_exec_ref(ref)) {
                if (report) {
                    std::cerr << "exec: invalid cfg path '" << ref << "'\n";
                    if (options_.strict) failed_ = true;
                }
                return StepResult::CommandBoundary;
            }
            const uint32_t block = resolve_module(ref);
            if (block == (std::numeric_limits<uint32_t>::max)()) {
                (void)report_exec_missing(ref, report);
                return StepResult::CommandBoundary;
            }
            note_exec(ref);
            if (op == Op::ExecAsync || op == Op::ExecAsyncI) {
                if (options_.engine_messages) engine_line("[InputService] queuing " + ref + " for async execution");
                submit_block(block, true, options_.exec_latency_ms);
            } else {
                if (options_.engine_messages) engine_line("[InputService] execing " + ref);
                if (!push_block(stream, block)) return StepResult::Failed;
                if (!time_after(options_.exec_latency_ms, stream.ready_ms)) return StepResult::Failed;
                if (options_.exec_latency_ms) return StepResult::Sleep;
            }
            return StepResult::CommandBoundary;
        }
        case Op::Sleep:
        case Op::SleepI: {
            const uint64_t delay = op == Op::SleepI
                ? (static_cast<uint64_t>(ins.x) | (static_cast<uint64_t>(ins.y) << 32u))
                : stream.regs[ins.a];
            if (delay > (std::numeric_limits<uint64_t>::max)() - now_ms_) {
                std::cerr << "sleep: virtual time overflow\n";
                failed_ = true;
                return StepResult::Failed;
            }
            stream.ready_ms = now_ms_ + delay;
            return StepResult::Sleep;
        }
        case Op::Clear:
            screen_clear();
            if (options_.ansi_clear && options_.interactive && console_visible_) std::cout << "\x1b[2J\x1b[H";
            return StepResult::CommandBoundary;
        case Op::Echo:
        case Op::EchoLn:
        case Op::EchoI:
        case Op::EchoLnI: {
            const std::string &text = (op == Op::EchoI || op == Op::EchoLnI) ? package_.str(ins.x) : reg_string(stream, ins.a);
            if (op == Op::Echo || op == Op::EchoI) {
                const std::string line = std::string("[Console] ") + text;
                if (options_.echo_delay_ms) {
                    uint64_t at;
                    if (!time_after(options_.echo_delay_ms, at)) return StepResult::Failed;
                    delayed_lines_.push(DelayedLine{at, next_line_id_++, line});
                } else screen_line(line);
            } else screen_line(text);
            return StepResult::CommandBoundary;
        }
        case Op::Say:
        case Op::SayTeam:
        case Op::SayI:
        case Op::SayTeamI: {
            const bool team = op == Op::SayTeam || op == Op::SayTeamI;
            const std::string &text = (op == Op::SayI || op == Op::SayTeamI) ? package_.str(ins.x) : reg_string(stream, ins.a);
            std::cout << '[' << (team ? "say_team" : "say") << "] " << text << '\n';
            return StepResult::CommandBoundary;
        }
        case Op::SetInfo:
            cvars_[lower_ascii(reg_string(stream, ins.a))] = reg_string(stream, ins.b);
            return StepResult::CommandBoundary;
        case Op::SetInfoI:
            cvars_[lower_ascii(package_.str(ins.x))] = package_.str(ins.y);
            return StepResult::CommandBoundary;
        case Op::IncrementVar:
        case Op::MultVar: {
            if (ins.b != 4u) { failed_ = true; return StepResult::Failed; }
            const std::string key = lower_ascii(reg_string(stream, ins.a));
            double lo = 0, hi = 0, value = 0, cur = 0;
            if (!parse_double(reg_string(stream, static_cast<uint8_t>(ins.a + 1u)), lo) ||
                !parse_double(reg_string(stream, static_cast<uint8_t>(ins.a + 2u)), hi) ||
                !parse_double(reg_string(stream, static_cast<uint8_t>(ins.a + 3u)), value) || hi < lo) {
                std::cerr << (op == Op::IncrementVar ? "incrementvar" : "multvar") << ": invalid numeric arguments\n";
                return StepResult::CommandBoundary;
            }
            auto it = cvars_.find(key);
            if (it != cvars_.end()) (void)parse_double(it->second, cur);
            if (op == Op::IncrementVar) {
                double next = cur + value;
                if (next > hi || next < lo) next = value >= 0 ? lo : hi;
                cvars_[key] = format_number(next);
            } else {
                cvars_[key] = format_number(std::clamp(cur * value, lo, hi));
            }
            return StepResult::CommandBoundary;
        }
        case Op::Toggle: {
            if (ins.b < 1u) { std::cerr << "toggle: expected cvar [values...]\n"; return StepResult::CommandBoundary; }
            const std::string key = lower_ascii(reg_string(stream, ins.a));
            std::vector<std::string> values;
            for (uint8_t i = 1; i < ins.b; ++i) values.push_back(reg_string(stream, static_cast<uint8_t>(ins.a + i)));
            if (values.empty()) values = {"0", "1"};
            const std::string current = cvars_.count(key) ? cvars_[key] : values.front();
            auto it = std::find(values.begin(), values.end(), current);
            if (it == values.end() || ++it == values.end()) cvars_[key] = values.front();
            else cvars_[key] = *it;
            return StepResult::CommandBoundary;
        }
        case Op::Dispatch: {
            std::vector<std::string> argv;
            argv.reserve(ins.b);
            for (uint8_t i = 0; i < ins.b; ++i) argv.push_back(reg_string(stream, static_cast<uint8_t>(ins.a + i)));
            return dispatch(stream, argv);
        }
        case Op::Dispatch0:
            return dispatch(stream, std::vector<std::string>{package_.str(ins.x)});
        case Op::Dispatch1:
            return dispatch(stream, std::vector<std::string>{package_.str(ins.x), package_.str(ins.y)});
        case Op::DispatchRaw: {
            const auto argv = tokenize(package_.str(ins.x));
            return dispatch(stream, argv);
        }
        case Op::Ret:
            if (!stream.stack.empty()) stream.stack.pop_back();
            return stream.stack.empty() ? StepResult::Finished : StepResult::Continue;
        }
        return StepResult::Failed;
    }

    StepResult dispatch(Stream &stream, const std::vector<std::string> &argv) {
        if (argv.empty()) return StepResult::CommandBoundary;
        const std::string command = lower_ascii(argv[0]);

        /* Raw malformed builtins keep old diagnostics. */
        if (command == "exec" || command == "execifexists" || command == "exec_async") {
            if (argv.size() < 2u) { std::cerr << command << ": missing cfg name\n"; return StepResult::CommandBoundary; }
        } else if (command == "sleep") {
            std::cerr << "sleep: invalid milliseconds\n"; return StepResult::CommandBoundary;
        } else if (command == "setinfo") {
            std::cerr << "setinfo: expected name value\n"; return StepResult::CommandBoundary;
        } else if (command == "incrementvar") {
            std::cerr << "incrementvar: expected <cvar> <min> <max> <delta>\n"; return StepResult::CommandBoundary;
        } else if (command == "multvar") {
            std::cerr << "multvar: expected <cvar> <min> <max> <factor>\n"; return StepResult::CommandBoundary;
        }

        if (command == "hideconsole") { console_visible_ = false; return StepResult::CommandBoundary; }
        if (command == "showconsole") { console_visible_ = true; return StepResult::CommandBoundary; }
        if (command == "clearall") {
            screen_clear();
            if (options_.ansi_clear && options_.interactive && console_visible_) std::cout << "\x1b[2J\x1b[H";
            return StepResult::CommandBoundary;
        }
        /* These are real CS2 builtins.  They must win over aliases even though
         * the simulator does not emulate their gameplay side effects. */
        if (command == "kill" || command == "help" || command == "version") return StepResult::CommandBoundary;

        if (sos_ && sos_->handles(command) && sos_->execute(argv)) {
            return StepResult::CommandBoundary;
        }

        if (auto cv = cvars_.find(command); cv != cvars_.end()) {
            if (argv.size() >= 2u) cv->second = argv[1];
            else std::cout << command << " = " << cv->second << '\n';
            return StepResult::CommandBoundary;
        }
        if (auto alias = aliases_.find(command); alias != aliases_.end()) {
            ++alias_calls_;
            if (!push_block(stream, alias->second.block)) return StepResult::Failed;
            return StepResult::CommandBoundary;
        }
        screen_line("Unknown command: " + argv[0]);
        ++unknown_commands_;
        if (options_.strict) { failed_ = true; return StepResult::Failed; }
        return StepResult::CommandBoundary;
    }

    bool drain() {
        while (!ready_.empty() || !delayed_lines_.empty()) {
            if (!delayed_lines_.empty() && (ready_.empty() || delayed_lines_.top().at < ready_.top()->ready_ms)) {
                const DelayedLine line = delayed_lines_.top();
                delayed_lines_.pop();
                now_ms_ = std::max(now_ms_, line.at);
                screen_line(line.text);
                continue;
            }
            auto stream = ready_.top();
            ready_.pop();
            if (stream->ready_ms > now_ms_) now_ms_ = stream->ready_ms;
            StepResult result = StepResult::Continue;
            while (result == StepResult::Continue) {
                if (stream->stack.empty()) { result = StepResult::Finished; break; }
                Frame &frame = stream->stack.back();
                if (frame.pc >= frame.end) {
                    stream->stack.pop_back();
                    if (stream->stack.empty()) result = StepResult::Finished;
                    continue;
                }
                if (frame.pc >= package_.code.size()) {
                    std::cerr << "scmdsim: bytecode PC out of range\n";
                    failed_ = true;
                    result = StepResult::Failed;
                    break;
                }
                const Instruction ins = package_.code[frame.pc++];
                ++instructions_executed_;
                if (options_.trace) {
                    std::cerr << "[vm t=" << now_ms_ << "ms stream=" << stream->id << " pc=" << (frame.pc - 1u)
                              << " op=" << static_cast<unsigned>(ins.op) << "]\n";
                }
                result = execute_instruction(*stream, ins);
            }
            if (result == StepResult::Failed || failed_) return false;
            if (result == StepResult::CommandBoundary || result == StepResult::Sleep) {
                if (++commands_executed_ > max_commands_) {
                    std::cerr << "scmdsim: command budget exceeded (" << max_commands_ << "); probable alias/exec loop\n";
                    failed_ = true;
                    return false;
                }
                if (!stream->stack.empty()) ready_.push(std::move(stream));
            }
        }
        /* snd_opvar_set SetOnSpawn changes are observed one entity/SOS update
         * later. Keeping them pending until this submitted console run drains
         * reproduces the useful same-line-old / next-line-new behavior. */
        if (sos_) sos_->flush_deferred();
        return true;
    }

    bool run_script(const char *path) {
        std::ifstream f(path);
        if (!f) {
            std::cerr << "scmdsim: cannot open script: " << path << '\n';
            failed_ = true;
            return false;
        }
        std::string line;
        while (std::getline(f, line)) {
            const std::string t = trim(line);
            if (t.empty() || t.rfind("//", 0) == 0 || t.rfind("#", 0) == 0) continue;
            if (t == ":quit" || t == ":q" || t == ":exit" || lower_ascii(t) == "quit" || lower_ascii(t) == "exit") break;
            if (!handle_meta(t)) {
                submit_console(t);
                if (!drain()) return false;
            }
        }
        return true;
    }

    std::vector<std::string> command_completions(std::string_view prefix_view) {
        static const std::array<std::string_view, 22> builtins = {
            "alias", "clear", "clearall", "echo", "echoln", "exec", "exec_async", "execifexists",
            "hideconsole", "showconsole", "incrementvar", "multvar", "say", "say_team", "setinfo",
            "sleep", "toggle", "help", "kill", "quit", "exit", "status"
        };
        const std::string prefix = lower_ascii(prefix_view);
        std::set<std::string> found;
        for (std::string_view b : builtins) if (std::string(b).rfind(prefix, 0) == 0) found.emplace(b);
        if (sos_) {
            for (const std::string &name : sos_->command_names()) {
                if (name.rfind(prefix, 0) == 0) found.emplace(name);
            }
        }
        if (completion_alias_generation_ != alias_generation_) {
            completion_aliases_.clear();
            completion_aliases_.reserve(aliases_.size());
            for (const auto &kv : aliases_) completion_aliases_.push_back(kv.first);
            std::sort(completion_aliases_.begin(), completion_aliases_.end());
            completion_alias_generation_ = alias_generation_;
        }
        auto it = std::lower_bound(completion_aliases_.begin(), completion_aliases_.end(), prefix);
        for (; it != completion_aliases_.end() && it->rfind(prefix, 0) == 0; ++it) found.insert(*it);
        for (const auto &kv : cvars_) if (kv.first.rfind(prefix, 0) == 0) found.insert(kv.first);
        return {found.begin(), found.end()};
    }

    std::vector<std::string> completions_for_line(const std::string &line, size_t &replace_begin) {
        replace_begin = line.find_last_of(" \t");
        replace_begin = replace_begin == std::string::npos ? 0u : replace_begin + 1u;
        const std::string prefix = line.substr(replace_begin);
        const auto argv = tokenize(line.substr(0, replace_begin));
        if (!argv.empty()) {
            const std::string cmd = lower_ascii(argv[0]);
            if (cmd == "exec" || cmd == "execifexists" || cmd == "exec_async") {
                return complete_exec_dynamic(prefix);
            }
        }
        if (replace_begin == 0u) return command_completions(prefix);
        return {};
    }

    void print_completions(const std::vector<std::string> &items) {
        if (items.empty()) return;
        std::cout << '\n';
        const size_t limit = std::min<size_t>(items.size(), 64u);
        for (size_t i = 0; i < limit; ++i) std::cout << items[i] << '\n';
        if (items.size() > limit) std::cout << "... " << (items.size() - limit) << " more\n";
    }

    bool handle_meta(const std::string &line) {
        if (line.empty() || line[0] != ':') return false;
        const auto argv = tokenize(line.substr(1));
        if (argv.empty()) return true;
        const std::string cmd = lower_ascii(argv[0]);
        if (cmd == "stats") {
            std::cout << "source=" << package_source_
                      << " startup=" << (static_cast<double>(startup_us_) / 1000.0) << "ms"
                      << " compile=" << (static_cast<double>(compile_us_) / 1000.0) << "ms"
                      << " sim_time=" << now_ms_ << "ms commands=" << commands_executed_
                      << " vm_instructions=" << instructions_executed_ << " aliases=" << aliases_.size()
                      << " modules=" << package_.modules.size() << " blocks=" << package_.blocks.size()
                      << " strings=" << package_.strings.size() << " execs=" << exec_calls_
                      << " alias_calls=" << alias_calls_ << " lazy_compiles=" << lazy_compiles_
                      << " cache_hits=" << cache_hits_ << " cache_misses=" << cache_misses_
                      << " unique_execs=" << module_loads_.size() << " unknown_commands=" << unknown_commands_
                      << " rejected_aliases=" << rejected_aliases_
                      << " echo_delay_ms=" << options_.echo_delay_ms << " exec_latency_ms=" << options_.exec_latency_ms << '\n';
        } else if (cmd == "time") {
            std::cout << now_ms_ << " ms\n";
        } else if (cmd == "cvars") {
            std::vector<std::string> names;
            for (const auto &kv : cvars_) names.push_back(kv.first);
            std::sort(names.begin(), names.end());
            for (const auto &name : names) std::cout << name << " = " << cvars_[name] << '\n';
        } else if (cmd == "aliases") {
            const std::string prefix = argv.size() > 1u ? lower_ascii(argv[1]) : "";
            std::vector<std::string> names;
            for (const auto &kv : aliases_) if (kv.first.rfind(prefix, 0) == 0) names.push_back(kv.first);
            std::sort(names.begin(), names.end());
            for (const auto &name : names) std::cout << name << " = " << package_.str(aliases_.at(name).body_sid) << '\n';
        } else if (cmd == "loads") {
            const std::string prefix = argv.size() > 1u ? scmd::bc::normalize_exec_ref(argv[1]) : "";
            std::vector<std::string> names;
            for (const auto &kv : module_loads_) if (kv.first.rfind(prefix, 0) == 0) names.push_back(kv.first);
            std::sort(names.begin(), names.end());
            for (const auto &name : names) std::cout << module_loads_.at(name) << "\t" << name << '\n';
        } else if (cmd == "modules" || cmd == "execs") {
            const std::string prefix = argv.size() > 1u ? argv[1] : "";
            const auto items = complete_exec_dynamic(prefix);
            for (const std::string &item : items) std::cout << item << '\n';
        } else if (cmd == "complete") {
            const std::string probe = line.size() > 10u ? line.substr(10) : "";
            size_t begin = 0;
            const auto items = completions_for_line(probe, begin);
            (void)begin;
            for (const auto &item : items) std::cout << item << '\n';
        } else if (cmd == "precompile") {
            if (!source_mode_) std::cout << "precompile: current input is already an SCB package\n";
            else if (!precompile_all_sources()) failed_ = true;
        } else if (cmd == "cache") {
            std::cout << "cache=" << (options_.use_cache ? cache_root_.string() : std::string("OFF"))
                      << " hits=" << cache_hits_ << " misses=" << cache_misses_ << '\n';
        } else if (cmd == "screen") {
            size_t n = 24u;
            if (argv.size() > 1u) {
                try { n = static_cast<size_t>(std::stoul(argv[1])); } catch (...) { n = 24u; }
                n = std::clamp<size_t>(n, 1u, 200u);
            }
            std::cout << "[screen]\n";
            const size_t start = screen_lines_.size() > n ? screen_lines_.size() - n : 0u;
            for (size_t i = start; i < screen_lines_.size(); ++i) std::cout << screen_lines_[i] << '\n';
            if (!screen_partial_.empty()) std::cout << screen_partial_ << '\n';
        } else if (cmd == "help") {
            std::cout << ":stats  :loads [prefix]  :time  :screen [lines]  :aliases [prefix]  :cvars  :modules [prefix]  :complete <line>\n"
                      << ":precompile  :cache  :quit\n";
        } else if (cmd != "quit" && cmd != "q" && cmd != "exit") {
            std::cout << "Unknown simulator meta-command: :" << argv[0] << '\n';
        }
        return true;
    }

    void apply_tab(std::string &line) {
        size_t begin = 0;
        const auto items = completions_for_line(line, begin);
        if (items.empty()) return;
        const std::string current = line.substr(begin);
        if (items.size() == 1u) {
            line.replace(begin, std::string::npos, items[0]);
            if (!items[0].empty() && items[0].back() != '/') line.push_back(' ');
            return;
        }
        const std::string common = longest_common_prefix(items);
        if (common.size() > current.size()) {
            line.replace(begin, std::string::npos, common);
        } else {
            print_completions(items);
        }
    }

    static void redraw_line(const std::string &line) {
        std::cout << "\r\x1b[2K> " << line << std::flush;
    }

    std::optional<std::string> read_line_edit(const std::vector<std::string> &history) {
#ifdef _WIN32
        if (!_isatty(_fileno(stdin))) {
            std::string line;
            if (!std::getline(std::cin, line)) return std::nullopt;
            return line;
        }
        std::string line;
        size_t hist = history.size();
        std::cout << "> " << std::flush;
        while (true) {
            const int ch = _getwch();
            if (ch == 13) { std::cout << '\n'; return line; }
            if (ch == 3) {
                std::cout << "^C\n";
                if (line.empty()) return std::nullopt;
                return std::string();
            }
            if (ch == 8) {
                if (!line.empty()) { pop_utf8_codepoint(line); redraw_line(line); }
                continue;
            }
            if (ch == 9) { apply_tab(line); redraw_line(line); continue; }
            if (ch == 0 || ch == 224) {
                const int ext = _getwch();
                if (ext == 72 && !history.empty()) {
                    if (hist > 0u) --hist;
                    line = history[hist]; redraw_line(line);
                } else if (ext == 80 && !history.empty()) {
                    if (hist + 1u < history.size()) { ++hist; line = history[hist]; }
                    else { hist = history.size(); line.clear(); }
                    redraw_line(line);
                }
                continue;
            }
            if (ch >= 32) {
                wchar_t units[2] = {static_cast<wchar_t>(ch), 0};
                int unit_count = 1;
                if (ch >= 0xD800 && ch <= 0xDBFF) {
                    const int low = _getwch();
                    if (low >= 0xDC00 && low <= 0xDFFF) {
                        units[1] = static_cast<wchar_t>(low);
                        unit_count = 2;
                    }
                }
                const std::string utf8 = utf8_from_wchars(units, unit_count);
                line += utf8;
                std::cout << utf8 << std::flush;
            }
        }
#else
        if (!isatty(STDIN_FILENO)) {
            std::string line;
            if (!std::getline(std::cin, line)) return std::nullopt;
            return line;
        }
        termios oldt{};
        if (tcgetattr(STDIN_FILENO, &oldt) != 0) return std::nullopt;
        termios raw = oldt;
        raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
        struct Restore { termios t; ~Restore(){ tcsetattr(STDIN_FILENO, TCSANOW, &t); } } restore{oldt};
        std::string line;
        size_t hist = history.size();
        std::cout << "> " << std::flush;
        while (true) {
            unsigned char c = 0;
            const ssize_t n = ::read(STDIN_FILENO, &c, 1);
            if (n <= 0) return std::nullopt;
            if (c == '\r' || c == '\n') { std::cout << '\n'; return line; }
            if (c == 3) {
                std::cout << "^C\n";
                if (line.empty()) return std::nullopt;
                return std::string();
            }
            if (c == 127 || c == 8) { if (!line.empty()) { pop_utf8_codepoint(line); redraw_line(line); } continue; }
            if (c == '\t') { apply_tab(line); redraw_line(line); continue; }
            if (c == 27) {
                unsigned char seq[2]{};
                if (::read(STDIN_FILENO, &seq[0], 1) > 0 && ::read(STDIN_FILENO, &seq[1], 1) > 0 && seq[0] == '[') {
                    if (seq[1] == 'A' && !history.empty()) { if (hist > 0u) --hist; line = history[hist]; redraw_line(line); }
                    else if (seq[1] == 'B' && !history.empty()) { if (hist + 1u < history.size()) { ++hist; line = history[hist]; } else { hist = history.size(); line.clear(); } redraw_line(line); }
                }
                continue;
            }
            if (c >= 32) { line.push_back(static_cast<char>(c)); std::cout << static_cast<char>(c) << std::flush; }
        }
#endif
    }

    void repl() {
#ifdef _WIN32
        SetConsoleOutputCP(CP_UTF8);
#endif
        std::vector<std::string> history;
        std::cout << "scmdsim " << SCMD_VERSION << " [SCB" << scmd::bc::kAbiVersion << '/' << SCMD_CS2_PROFILE
                  << "]  Tab: complete  quit/exit: leave  :help: simulator commands\n";
        while (true) {
            const auto maybe = read_line_edit(history);
            if (!maybe) break;
            const std::string line = *maybe;
            const std::string t = trim(line);
            if (t == ":quit" || t == ":q" || t == ":exit" || lower_ascii(t) == "quit" || lower_ascii(t) == "exit") break;
            if (!t.empty()) history.push_back(line);
            if (handle_meta(t)) continue;
            submit_console(line);
            if (!drain()) break;
        }
    }
};

} // namespace

extern "C" int scmd_simulator_run(const ScmdSimOptions *options) {
    if (!options) {
        std::cerr << "scmdsim: null options\n";
        return 2;
    }
    try {
        Simulator sim(*options);
        return sim.run();
    } catch (const std::exception &e) {
        std::cerr << "scmdsim: fatal error: " << e.what() << '\n';
        return 1;
    }
}
