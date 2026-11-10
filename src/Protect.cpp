#include "latch/Protect.hpp"

#include "json.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <fstream>
#include <memory>

namespace latch::protect {

const std::vector<std::string>& never_touch() {
    static const std::vector<std::string> list = {
        ".local/share/keyrings",
        ".config/dconf",
        ".ssh",
        ".gnupg",
        ".local/share/gnome-shell/extensions",
        ".local/share/flatpak",
    };
    return list;
}

namespace {
// std::atomic<shared_ptr> (C++20): the free functions on shared_ptr are deprecated (GCC 16).
std::atomic<std::shared_ptr<const std::vector<std::string>>> g_owned{std::make_shared<const std::vector<std::string>>()};
}  // namespace

void set_owned(std::vector<std::string> rels) {
    g_owned.store(std::make_shared<const std::vector<std::string>>(std::move(rels)));
}
std::vector<std::string> owned() { return *g_owned.load(); }

bool protected_name(std::string_view n) {
    static constexpr std::string_view ext = ".samson";
    if (n.size() <= ext.size()) return false;
    for (std::size_t i = 0; i < ext.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(n[n.size() - ext.size() + i])) != ext[i]) return false;
    return true;
}

std::vector<std::string> samson_places(const std::string& home, const std::string& config_home) {
    std::string samson, catalog, second;
    {
        std::ifstream in(config_home + "/samson/settings.json");
        if (in) {
            const auto j = nlohmann::json::parse(in, nullptr, false);
            const auto get = [&](const char* k, std::string& out) {
                if (j.is_object() && j.contains(k) && j[k].is_string()) out = j[k].get<std::string>();
            };
            get("samson_dir", samson);
            get("catalog_dir", catalog);
            get("second_home", second);
        }
    }
    if (samson.empty()) samson = home + "/Samson";
    if (catalog.empty()) catalog = home + "/.local/share/samson";
    std::vector<std::string> out;
    for (std::string p : {samson, catalog, second}) {
        if (p.empty()) continue;
        if (p.rfind("~/", 0) == 0) p = home + p.substr(1);
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        if (p.size() <= home.size() + 1 || p.compare(0, home.size() + 1, home + "/") != 0) continue;
        const std::string rel = p.substr(home.size() + 1);
        if (rel.find("/../") != std::string::npos || rel.rfind("../", 0) == 0 || rel == "..") continue;
        out.push_back(rel);
    }
    return out;
}

bool here(const std::string& rel) {
    const auto s = rel.rfind('/');
    const std::string base = s == std::string::npos ? rel : rel.substr(s + 1);
    const auto& nt = never_touch();
    if (std::find(nt.begin(), nt.end(), rel) != nt.end() || base == "fixes") return true;
    const auto own = g_owned.load();
    return std::find(own->begin(), own->end(), rel) != own->end() || protected_name(base);
}

}  // namespace latch::protect
