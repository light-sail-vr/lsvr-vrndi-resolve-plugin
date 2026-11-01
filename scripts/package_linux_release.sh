#!/usr/bin/env bash
# Build the Linux release artifacts (ticket #78; docs/linux-port-spec.md
# decisions 5, 11, 21-22): the Linux third of what scripts/package_release.sh
# (macOS) and scripts/package_windows_release.ps1 produce. One VERSION, one
# CHANGELOG, one release event with per-platform artifacts.
#
#   scripts/package_linux_release.sh [--cpu-only] [--sdk PATH] [--allow-old-sdk] [--force]
#       Release mode, on the Linux box (Docker + the Standard NDI SDK for Linux):
#       a fresh build in the Rocky Linux 8 container (spec decision 5) with
#       -DNDI_ENABLE_CUDA=ON, ctest, cmake --install into stage/, then the
#       packaging steps below. It deletes build/ and stage/ first, so in a git
#       checkout it refuses uncommitted changes (--force: a test package only).
#   scripts/package_linux_release.sh --stage DIR [--cpu-only] [--allow-stub] [--elf-checker PATH]
#       Package an already-staged tree (CI's -STUB tarball, the tests).
#
# Packaging refuses a stub libndi.so.6 (the CI stub exports
# ndioutput_stub_libndi) unless --allow-stub, which names the tarball -STUB;
# refuses an NDI runtime more than 30 days old (spec decision 11; the date is
# read from the staged libndi.so.6 itself) unless --allow-old-sdk, and always
# refuses an SDK date it can't parse or one after today; and runs
# test_plugin_elf on the staged .ofx. Output in dist/v<VERSION>/ (--out DIR):
#   NDIOutput-<VERSION>-linux-x86_64.tar.gz   bundle + install.sh/uninstall.sh + README
#   SHA256SUMS-Linux.txt                      (SHA256SUMS.txt is the macOS file in the same folder)
#
# Options:
#   --cpu-only       build with -DNDI_ENABLE_CUDA=OFF (no GPU-native path) and
#                    name the tarball ...-linux-x86_64-cpu-only.tar.gz, so a
#                    CPU-only release says so (the release notes do too)
#   --sdk PATH       NDI SDK for Linux [$NDI_SDK_PATH, else ~/sdks/NDI SDK for Linux]
#   --elf-checker P  test_plugin_elf binary [build/test_plugin_elf]
#   --out DIR        output folder [dist/v<VERSION>]
#   --force          release mode in a git checkout with uncommitted changes
#                    (a test package, never a release)
# Release order and the Linux hand-off: docs/RELEASING.md.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
VERSION="$(tr -d '[:space:]' < "$REPO/VERSION")"
MAX_SDK_AGE_DAYS=30
BASE_IMAGE="nvidia/cuda:12.9.1-devel-rockylinux8"   # = linux.yml, linux_remote.sh
BUILD_IMAGE="ndioutput-build:rocky8"

STAGE=""
SDK="${NDI_SDK_PATH:-$HOME/sdks/NDI SDK for Linux}"
CUDA=ON
CHECKER=""
OUT=""
ALLOW_STUB=0
ALLOW_OLD_SDK=0
FORCE=0
while [ $# -gt 0 ]; do
    case "$1" in
        --stage) STAGE="$2"; shift ;;
        --sdk) SDK="$2"; shift ;;
        --cpu-only) CUDA=OFF ;;
        --elf-checker) CHECKER="$2"; shift ;;
        --out) OUT="$2"; shift ;;
        --allow-stub) ALLOW_STUB=1 ;;
        --allow-old-sdk) ALLOW_OLD_SDK=1 ;;
        --force) FORCE=1 ;;
        -h|--help) sed -n '2,/^# Release order/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "Unknown option: $1 (see --help)" >&2; exit 2 ;;
    esac
    shift
done
OUT="${OUT:-$REPO/dist/v$VERSION}"
CHECKER="${CHECKER:-$REPO/build/test_plugin_elf}"

fail() { echo "package_linux_release: $*" >&2; exit 1; }

# GNU date on Linux; BSD date so the tests also run on the Mac. Prints the
# epoch seconds, or fails: never prints anything but a number.
epoch_of() {  # epoch_of <date text> [<BSD input format>]
    local e
    e="$(date -u -d "$1" +%s 2>/dev/null || date -u -j -f "${2:-%Y-%m-%d}" "$1" +%s 2>/dev/null)" || return 1
    [[ "$e" =~ ^-?[0-9]+$ ]] || return 1
    echo "$e"
}

