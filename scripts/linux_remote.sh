#!/usr/bin/env bash
# Linux dev loop, driven from the Mac over SSH (docs/linux-port-spec.md user
# story 16, ticket #73). The Linux box builds, installs and logs; a human only
# restarts Resolve and presses play there (BUILD.md, Linux section).
#
#   scripts/linux_remote.sh sync            rsync this worktree to the box
#   scripts/linux_remote.sh build [--cuda]  Rocky 8 container build + ctest + stage
#   scripts/linux_remote.sh install         staged bundle -> /usr/OFX/Plugins
#   scripts/linux_remote.sh logs [args]     journalctl -f -t NDIOutput [args]
#   scripts/linux_remote.sh cache           the plugin's entry in Resolve's OFX cache
#   scripts/linux_remote.sh cache-reset     delete the OFX cache (Resolve must be closed)
#
# Environment (defaults in brackets):
#   NDI_LINUX_HOST   SSH target [matt@100.80.25.1]; "local" runs the box-side
#                    steps on this machine (tests/test_linux_remote.sh)
#   NDI_LINUX_DIR    checkout on the box [~/ndioutput-dev]
#   NDI_LINUX_SDK    NDI SDK for Linux on the box [~/sdks/NDI SDK for Linux]
#   NDI_LINUX_CACHE  Resolve's OFX cache [~/.local/share/DaVinciResolve/configs/OFXPluginCacheV2.xml]
#   NDI_LINUX_PROC   /proc to search for a running Resolve [/proc] (local mode; tests)
# The box paths may be absolute, "~/..." or relative; the last two both mean
# under the box's $HOME, for sync (rsync) and the box-side steps alike.
#
# build runs inside the Rocky Linux 8 CUDA image CI uses (spec decision 5),
# never on the Ubuntu host toolchain: a host build pulls GLIBC_2.38 symbols
# and fails test_plugin_elf's 2.28 ceiling. The image with the build deps is
# committed once on the box as ndioutput-build:rocky8. The CPU-only plugin is
# the default until the CUDA path is verified on the box (ticket #74); --cuda
# builds the CUDA module too (and makes the plugin declare CUDA rendering).
#
# /usr/OFX/Plugins is owned by matt on the box, so install needs no sudo.
set -euo pipefail

HOST="${NDI_LINUX_HOST:-matt@100.80.25.1}"
REMOTE_DIR="${NDI_LINUX_DIR:-}"     # empty: the box-side default below
REMOTE_SDK="${NDI_LINUX_SDK:-}"
REMOTE_CACHE="${NDI_LINUX_CACHE:-}"
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BASE_IMAGE="nvidia/cuda:12.9.1-devel-rockylinux8"   # = linux.yml
BUILD_IMAGE="ndioutput-build:rocky8"

usage() {
    sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'
    exit "${1:-2}"
}

# Box-side prelude, expanded there (its $HOME, not the Mac's). Every box
# script gets dir/sdk/cache as $1-$3, then its own arguments.
BOX_PRELUDE='set -euo pipefail
# "~/x" arrives literally (quoted for ssh) and relative "x" is relative to
# $HOME, as rsync reads both; docker -v needs the absolute path.
box_path() {
    case "$1" in
        /*) printf "%s\n" "$1" ;;
        "~") printf "%s\n" "$HOME" ;;
        "~/"*) printf "%s\n" "$HOME/${1#"~/"}" ;;
        *) printf "%s\n" "$HOME/$1" ;;
    esac
}
dir="$(box_path "${1:-$HOME/ndioutput-dev}")"
sdk="$(box_path "${2:-$HOME/sdks/NDI SDK for Linux}")"
cache="$(box_path "${3:-$HOME/.local/share/DaVinciResolve/configs/OFXPluginCacheV2.xml}")"
shift 3
# Resolve by argv[0], not by name or executable: its main thread renames
# itself "GUI Thread" (`pgrep -x resolve` never matches), and it runs under
# `sg resolve`, which makes /proc/<pid>/exe unreadable even to its owner.
resolve_pids() {
    local p argv0
    for p in "${NDI_LINUX_PROC:-/proc}"/[0-9]*; do
        # 2>/dev/null before the <: a process may exit mid-scan.
        argv0="$(tr "\000" "\n" 2>/dev/null < "$p/cmdline" | head -n 1)" || continue
        case "$argv0" in */bin/resolve) echo "${p##*/}" ;; esac
    done
}
'

# Sets QUOTED to the arguments, each quoted for the remote shell: ssh joins
# its command words with spaces and the box's shell re-splits them (the SDK
# path has spaces; a logs filter may too).
quote_for_ssh() {
    QUOTED=()
    local a
    for a in "$@"; do QUOTED+=("$(printf '%q' "$a")"); done
}

# Runs the bash script on stdin on the box, after the prelude.
on_box() {
    local args=("$REMOTE_DIR" "$REMOTE_SDK" "$REMOTE_CACHE" "$@")
    if [ "$HOST" = local ]; then
        { printf '%s\n' "$BOX_PRELUDE"; cat; } | bash -s -- "${args[@]}"
    else
        quote_for_ssh "${args[@]}"
        { printf '%s\n' "$BOX_PRELUDE"; cat; } | ssh "$HOST" bash -s -- "${QUOTED[@]}"
    fi
}

