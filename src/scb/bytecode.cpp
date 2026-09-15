#include "bytecode_internal.hpp"
#include "scmd/bytecode.h"
#include "scmd/version.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <system_error>

namespace fs = std::filesystem;

namespace scmd::bc {

namespace {

constexpr std::array<char, 4> kMagic{'S','C','B','1'};
constexpr uint16_t kHeaderSize = 48;
constexpr uint32_t kMaxStrings = 4'000'000u;
constexpr uint32_t kMaxBlocks = 4'000'000u;
constexpr uint32_t kMaxInstructions = 50'000'000u;

uint64_t fnv1a64(const std::vector<uint8_t> &data) {
    uint64_t h = 1469598103934665603ULL;
    for (uint8_t c : data) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ULL;
    }
    return h;
}

void put_u16(std::vector<uint8_t> &out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v & 0xffu));
    out.push_back(static_cast<uint8_t>((v >> 8u) & 0xffu));
}

void put_u32(std::vector<uint8_t> &out, uint32_t v) {
    for (unsigned s = 0; s < 32; s += 8) out.push_back(static_cast<uint8_t>((v >> s) & 0xffu));
}

void put_u64(std::vector<uint8_t> &out, uint64_t v) {
    for (unsigned s = 0; s < 64; s += 8) out.push_back(static_cast<uint8_t>((v >> s) & 0xffu));
}

bool get_u16(const std::vector<uint8_t> &data, size_t &off, uint16_t &v) {
    if (off + 2u > data.size()) return false;
    v = static_cast<uint16_t>(data[off]) |
        static_cast<uint16_t>(static_cast<uint16_t>(data[off + 1u]) << 8u);
    off += 2u;
    return true;
}

bool get_u32(const std::vector<uint8_t> &data, size_t &off, uint32_t &v) {
    if (off + 4u > data.size()) return false;
    v = static_cast<uint32_t>(data[off]) |
        (static_cast<uint32_t>(data[off + 1u]) << 8u) |
        (static_cast<uint32_t>(data[off + 2u]) << 16u) |
        (static_cast<uint32_t>(data[off + 3u]) << 24u);
    off += 4u;
    return true;
}

bool get_u64(const std::vector<uint8_t> &data, size_t &off, uint64_t &v) {
    if (off + 8u > data.size()) return false;
    v = 0;
    for (unsigned s = 0; s < 64; s += 8) {
        v |= static_cast<uint64_t>(data[off++]) << s;
    }
    return true;
}

bool parse_u64(std::string_view sv, uint64_t &out) {
    if (sv.empty()) return false;
    const char *first = sv.data();
    const char *last = first + sv.size();
    auto [ptr, ec] = std::from_chars(first, last, out);
    return ec == std::errc{} && ptr == last;
}

Instruction make_instr(Op op, uint8_t dst = 0, uint8_t a = 0, uint8_t b = 0,
                       uint32_t x = 0, uint32_t y = 0, uint32_t z = 0) {
    Instruction ins{};
    ins.op = static_cast<uint8_t>(op);
    ins.dst = dst;
    ins.a = a;
    ins.b = b;
    ins.x = x;
    ins.y = y;
    ins.z = z;
    return ins;
}

void emit_kstr(Package &pkg, std::vector<Instruction> &out, uint8_t reg, std::string_view value) {
    out.push_back(make_instr(Op::KStr, reg, 0, 0, pkg.intern(value)));
}