# The NDI SDK stamps its build date into libndi.so.6:
# "NDI SDK LINUX 12:51:52 Apr 13 2026 6.3.2.0".
sdk_date() {  # sdk_date <libndi.so.6>: prints YYYY-MM-DD; nothing if there's no stamp; fails if it can't parse it
    local stamp mon day year e
    stamp="$(LC_ALL=C grep -a -o -m1 'NDI SDK LINUX [0-9:]* [A-Z][a-z][a-z] [ 0-9][0-9] [0-9][0-9][0-9][0-9]' "$1")" || return 0
    [ -n "$stamp" ] || return 0
    read -r _ _ _ _ mon day year <<<"$stamp"   # the first stamp, if a line holds several
    e="$(epoch_of "$mon $day $year" "%b %d %Y")" || return 1
    date -u -d "@$e" +%F 2>/dev/null || date -u -r "$e" +%F
}

# Spec decision 11 (Windows decision 13): the shipped NDI runtime is no more
# than 30 days old at release. Fails closed: a date it can't parse, or an SDK
# dated after today, is refused even with --allow-old-sdk.
check_sdk_age() {  # check_sdk_age <libndi.so.6>
    local d today today_e sdk_e age
    d="$(sdk_date "$1")" || fail "can't parse the NDI SDK build stamp in $1"
    if [ -z "$d" ]; then
        [ "$ALLOW_OLD_SDK" -eq 1 ] || fail "can't read the NDI SDK build date from $1 (pass --allow-old-sdk to package anyway)"
        echo "WARNING: NDI SDK build date unreadable in $1; packaging anyway (--allow-old-sdk)."
        return
    fi
    # NDIOUTPUT_TODAY is TEST-ONLY (tests/test_package_linux_release.sh pins
    # "today" with it); never set it for a release.
    today="${NDIOUTPUT_TODAY:-$(date -u +%F)}"
    today_e="$(epoch_of "$today")" || fail "can't parse today's date ($today)"
    sdk_e="$(epoch_of "$d")" || fail "can't parse the NDI SDK date ($d) from $1"
    age=$(( (today_e - sdk_e) / 86400 ))
    if [ "$age" -lt 0 ]; then
        fail "the NDI SDK in $1 is dated $d, after today ($today): check the clock and the SDK. A future-dated SDK is never packaged."
    fi
    if [ "$age" -le "$MAX_SDK_AGE_DAYS" ]; then
        echo "  [ok] NDI SDK built $d ($age days old)"
        return
    fi
    local msg="the NDI SDK in this build is from $d, $age days old; a release needs one no more than $MAX_SDK_AGE_DAYS days old. Download the current Standard NDI SDK for Linux (https://ndi.video/for-developers/) and rebuild"
    [ "$ALLOW_OLD_SDK" -eq 1 ] || fail "$msg (or pass --allow-old-sdk for a test package)."
    echo "WARNING: $msg. Packaging anyway (--allow-old-sdk): not for release."
}

# ------------------------------------------------------- release-mode build ----
if [ -z "$STAGE" ]; then
    # Release mode deletes build/ and stage/ and must build committed source.
    # A folder rsync'd from the Mac has no .git: RELEASING step 4a checks the
    # Mac checkout is clean before it syncs.
    if [ "$FORCE" -eq 1 ]; then
        echo "WARNING: --force: not checking $REPO for uncommitted changes. Not for release."
    elif [ -e "$REPO/.git" ]; then
        command -v git >/dev/null 2>&1 ||
            fail "$REPO is a git checkout but git isn't installed, so uncommitted changes can't be ruled out (--force for a test package)"
        dirty="$(git -C "$REPO" status --porcelain 2>&1)" ||
            fail "git status failed in $REPO: $dirty"
        [ -z "$dirty" ] || fail "uncommitted changes in $REPO:
