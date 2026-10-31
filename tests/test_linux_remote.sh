#!/usr/bin/env bash
# Tests for scripts/linux_remote.sh. The cache-reset guard: deleting Resolve's
# OFX plugin cache while Resolve runs is undone when it rewrites the cache on
# exit, so the script must refuse. Also box-path normalisation ("~/x" and
# relative paths mean under $HOME, as rsync reads them) and the quoting of
# `logs` arguments for the remote shell. Runs the box-side steps locally
# (NDI_LINUX_HOST=local) against a scratch cache file and a scratch /proc
# (NDI_LINUX_PROC) standing in for a running or stopped Resolve. Resolve is
# found by argv[0], not its name: its main thread renames itself "GUI
# Thread", so `pgrep -x resolve` never matches, and it runs under
# `sg resolve`, which makes /proc/<pid>/exe unreadable (LEARNINGS 2026-10-30).
# Run: bash tests/test_linux_remote.sh  (ctest runs it on Linux)
set -uo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
failures=0

expect() {  # expect <name> <condition...>
    local name="$1"; shift
    if "$@"; then echo "ok   $name"; else echo "FAIL $name" >&2; failures=$((failures + 1)); fi
}

absent() { ! grep -q "$1" "$2"; }  # absent <text> <file>

# A scratch /proc: <pid>/cmdline, NUL-separated argv like the kernel's.
# The box's real neighbours are always there: systemd-resolved, and the
# `sg resolve /opt/resolve/bin/resolve` and `sh -c` wrappers that launch it.
proc="$tmp/proc"
fakeproc() {  # fakeproc <pid> <argv...>
    local pid="$1"
    shift
    mkdir -p "$proc/$pid"
    printf '%s\0' "$@" > "$proc/$pid/cmdline"
}
fakeproc 1100 /usr/lib/systemd/systemd-resolved
fakeproc 11047 sg resolve /opt/resolve/bin/resolve
fakeproc 11048 sh -c /opt/resolve/bin/resolve
mkdir -p "$proc/121567"   # a process that exited mid-scan: no cmdline any more

cache="$tmp/OFXPluginCacheV2.xml"
reset() {  # run cache-reset against the scratch cache and /proc
    NDI_LINUX_HOST=local NDI_LINUX_PROC="$proc" NDI_LINUX_CACHE="$cache" \
        bash "$repo/scripts/linux_remote.sh" cache-reset >"$tmp/out" 2>&1
}

# Resolve running (comm "GUI Thread", argv[0] /opt/resolve/bin/resolve):
# refuse, keep the cache, say why, naming only Resolve itself.
echo '<cache/>' > "$cache"
fakeproc 4242 /opt/resolve/bin/resolve
reset; status=$?
expect "refuses while Resolve runs (exit 3)" [ "$status" -eq 3 ]
expect "cache kept while Resolve runs" [ -f "$cache" ]
expect "names the running pid" grep -q "pid 4242)" "$tmp/out"
expect "a vanished process is skipped silently" absent "No such file" "$tmp/out"

# Resolve closed: delete it.
rm -rf "${proc:?}/4242"
reset; status=$?
expect "succeeds with Resolve closed" [ "$status" -eq 0 ]
expect "cache deleted with Resolve closed" [ ! -e "$cache" ]
expect "says Resolve must be closed before deleting" \
    grep -q "Resolve must be closed; deleting $cache" "$tmp/out"

# Nothing to delete is not an error.
reset; status=$?
expect "no cache is not an error" [ "$status" -eq 0 ]
expect "says there was nothing to reset" grep -q "nothing to reset" "$tmp/out"

# "~/x" reaches the box literally (quoted for ssh) and a relative path is
# relative to $HOME, as rsync reads both: the box side resolves them there.
home="$tmp/home"
mkdir -p "$home/rel"
for spec in "~/tilde.xml" "rel/relative.xml"; do
    target="$home/${spec#"~/"}"
    echo '<cache/>' > "$target"
    HOME="$home" NDI_LINUX_HOST=local NDI_LINUX_PROC="$proc" NDI_LINUX_CACHE="$spec" \
        bash "$repo/scripts/linux_remote.sh" cache-reset >"$tmp/out" 2>&1
    expect "cache path $spec resolves under \$HOME" [ ! -e "$target" ]
done

# logs: ssh joins its command words with spaces and the remote shell
# re-splits them, so each argument must arrive quoted. A fake ssh plays the
# remote shell, with journalctl printing its argv one per line.
bin="$tmp/bin"
mkdir -p "$bin"
cat > "$bin/ssh" <<'FAKE'
#!/usr/bin/env bash
while [ "${1:-}" = -t ]; do shift; done
shift # the host
journalctl() { printf '%s\n' "$@"; }
eval "$*"
FAKE
chmod +x "$bin/ssh"
PATH="$bin:$PATH" NDI_LINUX_HOST=fakebox \
    bash "$repo/scripts/linux_remote.sh" logs --grep 'a b' "x; touch $tmp/pwned" >"$tmp/out" 2>&1
expect "logs keeps a spaced argument whole" grep -qx "a b" "$tmp/out"
expect "logs never runs an argument as a command" [ ! -e "$tmp/pwned" ]
PATH="$bin:$PATH" NDI_LINUX_HOST=fakebox \
    bash "$repo/scripts/linux_remote.sh" logs >"$tmp/out" 2>&1
expect "logs with no extra arguments" [ "$(cat "$tmp/out")" = "$(printf -- '-f\n-t\nNDIOutput')" ]

if [ "$failures" -ne 0 ]; then
    echo "$failures failure(s)" >&2
    exit 1
fi
echo "all linux_remote.sh tests passed"