void compile_command(Package &pkg, std::vector<Instruction> &out, std::string_view line) {
    const std::vector<std::string> argv = tokenize(line);
    if (argv.empty()) return;
    const std::string command = lower_ascii(argv[0]);

    if (command == "alias") {
        if (argv.size() == 1u) {
            out.push_back(make_instr(Op::AliasList));
            return;
        }
        const uint32_t name_sid = pkg.intern(lower_ascii(argv[1]));
        if (argv.size() == 2u) {
            out.push_back(make_instr(Op::AliasQueryI, 0, 0, 0, name_sid));
            return;
        }
        const std::string body = join_tokens(argv, 2);
        const uint32_t body_sid = pkg.intern(body);
        const uint32_t block_id = pkg.compile_text(body);
        out.push_back(make_instr(Op::AliasSetI, 0, 0, 0, name_sid, block_id, body_sid));
        return;
    }

    if (command == "exec" || command == "execifexists" || command == "exec_async") {
        if (argv.size() < 2u) {
            out.push_back(make_instr(Op::DispatchRaw, 0, 0, 0, pkg.intern(std::string(line))));
            return;
        }
        const uint32_t ref_sid = pkg.intern(argv[1]);
        const Op op = command == "exec" ? Op::ExecI :
                      command == "execifexists" ? Op::ExecIfExistsI : Op::ExecAsyncI;
        out.push_back(make_instr(op, 0, 0, 0, ref_sid));
        return;
    }

    if (command == "sleep") {
        uint64_t ms = 0;
        if (argv.size() >= 2u && parse_u64(argv[1], ms)) {
            out.push_back(make_instr(Op::SleepI, 0, 0, 0,
                                     static_cast<uint32_t>(ms & 0xffffffffULL),
                                     static_cast<uint32_t>(ms >> 32u)));
        } else {
            out.push_back(make_instr(Op::DispatchRaw, 0, 0, 0, pkg.intern(std::string(line))));
        }
        return;
    }

    if (command == "clear") {
        out.push_back(make_instr(Op::Clear));
        return;
    }

    if (command == "echo" || command == "echoln" || command == "say" || command == "say_team") {
        const uint32_t text_sid = pkg.intern(join_tokens(argv, 1));
        Op op = Op::EchoI;
        if (command == "echoln") op = Op::EchoLnI;
        else if (command == "say") op = Op::SayI;
        else if (command == "say_team") op = Op::SayTeamI;
        out.push_back(make_instr(op, 0, 0, 0, text_sid));
        return;
    }

    if (command == "setinfo") {
        if (argv.size() >= 3u) {
            out.push_back(make_instr(Op::SetInfoI, 0, 0, 0, pkg.intern(argv[1]), pkg.intern(argv[2])));
        } else {
            out.push_back(make_instr(Op::DispatchRaw, 0, 0, 0, pkg.intern(std::string(line))));
        }
        return;
    }

    if (command == "incrementvar" || command == "multvar") {
        if (argv.size() >= 5u) {
            for (uint8_t r = 0; r < 4; ++r) emit_kstr(pkg, out, r, argv[static_cast<size_t>(r) + 1u]);
            out.push_back(make_instr(command == "incrementvar" ? Op::IncrementVar : Op::MultVar,
                                     0, 0, 4));
        } else {
            out.push_back(make_instr(Op::DispatchRaw, 0, 0, 0, pkg.intern(std::string(line))));
        }
        return;
    }

    if (command == "toggle") {
        const size_t argc = argv.size() > 1u ? argv.size() - 1u : 0u;
        if (argc > 0u && argc <= kRegisterCount) {
            for (size_t i = 0; i < argc; ++i) emit_kstr(pkg, out, static_cast<uint8_t>(i), argv[i + 1u]);
            out.push_back(make_instr(Op::Toggle, 0, 0, static_cast<uint8_t>(argc)));
        } else {
            out.push_back(make_instr(Op::DispatchRaw, 0, 0, 0, pkg.intern(std::string(line))));
        }
        return;
    }

    if (argv.size() == 1u) {
        out.push_back(make_instr(Op::Dispatch0, 0, 0, 0, pkg.intern(argv[0])));
        return;
    }
    if (argv.size() == 2u) {
        out.push_back(make_instr(Op::Dispatch1, 0, 0, 0, pkg.intern(argv[0]), pkg.intern(argv[1])));
        return;
    }

    const size_t argc = argv.size();
    if (argc <= kRegisterCount) {
        for (size_t i = 0; i < argc; ++i) emit_kstr(pkg, out, static_cast<uint8_t>(i), argv[i]);
        out.push_back(make_instr(Op::Dispatch, 0, 0, static_cast<uint8_t>(argc)));
    } else {
        out.push_back(make_instr(Op::DispatchRaw, 0, 0, 0, pkg.intern(std::string(line))));
    }
}

