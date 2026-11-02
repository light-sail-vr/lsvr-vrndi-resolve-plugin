// The process half of the Linux browse dialog (ticket #76): runs zenity or
// kdialog with posix_spawnp and collects its stdout, stderr and exit
// status. LinuxFileDialog.h has the contract and the testable logic (argv,
// parsing, fallback, child environment); this file is only covered by the
// Tier 1-2 loop, which needs a human and a desktop.

#ifdef __linux__

#include "LinuxFileDialog.h"
#include "LinuxHostEvents.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <poll.h>
#include <dlfcn.h>
#include <locale.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace {

// A helper that writes more than this is not answering a file pick; the
// rest is drained and dropped so it can't block on a full pipe.
const size_t kMaxCapture = 64 * 1024;

// How often the idle hook runs while the helper is alive: often enough that
// Resolve repaints smoothly and answers the window manager's ping.
const int kIdleIntervalMs = 30;

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

// The running helper, if any (0 when none): closeRunningHelper signals its
// process group from the unload path, the exit path or any other thread.
std::mutex gHelperMutex;
pid_t gHelperPid = 0;
// Set by closeRunningHelper, cleared when the next dialog opens: the running
// dialog's idle tick sees it and closes its helper too.
std::atomic<bool> gCloseRequested{false};

void closeRunningHelper()
{
    gCloseRequested.store(true);
    std::lock_guard<std::mutex> lock(gHelperMutex);
    if (gHelperPid > 0) kill(-gHelperPid, SIGTERM);
}

// Resolve exiting (exit() runs this .so's static destructors, including
// from inside the dialog's event pump) closes a helper that is still up,
// rather than leaving an orphaned picker on screen. A crash doesn't run it.
struct CloseHelperAtExit {
    ~CloseHelperAtExit() { closeRunningHelper(); }
} gCloseHelperAtExit;

// After idle asks for the helper to close: SIGTERM, then SIGKILL if it is
// still holding its pipes this long after, and after as long again (a
// grandchild holding the pipes) stop reading.
const int kAbortGraceMs = 2000;

