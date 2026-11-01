#!/usr/bin/env bash
# VR.NDI Sender for DaVinci Resolve: Linux installer (ticket #78).
#
#   sudo ./install.sh              install or replace /usr/OFX/Plugins/NDIOutput.ofx.bundle
#   sudo ./install.sh --uninstall  remove it (what uninstall.sh runs)
set -euo pipefail

# Test hooks (tests/test_linux_installer.sh): NDIOUTPUT_ROOT is a fake "/",
# NDIOUTPUT_TEST_EUID a fake uid. Neither is for end users.
ROOT="${NDIOUTPUT_ROOT:-}"
EUID_NOW="${NDIOUTPUT_TEST_EUID:-$(id -u)}"

mode=install
case "${1:-}" in
    "") ;;
    --uninstall) mode=uninstall ;;
    -h|--help) sed -n '2,5p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "usage: sudo $0 [--uninstall]" >&2; exit 2 ;;
esac

if [ "$EUID_NOW" -ne 0 ]; then
    echo "This needs root to write /usr/OFX/Plugins: run it again with sudo." >&2
    exit 1
fi

HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/NDIOutput.ofx.bundle"
DEST_DIR="$ROOT/usr/OFX/Plugins"
DEST="$DEST_DIR/NDIOutput.ofx.bundle"

# Resolve by argv[0]: its main thread renames itself "GUI Thread" and it runs
# under `sg resolve`, so neither the process name nor /proc/<pid>/exe works
# (LEARNINGS 2026-10-30).
resolve_running() {
    local p argv0
    for p in "$ROOT"/proc/[0-9]*; do
        argv0="$(tr '\000' '\n' 2>/dev/null < "$p/cmdline" | head -n 1)" || continue
        case "$argv0" in */bin/resolve) return 0 ;; esac
    done
    return 1
}

# Resolve's plugin cache lives in the desktop user's home, not root's.
cache_path() {
    local home=""
    if [ -n "${SUDO_USER:-}" ] && [ "$SUDO_USER" != root ]; then
        home="$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6)" || home=""
    fi
    echo "${home:-~<your user>}/.local/share/DaVinciResolve/configs/OFXPluginCacheV2.xml"
}

restart_hints() {
    echo
    if resolve_running; then
        echo "DaVinci Resolve is running: quit it fully and start it again."
    fi
    echo "Next: restart DaVinci Resolve (OpenFX plugins are only scanned at startup)."
    echo "If the plugin doesn't appear (or an old one lingers), quit Resolve and delete"
    echo "  $(cache_path)"
    echo "then start Resolve again; it rescans every OpenFX plugin."
}

# --- prerequisite checks: informational, never fatal --------------------------
# The plugin loads without any of these; it just can't stream (or be found)
# until they're fixed, and it can't tell the user about the daemon itself.

# libndi.so.6 NEEDs both Avahi client libraries (spec decision 10). Without
# them the plugin passes frames through with "NDI off - Avahi client library
# missing" in Stream Status.
lib_present() {
    local d
    for d in usr/lib/x86_64-linux-gnu lib/x86_64-linux-gnu usr/lib64 lib64 usr/lib lib; do
        [ -e "$ROOT/$d/$1" ] && return 0
    done
    [ -z "$ROOT" ] && command -v ldconfig >/dev/null 2>&1 &&
        ldconfig -p 2>/dev/null | grep -q "^[[:space:]]*$1 "
}

check_avahi_libs() {
    local lib missing=()
    for lib in libavahi-client.so.3 libavahi-common.so.3; do
        lib_present "$lib" || missing+=("$lib")
    done
    if [ "${#missing[@]}" -eq 0 ]; then
        echo "  [ok] Avahi client libraries"
        return
    fi
    echo "  WARNING: the Avahi client libraries are missing (${missing[*]})."
    echo "           The NDI runtime needs them: without them the plugin loads but"
    echo "           cannot stream. Install them, then restart Resolve:"
    echo "             Ubuntu/Debian:      sudo apt install libavahi-client3"
    echo "             Rocky/RHEL/Alma:    sudo dnf install avahi-libs"
}

