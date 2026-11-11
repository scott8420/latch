// latch_selftest -- latch's own selftest, begun with the Tag verb (latch s002).
// Fixture home under a temp dir; never the real home. Exit 0 = all pass.
#include "latch/Door.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <sys/stat.h>
#include <sys/xattr.h>
#include <unistd.h>

namespace fs = std::filesystem;
using namespace latch::door;

static int g_pass = 0, g_fail = 0;
static void check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
    (ok ? g_pass : g_fail)++;
}

static void touch(const std::string& p, const std::string& body = "x") { std::ofstream(p) << body; }
static bool raw_set(const std::string& p, const std::string& n, const std::string& v) {
    return ::lsetxattr(p.c_str(), n.c_str(), v.data(), v.size(), 0) == 0;
}
static std::string val(const std::string& p, const std::string& n) {
    std::string v;
    return tag_of(p, n, v) ? v : std::string("<absent>");
}
static Plan plan1(Action a, bool previewed = true) {
    Plan p;
    p.label = "tag";
    p.actions.push_back(std::move(a));
    p.previewed = previewed;
    return p;
}
static Action tag(const std::string& f, const std::string& n, const std::string& v) {
    Action a;
    a.verb = Verb::Tag;
    a.from = f;
    a.attr = n;
    a.content = v;
    return a;
}
static Action untag(const std::string& f, const std::string& n) {
    Action a = tag(f, n, "");
    a.untag = true;
    return a;
}

