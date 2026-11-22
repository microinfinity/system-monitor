/// @file system_telemetry.cpp
/// @brief Sovereign C++20 Linux Kernel Telemetry, Process Table & Desktop App Grouping Engine.

#include "system_telemetry.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <pwd.h>
#include <set>
#include <signal.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <unordered_set>

namespace sysmon {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace {

// ============================================================================
// Fast Zero-Copy Text & Linux Virtual Filesystem Utilities
// ============================================================================

constexpr bool is_ascii_ws(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f';
}

constexpr char to_ascii_lower(char c) noexcept {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view sv_trim(std::string_view sv) noexcept {
    while (!sv.empty() && is_ascii_ws(sv.front())) sv.remove_prefix(1);
    while (!sv.empty() && is_ascii_ws(sv.back())) sv.remove_suffix(1);
    return sv;
}

std::string str_lower_trimmed(std::string_view sv) {
    sv = sv_trim(sv);
    std::string out(sv);
    for (char& c : out) c = to_ascii_lower(c);
    return out;
}

std::vector<std::string_view> sv_split_char(std::string_view sv, char sep) {
    std::vector<std::string_view> out;
    size_t pos = 0;
    while (true) {
        size_t next = sv.find(sep, pos);
        if (next == std::string_view::npos) {
            out.push_back(sv.substr(pos));
            break;
        }
        out.push_back(sv.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

std::vector<std::string_view> sv_split_ws(std::string_view sv) {
    std::vector<std::string_view> out;
    size_t i = 0;
    while (i < sv.size()) {
        while (i < sv.size() && is_ascii_ws(sv[i])) ++i;
        size_t start = i;
        while (i < sv.size() && !is_ascii_ws(sv[i])) ++i;
        if (i > start) out.push_back(sv.substr(start, i - start));
    }
    return out;
}

template <typename IntT>
bool sv_to_int(std::string_view sv, IntT& out) noexcept {
    sv = sv_trim(sv);
    if (sv.empty()) return false;
    if constexpr (std::is_unsigned_v<IntT>) {
        if (sv.front() == '-' || sv.front() == '+') return false;
    } else {
        if (sv.front() == '+') sv.remove_prefix(1);
        if (sv.empty()) return false;
    }
    auto [ptr, ec] = std::from_chars(sv.data(), sv.data() + sv.size(), out);
    return ec == std::errc() && ptr == sv.data() + sv.size();
}

uint64_t sv_u64(std::string_view sv, uint64_t fallback = 0) noexcept {
    uint64_t v = fallback;
    return sv_to_int(sv, v) ? v : fallback;
}

int64_t sv_i64(std::string_view sv, int64_t fallback = 0) noexcept {
    int64_t v = fallback;
    return sv_to_int(sv, v) ? v : fallback;
}

int sv_i32(std::string_view sv, int fallback = 0) noexcept {
    int v = fallback;
    return sv_to_int(sv, v) ? v : fallback;
}

std::string sanitize_utf8(std::string_view raw) {
    std::string clean;
    clean.reserve(raw.size());
    size_t i = 0;
    while (i < raw.size()) {
        uint8_t b0 = static_cast<uint8_t>(raw[i]);
        if (b0 < 0x80) {
            clean.push_back(static_cast<char>(b0));
            ++i;
            continue;
        }
        size_t seq_len = 0;
        uint32_t cp = 0;
        if ((b0 & 0xE0) == 0xC0) { seq_len = 2; cp = b0 & 0x1F; }
        else if ((b0 & 0xF0) == 0xE0) { seq_len = 3; cp = b0 & 0x0F; }
        else if ((b0 & 0xF8) == 0xF0) { seq_len = 4; cp = b0 & 0x07; }

        bool ok = (seq_len > 0 && i + seq_len <= raw.size());
        for (size_t k = 1; ok && k < seq_len; ++k) {
            uint8_t bk = static_cast<uint8_t>(raw[i + k]);
            ok = ((bk & 0xC0) == 0x80);
            cp = (cp << 6) | (bk & 0x3F);
        }
        constexpr uint32_t kMinCp[5] = {0, 0, 0x80, 0x800, 0x10000};
        if (ok && cp >= kMinCp[seq_len] && cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF)) {
            clean.append(raw.substr(i, seq_len));
            i += seq_len;
        } else {
            clean.append("\xEF\xBF\xBD");
            ++i;
        }
    }
    return clean;
}

bool icontains(std::string_view haystack, std::string_view needle) noexcept {
    if (needle.empty()) return true;
    if (haystack.size() < needle.size()) return false;
    auto it = std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end(),
                          [](char a, char b) { return to_ascii_lower(a) == to_ascii_lower(b); });
    return it != haystack.end();
}

int icompare(std::string_view a, std::string_view b) noexcept {
    const size_t n = std::min(a.size(), b.size());
    for (size_t i = 0; i < n; ++i) {
        char ca = to_ascii_lower(a[i]);
        char cb = to_ascii_lower(b[i]);
        if (ca != cb) return (static_cast<uint8_t>(ca) < static_cast<uint8_t>(cb)) ? -1 : 1;
    }
    if (a.size() != b.size()) return (a.size() < b.size()) ? -1 : 1;
    return a.compare(b);
}

std::string slurp_file(const std::string& path) {
    int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return {};
    std::string out;
    char buf[16384];
    while (true) {
        ssize_t n = ::read(fd, buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        out.append(buf, static_cast<size_t>(n));
    }
    ::close(fd);
    return out;
}

bool path_exists(const std::string& p) noexcept {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
}

bool path_is_dir(const std::string& p) noexcept {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string read_symlink_resolved(const std::string& link_path) {
    char buf[PATH_MAX];
    ssize_t len = ::readlink(link_path.c_str(), buf, sizeof(buf));
    if (len <= 0 || static_cast<size_t>(len) == sizeof(buf)) return {};
    std::string target(buf, static_cast<size_t>(len));
    if (!target.empty() && target.front() != '/') {
        size_t slash = link_path.rfind('/');
        std::string combined = (slash == std::string::npos ? "." : link_path.substr(0, slash)) + "/" + target;
        char resolved[PATH_MAX];
        if (::realpath(combined.c_str(), resolved)) return resolved;
        return combined;
    }
    return target;
}

std::string path_basename(std::string_view p) {
    size_t slash = p.rfind('/');
    return std::string(slash == std::string::npos ? p : p.substr(slash + 1));
}

std::string path_stem(std::string_view p) {
    std::string base = path_basename(p);
    size_t dot = base.rfind('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

std::vector<std::string> list_dir_names(const std::string& dir_path) {
    std::vector<std::string> names;
    DIR* d = ::opendir(dir_path.c_str());
    if (!d) return names;
    while (const dirent* e = ::readdir(d)) {
        std::string_view sv(e->d_name);
        if (sv != "." && sv != "..") names.emplace_back(sv);
    }
    ::closedir(d);
    return names;
}

constexpr uint64_t u64_delta(uint64_t cur, uint64_t prev) noexcept {
    return (cur >= prev) ? (cur - prev) : 0;
}

constexpr double calc_rate(uint64_t cur, uint64_t prev, double dt_sec) noexcept {
    return (dt_sec > 0.0) ? (static_cast<double>(u64_delta(cur, prev)) / dt_sec) : 0.0;
}

// ============================================================================
// Desktop Application Matcher (.desktop + Portable App Bundles)
// ============================================================================

bool has_word_prefix(const std::string& s, const std::string& prefix) noexcept {
    if (prefix.empty() || !s.starts_with(prefix)) return false;
    if (s.size() == prefix.size()) return true;
    char boundary = s[prefix.size()];
    return boundary == '-' || boundary == '.' || boundary == '_' || boundary == ' ';
}

bool has_word_suffix(const std::string& s, const std::string& suffix) noexcept {
    if (suffix.empty() || s.size() <= suffix.size()) return false;
    if (s.compare(s.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    char boundary = s[s.size() - suffix.size() - 1];
    return boundary == '-' || boundary == '.' || boundary == '_' || boundary == ' ';
}

bool has_script_ext(const std::string& filename) noexcept {
    size_t dot = filename.rfind('.');
    if (dot == std::string::npos) return false;
    std::string_view ext = std::string_view(filename).substr(dot + 1);
    for (std::string_view s : {"py", "pl", "rb", "sh", "js", "lua", "tcl"}) {
        if (ext == s) return true;
    }
    return false;
}

bool is_interpreter_bin(std::string_view bin) noexcept {
    for (std::string_view prefix : {"python", "perl", "ruby", "lua"}) {
        if (bin.starts_with(prefix)) {
            auto tail = bin.substr(prefix.size());
            if (std::all_of(tail.begin(), tail.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; })) {
                return true;
            }
        }
    }
    for (std::string_view exact : {"node", "nodejs", "gjs", "sh", "bash", "dash"}) {
        if (bin == exact) return true;
    }
    return false;
}

std::string extract_exec_binary(const std::string& exec_line) {
    std::vector<std::string> tokens;
    std::string cur;
    bool in_quote = false;
    char qchar = 0;

    for (size_t i = 0; i < exec_line.size(); ++i) {
        char c = exec_line[i];
        if (c == '\\' && i + 1 < exec_line.size()) {
            cur.push_back(exec_line[++i]);
            continue;
        }
        if (c == '"' || c == '\'') {
            if (!in_quote) { in_quote = true; qchar = c; }
            else if (qchar == c) { in_quote = false; }
            else { cur.push_back(c); }
            continue;
        }
        if (!in_quote && is_ascii_ws(c)) {
            if (!cur.empty()) {
                tokens.push_back(std::move(cur));
                cur.clear();
            }
            continue;
        }
        cur.push_back(c);
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));

    for (const auto& tok : tokens) {
        if (tok.empty() || tok.front() == '%' || tok.find('=') != std::string::npos) continue;
        std::string b = path_basename(tok);
        if (b == "env" || b == "sh" || b == "bash" || b == "dbus-launch") continue;
        return tok;
    }
    return {};
}

class DesktopCatalog {
public:
    static DesktopCatalog& instance() {
        static DesktopCatalog cat;
        return cat;
    }

    std::optional<DesktopAppInfo> resolveProcess(const std::string& proc_name, const std::string& prog_path) {
        std::lock_guard<std::mutex> lk(mtx_);
        ensureScanned();
        return matchEntry(proc_name, proc_name, prog_path, proc_name);
    }

    std::optional<DesktopAppInfo> findById(const std::string& id) {
        std::lock_guard<std::mutex> lk(mtx_);
        ensureScanned();
        auto it = by_id_.find(id);
        if (it != by_id_.end()) return it->second;
        std::string low = str_lower_trimmed(id);
        for (const auto& [k, v] : by_id_) {
            if (str_lower_trimmed(k) == low) return v;
        }
        return std::nullopt;
    }

private:
    struct EntryRecord {
        DesktopAppInfo info;
        std::string file_path;
        std::string exec_prog;
        std::string canonical_exec;
        std::string wm_class;
        bool terminal = false;
    };

    std::mutex mtx_;
    bool scanned_{false};
    std::vector<EntryRecord> records_;
    std::unordered_map<std::string, DesktopAppInfo> by_id_;
    std::unordered_map<std::string, DesktopAppInfo> portable_cache_;

    void ensureScanned() {
        if (scanned_) return;
        scanned_ = true;

        std::vector<std::string> roots;
        if (const char* home = std::getenv("HOME")) {
            roots.push_back(std::string(home) + "/.local/share/applications");
            roots.push_back(std::string(home) + "/.local/share/flatpak/exports/share/applications");
        }
        roots.push_back("/usr/local/share/applications");
        roots.push_back("/usr/share/applications");
        roots.push_back("/var/lib/flatpak/exports/share/applications");
        roots.push_back("/var/lib/snapd/desktop/applications");

        std::set<std::string> seen_ids;
        for (const auto& root : roots) {
            std::error_code ec;
            if (!fs::is_directory(root, ec)) continue;

            std::vector<std::string> desktop_files;
            for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
                 it != fs::recursive_directory_iterator(); it.increment(ec)) {
                if (ec) break;
                if (it->path().extension() == ".desktop" && it->is_regular_file(ec)) {
                    desktop_files.push_back(it->path().string());
                }
            }
            std::sort(desktop_files.begin(), desktop_files.end());

            for (const auto& fpath : desktop_files) {
                std::string id = fs::path(fpath).lexically_relative(root).string();
                std::replace(id.begin(), id.end(), '/', '-');
                if (id.size() > 8 && id.ends_with(".desktop")) id.resize(id.size() - 8);
                if (seen_ids.contains(id)) continue;
                if (loadDesktopFile(fpath, id)) seen_ids.insert(id);
            }
        }
    }

    bool loadDesktopFile(const std::string& fpath, const std::string& id) {
        std::ifstream in(fpath);
        if (!in.is_open()) return false;

        EntryRecord rec;
        rec.info.id = id;
        rec.file_path = fpath;

        bool in_main_group = false;
        std::string entry_type;
        std::string raw_exec;
        std::string line;

        while (std::getline(in, line)) {
            std::string_view sv = sv_trim(line);
            if (sv.empty() || sv.front() == '#') continue;
            if (sv.front() == '[' && sv.back() == ']') {
                in_main_group = (sv == "[Desktop Entry]");
                continue;
            }
            if (!in_main_group) continue;
            size_t eq = sv.find('=');
            if (eq == std::string_view::npos) continue;
            std::string_view k = sv_trim(sv.substr(0, eq));
            std::string_view v = sv_trim(sv.substr(eq + 1));

            if (k == "Type") entry_type = std::string(v);
            else if (k == "Name" && rec.info.name.empty()) rec.info.name = std::string(v);
            else if (k == "Icon" && rec.info.icon.empty()) rec.info.icon = std::string(v);
            else if (k == "Exec" && raw_exec.empty()) raw_exec = std::string(v);
            else if (k == "StartupWMClass" && rec.wm_class.empty()) rec.wm_class = std::string(v);
            else if (k == "Terminal") rec.terminal = (v == "true" || v == "1" || v == "True");
            else if (k == "NoDisplay") rec.info.noDisplay = (v == "true" || v == "1" || v == "True");
            else if (k == "Hidden") rec.info.hidden = (v == "true" || v == "1" || v == "True");
        }

        if (!entry_type.empty() && entry_type != "Application") return false;
        if (rec.info.name.empty() && !rec.info.hidden) return false;

        rec.exec_prog = extract_exec_binary(raw_exec);
        if (!rec.exec_prog.empty()) {
            std::error_code ec;
            fs::path ep(rec.exec_prog);
            if (!ep.is_absolute()) {
                for (const char* bdir : {"/usr/local/bin", "/usr/bin", "/bin"}) {
                    fs::path cand = fs::path(bdir) / ep;
                    if (fs::exists(cand, ec)) { ep = cand; break; }
                }
            }
            if (fs::exists(ep, ec)) {
                fs::path can = fs::canonical(ep, ec);
                if (!ec && !can.empty()) rec.canonical_exec = can.string();
            }
        }

        by_id_[id] = rec.info;
        size_t dot = id.rfind('.');
        if (dot != std::string::npos && dot + 1 < id.size()) {
            std::string tail = id.substr(dot + 1);
            if (!by_id_.contains(tail)) by_id_[tail] = rec.info;
        }
        records_.push_back(std::move(rec));
        return true;
    }

    std::optional<DesktopAppInfo> matchEntry(const std::string& app_id,
                                             const std::string& win_class,
                                             const std::string& exe_path,
                                             const std::string& exe_name) {
        const std::string w_app = str_lower_trimmed(app_id);
        const std::string w_cls = str_lower_trimmed(win_class);
        const std::string w_exe = str_lower_trimmed(exe_path);
        const std::string w_nam = str_lower_trimmed(exe_name);

        std::vector<std::string> path_segments;
        for (const auto& seg : fs::path(w_exe)) {
            if (!seg.empty() && seg != "/") path_segments.push_back(seg.string());
        }
        const bool is_script = has_script_ext(w_nam);
        const std::string script_stem = is_script ? path_stem(w_nam) : std::string{};

        const EntryRecord* best = nullptr;
        int best_score = 0;
        int best_vis = -1;

        for (const auto& rec : records_) {
            if (rec.info.hidden) continue;

            const std::string id = str_lower_trimmed(rec.info.id);
            const std::string wm = str_lower_trimmed(rec.wm_class);
            const std::string prog = str_lower_trimmed(rec.exec_prog);
            const std::string prog_base = str_lower_trimmed(path_basename(rec.exec_prog));
            const std::string cprog = str_lower_trimmed(rec.canonical_exec);
            const std::string cprog_base = str_lower_trimmed(path_basename(rec.canonical_exec));
            const std::string cdir = rec.canonical_exec.empty()
                ? std::string{}
                : str_lower_trimmed(fs::path(rec.canonical_exec).parent_path().string());
            const std::string edir = rec.exec_prog.starts_with('/')
                ? str_lower_trimmed(fs::path(rec.exec_prog).parent_path().string())
                : std::string{};
            const std::string icn = str_lower_trimmed(path_basename(rec.info.icon));
            const std::string nm = str_lower_trimmed(rec.info.name);
            const std::string fstem = str_lower_trimmed(path_stem(rec.file_path));

            const bool allow_proc = !rec.terminal;
            const std::string t_exe = allow_proc ? w_exe : std::string{};
            const std::string t_dir = (allow_proc && t_exe.starts_with('/'))
                ? str_lower_trimmed(fs::path(t_exe).parent_path().string())
                : std::string{};
            const std::string t_nam = allow_proc ? w_nam : std::string{};
            const std::string t_scr = allow_proc ? script_stem : std::string{};

            int score = 0;
            auto rank = [&score](bool cond, int pts) {
                if (cond && pts > score) score = pts;
            };

            rank(!w_app.empty() && id == w_app, 100);
            rank(!w_cls.empty() && wm == w_cls, 95);
            rank(!w_app.empty() && wm == w_app, 95);
            rank(!w_cls.empty() && id == w_cls, 90);
            rank(!t_exe.empty() && (prog == t_exe || cprog == t_exe), 85);
            rank(!t_dir.empty() && t_dir != "/usr/bin" && t_dir != "/bin" &&
                 t_dir != "/usr/local/bin" && t_dir != "/usr/libexec" &&
                 (t_dir == cdir || t_dir == edir), 82);
            rank(!t_nam.empty() && (prog_base == t_nam || cprog_base == t_nam), 80);
            rank(!t_scr.empty() && (prog_base == t_scr || cprog_base == t_scr), 80);
            rank(!t_exe.empty() && (fstem == t_exe || icn == t_exe), 75);
            rank(!t_nam.empty() && (fstem == t_nam || icn == t_nam), 75);
            rank(has_word_prefix(wm, w_app) || has_word_suffix(wm, w_app), 70);
            rank(has_word_prefix(wm, w_cls) || has_word_suffix(wm, w_cls), 70);
            rank(has_word_prefix(id, w_cls) || has_word_suffix(id, w_cls), 60);
            rank(has_word_prefix(fstem, w_cls) || has_word_suffix(fstem, w_cls), 60);
            rank(has_word_prefix(prog_base, w_cls) || has_word_prefix(cprog_base, w_cls), 58);
            rank(has_word_prefix(prog, w_cls), 55);
            rank(has_word_prefix(icn, w_cls) || has_word_suffix(icn, w_cls), 55);
            rank(has_word_prefix(nm, w_cls) || has_word_suffix(nm, w_cls), 50);
            rank(allow_proc && !is_script &&
                 ((!prog_base.empty() && std::find(path_segments.begin(), path_segments.end(), prog_base) != path_segments.end()) ||
                  (!cprog_base.empty() && std::find(path_segments.begin(), path_segments.end(), cprog_base) != path_segments.end())),
                 45);

            if (score == 0 || score < best_score) continue;
            int vis = (rec.info.hidden || rec.info.noDisplay) ? 0 : 2;
            if (score > best_score || vis > best_vis) {
                best = &rec;
                best_score = score;
                best_vis = vis;
            }
        }

        if (best) return best->info;

        if (!exe_path.empty() && exe_path.front() == '/') {
            auto it = portable_cache_.find(exe_path);
            if (it != portable_cache_.end()) {
                return it->second.id.empty() ? std::nullopt : std::optional<DesktopAppInfo>(it->second);
            }
            if (auto bundle = probePortableBundle(exe_path, exe_name)) {
                portable_cache_.emplace(exe_path, *bundle);
                by_id_[bundle->id] = *bundle;
                return bundle;
            }
            portable_cache_.emplace(exe_path, DesktopAppInfo{});
        }
        return std::nullopt;
    }

    static std::optional<DesktopAppInfo> probePortableBundle(const std::string& exe_path,
                                                             const std::string& exe_name) {
        std::error_code ec;
        fs::path ep(exe_path);
        fs::path dir = ep.parent_path();
        if (dir.empty()) return std::nullopt;

        fs::path prod_json = dir / "resources" / "app" / "product.json";
        if (!fs::exists(prod_json, ec) || !fs::is_regular_file(prod_json, ec)) return std::nullopt;

        DesktopAppInfo app;
        app.id = exe_name.empty() ? ep.filename().string() : exe_name;
        app.name = dir.filename().string();

        std::string json = slurp_file(prod_json.string());
        auto read_json_key = [&json](std::string_view key) -> std::string {
            std::string needle = "\"" + std::string(key) + "\"";
            size_t p = json.find(needle);
            if (p == std::string::npos) return {};
            size_t col = json.find(':', p + needle.size());
            if (col == std::string::npos) return {};
            size_t q1 = json.find('"', col + 1);
            if (q1 == std::string::npos) return {};
            size_t q2 = json.find('"', q1 + 1);
            if (q2 == std::string::npos) return {};
            return json.substr(q1 + 1, q2 - q1 - 1);
        };

        std::string title = read_json_key("nameLong");
        if (title.empty()) title = read_json_key("nameShort");
        if (!title.empty()) app.name = title;

        const fs::path icon_candidates[] = {
            dir / "resources" / "app" / "resources" / "linux" / "code.png",
            dir / "resources" / "app" / "out" / "media" / "code-icon.svg",
            dir / "resources" / "app" / "out" / "media" / "antigravity-gradient-logo.svg",
        };
        for (const auto& ic : icon_candidates) {
            if (fs::exists(ic, ec) && fs::is_regular_file(ic, ec)) {
                app.icon = ic.string();
                break;
            }
        }
        if (app.icon.empty()) {
            std::string linux_icon = read_json_key("linuxIconName");
            app.icon = linux_icon.empty() ? app.id : linux_icon;
        }
        return app;
    }
};

// ============================================================================
// Kernel Parsers (/proc & /sys)
// ============================================================================

struct CpuTickPair {
    uint64_t total = 0;
    uint64_t idle = 0;
};

CpuTickPair parse_cpu_stat_line(std::string_view line) {
    auto cols = sv_split_ws(line);
    CpuTickPair out;
    if (cols.size() < 5 || !cols[0].starts_with("cpu")) return out;
    size_t lim = std::min<size_t>(cols.size(), 9);
    for (size_t i = 1; i < lim; ++i) out.total += sv_u64(cols[i]);
    out.idle = sv_u64(cols[4]) + (cols.size() > 5 ? sv_u64(cols[5]) : 0);
    return out;
}

double calc_cpu_pct(const CpuTickPair& cur, const CpuTickPair& prev) noexcept {
    uint64_t dt = u64_delta(cur.total, prev.total);
    uint64_t di = u64_delta(cur.idle, prev.idle);
    return (dt > 0) ? (100.0 * static_cast<double>(dt - std::min(di, dt)) / static_cast<double>(dt)) : 0.0;
}

MemoryMetrics parse_meminfo_blob(std::string_view text) {
    std::unordered_map<std::string, uint64_t> kv;
    for (std::string_view line : sv_split_char(text, '\n')) {
        auto cols = sv_split_ws(line);
        if (cols.size() >= 2 && cols[0].ends_with(':')) {
            kv.emplace(std::string(cols[0].substr(0, cols[0].size() - 1)), sv_u64(cols[1]) * 1024ULL);
        }
    }
    auto get_v = [&kv](const char* k, uint64_t def = 0) -> uint64_t {
        auto it = kv.find(k);
        return (it != kv.end()) ? it->second : def;
    };

    MemoryMetrics m;
    m.total = get_v("MemTotal");
    m.cache = get_v("Cached") + get_v("SReclaimable");
    uint64_t shmem = get_v("Shmem");
    uint64_t est_avail = get_v("MemFree") + get_v("Buffers") + m.cache - std::min(m.cache, shmem);
    m.available = std::min(m.total, get_v("MemAvailable", est_avail));
    m.used = m.total - m.available;
    m.percent = (m.total > 0) ? (100.0 * static_cast<double>(m.used) / static_cast<double>(m.total)) : 0.0;
    m.swapTotal = get_v("SwapTotal");
    m.swapUsed = m.swapTotal - std::min(m.swapTotal, get_v("SwapFree"));
    m.swapPercent = (m.swapTotal > 0) ? (100.0 * static_cast<double>(m.swapUsed) / static_cast<double>(m.swapTotal)) : 0.0;
    return m;
}

bool parse_proc_stat_blob(std::string_view content, ProcessRecord& out, int64_t page_bytes) {
    size_t lp = content.find('(');
    size_t rp = content.rfind(')');
    if (lp == std::string_view::npos || lp < 1 || rp == std::string_view::npos || rp <= lp) return false;

    auto cols = sv_split_ws(content.substr(rp + 1));
    if (cols.size() < 22) return false;

    if (!sv_to_int(content.substr(0, lp), out.pid) || out.pid <= 0) {
        out.pid = 0;
        return false;
    }

    out.name = sanitize_utf8(content.substr(lp + 1, rp - lp - 1));
    out.state = std::string(cols[0]);
    out.parentPid = sv_i32(cols[1]);

    uint64_t kflags = 0;
    out.kernelThread = sv_to_int(cols[6], kflags) && (kflags & 0x00200000ULL) != 0;
    out.cpuTicks = sv_u64(cols[11]) + sv_u64(cols[12]);
    out.nice = sv_i32(cols[16]);
    out.threads = sv_i32(cols[17]);

    if (!sv_to_int(cols[19], out.startTicks)) {
        out.startTicks = 0;
        return false;
    }

    int64_t rss_pages = std::max<int64_t>(0, sv_i64(cols[21]));
    out.rss = static_cast<uint64_t>(rss_pages) * static_cast<uint64_t>(page_bytes);
    out.memory = out.rss;
    return true;
}

bool parse_status_euid(std::string_view status_blob, uint32_t& out_uid) {
    for (std::string_view line : sv_split_char(status_blob, '\n')) {
        if (!line.starts_with("Uid:")) continue;
        auto cols = sv_split_ws(line);
        if (cols.size() < 3) return false;
        uint64_t v = 0;
        if (!sv_to_int(cols[2], v) || v > UINT32_MAX) return false;
        out_uid = static_cast<uint32_t>(v);
        return true;
    }
    return false;
}

bool parse_smaps_pss(std::string_view rollup_blob, uint64_t& out_pss) {
    for (std::string_view line : sv_split_char(rollup_blob, '\n')) {
        if (!line.starts_with("Pss:")) continue;
        auto cols = sv_split_ws(line);
        if (cols.size() < 2) return false;
        uint64_t kb = 0;
        if (!sv_to_int(cols[1], kb)) return false;
        out_pss = kb * 1024ULL;
        return true;
    }
    return false;
}

std::string normalize_cmdline_args(std::string_view cmdline, const std::string& exe) {
    std::string prefix = exe + ' ';
    size_t nul = cmdline.find('\0');
    bool single_arg = cmdline.empty() || (nul == cmdline.size() - 1);
    if (!exe.empty() && single_arg && cmdline.starts_with(prefix)) {
        std::string copy(cmdline);
        std::replace(copy.begin(), copy.end(), ' ', '\0');
        return copy;
    }
    return std::string(cmdline);
}

std::string resolve_target_program(std::string_view cmdline_args) {
    auto args = sv_split_char(cmdline_args, '\0');
    if (args.empty()) return {};
    std::string prog(args[0]);
    if (is_interpreter_bin(path_basename(prog))) {
        prog.clear();
        for (size_t i = 1; i < args.size(); ++i) {
            std::string_view a = args[i];
            if (a.empty() || a == "-c" || a == "-e" || a == "-m") break;
            if (a.starts_with('-')) continue;
            prog = std::string(a);
            break;
        }
    }
    return sanitize_utf8(prog);
}

std::string extract_chromium_helper(std::string_view cmdline_args) {
    auto args = sv_split_char(cmdline_args, '\0');
    if (args.size() < 2 || !args[1].starts_with("--type=")) return {};
    std::string_view t = args[1].substr(7);
    for (std::string_view a : args) {
        if (t == "renderer" && a == "--extension-process") return "extension";
        if (t == "utility" && a.starts_with("--utility-sub-type=")) return sanitize_utf8(a.substr(19));
    }
    return sanitize_utf8(t);
}

int decode_hex_nibble(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string extract_cgroup_unit(std::string_view cgroup_blob) {
    for (std::string_view line : sv_split_char(cgroup_blob, '\n')) {
        if (!line.starts_with("0::")) continue;
        std::string_view p = line.substr(3);
        size_t slash = p.rfind('/');
        return sanitize_utf8(slash == std::string_view::npos ? p : p.substr(slash + 1));
    }
    return {};
}

std::string extract_cgroup_app_id(std::string_view cgroup_blob) {
    std::string unit = extract_cgroup_unit(cgroup_blob);
    std::string_view u(unit);
    if (!u.starts_with("app-")) return {};

    std::string_view raw_id;
    if (u.ends_with(".scope")) {
        auto parts = sv_split_char(u.substr(0, u.size() - 6), '-');
        if (parts.size() < 3) return {};
        raw_id = parts[parts.size() - 2];
    } else if (u.ends_with(".service")) {
        auto parts = sv_split_char(u.substr(0, u.size() - 8), '-');
        if (parts.size() < 2) return {};
        raw_id = parts.back();
        size_t at = raw_id.find('@');
        if (at != std::string_view::npos) raw_id = raw_id.substr(0, at);
    } else {
        return {};
    }

    std::string decoded;
    for (size_t i = 0; i < raw_id.size(); ++i) {
        if (raw_id[i] == '\\' && i + 3 < raw_id.size() && raw_id[i + 1] == 'x') {
            int hi = decode_hex_nibble(raw_id[i + 2]);
            int lo = decode_hex_nibble(raw_id[i + 3]);
            if (hi >= 0 && lo >= 0) {
                decoded.push_back(static_cast<char>((hi << 4) | lo));
                i += 3;
                continue;
            }
        }
        decoded.push_back(raw_id[i]);
    }
    return sanitize_utf8(decoded);
}

std::string decode_mount_escapes(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    auto is_oct = [](char c) { return c >= '0' && c <= '7'; };
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\\' && i + 3 < raw.size() && is_oct(raw[i + 1]) && is_oct(raw[i + 2]) && is_oct(raw[i + 3])) {
            out.push_back(static_cast<char>((raw[i + 1] - '0') * 64 + (raw[i + 2] - '0') * 8 + (raw[i + 3] - '0')));
            i += 3;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

bool path_under_prefix(std::string_view prefix, std::string_view target) noexcept {
    return target == prefix || (target.starts_with(prefix) && target[prefix.size()] == '/');
}

uint64_t query_statx_mount_id(const std::string& mnt_point) noexcept {
    struct statx sx{};
    if (::statx(AT_FDCWD, mnt_point.c_str(), AT_NO_AUTOMOUNT | AT_STATX_DONT_SYNC, STATX_MNT_ID, &sx) != 0 ||
        !(sx.stx_mask & STATX_MNT_ID)) {
        return 0;
    }
    return sx.stx_mnt_id;
}

std::vector<Volume> query_mounted_volumes() {
    struct MountEntry {
        uint64_t id = 0;
        std::string mount_point;
        std::string fs_type;
        std::string dev_source;
    };

    std::vector<MountEntry> mounts;
    std::string blob = slurp_file("/proc/self/mountinfo");
    for (std::string_view line : sv_split_char(blob, '\n')) {
        auto cols = sv_split_char(line, ' ');
        size_t sep = 6;
        while (sep < cols.size() && cols[sep] != "-") ++sep;
        if (sep + 2 >= cols.size()) continue;

        MountEntry me;
        me.id = sv_u64(cols[0]);
        me.mount_point = decode_mount_escapes(cols[4]);
        me.fs_type = std::string(cols[sep + 1]);
        me.dev_source = decode_mount_escapes(cols[sep + 2]);

        bool skip = (me.fs_type == "rootfs");
        for (std::string_view sys_dir : {"/dev", "/proc", "/sys", "/var/run", "/var/lock"}) {
            if (path_under_prefix(sys_dir, me.mount_point)) { skip = true; break; }
        }
        if (!skip) mounts.push_back(std::move(me));
    }

    std::vector<Volume> vols;
    for (size_t i = 0; i < mounts.size(); ++i) {
        const auto& m = mounts[i];
        uint64_t live_id = query_statx_mount_id(m.mount_point);
        if (live_id == 0) {
            bool shadowed = false;
            for (size_t j = i + 1; j < mounts.size() && !shadowed; ++j) {
                shadowed = path_under_prefix(mounts[j].mount_point, m.mount_point);
            }
            if (shadowed) continue;
        } else if (live_id != m.id) {
            continue;
        }

        if (m.fs_type == "tmpfs" || m.fs_type == "devtmpfs" || m.fs_type == "squashfs" ||
            m.mount_point.starts_with("/proc") || m.mount_point.starts_with("/sys")) {
            continue;
        }

        struct statvfs vfs{};
        if (::statvfs(m.mount_point.c_str(), &vfs) != 0) continue;
        uint64_t total_bytes = static_cast<uint64_t>(vfs.f_blocks) * vfs.f_frsize;
        if (total_bytes == 0) continue;

        Volume v;
        v.mount = sanitize_utf8(m.mount_point);
        v.device = sanitize_utf8(m.dev_source);
        v.type = sanitize_utf8(m.fs_type);
        v.total = total_bytes;
        v.available = static_cast<uint64_t>(vfs.f_bavail) * vfs.f_frsize;
        v.used = total_bytes - static_cast<uint64_t>(vfs.f_bfree) * vfs.f_frsize;
        vols.push_back(std::move(v));
    }
    return vols;
}

} // namespace

// ============================================================================
// TelemetryCollector
// ============================================================================

class TelemetryCollector {
public:
    TelemetryCollector() {
        uid_ = ::getuid();
        long ps = ::sysconf(_SC_PAGESIZE);
        if (ps > 0) page_size_ = ps;

        std::string cpuinfo = slurp_file("/proc/cpuinfo");
        for (std::string_view line : sv_split_char(cpuinfo, '\n')) {
            if (line.starts_with("model name") || line.starts_with("Hardware")) {
                size_t col = line.find(':');
                cpu_brand_ = sanitize_utf8(sv_trim(col == std::string_view::npos ? line : line.substr(col + 1)));
                break;
            }
        }
        if (cpu_brand_.empty()) {
            utsname u{};
            if (::uname(&u) == 0) {
                cpu_brand_ = (std::string_view(u.machine) == "aarch64") ? "arm64" : u.machine;
            }
        }
    }

    TelemetryFrame collect() {
        TelemetryFrame frame;
        std::string stat_blob = slurp_file("/proc/stat");
        std::string mem_blob = slurp_file("/proc/meminfo");
        if (!stat_blob.starts_with("cpu ") || mem_blob.empty()) {
            frame.err_msg = "System information is unavailable.";
            return frame;
        }

        int64_t now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - start_clock_)
                             .count();
        double dt_sec = has_prev_ ? static_cast<double>(now_ns - prev_ns_) / 1e9 : 0.0;

        Metrics& m = frame.metrics;
        m.memory = parse_meminfo_blob(mem_blob);
        m.cpuName = cpu_brand_;

        std::unordered_map<std::string, CpuTickPair> cur_cpus;
        for (std::string_view line : sv_split_char(stat_blob, '\n')) {
            if (!line.starts_with("cpu")) continue;
            auto cols = sv_split_ws(line);
            if (cols.empty()) continue;
            std::string label(cols.front());
            CpuTickPair ticks = parse_cpu_stat_line(line);
            cur_cpus[label] = ticks;

            auto pit = prev_cpus_.find(label);
            double pct = (pit != prev_cpus_.end()) ? calc_cpu_pct(ticks, pit->second) : 0.0;
            if (label == "cpu") {
                m.cpuPercent = pct;
            } else {
                m.cores.push_back(Processor{sv_i32(std::string_view(label).substr(3)), pct});
            }
        }

        uint64_t cur_total_ticks = cur_cpus.contains("cpu") ? cur_cpus["cpu"].total : 0;
        uint64_t prev_total_ticks = prev_cpus_.contains("cpu") ? prev_cpus_["cpu"].total : 0;
        uint64_t sys_tick_delta = u64_delta(cur_total_ticks, prev_total_ticks);

        std::unordered_map<int, ProcessRecord> next_proc_map;
        for (const std::string& dname : list_dir_names("/proc")) {
            int pid = 0;
            if (!sv_to_int(dname, pid) || pid <= 0) continue;

            std::string pdir = "/proc/" + dname;
            ProcessRecord rec;
            if (!parse_proc_stat_blob(slurp_file(pdir + "/stat"), rec, page_size_)) continue;

            struct stat st{};
            rec.uid = (::stat(pdir.c_str(), &st) == 0) ? st.st_uid : static_cast<uint32_t>(-2);
            if (rec.uid == 0 && !rec.kernelThread && !parse_status_euid(slurp_file(pdir + "/status"), rec.uid)) {
                continue;
            }
            rec.user = lookupUserName(rec.uid);

            std::string io_blob = slurp_file(pdir + "/io");
            bool got_r = false;
            bool got_w = false;
            for (std::string_view line : sv_split_char(io_blob, '\n')) {
                auto cols = sv_split_ws(line);
                if (cols.size() < 2) continue;
                if (cols[0] == "read_bytes:") { rec.readBytes = sv_u64(cols[1]); got_r = true; }
                else if (cols[0] == "write_bytes:") { rec.writeBytes = sv_u64(cols[1]); got_w = true; }
            }
            rec.ioAvailable = got_r && got_w;

            auto pit = prev_procs_.find(rec.pid);
            const ProcessRecord* prev = (pit != prev_procs_.end()) ? &pit->second : nullptr;
            bool same_instance = (prev && prev->pid == rec.pid && prev->startTicks == rec.startTicks);

            if (same_instance && prev->name == rec.name) {
                rec.executable = prev->executable;
                rec.program = prev->program;
                rec.command = prev->command;
                rec.helperType = prev->helperType;
                rec.displayName = prev->displayName;
                rec.iconName = prev->iconName;
                rec.scopeId = prev->scopeId;
                rec.pss = prev->pss;
                rec.pssTime = prev->pssTime;
                rec.pssAvailable = prev->pssAvailable;
                rec.scopeSettled = true;
                if (!prev->scopeSettled && rec.uid == uid_ && !rec.kernelThread) {
                    rec.scopeId = extract_cgroup_app_id(slurp_file(pdir + "/cgroup"));
                }
            } else {
                populateProcessIdentity(pdir, rec);
                if (rec.uid == uid_ && !rec.kernelThread) {
                    rec.scopeId = extract_cgroup_app_id(slurp_file(pdir + "/cgroup"));
                }
                ProcessRecord check;
                if (!parse_proc_stat_blob(slurp_file(pdir + "/stat"), check, page_size_) ||
                    check.startTicks != rec.startTicks) {
                    continue;
                }
            }

            if (same_instance) {
                if (sys_tick_delta > 0) {
                    rec.cpu = std::min(100.0, 100.0 * static_cast<double>(u64_delta(rec.cpuTicks, prev->cpuTicks)) /
                                                  static_cast<double>(sys_tick_delta));
                }
                if (prev->ioAvailable && rec.ioAvailable) {
                    rec.readRate = calc_rate(rec.readBytes, prev->readBytes, dt_sec);
                    rec.writeRate = calc_rate(rec.writeBytes, prev->writeBytes, dt_sec);
                }
            }

            next_proc_map[rec.pid] = rec;
            frame.processes.push_back(std::move(rec));
        }

        std::sort(frame.processes.begin(), frame.processes.end(),
                  [](const ProcessRecord& a, const ProcessRecord& b) { return a.pid < b.pid; });
        m.processCount = static_cast<int>(frame.processes.size());

        for (ProcessRecord& rec : frame.processes) {
            if (rec.uid != uid_ || rec.kernelThread) {
                rec.pssAvailable = false;
                continue;
            }
            if (!rec.pssAvailable || (now_ns - rec.pssTime) >= 5'000'000'000LL) {
                rec.pssAvailable = parse_smaps_pss(slurp_file("/proc/" + std::to_string(rec.pid) + "/smaps_rollup"), rec.pss);
                rec.pssTime = now_ns;
                auto& saved = next_proc_map[rec.pid];
                saved.pss = rec.pss;
                saved.pssTime = rec.pssTime;
                saved.pssAvailable = rec.pssAvailable;
            }
            if (rec.pssAvailable) rec.memory = rec.pss;
        }

        sampleNetworks(m, dt_sec);
        sampleDisks(m, dt_sec);
        sampleGpus(m, frame.processes, dt_sec);

        prev_cpus_ = std::move(cur_cpus);
        prev_procs_ = std::move(next_proc_map);
        prev_ns_ = now_ns;
        frame.is_warmup = !has_prev_;
        has_prev_ = true;
        frame.ok = true;
        return frame;
    }

private:
    struct DrmEngineStat {
        uint64_t busy_ns = 0;
        uint64_t cycles = 0;
        uint64_t total_cycles = 0;
        int capacity = 1;
        bool has_time = false;
        bool has_cycles = false;
    };

    struct DrmClientStat {
        std::string id;
        std::map<std::string, DrmEngineStat> engines;
    };

    using DrmClientMap = std::unordered_map<std::string, DrmClientStat>;

    struct GpuDeviceSlot {
        std::string dev_dir;
        std::string hwmon_dir;
        std::string name;
        std::string driver;
        std::vector<std::string> drm_nodes;
        DrmClientMap prev_clients;
        bool primary = false;
        bool has_busy_sysfs = false;
        bool has_client_busy = false;
        bool has_temp = false;
        bool has_freq = false;
        bool has_vram = false;
    };

    struct CachedFdNodes {
        uint64_t start_ticks = 0;
        std::vector<std::pair<std::string, int>> fd_to_card;
    };

    uint32_t uid_{0};
    int64_t page_size_{4096};
    std::string cpu_brand_;
    std::chrono::steady_clock::time_point start_clock_{std::chrono::steady_clock::now()};
    int64_t prev_ns_{0};
    bool has_prev_{false};

    std::unordered_map<std::string, CpuTickPair> prev_cpus_;
    std::unordered_map<int, ProcessRecord> prev_procs_;
    std::unordered_map<uint32_t, std::string> uid_cache_;
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> prev_net_io_;
    std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> prev_disk_io_;

    bool gpu_probed_{false};
    uint64_t gpu_tick_{0};
    std::vector<GpuDeviceSlot> gpu_slots_;
    std::unordered_map<std::string, int> drm_node_owner_;
    std::unordered_map<int, CachedFdNodes> proc_drm_fds_;

    void populateProcessIdentity(const std::string& pdir, ProcessRecord& rec) const {
        rec.executable = sanitize_utf8(read_symlink_resolved(pdir + "/exe"));
        std::string clean_exe = rec.executable;
        if (clean_exe.ends_with(" (deleted)") && !path_exists(clean_exe)) {
            clean_exe.resize(clean_exe.size() - 10);
        }

        std::string raw_cmd = slurp_file(pdir + "/cmdline");
        std::string norm_args = normalize_cmdline_args(raw_cmd, clean_exe);
        rec.program = raw_cmd.empty() ? clean_exe : resolve_target_program(norm_args);
        rec.helperType = extract_chromium_helper(norm_args);

        std::string flat_cmd = raw_cmd;
        std::replace(flat_cmd.begin(), flat_cmd.end(), '\0', ' ');
        rec.command = sanitize_utf8(sv_trim(flat_cmd));

        std::string exe_base = path_basename(clean_exe);
        rec.displayName = exe_base.empty() ? rec.name : exe_base;
        rec.iconName = "application-x-executable";
    }

    std::string lookupUserName(uint32_t uid) {
        auto it = uid_cache_.find(uid);
        if (it != uid_cache_.end()) return it->second;

        std::vector<char> buf(16384);
        passwd pw{};
        passwd* res = nullptr;
        int rc = ::getpwuid_r(uid, &pw, buf.data(), buf.size(), &res);
        std::string name = (rc == 0 && res) ? sanitize_utf8(pw.pw_name) : std::to_string(uid);
        uid_cache_.emplace(uid, name);
        return name;
    }

    void sampleNetworks(Metrics& m, double dt_sec) {
        std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> cur_net;
        std::vector<NetworkInterface> list;
        double sum_dl = 0.0;
        double sum_ul = 0.0;
        uint64_t sum_rx = 0;
        uint64_t sum_tx = 0;

        std::string blob = slurp_file("/proc/net/dev");
        for (std::string_view line : sv_split_char(blob, '\n')) {
            size_t col = line.find(':');
            if (col == std::string_view::npos) continue;
            std::string ifname = sanitize_utf8(sv_trim(line.substr(0, col)));
            auto vals = sv_split_ws(line.substr(col + 1));
            if (ifname == "lo" || vals.size() < 16) continue;

            uint64_t rx = sv_u64(vals[0]);
            uint64_t tx = sv_u64(vals[8]);
            cur_net[ifname] = {rx, tx};

            auto pit = prev_net_io_.find(ifname);
            double rx_rate = (pit != prev_net_io_.end()) ? calc_rate(rx, pit->second.first, dt_sec) : 0.0;
            double tx_rate = (pit != prev_net_io_.end()) ? calc_rate(tx, pit->second.second, dt_sec) : 0.0;

            std::string sys_dir = "/sys/class/net/" + ifname;
            bool is_phys = path_exists(sys_dir + "/device");
            if (is_phys) {
                sum_dl += rx_rate;
                sum_ul += tx_rate;
                sum_rx += rx;
                sum_tx += tx;
            }

            NetworkInterface nif;
            nif.name = ifname;
            nif.state = sanitize_utf8(sv_trim(slurp_file(sys_dir + "/operstate")));
            nif.address = sanitize_utf8(sv_trim(slurp_file(sys_dir + "/address")));
            nif.download = rx_rate;
            nif.upload = tx_rate;
            nif.received = rx;
            nif.sent = tx;
            nif.physical = is_phys;
            list.push_back(std::move(nif));
        }

        m.download = sum_dl;
        m.upload = sum_ul;
        m.received = sum_rx;
        m.sent = sum_tx;
        m.interfaces = std::move(list);
        prev_net_io_ = std::move(cur_net);
    }

    void sampleDisks(Metrics& m, double dt_sec) {
        std::unordered_map<std::string, std::pair<uint64_t, uint64_t>> cur_disks;
        std::vector<Disk> list;
        double sum_r = 0.0;
        double sum_w = 0.0;

        std::string blob = slurp_file("/proc/diskstats");
        for (std::string_view line : sv_split_char(blob, '\n')) {
            auto vals = sv_split_ws(line);
            if (vals.size() < 14) continue;
            std::string dname = sanitize_utf8(vals[2]);
            std::string sys_dir = "/sys/block/" + dname;
            if (!path_exists(sys_dir + "/device")) continue;

            uint64_t r_bytes = sv_u64(vals[5]) * 512ULL;
            uint64_t w_bytes = sv_u64(vals[9]) * 512ULL;
            cur_disks[dname] = {r_bytes, w_bytes};

            auto pit = prev_disk_io_.find(dname);
            double r_rate = (pit != prev_disk_io_.end()) ? calc_rate(r_bytes, pit->second.first, dt_sec) : 0.0;
            double w_rate = (pit != prev_disk_io_.end()) ? calc_rate(w_bytes, pit->second.second, dt_sec) : 0.0;

            Disk d;
            d.name = dname;
            d.model = sanitize_utf8(sv_trim(slurp_file(sys_dir + "/device/model")));
            d.read = r_rate;
            d.write = w_rate;
            d.size = sv_u64(sv_trim(slurp_file(sys_dir + "/size"))) * 512ULL;
            sum_r += d.read;
            sum_w += d.write;
            list.push_back(std::move(d));
        }

        m.diskRead = sum_r;
        m.diskWrite = sum_w;
        m.disks = std::move(list);
        prev_disk_io_ = std::move(cur_disks);
    }

    static std::vector<std::string> sorted_subdirs(const std::string& parent) {
        std::vector<std::string> out;
        for (std::string& entry : list_dir_names(parent)) {
            if (path_is_dir(parent + "/" + entry)) out.push_back(std::move(entry));
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    static std::string lookup_pci_id_name(std::string_view ven, std::string_view dev) {
        if (ven.starts_with("0x")) ven.remove_prefix(2);
        if (dev.starts_with("0x")) dev.remove_prefix(2);
        std::string v_id = str_lower_trimmed(ven);
        std::string d_id = str_lower_trimmed(dev);
        if (v_id.size() != 4 || d_id.size() != 4) return {};

        for (const char* db_path : {"/usr/share/misc/pci.ids", "/usr/share/hwdata/pci.ids"}) {
            std::string blob = slurp_file(db_path);
            if (blob.empty()) continue;
            bool inside_vendor = false;
            for (std::string_view line : sv_split_char(blob, '\n')) {
                if (line.empty() || line.front() == '#') continue;
                if (line.front() != '\t') {
                    if (inside_vendor) return {};
                    inside_vendor = line.starts_with(v_id + "  ");
                } else if (inside_vendor && line.starts_with("\t" + d_id + "  ")) {
                    return sanitize_utf8(sv_trim(line.substr(7)));
                }
            }
            return {};
        }
        return {};
    }

    void probeGpuDevices() {
        gpu_probed_ = true;
        const std::string drm_root = "/sys/class/drm";
        for (const std::string& entry : sorted_subdirs(drm_root)) {
            if (entry.size() <= 4 || !entry.starts_with("card") ||
                !std::all_of(entry.begin() + 4, entry.end(), [](char c) { return c >= '0' && c <= '9'; })) {
                continue;
            }

            GpuDeviceSlot slot;
            slot.dev_dir = drm_root + "/" + entry + "/device";
            auto hwmons = sorted_subdirs(slot.dev_dir + "/hwmon");
            if (!hwmons.empty()) slot.hwmon_dir = slot.dev_dir + "/hwmon/" + hwmons.front();

            slot.has_busy_sysfs = path_exists(slot.dev_dir + "/gpu_busy_percent");
            slot.has_temp = !slot.hwmon_dir.empty() && path_exists(slot.hwmon_dir + "/temp1_input");
            slot.has_freq = !slot.hwmon_dir.empty() && path_exists(slot.hwmon_dir + "/freq1_input");
            slot.has_vram = path_exists(slot.dev_dir + "/mem_info_vram_total");

            for (const std::string& nd : sorted_subdirs(slot.dev_dir + "/drm")) {
                if (nd.starts_with("card") || nd.starts_with("renderD")) slot.drm_nodes.push_back(nd);
            }
            if (!slot.has_busy_sysfs && !slot.has_temp && slot.drm_nodes.empty()) continue;

            slot.primary = (sv_trim(slurp_file(slot.dev_dir + "/boot_vga")) == "1");
            slot.driver = sanitize_utf8(path_basename(read_symlink_resolved(slot.dev_dir + "/driver")));
            slot.name = sanitize_utf8(sv_trim(slurp_file(slot.dev_dir + "/product_name")));
            if (slot.name.empty()) {
                slot.name = lookup_pci_id_name(sv_trim(slurp_file(slot.dev_dir + "/vendor")),
                                               sv_trim(slurp_file(slot.dev_dir + "/device")));
            }
            if (slot.name.empty()) slot.name = slot.driver.empty() ? entry : slot.driver;
            gpu_slots_.push_back(std::move(slot));
        }

        std::stable_sort(gpu_slots_.begin(), gpu_slots_.end(),
                         [](const GpuDeviceSlot& a, const GpuDeviceSlot& b) { return a.primary && !b.primary; });
        for (size_t idx = 0; idx < gpu_slots_.size(); ++idx) {
            if (gpu_slots_[idx].has_busy_sysfs) continue;
            for (const std::string& nd : gpu_slots_[idx].drm_nodes) {
                drm_node_owner_.emplace("/dev/dri/" + nd, static_cast<int>(idx));
            }
        }
    }

    static DrmClientStat parseDrmFdinfoBlob(std::string_view blob) {
        DrmClientStat cl;
        std::map<std::string, int> caps;
        for (std::string_view line : sv_split_char(blob, '\n')) {
            size_t col = line.find(':');
            if (col == std::string_view::npos) continue;
            std::string_view k = line.substr(0, col);
            auto vals = sv_split_ws(line.substr(col + 1));
            std::string_view v = vals.empty() ? std::string_view{} : vals.front();
            if (k == "drm-client-id") {
                cl.id = std::string(v);
                continue;
            }
            uint64_t num = 0;
            if (!sv_to_int(v, num)) continue;
            if (k.starts_with("drm-engine-capacity-")) {
                caps[std::string(k.substr(20))] = std::max(1, static_cast<int>(num));
            } else if (k.starts_with("drm-engine-")) {
                auto& eng = cl.engines[std::string(k.substr(11))];
                eng.busy_ns = num;
                eng.has_time = true;
            } else if (k.starts_with("drm-total-cycles-")) {
                cl.engines[std::string(k.substr(17))].total_cycles = num;
            } else if (k.starts_with("drm-cycles-")) {
                auto& eng = cl.engines[std::string(k.substr(11))];
                eng.cycles = num;
                eng.has_cycles = true;
            }
        }
        for (auto& [ename, eng] : cl.engines) {
            auto it = caps.find(ename);
            eng.capacity = (it != caps.end()) ? it->second : 1;
        }
        return cl;
    }

    static double computeDrmLoadPct(const DrmClientMap& cur, const DrmClientMap& prev, double dt_ns) {
        std::map<std::string, double> loads;
        std::map<std::string, int> caps;
        for (const auto& [cid, client] : cur) {
            auto pit = prev.find(cid);
            if (pit == prev.end()) continue;
            for (const auto& [ename, eng] : client.engines) {
                auto eit = pit->second.engines.find(ename);
                DrmEngineStat old_eng = (eit != pit->second.engines.end()) ? eit->second : DrmEngineStat{};
                double l = 0.0;
                if (eng.has_cycles && old_eng.has_cycles && eng.total_cycles > old_eng.total_cycles) {
                    l = static_cast<double>(u64_delta(eng.cycles, old_eng.cycles)) /
                        static_cast<double>(eng.total_cycles - old_eng.total_cycles);
                } else if (eng.has_time && old_eng.has_time && dt_ns > 0.0) {
                    l = static_cast<double>(u64_delta(eng.busy_ns, old_eng.busy_ns)) / dt_ns;
                }
                loads[ename] += l;
                caps[ename] = std::max(caps.contains(ename) ? caps[ename] : 1, eng.capacity);
            }
        }
        double peak = 0.0;
        for (const auto& [ename, l] : loads) {
            peak = std::max(peak, l / static_cast<double>(caps[ename]));
        }
        return std::clamp(100.0 * peak, 0.0, 100.0);
    }

    std::vector<std::pair<std::string, int>> scanProcessDrmFds(const std::string& pdir) const {
        std::vector<std::pair<std::string, int>> out;
        DIR* d = ::opendir((pdir + "/fd").c_str());
        if (!d) return out;
        while (const dirent* e = ::readdir(d)) {
            char target[256];
            ssize_t len = ::readlinkat(::dirfd(d), e->d_name, target, sizeof(target));
            if (len <= 0 || static_cast<size_t>(len) == sizeof(target)) continue;
            auto it = drm_node_owner_.find(std::string(target, static_cast<size_t>(len)));
            if (it != drm_node_owner_.end()) out.emplace_back(e->d_name, it->second);
        }
        ::closedir(d);
        return out;
    }

    void sampleGpus(Metrics& m, const std::vector<ProcessRecord>& procs, double dt_sec) {
        if (!gpu_probed_) probeGpuDevices();

        std::vector<bool> card_active(gpu_slots_.size(), false);
        std::vector<bool> need_fdinfo(gpu_slots_.size(), false);
        for (size_t i = 0; i < gpu_slots_.size(); ++i) {
            card_active[i] = (sv_trim(slurp_file(gpu_slots_[i].dev_dir + "/power/runtime_status")) != "suspended");
            need_fdinfo[i] = card_active[i] && !gpu_slots_[i].has_busy_sysfs && !gpu_slots_[i].drm_nodes.empty();
        }

        std::vector<DrmClientMap> card_clients(gpu_slots_.size());
        if (std::find(need_fdinfo.begin(), need_fdinfo.end(), true) != need_fdinfo.end()) {
            ++gpu_tick_;
            std::unordered_map<int, CachedFdNodes> next_fds;
            for (const ProcessRecord& p : procs) {
                if (p.uid != uid_ || p.kernelThread) continue;
                std::string pdir = "/proc/" + std::to_string(p.pid);
                auto fit = proc_drm_fds_.find(p.pid);
                CachedFdNodes cached = (fit != proc_drm_fds_.end()) ? fit->second : CachedFdNodes{};
                if (cached.start_ticks != p.startTicks || (gpu_tick_ + static_cast<uint64_t>(p.pid)) % 5 == 0) {
                    cached = {p.startTicks, scanProcessDrmFds(pdir)};
                }
                for (const auto& [fd_str, card_idx] : cached.fd_to_card) {
                    if (!need_fdinfo[static_cast<size_t>(card_idx)]) continue;
                    DrmClientStat cl = parseDrmFdinfoBlob(slurp_file(pdir + "/fdinfo/" + fd_str));
                    if (cl.engines.empty()) continue;
                    std::string key = cl.id.empty() ? (std::to_string(p.pid) + ":" + fd_str) : cl.id;
                    card_clients[static_cast<size_t>(card_idx)][key] = std::move(cl);
                }
                next_fds[p.pid] = std::move(cached);
            }
            proc_drm_fds_ = std::move(next_fds);
        }

        std::vector<Gpu> out_gpus;
        for (size_t i = 0; i < gpu_slots_.size(); ++i) {
            auto& slot = gpu_slots_[i];
            int busy_pct = 0;
            if (need_fdinfo[i]) {
                if (!card_clients[i].empty()) slot.has_client_busy = true;
                busy_pct = static_cast<int>(std::lround(computeDrmLoadPct(card_clients[i], slot.prev_clients, dt_sec * 1e9)));
                slot.prev_clients = std::move(card_clients[i]);
            }
            if (!slot.has_busy_sysfs && !slot.has_client_busy && !slot.has_temp) continue;

            Gpu g;
            g.name = slot.name;
            g.driver = slot.driver;
            g.active = card_active[i];
            g.hasBusy = slot.has_busy_sysfs || slot.has_client_busy;
            g.hasTemperature = slot.has_temp;
            g.hasFrequency = slot.has_freq;
            g.hasVram = slot.has_vram;

            if (card_active[i] && slot.has_busy_sysfs) {
                int64_t v = sv_i64(slurp_file(slot.dev_dir + "/gpu_busy_percent"));
                busy_pct = static_cast<int>(std::clamp<int64_t>(v, 0, 100));
            }
            g.busy = busy_pct;

            if (card_active[i] && slot.has_temp) {
                int64_t milli_c = 0;
                if (sv_to_int(slurp_file(slot.hwmon_dir + "/temp1_input"), milli_c)) {
                    g.temperature = static_cast<double>(milli_c) / 1000.0;
                }
            }
            if (card_active[i] && slot.has_freq) {
                g.frequency = static_cast<double>(sv_u64(slurp_file(slot.hwmon_dir + "/freq1_input"))) / 1e6;
            }
            if (card_active[i] && slot.has_vram) {
                g.vramUsed = sv_u64(slurp_file(slot.dev_dir + "/mem_info_vram_used"));
                g.vramTotal = sv_u64(slurp_file(slot.dev_dir + "/mem_info_vram_total"));
            }
            out_gpus.push_back(std::move(g));
        }

        const Gpu* primary_gpu = out_gpus.empty() ? nullptr : &out_gpus.front();
        for (const Gpu& g : out_gpus) {
            if (g.hasBusy) {
                if (!primary_gpu->hasBusy) primary_gpu = &g;
                break;
            }
        }

        m.gpuAvailable = !out_gpus.empty();
        m.gpuName = primary_gpu ? primary_gpu->name : std::string{};
        m.gpuHasBusy = primary_gpu && primary_gpu->hasBusy;
        m.gpuHasTemperature = primary_gpu && primary_gpu->hasTemperature;
        m.gpuPercent = primary_gpu ? static_cast<double>(primary_gpu->busy) : 0.0;
        m.gpuTemperature = primary_gpu ? std::max(0.0, primary_gpu->temperature) : 0.0;
        m.gpus = std::move(out_gpus);
    }
};

// ============================================================================
// Preferences Implementation
// ============================================================================

Preferences Preferences::load(const std::string& path) {
    Preferences pref;
    std::ifstream in(path);
    if (!in.is_open()) return pref;

    std::string line;
    std::string sec;
    while (std::getline(in, line)) {
        std::string_view sv = sv_trim(line);
        if (sv.empty() || sv.front() == '#' || sv.front() == ';') continue;
        if (sv.front() == '[' && sv.back() == ']') {
            sec = std::string(sv_trim(sv.substr(1, sv.size() - 2)));
            continue;
        }
        size_t eq = sv.find('=');
        if (eq == std::string_view::npos) continue;
        std::string k(sv_trim(sv.substr(0, eq)));
        std::string v(sv_trim(sv.substr(eq + 1)));
        if (!sec.empty() && sec != "General") k = sec + "/" + k;

        if (k == "UpdateInterval") {
            int64_t iv = sv_i64(v, 1000);
            pref.interval = (iv == 2000 || iv == 5000) ? static_cast<int>(iv) : 1000;
        } else if (k == "Processes/Scope") {
            pref.scope = std::clamp(sv_i32(v, 1), 0, 2);
        } else if (k == "Processes/SortColumn" && !v.empty()) {
            pref.sortColumn = v;
        } else if (k == "Processes/SortAscending") {
            pref.sortAscending = (v == "true" || v == "1" || v == "yes");
        }
    }
    return pref;
}

void Preferences::save(const std::string& path) const {
    if (path.empty()) return;
    std::error_code ec;
    auto parent = fs::path(path).parent_path();
    if (!parent.empty()) fs::create_directories(parent, ec);

    std::ofstream out(path, std::ios::trunc);
    if (!out.is_open()) return;
    out << "UpdateInterval=" << interval << "\n\n"
        << "[Processes]\n"
        << "Scope=" << scope << "\n"
        << "SortColumn=" << sortColumn << "\n"
        << "SortAscending=" << (sortAscending ? "true" : "false") << "\n";
}

std::string Preferences::defaultPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && xdg[0] != '\0') {
        return std::string(xdg) + "/obsidian/system-monitor.conf";
    }
    if (const char* home = std::getenv("HOME"); home && home[0] != '\0') {
        return std::string(home) + "/.config/obsidian/system-monitor.conf";
    }
    return "/tmp/obsidian-system-monitor.conf";
}

// ============================================================================
// ProcessList Implementation
// ============================================================================

ProcessList::ProcessList() : current_uid_(::getuid()) {}

void ProcessList::apply(std::vector<ProcessRecord> processes) {
    processes_ = std::move(processes);
    rebuildView();
}

const ProcessRecord* ProcessList::find(int pid) const {
    auto it = std::lower_bound(processes_.begin(), processes_.end(), pid,
                               [](const ProcessRecord& r, int p) { return r.pid < p; });
    return (it != processes_.end() && it->pid == pid) ? &(*it) : nullptr;
}

void ProcessList::setSearch(std::string query) {
    if (search_ == query) return;
    search_ = std::move(query);
    rebuildView();
}

void ProcessList::setAllUsers(bool all_users) {
    if (all_users_ == all_users) return;
    all_users_ = all_users;
    rebuildView();
}

void ProcessList::sortBy(const std::string& column) {
    bool asc = (sort_col_ == column) ? !ascending_ : (column == "name" || column == "pid");
    setSorting(column, asc);
}

void ProcessList::setSorting(const std::string& column, bool ascending) {
    if (column != "name" && column != "cpu" && column != "memory" &&
        column != "read" && column != "write" && column != "pid") {
        return;
    }
    sort_col_ = column;
    ascending_ = ascending;
    rebuildView();
    sortingChanged.emit();
}

const ProcessRecord* ProcessList::at(size_t row) const {
    return (row < visible_indices_.size()) ? &processes_[visible_indices_[row]] : nullptr;
}

int ProcessList::indexOf(int pid) const {
    for (size_t i = 0; i < visible_indices_.size(); ++i) {
        if (processes_[visible_indices_[i]].pid == pid) return static_cast<int>(i);
    }
    return -1;
}

std::string ProcessList::programName(const ProcessRecord& proc) {
    std::string exe = proc.executable;
    if (exe.ends_with(" (deleted)")) exe.resize(exe.size() - 10);
    std::string label = proc.name;
    for (const std::string& cand_path : {proc.program, exe}) {
        std::string b = path_basename(cand_path);
        if (!b.empty() && !b.starts_with('-')) {
            label = b;
            break;
        }
    }
    if (proc.helperType.empty()) return label;
    return label + " (" + Monitor::describeHelperRole(proc.helperType) + ")";
}

bool ProcessList::matchesFilter(const ProcessRecord& proc) const {
    if (!all_users_ && proc.uid != current_uid_) return false;
    std::string_view q = sv_trim(search_);
    if (q.empty()) return true;

    auto hit = [q](const ProcessRecord& r) {
        return icontains(r.name, q) ||
               icontains(r.displayName, q) ||
               icontains(r.command, q) ||
               icontains(r.user, q) ||
               std::to_string(r.pid).find(q) != std::string::npos;
    };
    if (hit(proc)) return true;
    return std::any_of(proc.members.begin(), proc.members.end(), hit);
}

bool ProcessList::compareRows(const ProcessRecord& a, const ProcessRecord& b) const {
    if (sort_col_ == "name") {
        if (a.displayName == b.displayName) return a.pid < b.pid;
        return icompare(a.displayName, b.displayName) < 0;
    }
    if (sort_col_ == "pid") return a.pid < b.pid;

    auto metric_of = [this](const ProcessRecord& r) -> double {
        if (sort_col_ == "cpu") return r.cpu;
        if (sort_col_ == "memory") return static_cast<double>(r.memory);
        if (sort_col_ == "read") return r.readRate;
        return r.writeRate;
    };
    double va = metric_of(a);
    double vb = metric_of(b);
    return (va == vb) ? (a.pid < b.pid) : (va < vb);
}

void ProcessList::rebuildView() {
    visible_indices_.clear();
    for (size_t i = 0; i < processes_.size(); ++i) {
        if (matchesFilter(processes_[i])) visible_indices_.push_back(i);
    }
    std::stable_sort(visible_indices_.begin(), visible_indices_.end(), [this](size_t lhs, size_t rhs) {
        return ascending_ ? compareRows(processes_[lhs], processes_[rhs])
                          : compareRows(processes_[rhs], processes_[lhs]);
    });
    changed.emit();
}

// ============================================================================
// Monitor Implementation
// ============================================================================

Monitor::Monitor(std::string config_path)
    : config_path_(std::move(config_path))
    , collector_(std::make_shared<TelemetryCollector>()) {
    Preferences pref = config_path_.empty() ? Preferences{} : Preferences::load(config_path_);
    interval_ms_ = (pref.interval == 2000 || pref.interval == 5000) ? pref.interval : 1000;
    scope_ = pref.scope;
    processes_.setAllUsers(scope_ == 2);
    processes_.setSorting(pref.sortColumn, pref.sortAscending);
    applications_.setSorting(pref.sortColumn, pref.sortAscending);

    next_due_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms_);
    dispatchSample();
}

Monitor::~Monitor() {
    if (frame_in_flight_ && frame_future_.valid()) frame_future_.wait();
    if (vol_in_flight_ && vol_future_.valid()) vol_future_.wait();
}

void Monitor::tick() {
    const auto now = std::chrono::steady_clock::now();

    if (vol_in_flight_ && vol_future_.valid()) {
        if (vol_future_.wait_for(0ms) == std::future_status::ready) {
            cached_volumes_ = vol_future_.get();
            vol_in_flight_ = false;
        }
    }

    if (frame_in_flight_ && frame_future_.valid()) {
        if (frame_future_.wait_for(0ms) == std::future_status::ready) {
            TelemetryFrame f = frame_future_.get();
            frame_in_flight_ = false;
            ingestFrame(std::move(f));
        }
    }

    if (warmup_pending_ && now >= warmup_due_) {
        warmup_pending_ = false;
        dispatchSample();
    }

    if (now >= next_due_) {
        next_due_ = now + std::chrono::milliseconds(interval_ms_);
        dispatchSample();
    }
}

void Monitor::setInterval(int interval_ms) {
    if ((interval_ms != 1000 && interval_ms != 2000 && interval_ms != 5000) || interval_ms == interval_ms_) {
        return;
    }
    interval_ms_ = interval_ms;
    next_due_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(interval_ms_);
    persistConfig();
    intervalChanged.emit();
    updated.emit();
}

void Monitor::setScope(int scope) {
    scope = std::clamp(scope, 0, 2);
    if (scope_ == scope) return;
    scope_ = scope;
    processes_.setAllUsers(scope_ == 2);
    persistConfig();
    scopeChanged.emit();
}

void Monitor::sortBy(const std::string& column) {
    processes_.sortBy(column);
    applications_.sortBy(column);
    persistConfig();
}

void Monitor::persistConfig() const {
    if (config_path_.empty()) return;
    Preferences pref;
    pref.interval = interval_ms_;
    pref.scope = scope_;
    pref.sortColumn = processes_.sortColumn();
    pref.sortAscending = processes_.ascending();
    pref.save(config_path_);
}

void Monitor::dispatchSample() {
    if (!vol_in_flight_) {
        vol_in_flight_ = true;
        vol_future_ = std::async(std::launch::async, [] { return query_mounted_volumes(); });
    }
    if (frame_in_flight_) return;
    auto col = collector_;
    frame_in_flight_ = true;
    frame_future_ = std::async(std::launch::async, [col] { return col->collect(); });
}

void Monitor::ingestFrame(TelemetryFrame frame) {
    if (frame.is_warmup) {
        warmup_pending_ = true;
        warmup_due_ = std::chrono::steady_clock::now() + 250ms;
        return;
    }

    error_ = frame.err_msg;
    if (frame.ok) {
        ready_ = true;
        metrics_ = std::move(frame.metrics);
        metrics_.volumes = cached_volumes_;
        ++sample_seq_;

        for (ProcessRecord& p : frame.processes) tagProcessApp(p);
        applications_.apply(aggregateApplications(frame.processes));

        auto is_stale = [this](const auto& kv) { return (sample_seq_ - kv.second.last_sample) > 60ULL; };
        std::erase_if(app_by_prog_, is_stale);
        std::erase_if(app_by_id_, is_stale);

        processes_.apply(std::move(frame.processes));

        int64_t ts_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - epoch_)
                            .count();
        history_.timestamps.push_back(ts_ms);
        history_.cpuPercent.push_back(metrics_.cpuPercent);
        history_.memoryPercent.push_back(metrics_.memory.percent);
        history_.download.push_back(metrics_.download);
        history_.upload.push_back(metrics_.upload);
        history_.diskRead.push_back(metrics_.diskRead);
        history_.diskWrite.push_back(metrics_.diskWrite);
        history_.gpuPercent.push_back(metrics_.gpuPercent);
        history_.gpuTemperature.push_back(metrics_.gpuTemperature);

        // Per-GPU series: realign with the timestamps when the GPU set changes.
        if (history_.gpus.size() != metrics_.gpus.size()) {
            history_.gpus.assign(metrics_.gpus.size(), GpuHistory{});
            for (auto& gh : history_.gpus) {
                gh.busy.assign(history_.timestamps.size() - 1, 0.0);
                gh.temperature.assign(history_.timestamps.size() - 1, 0.0);
            }
        }
        for (size_t gi = 0; gi < metrics_.gpus.size(); ++gi) {
            history_.gpus[gi].busy.push_back(static_cast<double>(metrics_.gpus[gi].busy));
            history_.gpus[gi].temperature.push_back(std::max(0.0, metrics_.gpus[gi].temperature));
        }

        int64_t horizon_ms = ts_ms - static_cast<int64_t>(historyDuration()) * 1000LL;
        while (history_.timestamps.size() > 2 && history_.timestamps[1] < horizon_ms) {
            for (auto* vec : {&history_.cpuPercent, &history_.memoryPercent, &history_.download,
                              &history_.upload, &history_.diskRead, &history_.diskWrite,
                              &history_.gpuPercent, &history_.gpuTemperature}) {
                vec->erase(vec->begin());
            }
            for (auto& gh : history_.gpus) {
                gh.busy.erase(gh.busy.begin());
                gh.temperature.erase(gh.temperature.begin());
            }
            history_.timestamps.erase(history_.timestamps.begin());
        }
    }
    updated.emit();
}

void Monitor::tagProcessApp(ProcessRecord& proc) {
    if (proc.program.empty()) return;

    auto it = app_by_prog_.find(proc.program);
    if (it == app_by_prog_.end()) {
        std::string bname = path_basename(proc.program);
        auto found = DesktopCatalog::instance().resolveProcess(bname, proc.program);
        CachedAppIdentity cid;
        if (found && !found->noDisplay && !found->hidden) {
            cid.id = found->id;
            cid.name = found->name;
            cid.icon = found->icon;
        }
        it = app_by_prog_.emplace(proc.program, std::move(cid)).first;
    }
    it->second.last_sample = sample_seq_;
    proc.appId = it->second.id;
    if (!it->second.name.empty()) proc.displayName = it->second.name;
    if (!it->second.icon.empty()) proc.iconName = it->second.icon;
    if (!proc.helperType.empty()) {
        proc.displayName += " (" + describeHelperRole(proc.helperType) + ")";
    }
}

std::vector<ProcessRecord> Monitor::aggregateApplications(const std::vector<ProcessRecord>& all_procs) {
    const uint32_t my_uid = ::getuid();
    std::unordered_map<int, size_t> pid_to_idx;
    pid_to_idx.reserve(all_procs.size());
    for (size_t i = 0; i < all_procs.size(); ++i) {
        pid_to_idx.emplace(all_procs[i].pid, i);
    }

    auto find_idx = [&pid_to_idx](int pid) -> int {
        auto it = pid_to_idx.find(pid);
        return (it != pid_to_idx.end()) ? static_cast<int>(it->second) : -1;
    };

    std::unordered_map<std::string, std::vector<size_t>> buckets;
    std::vector<std::string> app_order;

    for (size_t i = 0; i < all_procs.size(); ++i) {
        const ProcessRecord& p = all_procs[i];
        if (p.uid != my_uid || p.kernelThread) continue;

        std::string aid = p.appId;
        int cur_parent = find_idx(p.parentPid);
        for (int depth = 0; aid.empty() && cur_parent >= 0 && depth < 64; ++depth) {
            const ProcessRecord& par = all_procs[static_cast<size_t>(cur_parent)];
            if (par.uid != my_uid || par.pid <= 1) break;
            aid = par.appId;
            cur_parent = find_idx(par.parentPid);
        }

        if (aid.empty() && !p.scopeId.empty()) {
            auto sit = app_by_id_.find(p.scopeId);
            if (sit == app_by_id_.end()) {
                auto found = DesktopCatalog::instance().findById(p.scopeId);
                CachedAppIdentity cid;
                if (found && !found->noDisplay && !found->hidden) {
                    cid.id = found->id;
                    cid.name = found->name;
                    cid.icon = found->icon;
                }
                sit = app_by_id_.emplace(p.scopeId, std::move(cid)).first;
            }
            sit->second.last_sample = sample_seq_;
            aid = sit->second.id;
        }

        if (aid.empty()) continue;
        auto& vec = buckets[aid];
        if (vec.empty()) app_order.push_back(aid);
        vec.push_back(i);
    }

    std::vector<ProcessRecord> grouped;
    grouped.reserve(app_order.size());

    for (const std::string& aid : app_order) {
        const auto& member_indices = buckets[aid];
        std::unordered_set<int> member_pids;
        for (size_t idx : member_indices) member_pids.insert(all_procs[idx].pid);

        const ProcessRecord* leader = nullptr;
        for (size_t idx : member_indices) {
            const ProcessRecord& cand = all_procs[idx];
            if (member_pids.contains(cand.parentPid)) continue;
            bool cand_direct = (cand.appId == aid);
            bool leader_direct = (leader && leader->appId == aid);
            if (!leader || (cand_direct && !leader_direct) ||
                (cand_direct == leader_direct && cand.startTicks < leader->startTicks)) {
                leader = &cand;
            }
        }
        if (!leader) {
            for (size_t idx : member_indices) {
                const ProcessRecord& cand = all_procs[idx];
                if (!leader || cand.startTicks < leader->startTicks) leader = &cand;
            }
        }

        auto ait = app_by_id_.find(aid);
        if (ait == app_by_id_.end()) {
            auto found = DesktopCatalog::instance().findById(aid);
            CachedAppIdentity cid = found
                ? CachedAppIdentity{found->id, found->name, found->icon, sample_seq_}
                : CachedAppIdentity{aid, leader->displayName, leader->iconName, sample_seq_};
            ait = app_by_id_.emplace(aid, std::move(cid)).first;
        }
        ait->second.last_sample = sample_seq_;

        ProcessRecord agg = *leader;
        agg.appId = aid;
        agg.displayName = ait->second.name.empty() ? leader->displayName : ait->second.name;
        agg.iconName = ait->second.icon.empty() ? leader->iconName : ait->second.icon;
        agg.cpu = 0.0;
        agg.memory = 0;
        agg.readRate = 0.0;
        agg.writeRate = 0.0;
        agg.threads = 0;
        agg.ioAvailable = false;
        agg.members.clear();
        agg.members.reserve(member_indices.size());

        for (size_t idx : member_indices) {
            const ProcessRecord& m = all_procs[idx];
            agg.cpu += m.cpu;
            agg.memory += m.memory;
            agg.threads += m.threads;
            if (m.ioAvailable) {
                agg.ioAvailable = true;
                agg.readRate += m.readRate;
                agg.writeRate += m.writeRate;
            }
            agg.members.push_back(m);
        }
        agg.cpu = std::min(100.0, agg.cpu);
        grouped.push_back(std::move(agg));
    }

    std::sort(grouped.begin(), grouped.end(),
              [](const ProcessRecord& a, const ProcessRecord& b) { return a.pid < b.pid; });
    return grouped;
}

const ProcessRecord* Monitor::process(int pid, uint64_t start_ticks) const {
    const ProcessRecord* r = processes_.find(pid);
    return (r && r->startTicks == start_ticks) ? r : nullptr;
}

const ProcessRecord* Monitor::application(int pid, uint64_t start_ticks) const {
    const ProcessRecord* r = applications_.find(pid);
    return (r && r->startTicks == start_ticks) ? r : nullptr;
}

bool Monitor::canManage(const ProcessRecord& proc) {
    if (proc.members.empty()) {
        return proc.uid == ::getuid() && proc.pid > 1 && proc.pid != ::getpid();
    }
    return std::all_of(proc.members.begin(), proc.members.end(),
                       [](const ProcessRecord& m) { return canManage(m); });
}

std::string Monitor::endProcess(int pid, uint64_t start_ticks, bool force) {
    std::string err = signalProcess(pid, start_ticks, force);
    if (err.empty()) dispatchSample();
    return err;
}

std::string Monitor::endApplication(int pid, uint64_t start_ticks, bool force) {
    const ProcessRecord* app = application(pid, start_ticks);
    if (!app) return "The application has already quit.";
    if (!canManage(*app)) return "This application cannot be quit here.";

    std::unordered_set<int> member_pids;
    for (const auto& m : app->members) member_pids.insert(m.pid);

    std::string first_err;
    bool sent_any = false;
    for (const auto& m : app->members) {
        if (!force && member_pids.contains(m.parentPid)) continue;
        std::string res = signalProcess(m.pid, m.startTicks, force);
        if (res.empty()) {
            sent_any = true;
        } else if (res != "The process has already exited." && first_err.empty()) {
            first_err = res;
        }
    }
    if (!first_err.empty()) return first_err;
    if (!sent_any) return "The application has already quit.";
    dispatchSample();
    return {};
}

std::string Monitor::signalProcess(int pid, uint64_t start_ticks, bool force) {
    if (pid <= 1 || pid == ::getpid()) return "This process cannot be ended here.";

    int pfd = static_cast<int>(::syscall(SYS_pidfd_open, pid, 0));
    if (pfd < 0) {
        return (errno == ESRCH)
            ? "The process has already exited."
            : (std::string("Unable to open the process: ") + std::strerror(errno));
    }

    ProcessRecord verify;
    std::string err;
    if (!parse_proc_stat_blob(slurp_file("/proc/" + std::to_string(pid) + "/stat"), verify, 1) ||
        verify.startTicks != start_ticks) {
        err = "The process has already exited.";
    } else if (::syscall(SYS_pidfd_send_signal, pfd, force ? SIGKILL : SIGTERM, nullptr, 0) != 0) {
        int e = errno;
        if (e == ESRCH) err = "The process has already exited.";
        else if (e == EPERM) err = "You can only end your own processes.";
        else err = std::string("Unable to end the process: ") + std::strerror(e);
    }
    ::close(pfd);
    return err;
}

int64_t Monitor::systemBootTime() {
    std::string stat_blob = slurp_file("/proc/stat");
    for (std::string_view line : sv_split_char(stat_blob, '\n')) {
        if (!line.starts_with("btime ")) continue;
        auto cols = sv_split_ws(line);
        if (cols.size() >= 2) return sv_i64(cols[1], 0);
    }
    return 0;
}

ProcessExtras Monitor::inspectProcessExtras(int pid, uint64_t start_ticks) {
    std::string pdir = "/proc/" + std::to_string(pid);
    ProcessExtras ex;
    if (DIR* d = ::opendir((pdir + "/fd").c_str())) {
        ex.openFiles = 0;
        while (const dirent* e = ::readdir(d)) {
            if (e->d_name[0] != '.') ++ex.openFiles;
        }
        ::closedir(d);
    }
    ex.unit = extract_cgroup_unit(slurp_file(pdir + "/cgroup"));

    ProcessRecord check;
    if (!parse_proc_stat_blob(slurp_file(pdir + "/stat"), check, 1) || check.startTicks != start_ticks) {
        return {};
    }
    return ex;
}

std::string Monitor::describeProcessState(std::string_view code) {
    if (code == "R") return "Running";
    if (code == "S" || code == "I") return "Sleeping";
    if (code == "D") return "Waiting for I/O";
    if (code == "T" || code == "t") return "Stopped";
    if (code == "Z") return "Zombie";
    return std::string(code);
}

std::string Monitor::describeHelperRole(std::string_view role) {
    if (role == "renderer") return "Renderer";
    if (role == "extension") return "Extension";
    if (role == "gpu-process") return "GPU";
    if (role == "zygote") return "Zygote";
    if (role == "network.mojom.NetworkService") return "Network";
    if (role == "audio.mojom.AudioService") return "Audio";
    if (role == "storage.mojom.StorageService") return "Storage";
    if (role == "video_capture.mojom.VideoCaptureService") return "Video Capture";
    if (role == "utility" || role.find(".mojom.") != std::string_view::npos) return "Utility";
    return std::string(role);
}

namespace {

std::string format_number_Grouped(double val, int precision) {
    if (std::isnan(val)) return "nan";
    if (std::isinf(val)) return val < 0.0 ? "-inf" : "inf";
    precision = std::max(0, precision);

    char buf[128];
    std::snprintf(buf, sizeof(buf), "%.*f", precision, std::fabs(val));
    std::string s(buf);
    size_t dot = s.find('.');
    std::string int_part = (dot == std::string::npos) ? s : s.substr(0, dot);
    std::string frac_part = (dot == std::string::npos) ? std::string{} : s.substr(dot + 1);

    for (int64_t idx = static_cast<int64_t>(int_part.size()) - 3; idx > 0; idx -= 3) {
        int_part.insert(static_cast<size_t>(idx), 1, ',');
    }

    std::string res = (val < 0.0 ? "-" : "") + int_part;
    if (precision > 0 && !frac_part.empty()) {
        res += "." + frac_part;
    }
    return res;
}

} // namespace

std::string Monitor::formatBytes(double bytes) {
    static constexpr const char* kUnits[5] = {"B", "KiB", "MiB", "GiB", "TiB"};
    int u = 0;
    bytes = std::max(0.0, bytes);
    while (bytes >= 1024.0 && u < 4) {
        bytes /= 1024.0;
        ++u;
    }
    return format_number_Grouped(bytes, u == 0 ? 0 : 1) + " " + kUnits[u];
}

std::string Monitor::formatRate(double bytes_per_sec) {
    return formatBytes(bytes_per_sec) + "/s";
}

std::string Monitor::formatPercent(double pct) {
    return format_number_Grouped(pct, 1) + "%";
}

std::string Monitor::formatTemperature(double celsius) {
    return format_number_Grouped(celsius, 0) + " °C";
}

std::string Monitor::formatDuration(int64_t seconds) {
    seconds = std::max<int64_t>(0, seconds);
    int64_t d = seconds / 86400;
    int64_t h = (seconds / 3600) % 24;
    int64_t m = (seconds / 60) % 60;
    if (seconds >= 86400) return std::to_string(d) + " d " + std::to_string(h) + " h";
    if (seconds >= 3600) return std::to_string(h) + " h " + std::to_string(m) + " min";
    if (seconds >= 60) return std::to_string(m) + " min";
    return std::to_string(seconds) + " s";
}

} // namespace sysmon
