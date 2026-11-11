#include "latch/Door.hpp"
#include "latch/Package.hpp"
#include "latch/Protect.hpp"   // the never-touch policy, shared with every scan
#include "latch/Vcs.hpp"    // the Clone verb

#include "json.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <linux/fs.h>        // FIDEDUPERANGE
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/xattr.h>     // the Tag verb
#include <unistd.h>

// Door.cpp -- the ONLY file in Rouster that changes a user's files. The
// selftest's source gate allowlists it by name; a write call anywhere else in
// src/ or include/ fails the build's selftest.

namespace latch::door {

namespace fs = std::filesystem;
using json = nlohmann::json;

const char* verb_name(Verb v) {
    switch (v) {
        case Verb::Trash: return "trash";
        case Verb::Move:  return "move";
        case Verb::Write: return "write";
        case Verb::Clone: return "clone";
        case Verb::Dedupe: return "dedupe";
        case Verb::MakeDir: return "mkdir";
        case Verb::Copy: return "copy";
        case Verb::Settings: return "settings";
        case Verb::Root: return "root";
        case Verb::Tag: return "tag";
    }
    return "?";
}
bool creates(Verb v) { return v == Verb::Write || v == Verb::Clone || v == Verb::MakeDir || v == Verb::Copy; }

const std::vector<Verb>& verbs() {
    static const std::vector<Verb> v = {Verb::Trash, Verb::Move, Verb::Write, Verb::Clone, Verb::Dedupe, Verb::MakeDir, Verb::Copy, Verb::Settings, Verb::Root, Verb::Tag};
    return v;
}

namespace {
Verb verb_of(const std::string& s) {
    for (Verb v : verbs()) if (s == verb_name(v)) return v;
    return Verb::Trash;
}
}  // namespace

std::string tag_name_problem(const std::string& name) {
    const std::string pre = kTagPrefix;
    if (name.rfind(pre, 0) != 0) return "not a " + pre + "* tag: " + name;
    if (name.size() == pre.size()) return "a tag needs a name after " + pre;
    if (name.size() > 255) return "tag name too long: " + name;
    for (std::size_t i = pre.size(); i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (!(std::islower(c) || std::isdigit(c) || c == '.' || c == '_' || c == '-'))
            return "not a tag name ([a-z0-9._-] after " + pre + "): " + name;
    }
    return {};
}

bool tag_of(const std::string& path, const std::string& name, std::string& value, std::string* err) {
    value.clear();
    for (int tries = 0; tries < 3; ++tries) {
        const ssize_t n = ::lgetxattr(path.c_str(), name.c_str(), nullptr, 0);
        if (n < 0) {
            if (err) *err = errno == ENODATA ? std::string() : std::string(std::strerror(errno));
            return false;
        }
        value.resize(static_cast<std::size_t>(n));
        const ssize_t m = n == 0 ? 0 : ::lgetxattr(path.c_str(), name.c_str(), value.data(), value.size());
        if (m >= 0) { value.resize(static_cast<std::size_t>(m)); if (err) err->clear(); return true; }
        if (errno != ERANGE) { if (err) *err = std::strerror(errno); return false; }   // grew between calls: again
    }
    if (err) *err = "the tag kept changing size";
    return false;
}

bool identity_of(const std::string& path, Identity& out) {
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return false;
    out.dev = st.st_dev;
    out.ino = st.st_ino;
    out.size = static_cast<std::uint64_t>(st.st_size);
    out.mtime_ns = static_cast<std::int64_t>(st.st_mtim.tv_sec) * 1'000'000'000 + st.st_mtim.tv_nsec;
    out.dir = S_ISDIR(st.st_mode);
    return true;
}

std::string trashinfo(const std::string& abs, std::int64_t when) {
    std::string enc;
    static const char* hex = "0123456789ABCDEF";
    for (unsigned char c : abs) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~') enc += static_cast<char>(c);
        else { enc += '%'; enc += hex[c >> 4]; enc += hex[c & 15]; }
    }
    std::time_t t = static_cast<std::time_t>(when);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char date[32];
    std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &tm);
    return "[Trash Info]\nPath=" + enc + "\nDeletionDate=" + date + "\n";
}

namespace {

std::int64_t now_s() { return static_cast<std::int64_t>(std::time(nullptr)); }

// rename that never overwrites. renameat2(RENAME_NOREPLACE) where the
// filesystem has it (btrfs, ext4, xfs, tmpfs); else check-then-rename.
int rename_noreplace(const std::string& a, const std::string& b) {
#ifdef RENAME_NOREPLACE
    if (::renameat2(AT_FDCWD, a.c_str(), AT_FDCWD, b.c_str(), RENAME_NOREPLACE) == 0) return 0;
    if (errno != EINVAL && errno != ENOSYS) return -1;
#endif
    struct stat st{};
    if (::lstat(b.c_str(), &st) == 0) { errno = EEXIST; return -1; }
    return ::rename(a.c_str(), b.c_str());
}

bool inside(const std::string& path, const std::string& dir) {   // strictly below, or equal
    return path == dir || (path.size() > dir.size() && path.rfind(dir, 0) == 0 && path[dir.size()] == '/');
}

// Tag values go into the manifest as hex: a value someone else wrote may not be
// UTF-8, and the manifest is JSON.
std::string hex_of(const std::string& s) {
    static const char* h = "0123456789abcdef";
    std::string o;
    for (unsigned char c : s) { o += h[c >> 4]; o += h[c & 15]; }
    return o;
}
std::string unhex(const std::string& s) {
    const auto v = [](char c) { return c >= 'a' ? c - 'a' + 10 : c - '0'; };
    std::string o;
    for (std::size_t i = 0; i + 1 < s.size(); i += 2) o += static_cast<char>(v(s[i]) * 16 + v(s[i + 1]));
    return o;
}

std::string ident_json(const Identity& i) {
    return json{{"dev", i.dev}, {"ino", i.ino}, {"size", i.size}, {"mtime_ns", i.mtime_ns}, {"dir", i.dir}}.dump();
}

}  // namespace

std::vector<std::string> dconf_keys(const std::string& ini) {
    std::vector<std::string> out;
    std::string section;
    std::istringstream in(ini);
    for (std::string l; std::getline(in, l);) {
        if (l.empty() || l[0] == '#') continue;
        if (l.front() == '[' && l.back() == ']') { section = l.substr(1, l.size() - 2); continue; }
        const auto eq = l.find('=');
        if (eq == std::string::npos || section.empty()) continue;
        std::string dir = section == "/" ? "" : section;
        while (!dir.empty() && dir.back() == '/') dir.pop_back();
        out.push_back((dir.empty() || dir[0] != '/' ? "/" : "") + dir + (dir.empty() ? "" : "/") + l.substr(0, eq));
    }
    return out;
}