int main() {
    char tmpl[] = "/tmp/latch-selftest-XXXXXX";
    const std::string root = ::mkdtemp(tmpl);
    const std::string home = fs::weakly_canonical(root + "/home").string();
    const std::string data = home + "/.local/share/latchtest";
    fs::create_directories(home + "/Documents");
    fs::create_directories(home + "/fixes");
    fs::create_directories(home + "/.local/share/Trash/files");
    fs::create_directories(data);
    set_app_name("latchtest");
    const std::string f = home + "/Documents/note.txt";
    const std::string d = home + "/Documents";
    touch(f);
    touch(home + "/fixes/x.txt");
    touch(home + "/.local/share/Trash/files/old.txt");
    touch(data + "/catalog.db");
    touch(root + "/outside.txt");
    fs::create_symlink(f, home + "/link.txt");

    if (!raw_set(f, "user.latchprobe", "1")) {
        std::printf("this filesystem keeps no user xattrs (%s) -- cannot test Tag\n", root.c_str());
        return 1;
    }
    ::lremovexattr(f.c_str(), "user.latchprobe");

    std::printf("-- the verb table\n");
    check(verbs().size() == static_cast<std::size_t>(kVerbCount) && kVerbCount == 10, "10 verbs, table and count agree");
    check(std::string(verb_name(Verb::Tag)) == "tag" && !creates(Verb::Tag), "tag: named, creates nothing");

    std::printf("-- tag names: user.tidy.* only\n");
    check(tag_name_problem("user.tidy.id").empty(), "user.tidy.id allowed");
    check(tag_name_problem("user.tidy.retention").empty(), "user.tidy.retention allowed");
    for (const char* bad : {"user.xdg.origin.url", "user.tidy.", "user.tidy.Retention", "user.tidy.a b",
                            "security.selinux", "trusted.tidy.x", "user.tidyx", "tidy.id"})
        check(!tag_name_problem(bad).empty(), std::string("refused: ") + bad);

    Door door(home, data);
    door.load();
    std::printf("-- refusals (nothing written)\n");
    const auto refused = [&](const Action& a, const std::string& what, bool previewed = true) {
        const auto r = door.run(plan1(a, previewed));
        check(!r.ok && r.done == 0 && !r.messages.empty() && r.messages[0].rfind("refused", 0) == 0,
              "refused: " + what + (r.messages.empty() ? "" : "  [" + r.messages[0] + "]"));
    };
    refused(tag(f, "user.tidy.id", "a"), "a plan never previewed", false);
    refused(tag(f, "user.xdg.origin.url", "http://x"), "another program's xattr");
    refused(tag(f, "user.tidy.id", std::string("a\0b", 3)), "a value with NUL");
    refused(tag(f, "user.tidy.id", std::string(kTagMax + 1, 'a')), "a value over kTagMax");
    refused(tag(home + "/link.txt", "user.tidy.id", "a"), "a symlink");
    refused(tag(home + "/Documents/missing.txt", "user.tidy.id", "a"), "a missing file");
    refused(tag(home + "/.local/share/Trash/files/old.txt", "user.tidy.id", "a"), "a file in the Trash");
    refused(tag(home + "/fixes/x.txt", "user.tidy.id", "a"), "fixes/ (Protected)");
    refused(tag(root + "/outside.txt", "user.tidy.id", "a"), "outside home");
    refused(tag(home, "user.tidy.id", "a"), "home itself");
    refused(tag(data + "/catalog.db", "user.tidy.id", "a"), "the app's own records");
    { Action a = untag(f, "user.tidy.id"); a.content = "x"; refused(a, "an untag with a value"); }
    { Action a = tag(f, "user.tidy.id", "a"); a.to = home + "/x"; refused(a, "a tag with a destination"); }
    check(val(f, "user.tidy.id") == "<absent>" && door.batches().empty(), "after the refusals: no tag, no batch");

    std::printf("-- set, undo\n");
    raw_set(f, "user.xdg.origin.url", "https://example.org/f");
    auto r = door.run(plan1(tag(f, "user.tidy.id", "8c1f-uuid")));
    check(r.ok && r.done == 1 && val(f, "user.tidy.id") == "8c1f-uuid", "new tag set");
    check(door.can_undo() && door.undo().ok && val(f, "user.tidy.id") == "<absent>", "undo: the tag is gone again");
    check(val(f, "user.xdg.origin.url") == "https://example.org/f", "another program's xattr untouched throughout");

    std::printf("-- overwrite, untag, undo\n");
    door.run(plan1(tag(f, "user.tidy.retention", "undecided")));
    r = door.run(plan1(tag(f, "user.tidy.retention", "keep")));
    check(r.ok && val(f, "user.tidy.retention") == "keep", "overwrite: undecided -> keep");
    check(door.undo().ok && val(f, "user.tidy.retention") == "undecided", "undo: back to undecided");
    r = door.run(plan1(untag(f, "user.tidy.retention")));
    check(r.ok && val(f, "user.tidy.retention") == "<absent>", "untag removes it");
    check(door.undo().ok && val(f, "user.tidy.retention") == "undecided", "undo: the untag put back");

    std::printf("-- a folder; a plan of several\n");
    {
        Plan p;
        p.label = "tag two";
        p.previewed = true;
        p.actions = {tag(d, "user.tidy.role", "input"), tag(f, "user.tidy.purpose", "taxes-2025")};
        r = door.run(p);
        check(r.ok && r.done == 2 && val(d, "user.tidy.role") == "input" && val(f, "user.tidy.purpose") == "taxes-2025",
              "folder + file tagged in one batch");
        check(door.undo().ok && val(d, "user.tidy.role") == "<absent>" && val(f, "user.tidy.purpose") == "<absent>",
              "undo takes both back");
    }

    std::printf("-- undo refuses what changed since\n");
    door.run(plan1(tag(f, "user.tidy.retention", "held")));
    raw_set(f, "user.tidy.retention", "keep");   // someone else changed it
    r = door.undo();
    check(!r.ok && val(f, "user.tidy.retention") == "keep", "tag changed since: left as it is");
    door.run(plan1(tag(f, "user.tidy.id", "v1")));
    {   // an atomic save: a new inode under the same name
        const std::string tmp = home + "/Documents/.note.tmp";
        touch(tmp, "y");
        fs::rename(tmp, f);
    }
    r = door.undo();
    check(!r.ok && !r.messages.empty() && r.messages[0].find("something else") != std::string::npos,
          "file replaced by an atomic save: undo refuses by identity");

    std::printf("-- the manifest: reload, non-UTF-8 values, a crash\n");
    raw_set(f, "user.tidy.id", std::string("\xff\xfe old", 6));
    door.run(plan1(tag(f, "user.tidy.id", "new")));
    {
        Door again(home, data);
        again.load();
        check(again.can_undo() && again.undo().ok && val(f, "user.tidy.id") == std::string("\xff\xfe old", 6),
              "reloaded door undoes; a non-UTF-8 old value comes back byte for byte");
    }
    {   // a crash: intent on disk, tag set, no outcome line
        const std::string m = data + "/manifest.jsonl";
        std::string mark;
        {
            Door c(home, data);
            c.load();
            c.run(plan1(tag(f, "user.tidy.purpose", "crash")));
        }
        std::ifstream in(m);
        std::string all, line, last;
        while (std::getline(in, line)) { if (!last.empty()) all += last + "\n"; last = line; }
        in.close();
        check(last.find("\"done\"") != std::string::npos, "the run ended with a done line");
        std::ofstream(m, std::ios::trunc) << all;   // drop the done line
        Door c(home, data);
        c.load();
        check(c.unfinished().size() == 1 && c.unfinished()[0].find("had happened") != std::string::npos,
              "crash resolved by looking at the tag: had happened");
        check(c.undo().ok && val(f, "user.tidy.purpose") == "<absent>", "and undo takes it back");
    }

    std::printf("-- replace without undo (latch s003)\n");
    {
        const std::string H = "# latchtest ids";
        const std::string own = home + "/Documents/.latchtest-files";
        touch(own, H + "\nold line\n");
        Door r(home, data);
        r.load();
        const std::string nu = H + "\nnew line\n";
        check(!r.replace_refusal(own, nu).empty(), "no files registered -> refused");
        set_own_files({{".latchtest-files", H}});
        check(r.replace_refusal(own, nu).empty(), "registered, ours -> may");
        struct stat before{};
        ::lstat(own.c_str(), &before);
        const auto batches = r.batches().size();
        std::error_code mec;
        const auto msize = fs::file_size(r.manifest_path(), mec);
        check(r.replace_own(own, nu).empty(), "replaced");
        std::ifstream in(own);
        std::string all((std::istreambuf_iterator<char>(in)), {});
        struct stat after{};
        ::lstat(own.c_str(), &after);
        check(all == nu && after.st_ino != before.st_ino, "the new text in place (a new inode: tmp -> rename)");
        check(r.batches().size() == batches && fs::file_size(r.manifest_path(), mec) == msize,
              "no batch, no manifest line -- nothing to undo");
        bool stray = false;
        for (const auto& e : fs::directory_iterator(home + "/Documents"))
            stray = stray || e.path().filename().string().find(".new") != std::string::npos;
        check(!stray, "no tmp left behind");

        touch(home + "/Documents/.other", H + "\n");
        check(!r.replace_refusal(home + "/Documents/.other", nu).empty(), "a name not registered -> refused");
        const std::string users = home + "/.latchtest-files";
        touch(users, "my own notes\n");
        check(!r.replace_refusal(users, nu).empty() && r.replace_own(users, nu) != "", "a user's file of that name (no header) -> refused");
        { std::ifstream u(users); std::string l; std::getline(u, l); check(l == "my own notes", "and left as it was"); }
        check(!r.replace_refusal(home + "/Pictures/.latchtest-files", nu).empty(), "missing -> refused (never creates)");
        check(!r.replace_refusal(own, "no header\n").empty(), "new text without the header -> refused");
        check(!r.replace_refusal(own, H + "\n" + std::string(kOwnMax, 'x')).empty(), "new text too big -> refused");
        touch(home + "/fixes/.latchtest-files", H + "\n");
        check(!r.replace_refusal(home + "/fixes/.latchtest-files", nu).empty(), "inside fixes/ (protected) -> refused");
        fs::create_directories(home + "/Big");
        touch(home + "/Big/.latchtest-files", H + "\n" + std::string(kOwnMax, 'y'));
        check(!r.replace_refusal(home + "/Big/.latchtest-files", nu).empty(), "an old file too big to be ours -> refused");
        fs::create_directories(home + "/Real");
        touch(home + "/Real/.target", H + "\n");
        fs::create_directories(home + "/Links");
        fs::create_symlink(home + "/Real/.target", home + "/Links/.latchtest-files");
        check(!r.replace_refusal(home + "/Links/.latchtest-files", nu).empty(), "a symlink -> refused");
        fs::create_directory_symlink(home + "/Documents", home + "/Via");
        check(!r.replace_refusal(home + "/Via/.latchtest-files", nu).empty(), "through a symlinked folder -> refused");
        touch(root + "/.latchtest-files", H + "\n");
        check(!r.replace_refusal(root + "/.latchtest-files", nu).empty(), "outside home -> refused");
        touch(data + "/.latchtest-files", H + "\n");
        check(!r.replace_refusal(data + "/.latchtest-files", nu).empty(), "in the app's records -> refused");
        touch(home + "/.local/share/Trash/files/.latchtest-files", H + "\n");
        check(!r.replace_refusal(home + "/.local/share/Trash/files/.latchtest-files", nu).empty(), "in the Trash -> refused");
        set_own_files({});
    }

    fs::remove_all(root);
    std::printf("-----------------------------------------------\n%d pass / %d fail\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