# NDI on Linux advertises through avahi-daemon only: with it stopped the
# plugin streams, but no receiver can discover the source (LEARNINGS 2026-10-30).
daemon_running() {
    local c
    for c in "$ROOT"/proc/[0-9]*/comm; do
        [ "$(cat "$c" 2>/dev/null)" = avahi-daemon ] && return 0
    done
    return 1
}

check_avahi_daemon() {
    if daemon_running; then
        echo "  [ok] avahi-daemon is running"
        return
    fi
    if [ -e "$ROOT/usr/sbin/avahi-daemon" ] || [ -e "$ROOT/sbin/avahi-daemon" ]; then
        echo "  WARNING: avahi-daemon is not running. NDI sources are only discoverable"
        echo "           while it runs. Start it (and keep it enabled):"
    else
        echo "  WARNING: avahi-daemon is not installed. NDI sources are only discoverable"
        echo "           while it runs. Install and start it:"
        echo "             Ubuntu/Debian:      sudo apt install avahi-daemon"
        echo "             Rocky/RHEL/Alma:    sudo dnf install avahi"
    fi
    echo "             sudo systemctl enable --now avahi-daemon"
    echo "           Never stop avahi-daemon while Resolve is open: Resolve crashes."
}

# Informational: the GPU-native path needs an NVIDIA driver; anything else
# runs the CPU path (stream at Half or Quarter there).
check_nvidia() {
    local v=""
    if [ -r "$ROOT/proc/driver/nvidia/version" ]; then
        v="$(sed -n 's/.*Kernel Module[[:space:]]*\([0-9][0-9.]*\).*/\1/p' \
            "$ROOT/proc/driver/nvidia/version" | head -n 1)"
    fi
    if [ -n "$v" ]; then
        echo "  [info] NVIDIA driver: $v"
    else
        echo "  [info] NVIDIA driver: none loaded. The plugin uses its CPU path, which has a"
        echo "         lower resolution ceiling (stream at Half or Quarter)."
    fi
}

check_stub() {
    if grep -q ndioutput_stub_libndi "$SRC/Contents/Linux-x86-64/libndi.so.6" 2>/dev/null; then
        echo "  WARNING: this is a STUB build (CI pipeline proof). Its libndi.so.6 has no"
        echo "           NDI code: the plugin loads but never streams. Do not ship it."
    fi
}

do_uninstall() {
    if [ ! -e "$DEST" ]; then
        echo "VR.NDI Sender is not installed ($DEST is absent); nothing to remove."
        exit 0
    fi
    # Removing a loaded plugin is safe on Linux: a running Resolve keeps its
    # mapped copy until it exits.
    rm -rf "$DEST"
    echo "Removed $DEST"
    restart_hints
}

do_install() {
    if [ ! -f "$SRC/Contents/Linux-x86-64/NDIOutput.ofx" ]; then
        echo "No plugin bundle beside this script ($SRC). Run install.sh from the extracted tarball." >&2
        exit 1
    fi
    echo "Checking prerequisites:"
    check_stub
    check_avahi_libs
    check_avahi_daemon
    check_nvidia
    echo
    mkdir -p "$DEST_DIR"
    # Copy beside the target, then swap: a failed copy never leaves a
    # half-installed bundle, and a running Resolve keeps its mapped old copy
    # (new inodes). Leftovers of an interrupted run are cleared first.
    rm -rf "$DEST_DIR"/.NDIOutput.ofx.bundle.new.*
    local tmp="$DEST_DIR/.NDIOutput.ofx.bundle.new.$$"
    cp -R "$SRC" "$tmp"
    chmod -R u=rwX,go=rX "$tmp"
    local f
    for f in "$tmp"/Contents/Linux-x86-64/*.ofx "$tmp"/Contents/Linux-x86-64/*.so*; do
        [ -f "$f" ] && chmod 755 "$f"
    done
    rm -rf "$DEST"
    mv "$tmp" "$DEST"
    echo "Installed VR.NDI Sender to $DEST"
    restart_hints
}

"do_$mode"
