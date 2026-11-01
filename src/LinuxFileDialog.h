// Linux counterpart of MacFileDialog.h / WinFileDialog.h (ticket #76, spec
// decision 18): the browse push-buttons pop a file picker because Resolve
// renders kOfxParamStringIsFilePath string params as plain text fields.
// There is no toolkit the plugin can safely link (Resolve's private Qt is
// decision 6's ABI clash), so the picker is a helper process: `zenity
// --file-selection`, falling back to `kdialog --getopenfilename`. Both talk
// to xdg-desktop-portal themselves under Wayland.
//
// Everything decidable without a desktop lives here as inline helpers in
// ndi_linuxdlg, with the process spawn injected, so
// tests/test_linux_file_dialog.cpp runs it headless on any platform; the
// posix_spawn half stays in LinuxFileDialog.cpp and is only covered by the
// Tier 1-2 loop.

#ifndef LINUX_FILE_DIALOG_H
#define LINUX_FILE_DIALOG_H

#include <cstring>
#include <functional>
#include <string>
#include <vector>

namespace ndi_linuxdlg {

// What the plugin asks for: title, one extension or a ';'-separated list
// ("braw;ilpd"), the field's current value (may be empty), and $HOME for
// when it is.
struct Request {
    std::string title;
    std::string extensions;
    std::string initialPath;
    std::string home;
};

// Directory portion of a file path, trailing '/' kept so both helpers treat
// it as a folder to open in. Empty when the path has no '/'.
inline std::string parentDirectory(const std::string& path)
{
    const size_t pos = path.rfind('/');
    return (pos == std::string::npos) ? std::string() : path.substr(0, pos + 1);
}

inline std::vector<std::string> splitExtensions(const std::string& extensions)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= extensions.size()) {
        size_t end = extensions.find(';', start);
        if (end == std::string::npos) end = extensions.size();
        if (end > start) out.push_back(extensions.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

inline std::string asciiUpper(std::string s)
{
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return s;
}

// "*.exr *.EXR": Linux filesystems are case-sensitive, and cameras and
// tools write upper-case extensions as often as lower.
inline std::string filterPatterns(const std::string& extensions)
{
    std::string patterns;
    for (const std::string& ext : splitExtensions(extensions)) {
        const std::string upper = asciiUpper(ext);
        if (!patterns.empty()) patterns += " ";
        patterns += "*." + ext;
        if (upper != ext) patterns += " *." + upper;
    }
    return patterns;
}

// "EXR files", "BRAW/ILPD files".
inline std::string filterLabel(const std::string& extensions)
{
    std::string label;
    for (const std::string& ext : splitExtensions(extensions)) {
        if (!label.empty()) label += "/";
        label += asciiUpper(ext);
    }
    return label + " files";
}

// The folder the dialog opens in: the field's directory, else $HOME.
inline std::string startDirectory(const Request& r)
{
    std::string dir = parentDirectory(r.initialPath);
    if (dir.empty() && !r.home.empty()) {
        dir = r.home;
        if (dir.back() != '/') dir += "/";
    }
    return dir;
}

// argv for zenity (GTK). Each element is one argv entry, handed to
// posix_spawnp as-is: no shell, so spaces and non-ASCII need no quoting.
// --modal is only a window-manager hint (zenity 4 can't take a parent).
inline std::vector<std::string> zenityArgv(const Request& r)
{
    std::vector<std::string> argv = {"zenity", "--file-selection", "--modal"};
    if (!r.title.empty()) argv.push_back("--title=" + r.title);
    if (!splitExtensions(r.extensions).empty()) {
        argv.push_back("--file-filter=" + filterLabel(r.extensions) + " | " +
                       filterPatterns(r.extensions));
        argv.push_back("--file-filter=All files | *");
    }
    const std::string dir = startDirectory(r);
    if (!dir.empty()) argv.push_back("--filename=" + dir);
    return argv;
}

// argv for kdialog (KDE): the start directory is positional and must be
// present for the filter to land in its slot, hence "." as a last resort.
// The filter is KDE's "patterns|label" form, one row per line, which every
// kdialog since KDE 4 accepts.
inline std::vector<std::string> kdialogArgv(const Request& r)
{
    std::vector<std::string> argv = {"kdialog"};
    if (!r.title.empty()) {
        argv.push_back("--title");
        argv.push_back(r.title);
    }
    argv.push_back("--getopenfilename");
    const std::string dir = startDirectory(r);
    argv.push_back(dir.empty() ? std::string(".") : dir);
    if (!splitExtensions(r.extensions).empty()) {
        argv.push_back(filterPatterns(r.extensions) + "|" + filterLabel(r.extensions) +
                       "\n*|All files");
    }
    return argv;
}

// One helper run, as the spawner reports it. kNotFound: the program isn't
// on PATH (posix_spawnp's ENOENT). kFailed: it couldn't be started or
// waited for at all; err then holds the reason.
struct SpawnResult {
    enum Status { kExited, kNotFound, kFailed };
    Status status = kFailed;
    int exitCode = -1;
    std::string out; // stdout
    std::string err; // stderr, or the spawn failure's reason
};

// Runs argv (argv[0] looked up on PATH) to completion. Injected so the tests
// never launch a GUI; LinuxFileDialog.cpp has the posix_spawn one.
using Spawner = std::function<SpawnResult(const std::vector<std::string>& argv)>;

enum class Outcome { kPicked, kCancelled, kFailed };

struct Result {
    Outcome outcome = Outcome::kFailed;
    std::string path;  // kPicked only
    std::string error; // kFailed only: one line for Stream Status and the log
};

inline std::string firstLine(const std::string& s)
{
    return s.substr(0, s.find('\n'));
}

// What a helper's exit means. Both helpers print the path plus one '\n' and
// exit 0 on OK, and exit 1 on Cancel or a closed window. Exit 0 without an
// absolute path is treated as a failure, never as a pick.
inline Result interpret(const char* helper, const SpawnResult& run)
{
    Result r;
    if (run.status == SpawnResult::kFailed) {
        r.error = std::string("Browse: couldn't run ") + helper + ": " + firstLine(run.err);
        return r;
    }
    if (run.exitCode == 1) {
        r.outcome = Outcome::kCancelled;
        return r;
    }
    if (run.exitCode != 0) {
        r.error = std::string("Browse: ") + helper + " exited with status " +
                  std::to_string(run.exitCode);
        if (!run.err.empty()) r.error += ": " + firstLine(run.err);
        return r;
    }
    std::string path = run.out;
    if (!path.empty() && path.back() == '\n') path.pop_back();
    if (path.empty() || path[0] != '/') {
        r.error = std::string("Browse: ") + helper + " returned no file path";
        return r;
    }
    r.outcome = Outcome::kPicked;
    r.path = path;
    return r;
}

inline bool helperMissing(const SpawnResult& run)
{
    // 127 is the shell's "command not found", for a wrapper on PATH whose
    // real program is gone.
    return run.status == SpawnResult::kNotFound ||
           (run.status == SpawnResult::kExited && run.exitCode == 127);
}

// The whole browse: zenity, else kdialog when zenity isn't installed. A
// helper that ran is never retried with the other one, whatever it
// returned. haveDisplay is whether Resolve has DISPLAY or WAYLAND_DISPLAY:
// zenity exits 1 when it can't open a display (verified on the box), which
// would otherwise read as a silent cancel.
inline Result runDialog(const Request& request, bool haveDisplay, const Spawner& spawn)
{
    if (!haveDisplay) {
        Result r;
        r.error = "Browse: no desktop session (neither DISPLAY nor WAYLAND_DISPLAY is set "
                  "for Resolve)";
        return r;
    }
    const SpawnResult zenity = spawn(zenityArgv(request));
    if (!helperMissing(zenity)) {
        return interpret("zenity", zenity);
    }
    const SpawnResult kdialog = spawn(kdialogArgv(request));
    if (!helperMissing(kdialog)) {
        return interpret("kdialog", kdialog);
    }
    Result r;
    r.error = "Browse needs zenity or kdialog: install the zenity package "
              "(e.g. sudo apt install zenity), or kdialog on KDE";
    return r;
}

// Value of NAME in a "NAME=value" environment list; empty when unset.
inline std::string envValue(const std::vector<std::string>& env, const char* name)
{
    const size_t n = std::strlen(name);
    for (const std::string& kv : env) {
        if (kv.size() > n && kv.compare(0, n, name) == 0 && kv[n] == '=') {
            return kv.substr(n + 1);
        }
    }
    return std::string();
}

inline bool hasDisplay(const std::vector<std::string>& env)
{
    return !envValue(env, "DISPLAY").empty() || !envValue(env, "WAYLAND_DISPLAY").empty();
}

// Resolve's environment for the helper, minus what would point a GTK or Qt
// program at libraries that aren't its own: the loader overrides and Qt's
// plugin/QML paths (a Qt kdialog loading Resolve's Qt plugins is the
// decision-6 ABI clash in another process). Everything else (DISPLAY,
// XAUTHORITY, the session bus, the theme) is inherited, which is the point.
inline std::vector<std::string> childEnvironment(const std::vector<std::string>& env)
{
    static const char* const kDropped[] = {
        "LD_LIBRARY_PATH", "LD_PRELOAD", "QT_PLUGIN_PATH", "QT_QPA_PLATFORM_PLUGIN_PATH",
        "QML2_IMPORT_PATH", "QML_IMPORT_PATH"};
    std::vector<std::string> out;
    for (const std::string& kv : env) {
        const std::string name = kv.substr(0, kv.find('='));
        bool drop = false;
        for (const char* d : kDropped) {
            drop = drop || name == d;
        }
        if (!drop) out.push_back(kv);
    }
    return out;
}

// The native_open_file_dialog contract (MacFileDialog.h) over runDialog:
// true and outPath filled only on a pick; cancel and every failure return
// false with outPath untouched. *error gets the failure's one-line reason,
// and is cleared on a pick or a cancel. A path that doesn't fit is a
// failure, never a truncation.
inline bool openFileDialog(const char* message, const char* extension, const char* initialPath,
                           char* outPath, size_t outPathSize, const std::vector<std::string>& env,
                           const Spawner& spawn, std::string* error)
{
    error->clear();
    if (!outPath || outPathSize == 0) {
        *error = "Browse: no buffer for the picked path";
        return false;
    }
    Request request;
    request.title = message ? message : "";
    request.extensions = extension ? extension : "";
    request.initialPath = initialPath ? initialPath : "";
    request.home = envValue(env, "HOME");
    const Result r = runDialog(request, hasDisplay(env), spawn);
    if (r.outcome != Outcome::kPicked) {
        *error = r.error;
        return false;
    }
    if (r.path.size() + 1 > outPathSize) {
        *error = "Browse: the picked path is too long (" + std::to_string(r.path.size()) +
                 " bytes)";
        return false;
    }
    std::memcpy(outPath, r.path.c_str(), r.path.size() + 1);
    return true;
}

} // namespace ndi_linuxdlg

#ifdef __linux__

// Same contract as mac_open_file_dialog / win_open_file_dialog (extension may
// be a ';'-separated list; initialPath may be NULL/empty, and when set the
// dialog opens in its directory). Returns true and fills outPath (UTF-8)
// only when the user picks a file; cancel and every failure return false
// with outPath untouched. Blocks the calling thread until the helper exits,
// like the modal panels do; the helper is a separate process, so nothing in
// Resolve's own event loop is re-entered.
bool linux_open_file_dialog(const char* message, const char* extension,
                            const char* initialPath, char* outPath, size_t outPathSize);

// Why the calling thread's last linux_open_file_dialog failed (empty after
// a pick or a cancel), cleared by the read: the one line the plugin puts in
// Stream Status, e.g. the package to install when neither helper exists.
std::string linux_take_file_dialog_error();

#endif // __linux__

#endif // LINUX_FILE_DIALOG_H
