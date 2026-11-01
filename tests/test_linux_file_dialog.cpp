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
// files" escape hatch, and the field's directory with a trailing slash so
// GTK opens IN it rather than selecting a file named after it.
static void testZenityArgv()
{
    expectStr(joined(ndi_linuxdlg::zenityArgv(
                  request("Choose the left-eye STMap EXR", "exr", "/media/maps/left.exr"))),
              "[zenity][--file-selection][--modal][--title=Choose the left-eye STMap EXR]"
              "[--file-filter=EXR files | *.exr *.EXR][--file-filter=All files | *]"
              "[--filename=/media/maps/]",
              "zenity argv: title, filters, start directory");

    // A ';'-separated list becomes one row; paths with spaces and UTF-8 go
    // through verbatim (no shell between us and the helper).
    expectStr(joined(ndi_linuxdlg::zenityArgv(
                  request("Choose a clip", "braw;ilpd", "/media/Bühne 2/A001 clip.braw"))),
              "[zenity][--file-selection][--modal][--title=Choose a clip]"
              "[--file-filter=BRAW/ILPD files | *.braw *.BRAW *.ilpd *.ILPD]"
              "[--file-filter=All files | *][--filename=/media/Bühne 2/]",
              "zenity argv: extension list, spaces and non-ASCII verbatim");

    // An empty field opens in $HOME, not in Resolve's working directory.
    expectStr(joined(ndi_linuxdlg::zenityArgv(request("Choose the LUT", "cube", ""))),
              "[zenity][--file-selection][--modal][--title=Choose the LUT]"
              "[--file-filter=CUBE files | *.cube *.CUBE][--file-filter=All files | *]"
              "[--filename=/home/matt/]",
              "zenity argv: empty field opens in $HOME");

    // A bare name with no directory and no $HOME: no --filename at all.
    expectStr(joined(ndi_linuxdlg::zenityArgv(request("T", "cube", "lut.cube", ""))),
              "[zenity][--file-selection][--modal][--title=T]"
              "[--file-filter=CUBE files | *.cube *.CUBE][--file-filter=All files | *]",
              "zenity argv: no directory, no $HOME");
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
    std::string error = "stale";
    expectTrue(ndi_linuxdlg::openFileDialog("T", "exr", "/maps/old.exr", buf, sizeof(buf),
                                            kDesktopEnv, h.spawner(), &error),
               "a pick returns true");
    expectStr(buf, "/maps/l.exr", "a pick fills the buffer");
    expectStr(error, "", "a pick clears the error");

    FakeHelpers c;
    c.answers.push_back({"zenity", {SpawnResult::kExited, 1, "", ""}});
    std::strcpy(buf, "sentinel");
    expectTrue(!ndi_linuxdlg::openFileDialog("T", "exr", nullptr, buf, sizeof(buf), kDesktopEnv,
                                             c.spawner(), &error),
               "a cancel returns false");
    expectStr(buf, "sentinel", "a cancel leaves the buffer untouched");
    expectStr(error, "", "a cancel leaves no error");

    FakeHelpers none;
    std::strcpy(buf, "sentinel");
    expectTrue(!ndi_linuxdlg::openFileDialog("T", "exr", "", buf, sizeof(buf), kDesktopEnv,
                                             none.spawner(), &error),
               "no helper returns false");
    expectStr(buf, "sentinel", "no helper leaves the buffer untouched");
    expectTrue(error.find("install") != std::string::npos, "no helper reports the install hint");

    // Too long for the caller's buffer: a failure, never a truncation.
    FakeHelpers big;
    big.answers.push_back({"zenity", {SpawnResult::kExited, 0, "/" + std::string(70, 'x') + "\n", ""}});
    std::strcpy(buf, "sentinel");
    expectTrue(!ndi_linuxdlg::openFileDialog("T", "exr", "", buf, sizeof(buf), kDesktopEnv,
                                             big.spawner(), &error),
               "a path too long for the buffer returns false");
    expectStr(buf, "sentinel", "a too-long path leaves the buffer untouched");
    expectTrue(!error.empty(), "a too-long path is reported");

    // $HOME from the environment seeds the start directory.
    FakeHelpers seen;
    std::vector<std::string> argv;
    ndi_linuxdlg::openFileDialog("T", "exr", "", buf, sizeof(buf), kDesktopEnv,
                                 [&](const std::vector<std::string>& a) {
                                     argv = a;
                                     SpawnResult r;
                                     r.status = SpawnResult::kExited;
                                     r.exitCode = 1;
                                     return r;
                                 },
                                 &error);
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

    if (failures) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }
    std::printf("all passed\n");
    return 0;
}
