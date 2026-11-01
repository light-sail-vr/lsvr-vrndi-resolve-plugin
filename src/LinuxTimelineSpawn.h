// Spawn seam for the Linux timeline-clip watcher (ticket #75): posix_spawn
// with the child's stdout on a pipe, ported from src/MacTimelineWatch.h
// (copied, not shared — spec decision 17 keeps macOS code untouched) and
// adapted where glibc differs from Darwin: no POSIX_SPAWN_CLOEXEC_DEFAULT,
// and the child's signal state is reset explicitly. Linux only; smoke-tested
// with /bin/sh in tests/test_linux_timeline_spawn.cpp. The pure discovery
// half is src/LinuxTimelineWatch.h.

#ifndef LINUX_TIMELINE_SPAWN_H
#define LINUX_TIMELINE_SPAWN_H

#include "LinuxTimelineWatch.h"

#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstring>
#include <string>
#include <vector>

extern char** environ;

namespace ndi_timelinewatch_linux {

// ---- dead-child description ------------------------------------------------------
// One phrase for the helper-exit log from a waitpid() status: "code N" for a
// normal exit, "signal N" for a signal death (issue #34: the exit code is
// what identifies a dead interpreter).
inline std::string describeWaitStatus(int waitStatus)
{
    if (WIFEXITED(waitStatus)) {
        return "code " + std::to_string(WEXITSTATUS(waitStatus));
    }
    if (WIFSIGNALED(waitStatus)) {
        return "signal " + std::to_string(WTERMSIG(waitStatus));
    }
    return "status " + std::to_string(waitStatus);
}

// ---- Closing the host's descriptors in the child ---------------------------------
// glibc >= 2.34 has posix_spawn_file_actions_addclosefrom_np, which closes
// everything from N up in the child with no race. The plugin is built
// against the Rocky 8 baseline (glibc 2.28, spec decision 6), which doesn't
// declare it, so it's looked up at run time; without it, every descriptor
// listed in /proc/self/fd just before the spawn gets its own close action
// (descriptorsToClose). That list can miss an fd another Resolve thread
// opens in the gap, so it is the fallback only.
using AddClosefromFn = int (*)(posix_spawn_file_actions_t*, int);

inline AddClosefromFn addClosefrom()
{
    static const AddClosefromFn fn = reinterpret_cast<AddClosefromFn>(
        dlsym(RTLD_DEFAULT, "posix_spawn_file_actions_addclosefrom_np"));
    return fn;
}

inline bool haveAddClosefrom()
{
    return addClosefrom() != nullptr;
}

enum class CloseStrategy {
    Auto,        // addclosefrom_np when glibc has it, else the /proc/self/fd list
    ProcFdList,  // always the list (tests exercise the fallback on new glibc)
};

inline std::vector<std::string> procSelfFdNames()
{
    std::vector<std::string> names;
    DIR* dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return names;
    }
    while (dirent* entry = readdir(dir)) {
        names.emplace_back(entry->d_name);
    }
    closedir(dir);
    return names;
}

// ---- Process spawn -----------------------------------------------------------------
// posix_spawn with the child's stdout on a fresh pipe, stdin/stderr on
// /dev/null and no other descriptor of ours (stderr must never join the pipe
// — the line protocol would read it as clip paths). Our copy of the write end
// is closed before returning, so the child's death EOFs the read end — the
// watcher's only wake-up signal. The child starts with an empty signal mask
// and every signal at its default action: posix_spawn otherwise keeps the
// spawning thread's mask and every ignored signal across exec, and a helper
// that inherited a blocked SIGTERM would shrug off the watcher's shutdown.
// `env` (KEY=VALUE entries) replaces the environment; nullptr inherits ours.
// On success *pid and *readFd are the caller's to reap and close; on failure
// both are -1 and *error carries errno wording.
inline bool spawnPipedProcess(const std::vector<std::string>& argv,
                              const std::vector<std::string>* env, pid_t* pid, int* readFd,
                              std::string* error, CloseStrategy strategy = CloseStrategy::Auto)
{
    *pid = -1;
    *readFd = -1;
    auto fail = [error](const char* what, int err) {
        if (error) {
            *error = std::string(what) + ": " + strerror(err);
        }
        return false;
    };
    if (argv.empty()) {
        return fail("spawn", EINVAL);
    }

    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) {
        // Close-on-exec on both ends: any OTHER child Resolve forks meanwhile
        // would otherwise inherit the write end and hold the pipe open past
        // the helper's death. The dup2 below gives the helper a plain stdout.
        return fail("pipe2() failed", errno);
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    // Then everything from 3 up — our pipe ends included — so the helper
    // never holds Resolve's listening sockets (LEARNINGS 2026-10-29).
    const AddClosefromFn closefrom = strategy == CloseStrategy::Auto ? addClosefrom() : nullptr;
    if (closefrom != nullptr) {
        closefrom(&actions, 3);
    } else {
        // glibc ignores EBADF from a close action, so an fd that closed after
        // the listing is harmless.
        for (int fd : descriptorsToClose(procSelfFdNames())) {
            posix_spawn_file_actions_addclose(&actions, fd);
        }
    }

    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none, all;
    sigemptyset(&none);
    sigfillset(&all);
    posix_spawnattr_setsigmask(&attr, &none);
    posix_spawnattr_setsigdefault(&attr, &all);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<char*> args;
    args.reserve(argv.size() + 1);
    for (const std::string& a : argv) {
        args.push_back(const_cast<char*>(a.c_str()));
    }
    args.push_back(nullptr);
    std::vector<char*> envp;
    if (env != nullptr) {
        envp.reserve(env->size() + 1);
        for (const std::string& e : *env) {
            envp.push_back(const_cast<char*>(e.c_str()));
        }
        envp.push_back(nullptr);
    }

    pid_t child = -1;
    const int rc = posix_spawn(&child, argv[0].c_str(), &actions, &attr, args.data(),
                               env != nullptr ? envp.data() : environ);
    posix_spawnattr_destroy(&attr);
    posix_spawn_file_actions_destroy(&actions);
    close(fds[1]);  // the child holds its own copy now (or never will)
    if (rc != 0) {
        close(fds[0]);
        return fail("posix_spawn failed", rc);
    }
    *pid = child;
    *readFd = fds[0];
    return true;
}

// ---- Probe-once ---------------------------------------------------------------------
// Run a candidate to completion with its output discarded; true only for a
// clean exit(0). /usr/bin/python3 is a real interpreter on Linux, but the
// fallback still trusts it only after seeing it run `--version` (issue #34's
// rule: never trust an interpreter you haven't seen speak).
inline bool exitsCleanly(const std::vector<std::string>& argv)
{
    pid_t pid = -1;
    int fd = -1;
    std::string error;
    if (!spawnPipedProcess(argv, nullptr, &pid, &fd, &error)) {
        return false;
    }
    char sink[256];
    while (read(fd, sink, sizeof(sink)) > 0) {
    }
    close(fd);
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
        return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

} // namespace ndi_timelinewatch_linux

#endif // LINUX_TIMELINE_SPAWN_H
