#!/usr/bin/env bash
# Tests for scripts/package_linux_release.sh's packaging guards (ticket #78),
# in --stage mode (package an already-staged tree, no container build): it
# refuses a stub libndi.so.6 unless --allow-stub (and then names the tarball
# -STUB), refuses an NDI runtime more than 30 days old unless
# --allow-old-sdk, refuses a tree the ELF checker rejects, and emits
# NDIOutput-<V>-linux-x86_64.tar.gz + SHA256SUMS-Linux.txt. The ELF checker
# is faked here; CI and the box run the real test_plugin_elf.
# NDIOUTPUT_TODAY (test-only) pins "today" for the age guard. The last
# section runs release mode in throwaway git repos to check the
# uncommitted-changes guard, which fires before any build.
# Run: bash tests/test_package_linux_release.sh  (ctest runs it on Linux)
set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
failures=0
V="$(tr -d '[:space:]' < "$repo/VERSION")"

expect() {  # expect <name> <condition...>
    local name="$1"; shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name" >&2; failures=$((failures + 1)); fi
}
has() { grep -q -- "$1" "$2"; }
absent() { ! grep -q -- "$1" "$2"; }

# A staged tree as `cmake --install build --prefix stage` lays it out with
# the real SDK: libndi.so.6 carries the SDK's build stamp.
stage="$tmp/stage"
bin="$stage/NDIOutput.ofx.bundle/Contents/Linux-x86-64"
mkdir -p "$bin" "$stage/NDIOutput.ofx.bundle/Contents/Resources"
echo "plugin" > "$bin/NDIOutput.ofx"
printf 'ELF\0junk\0NDI SDK LINUX 12:51:52 Apr 13 2026 6.3.2.0\0more\0' > "$bin/libndi.so.6"
echo "licenses" > "$bin/libndi_licenses.txt"
echo "icon" > "$stage/NDIOutput.ofx.bundle/Contents/Resources/LSVR.NDIOutput.png"
cp "$bin/libndi.so.6" "$tmp/libndi.real"

printf '#!/bin/sh\necho "checked $1" > "%s/checked"\nexit 0\n' "$tmp" > "$tmp/elf_ok"
printf '#!/bin/sh\necho "FAIL NEEDED libstdc++.so.6"\nexit 1\n' > "$tmp/elf_bad"
chmod +x "$tmp/elf_ok" "$tmp/elf_bad"

out="$tmp/dist"
pack() {  # pack [extra args]: package $stage into $out; output in $tmp/log
    rm -rf "$out"
    NDIOUTPUT_TODAY="${TODAY:-2026-04-20}" bash "$repo/scripts/package_linux_release.sh" \
        --stage "$stage" --out "$out" --elf-checker "${CHECKER:-$tmp/elf_ok}" "$@" >"$tmp/log" 2>&1
}
tarball="NDIOutput-$V-linux-x86_64.tar.gz"
stubball="NDIOutput-$V-linux-x86_64-STUB.tar.gz"

# --- a fresh real-SDK tree packages ---------------------------------------------
pack; status=$?
expect "real tree, fresh SDK: packages (exit 0)" [ "$status" -eq 0 ]
expect "emits the tarball" [ -f "$out/$tarball" ]
expect "emits SHA256SUMS-Linux.txt" [ -f "$out/SHA256SUMS-Linux.txt" ]
expect "the checksum file verifies the tarball by bare name" \
    bash -c "cd '$out' && { sha256sum -c SHA256SUMS-Linux.txt || shasum -a 256 -c SHA256SUMS-Linux.txt; } >/dev/null 2>&1"
expect "ran the ELF checker on the staged .ofx" has "Linux-x86-64/NDIOutput.ofx" "$tmp/checked"
tar -tzf "$out/$tarball" > "$tmp/list" 2>/dev/null
top="NDIOutput-$V-linux-x86_64"
for f in install.sh uninstall.sh README.txt LICENSE.txt libndi_licenses.txt \
         NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx \
         NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi.so.6 \
         NDIOutput.ofx.bundle/Contents/Linux-x86-64/libndi_licenses.txt; do
    expect "tarball holds $top/$f" grep -qx "$top/$f" "$tmp/list"
done
mkdir -p "$tmp/x" && tar -xzf "$out/$tarball" -C "$tmp/x"
expect "install.sh is executable in the tarball" [ -x "$tmp/x/$top/install.sh" ]
expect "uninstall.sh is executable in the tarball" [ -x "$tmp/x/$top/uninstall.sh" ]