std::string block_context(const Package &pkg, size_t block_index) {
    if (block_index >= pkg.blocks.size()) return "block<?>";
    const Block &b = pkg.blocks[block_index];
    if (b.name_sid != kNoString && b.name_sid < pkg.strings.size()) return pkg.strings[b.name_sid];
    return "anonymous block " + std::to_string(block_index);
}

} // namespace

std::string trim(std::string_view sv) {
    size_t a = 0;
    while (a < sv.size() && (sv[a] == ' ' || sv[a] == '\t' || sv[a] == '\r' || sv[a] == '\n')) ++a;
    size_t b = sv.size();
    while (b > a && (sv[b - 1u] == ' ' || sv[b - 1u] == '\t' || sv[b - 1u] == '\r' || sv[b - 1u] == '\n')) --b;
    return std::string(sv.substr(a, b - a));
}

std::string lower_ascii(std::string_view sv) {
    std::string s;
    s.reserve(sv.size());
    for (char raw : sv) {
        unsigned char c = static_cast<unsigned char>(raw);
        if (c >= 'A' && c <= 'Z') c = static_cast<unsigned char>(c - 'A' + 'a');
        s.push_back(static_cast<char>(c));
    }
    return s;
}

std::vector<std::string> split_commands(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    cur.reserve(128);
    bool quote = false;
    bool comment = false;
    auto flush = [&]() {
        std::string s = trim(cur);
        if (!s.empty()) out.push_back(std::move(s));
        cur.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (comment) {
            if (c == '\n') { comment = false; flush(); }
            continue;
        }
        if (!quote && c == '/' && i + 1u < text.size() && text[i + 1u] == '/') {
            comment = true;
            ++i;
            continue;
        }
        if (c == '"') { quote = !quote; cur.push_back(c); continue; }
        if (!quote && (c == ';' || c == '\n')) { flush(); continue; }
        if (c != '\r') cur.push_back(c);
    }
    flush();
    return out;
}

std::vector<std::string> tokenize(std::string_view line) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        std::string tok;
        if (line[i] == '"') {
            ++i;
            while (i < line.size()) {
                const char c = line[i++];
                if (c == '"') break;
                // Console CFG is not a C string literal. Preserve backslashes;
                // quotes delimit tokens rather than being escaped with \.
                tok.push_back(c);
            }
        } else {
            while (i < line.size() && line[i] != ' ' && line[i] != '\t') tok.push_back(line[i++]);
        }
        out.push_back(std::move(tok));
    }
    return out;
}

std::string join_tokens(const std::vector<std::string> &v, size_t first) {
    std::string out;
    for (size_t i = first; i < v.size(); ++i) {
        if (i != first) out.push_back(' ');
        out += v[i];
    }
    return out;
}

std::string normalize_exec_ref(std::string ref) {
    std::replace(ref.begin(), ref.end(), '\\', '/');
    while (ref.rfind("./", 0) == 0) ref.erase(0, 2);
    if (ref.size() >= 4u && lower_ascii(ref.substr(ref.size() - 4u)) == ".cfg") ref.resize(ref.size() - 4u);
    fs::path p(ref);
    p = p.lexically_normal();
    std::string out = p.generic_string();
    if (out == ".") out.clear();
    return lower_ascii(out);
}

uint32_t Package::intern(std::string_view value) {
    std::string key(value);
    auto it = string_ids.find(key);
    if (it != string_ids.end()) return it->second;
    if (strings.size() >= static_cast<size_t>(std::numeric_limits<uint32_t>::max())) throw std::runtime_error("string pool overflow");
    const uint32_t id = static_cast<uint32_t>(strings.size());
    strings.push_back(key);
    string_ids.emplace(std::move(key), id);
    return id;
}