std::string dconf_problem(const std::string& ini) {
    if (ini.empty()) return "nothing to load";
    if (ini.size() > kWriteMax) return "too big to load";
    std::string section;
    std::istringstream in(ini);
    int n = 0;
    for (std::string l; std::getline(in, l);) {
        ++n;
        if (l.empty() || l[0] == '#') continue;
        if (l.front() == '[') {
            if (l.back() != ']' || l.size() < 3) return "line " + std::to_string(n) + " is not a [section]";
            section = l.substr(1, l.size() - 2);
            if (section.find("..") != std::string::npos || section.find("//") != std::string::npos)
                return "line " + std::to_string(n) + ": a strange path";
            continue;
        }
        const auto eq = l.find('=');
        if (section.empty() || eq == std::string::npos || eq == 0 || l.substr(0, eq).find('/') != std::string::npos)
            return "line " + std::to_string(n) + " is not key=value under a [section]";
    }
    return {};
}

std::vector<std::string> fixture_dconf_argv(const std::string& home, const std::vector<std::string>& argv) {
    std::vector<std::string> out = {"env", "XDG_CONFIG_HOME=" + home + "/.config", "dbus-run-session", "--"};
    out.insert(out.end(), argv.begin(), argv.end());
    return out;
}

DconfRunner fixture_dconf(const std::string& home) {
    return [home](const std::vector<std::string>& argv, const std::string& in) {
        return vcs::feed(fixture_dconf_argv(home, argv), in, "", 60);
    };
}

namespace {
RootRules g_root_rules;   // set once at startup, before any Door runs
std::string g_app = "the app";
std::string own_records() { return g_app == "the app" ? std::string("the app's own records: ") : g_app + "'s own records: "; }
std::string staging_prefix() {
    std::string p = ".";
    for (char ch : g_app) p += ch == ' ' ? '-' : static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return p + "-";
}
}  // namespace

void set_app_name(std::string name) { g_app = name.empty() ? std::string("the app") : std::move(name); }
const std::string& app_name() { return g_app; }

namespace {
std::vector<OwnFile> g_own;
}  // namespace
void set_own_files(std::vector<OwnFile> files) { g_own = std::move(files); }
const std::vector<OwnFile>& own_files() { return g_own; }

namespace {
std::vector<MediaRoot> g_media;
bool trash_name(const std::string& part) { return part.rfind(".Trash", 0) == 0; }
}  // namespace
void set_media_roots(std::vector<MediaRoot> roots) {
    for (auto& m : roots) while (m.path.size() > 1 && m.path.back() == '/') m.path.pop_back();
    g_media = std::move(roots);
}
const std::vector<MediaRoot>& media_roots() { return g_media; }

std::string media_root_problem(const MediaRoot& m, const std::string& home) {
    const std::string& p = m.path;
    if (p.empty() || p[0] != '/' || fs::path(p).lexically_normal().string() != p || p == "/" || p.back() == '/')
        return "not a clean absolute folder: " + p;
    if (inside(p, home) || inside(home, p)) return "in or around the home folder: " + p;
    for (const auto& part : fs::path(p)) if (trash_name(part.string())) return "a trash folder: " + p;
    struct stat st{};
    if (::lstat(p.c_str(), &st) != 0) return "not there (not mounted?): " + p;
    if (S_ISLNK(st.st_mode)) return "a symlink: " + p;
    if (!S_ISDIR(st.st_mode)) return "not a folder: " + p;
    if (::access(p.c_str(), W_OK) != 0) return "not writable: " + p;
    if (m.other_disk) {
        struct stat hs{}, rs{};
        if (::stat(home.c_str(), &hs) == 0 && hs.st_dev == st.st_dev)
            return "on the same disk as home (not mounted?): " + p;
        if (::stat("/", &rs) == 0 && rs.st_dev == st.st_dev) return "on the system disk (not mounted?): " + p;
    }
    return {};
}

const MediaRoot* media_root_of(const std::string& path) {
    for (const auto& m : g_media)
        if (!m.path.empty() && path != m.path && inside(path, m.path)) return &m;
    return nullptr;
}

std::string media_trash_dir(const MediaRoot& m) {
    const std::string uid = std::to_string(::getuid());
    struct stat st{};
    if (::stat(m.path.c_str(), &st) != 0) return m.path + "/.Trash-" + uid;
    // The disk's top: walk up while the parent is on the same device.
    fs::path top = m.path;
    while (top.has_parent_path() && top != top.parent_path()) {
        struct stat ps{};
        if (::stat(top.parent_path().c_str(), &ps) != 0 || ps.st_dev != st.st_dev) break;
        top = top.parent_path();
    }
    const std::string t = (top / (".Trash-" + uid)).string();
    struct stat ts{};
    if (::lstat(t.c_str(), &ts) == 0 ? S_ISDIR(ts.st_mode) && !S_ISLNK(ts.st_mode)
                                     : top != "/" && ::access(top.c_str(), W_OK) == 0)
        return t;
    return m.path + "/.Trash-" + uid;
}

void set_root_rules(RootRules rules) { g_root_rules = std::move(rules); }
const RootRules& root_rules() { return g_root_rules; }

std::vector<std::string> root_argv_for(const std::string& helper_path, const std::string& request, bool dry) {
    std::vector<std::string> v = {"/usr/bin/pkexec", helper_path, "run"};
    if (dry) v.push_back("--dry-run");
    v.push_back(request);
    return v;
}


Door::Door(const std::string& home, const std::string& data_dir) {
    std::error_code ec;
    m_home = fs::weakly_canonical(home, ec).string();
    m_data = fs::weakly_canonical(data_dir, ec).string();
    m_manifest = m_data + "/manifest.jsonl";
    m_trash = m_home + "/.local/share/Trash";
    m_dconf = [](const std::vector<std::string>& argv, const std::string& in) { return vcs::feed(argv, in, "", 60); };
    m_root = [](const std::vector<std::string>& argv) { return vcs::child(argv, 15 * 60); };
}

Record* Door::find(std::uint64_t id) {
    for (auto& r : m_records) if (r.id == id) return &r;
    return nullptr;
}
const Record* Door::record(std::uint64_t id) const {
    for (const auto& r : m_records) if (r.id == id) return &r;
    return nullptr;
}