# --- CPU-only packages say so in their name ------------------------------------------
cpuball="NDIOutput-$V-linux-x86_64-cpu-only.tar.gz"
pack --cpu-only; status=$?
expect "--cpu-only: packages (exit 0)" [ "$status" -eq 0 ]
expect "--cpu-only: the tarball is named -cpu-only" [ -f "$out/$cpuball" ]
expect "--cpu-only: no unsuffixed tarball" [ ! -e "$out/$tarball" ]
expect "--cpu-only: checksums name the -cpu-only tarball" has "$cpuball" "$out/SHA256SUMS-Linux.txt"
pack --cuda; status=$?
expect "the old --cuda flag is rejected (CUDA is the release default)" [ "$status" -eq 2 ]
pack --no-cuda; status=$?
expect "--no-cuda is rejected (--cpu-only is the one override)" [ "$status" -eq 2 ]

# --- stub guard -------------------------------------------------------------------
printf 'ELF\0ndioutput_stub_libndi\0' > "$bin/libndi.so.6"
rm "$bin/libndi_licenses.txt"
pack; status=$?
expect "stub tree: refused (exit 1)" [ "$status" -eq 1 ]
expect "stub tree: says it is the CI stub" has "stub" "$tmp/log"
expect "stub tree: no tarball" [ ! -e "$out/$tarball" ]
expect "stub tree: no -STUB tarball without --allow-stub" [ ! -e "$out/$stubball" ]
pack --allow-stub; status=$?
expect "stub tree + --allow-stub: packages (exit 0)" [ "$status" -eq 0 ]
expect "stub tree + --allow-stub: the tarball is named -STUB" [ -f "$out/$stubball" ]
expect "stub tree + --allow-stub: no unsuffixed tarball" [ ! -e "$out/$tarball" ]
expect "stub tree + --allow-stub: checksums name the -STUB tarball" has "$stubball" "$out/SHA256SUMS-Linux.txt"
cp "$tmp/libndi.real" "$bin/libndi.so.6"
echo "licenses" > "$bin/libndi_licenses.txt"

# A real runtime without its licenses file can't ship (spec decision 11).
mv "$bin/libndi_licenses.txt" "$tmp/lic.away"
pack; status=$?
mv "$tmp/lic.away" "$bin/libndi_licenses.txt"
expect "real tree without the NDI licenses file: refused (exit 1)" [ "$status" -eq 1 ]

# --- SDK-age guard (spec decision 11: no more than 30 days old at release) --------
TODAY=2026-05-13 pack; status=$?
expect "SDK exactly 30 days old: packages (exit 0)" [ "$status" -eq 0 ]
TODAY=2026-11-01 pack; status=$?
expect "SDK 202 days old: refused (exit 1)" [ "$status" -eq 1 ]
expect "SDK too old: names the SDK date" has "2026-04-13" "$tmp/log"
expect "SDK too old: names its age" has "202 days" "$tmp/log"
expect "SDK too old: no tarball" [ ! -e "$out/$tarball" ]
TODAY=2026-11-01 pack --allow-old-sdk; status=$?
expect "SDK too old + --allow-old-sdk: packages (exit 0)" [ "$status" -eq 0 ]
expect "SDK too old + --allow-old-sdk: still warns" has "WARNING" "$tmp/log"
printf 'ELF\0no stamp here\0' > "$bin/libndi.so.6"
pack; status=$?
cp "$tmp/libndi.real" "$bin/libndi.so.6"
expect "SDK date unreadable: refused (exit 1)" [ "$status" -eq 1 ]