$dirty
A release builds committed source, and this deletes build/ and stage/. Commit or stash them, or pass --force for a test package."
    else
        echo "  [info] $REPO is not a git checkout (a release folder synced from the Mac, whose checkout RELEASING step 4a checks)"
    fi
    command -v docker >/dev/null 2>&1 || fail "release mode builds in Docker (or pass --stage DIR)"
    [ -f "$SDK/include/Processing.NDI.Lib.h" ] && [ -f "$SDK/lib/x86_64-linux-gnu/libndi.so.6" ] ||
        fail "no NDI SDK for Linux at $SDK (pass --sdk PATH or set NDI_SDK_PATH)"
    src_ver="$(sed -n 's/^#define kPluginVersionString "\(.*\)"/\1/p' "$REPO/src/NDIOutputPlugin.cpp")"
    [ "$src_ver" = "$VERSION" ] || fail "VERSION ($VERSION) != kPluginVersionString ($src_ver): run scripts/set_version.sh"
    echo "Checking the NDI SDK before building:"
    check_sdk_age "$SDK/lib/x86_64-linux-gnu/libndi.so.6"   # fail before a 10-minute build
    if ! docker image inspect "$BUILD_IMAGE" >/dev/null 2>&1; then
        echo "== one-time: $BUILD_IMAGE = $BASE_IMAGE + scripts/linux_build_deps.sh"
        docker rm -f ndioutput-build-prep >/dev/null 2>&1 || true
        docker run --name ndioutput-build-prep -v "$REPO/scripts:/scripts:ro" "$BASE_IMAGE" \
            bash /scripts/linux_build_deps.sh
        docker commit ndioutput-build-prep "$BUILD_IMAGE" >/dev/null
        docker rm ndioutput-build-prep >/dev/null
    fi
    if [ "$CUDA" = ON ]; then
        echo "== fresh Rocky 8 container build of v$VERSION with the CUDA GPU-native pipeline"
        cuda_status="build with the CUDA GPU-native pipeline"
    else
        echo "== fresh Rocky 8 container build of v$VERSION, CPU-only (--cpu-only)"
        cuda_status="CPU-only build"
    fi
    # A fresh tree: a release never inherits a dev build's cache (CUDA, SDK path).
    rm -rf "$REPO/build" "$REPO/stage"
    docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e NDI_CUDA="$CUDA" \
        -e NDI_CUDA_STATUS="$cuda_status" \
        -v "$REPO:/src" -v "$SDK:/ndi:ro" -w /src "$BUILD_IMAGE" bash -c '
        set -euo pipefail
        source /opt/rh/gcc-toolset-13/enable
        cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNDI_SDK_PATH=/ndi \
            -DNDI_ENABLE_CUDA="$NDI_CUDA" | tee build-configure.log
        grep -q "NDI: using installed SDK" build-configure.log
        grep -q "$NDI_CUDA_STATUS" build-configure.log
        rm -f build-configure.log
        cmake --build build --parallel "$(nproc)"
        ctest --test-dir build --output-on-failure
        cmake --install build --prefix stage >/dev/null'
    STAGE="$REPO/stage"
fi

# ------------------------------------------------------------- packaging ----
bundle="$STAGE/NDIOutput.ofx.bundle"
bin="$bundle/Contents/Linux-x86-64"
echo "Packaging v$VERSION from $STAGE:"
[ -f "$bin/NDIOutput.ofx" ] || fail "no staged plugin at $bin/NDIOutput.ofx (cmake --install build --prefix stage)"
[ -f "$bin/libndi.so.6" ] || fail "no libndi.so.6 in $bin"

suffix=""
if LC_ALL=C grep -a -q ndioutput_stub_libndi "$bin/libndi.so.6"; then
    [ "$ALLOW_STUB" -eq 1 ] || fail "$bin/libndi.so.6 is the CI stub, not the NDI runtime: it loads but never streams. Build against the Standard NDI SDK for Linux (or pass --allow-stub for a -STUB pipeline package)."
    suffix="-STUB"
    echo "WARNING: packaging a STUB build (--allow-stub). It installs and loads but never streams. Never release it."
else
    licenses=("$bin"/*licenses*.txt)
    [ -f "${licenses[0]}" ] || fail "no NDI licenses file (*licenses*.txt) beside libndi.so.6 in $bin (spec decision 11)"
    check_sdk_age "$bin/libndi.so.6"
fi

[ -x "$CHECKER" ] || fail "no ELF checker at $CHECKER (build test_plugin_elf, or pass --elf-checker)"
echo "== test_plugin_elf on the staged .ofx"
"$CHECKER" "$bin/NDIOutput.ofx" || fail "test_plugin_elf rejected $bin/NDIOutput.ofx"

[ "$CUDA" = ON ] || suffix="-cpu-only$suffix"
name="NDIOutput-$VERSION-linux-x86_64$suffix"
mkdir -p "$OUT"
OUT="$(cd "$OUT" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
top="$work/$name"
mkdir -p "$top"
cp -R "$bundle" "$top/"
cp "$REPO/installer/linux/install.sh" "$REPO/installer/linux/uninstall.sh" \
   "$REPO/installer/linux/README.txt" "$top/"
cp "$REPO/LICENSE" "$top/LICENSE.txt"
if [ -z "$suffix" ]; then cp "${licenses[@]}" "$top/"; fi
chmod -R u=rwX,go=rX "$top"
chmod 755 "$top/install.sh" "$top/uninstall.sh" "$top/NDIOutput.ofx.bundle/Contents/Linux-x86-64/"{*.ofx,libndi.so.6}

rm -f "$OUT/$name.tar.gz"
if tar --version 2>/dev/null | grep -q GNU; then
    tar -C "$work" --owner=0 --group=0 --numeric-owner --sort=name -czf "$OUT/$name.tar.gz" "$name"
else
    tar -C "$work" --uid 0 --gid 0 -czf "$OUT/$name.tar.gz" "$name"
fi
(
    cd "$OUT"
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$name.tar.gz"
    else shasum -a 256 "$name.tar.gz"; fi
) > "$OUT/SHA256SUMS-Linux.txt"

echo
echo "  built:     $OUT/$name.tar.gz"
echo "  checksums: $OUT/SHA256SUMS-Linux.txt"