bool Door::append(const std::string& line) {
    std::error_code ec;
    fs::create_directories(m_data, ec);
    const int fd = ::open(m_manifest.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const std::string l = line + "\n";
    const bool ok = ::write(fd, l.data(), l.size()) == static_cast<ssize_t>(l.size()) && ::fdatasync(fd) == 0;
    ::close(fd);
    return ok;
}

bool Door::load() {
    m_batches.clear();
    m_records.clear();
    m_unfinished.clear();
    m_next_id = m_next_batch = 1;
    std::ifstream in(m_manifest);
    if (!in) return true;   // no manifest yet: nothing done, nothing to undo
    std::string line;
    while (std::getline(in, line)) {
        json j = json::parse(line, nullptr, false);
        if (j.is_discarded() || !j.contains("t")) continue;   // a torn last line from a crash
        const std::string t = j["t"];
        if (t == "batch") {
            Batch b{j.value("batch", 0ull), j.value("label", ""), j.value("when", 0ll), {}, false};
            m_next_batch = std::max(m_next_batch, b.id + 1);
            m_batches.push_back(b);
        } else if (t == "intent") {
            Record r;
            r.id = j.value("id", 0ull);
            r.batch = j.value("batch", 0ull);
            r.verb = verb_of(j.value("verb", ""));
            r.from = j.value("from", "");
            r.to = j.value("to", "");
            r.info = j.value("info", "");
            r.src = j.value("src", "");
            r.when = j.value("when", 0ll);
            if (r.verb == Verb::Tag) {
                r.attr = j.value("attr", "");
                r.had = j.value("had", false);
                r.untag = j.value("untag", false);
                r.before = unhex(j.value("before", ""));
                r.after = unhex(j.value("after", ""));
            }
            if (j.contains("ident")) {
                const auto& i = j["ident"];
                r.ident = {i.value("dev", 0ull), i.value("ino", 0ull), i.value("size", 0ull),
                           i.value("mtime_ns", 0ll), i.value("dir", false)};
            }
            m_next_id = std::max(m_next_id, r.id + 1);
            m_records.push_back(r);
            for (auto& b : m_batches) if (b.id == r.batch) b.records.push_back(r.id);
        } else if (Record* r = find(j.value("id", 0ull))) {
            if (t == "done")        { r->state = State::Done; r->note = j.value("note", ""); }
            else if (t == "failed") { r->state = State::Failed; r->error = j.value("error", ""); }
            else if (t == "undone") r->state = State::Undone;
            // "undo-failed" leaves it Done: still in the Trash / still moved.
        } else if (t == "batch-undone") {
            for (auto& b : m_batches) if (b.id == j.value("batch", 0ull)) b.undone = true;
        }
    }
    // A crash between intent and outcome: look where the thing is now.
    for (auto& r : m_records) {
        if (r.state != State::Intent) continue;
        if (r.verb == Verb::Settings) {   // can't be looked at: count it done -- Undo is safe either way
            r.state = State::Done;
            m_unfinished.push_back("settings load -- may or may not have happened; Undo puts the saved settings back");
            continue;
        }
        if (r.verb == Verb::Root) {       // the same: what root did can't be looked at from here
            r.state = State::Done;
            m_unfinished.push_back("root " + r.to + " -- may or may not have happened");
            continue;
        }
        if (r.verb == Verb::Tag) {        // look at the tag: ours -> done, as before -> not done
            std::string v;
            Identity at{};
            const bool same = identity_of(r.from, at) && at.dev == r.ident.dev && at.ino == r.ident.ino;
            const bool has = same && tag_of(r.from, r.attr, v);
            const bool ours = same && (r.untag ? !has : has && v == r.after);
            const bool old = same && (r.had ? has && v == r.before : !has);
            if (ours && !old) r.state = State::Done;
            else if (old) r.state = State::Failed;   // also when set == before: nothing to undo either way
            else r.state = State::Failed, r.error = "unaccounted for after a crash";
            m_unfinished.push_back("tag " + r.attr + " on " + r.from + " -- " +
                                   (r.state == State::Done ? "had happened" : "had not happened"));
            continue;
        }
        Identity at_to{}, at_from{};
        if (identity_of(r.to, at_to) && at_to == r.ident) r.state = State::Done;
        else if (identity_of(r.from, at_from) && at_from == r.ident) r.state = State::Failed;
        else r.state = State::Failed, r.error = "unaccounted for after a crash";
        m_unfinished.push_back(std::string(verb_name(r.verb)) + " " + r.from + " -- " +
                               (r.state == State::Done ? "had happened" : "had not happened"));
    }
    return true;
}

std::string Door::refusal(const Action& a) const { return refusal(a, {}); }

std::string Door::refusal(const Action& a, const std::vector<std::string>& made) const {
    return refusal(a, made, {});
}

std::string Door::refusal(const Action& a, const std::vector<std::string>& made,
                          const std::vector<std::string>& freed) const {
    const auto was_freed = [&](const std::string& p) { return std::find(freed.begin(), freed.end(), p) != freed.end(); };
    const auto planned = [&](const std::string& p) { return std::find(made.begin(), made.end(), p) != made.end(); };
    const auto bad = [](const std::string& p) {
        return p.empty() || p[0] != '/' || fs::path(p).lexically_normal().string() != p ||
               (p.size() > 1 && p.back() == '/');
    };
    if (a.verb == Verb::Root) {       // no path: one request word for the root helper
        if (!a.from.empty() || !a.to.empty()) return "a root request names no file";
        if (g_root_rules.helper_path.empty() || !g_root_rules.known) return "this app has no root helper";
        if (!g_root_rules.known(a.content)) return "not a request the helper knows: " + a.content;
        return {};
    }
    if (a.verb == Verb::Settings) {   // no path: settings, through dconf
        if (!a.from.empty() || !a.to.empty()) return "a settings load names no file";
        if (auto why = dconf_problem(a.content); !why.empty()) return "not a dconf dump: " + why;
        return {};
    }
    if (!creates(a.verb)) {
        if (bad(a.from)) return "not a clean absolute path: " + a.from;
        if (!inside(a.from, m_home) || a.from == m_home) return "outside the home folder: " + a.from;
    }

    // Every component from home down: no symlinked parent, nothing Protected.
    const auto walk = [&](const std::string& p, bool last_may_be_missing) -> std::string {
        const std::string rel = p.substr(m_home.size() + 1);
        std::string cur;
        std::size_t pos = 0;
        while (true) {
            const auto s = rel.find('/', pos);
            cur = rel.substr(0, s);
            const bool last = s == std::string::npos;
            if (protect::here(cur)) return "protected (never touched): ~/" + cur;
            // s015a: nothing inside a Mac package -- it is kept whole (moved whole, at most).
            if (!last && pkg::is_package_name(rel.substr(pos, s - pos)))
                return "inside a package (kept whole, never split): ~/" + cur;
            struct stat st{};
            const std::string abs = m_home + "/" + cur;
            if (::lstat(abs.c_str(), &st) != 0) {
                if (last && last_may_be_missing) break;
                if (planned(abs)) { if (last) break; pos = s + 1; continue; }   // an earlier MakeDir makes it
                return "does not exist: " + abs;
            }
            if (!last && S_ISLNK(st.st_mode)) return "goes through a symlink: ~/" + cur;
            if (last) break;
            pos = s + 1;
        }
        return {};
    };
    if (creates(a.verb)) {
        if (a.verb == Verb::Copy) {   // the source: a regular file in home, nothing protected on the way
            if (bad(a.from)) return "not a clean absolute path to copy: " + a.from;
            if (!inside(a.from, m_home) || a.from == m_home) return "outside the home folder: " + a.from;
            if (auto w = walk(a.from, false); !w.empty()) return w;
            if (inside(a.from, m_trash)) return "in the Trash: " + a.from;
            if (inside(a.from, m_data)) return own_records() + a.from;
            struct stat ss{};
            if (::lstat(a.from.c_str(), &ss) != 0 || !S_ISREG(ss.st_mode)) return "not a file: " + a.from;
        } else if (!a.from.empty()) return "a " + std::string(verb_name(a.verb)) + " has no source";
        if (bad(a.to)) return "not a clean absolute path to create: " + a.to;
        if (const MediaRoot* mr = media_root_of(a.to)) return media_refusal(a, *mr, made);   // s004
        if (!inside(a.to, m_home) || a.to == m_home) return "outside the home folder: " + a.to;
        if (inside(a.to, m_trash) || inside(m_trash, a.to)) return "not in the Trash: " + a.to;
        if (inside(a.to, m_data) || inside(m_data, a.to)) return own_records() + a.to;
        if (auto w = walk(a.to, true); !w.empty()) return w;
        struct stat st{};
        if ((::lstat(a.to.c_str(), &st) == 0 && !was_freed(a.to)) || planned(a.to)) return "already exists: " + a.to;
        const std::string parent = fs::path(a.to).parent_path().string();
        if (!planned(parent) && (::stat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode))) return "folder missing: " + parent;
        if (a.verb == Verb::MakeDir || a.verb == Verb::Copy) return {};
        if (a.verb == Verb::Write) {
            if (a.content.size() > kWriteMax) return "too big to write: " + a.to;
            return {};
        }
        const auto flag = [](const std::string& s) { return !s.empty() && s[0] == '-'; };
        if (a.url.empty() || flag(a.url)) return "no clone URL for " + a.to;
        if (flag(a.branch)) return "not a branch name: " + a.branch;
        for (char c : a.commit)
            if (!std::isxdigit(static_cast<unsigned char>(c))) return "not a commit: " + a.commit;
        return {};
    }
    if (auto w = walk(a.from, false); !w.empty()) return w;
    if (inside(a.from, m_data) || inside(m_data, a.from)) return own_records() + a.from;

    if (a.verb == Verb::Tag) {
        if (!a.to.empty()) return "a tag names one file, no destination: " + a.to;
        if (auto why = tag_name_problem(a.attr); !why.empty()) return why;
        if (!a.untag) {
            if (a.content.size() > kTagMax) return "tag value too long: " + a.attr;
            if (a.content.find('\0') != std::string::npos) return "tag value is not text: " + a.attr;
        } else if (!a.content.empty()) return "an untag carries no value: " + a.attr;
        if (inside(a.from, m_trash) || inside(m_trash, a.from)) return "in the Trash: " + a.from;
        struct stat st{};
        if (::lstat(a.from.c_str(), &st) != 0) return "does not exist: " + a.from;
        if (S_ISLNK(st.st_mode)) return "a symlink is never tagged: " + a.from;
        if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) return "not a file or folder: " + a.from;
        return {};
    }

    if (a.verb == Verb::Dedupe) {
        if (bad(a.to)) return "not a clean absolute path: " + a.to;
        if (!inside(a.to, m_home) || a.to == m_home) return "outside the home folder: " + a.to;
        if (auto w = walk(a.to, false); !w.empty()) return w;
        for (const auto* p : {&a.from, &a.to}) {
            if (inside(*p, m_trash)) return "in the Trash: " + *p;
            if (inside(*p, m_data)) return own_records() + *p;
        }
        struct stat sa{}, sb{};
        if (::lstat(a.from.c_str(), &sa) != 0 || !S_ISREG(sa.st_mode)) return "not a file: " + a.from;
        if (::lstat(a.to.c_str(), &sb) != 0 || !S_ISREG(sb.st_mode)) return "not a file: " + a.to;
        if (sa.st_dev != sb.st_dev) return "on different filesystems: " + a.to;
        if (sa.st_ino == sb.st_ino) return "the same file (a hard link): " + a.to;
        if (sa.st_size != sb.st_size) return "not the same size: " + a.to;
        return {};
    }
    if (a.verb == Verb::Trash) {
        if (inside(a.from, m_trash) || inside(m_trash, a.from)) return "already in the Trash: " + a.from;
        if (struct stat ps{}; ::lstat(a.from.c_str(), &ps) == 0 && S_ISDIR(ps.st_mode) &&
                              pkg::is_package_name(fs::path(a.from).filename().string()))
            return "a package is only ever moved whole, never trashed by " + g_app + ": " + a.from;   // s015a
        return {};
    }
    // Move.
    if (bad(a.to)) return "not a clean absolute destination: " + a.to;
    if (!inside(a.to, m_home) || a.to == m_home) return "destination outside the home folder: " + a.to;
    if (inside(a.to, a.from)) return "cannot move a folder into itself: " + a.to;
    if (inside(a.to, m_trash)) return "use Trash, not Move, for the Trash: " + a.to;
    if (inside(a.to, m_data)) return own_records() + a.to;
    if (auto w = walk(a.to, true); !w.empty()) return "destination " + w;
    struct stat st{};
    if (::lstat(a.to.c_str(), &st) == 0 || planned(a.to)) return "destination exists: " + a.to;
    const std::string parent = fs::path(a.to).parent_path().string();
    if (!planned(parent) && (::stat(parent.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)))
        return "destination folder missing: " + parent;
    return {};
}