// Read both pipes to EOF. poll, not two blocking reads: a helper filling
// the stderr pipe while we wait on stdout would deadlock both of us. With an
// idle hook, poll wakes every kIdleIntervalMs to run it; the first false it
// returns closes the helper (pid) and sets *aborted.
void drain(int outFd, int errFd, std::string* out, std::string* err,
           const std::function<bool()>& idle, pid_t pid, bool* aborted)
{
    using Clock = std::chrono::steady_clock;
    struct pollfd fds[2] = {{outFd, POLLIN, 0}, {errFd, POLLIN, 0}};
    std::string* sinks[2] = {out, err};
    int open = 2;
    char buf[4096];
    Clock::time_point lastIdle = Clock::now();
    Clock::time_point abortedAt;
    bool killed = false;
    while (open > 0) {
        const bool ticking = idle && !*aborted;
        const int ready = poll(fds, 2, (ticking || *aborted) ? kIdleIntervalMs : -1);
        const Clock::time_point now = Clock::now();
        if (ticking && now - lastIdle >= std::chrono::milliseconds(kIdleIntervalMs)) {
            if (!idle()) {
                *aborted = true;
                abortedAt = Clock::now();
                kill(-pid, SIGTERM); // its process group: a wrapper script's children too
            }
            lastIdle = Clock::now();
        } else if (*aborted && now - abortedAt >= std::chrono::milliseconds(kAbortGraceMs)) {
            if (killed && now - abortedAt >= std::chrono::milliseconds(2 * kAbortGraceMs)) break;
            if (!killed) kill(-pid, SIGKILL);
            killed = true;
        }
        if (ready < 0) {
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

} // namespace

ndi_linuxdlg::SpawnResult ndi_linuxdlg::spawnProcess(const std::vector<std::string>& argvIn,
                                                     const std::vector<std::string>& envIn,
                                                     const std::function<bool()>& idle)
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

    // Every posix_spawn_file_actions_* / posix_spawnattr_* call can fail
    // (ENOMEM, EBADF); a helper started with half its setup would write its
    // answer to Resolve's stdout, so any failure is a spawn failure.
    int setupError = 0;
    auto check = [&setupError](int rc) {
        if (rc != 0 && setupError == 0) setupError = rc;
    };

    posix_spawn_file_actions_t actions;
    const int actionsInit = posix_spawn_file_actions_init(&actions);
    check(actionsInit);
    if (actionsInit == 0) {
        check(posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0));
        check(posix_spawn_file_actions_adddup2(&actions, outPipe[1], STDOUT_FILENO));
        check(posix_spawn_file_actions_adddup2(&actions, errPipe[1], STDERR_FILENO));
    }

    // The helper starts with no signals blocked and the usual dispositions
    // for the ones a host commonly ignores: an inherited SIG_IGN on SIGPIPE
    // or SIGCHLD, or the calling thread's blocked mask, would otherwise
    // survive exec into zenity.
    posix_spawnattr_t attr;
    const int attrInit = posix_spawnattr_init(&attr);
    check(attrInit);
    if (attrInit == 0) {
        sigset_t none;
        sigemptyset(&none);
        check(posix_spawnattr_setsigmask(&attr, &none));
        sigset_t defaults;
        sigemptyset(&defaults);
        for (int sig : {SIGPIPE, SIGCHLD, SIGINT, SIGTERM, SIGHUP, SIGQUIT, SIGUSR1, SIGUSR2, SIGALRM}) {
            sigaddset(&defaults, sig);
        }
        check(posix_spawnattr_setsigdefault(&attr, &defaults));
        // Its own process group (pgid = its pid), so closing it reaches
        // whatever a wrapper script on PATH started, and nothing of Resolve's.
        check(posix_spawnattr_setpgroup(&attr, 0));
        check(posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF |
                                                  POSIX_SPAWN_SETPGROUP));
    }

    std::vector<std::string> argvStrings = argvIn;
    std::vector<std::string> envStrings = envIn;
    std::vector<char*> argv = cStrings(argvStrings);
    std::vector<char*> envp = cStrings(envStrings);

    pid_t pid = -1;
    int rc = setupError;
    if (rc == 0) {
        // Under the helper lock, so closeRunningHelper never misses a helper
        // that has started but isn't recorded yet.
        std::lock_guard<std::mutex> lock(gHelperMutex);
        rc = posix_spawnp(&pid, argv[0], &actions, &attr, argv.data(), envp.data());
        if (rc == 0) gHelperPid = pid;
    }
    if (actionsInit == 0) posix_spawn_file_actions_destroy(&actions);
    if (attrInit == 0) posix_spawnattr_destroy(&attr);
    close(outPipe[1]);
    close(errPipe[1]);

    if (setupError != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        result.status = SpawnResult::kFailed;
        result.err = std::string("couldn't set up the helper's stdio and signals: ") +
                     std::strerror(setupError);
        return result;
    }
    if (rc != 0) {
        close(outPipe[0]);
        close(errPipe[0]);
        result.status = (rc == ENOENT) ? SpawnResult::kNotFound : SpawnResult::kFailed;
        result.err = std::strerror(rc);
        return result;
    }

    drain(outPipe[0], errPipe[0], &result.out, &result.err, idle, pid, &result.aborted);
    close(outPipe[0]);
    close(errPipe[0]);

    // Wait for the exit without reaping (WNOWAIT), so the pid can't be
    // reused while closeRunningHelper may still signal it; forget it under
    // the lock, then reap. (A host that ignores SIGCHLD has the kernel reap
    // it, so waitid fails with ECHILD and we just forget it.)
    siginfo_t info;
    int waitedNoReap;
    do {
        waitedNoReap = waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT);
    } while (waitedNoReap < 0 && errno == EINTR);
    {
        std::lock_guard<std::mutex> lock(gHelperMutex);
        gHelperPid = 0;
    }
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

