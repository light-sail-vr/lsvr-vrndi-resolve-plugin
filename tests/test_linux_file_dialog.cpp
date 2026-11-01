// Tests for the pure logic behind the Linux browse dialog
// (src/LinuxFileDialog.h, ticket #76): the zenity/kdialog argv, the parse of
// the helper's stdout and exit status, the zenity -> kdialog fallback and the
// missing-helper message, and the child environment. The spawner is
// injected, so nothing here launches a GUI; the real posix_spawn half
// (LinuxFileDialog.cpp) is covered by the Tier 1-2 loop on the box.
// Build & run: ctest (all CMake platforms; the header is host-independent);
// on macOS by hand:
//   c++ -std=c++17 -Isrc tests/test_linux_file_dialog.cpp -o build/t && build/t

#include "LinuxFileDialog.h"
#include "LinuxHostEvents.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using ndi_linuxdlg::Outcome;
using ndi_linuxdlg::Request;
using ndi_linuxdlg::SpawnResult;

static int failures = 0;

static void expectStr(const std::string& actual, const std::string& expected, const char* name)
{
    if (actual != expected) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n  expected: '%s'\n  actual:   '%s'\n",
                     name, expected.c_str(), actual.c_str());
    } else {
        std::printf("ok   %s\n", name);
    }
}

static void expectTrue(bool actual, const char* name)
{
    if (!actual) {
        ++failures;
        std::fprintf(stderr, "FAIL %s\n", name);
    } else {
        std::printf("ok   %s\n", name);
    }
}

static std::string joined(const std::vector<std::string>& argv)
{
    std::string s;
    for (const std::string& a : argv) {
        s += "[" + a + "]";
    }
    return s;
}

static Request request(const char* title, const char* extensions, const char* initialPath,
                       const char* home = "/home/matt")
{
    Request r;
    r.title = title;
    r.extensions = extensions;
    r.initialPath = initialPath;
    r.home = home;
    return r;
}

// zenity gets the title, one filter row for the extension (both cases:
// Linux filesystems are case-sensitive and cameras write .EXR), an "All
// files" escape hatch, and the field's file itself as --filename: zenity
// 4.0.1 hands it to gtk_file_chooser_set_file, which opens the file's folder
// with the file selected. A directory there (even with a trailing '/') is
// selected inside ITS parent instead (seen on the box, #76 Tier 2).
static void testZenityArgv()
{
    expectStr(joined(ndi_linuxdlg::zenityArgv(
                  request("Choose the left-eye STMap EXR", "exr", "/media/maps/left.exr"))),
              "[zenity][--file-selection][--modal][--title=Choose the left-eye STMap EXR]"
              "[--file-filter=EXR files | *.exr *.EXR][--file-filter=All files | *]"
              "[--filename=/media/maps/left.exr]",
              "zenity argv: title, filters, the field's file");

    // A ';'-separated list becomes one row; paths with spaces and UTF-8 go
    // through verbatim (no shell between us and the helper).
    expectStr(joined(ndi_linuxdlg::zenityArgv(
                  request("Choose a clip", "braw;ilpd", "/media/Bühne 2/A001 clip.braw"))),
              "[zenity][--file-selection][--modal][--title=Choose a clip]"
              "[--file-filter=BRAW/ILPD files | *.braw *.BRAW *.ilpd *.ILPD]"
              "[--file-filter=All files | *][--filename=/media/Bühne 2/A001 clip.braw]",
              "zenity argv: extension list, spaces and non-ASCII verbatim");

    // An empty field starts at the last file this process picked, so the
    // right eye's Browse opens where the left eye's left off.
    Request again = request("Choose the right-eye STMap EXR", "exr", "");
    again.lastPicked = "/media/Bühne 2/left eye.exr";
    expectStr(joined(ndi_linuxdlg::zenityArgv(again)),
              "[zenity][--file-selection][--modal][--title=Choose the right-eye STMap EXR]"
              "[--file-filter=EXR files | *.exr *.EXR][--file-filter=All files | *]"
              "[--filename=/media/Bühne 2/left eye.exr]",
              "zenity argv: empty field starts at the last pick");

    // Nothing picked yet: $HOME, not Resolve's working directory. (zenity
    // 4.0.1 shows it selected inside /home; newer zenity opens it.)
    expectStr(joined(ndi_linuxdlg::zenityArgv(request("Choose the LUT", "cube", ""))),
              "[zenity][--file-selection][--modal][--title=Choose the LUT]"
              "[--file-filter=CUBE files | *.cube *.CUBE][--file-filter=All files | *]"
              "[--filename=/home/matt/]",
              "zenity argv: nothing to start from opens $HOME");

    // A relative field value means nothing to the helper (its working
    // directory is Resolve's): treated as empty. No $HOME either: no
    // --filename at all.
    expectStr(joined(ndi_linuxdlg::zenityArgv(request("T", "cube", "lut.cube", ""))),
              "[zenity][--file-selection][--modal][--title=T]"
              "[--file-filter=CUBE files | *.cube *.CUBE][--file-filter=All files | *]",
              "zenity argv: relative field, no $HOME");
}