std::string Door::trash_for(const std::string& path) const {
    if (const MediaRoot* m = media_root_of(path)) return media_trash_dir(*m);
    return m_trash;
}

// A create onto a registered media root (s004). Copy's source was checked by
// the caller (a file in home, nothing protected on the way).
std::string Door::media_refusal(const Action& a, const MediaRoot& m, const std::vector<std::string>& made) const {
    const auto planned = [&](const std::string& p) { return std::find(made.begin(), made.end(), p) != made.end(); };
    const std::string where = m.label.empty() ? m.path : m.label;
    if (a.verb != Verb::Copy && a.verb != Verb::MakeDir && a.verb != Verb::Write)
        return std::string("only copy, mkdir and write go onto ") + where + ": " + a.to;
    if (auto why = media_root_problem(m, m_home); !why.empty()) return where + ": " + why;
    const std::string rel = a.to.substr(m.path.size() + 1);
    std::string cur;
    std::size_t pos = 0;
    while (true) {
        const auto s = rel.find('/', pos);
        cur = rel.substr(0, s);
        const bool last = s == std::string::npos;
        const std::string part = rel.substr(pos, last ? std::string::npos : s - pos);
        if (trash_name(part)) return "not in the trash: " + a.to;
        const std::string abs = m.path + "/" + cur;
        struct stat st{};
        if (::lstat(abs.c_str(), &st) != 0) {
            if (last) break;
            if (planned(abs)) { pos = s + 1; continue; }
            return "does not exist: " + abs;
        }
        if (last) return "already exists on " + where + ": " + a.to;
        if (S_ISLNK(st.st_mode)) return "goes through a symlink on " + where + ": " + abs;
        if (!S_ISDIR(st.st_mode)) return "not a folder: " + abs;
        pos = s + 1;
    }
    if (planned(a.to)) return "already exists: " + a.to;
    if (a.verb == Verb::Write && a.content.size() > kWriteMax) return "too big to write: " + a.to;
    return {};
}

