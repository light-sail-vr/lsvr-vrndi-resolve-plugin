# CLAUDE.md

OpenFX plugin for DaVinci Resolve that streams the rendered frame to NDI (Standard NDI SDK, Metal-accelerated on macOS; Windows/CUDA port in the same tree since 2026-09-01, CMake + VS2022 + CUDA 12.9, first released in v1.14.1 — see below).

## Workflow rules

- **Never commit to `master`.** Day-to-day base branch is `dev`.
- **Branching:** every new feature gets its own `feature/<name>` branch off `dev` and merges into `dev` by PR once the testing loop passes (Matt, 2026-08-30).
- **Releases and merges into `master`:** follow [docs/RELEASING.md](docs/RELEASING.md) — whenever Matt asks to strike a release or anything merges into `master`. It covers the Mac/Windows hand-off prompts and the human gates; a release starts only on Matt's explicit instruction.
- **Testing loop:** LEARNINGS.md §2. Tier 0 (`make dev`) on every change; Tiers 1–2 (install → restart Resolve → verify stream in NDI Video Monitor) before calling anything working. Playback must be started by a human — the Resolve API has no transport control.
- **After fixing any bug or discovering a workflow gotcha, append an entry to LEARNINGS.md** (template at the bottom of that file). This is a standing instruction from Matt.
- **Build/install questions:** BUILD.md is the single source of truth; update it in the same PR as any build change.
- **Version bumps:** only via `scripts/increment_version.sh` / `scripts/set_version.sh` (they sync `VERSION` + the `#define`s in `src/NDIOutputPlugin.cpp`). Never `make bump-*`.

## Windows port