namespace {

// Browse clicks arrive on Resolve's main thread, but the gate is atomic and
// released by RAII anyway, so neither a thread nor an exception (bad_alloc
// in the argv) can leave Browse locked for the session.
ndi_linuxdlg::DialogState gBrowseState; // last pick + last failure's reason
ndi_linuxdlg::DialogGate gDialogGate;

// Whether the system has a C.UTF-8 locale to give the helper.
bool utf8LocaleAvailable()
{
    locale_t utf8 = newlocale(LC_CTYPE_MASK, "C.UTF-8", static_cast<locale_t>(0));
    if (!utf8) return false;
    freelocale(utf8);
    return true;
}

std::int64_t monotonicMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool onMainThread()
{
    return static_cast<pid_t>(syscall(SYS_gettid)) == getpid();
}

// Resolve's Qt event pump, looked up once (LinuxHostEvents.h).
const ndi_linux_host::EventPump& hostEventPump()
{
    static const ndi_linux_host::EventPump pump = ndi_linux_host::resolveEventPump(
        [](const char* symbol) { return dlsym(RTLD_DEFAULT, symbol); });
    return pump;
}

} // namespace

bool linux_open_file_dialog(const char* message, const char* extension,
                            const char* initialPath, char* outPath, size_t outPathSize)
{
    // The pump runs Resolve's non-input events while the helper is up; none
    // of them should reach another Browse, but a second dialog stacked on the
    // first is refused rather than nested.
    const ndi_linuxdlg::DialogSlot slot(gDialogGate, monotonicMs);
    if (slot.admission() == ndi_linuxdlg::Admission::kDebounced) {
        // A click replayed from while the last dialog was up (DialogGate).
        gBrowseState.error.clear();
        return false;
    }
    if (!slot.held()) {
        gBrowseState.error = "Browse: a file dialog is already open";
        return false;
    }
    gCloseRequested.store(false);
    const bool mainThread = onMainThread();
    const ndi_linux_host::EventPump& pump = hostEventPump();
    const bool appAtStart = pump.appExists();
    // Resolve's environment, read once per Browse: HOME and the display for
    // the dialog's logic, and the helper's own environment for each spawn.
    const std::vector<std::string> env = currentEnvironment();
    static const bool utf8Available = utf8LocaleAvailable();
    const std::vector<std::string> helperEnv =
        ndi_linuxdlg::withUtf8Locale(ndi_linuxdlg::childEnvironment(env), utf8Available);
    const bool picked = ndi_linuxdlg::openFileDialog(
        message, extension, initialPath, outPath, outPathSize, env,
        [&](const std::vector<std::string>& argv) {
            ndi_linuxdlg::SpawnResult run = ndi_linuxdlg::spawnProcess(argv, helperEnv, [&] {
                return pump.tick(mainThread, appAtStart, gCloseRequested.load());
            });
            // A close can kill the helper before the next tick sees the
            // request; it is still the plugin's close, not a helper failure.
            if (gCloseRequested.load()) run.aborted = true;
            return run;
        },
        &gBrowseState);
    return picked;
}

void linux_file_dialog_action_done()
{
    gDialogGate.settle(monotonicMs());
}

void linux_file_dialog_set_debounce_ms(int ms)
{
    gDialogGate.setDebounceMs(ms);
}

void linux_close_file_dialog()
{
    closeRunningHelper();
}

ndi_linuxdlg::LiveInstances& linux_browse_live_instances()
{
    static ndi_linuxdlg::LiveInstances live;
    return live;
}

std::string linux_take_file_dialog_error()
{
    std::string error;
    error.swap(gBrowseState.error);
    return error;
}

#endif // __linux__