// kdialog (KDE) takes a positional start directory and a KDE-style filter
// ("patterns|label", one row per line), again with an "All files" row.
static void testKdialogArgv()
{
    expectStr(joined(ndi_linuxdlg::kdialogArgv(
                  request("Choose a clip", "braw;ilpd", "/media/Bühne 2/A001 clip.braw"))),
              "[kdialog][--title][Choose a clip][--getopenfilename][/media/Bühne 2/]"
              "[*.braw *.BRAW *.ilpd *.ILPD|BRAW/ILPD files\n*|All files]",
              "kdialog argv: title, start directory, filter");

    // The start directory is positional, so it must be there for the filter
    // to land in the right slot even with no field and no $HOME.
    expectStr(joined(ndi_linuxdlg::kdialogArgv(request("T", "exr", "", ""))),
              "[kdialog][--title][T][--getopenfilename][.][*.exr *.EXR|EXR files\n*|All files]",
              "kdialog argv: placeholder start directory");

    Request again = request("T", "exr", "");
    again.lastPicked = "/media/Bühne 2/left eye.exr";
    expectStr(joined(ndi_linuxdlg::kdialogArgv(again)),
              "[kdialog][--title][T][--getopenfilename][/media/Bühne 2/][*.exr *.EXR|EXR files\n*|All files]",
              "kdialog argv: empty field starts in the last pick's folder");
}

// A fake helper set: records every argv it is asked to run and answers per
// program name. A program missing from the map is "not installed".
struct FakeHelpers {
    struct Answer {
        SpawnResult::Status status;
        int exitCode;
        std::string out;
        std::string err;
    };
    std::vector<std::pair<std::string, Answer>> answers;
    std::vector<std::string> ran;

    ndi_linuxdlg::Spawner spawner()
    {
        return [this](const std::vector<std::string>& argv) {
            ran.push_back(argv.empty() ? std::string() : argv[0]);
            for (const auto& a : answers) {
                if (!argv.empty() && a.first == argv[0]) {
                    SpawnResult r;
                    r.status = a.second.status;
                    r.exitCode = a.second.exitCode;
                    r.out = a.second.out;
                    r.err = a.second.err;
                    return r;
                }
            }
            SpawnResult r;
            r.status = SpawnResult::kNotFound;
            return r;
        };
    }
};

static ndi_linuxdlg::Result run(FakeHelpers& helpers, bool haveDisplay = true)
{
    return ndi_linuxdlg::runDialog(request("Choose the LUT", "cube", "/luts/a.cube"),
                                   haveDisplay, helpers.spawner());
}

static void testPicked()
{
    FakeHelpers h;
    h.answers.push_back({"zenity", {SpawnResult::kExited, 0, "/luts/My Grade ü.cube\n", ""}});
    const ndi_linuxdlg::Result r = run(h);
    expectTrue(r.outcome == Outcome::kPicked, "zenity exit 0 is a pick");
    expectStr(r.path, "/luts/My Grade ü.cube", "picked path has the trailing newline stripped");
    expectStr(joined(h.ran), "[zenity]", "zenity alone runs when it is installed");
}

// Only the one newline zenity appends is stripped: a name may end in
// whitespace.
static void testOnlyOneNewlineStripped()
{
    FakeHelpers h;
    h.answers.push_back({"zenity", {SpawnResult::kExited, 0, "/luts/odd name \n", ""}});
    expectStr(run(h).path, "/luts/odd name ", "trailing space in a name survives");
}