# Fail closed: a date that can't be parsed, or an SDK from the future, never
# packages, not even with --allow-old-sdk.
TODAY=2026-04-01 pack; status=$?
expect "SDK dated after today: refused (exit 1)" [ "$status" -eq 1 ]
expect "SDK dated after today: says so" has "after today" "$tmp/log"
expect "SDK dated after today: no tarball" [ ! -e "$out/$tarball" ]
TODAY=2026-04-01 pack --allow-old-sdk; status=$?
expect "SDK dated after today + --allow-old-sdk: still refused (exit 1)" [ "$status" -eq 1 ]
TODAY=not-a-date pack; status=$?
expect "today unparseable: refused (exit 1)" [ "$status" -eq 1 ]
expect "today unparseable: says so" has "can't parse today" "$tmp/log"
TODAY=not-a-date pack --allow-old-sdk; status=$?
expect "today unparseable + --allow-old-sdk: still refused (exit 1)" [ "$status" -eq 1 ]
printf 'ELF\0NDI SDK LINUX 12:51:52 Xyz 13 2026 6.3.2.0\0' > "$bin/libndi.so.6"
pack --allow-old-sdk; status=$?
cp "$tmp/libndi.real" "$bin/libndi.so.6"
expect "SDK stamp unparseable + --allow-old-sdk: still refused (exit 1)" [ "$status" -eq 1 ]
# Two stamps in one binary "line": the first one counts.
printf 'ELF\0NDI SDK LINUX 12:51:52 Apr 13 2026 6.3.2.0\0NDI SDK LINUX 01:00:00 Jan  1 2020 5.0\0' > "$bin/libndi.so.6"
pack; status=$?
cp "$tmp/libndi.real" "$bin/libndi.so.6"
expect "two stamps: the first (fresh) one counts (exit 0)" [ "$status" -eq 0 ]

# --- ELF checker ------------------------------------------------------------------
CHECKER="$tmp/elf_bad" pack; status=$?
expect "ELF checker fails: refused (exit 1)" [ "$status" -eq 1 ]
expect "ELF checker fails: its output is shown" has "libstdc++" "$tmp/log"
expect "ELF checker fails: no tarball" [ ! -e "$out/$tarball" ]
CHECKER="$tmp/missing" pack; status=$?
expect "ELF checker missing: refused (exit 1)" [ "$status" -eq 1 ]

# --- release mode refuses uncommitted changes (it deletes build/ and stage/) ------
# Each run stops before any build: no NDI SDK at --sdk (or no docker, in CI).
mkrepo() {  # mkrepo <dir>: the packager in a throwaway checkout layout
    rm -rf "$1"
    mkdir -p "$1/scripts" "$1/build"
    cp "$repo/scripts/package_linux_release.sh" "$1/scripts/"
    cp "$repo/VERSION" "$repo/.gitignore" "$1/"
    echo "dev build" > "$1/build/keep"
}
stopped_before_build() {  # the run stopped at the docker or SDK check, as expected here
    grep -q -e "no NDI SDK" -e "builds in Docker" "$tmp/log"
}
release() {  # release <repo dir> [args]: release mode; output in $tmp/log
    bash "$1/scripts/package_linux_release.sh" --sdk "$tmp/no-sdk" "${@:2}" >"$tmp/log" 2>&1
}
if command -v git >/dev/null 2>&1; then
    r="$tmp/gitrepo"
    mkrepo "$r"
    git -C "$r" init -q
    git -C "$r" add -A
    git -C "$r" -c user.name=t -c user.email=t@example.com commit -q -m init
    release "$r"; status=$?
    expect "clean checkout: passes the guard (then stops: no SDK)" absent "uncommitted" "$tmp/log"
    expect "clean checkout: then stops at the docker/SDK check" stopped_before_build
    echo "local edit" >> "$r/VERSION.notes"
    release "$r"; status=$?
    expect "uncommitted changes: refused (exit 1)" [ "$status" -eq 1 ]
    expect "uncommitted changes: says so" has "uncommitted changes" "$tmp/log"
    expect "uncommitted changes: names the file" has "VERSION.notes" "$tmp/log"
    expect "uncommitted changes: build/ is untouched" [ -f "$r/build/keep" ]
    release "$r" --force; status=$?
    expect "uncommitted changes + --force: passes the guard, warning" has "WARNING: --force" "$tmp/log"
    expect "uncommitted changes + --force: then stops at the docker/SDK check" stopped_before_build
    expect "uncommitted changes + --force: build/ is untouched by the stop" [ -f "$r/build/keep" ]
else
    echo "skip uncommitted-changes guard tests: no git"
fi
r="$tmp/rsynced"
mkrepo "$r"
release "$r"; status=$?
expect "not a git checkout (rsync'd): passes the guard with a note" has "not a git checkout" "$tmp/log"
expect "not a git checkout: then stops at the docker/SDK check" stopped_before_build

echo
if [ "$failures" -ne 0 ]; then echo "$failures test(s) failed" >&2; exit 1; fi
echo "all package_linux_release tests passed"