const std::string &Package::str(uint32_t sid) const {
    if (sid >= strings.size()) throw std::out_of_range("SCB string id out of range");
    return strings[sid];
}

uint32_t Package::compile_text(std::string_view text, std::string_view block_name, uint32_t flags) {
    if (blocks.size() >= static_cast<size_t>(std::numeric_limits<uint32_t>::max())) throw std::runtime_error("block table overflow");
    const uint32_t block_id = static_cast<uint32_t>(blocks.size());
    blocks.push_back(Block{});
    std::vector<Instruction> local;
    const std::vector<std::string> commands = split_commands(text);
    local.reserve(commands.size() * 3u + 1u);
    for (const std::string &cmd : commands) {
        if (cmd.size() > SCMD_CS2_MAX_COMMAND_BYTES) {
            std::cerr << "WARNING: Command too long... ignoring!\n" << cmd << '\n';
            continue;
        }
        compile_command(*this, local, cmd);
    }
    local.push_back(make_instr(Op::Ret));
    const uint32_t first = static_cast<uint32_t>(code.size());
    code.insert(code.end(), local.begin(), local.end());
    Block b{};
    b.name_sid = block_name.empty() ? kNoString : intern(block_name);
    b.first = first;
    b.count = static_cast<uint32_t>(local.size());
    b.flags = flags;
    blocks[block_id] = b;
    if ((flags & kBlockModule) != 0u && !block_name.empty()) {
        const std::string key = normalize_exec_ref(std::string(block_name));
        modules[key] = block_id;
        module_names.push_back(std::string(block_name));
    }
    return block_id;
}

bool Package::compile_cfg_module(const fs::path &path, std::string_view module_name, std::string &error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot read cfg: " + path.string(); return false; }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (!f.good() && !f.eof()) { error = "failed while reading cfg: " + path.string(); return false; }
    const std::string display(module_name);
    const std::string key = normalize_exec_ref(display);
    const bool known = modules.find(key) != modules.end();
    const uint32_t block = compile_text(ss.str(), display, kBlockModule);
    modules[key] = block;
    if (known) {
        /* compile_text appends the display name for module blocks. Keep the public
         * completion list unique even when a source module is hot-reloaded. */
        std::unordered_map<std::string, bool> seen;
        std::vector<std::string> unique;
        unique.reserve(module_names.size());
        for (const std::string &name : module_names) {
            const std::string nk = normalize_exec_ref(name);
            if (!seen[nk]) { seen[nk] = true; unique.push_back(name); }
        }
        module_names.swap(unique);
    }
    return true;
}