The Windows port lives in this tree — the long-lived `windows-port` integration branch was merged into `dev` and retired on 2026-09-01 (PR #37). Spec: [docs/windows-port-spec.md](docs/windows-port-spec.md) (decision 2 amended there); research: [docs/2026-08-30-windows-port-feasibility.md](docs/2026-08-30-windows-port-feasibility.md); tickets: GitHub issues labeled `windows`.

- **Branching:** Windows work goes on `feature/win-<name>` branches off `dev`, merged into `dev` by PR once the Windows testing loop passes — same as any feature. The spec's release bar (fresh-machine installer, GPU-native CUDA at 8K stereo, deferred-features list in the notes) gates the unified `dev` → `master` release, not integration into `dev`; a release may ship with one platform lagging, stated in its notes. `master` and release rules are unchanged. No long-lived per-platform branches again — they produced duplicate version bumps and duplicate files against macOS features.
- **Platform isolation:** platform code lives in platform-prefixed files (`src/Win*`, `src/Mac*`, `*.mm`, `*.cu`) behind shared seam headers with host-independent unit tests; gate plugin call sites on capability macros (`NDI_TIMELINE_WATCH`-style) rather than raw `__APPLE__`/`_WIN32` checks. A macOS change must keep the Windows CI compile green and vice versa.
- **One version for both platforms:** `VERSION`/`CHANGELOG.md` stay unified — Windows has its own build pipeline (CMake + VS2022 + CUDA 12.9, compile-only CI on `windows-2022`, Inno Setup installer) but never its own version numbers.
- **Windows testing loop** mirrors LEARNINGS.md §2: Tier 0 = CMake build + unit tests under MSVC (CI runs these on every push); Tiers 1–2 = install the bundle to `C:\Program Files\Common Files\OFX\Plugins\`, fully restart Resolve, verify the stream in NDI Tools' **Studio Monitor** (ideally from a second machine — firewall behavior is part of the test). Playback must still be started by a human.
- **Windows plugin cache:** `%APPDATA%\Blackmagic Design\DaVinci Resolve\Support\OFXPluginCacheV2.xml` (note the extra `Support\` level vs macOS); same delete-it-with-Resolve-closed rescan procedure.
- **GPU rules on Windows:** enqueue kernels on the host-provided CUDA stream; never `cudaDeviceSynchronize`; never `cudaDeviceReset` inside Resolve's process.
- **Live logs on Windows:** `NDI_LOG` routes to `OutputDebugStringA` (+ optional file sink) — watch with DebugView/WinDbg, not `monitor_ndi_logs.sh`.
- The LEARNINGS.md standing rule (append every fixed bug / workflow gotcha) applies to Windows work unchanged, and BUILD.md stays the single source of truth for build/install on both platforms.

## Linux port

The Linux port lives in this tree too, on the same `CMakeLists.txt` as Windows (Rocky Linux 8 container, CMake + gcc-toolset-13 + CUDA 12.9; the Makefile stays macOS-only). Spec: [docs/linux-port-spec.md](docs/linux-port-spec.md); tickets: GitHub issues labelled `linux` (#70 is the spec issue). The test box is COMFYBOX (`ssh matt@100.80.25.1`, Ubuntu 24.04, RTX 3090 Ti, Resolve Studio 21.1); its paths are in LEARNINGS.md → "Linux environment map".

- **Branching:** `feature/linux-<name>` branches off `dev`, merged into `dev` by PR once the Linux testing loop passes. No long-lived `linux-port` branch. The spec's release bar (decision 3) gates the first `dev` → `master` release with Linux artifacts, not integration into `dev`.
- **Platform isolation:** **never modify macOS code** (Matt's rule, 2026-10-30): no edits to `*.mm`, `src/Mac*`, `src/TimelineClipWatcher.cpp`, the `__APPLE__` branches or the Makefile's behaviour, and Windows behaviour stays identical. Linux code goes in `src/Linux*` files behind seam headers with host-independent unit tests; call sites are gated on capability macros (`NDI_RUNTIME_DLOPEN`, `NDI_TIMELINE_WATCH`, …), never raw `__linux__`. A Linux change must keep `make dev`/`make test` on the Mac and both CI workflows green.
- **NDI runtime:** the plugin never links `libndi`. It `dlopen`s `Contents/Linux-x86-64/libndi.so.6` from its own bundle (`src/LinuxNDIRuntime.h`) and calls NDI through `NDI_API(fn)`, because `libndi` NEEDs the Avahi client libraries. `test_plugin_elf` keeps `libndi` out of `NEEDED`.
- **Testing loop**, driven from the Mac with `scripts/linux_remote.sh` (BUILD.md, Linux section): Tier 0 = `sync` + `build` (the Rocky 8 container on the box, `ctest` included; CI runs the same on every push). Tiers 1–2 = `install` (to `/usr/OFX/Plugins`, no sudo), Matt fully restarts Resolve and presses play at the box, then verify from the Mac with `./build/ndi_verify --source "COMFYBOX (VR.NDI Sender)"`, so avahi and the firewall are part of the test. You can't restart Resolve, press play or use sudo on the box: ask for those as numbered steps.
- **Linux plugin cache:** `~/.local/share/DaVinciResolve/configs/OFXPluginCacheV2.xml`. `linux_remote.sh cache` shows the plugin's entry; `linux_remote.sh cache-reset` deletes it and refuses while Resolve runs.
- **Live logs on Linux:** `NDI_LOG` goes to journald tagged `NDIOutput` (`scripts/linux_remote.sh logs` = `journalctl -f -t NDIOutput`), plus the optional `NDI_OUTPUT_LOG_FILE` sink. Each line is tagged per message: never call `openlog()` in the plugin, because it would re-tag Resolve's own syslog lines.
- **Never stop `avahi-daemon` while Resolve runs on the box**: Resolve's own Bonjour layer segfaults. NDI on Linux is only discoverable while the daemon runs (docs/2026-10-30-linux-host-probes.md §5).
- **GPU rules on Linux:** the same as Windows (enqueue on the host's CUDA stream; never `cudaDeviceSynchronize` or `cudaDeviceReset`). Until #74 verifies CUDA on the box, dev builds are CPU-only (`linux_remote.sh build`; `--cuda` opts in), so the plugin doesn't declare CUDA rendering.
- The LEARNINGS.md standing rule applies to Linux work unchanged, and BUILD.md stays the single source of truth for build/install.

## Key commands

```bash
make dev                 # build → NDIOutput.ofx.bundle/
sudo make install        # install to /Library/OFX/Plugins + fix dylib path
./scripts/monitor_ndi_logs.sh   # live plugin logs (os_log, prefix "NDI Plugin:")
```

Resolve must be fully restarted to pick up a new binary (no OFX hot reload). The plugin cache at `~/Library/Application Support/Blackmagic Design/DaVinci Resolve/OFXPluginCacheV2.xml` re-scans on binary mtime/size change; delete it (Resolve closed) if the plugin vanishes from the Effects Library.

## Where things are

- `src/NDIOutputPlugin.cpp` — the whole plugin (OFX C API, no wrapper classes); Metal path in `src/MetalGPUAcceleration.mm`, CUDA port in `src/CudaGPUAcceleration.cu`
- `docs/` — research documents (e.g. the stereo program-tap feasibility study driving the roadmap)
- `LEARNINGS.md` — workflow, testing loop, environment map, solved-bug log