bool Door::reserve_trash(const std::string& path, std::int64_t when, std::string& dest_out,
                         std::string& info_out, std::string& err) {
    std::error_code ec;
    const std::string trash = trash_for(path);   // s004: a media root's own trash -- a rename on that disk
    fs::create_directories(trash + "/files", ec);
    fs::create_directories(trash + "/info", ec);
    if (trash != m_trash) ::chmod(trash.c_str(), 0700);
    const std::string base = fs::path(path).filename().string();
    // Reserve a name by creating its .trashinfo exclusively (the spec's way).
    for (int n = 1; n < 10000; ++n) {
        const std::string name = n == 1 ? base : base + "." + std::to_string(n);
        const std::string info = trash + "/info/" + name + ".trashinfo";
        const std::string dest = trash + "/files/" + name;
        struct stat st{};
        if (::lstat(dest.c_str(), &st) == 0) continue;
        const int fd = ::open(info.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) { if (errno == EEXIST) continue; err = std::strerror(errno); return false; }
        const std::string body = trashinfo(path, when);
        const bool wrote = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size()) && ::fsync(fd) == 0;
        ::close(fd);
        if (!wrote) { ::unlink(info.c_str()); err = "could not write the trash info"; return false; }
        dest_out = dest;
        info_out = info;
        return true;
    }
    err = "no free name in the Trash";
    return false;
}

