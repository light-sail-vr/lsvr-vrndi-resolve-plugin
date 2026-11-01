#!/usr/bin/env bash
# The release tarball's install/uninstall cycle for real, as root, inside a
# throwaway container (ticket #78; linux.yml's installer job runs it on
# Ubuntu 24.04 and Rocky 8, each with and without the Avahi client
# libraries). The fake-root unit test is tests/test_linux_installer.sh.
#
#   tests/linux_installer_container_test.sh <tarball> --avahi-libs yes|no [--elf-checker PATH]
#
# --avahi-libs says what the container has, and so whether install.sh must
# warn about them. Containers never run avahi-daemon, so that warning is
# always expected. --elf-checker runs test_plugin_elf on the installed .ofx.
# Never run it on a real workstation: it installs into /usr/OFX/Plugins.
set -uo pipefail

tarball="${1:?usage: $0 <tarball> --avahi-libs yes|no [--elf-checker PATH]}"
shift
avahi=""
checker=""
while [ $# -gt 0 ]; do
    case "$1" in
        --avahi-libs) avahi="$2"; shift ;;
        --elf-checker) checker="$2"; shift ;;
        *) echo "unknown option $1" >&2; exit 2 ;;
    esac
    shift
done
case "$avahi" in yes|no) ;; *) echo "--avahi-libs yes|no is required" >&2; exit 2 ;; esac
[ "$(id -u)" -eq 0 ] || { echo "run as root, in a container" >&2; exit 2; }
[ -f /.dockerenv ] || [ -n "${CI:-}" ] || [ -n "${container:-}" ] ||
    { echo "refusing to run outside a container (it installs into /usr/OFX/Plugins)" >&2; exit 2; }

failures=0
expect() {  # expect <name> <condition...>
    local name="$1"; shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name" >&2; failures=$((failures + 1)); fi
}
has() { grep -q -- "$1" "$2"; }
absent() { ! grep -q -- "$1" "$2"; }

# Rocky 8's base image has no find or cmp (findutils, diffutils): install.sh
# doesn't need them, so neither may this test.
shopt -s globstar dotglob nullglob
tree() { local f; for f in "$1"/**; do echo "${f#"$1"}"; done; }   # tree <dir>: every path under it
all_files() {  # all_files <dir> <test...>: every path under dir passes test
    local dir="$1" f; shift
    for f in "$dir" "$dir"/**; do "$@" "$f" || { echo "  fails: $f" >&2; return 1; }; done
}
owned_by_root() { [ "$(stat -c %u "$1")" = 0 ]; }
world_readable() { [ "$(( 0$(stat -c %a "$1") & 04 ))" -ne 0 ]; }

dest=/usr/OFX/Plugins/NDIOutput.ofx.bundle
work="$(mktemp -d)"
chmod 755 "$work"
tar -xzf "$tarball" -C "$work"
pkgs=("$work"/*/)
pkg="${pkgs[0]%/}"
echo "== $(. /etc/os-release && echo "$PRETTY_NAME"), Avahi client libraries: $avahi, package $(basename "$pkg")"

# The container must match what we were told, or the warning check means nothing.
if ldconfig -p | grep -q 'libavahi-client\.so\.3 '; then have=yes; else have=no; fi
expect "container precondition: Avahi client libraries $avahi" [ "$have" = "$avahi" ]

# Non-root: refused, nothing written.
nonroot() {
    if command -v setpriv >/dev/null 2>&1; then
        setpriv --reuid=65534 --regid=65534 --clear-groups "$@"
    else
        su -s /bin/bash nobody -c "$(printf '%q ' "$@")"
    fi
}
nonroot "$pkg/install.sh" > "$work/out" 2>&1; status=$?
expect "non-root install refused (exit 1)" [ "$status" -eq 1 ]
expect "non-root install wrote nothing" [ ! -e "$dest" ]

"$pkg/install.sh" > "$work/out" 2>&1; status=$?
cat "$work/out"
expect "install as root (exit 0)" [ "$status" -eq 0 ]
expect "bundle installed" [ -f "$dest/Contents/Linux-x86-64/NDIOutput.ofx" ]
expect "NDI runtime installed" [ -f "$dest/Contents/Linux-x86-64/libndi.so.6" ]
expect "installed files owned by root" all_files "$dest" owned_by_root
expect "installed tree world-readable" all_files "$dest" world_readable
expect "plugin binary executable" [ -x "$dest/Contents/Linux-x86-64/NDIOutput.ofx" ]
if [ "$avahi" = no ]; then
    expect "warns about the missing Avahi client libraries" has "WARNING: the Avahi client libraries" "$work/out"
else
    expect "no Avahi library warning" absent "WARNING: the Avahi client libraries" "$work/out"
fi
expect "warns that avahi-daemon is not running/installed" has "WARNING: avahi-daemon is not" "$work/out"
expect "NVIDIA info line" has "NVIDIA driver:" "$work/out"
expect "restart hint" has "restart DaVinci Resolve" "$work/out"
expect "cache-reset hint" has "OFXPluginCacheV2.xml" "$work/out"
tree "$dest" > "$work/first"

"$pkg/install.sh" > "$work/out" 2>&1; status=$?
expect "second install (exit 0)" [ "$status" -eq 0 ]
tree "$dest" > "$work/second"
expect "second install leaves the same tree (idempotent)" [ "$(cat "$work/first")" = "$(cat "$work/second")" ]
expect "the tree is not empty" [ "$(wc -l < "$work/first")" -gt 5 ]
expect "no temp trees left in /usr/OFX/Plugins" [ "$(ls -A /usr/OFX/Plugins)" = "NDIOutput.ofx.bundle" ]

if [ -n "$checker" ]; then
    "$checker" "$dest/Contents/Linux-x86-64/NDIOutput.ofx" > "$work/elf" 2>&1; status=$?
    tail -n 3 "$work/elf"
    expect "test_plugin_elf passes on the installed .ofx" [ "$status" -eq 0 ]
fi

"$pkg/uninstall.sh" > "$work/out" 2>&1; status=$?
cat "$work/out"
expect "uninstall (exit 0)" [ "$status" -eq 0 ]
expect "bundle removed" [ ! -e "$dest" ]
"$pkg/uninstall.sh" > "$work/out" 2>&1; status=$?
expect "second uninstall is a no-op (exit 0)" [ "$status" -eq 0 ]
expect "second uninstall says not installed" has "not installed" "$work/out"

rm -rf "$work"
echo
if [ "$failures" -ne 0 ]; then echo "$failures check(s) failed" >&2; exit 1; fi
echo "installer container test passed"
