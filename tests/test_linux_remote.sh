#!/usr/bin/env bash
# Tests for scripts/linux_remote.sh's cache-reset guard: deleting Resolve's
# OFX plugin cache while Resolve runs is undone when it rewrites the cache on
# exit, so the script must refuse. Runs the box-side steps locally
# (NDI_LINUX_HOST=local) against a scratch cache file, with a fake pgrep
# standing in for a running or stopped Resolve.
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

# pgrep -x resolve: prints $FAKE_RESOLVE_PID and succeeds when it is set.
mkdir -p "$tmp/bin"
cat > "$tmp/bin/pgrep" <<'EOF'
#!/usr/bin/env bash
[ "$*" = "-x resolve" ] || { echo "fake pgrep: unexpected args: $*" >&2; exit 2; }
[ -n "${FAKE_RESOLVE_PID:-}" ] || exit 1
echo "$FAKE_RESOLVE_PID"
EOF
chmod +x "$tmp/bin/pgrep"

cache="$tmp/OFXPluginCacheV2.xml"
reset() {  # reset [pid]: run cache-reset with Resolve "running" as pid, if given
    FAKE_RESOLVE_PID="${1:-}" PATH="$tmp/bin:$PATH" NDI_LINUX_HOST=local \
        NDI_LINUX_CACHE="$cache" bash "$repo/scripts/linux_remote.sh" cache-reset \
        >"$tmp/out" 2>&1
}

# Resolve running: refuse, keep the cache, say why.
echo '<cache/>' > "$cache"
reset 4242; status=$?
expect "refuses while Resolve runs (exit 3)" [ "$status" -eq 3 ]
expect "cache kept while Resolve runs" [ -f "$cache" ]
expect "names the running pid" grep -q "pid 4242" "$tmp/out"

# Resolve closed: delete it.
reset; status=$?
expect "succeeds with Resolve closed" [ "$status" -eq 0 ]
expect "cache deleted with Resolve closed" [ ! -e "$cache" ]

# Nothing to delete is not an error.
reset; status=$?
expect "no cache is not an error" [ "$status" -eq 0 ]
expect "says there was nothing to reset" grep -q "nothing to reset" "$tmp/out"

if [ "$failures" -ne 0 ]; then
    echo "$failures failure(s)" >&2
    exit 1
fi
echo "all linux_remote.sh tests passed"