cmd_sync() {
    [ "$HOST" != local ] || { echo "sync: needs a remote NDI_LINUX_HOST" >&2; exit 2; }
    local dir="${REMOTE_DIR:-ndioutput-dev}"   # rsync paths are relative to the box's $HOME
    case "$dir" in               # so "~/x" is "x" (box_path agrees)
        "~") dir=. ;;
        "~/"*) dir="${dir#"~/"}" ;;
    esac
    # build/ and stage/ on the box are excluded, so --delete never touches them.
    rsync -az --delete \
        --exclude '.git/' --exclude '/build/' --exclude '/stage/' --exclude '/dist/' \
        --exclude '/NDIOutput.ofx.bundle/' --exclude '*.o' --exclude 'src/*.d' \
        --exclude '.DS_Store' --exclude '__pycache__/' \
        "$REPO_ROOT/" "$HOST:$dir/"
    echo "synced $REPO_ROOT -> $HOST:$dir"
}

cmd_build() {
    local cuda=OFF
    case "${1:-}" in
        "") ;;
        --cuda) cuda=ON ;;
        *) usage ;;
    esac
    on_box "$cuda" "$BASE_IMAGE" "$BUILD_IMAGE" <<'EOF'
cuda="$1"; base="$2"; img="$3"
cd "$dir"
[ -f "$sdk/include/Processing.NDI.Lib.h" ] || { echo "build: no NDI SDK at $sdk" >&2; exit 1; }
if ! docker image inspect "$img" >/dev/null 2>&1; then
    echo "== one-time: $img = $base + scripts/linux_build_deps.sh"
    docker rm -f ndioutput-build-prep >/dev/null 2>&1 || true
    docker run --name ndioutput-build-prep -v "$dir/scripts:/scripts:ro" "$base" \
        bash /scripts/linux_build_deps.sh
    docker commit ndioutput-build-prep "$img" >/dev/null
    docker rm ndioutput-build-prep >/dev/null
fi
echo "== Rocky 8 container build (CUDA $cuda) in $dir"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -e NDI_CUDA="$cuda" \
    -v "$dir:/src" -v "$sdk:/ndi:ro" -w /src "$img" bash -c '
    set -euo pipefail
    source /opt/rh/gcc-toolset-13/enable
    cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DNDI_SDK_PATH=/ndi -DNDI_ENABLE_CUDA="$NDI_CUDA"
    cmake --build build --parallel "$(nproc)"
    ctest --test-dir build --output-on-failure
    rm -rf stage
    cmake --install build --prefix stage >/dev/null'
echo "== staged:"
find stage -type f | sort
EOF
}

cmd_install() {
    on_box <<'EOF'
src="$dir/stage/NDIOutput.ofx.bundle"
dst=/usr/OFX/Plugins
bin="$src/Contents/Linux-x86-64"
[ -f "$bin/NDIOutput.ofx" ] || { echo "install: no staged bundle at $src (run build first)" >&2; exit 1; }
# A stub libndi loads but never streams (BUILD.md); never install one by accident.
if grep -q ndioutput_stub_libndi "$bin/libndi.so.6" 2>/dev/null; then
    echo "install: $bin/libndi.so.6 is the CI stub, not the NDI runtime; refusing" >&2
    exit 1
fi
# rm, then copy: fresh inodes, so a running Resolve keeps its mapped copy intact.
rm -rf "$dst/NDIOutput.ofx.bundle"
cp -a "$src" "$dst/"
echo "installed $(cat "$dir/VERSION") -> $dst/NDIOutput.ofx.bundle"
ls -l "$dst/NDIOutput.ofx.bundle/Contents/Linux-x86-64"
if [ -n "$(resolve_pids)" ]; then
    echo "NOTE: Resolve is running; it picks up the new binary only after a full restart."
fi
EOF
}

cmd_logs() {
    if [ "$HOST" = local ]; then
        exec journalctl -f -t NDIOutput "$@"
    fi
    quote_for_ssh "$@"
    exec ssh -t "$HOST" journalctl -f -t NDIOutput ${QUOTED[@]+"${QUOTED[@]}"}
}

cmd_cache() {
    on_box <<'EOF'
[ -f "$cache" ] || { echo "no OFX cache at $cache (Resolve rescans everything at next launch)"; exit 0; }
ls -l "$cache"
grep -n -B2 -A2 'LSVR.NDIOutput' "$cache" || echo "LSVR.NDIOutput is not in $cache"
EOF
}

cmd_cache_reset() {
    on_box <<'EOF'
pids="$(resolve_pids)"
if [ -n "$pids" ]; then
    echo "cache-reset: Resolve is running (pid $(echo $pids)). Quit it fully, then retry:" >&2
    echo "  Resolve rewrites the cache on exit, which would undo the reset." >&2
    exit 3
fi
if [ -f "$cache" ]; then
    echo "Resolve must be closed; deleting $cache"
    rm -f "$cache"
    echo "deleted $cache; Resolve rescans every OFX plugin at its next launch"
else
    echo "no OFX cache at $cache; nothing to reset"
fi
EOF
}

case "${1:-}" in
    sync) shift; cmd_sync "$@" ;;
    build) shift; cmd_build "$@" ;;
    install) shift; cmd_install "$@" ;;
    logs) shift; cmd_logs "$@" ;;
    cache) shift; cmd_cache "$@" ;;
    cache-reset) shift; cmd_cache_reset "$@" ;;
    -h|--help|help) usage 0 ;;
    *) usage ;;
esac
