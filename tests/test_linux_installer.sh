#!/usr/bin/env bash
# Tests for installer/linux/install.sh and uninstall.sh (ticket #78), run
# against fake roots: NDIOUTPUT_ROOT stands in for "/" (the plugin directory,
# library directories, /proc and the avahi-daemon binary all live under it),
# and NDIOUTPUT_TEST_EUID stands in for the caller's uid. The container test
# (tests/linux_installer_container_test.sh, linux.yml) runs the same scripts
# for real, as root, on Ubuntu 24.04 and Rocky 8.
# Run: bash tests/test_linux_installer.sh  (ctest runs it on Linux)
set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
failures=0

expect() {  # expect <name> <condition...>
    local name="$1"; shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name" >&2; failures=$((failures + 1)); fi
}
absent() { ! grep -q -- "$1" "$2"; }  # absent <text> <file>
has() { grep -q -- "$1" "$2"; }       # has <text> <file>

# An extracted tarball: the scripts beside a bundle.
pkg="$tmp/pkg"
mkdir -p "$pkg/NDIOutput.ofx.bundle/Contents/Linux-x86-64" "$pkg/NDIOutput.ofx.bundle/Contents/Resources"
cp "$repo/installer/linux/install.sh" "$repo/installer/linux/uninstall.sh" "$pkg/"
echo "plugin v1" > "$pkg/NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx"
echo "real runtime" > "$pkg/NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi.so.6"
echo "icon" > "$pkg/NDIOutput.ofx.bundle/Contents/Resources/LSVR.NDIOutput.png"

newroot() {  # newroot <name>: an empty fake root, path in $root
    root="$tmp/$1"
    rm -rf "$root"
    mkdir -p "$root/proc"
}
fakeproc() {  # fakeproc <pid> <comm> <argv...>
    local pid="$1" comm="$2"
    shift 2
    mkdir -p "$root/proc/$pid"
    echo "$comm" > "$root/proc/$pid/comm"
    printf '%s\0' "$@" > "$root/proc/$pid/cmdline"
}
avahi_libs() {  # avahi_libs <libdir>: both client libraries, as Ubuntu or Rocky lays them out
    mkdir -p "$root/$1"
    touch "$root/$1/libavahi-client.so.3" "$root/$1/libavahi-common.so.3"
}
run() {  # run <script> [args]: as root (uid 0) unless EUID_AS is set; output in $tmp/out
    local script="$1"; shift
    NDIOUTPUT_ROOT="$root" NDIOUTPUT_TEST_EUID="${EUID_AS:-0}" \
        bash "$pkg/$script" "$@" >"$tmp/out" 2>&1
}
plugins() { echo "$root/usr/OFX/Plugins"; }

# --- root check ---------------------------------------------------------------
newroot nonroot
EUID_AS=1000 run install.sh; status=$?
expect "install refuses a non-root caller (exit 1)" [ "$status" -eq 1 ]
expect "install says to use sudo" has "sudo" "$tmp/out"
expect "nothing installed for a non-root caller" [ ! -e "$(plugins)/NDIOutput.ofx.bundle" ]
EUID_AS=1000 run uninstall.sh; status=$?
expect "uninstall refuses a non-root caller (exit 1)" [ "$status" -eq 1 ]

# --- install, replace, idempotent, uninstall -----------------------------------
newroot fresh
run install.sh; status=$?
expect "install succeeds as root on a bare system (exit 0)" [ "$status" -eq 0 ]
expect "creates /usr/OFX/Plugins and installs the bundle" \
    [ -f "$(plugins)/NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx" ]
expect "installs the bundled NDI runtime" \
    [ -f "$(plugins)/NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi.so.6" ]
expect "the plugin binary is world-readable and executable" \
    [ -x "$(plugins)/NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx" ]
expect "the NDI runtime is executable" \
    [ -x "$(plugins)/NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi.so.6" ]
expect "other files are not made executable" \
    [ ! -x "$(plugins)/NDIOutput.ofx.bundle/Contents/Resources/LSVR.NDIOutput.png" ]
expect "tells the user to restart Resolve" has "restart DaVinci Resolve" "$tmp/out"
expect "gives the cache-reset hint" has "OFXPluginCacheV2.xml" "$tmp/out"

# An older install with a file the new build no longer ships, plus a
# half-copied temp tree from an interrupted run.
echo "stale" > "$(plugins)/NDIOutput.ofx.bundle/Contents/Resources/old_helper.py"
mkdir -p "$(plugins)/.NDIOutput.ofx.bundle.new.123"
echo "plugin v2" > "$pkg/NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx"
run install.sh; status=$?
expect "re-install succeeds (exit 0)" [ "$status" -eq 0 ]
expect "re-install replaces the binary" \
    grep -q "plugin v2" "$(plugins)/NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx"
expect "re-install replaces the whole bundle (no stale files)" \
    [ ! -e "$(plugins)/NDIOutput.ofx.bundle/Contents/Resources/old_helper.py" ]
expect "re-install leaves no temp trees behind" \
    [ -z "$(find "$(plugins)" -maxdepth 1 -name '.NDIOutput*')" ]
expect "only the bundle is in the plugin directory" \
    [ "$(ls -A "$(plugins)")" = "NDIOutput.ofx.bundle" ]