static void testCancel()
{
    FakeHelpers h;
    h.answers.push_back({"zenity", {SpawnResult::kExited, 1, "", ""}});
    const ndi_linuxdlg::Result r = run(h);
    expectTrue(r.outcome == Outcome::kCancelled, "zenity exit 1 is a cancel");
    expectStr(r.path, "", "a cancel carries no path");
    expectStr(r.error, "", "a cancel is not an error");
    expectStr(joined(h.ran), "[zenity]", "a cancel doesn't fall back to kdialog");
}

static void testKdialogFallback()
{
    FakeHelpers h;
    h.answers.push_back({"kdialog", {SpawnResult::kExited, 0, "/luts/b.cube\n", ""}});
    const ndi_linuxdlg::Result r = run(h);
    expectTrue(r.outcome == Outcome::kPicked, "kdialog picks when zenity is missing");
    expectStr(r.path, "/luts/b.cube", "kdialog path parsed");
    expectStr(joined(h.ran), "[zenity][kdialog]", "zenity tried first, then kdialog");

    FakeHelpers c;
    c.answers.push_back({"kdialog", {SpawnResult::kExited, 1, "", ""}});
    expectTrue(run(c).outcome == Outcome::kCancelled, "kdialog exit 1 is a cancel");
}

// Neither helper: fail, and say which package to install (spec decision 18).
static void testNoHelper()
{
    FakeHelpers h;
    const ndi_linuxdlg::Result r = run(h);
    expectTrue(r.outcome == Outcome::kFailed, "no helper is a failure");
    expectTrue(r.error.find("zenity") != std::string::npos &&
                   r.error.find("kdialog") != std::string::npos &&
                   r.error.find("install") != std::string::npos,
               "the message names the packages to install");
    expectStr(joined(h.ran), "[zenity][kdialog]", "both helpers were tried");

    // A shell-style 127 ("command not found") counts as missing too.
    FakeHelpers s;
    s.answers.push_back({"zenity", {SpawnResult::kExited, 127, "", ""}});
    s.answers.push_back({"kdialog", {SpawnResult::kExited, 0, "/luts/c.cube\n", ""}});
    expectStr(run(s).path, "/luts/c.cube", "exit 127 falls back like a missing helper");
}

// zenity exits 1 when it can't open a display, which would read as a
// silent cancel (verified on the box), so no display is checked first.
static void testNoDisplay()
{
    FakeHelpers h;
    h.answers.push_back({"zenity", {SpawnResult::kExited, 1, "", "Failed to open display"}});
    const ndi_linuxdlg::Result r = run(h, false);
    expectTrue(r.outcome == Outcome::kFailed, "no display is a failure, not a cancel");
    expectTrue(r.error.find("DISPLAY") != std::string::npos, "the message says why");
    expectStr(joined(h.ran), "", "nothing is spawned without a display");
}

static void testHelperErrors()
{
    FakeHelpers h;
    h.answers.push_back({"zenity", {SpawnResult::kExited, 255, "", "zenity: bad option\nmore"}});
    ndi_linuxdlg::Result r = run(h);
    expectTrue(r.outcome == Outcome::kFailed, "an unexpected exit status is a failure");
    expectTrue(r.error.find("255") != std::string::npos &&
                   r.error.find("zenity: bad option") != std::string::npos &&
                   r.error.find("more") == std::string::npos,
               "the error names the status and the first stderr line");
    expectStr(joined(h.ran), "[zenity]", "a helper that ran and failed is not retried");

    FakeHelpers e;
    e.answers.push_back({"zenity", {SpawnResult::kExited, 0, "\n", ""}});
    expectTrue(run(e).outcome == Outcome::kFailed, "exit 0 with no path is a failure");

    FakeHelpers rel;
    rel.answers.push_back({"zenity", {SpawnResult::kExited, 0, "luts/a.cube\n", ""}});
    expectTrue(run(rel).outcome == Outcome::kFailed, "a relative path is a failure");

    FakeHelpers spawn;
    spawn.answers.push_back({"zenity", {SpawnResult::kFailed, 0, "", "Resource temporarily unavailable"}});
    r = run(spawn);
    expectTrue(r.outcome == Outcome::kFailed &&
                   r.error.find("Resource temporarily unavailable") != std::string::npos,
               "a spawn failure is a failure with its reason");
}

