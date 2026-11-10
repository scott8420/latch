#pragma once
#include <atomic>
#include <string>
#include <vector>

// ─────────────────────────────────────────────────────────────────────────────
// Git -- `git -C dir args...` as a child (s007a; its own file since s007b, when
// the door's Clone verb became the second caller). GTK-free.
// ─────────────────────────────────────────────────────────────────────────────
namespace latch::vcs {

struct Run {
    int         code = -1;        // exit status; -1 = did not run / killed
    std::string out, err;
    bool        timed_out = false;
};

// Prompts off (GIT_TERMINAL_PROMPT=0, askpass /bin/true, ssh BatchMode),
// optional locks off (GIT_OPTIONAL_LOCKS=0: `status` does not refresh the
// index), LC_ALL=C, no stdin. Killed after timeout_s, or when *cancel turns
// true. The user's git config still applies (url rewrites, credential helpers).
Run git(const std::string& dir, const std::vector<std::string>& args, int timeout_s,
        const std::atomic<bool>* cancel = nullptr);

// Any other program the same way (s012, profile backup: dconf, dnf, flatpak):
// no stdin, its own process group, LC_ALL=C, killed after timeout_s or on
// *cancel. argv[0] is looked up on PATH; code 127 = not found.
Run child(const std::vector<std::string>& argv, int timeout_s, const std::atomic<bool>* cancel = nullptr);

// s012c (secrets): a program fed through pipes -- `in` on its stdin, `fd3` on
// file descriptor 3 (gpg's --passphrase-fd 3: a secret never goes on argv or
// in the environment). Otherwise as child(). Lives in Pipe.cpp, the one file
// besides the door allowed to say write(): it writes ONLY into the pipes of
// the child it made.
Run feed(const std::vector<std::string>& argv, const std::string& in, const std::string& fd3, int timeout_s,
         const std::atomic<bool>* cancel = nullptr);

}  // namespace latch::vcs