bool Package::merge_from(const Package &other, std::string &error) {
    if (other.profile != profile) {
        error = "cannot merge SCB package with profile '" + other.profile + "' into '" + profile + "'";
        return false;
    }
    if (strings.size() + other.strings.size() > kMaxStrings ||
        blocks.size() + other.blocks.size() > kMaxBlocks ||
        code.size() + other.code.size() > kMaxInstructions) {
        error = "merged SCB package would exceed verifier limits";
        return false;
    }

    std::vector<uint32_t> smap(other.strings.size(), kNoString);
    for (size_t i = 0; i < other.strings.size(); ++i) smap[i] = intern(other.strings[i]);

    const uint32_t block_base = static_cast<uint32_t>(blocks.size());
    const uint32_t code_base = static_cast<uint32_t>(code.size());
    std::vector<uint32_t> bmap(other.blocks.size(), 0u);
    for (size_t i = 0; i < other.blocks.size(); ++i) bmap[i] = block_base + static_cast<uint32_t>(i);

    blocks.reserve(blocks.size() + other.blocks.size());
    for (const Block &ob : other.blocks) {
        Block b = ob;
        if (b.name_sid != kNoString) {
            if (b.name_sid >= smap.size()) { error = "merge: invalid block name string id"; return false; }
            b.name_sid = smap[b.name_sid];
        }
        b.first += code_base;
        blocks.push_back(b);
    }

    code.reserve(code.size() + other.code.size());
    for (const Instruction &src : other.code) {
        Instruction ins = src;
        const Op op = static_cast<Op>(ins.op);
        auto map_string = [&](uint32_t &id) -> bool {
            if (id >= smap.size()) return false;
            id = smap[id];
            return true;
        };
        auto map_block = [&](uint32_t &id) -> bool {
            if (id >= bmap.size()) return false;
            id = bmap[id];
            return true;
        };
        if (op == Op::KStr || op == Op::DispatchRaw || op == Op::AliasQueryI ||
            op == Op::ExecI || op == Op::ExecIfExistsI || op == Op::ExecAsyncI ||
            op == Op::EchoI || op == Op::EchoLnI || op == Op::SayI || op == Op::SayTeamI ||
            op == Op::Dispatch0) {
            if (!map_string(ins.x)) { error = "merge: invalid string operand"; return false; }
        } else if (op == Op::Dispatch1 || op == Op::SetInfoI) {
            if (!map_string(ins.x) || !map_string(ins.y)) { error = "merge: invalid string pair operand"; return false; }
        } else if (op == Op::AliasSetI) {
            if (!map_string(ins.x) || !map_block(ins.y) || !map_string(ins.z)) {
                error = "merge: invalid immediate alias operand"; return false;
            }
        } else if (op == Op::AliasSet) {
            if (!map_block(ins.x) || !map_string(ins.y)) { error = "merge: invalid alias operand"; return false; }
        }
        code.push_back(ins);
    }

    for (const auto &kv : other.modules) {
        if (kv.second >= bmap.size()) { error = "merge: invalid module block id"; return false; }
        modules[kv.first] = bmap[kv.second];
    }
    std::unordered_map<std::string, bool> seen;
    for (const std::string &name : module_names) seen[normalize_exec_ref(name)] = true;
    for (const std::string &name : other.module_names) {
        const std::string key = normalize_exec_ref(name);
        if (!seen[key]) { seen[key] = true; module_names.push_back(name); }
    }
    std::sort(module_names.begin(), module_names.end());
    return verify(error);
}