# Another vendor's plugin beside ours is never touched.
mkdir -p "$(plugins)/Other.ofx.bundle"
run uninstall.sh; status=$?
expect "uninstall succeeds (exit 0)" [ "$status" -eq 0 ]
expect "uninstall removes the bundle" [ ! -e "$(plugins)/NDIOutput.ofx.bundle" ]
expect "uninstall leaves other plugins alone" [ -d "$(plugins)/Other.ofx.bundle" ]
run uninstall.sh; status=$?
expect "uninstall again is a no-op (exit 0)" [ "$status" -eq 0 ]
expect "uninstall again says nothing was installed" has "not installed" "$tmp/out"

# A missing bundle beside the script is an error, not an empty install.
newroot nobundle
mv "$pkg/NDIOutput.ofx.bundle" "$tmp/bundle.away"
run install.sh; status=$?
mv "$tmp/bundle.away" "$pkg/NDIOutput.ofx.bundle"
expect "install without a bundle beside it fails (exit 1)" [ "$status" -eq 1 ]
expect "and installs nothing" [ ! -e "$(plugins)/NDIOutput.ofx.bundle" ]

# --- prerequisite checks (warnings: they never fail the install) ---------------
# Bare system: no Avahi libraries, no daemon, no NVIDIA driver.
newroot bare
run install.sh; status=$?
expect "bare system: install still succeeds (exit 0)" [ "$status" -eq 0 ]
expect "bare system: warns the Avahi client libraries are missing" has "WARNING: the Avahi client libraries" "$tmp/out"
expect "bare system: names the Ubuntu package" has "libavahi-client3" "$tmp/out"
expect "bare system: names the Rocky package" has "avahi-libs" "$tmp/out"
expect "bare system: warns avahi-daemon is not installed" has "avahi-daemon is not installed" "$tmp/out"
expect "bare system: NVIDIA info line says none found" has "NVIDIA driver: none" "$tmp/out"
expect "bare system: no Resolve-running line" absent "Resolve is running" "$tmp/out"

# Ubuntu layout, daemon installed and running, NVIDIA driver loaded.
newroot ubuntu
avahi_libs usr/lib/x86_64-linux-gnu
mkdir -p "$root/usr/sbin" && touch "$root/usr/sbin/avahi-daemon"
fakeproc 812 avahi-daemon "avahi-daemon: running [COMFYBOX.local]"
mkdir -p "$root/proc/driver/nvidia"
echo "NVRM version: NVIDIA UNIX x86_64 Kernel Module  610.43.02  Release Build  (dvs-builder@U22-I3-AF03-09-1)  Thu Oct  2 2026" \
    > "$root/proc/driver/nvidia/version"
run install.sh; status=$?
expect "ubuntu: install succeeds (exit 0)" [ "$status" -eq 0 ]
expect "ubuntu: no Avahi library warning" absent "WARNING: the Avahi client libraries" "$tmp/out"
expect "ubuntu: no avahi-daemon warning" absent "WARNING: avahi-daemon" "$tmp/out"
expect "ubuntu: reports the NVIDIA driver version" has "NVIDIA driver: 610.43.02" "$tmp/out"

# Rocky layout (lib64); the daemon is installed but stopped; Resolve is running.
newroot rocky
avahi_libs usr/lib64
mkdir -p "$root/usr/sbin" && touch "$root/usr/sbin/avahi-daemon"
fakeproc 11047 sg sg resolve /opt/resolve/bin/resolve
fakeproc 11049 "GUI Thread" /opt/resolve/bin/resolve
run install.sh; status=$?
expect "rocky: install succeeds (exit 0)" [ "$status" -eq 0 ]
expect "rocky: no Avahi library warning (lib64)" absent "WARNING: the Avahi client libraries" "$tmp/out"
expect "rocky: warns avahi-daemon is not running" has "WARNING: avahi-daemon is not running" "$tmp/out"
expect "rocky: says how to start it" has "systemctl enable --now avahi-daemon" "$tmp/out"
expect "rocky: never suggests stopping or restarting avahi under Resolve" absent "restart avahi" "$tmp/out"
expect "rocky: says Resolve is running and needs a full restart" has "Resolve is running" "$tmp/out"

# Only one of the two client libraries is there: still a warning.
newroot halfavahi
mkdir -p "$root/usr/lib/x86_64-linux-gnu"
touch "$root/usr/lib/x86_64-linux-gnu/libavahi-common.so.3"
run install.sh
expect "one Avahi library missing: warns, naming it" has "libavahi-client.so.3" "$tmp/out"

# A CI stub bundle installs (linux.yml proves the pipeline with one) but says
# loudly that it never streams.
newroot stub
cp "$pkg/NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi.so.6" "$tmp/libndi.real"
printf 'ELF...ndioutput_stub_libndi...' > "$pkg/NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi.so.6"
run install.sh; status=$?
cp "$tmp/libndi.real" "$pkg/NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi.so.6"
expect "stub bundle: install succeeds (exit 0)" [ "$status" -eq 0 ]
expect "stub bundle: warns it never streams" has "STUB" "$tmp/out"
run install.sh
expect "real bundle: no stub warning" absent "STUB" "$tmp/out"

echo
if [ "$failures" -ne 0 ]; then echo "$failures test(s) failed" >&2; exit 1; fi
echo "all installer tests passed"