static const std::vector<std::string> kDesktopEnv = {
    "HOME=/home/matt", "DISPLAY=:0", "PATH=/usr/bin:/bin"};

// The native_open_file_dialog contract (MacFileDialog.h): true and outPath
// filled only on a pick; cancel and every failure return false with outPath
// untouched.
static void testContract()
{
    FakeHelpers h;
    h.answers.push_back({"zenity", {SpawnResult::kExited, 0, "/maps/l.exr\n", ""}});
    char buf[64];
    std::strcpy(buf, "sentinel");
    ndi_linuxdlg::DialogState state;
    state.error = "stale";
    expectTrue(ndi_linuxdlg::openFileDialog("T", "exr", "/maps/old.exr", buf, sizeof(buf),
                                            kDesktopEnv, h.spawner(), &state),
               "a pick returns true");
    expectStr(buf, "/maps/l.exr", "a pick fills the buffer");
    expectStr(state.error, "", "a pick clears the error");
    expectStr(state.lastPicked, "/maps/l.exr", "a pick is remembered for the next Browse");

    FakeHelpers c;
    c.answers.push_back({"zenity", {SpawnResult::kExited, 1, "", ""}});
    std::strcpy(buf, "sentinel");
    expectTrue(!ndi_linuxdlg::openFileDialog("T", "exr", nullptr, buf, sizeof(buf), kDesktopEnv,
                                             c.spawner(), &state),
               "a cancel returns false");
    expectStr(buf, "sentinel", "a cancel leaves the buffer untouched");
    expectStr(state.error, "", "a cancel leaves no error");
    expectStr(state.lastPicked, "/maps/l.exr", "a cancel keeps the last pick");

    FakeHelpers none;
    std::strcpy(buf, "sentinel");
    expectTrue(!ndi_linuxdlg::openFileDialog("T", "exr", "", buf, sizeof(buf), kDesktopEnv,
                                             none.spawner(), &state),
               "no helper returns false");
    expectStr(buf, "sentinel", "no helper leaves the buffer untouched");
    expectTrue(state.error.find("install") != std::string::npos, "no helper reports the install hint");

    // Too long for the caller's buffer: a failure, never a truncation.
    FakeHelpers big;
    big.answers.push_back({"zenity", {SpawnResult::kExited, 0, "/" + std::string(70, 'x') + "\n", ""}});
    std::strcpy(buf, "sentinel");
    expectTrue(!ndi_linuxdlg::openFileDialog("T", "exr", "", buf, sizeof(buf), kDesktopEnv,
                                             big.spawner(), &state),
               "a path too long for the buffer returns false");
    expectStr(buf, "sentinel", "a too-long path leaves the buffer untouched");
    expectTrue(!state.error.empty(), "a too-long path is reported");

    // The last pick seeds an empty field's start...
    std::vector<std::string> argv;
    auto record = [&](const std::vector<std::string>& a) {
        argv = a;
        SpawnResult r;
        r.status = SpawnResult::kExited;
        r.exitCode = 1;
        return r;
    };
    ndi_linuxdlg::openFileDialog("T", "exr", "", buf, sizeof(buf), kDesktopEnv, record, &state);
    expectStr(argv.empty() ? std::string() : argv.back(), "--filename=/maps/l.exr",
              "an empty field starts at the last pick");

    // ...and $HOME from the environment when nothing was picked yet.
    state = ndi_linuxdlg::DialogState();
    ndi_linuxdlg::openFileDialog("T", "exr", "", buf, sizeof(buf), kDesktopEnv,
                                 [&](const std::vector<std::string>& a) {
                                     argv = a;
                                     SpawnResult r;
                                     r.status = SpawnResult::kExited;
                                     r.exitCode = 1;
                                     return r;
                                 },
                                 &state);
    expectStr(argv.empty() ? std::string() : argv.back(), "--filename=/home/matt/",
              "$HOME comes from the environment");
}

static void testDisplayDetection()
{
    expectTrue(ndi_linuxdlg::hasDisplay({"DISPLAY=:0"}), "DISPLAY is a display");
    expectTrue(ndi_linuxdlg::hasDisplay({"WAYLAND_DISPLAY=wayland-0"}), "WAYLAND_DISPLAY is a display");
    expectTrue(!ndi_linuxdlg::hasDisplay({"DISPLAY="}), "an empty DISPLAY is not");
    expectTrue(!ndi_linuxdlg::hasDisplay({"HOME=/home/matt", "XDISPLAY=:0"}), "no display variable");
}

