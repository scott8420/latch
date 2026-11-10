#include "latch/Vcs.hpp"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <ctime>

// Pipe.cpp (s012c) -- a child fed through pipes: stdin from a buffer, and a
// second buffer on fd 3 (gpg --passphrase-fd 3). Allowlisted in the source
// gate for ONE reason: its write() calls go only into the pipes of the child
// it just made -- never to a path. Otherwise the same rules as vcs::child():
// its own process group, LC_ALL=C, killed after the timeout or on cancel.
//
// SIGPIPE: if the child exits before reading all its input, a write would
// raise SIGPIPE and kill Rouster. It is BLOCKED in this thread while feeding
// (write then fails with EPIPE), any pending one is consumed, and the mask is
// put back. The child gets the caller's original mask before exec.

extern char** environ;

namespace latch::vcs {

Run feed(const std::vector<std::string>& argv_in, const std::string& in, const std::string& fd3, int timeout_s,
         const std::atomic<bool>* cancel) {
    Run r;
    if (argv_in.empty()) return r;
    std::vector<std::string> argv_s = argv_in;
    std::vector<std::string> env_s;
    for (char** e = environ; e && *e; ++e) {
        const std::string kv = *e;
        if (!kv.starts_with("LC_ALL=")) env_s.push_back(kv);
    }
    env_s.push_back("LC_ALL=C");
    std::vector<char*> argv, envp;
    for (auto& a : argv_s) argv.push_back(a.data());
    argv.push_back(nullptr);
    for (auto& e : env_s) envp.push_back(e.data());
    envp.push_back(nullptr);

    int pin[2] = {-1, -1}, pout[2] = {-1, -1}, perr[2] = {-1, -1}, p3[2] = {-1, -1};
    const auto close_all = [&] {
        for (int* p : {pin, pout, perr, p3})
            for (int k = 0; k < 2; ++k)
                if (p[k] >= 0) { ::close(p[k]); p[k] = -1; }
    };
    if (::pipe2(pin, O_CLOEXEC) || ::pipe2(pout, O_CLOEXEC) || ::pipe2(perr, O_CLOEXEC) || ::pipe2(p3, O_CLOEXEC)) {
        close_all();
        return r;
    }
    // fd 3 must end up as the passphrase pipe's read end: move every pipe end
    // above 9 first, so no dup2 below lands on a pipe it still needs.
    for (int* p : {pin, pout, perr, p3})
        for (int k = 0; k < 2; ++k) {
            const int moved = ::fcntl(p[k], F_DUPFD_CLOEXEC, 10);
            if (moved < 0) { close_all(); return r; }
            ::close(p[k]);
            p[k] = moved;
        }

    sigset_t block, old;
    sigemptyset(&block);
    sigaddset(&block, SIGPIPE);
    ::pthread_sigmask(SIG_BLOCK, &block, &old);

    const pid_t pid = ::fork();
    if (pid < 0) {
        close_all();
        ::pthread_sigmask(SIG_SETMASK, &old, nullptr);
        return r;
    }
    if (pid == 0) {
        ::pthread_sigmask(SIG_SETMASK, &old, nullptr);
        ::dup2(pin[0], 0);       // dup2 clears close-on-exec on the new number
        ::dup2(pout[1], 1);
        ::dup2(perr[1], 2);
        ::dup2(p3[0], 3);
        ::setpgid(0, 0);
        ::execvpe(argv[0], argv.data(), envp.data());
        ::_exit(127);
    }
    ::close(pin[0]);  pin[0] = -1;
    ::close(pout[1]); pout[1] = -1;
    ::close(perr[1]); perr[1] = -1;
    ::close(p3[0]);   p3[0] = -1;
    ::fcntl(pin[1], F_SETFL, O_NONBLOCK);
    ::fcntl(p3[1], F_SETFL, O_NONBLOCK);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    std::size_t in_at = 0, p3_at = 0;
    if (in.empty()) { ::close(pin[1]); pin[1] = -1; }
    if (fd3.empty()) { ::close(p3[1]); p3[1] = -1; }
    char buf[16384];
    bool killed = false;
    while (pout[0] >= 0 || perr[0] >= 0) {
        const bool stop = cancel && cancel->load();
        if (!killed && (stop || std::chrono::steady_clock::now() > deadline)) {
            ::kill(-pid, SIGKILL);
            ::kill(pid, SIGKILL);
            killed = true;
            r.timed_out = !stop;
        }
        pollfd fds[4] = {{pout[0], POLLIN, 0}, {perr[0], POLLIN, 0}, {pin[1], POLLOUT, 0}, {p3[1], POLLOUT, 0}};
        const int n = ::poll(fds, 4, 100);
        if (n < 0) { if (errno == EINTR) continue; break; }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t got = ::read(fds[i].fd, buf, sizeof buf);
            if (got > 0) (i == 0 ? r.out : r.err).append(buf, static_cast<std::size_t>(got));
            else if (got == 0 || errno != EAGAIN) { ::close(fds[i].fd); (i == 0 ? pout[0] : perr[0]) = -1; }
        }
        // The two inputs: write what fits, close when all is written (EOF for
        // the child) or when the child stopped reading (EPIPE).
        const auto push = [&](int idx, int& fd, const std::string& data, std::size_t& at) {
            if (fd < 0 || !(fds[idx].revents & (POLLOUT | POLLERR | POLLHUP))) return;
            const ssize_t w = ::write(fd, data.data() + at, data.size() - at);
            if (w > 0) at += static_cast<std::size_t>(w);
            if (at >= data.size() || (w < 0 && errno != EAGAIN && errno != EINTR)) { ::close(fd); fd = -1; }
        };
        push(3, p3[1], fd3, p3_at);
        push(2, pin[1], in, in_at);
    }
    close_all();
    int st = 0;
    while (::waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    r.code = (!killed && WIFEXITED(st)) ? WEXITSTATUS(st) : -1;

    // Consume a SIGPIPE raised while it was blocked, then put the mask back.
    timespec zero{0, 0};
    while (::sigtimedwait(&block, nullptr, &zero) > 0) {}
    ::pthread_sigmask(SIG_SETMASK, &old, nullptr);
    return r;
}

}  // namespace latch::vcs
