#include "latch/Vcs.hpp"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <utility>

// Git.cpp -- git as a child process. Prompts, askpass and optional locks off;
// no stdin; its own process group so a timeout takes ssh with it. Whether a
// call writes is the CALLER's business: Dev only reads; the door's Clone verb
// is the one caller that writes (a clone into a staging folder it owns).

extern char** environ;

namespace latch::vcs {

namespace {
Run spawn(std::vector<std::string> argv_s, std::vector<std::string> env_s, int timeout_s,
          const std::atomic<bool>* cancel);
}  // namespace

Run git(const std::string& dir, const std::vector<std::string>& args, int timeout_s,
        const std::atomic<bool>* cancel) {
    std::vector<std::string> argv_s = {"git", "-C", dir};
    argv_s.insert(argv_s.end(), args.begin(), args.end());

    // The environment: the user's, with prompts and optional locks off.
    std::vector<std::string> env_s;
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        if (kv.starts_with("GIT_TERMINAL_PROMPT=") || kv.starts_with("GIT_OPTIONAL_LOCKS=") ||
            kv.starts_with("GIT_ASKPASS=") || kv.starts_with("SSH_ASKPASS=") || kv.starts_with("LC_ALL=") ||
            kv.starts_with("GIT_SSH_COMMAND=") || kv.starts_with("GIT_DIR=") || kv.starts_with("GIT_WORK_TREE="))
            continue;
        env_s.push_back(kv);
    }
    env_s.push_back("GIT_TERMINAL_PROMPT=0");
    env_s.push_back("GIT_OPTIONAL_LOCKS=0");
    env_s.push_back("GIT_ASKPASS=/bin/true");
    env_s.push_back("SSH_ASKPASS=/bin/true");
    env_s.push_back("LC_ALL=C");
    env_s.push_back("GIT_SSH_COMMAND=ssh -o BatchMode=yes -o ConnectTimeout=10");
    return spawn(std::move(argv_s), std::move(env_s), timeout_s, cancel);
}

Run child(const std::vector<std::string>& argv_in, int timeout_s, const std::atomic<bool>* cancel) {
    std::vector<std::string> env_s;
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        if (!kv.starts_with("LC_ALL=")) env_s.push_back(kv);
    }
    env_s.push_back("LC_ALL=C");
    return spawn(argv_in, std::move(env_s), timeout_s, cancel);
}

namespace {
Run spawn(std::vector<std::string> argv_s, std::vector<std::string> env_s, int timeout_s,
          const std::atomic<bool>* cancel) {
    Run r;
    if (argv_s.empty()) return r;
    std::vector<char*> argv;
    for (auto& a : argv_s) argv.push_back(a.data());
    argv.push_back(nullptr);
    std::vector<char*> envp;
    for (auto& e : env_s) envp.push_back(e.data());
    envp.push_back(nullptr);

    int out[2], err[2];
    if (::pipe2(out, O_CLOEXEC) != 0) return r;
    if (::pipe2(err, O_CLOEXEC) != 0) { ::close(out[0]); ::close(out[1]); return r; }
    const pid_t pid = ::fork();
    if (pid < 0) {
        for (int fd : {out[0], out[1], err[0], err[1]}) ::close(fd);
        return r;
    }
    if (pid == 0) {
        ::dup2(out[1], 1);
        ::dup2(err[1], 2);
        ::close(0);                 // no stdin: nothing can wait on a prompt
        ::setpgid(0, 0);            // its own group, so a timeout kills ssh too
        ::execvpe(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(out[1]);
    ::close(err[1]);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    pollfd fds[2] = {{out[0], POLLIN, 0}, {err[0], POLLIN, 0}};
    int open_fds = 2;
    char buf[8192];
    bool killed = false;
    while (open_fds > 0) {
        const bool stop = cancel && cancel->load();
        if (!killed && (stop || std::chrono::steady_clock::now() > deadline)) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            killed = true;
            r.timed_out = !stop;
        }
        const int n = ::poll(fds, 2, 100);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t got = ::read(fds[i].fd, buf, sizeof buf);
            if (got > 0) (i == 0 ? r.out : r.err).append(buf, static_cast<std::size_t>(got));
            else { ::close(fds[i].fd); fds[i].fd = -1; --open_fds; }
        }
    }
    for (auto& f : fds) if (f.fd >= 0) ::close(f.fd);
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    r.code = (!killed && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;
    return r;
}
}  // namespace

}  // namespace latch::vcs
