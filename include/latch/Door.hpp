#pragma once
#include "latch/Vcs.hpp"   // vcs::Run (the Settings verb's dconf runner)

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Door -- THE ONE DOOR (s004, REQUIREMENTS §1.1 / §6, RULES "Undo from the
// first write"). latch: shared by Rouster and Tidy since 2026-11-10 (the
// history below is Rouster's, where it was built). Every change an app of ours
// makes to a user's files goes through
// Door::run. Nothing else in the code base renames, unlinks or writes a user
// file, and the selftest's source gate fails if anything does.
//
// The shape (ARCHITECTURE "the plan is data"):
//
//   module -> Plan{ label, actions[], previewed }      (a module never acts)
//   UI shows the plan; the user unchecks what they don't want; previewed=true
//   Door::run(plan):  policy check, every action, BEFORE anything moves
//                     batch line -> for each action:
//                       intent line (fsync) -> the rename -> done / failed line
//   Door::undo():     the last batch, backwards; each item is checked to be
//                     the SAME file (dev + inode) before it is moved back
//
// Two pictures, not an inverse (docs/undo.md): an action records where the
// thing was (from) and where it went (to) plus its identity; undo moves it
// from `to` back to `from`. No per-verb undo code beyond the Trash's info file.
//
// What the door refuses (policy lives HERE, not in modules -- a module bug
// cannot get past it):
//   * a plan that was not previewed;
//   * anything outside home, or home itself;
//   * anything Protected (scan::never_touch(), any fixes/), at any depth;
//   * a path with a symlinked parent (the rename would land somewhere else);
//   * Rouster's own records (the data dir holding the manifest);
//   * Trashing what is already in the Trash; Moving into the Trash;
//   * a Move onto an existing path, or into a missing folder;
//   * crossing filesystems: the rename fails with EXDEV and nothing is copied.
//
// Two verbs CREATE (s007b, retire / restore): Write (a small text file) and
// Clone (git clone). Each builds in a hidden staging name beside the target
// that the door owns, then ONE rename puts it in place -- never half there.
// Their undo is not a rename back (there was nothing before): the created
// thing goes to the Trash, after the same identity check.
//
// MakeDir (s009, Archive) makes one empty folder the same way (staging name,
// one rename). A plan may make a folder and then move into it: the policy
// check reads the plan in order, so a later Move may name a folder an earlier
// MakeDir makes. Its undo sends the folder to the Trash -- only while it is
// still empty (undo runs newest first, so the moves have gone back by then).
//
// One verb MOVES NOTHING (s008, Duplicates): Dedupe shares the blocks of `to`
// with `from` (FIDEDUPERANGE -- the kernel compares the bytes itself, under
// lock, and shares nothing if they differ; a stale hash can't do harm). Both
// paths stay, both contents stay; only free space changes. Its undo puts no
// bytes anywhere: the files are and were identical, so the step is closed
// with nothing to move (re-splitting blocks is not a way back, it's a copy).
//
// Copy (s010, new version / templates) makes a new file `to` from the file
// `from` -- a reflink (FICLONE) where the filesystem shares blocks, else the
// bytes -- into a staging name, then one rename. The source is untouched. Its
// undo is a create's: the copy goes to the Trash.
//
// One verb changes SETTINGS, not files (s012d, profile restore, Q11b):
// Settings loads a dconf dump (`content`) with `dconf load /`. Before it does,
// the door saves `dconf dump /` -- what the settings are now -- and the text
// it is about to load into Rouster's records (settings-<id>.before.ini /
// .load.ini, beside the manifest; r.from / r.src). Its undo resets every key
// the load named that the saved dump didn't have, then loads the saved dump:
// the settings are exactly as before. dconf goes through its own service --
// ~/.config/dconf (never-touch) is never opened. A crash between intent and
// outcome can't be looked at; it counts as done (Undo is safe either way).
//
// One verb runs as ROOT (s013, REQUIREMENTS §1.4): Root hands ONE request
// word (`content`, e.g. "snapshot:5" -- core::helper::parse decides what is
// one) to Rouster's helper through pkexec. No path, no command crosses; the
// helper checks again for itself. What it said is kept as the record's
// `note` ("took /home/.rouster-snapshots/home-..."). Its undo moves nothing
// and closes the step: a snapshot isn't taken back (Rouster's own expire
// after the newest few). A crash between intent and outcome can't be looked
// at; it counts as done, and Activity says it may or may not have happened.
// With a fixture home (ROUSTER_HOME) the helper is asked for a --dry-run.
//
// One verb changes a TAG, not a file's bytes or place (latch s002, for Tidy):
// Tag sets (or, with `untag`, removes) ONE extended attribute on `from`. Only
// the `user.tidy.` namespace -- any other name is refused, so the door cannot
// touch another program's xattrs (user.xdg.origin.url, security.*, ...). The
// value is text (no NUL, <= kTagMax). Before it acts the door reads what the
// tag is now (absent, or its value) and journals it with the new value and
// the file's identity. Undo checks the SAME file (dev + inode) still carries
// the value the door wrote, then puts the old value back (or removes the tag
// if there was none). A symlink is never tagged (l*xattr: never followed).
// A crash between intent and outcome is resolved by looking at the tag.
//
// Hard-delete of a user's file does not exist here, and will not (§1.1).
// ─────────────────────────────────────────────────────────────────────────────
namespace latch::door {

enum class Verb { Trash, Move, Write, Clone, Dedupe, MakeDir, Copy, Settings, Root, Tag };
const char* verb_name(Verb v);   // "trash", "move", "write", "clone", "dedupe", "mkdir", "copy", "settings", "root", "tag"
bool creates(Verb v);            // Write, Clone, MakeDir, Copy: `to` is made (Copy: from `from`; the others have no source)
// THE VERB TABLE the selftest gate walks: a verb added to the enum and not
// here fails the gate's count; every verb here is run through the door,
// checked to be one step, and undone back to the exact tree.
const std::vector<Verb>& verbs();
inline constexpr int kVerbCount = 10;
inline constexpr std::size_t kTagMax = 1024;            // Tag: a value is a word, a UUID, a purpose's name
inline constexpr const char* kTagPrefix = "user.tidy.";  // Tag: the only namespace the door will set or clear
inline constexpr std::size_t kWriteMax = 4 * 1024 * 1024;   // Write is for text Rouster made (RESTORE.txt, a dconf dump): 4 MiB (64 KB until s012)

struct Action {
    Verb        verb = Verb::Trash;
    std::string from;            // absolute path
    std::string to;              // Move: absolute destination. Trash: filled in by run().
                                 // Write / Clone: the path to create (must not exist).
    std::string reason;          // why the module proposed it (shown in preview)
    std::uint64_t bytes = 0;     // for the preview
    std::string content;         // Write: the file's text (<= kWriteMax). Settings: the dconf dump to load.
                                 // Root: the helper request ("snapshot:5").
    std::uint32_t mode = 0644;   // Write: its permissions (s012c: secrets.tar.gpg is 0600)
    std::string url;             // Clone: what to clone
    std::string branch;          // Clone: check this branch out ("" = the remote's default)
    std::string commit;          // Clone: must be this commit or an ancestor of HEAD ("" = any)
    // Dedupe: `from` is the keeper (its blocks), `to` the copy that will share them.
    std::string attr;            // Tag: the xattr name, "user.tidy.<key>"; `content` is its value
    bool        untag = false;   // Tag: remove `attr` instead of setting it
};

// Tag: "" if `name` is a tag the door may set ("user.tidy." + [a-z0-9._-]+); else why not.
std::string tag_name_problem(const std::string& name);
// Tag: the value of xattr `name` on `path` (never following a symlink).
// false = absent (or unreadable: `err` says which).
bool tag_of(const std::string& path, const std::string& name, std::string& value, std::string* err = nullptr);

// A dconf dump's keys, as full paths ("/org/gnome/desktop/interface/color-scheme").
std::vector<std::string> dconf_keys(const std::string& ini);
// "" if `ini` reads as a dconf dump ([section] lines, key=value lines, blanks,
// comments); else why not.
std::string dconf_problem(const std::string& ini);

// How the door runs dconf (argv, stdin). Default: vcs::feed. The selftest
// passes an in-memory dconf.
using DconfRunner = std::function<vcs::Run(const std::vector<std::string>& argv, const std::string& in)>;

// The argv for dconf against a FIXTURE's own database (ROUSTER_HOME): dconf
// with XDG_CONFIG_HOME=<home>/.config, inside `dbus-run-session` so the dconf
// service that writes is a private one started with that setting -- never the
// desktop's. RULES: fixtures, never the real home. (s012d)
std::vector<std::string> fixture_dconf_argv(const std::string& home, const std::vector<std::string>& argv);
DconfRunner fixture_dconf(const std::string& home);

// How the door runs the root helper (argv; no stdin). Default: vcs::child,
// 15 minutes (pkexec waits on the password prompt). The selftest passes a fake.
using RootRunner = std::function<vcs::Run(const std::vector<std::string>& argv)>;

// latch (2026-11-10): the Root verb's rules belong to the APP that has a root
// helper (Rouster); the door only carries them. Set once at startup. With none
// set, every Root action is refused -- an app without a helper (Tidy) never
// runs one.
struct RootRules {
    std::string helper_path;                                        // where the polkit action says it lives
    std::function<bool(const std::string&)> known;                  // is `request` one the helper knows?
    std::function<std::vector<std::string>(const std::string&)> notes;   // what the helper said, one per line ("" = none)
};
// The app's name, said in refusals ("Tidy's own records") and in the door's
// staging names (".tidy-<name>.<id>"). Set once at startup; default "the app".
void set_app_name(std::string name);
const std::string& app_name();

// ── Replace WITHOUT undo (latch s003, for Tidy's own dot files) ──────────────
// Scott (Tidy s011): "a special condition in latch to del". An app's OWN
// bookkeeping files that live in the user's folders (Tidy: .tidy-files,
// .tidy-project) are rewritten in place: the new text beside it (a hidden tmp
// name), fsync, ONE rename over the old -- the old is gone, never half there.
// No Trash, no staging, no manifest record, no undo: the app rebuilds them
// from its own records. NOT a Verb -- it never sits in a batch Undo walks, and
// the verb table stays as it is.
// Guards (every one, or refused): a registered exact file name; a clean path
// in home, nothing protected or symlinked on the way, not the Trash, not the
// app's records; the file EXISTS (a rewrite, never a create or a bare delete),
// is a regular file (no symlink), <= kOwnMax, and its first line starts with
// the registered header -- a user's file of that name is refused. The new
// text must start with the header too and be <= kOwnMax.
struct OwnFile {
    std::string name;     // ".tidy-files"
    std::string header;   // what its first line starts with: "# Tidy file ids"
};
inline constexpr std::size_t kOwnMax = 64 * 1024;
void set_own_files(std::vector<OwnFile> files);   // once at startup; none -> every replace refused
const std::vector<OwnFile>& own_files();

void set_root_rules(RootRules rules);
const RootRules& root_rules();
// {"/usr/bin/pkexec", helper_path, "run", ["--dry-run",] request}
std::vector<std::string> root_argv_for(const std::string& helper_path, const std::string& request, bool dry);

struct Plan {
    std::string         label;   // "Sweep: 3 caches" -- names the undo step
    std::vector<Action> actions;
    bool previewed = false;      // the UI sets this after showing the plan (RULES: preview first)
};

struct Identity {
    std::uint64_t dev = 0, ino = 0, size = 0;
    std::int64_t  mtime_ns = 0;
    bool          dir = false;
    bool operator==(const Identity&) const = default;
};

enum class State { Intent, Done, Failed, Undone };

struct Record {
    std::uint64_t id = 0, batch = 0;
    Verb          verb = Verb::Trash;
    std::string   from, to, info;   // info: the .trashinfo path (Trash only)
    std::string   src;              // Copy: the file copied (from is the staging name)
    Identity      ident;
    std::int64_t  when = 0;         // unix seconds
    State         state = State::Intent;
    std::string   error;
    std::string   note;             // Root: what the helper said it did
    // Tag (r.to = "xattr:<attr>"):
    std::string   attr;
    bool          had = false;      // the tag existed before
    std::string   before, after;    // its value before; what the door set (untag: unused)
    bool          untag = false;
};

struct Batch {
    std::uint64_t id = 0;
    std::string   label;
    std::int64_t  when = 0;
    std::vector<std::uint64_t> records;
    bool          undone = false;
};

struct Result {
    bool ok = false;                       // everything asked for happened
    std::size_t done = 0, failed = 0;
    std::vector<std::string> messages;     // refusals and failures, one per line
    std::uint64_t batch = 0;
};

class Door {
public:
    // home: the user's home (canonicalised). data_dir: where the manifest lives
    // (Rouster's own folder, e.g. ~/.local/share/rouster) -- refused as a target.
    Door(const std::string& home, const std::string& data_dir);

    // Replay the manifest. An intent with no outcome (a crash mid-action) is
    // resolved by looking: the item at `to` with the recorded identity -> done,
    // still at `from` -> failed; either way it is listed in unfinished().
    bool load();

    // "" = allowed; otherwise why not. run() asks this of every action first.
    std::string refusal(const Action& a) const;
    // ...given the folders earlier MakeDirs in the same plan will make.
    std::string refusal(const Action& a, const std::vector<std::string>& made) const;
    // ...and the paths earlier Trash actions in the plan free: a create may
    // take the place of a file the same plan trashes first (s012b, restore).
    std::string refusal(const Action& a, const std::vector<std::string>& made,
                        const std::vector<std::string>& freed) const;

    Result run(const Plan& plan);
    // Replace WITHOUT undo (above). "" = may; else why not.
    std::string replace_refusal(const std::string& path, const std::string& content) const;
    // "" = replaced; else why not (refused, or the write / rename failed --
    // then the old file stays as it was).
    std::string replace_own(const std::string& path, const std::string& content);
    void set_dconf(DconfRunner r) { m_dconf = std::move(r); }
    void set_root(RootRunner r, bool dry) { m_root = std::move(r); m_root_dry = dry; }
    Result undo();                          // the last batch not yet undone
    bool        can_undo() const;
    std::string undo_label() const;

    const std::vector<Batch>&  batches() const { return m_batches; }
    const Record*              record(std::uint64_t id) const;
    const std::vector<std::string>& unfinished() const { return m_unfinished; }
    std::string manifest_path() const { return m_manifest; }
    std::string trash_dir() const { return m_trash; }

private:
    bool append(const std::string& line);   // O_APPEND + fdatasync
    bool do_trash(Record& r);
    bool reserve_trash(const std::string& path, std::int64_t when, std::string& dest,
                       std::string& info, std::string& err);   // name + .trashinfo, for `path`
    bool trash_now(const std::string& path, std::string& dest, std::string& err);   // reserve + rename
    bool do_create(Record& r, const Action& a);              // Write / Clone into staging (r.from)
    bool do_move(Record& r);
    bool do_dedupe(Record& r);                               // FIDEDUPERANGE, in chunks
    bool undo_one(Record& r, std::string& why);
    bool prep_settings(Record& r, const Action& a);          // save dump + text to load (Rouster's records)
    bool do_settings(Record& r);                             // dconf load /
    bool undo_settings(Record& r, std::string& why);         // reset the added keys, load the saved dump
    bool do_root(Record& r);                                 // pkexec rouster-helper run <request>
    bool prep_tag(Record& r, const Action& a);               // identity + what the tag is now
    bool do_tag(Record& r);                                  // lsetxattr / lremovexattr
    bool undo_tag(Record& r, std::string& why);              // same file, still our value -> put the old one back
    DconfRunner m_dconf;
    RootRunner  m_root;
    bool        m_root_dry = false;
    Record* find(std::uint64_t id);

    std::string m_home, m_data, m_manifest, m_trash;
    std::vector<Batch>  m_batches;
    std::vector<Record> m_records;
    std::vector<std::string> m_unfinished;
    std::uint64_t m_next_id = 1, m_next_batch = 1;
};

// Identity of what is at `path` now (lstat; a symlink is itself). false = gone.
bool identity_of(const std::string& path, Identity& out);

// The freedesktop .trashinfo body for `abs` deleted at unix time `when`
// (local time, percent-encoded path). Public so the selftest can check it.
std::string trashinfo(const std::string& abs, std::int64_t when);

}  // namespace latch::door