bool Package::compile_cfg_root(const fs::path &root, std::string &error) {
    strings.clear(); string_ids.clear(); blocks.clear(); code.clear(); modules.clear(); module_names.clear();
    profile = SCMD_CS2_PROFILE;
    intern(profile);
    std::error_code ec;
    fs::path abs = fs::absolute(root, ec);
    if (ec || !fs::exists(abs) || !fs::is_directory(abs)) {
        error = "cfg root does not exist: " + root.string();
        return false;
    }
    std::vector<fs::path> files;
    for (fs::recursive_directory_iterator it(abs, fs::directory_options::skip_permission_denied, ec), end; it != end; it.increment(ec)) {
        if (ec) { ec.clear(); continue; }
        if (!it->is_regular_file()) continue;
        if (lower_ascii(it->path().extension().string()) == ".cfg") files.push_back(it->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path &path : files) {
        fs::path rel = fs::relative(path, abs, ec);
        if (ec) { error = "cannot make cfg path relative: " + path.string(); return false; }
        rel.replace_extension();
        if (!compile_cfg_module(path, rel.generic_string(), error)) return false;
    }
    std::sort(module_names.begin(), module_names.end());
    return verify(error);
}

uint32_t Package::find_module(std::string_view ref) const {
    const std::string key = normalize_exec_ref(std::string(ref));
    auto it = modules.find(key);
    return it == modules.end() ? std::numeric_limits<uint32_t>::max() : it->second;
}

std::vector<std::string> Package::complete_exec(std::string_view prefix_view) const {
    std::string prefix(prefix_view);
    std::replace(prefix.begin(), prefix.end(), '\\', '/');
    const std::string lower_prefix = lower_ascii(prefix);
    const size_t slash = prefix.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "" : prefix.substr(0, slash + 1u);
    const std::string leaf = slash == std::string::npos ? prefix : prefix.substr(slash + 1u);
    const std::string lower_dir = lower_ascii(dir);
    const std::string lower_leaf = lower_ascii(leaf);
    std::vector<std::string> out;
    std::unordered_map<std::string, bool> seen;
    for (const std::string &name_raw : module_names) {
        std::string name = name_raw;
        std::replace(name.begin(), name.end(), '\\', '/');
        const std::string lname = lower_ascii(name);
        if (!lower_dir.empty() && lname.rfind(lower_dir, 0) != 0) continue;
        if (lower_dir.empty() && name.find('/') != std::string::npos) {
            const size_t first_slash = name.find('/');
            const std::string child = name.substr(0, first_slash + 1u);
            if (lower_ascii(child).rfind(lower_leaf, 0) == 0 && !seen[lower_ascii(child)]) {
                seen[lower_ascii(child)] = true; out.push_back(child);
            }
            continue;
        }
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

bool Package::verify(std::string &error) const {
    if (strings.size() > kMaxStrings || blocks.size() > kMaxBlocks || code.size() > kMaxInstructions) {
        error = "SCB package exceeds verifier limits";
        return false;
    }
    const uint8_t max_op = static_cast<uint8_t>(Op::Ret);
    for (size_t bi = 0; bi < blocks.size(); ++bi) {
        const Block &b = blocks[bi];
        if (b.name_sid != kNoString && b.name_sid >= strings.size()) {
            error = block_context(*this, bi) + ": invalid name string id"; return false;
        }
        if (static_cast<uint64_t>(b.first) + static_cast<uint64_t>(b.count) > code.size()) {
            error = block_context(*this, bi) + ": instruction range out of bounds"; return false;
        }
    }
    for (size_t i = 0; i < code.size(); ++i) {
        const Instruction &ins = code[i];
        if (ins.op > max_op) { error = "instruction " + std::to_string(i) + ": invalid opcode"; return false; }
        const Op op = static_cast<Op>(ins.op);
        if ((op == Op::KStr || op == Op::DispatchRaw || op == Op::AliasQueryI ||
             op == Op::ExecI || op == Op::ExecIfExistsI || op == Op::ExecAsyncI ||
             op == Op::EchoI || op == Op::EchoLnI || op == Op::SayI || op == Op::SayTeamI ||
             op == Op::Dispatch0) && ins.x >= strings.size()) {
            error = "instruction " + std::to_string(i) + ": string id out of range"; return false;
        }
        if (op == Op::Dispatch1 && (ins.x >= strings.size() || ins.y >= strings.size())) {
            error = "instruction " + std::to_string(i) + ": dispatch string id out of range"; return false;
        }
        if (op == Op::SetInfoI && (ins.x >= strings.size() || ins.y >= strings.size())) {
            error = "instruction " + std::to_string(i) + ": setinfo string id out of range"; return false;
        }
        if (op == Op::AliasSetI && (ins.x >= strings.size() || ins.y >= blocks.size() || ins.z >= strings.size())) {
            error = "instruction " + std::to_string(i) + ": invalid immediate alias operands"; return false;
        }
        if (op == Op::AliasSet) {
            if (ins.a >= kRegisterCount || ins.x >= blocks.size() || ins.y >= strings.size()) {
                error = "instruction " + std::to_string(i) + ": invalid alias operands"; return false;
            }
        }
        if ((op == Op::AliasQuery || op == Op::Exec || op == Op::ExecIfExists || op == Op::ExecAsync ||
             op == Op::Sleep || op == Op::Echo || op == Op::EchoLn || op == Op::Say || op == Op::SayTeam) && ins.a >= kRegisterCount) {
            error = "instruction " + std::to_string(i) + ": register out of range"; return false;
        }
        if ((op == Op::SetInfo || op == Op::IncrementVar || op == Op::MultVar || op == Op::Toggle || op == Op::Dispatch) &&
            (ins.a >= kRegisterCount || ins.b > kRegisterCount || static_cast<size_t>(ins.a) + static_cast<size_t>(ins.b) > kRegisterCount)) {
            error = "instruction " + std::to_string(i) + ": register window out of range"; return false;
        }
        if ((op == Op::KStr || op == Op::KImm) && ins.dst >= kRegisterCount) {
            error = "instruction " + std::to_string(i) + ": destination register out of range"; return false;
        }
    }
    for (const auto &kv : modules) {
        if (kv.second >= blocks.size()) { error = "module table block out of range"; return false; }
    }
    return true;
}

bool Package::save(const fs::path &path, std::string &error) const {
    std::string verr;
    if (!verify(verr)) { error = "refusing to write invalid package: " + verr; return false; }
    std::vector<uint8_t> payload;
    size_t string_bytes = 0;
    for (const auto &s : strings) string_bytes += 4u + s.size();
    payload.reserve(string_bytes + blocks.size() * 16u + code.size() * 16u);
    for (const std::string &s : strings) {
        if (s.size() > std::numeric_limits<uint32_t>::max()) { error = "string too large"; return false; }
        put_u32(payload, static_cast<uint32_t>(s.size()));
        payload.insert(payload.end(), s.begin(), s.end());
    }
    for (const Block &b : blocks) {
        put_u32(payload, b.name_sid); put_u32(payload, b.first); put_u32(payload, b.count); put_u32(payload, b.flags);
    }
    for (const Instruction &ins : code) {
        payload.push_back(ins.op); payload.push_back(ins.dst); payload.push_back(ins.a); payload.push_back(ins.b);
        put_u32(payload, ins.x); put_u32(payload, ins.y); put_u32(payload, ins.z);
    }
    const uint64_t checksum = fnv1a64(payload);
    std::vector<uint8_t> header;
    header.reserve(kHeaderSize);
    header.insert(header.end(), kMagic.begin(), kMagic.end());
    put_u16(header, kAbiVersion); put_u16(header, kHeaderSize);
    put_u32(header, 0u);
    uint32_t profile_sid = kNoString;
    auto pit = string_ids.find(profile);
    if (pit != string_ids.end()) profile_sid = pit->second;
    else {
        for (size_t i = 0; i < strings.size(); ++i) {
            if (strings[i] == profile) { profile_sid = static_cast<uint32_t>(i); break; }
        }
    }
    if (profile_sid == kNoString) { error = "profile missing from string pool"; return false; }
    put_u32(header, profile_sid);
    put_u32(header, static_cast<uint32_t>(strings.size()));
    put_u32(header, static_cast<uint32_t>(blocks.size()));
    put_u32(header, static_cast<uint32_t>(code.size()));
    put_u32(header, 0u);
    put_u64(header, static_cast<uint64_t>(payload.size()));
    put_u64(header, checksum);
    if (header.size() != kHeaderSize) { error = "internal SCB header size mismatch"; return false; }
    std::ofstream f(path, std::ios::binary);
    if (!f) { error = "cannot create SCB: " + path.string(); return false; }
    f.write(reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size()));
    f.write(reinterpret_cast<const char *>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!f) { error = "failed while writing SCB: " + path.string(); return false; }
    return true;
}

bool Package::load(const fs::path &path, std::string &error) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { error = "cannot open SCB: " + path.string(); return false; }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.size() < kHeaderSize) { error = "SCB file too short"; return false; }
    if (!std::equal(kMagic.begin(), kMagic.end(), data.begin())) { error = "invalid SCB magic"; return false; }
    size_t off = 4;
    uint16_t abi = 0, hsize = 0;
    uint32_t flags = 0, profile_sid = 0, string_count = 0, block_count = 0, ins_count = 0, reserved = 0;
    uint64_t payload_bytes = 0, checksum = 0;
    if (!get_u16(data, off, abi) || !get_u16(data, off, hsize) || !get_u32(data, off, flags) ||
        !get_u32(data, off, profile_sid) || !get_u32(data, off, string_count) || !get_u32(data, off, block_count) ||
        !get_u32(data, off, ins_count) || !get_u32(data, off, reserved) || !get_u64(data, off, payload_bytes) ||
        !get_u64(data, off, checksum)) { error = "truncated SCB header"; return false; }
    (void)flags; (void)reserved;
    if (abi != kAbiVersion) { error = "unsupported SCB ABI " + std::to_string(abi); return false; }
    if (hsize != kHeaderSize) { error = "unsupported SCB header size"; return false; }
    if (string_count > kMaxStrings || block_count > kMaxBlocks || ins_count > kMaxInstructions) { error = "SCB count exceeds verifier limits"; return false; }
    if (payload_bytes != data.size() - kHeaderSize) { error = "SCB payload length mismatch"; return false; }
    std::vector<uint8_t> payload(data.begin() + kHeaderSize, data.end());
    if (fnv1a64(payload) != checksum) { error = "SCB checksum mismatch"; return false; }

    strings.clear(); string_ids.clear(); blocks.clear(); code.clear(); modules.clear(); module_names.clear();
    off = kHeaderSize;
    strings.reserve(string_count);
    for (uint32_t i = 0; i < string_count; ++i) {
        uint32_t len = 0;
        if (!get_u32(data, off, len) || off + len > data.size()) { error = "truncated SCB string table"; return false; }
        std::string s(reinterpret_cast<const char *>(data.data() + off), len);
        off += len;
        /* Loaded packages keep the base string pool immutable. We intentionally
           do not rebuild the huge reverse hash here; interactive/JIT strings may
           be appended as duplicates without changing bytecode semantics. */
        strings.push_back(std::move(s));
    }
    if (profile_sid >= strings.size()) { error = "invalid SCB profile string id"; return false; }
    profile = strings[profile_sid];
    blocks.reserve(block_count);
    for (uint32_t i = 0; i < block_count; ++i) {
        Block b{};
        if (!get_u32(data, off, b.name_sid) || !get_u32(data, off, b.first) || !get_u32(data, off, b.count) || !get_u32(data, off, b.flags)) {
            error = "truncated SCB block table"; return false;
        }
        blocks.push_back(b);
    }
    code.reserve(ins_count);
    for (uint32_t i = 0; i < ins_count; ++i) {
        if (off + 16u > data.size()) { error = "truncated SCB instruction stream"; return false; }
        Instruction ins{};
        ins.op = data[off++]; ins.dst = data[off++]; ins.a = data[off++]; ins.b = data[off++];
        if (!get_u32(data, off, ins.x) || !get_u32(data, off, ins.y) || !get_u32(data, off, ins.z)) { error = "truncated SCB instruction"; return false; }
        code.push_back(ins);
    }
    if (off != data.size()) { error = "unexpected trailing bytes in SCB"; return false; }
    for (uint32_t i = 0; i < blocks.size(); ++i) {
        const Block &b = blocks[i];
        if ((b.flags & kBlockModule) != 0u && b.name_sid != kNoString && b.name_sid < strings.size()) {
            modules[normalize_exec_ref(strings[b.name_sid])] = i;
            module_names.push_back(strings[b.name_sid]);
        }
    }
    std::sort(module_names.begin(), module_names.end());
    return verify(error);
}

} // namespace scmd::bc

extern "C" bool scmd_bytecode_pack_cfg_root(const char *cfg_root, const char *output_scb, const char *profile) {
    if (!cfg_root || !output_scb) {
        std::cerr << "scmdc: pack requires cfg root and output path\n";
        return false;
    }
    try {
        scmd::bc::Package pkg;
        std::string error;
        if (!pkg.compile_cfg_root(cfg_root, error)) {
            std::cerr << "scmdc: pack: " << error << '\n';
            return false;
        }
        if (profile && *profile) pkg.profile = profile;
        pkg.intern(pkg.profile);
        if (!pkg.save(output_scb, error)) {
            std::cerr << "scmdc: pack: " << error << '\n';
            return false;
        }
        std::cout << "scmdc: packed " << pkg.module_names.size() << " cfg module(s), "
                  << pkg.blocks.size() << " block(s), " << pkg.code.size() << " instruction(s) -> "
                  << output_scb << '\n';
        return true;
    } catch (const std::exception &e) {
        std::cerr << "scmdc: pack: fatal error: " << e.what() << '\n';
        return false;
    }
}
