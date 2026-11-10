#pragma once
#include <string>
#include <string_view>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Protect -- what no app of ours ever touches (Rouster REQUIREMENTS §1.5; lifted
// out of Rouster's scan into latch, 2026-11-10, so Rouster and Tidy refuse the
// same things). Code, not configuration. The door refuses anything here, at
// any depth; a scan classifies it Protected.
// ─────────────────────────────────────────────────────────────────────────────
namespace latch::protect {

// The never-touch list, relative to home. Any directory named `fixes` is
// protected as well, wherever it is.
const std::vector<std::string>& never_touch();

// Places ANOTHER of our apps owns, protected exactly like the never-touch list
// (inherited all the way down): Samson's folder, its second home and its
// catalog. Read from <config_home>/samson/settings.json (samson_dir,
// catalog_dir, second_home; "" = its defaults ~/Samson,
// ~/.local/share/samson). Relative to home; places outside home are left out.
std::vector<std::string> samson_places(const std::string& home, const std::string& config_home);
// Set once at startup (the app; the selftest).
void set_owned(std::vector<std::string> rels);
std::vector<std::string> owned();

// A name never moved, wherever it is: a Samson container ("*.samson").
bool protected_name(std::string_view name);

// Is the directory at `rel` (relative to home) itself a protected root? --
// on the never-touch list, named `fixes`, an owned place, or a protected name.
// Protection is inherited: a caller walking down from home stops at the first
// component for which this is true.
bool here(const std::string& rel);

}  // namespace latch::protect
