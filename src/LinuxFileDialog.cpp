// The process half of the Linux browse dialog (ticket #76): runs zenity or
// kdialog with posix_spawnp and collects its stdout, stderr and exit
// status. LinuxFileDialog.h has the contract and the testable logic (argv,
// parsing, fallback, child environment); this file is only covered by the
// Tier 1-2 loop, which needs a human and a desktop.

#ifdef __linux__

#include "LinuxFileDialog.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

// A helper that writes more than this is not answering a file pick; the
// rest is drained and dropped so it can't block on a full pipe.
const size_t kMaxCapture = 64 * 1024;

std::vector<std::string> currentEnvironment()
{
    std::vector<std::string> env;
    for (char** e = environ; e && *e; ++e) {
        env.push_back(*e);
    }
    return env;
}

std::vector<char*> cStrings(std::vector<std::string>& strings)
{
    std::vector<char*> out;
    for (std::string& s : strings) {
        out.push_back(&s[0]);
    }
    out.push_back(nullptr);
    return out;
}

// Read both pipes to EOF. poll, not two blocking reads: a helper filling
// the stderr pipe while we wait on stdout would deadlock both of us.
void drain(int outFd, int errFd, std::string* out, std::string* err)
{
    struct pollfd fds[2] = {{outFd, POLLIN, 0}, {errFd, POLLIN, 0}};
    std::string* sinks[2] = {out, err};
    int open = 2;
    char buf[4096];
    while (open > 0) {
        if (poll(fds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            break;
        }
        for (int i = 0; i < 2; ++i) {
            if (fds[i].fd < 0 || !(fds[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            const ssize_t n = read(fds[i].fd, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) {
                fds[i].fd = -1; // poll ignores negative fds
                --open;
                continue;
            }
            if (sinks[i]->size() < kMaxCapture) {
                sinks[i]->append(buf, static_cast<size_t>(n));
            }
        }
    }
}

ndi_linuxdlg::SpawnResult spawnHelper(const std::vector<std::string>& argvIn)
{
    using ndi_linuxdlg::SpawnResult;
    SpawnResult result;

    int outPipe[2];
    int errPipe[2];
    if (pipe2(outPipe, O_CLOEXEC) != 0) {
        result.err = std::strerror(errno);
        return result;
    }
    if (pipe2(errPipe, O_CLOEXEC) != 0) {
        result.err = std::strerror(errno);
        close(outPipe[0]);
        close(outPipe[1]);
        return result;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO);

    // The helper starts with no signals blocked and the usual dispositions
    // for the ones a host commonly ignores: an inherited SIG_IGN on SIGPIPE
    // or SIGCHLD, or the calling thread's blocked mask, would otherwise
    // survive exec into zenity.
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    sigset_t none;
    sigemptyset(&none);
    posix_spawnattr_setsigmask(&attr, &none);
    sigset_t defaults;
    sigemptyset(&defaults);
    for (int sig : {SIGPIPE, SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2, SIGALRM}) {
        sigaddset(&defaults, sig);
    }
    posix_spawnattr_setsigdefault(&attr, &defaults);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<std::string> argvStrings = argvIn;
    std::vector<std::string> envStrings = ndi_linuxdlg::childEnvironment(currentEnvironment());
    std::vector<char*> argv = cStrings(argvStrings);
    std::vector<char*> envp = cStrings(envStrings);

    pid_t pid = -1;
    const int rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(outPipe[1]);
    close(errPipe[1]);

    if (rc != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        result.status = (rc == ENOENT) ? SpawnResult::kNotFound : SpawnResult::kFailed;
        result.err = std::strerror(rc);
        return result;
    }

    drain(outPipe[0], errPipe[0], &result.out, &result.err);
    close(outPipe[0]);
    close(errPipe[0]);

    int status = 0;
    pid_t waited;
    do {
        waited = waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    result.status = SpawnResult::kExited;
    if (waited < 0) {
        // ECHILD: the host ignores SIGCHLD or another thread reaped the
        // child first, so the status is gone. Both helpers print a path
        // only on OK, so stdout is the answer.
        result.exitCode = result.out.empty() ? 1 : 0;
    } else if (WIFEXITED(status)) {
        result.exitCode = WEXITSTATUS(status);
    } else {
        result.exitCode = 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
        if (result.err.empty()) result.err = "killed by a signal";
    }
    return result;
}

thread_local std::string tLastError;

} // namespace

bool linux_open_file_dialog(const char* message, const char* extension,
                            const char* initialPath, char* outPath, size_t outPathSize)
{
    return ndi_linuxdlg::openFileDialog(message, extension, initialPath, outPath, outPathSize,
                                        currentEnvironment(), spawnHelper, &tLastError);
}

std::string linux_take_file_dialog_error()
{
    std::string error;
    error.swap(tLastError);
    return error;
}

#endif // __linux__
