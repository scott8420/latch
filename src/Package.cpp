#include "latch/Package.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <memory>
#include <atomic>

#include "json.hpp"

#include <dirent.h>
#include <sys/stat.h>

// Package.cpp -- READ-ONLY: names, opendir/readdir/lstat. Nothing here opens
// a file's contents ("never looked inside") or changes the disk.

namespace latch::pkg {

const std::vector<std::string>& kinds() {
    static const std::vector<std::string> v = {
        ".scriv", ".scrivtemplate", ".pages", ".numbers", ".key", ".rtfd", ".app", ".photoslibrary",
        // s017: our own apps' bundles are folders too (read from their code): Curvz, Folio (v5), jot, Optik.
        ".curvz", ".folio", ".jots", ".optik",
    };
    return v;
}

namespace {
// std::atomic<shared_ptr> (C++20): the free functions on shared_ptr are deprecated (GCC 16).
std::atomic<std::shared_ptr<const std::vector<std::string>>> g_extra{std::make_shared<const std::vector<std::string>>()};
}  // namespace

std::vector<std::string> load_extra(const std::string& data_dir) {
    std::vector<std::string> out;
    std::ifstream in(data_dir + "/bundles.json");
    if (!in) return out;
    const auto j = nlohmann::json::parse(in, nullptr, false);
    if (!j.is_object() || !j.contains("bundles") || !j["bundles"].is_array()) return out;
    for (const auto& e : j["bundles"]) {
        if (!e.is_string()) continue;
        std::string x = e.get<std::string>();
        for (char& c : x) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (!x.empty() && x[0] != '.') x = "." + x;
        if (x.size() < 2 || x.find('/') != std::string::npos || x.find('.', 1) != std::string::npos) continue;
        if (std::find(out.begin(), out.end(), x) == out.end()) out.push_back(x);
    }
    return out;
}
void set_extra(std::vector<std::string> exts) {
    g_extra.store(std::make_shared<const std::vector<std::string>>(std::move(exts)));
}
std::vector<std::string> extra() { return *g_extra.load(); }

bool is_package_name(std::string_view name) {
    const auto more = g_extra.load();
    for (const auto* list : {&kinds(), more.get()})
    for (const auto& k : *list) {
        if (name.size() <= k.size()) continue;   // something before the dot
        const auto tail = name.substr(name.size() - k.size());
        bool same = true;
        for (std::size_t i = 0; i < k.size() && same; ++i)
            same = std::tolower(static_cast<unsigned char>(tail[i])) == k[i];
        if (same) return true;
    }
    return false;
}

namespace {
// The prefix of `path` through its first component that is a package's name,
// considering components [0, n) where n excludes the last unless `last`.
std::string first_package(std::string_view path, bool last) {
    std::size_t pos = path.size() && path[0] == '/' ? 1 : 0;
    while (pos < path.size()) {
        const auto s = path.find('/', pos);
        const bool is_last = s == std::string_view::npos;
        if (is_last && !last) break;
        const auto part = path.substr(pos, is_last ? std::string_view::npos : s - pos);
        if (is_package_name(part)) return std::string(path.substr(0, is_last ? path.size() : s));
        if (is_last) break;
        pos = s + 1;
    }
    return {};
}
}  // namespace

std::string enclosing(std::string_view path) { return first_package(path, false); }

std::string package_of(std::string_view path, bool last_is_dir) { return first_package(path, last_is_dir); }

namespace {
void walk(const std::string& abs, dev_t dev, int depth, Measure& m) {
    if (depth > 64) { ++m.errors; return; }
    DIR* d = ::opendir(abs.c_str());
    if (!d) { ++m.errors; return; }
    while (dirent* e = ::readdir(d)) {
        if (!std::strcmp(e->d_name, ".") || !std::strcmp(e->d_name, "..")) continue;
        const std::string c = abs + "/" + e->d_name;
        struct stat st{};
        if (::lstat(c.c_str(), &st) != 0) { ++m.errors; continue; }
        m.bytes += static_cast<std::uint64_t>(st.st_blocks) * 512u;
        m.newest = std::max<std::int64_t>(m.newest, st.st_mtim.tv_sec);
        if (S_ISDIR(st.st_mode)) {
            if (st.st_dev == dev) walk(c, dev, depth + 1, m);
        } else {
            ++m.files;
        }
    }
    ::closedir(d);
}
}  // namespace

Measure measure(const std::string& abs) {
    Measure m;
    struct stat st{};
    if (::lstat(abs.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) { ++m.errors; return m; }
    m.bytes = static_cast<std::uint64_t>(st.st_blocks) * 512u;
    m.newest = st.st_mtim.tv_sec;
    walk(abs, st.st_dev, 0, m);
    return m;
}

}  // namespace latch::pkg