// Share `to`'s blocks with `from`'s. The kernel locks both, compares the bytes,
// and shares only what is equal -- if anything differs it reports DIFFERS and
// that range is left exactly as it was. Opened read-only: the owner may dedupe.
bool Door::do_dedupe(Record& r) {
    const int src = ::open(r.from.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const int dst = ::open(r.to.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    const auto close_both = [&] { if (src >= 0) ::close(src); if (dst >= 0) ::close(dst); };
    if (src < 0 || dst < 0) { r.error = std::strerror(errno); close_both(); return false; }
    struct stat st{};
    ::fstat(src, &st);
    const std::uint64_t size = static_cast<std::uint64_t>(st.st_size);
    constexpr std::uint64_t kChunk = 16ull << 20;   // btrfs takes up to 16 MiB per call
    std::vector<char> buf(sizeof(file_dedupe_range) + sizeof(file_dedupe_range_info));
    auto* range = reinterpret_cast<file_dedupe_range*>(buf.data());
    for (std::uint64_t off = 0; off < size;) {
        std::memset(buf.data(), 0, buf.size());
        range->src_offset = off;
        range->src_length = std::min(kChunk, size - off);
        range->dest_count = 1;
        range->info[0].dest_fd = dst;
        range->info[0].dest_offset = off;
        if (::ioctl(src, FIDEDUPERANGE, range) != 0) {
            r.error = errno == EOPNOTSUPP || errno == EINVAL || errno == ENOTTY
                          ? "this filesystem can't share blocks" : std::string(std::strerror(errno));
            close_both();
            return false;
        }
        if (range->info[0].status == FILE_DEDUPE_RANGE_DIFFERS) {
            r.error = "the contents differ now -- nothing more was shared";
            close_both();
            return false;
        }
        if (range->info[0].status < 0) {
            r.error = std::strerror(-range->info[0].status);
            close_both();
            return false;
        }
        if (range->info[0].bytes_deduped == 0) { r.error = "the filesystem shared nothing"; close_both(); return false; }
        off += range->info[0].bytes_deduped;
    }
    close_both();
    return true;
}

bool Door::do_trash(Record& r) {
    return reserve_trash(r.from, r.when, r.to, r.info, r.error);
}

bool Door::trash_now(const std::string& path, std::string& dest, std::string& err) {
    std::string info;
    if (!reserve_trash(path, now_s(), dest, info, err)) return false;
    if (rename_noreplace(path, dest) != 0) {
        err = std::strerror(errno);
        ::unlink(info.c_str());
        return false;
    }
    return true;
}

// Write / Clone build in a staging name the door owns -- `.rouster-<name>.<id>`
// beside the target -- recorded as r.from. run() then renames it into place.
// A staging clone that fails goes to the Trash, never rm -r (§1.1).
bool Door::do_create(Record& r, const Action& a) {
    const fs::path to(r.to);
    r.from = (to.parent_path() / (staging_prefix() + to.filename().string() + "." + std::to_string(r.id))).string();
    struct stat st{};
    if (::lstat(r.from.c_str(), &st) == 0) { r.error = "staging name taken: " + r.from; return false; }
    if (r.verb == Verb::MakeDir) {
        if (::mkdir(r.from.c_str(), 0755) != 0) { r.error = std::strerror(errno); return false; }
    } else if (r.verb == Verb::Write) {
        const int fd = ::open(r.from.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, a.mode & 0700 ? a.mode & 0777 : 0644);
        if (fd < 0) { r.error = std::strerror(errno); return false; }
        const bool wrote = ::write(fd, a.content.data(), a.content.size()) == static_cast<ssize_t>(a.content.size()) &&
                           ::fsync(fd) == 0;
        ::close(fd);
        if (!wrote) { r.error = "could not write " + r.from; ::unlink(r.from.c_str()); return false; }
    } else if (r.verb == Verb::Copy) {
        // A reflink where the filesystem can (btrfs: instant, shares blocks),
        // else the bytes. Into the staging name; the source is opened read-only.
        const int in = ::open(r.src.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (in < 0) { r.error = std::strerror(errno); return false; }
        struct stat ss{};
        if (::fstat(in, &ss) != 0 || !S_ISREG(ss.st_mode)) { ::close(in); r.error = "not a file: " + r.src; return false; }
        const int out = ::open(r.from.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, ss.st_mode & 0777);
        if (out < 0) { r.error = std::strerror(errno); ::close(in); return false; }
        bool ok = ::ioctl(out, FICLONE, in) == 0;
        if (!ok) {
            ok = true;
            std::vector<char> buf(1 << 20);
            for (;;) {
                const ssize_t n = ::read(in, buf.data(), buf.size());
                if (n < 0) { if (errno == EINTR) continue; ok = false; break; }
                if (n == 0) break;
                for (ssize_t off = 0; off < n;) {
                    const ssize_t w = ::write(out, buf.data() + off, static_cast<std::size_t>(n - off));
                    if (w < 0) { if (errno == EINTR) continue; ok = false; break; }
                    off += w;
                }
                if (!ok) break;
            }
        }
        ok = ok && ::fsync(out) == 0;
        ::close(out);
        ::close(in);
        if (!ok) { r.error = "could not copy " + r.src; ::unlink(r.from.c_str()); return false; }
    } else {
        const std::string parent = to.parent_path().string();
        const auto fail = [&](const std::string& why) {
            r.error = why;
            std::string dest, err;
            if (::lstat(r.from.c_str(), &st) == 0) trash_now(r.from, dest, err);
            return false;
        };
        const auto c = vcs::git(parent, {"clone", "-q", "--", a.url, r.from}, 900);
        if (c.code != 0)
            return fail("git clone failed: " + (c.timed_out ? std::string("no answer in 15 minutes") : c.err.substr(0, c.err.find('\n'))));
        if (!a.branch.empty()) {
            const auto cur = vcs::git(r.from, {"symbolic-ref", "-q", "--short", "HEAD"}, 20);
            if (cur.out.substr(0, cur.out.find('\n')) != a.branch &&
                vcs::git(r.from, {"checkout", "-q", a.branch}, 60).code != 0)
                return fail("the clone has no branch " + a.branch);
        }
        if (!a.commit.empty() && vcs::git(r.from, {"merge-base", "--is-ancestor", a.commit, "HEAD"}, 20).code != 0)
            return fail("commit " + a.commit.substr(0, 7) + " is not in the clone's history");
    }
    if (!identity_of(r.from, r.ident)) { r.error = "staging vanished: " + r.from; return false; }
    return true;
}

Result Door::run(const Plan& plan) {
    Result res;
    if (!plan.previewed) { res.messages.push_back("refused: the plan was never previewed"); return res; }
    if (plan.actions.empty()) { res.ok = true; return res; }

    // Policy first, for the WHOLE plan, before anything moves.
    {
        std::vector<std::string> made, freed;
        for (const auto& a : plan.actions) {
            if (auto why = refusal(a, made, freed); !why.empty()) res.messages.push_back("refused: " + why);
            if (a.verb == Verb::MakeDir) made.push_back(a.to);
            if (a.verb == Verb::Trash) freed.push_back(a.from);   // a later create may take its place (s012b restore)
        }
    }
    if (!res.messages.empty()) return res;

    Batch b{m_next_batch++, plan.label, now_s(), {}, false};
    if (!append(json{{"t", "batch"}, {"batch", b.id}, {"label", b.label}, {"when", b.when}}.dump())) {
        res.messages.push_back("could not write the manifest " + m_manifest + " -- nothing done");
        return res;
    }
    m_batches.push_back(b);
    Batch& batch = m_batches.back();
    res.batch = batch.id;

    bool root_refused = false;   // s013b: once polkit says no, the plan's other root steps don't ask again
    for (const auto& a : plan.actions) {
        Record r;
        r.id = m_next_id++;
        r.batch = batch.id;
        r.verb = a.verb;
        r.from = a.from;
        r.to = a.to;
        if (r.verb == Verb::Copy) r.src = a.from;
        r.when = now_s();
        if (r.verb == Verb::Settings) { if (!prep_settings(r, a)) { /* r.error set */ } }
        else if (r.verb == Verb::Root) { r.to = a.content; if (root_refused) r.error = "skipped -- not authorised"; }
        else if (r.verb == Verb::Tag) { if (!prep_tag(r, a)) { /* r.error set */ } }
        else if (creates(r.verb)) { if (!do_create(r, a)) { /* r.error set */ } }
        else if (r.verb == Verb::Dedupe) { if (!identity_of(r.to, r.ident)) r.error = "vanished before it could be shared"; }
        else if (!identity_of(r.from, r.ident)) { r.error = "vanished before it could be moved"; }
        else if (r.verb == Verb::Trash && !do_trash(r)) { /* r.error set */ }

        if (r.error.empty()) {
            // Write-ahead: the intent is on disk before the rename happens.
            json ij{{"t", "intent"}, {"id", r.id}, {"batch", r.batch}, {"verb", verb_name(r.verb)},
                    {"from", r.from}, {"to", r.to}, {"info", r.info}, {"src", r.src}, {"when", r.when},
                    {"ident", json::parse(ident_json(r.ident))}};
            if (r.verb == Verb::Tag) {
                ij["attr"] = r.attr;
                ij["had"] = r.had;
                ij["untag"] = r.untag;
                ij["before"] = hex_of(r.before);
                ij["after"] = hex_of(r.after);
            }
            if (!append(ij.dump())) {
                if (!r.info.empty()) ::unlink(r.info.c_str());
                res.messages.push_back("could not write the manifest -- stopped before " + r.from);
                break;
            }
            m_records.push_back(r);
            batch.records.push_back(r.id);
            Record& live = m_records.back();
            const bool acted = live.verb == Verb::Dedupe     ? do_dedupe(live)
                             : live.verb == Verb::Settings ? do_settings(live)
                             : live.verb == Verb::Root     ? do_root(live)
                             : live.verb == Verb::Tag      ? do_tag(live)
                                                           : rename_noreplace(live.from, live.to) == 0;
            if (acted) {
                live.state = State::Done;
                json dj{{"t", "done"}, {"id", live.id}};
                if (!live.note.empty()) dj["note"] = live.note;
                append(dj.dump());
                ++res.done;
            } else {
                if (live.verb != Verb::Dedupe && live.verb != Verb::Settings && live.verb != Verb::Root &&
                    live.verb != Verb::Tag)
                    live.error = errno == EXDEV ? "on another filesystem -- " + g_app + " never copies-and-deletes"
                                                : std::string(std::strerror(errno));
                if (!live.info.empty()) ::unlink(live.info.c_str());
                if (creates(live.verb)) { std::string d, e; trash_now(live.from, d, e); }   // the staging copy
                if (live.verb == Verb::Root && live.error.rfind("not authorised", 0) == 0) root_refused = true;
                live.state = State::Failed;
                append(json{{"t", "failed"}, {"id", live.id}, {"error", live.error}}.dump());
                ++res.failed;
                res.messages.push_back("failed: " + (live.verb == Verb::Settings ? std::string("GNOME settings")
                                                     : live.verb == Verb::Root ? "root " + live.to
                                                     : creates(live.verb) || live.verb == Verb::Dedupe ? live.to : live.from) +
                                       " -- " + live.error);
            }
        } else {
            ++res.failed;
            res.messages.push_back("failed: " + (r.verb == Verb::Settings ? std::string("GNOME settings")
                                                 : r.verb == Verb::Root ? "root " + r.to
                                                 : creates(r.verb) ? r.to : r.from) + " -- " + r.error);
        }
    }
    res.ok = res.failed == 0 && res.done == plan.actions.size();
    return res;
}

bool Door::prep_settings(Record& r, const Action& a) {
    const auto now = m_dconf({"dconf", "dump", "/"}, "");
    if (now.code != 0) {
        r.error = now.code == 127 ? "dconf is not installed" : "dconf dump failed: " + now.err.substr(0, now.err.find('\n'));
        return false;
    }
    // Rouster's own records, beside the manifest: what the settings were, and what is loaded.
    std::error_code ec;
    fs::create_directories(m_data, ec);
    r.from = m_data + "/settings-" + std::to_string(r.id) + ".before.ini";
    r.src = m_data + "/settings-" + std::to_string(r.id) + ".load.ini";
    r.to = "dconf:/";
    for (const auto& [path, text] : {std::pair{r.from, &now.out}, std::pair{r.src, &a.content}}) {
        const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd < 0) { r.error = "could not save " + path + ": " + std::strerror(errno); return false; }
        const bool ok = ::write(fd, text->data(), text->size()) == static_cast<ssize_t>(text->size()) && ::fsync(fd) == 0;
        ::close(fd);
        if (!ok) { r.error = "could not save " + path; return false; }
    }
    return true;
}

bool Door::do_settings(Record& r) {
    std::ifstream in(r.src, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const auto l = m_dconf({"dconf", "load", "/"}, ss.str());
    if (l.code != 0) { r.error = "dconf load failed: " + l.err.substr(0, l.err.find('\n')); return false; }
    return true;
}

bool Door::undo_settings(Record& r, std::string& why) {
    const auto slurp = [](const std::string& p, std::string& out) {
        std::ifstream in(p, std::ios::binary);
        if (!in) return false;
        std::ostringstream ss;
        ss << in.rdbuf();
        out = ss.str();
        return true;
    };
    std::string before, loaded;
    if (!slurp(r.from, before) || !slurp(r.src, loaded)) { why = "the saved settings are gone: " + r.from; return false; }
    const auto had = dconf_keys(before);
    for (const auto& k : dconf_keys(loaded)) {
        if (std::find(had.begin(), had.end(), k) != had.end()) continue;
        const auto x = m_dconf({"dconf", "reset", k}, "");
        if (x.code != 0) { why = "dconf reset " + k + " failed: " + x.err.substr(0, x.err.find('\n')); return false; }
    }
    if (!before.empty()) {
        const auto l = m_dconf({"dconf", "load", "/"}, before);
        if (l.code != 0) { why = "dconf load failed: " + l.err.substr(0, l.err.find('\n')); return false; }
    }
    return true;
}

bool Door::do_root(Record& r) {
    const auto x = m_root(root_argv_for(g_root_rules.helper_path, r.to, m_root_dry));
    const auto lines = g_root_rules.notes ? g_root_rules.notes(x.out) : std::vector<std::string>{};
    for (const auto& l : lines) r.note += (r.note.empty() ? "" : "; ") + l;
    if (x.code == 0 && !lines.empty()) return true;
    const auto first = [](const std::string& s) { return s.substr(0, s.find('\n')); };
    if (x.timed_out)        r.error = "the helper did not finish in time";
    else if (x.code == 126 || x.err.find("Not authorized") != std::string::npos || x.err.find("dismissed") != std::string::npos)
        r.error = "not authorised -- the password prompt was dismissed or refused; nothing ran";   // pkexec: 126 dismissed, 127 refused
    else if (x.code == 127) r.error = ::access(g_root_rules.helper_path.c_str(), X_OK) != 0 ? std::string("the root helper isn't installed -- ./install.sh --helper")
                                    : "pkexec could not run the helper" + (x.err.empty() ? std::string() : " -- " + first(x.err));
    else if (!lines.empty() && !r.note.empty()) r.error = r.note;
    else if (!x.err.empty()) r.error = first(x.err);
    else                    r.error = "the helper failed (exit " + std::to_string(x.code) + ")";
    return false;
}

bool Door::prep_tag(Record& r, const Action& a) {
    if (!identity_of(r.from, r.ident)) { r.error = "vanished before it could be tagged"; return false; }
    r.attr = a.attr;
    r.untag = a.untag;
    r.after = a.untag ? std::string() : a.content;
    r.to = "xattr:" + a.attr;
    std::string err;
    r.had = tag_of(r.from, r.attr, r.before, &err);
    if (!r.had && !err.empty()) { r.error = "could not read the tag: " + err; return false; }
    return true;
}

bool Door::do_tag(Record& r) {
    const int rc = r.untag ? ::lremovexattr(r.from.c_str(), r.attr.c_str())
                           : ::lsetxattr(r.from.c_str(), r.attr.c_str(), r.after.data(), r.after.size(), 0);
    if (rc == 0 || (r.untag && errno == ENODATA)) return true;   // untag of an absent tag: already so
    r.error = errno == ENOTSUP ? std::string("this filesystem keeps no tags (xattrs)") : std::string(std::strerror(errno));
    return false;
}

bool Door::undo_tag(Record& r, std::string& why) {
    Identity now{};
    if (!identity_of(r.from, now)) { why = "no longer at " + r.from; return false; }
    if (!(now.dev == r.ident.dev && now.ino == r.ident.ino)) { why = "something else is at " + r.from + " now"; return false; }
    std::string v;
    const bool has = tag_of(r.from, r.attr, v);
    if (r.untag ? has : (!has || v != r.after)) { why = "the tag " + r.attr + " changed since -- left as it is"; return false; }
    const int rc = r.had ? ::lsetxattr(r.from.c_str(), r.attr.c_str(), r.before.data(), r.before.size(), 0)
                         : ::lremovexattr(r.from.c_str(), r.attr.c_str());
    if (rc != 0 && !(!r.had && errno == ENODATA)) { why = std::strerror(errno); return false; }
    return true;
}

bool Door::undo_one(Record& r, std::string& why) {
    if (r.verb == Verb::Tag) return undo_tag(r, why);
    if (r.verb == Verb::Root) return true;   // nothing moved: the step is closed (a snapshot isn't taken back)
    if (r.verb == Verb::Settings) return undo_settings(r, why);
    Identity now{};
    if (!identity_of(r.to, now)) { why = "no longer at " + r.to; return false; }
    if (r.verb == Verb::Dedupe) {   // nothing moved: the copy must still be the copy, then the step is closed
        if (!(now.dev == r.ident.dev && now.ino == r.ident.ino)) { why = "something else is at " + r.to + " now"; return false; }
        return true;
    }
    if (creates(r.verb)) {   // nothing was there before: what was made goes to the Trash
        if (!(now.dev == r.ident.dev && now.ino == r.ident.ino && now.dir == r.ident.dir)) {
            why = "something else is at " + r.to + " now";
            return false;
        }
        if (r.verb == Verb::MakeDir) {   // only an empty folder goes; anything put in it since stays put
            std::error_code ec;
            if (!fs::is_empty(r.to, ec) || ec) { why = "something is in " + r.to + " now -- left in place"; return false; }
        }
        std::string dest;
        if (!trash_now(r.to, dest, why)) return false;
        r.info = dest;
        return true;
    }
    if (!(now == r.ident)) {
        // A directory's mtime/size change when something inside it changes;
        // what must match is that it is the same object.
        if (!(now.dev == r.ident.dev && now.ino == r.ident.ino && now.dir == r.ident.dir)) {
            why = "something else is at " + r.to + " now";
            return false;
        }
    }
    struct stat st{};
    const std::string parent = fs::path(r.from).parent_path().string();
    if (::stat(parent.c_str(), &st) != 0) { why = "its folder is gone: " + parent; return false; }
    if (rename_noreplace(r.to, r.from) != 0) {
        why = errno == EEXIST ? "something new is at " + r.from : std::string(std::strerror(errno));
        return false;
    }
    if (!r.info.empty()) ::unlink(r.info.c_str());
    return true;
}

bool Door::can_undo() const {
    for (auto it = m_batches.rbegin(); it != m_batches.rend(); ++it)
        if (!it->undone) return true;
    return false;
}

std::string Door::undo_label() const {
    for (auto it = m_batches.rbegin(); it != m_batches.rend(); ++it)
        if (!it->undone) return it->label;
    return {};
}

Result Door::undo() {
    Result res;
    Batch* b = nullptr;
    for (auto it = m_batches.rbegin(); it != m_batches.rend(); ++it)
        if (!it->undone) { b = &*it; break; }
    if (!b) { res.messages.push_back("nothing to undo"); return res; }
    res.batch = b->id;
    std::size_t todo = 0;
    for (auto it = b->records.rbegin(); it != b->records.rend(); ++it) {
        Record* r = find(*it);
        if (!r || r->state != State::Done) continue;
        ++todo;
        append(json{{"t", "undo-intent"}, {"id", r->id}}.dump());
        std::string why;
        if (undo_one(*r, why)) {
            r->state = State::Undone;
            append(json{{"t", "undone"}, {"id", r->id}}.dump());
            ++res.done;
        } else {
            append(json{{"t", "undo-failed"}, {"id", r->id}, {"error", why}}.dump());
            ++res.failed;
            res.messages.push_back("could not put back " + r->from + " -- " + why);
        }
    }
    // The batch is closed even if an item could not go back: the failure is
    // in the manifest and the message; retrying blindly would not fix it.
    b->undone = true;
    append(json{{"t", "batch-undone"}, {"batch", b->id}}.dump());
    res.ok = res.failed == 0 && res.done == todo;
    return res;
}

// ── Replace WITHOUT undo (latch s003) ────────────────────────────────────────
std::string Door::replace_refusal(const std::string& path, const std::string& content) const {
    const std::string name = fs::path(path).filename().string();
    const OwnFile* own = nullptr;
    for (const auto& o : g_own)
        if (o.name == name) own = &o;
    if (!own) return "not one of " + g_app + "'s own files: " + name;
    // The place: as a Tag would be judged (clean path in home, nothing protected
    // or symlinked on the way, not the Trash, not our records, no symlink, exists).
    Action probe;
    probe.verb = Verb::Tag;
    probe.from = path;
    probe.attr = std::string(kTagPrefix) + "replace";
    probe.content = "x";
    if (auto why = refusal(probe); !why.empty()) return why;
    struct stat st{};
    if (::lstat(path.c_str(), &st) != 0) return "does not exist (a replace never creates): " + path;
    if (!S_ISREG(st.st_mode)) return "not a regular file: " + path;
    if (static_cast<std::size_t>(st.st_size) > kOwnMax) return "too big to be " + g_app + "'s: " + path;
    std::ifstream in(path, std::ios::binary);
    std::string first;
    std::getline(in, first);
    if (first.rfind(own->header, 0) != 0) return "its first line is not " + g_app + "'s header -- not ours: " + path;
    if (content.size() > kOwnMax) return "the new text is too big: " + path;
    if (content.rfind(own->header, 0) != 0) return "the new text does not start with the header: " + path;
    return {};
}

std::string Door::replace_own(const std::string& path, const std::string& content) {
    if (auto why = replace_refusal(path, content); !why.empty()) return why;
    const fs::path p(path);
    const std::string tmp = (p.parent_path() / (staging_prefix() + p.filename().string().substr(1) + ".new")).string();
    struct stat st{};
    if (::lstat(tmp.c_str(), &st) == 0) return "tmp name taken: " + tmp;
    struct stat old{};
    ::lstat(path.c_str(), &old);
    const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, old.st_mode & 0777);
    if (fd < 0) return std::string("could not write ") + tmp + ": " + std::strerror(errno);
    const bool wrote = ::write(fd, content.data(), content.size()) == static_cast<ssize_t>(content.size()) && ::fsync(fd) == 0;
    ::close(fd);
    if (!wrote) { ::unlink(tmp.c_str()); return "could not write " + tmp; }
    if (::rename(tmp.c_str(), path.c_str()) != 0) {
        const std::string why = std::strerror(errno);
        ::unlink(tmp.c_str());
        return "could not put it in place: " + why;
    }
    return {};
}

}  // namespace latch::door