// The helper inherits Resolve's environment (DISPLAY, XAUTHORITY, the
// session bus) minus the variables that would point a GTK or Qt program at
// libraries that aren't its own.
static void testChildEnvironment()
{
    expectStr(joined(ndi_linuxdlg::childEnvironment(
                  {"DISPLAY=:0", "LD_LIBRARY_PATH=/opt/resolve/libs", "XAUTHORITY=/run/x",
                   "LD_PRELOAD=/opt/x.so", "QT_PLUGIN_PATH=/opt/resolve/libs/plugins",
                   "QT_QPA_PLATFORM_PLUGIN_PATH=/opt/p", "QML2_IMPORT_PATH=/opt/q",
                   "QT_SCALE_FACTOR=2", "LD_LIBRARY_PATHX=keep"})),
              "[DISPLAY=:0][XAUTHORITY=/run/x][QT_SCALE_FACTOR=2][LD_LIBRARY_PATHX=keep]",
              "child environment drops the loader and Qt plugin paths only");
}

// The host event pump (src/LinuxHostEvents.h): while the helper runs, the
// plugin lets Resolve's Qt event loop handle everything but user input, so
// Resolve repaints and answers the window manager's ping instead of going
// "not responding" (#76 Tier 2). Resolved by symbol at runtime; any doubt
// means no pump, i.e. the plain blocking wait.
static int gPumpCalls = 0;
static int gPumpFlags = -1;
static void fakeProcessEvents(int flags)
{
    ++gPumpCalls;
    gPumpFlags = flags;
}
static void* gFakeApp = &gPumpCalls; // any non-null QCoreApplication*
static void* gNoApp = nullptr;

static ndi_linux_host::Lookup lookupWith(bool haveProcessEvents, void** self)
{
    return [=](const char* name) -> void* {
        if (std::strcmp(name, ndi_linux_host::kProcessEventsSymbol) == 0) {
            return haveProcessEvents ? reinterpret_cast<void*>(&fakeProcessEvents) : nullptr;
        }
        if (std::strcmp(name, ndi_linux_host::kAppSelfSymbol) == 0) {
            return self;
        }
        return nullptr;
    };
}

static void testEventPump()
{
    expectStr(ndi_linux_host::kProcessEventsSymbol,
              "_ZN16QCoreApplication13processEventsE6QFlagsIN10QEventLoop17ProcessEventsFlagEE",
              "processEvents is looked up by its Qt 5.15 symbol (nm -D on Resolve's libQt5Core)");

    gPumpCalls = 0;
    ndi_linux_host::EventPump pump = ndi_linux_host::resolveEventPump(lookupWith(true, &gFakeApp));
    pump.pump(true);
    expectTrue(gPumpCalls == 1, "the pump runs Qt's processEvents on the main thread");
    expectTrue(gPumpFlags == 1, "with ExcludeUserInputEvents (0x01) only");

    gPumpCalls = 0;
    pump.pump(false);
    expectTrue(gPumpCalls == 0, "never off the main thread (Qt's loop belongs to it)");

    gPumpCalls = 0;
    ndi_linux_host::resolveEventPump(lookupWith(true, &gNoApp)).pump(true);
    expectTrue(gPumpCalls == 0, "no pump before a QCoreApplication exists");

    gPumpCalls = 0;
    ndi_linux_host::resolveEventPump(lookupWith(true, nullptr)).pump(true);
    expectTrue(gPumpCalls == 0, "no pump without QCoreApplication::self");

    gPumpCalls = 0;
    ndi_linux_host::resolveEventPump(lookupWith(false, &gFakeApp)).pump(true);
    expectTrue(gPumpCalls == 0, "no pump in a host without Qt (plain blocking wait)");
}

int main()
{
    testZenityArgv();
    testKdialogArgv();
    testPicked();
    testOnlyOneNewlineStripped();
    testCancel();
    testKdialogFallback();
    testNoHelper();
    testNoDisplay();
    testHelperErrors();
    testContract();
    testDisplayDetection();
    testChildEnvironment();
    testEventPump();

    if (failures) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
