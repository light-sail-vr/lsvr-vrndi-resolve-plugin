# Spec: Linux Port of the NDI Output OFX Plugin

**Status:** approved plan (Matt, 2026-10-30) · **Date:** 2026-10-30 · **Base:** `dev` (work on `feature/linux-<name>` branches)
**Prior art:** [windows-port-spec.md](windows-port-spec.md) — this port reuses its toolchain (CMake + CUDA 12.9), GPU module, test strategy and release model. Where a decision is "same as Windows", the Windows spec's rationale applies unchanged.
**Supersedes:** the Windows spec's out-of-scope line "Linux (the old scaffold's Linux stub stays a stub)".
**Tickets:** GitHub issues labelled `linux`, each referencing this spec; spec issue #70; work tickets #71 (workstation, human), #72 (CMake + CI), #73 (first stream), #74 (CUDA), #75 (Timeline Auto), #76 (Browse), #77 (BRAW), #78 (packaging/release). Blocking edges are native GitHub dependencies; work the frontier.

## Problem Statement

Studios that run DaVinci Resolve on Linux can't use the plugin, even though they are often the facilities with the heaviest NVIDIA hardware. Linux is the only desktop platform Resolve runs on where the plugin doesn't exist. The macOS and Windows builds also share almost all of their code now: the portable core, the CUDA module, the POSIX helper-process code. Most of a Linux port has therefore already been written and only needs a build, platform plumbing and packaging.

## Solution

Ship Linux as a third platform of the same product, with one codebase, one version and one release event that carries per-platform artifacts. Linux users download a tarball from the GitHub release, run `sudo ./install.sh`, restart Resolve and get the same NDI Output effect, at **full feature parity** with macOS and Windows:

- SDR/HDR output color
- stereo pairing
- STMap/ILPD dewarp
- resolution controls
- Browse buttons
- Timeline (Auto) camera-clip source and Timeline Color = Auto
- BRAW Camera Metadata projection

On NVIDIA hardware the pipeline is GPU-native through the existing CUDA module. Everything else uses the CPU path as a fallback.

**Support statement:** tested on Ubuntu (Matt's workstation, RTX 30/40-class GPU, Resolve Studio). Built for Rocky Linux 8+ and glibc 2.28+, the platform Blackmagic certifies. CI shows that the binary loads in a Rocky 8 container, but Rocky gets no Tier 1–2 pass. Other distros are expected to work but are unsupported.

## User Stories

1. As a Linux Resolve editor, I want to add the NDI Output effect and see my program feed as an NDI source on the network, so that I can monitor on any NDI receiver.
2. As a Rocky Linux user, the platform Blackmagic certifies, I want the same tarball to install and stream, so that I'm not left out because the maintainer tests on Ubuntu.
3. As an immersive editor on a Linux NVIDIA workstation, I want eye pairing, packing, downscale and color conversion to run on the GPU through CUDA, so that 8K stereo plays in real time.
4. As a colorist on Linux, I want HDR and the Output Color presets to produce byte-identical results to macOS and Windows, so that my monitor shows the real grade.
5. As a Linux user whose GPU isn't NVIDIA, or whose Resolve is set to OpenCL, I want an automatic CPU fallback with a documented resolution ceiling, so that the plugin still works.
6. As a user whose NVIDIA driver is too old for the plugin's CUDA runtime, I want the plugin to fall back to the CPU path with a status message instead of failing.
7. As a stereo/immersive editor on Linux, I want STMap/ILPD dewarp, BRAW Camera Metadata projection and Timeline (Auto) to behave exactly as on the other platforms, so that I can use the same workflows.
8. As a user, I want the Browse buttons to open a native file picker on my desktop (GNOME or KDE, X11 or Wayland), so that I don't have to type paths by hand.
9. As a user with no NDI software installed, I want the plugin to work out of the box, so that I never have to hunt for an NDI runtime.
10. As a user installing the plugin, I want one `sudo ./install.sh` (and a matching uninstall), and a clear message if a runtime prerequisite such as `avahi-daemon` is missing, so that the install either works or tells me why it can't.
11. As a user whose stream doesn't appear on the network, I want the avahi/firewall requirements documented, so that I can fix discovery myself.
12. As a user whose plugin vanished from the Effects Library, I want a documented plugin-cache reset procedure for Linux.
13. As a user reporting a problem, I want live plugin logs I can capture with a standard tool (`journalctl`), so that I can attach diagnostics.
14. As the maintainer, I want compile-only Linux CI on every push, including a load check inside a Rocky 8 container, so that the Linux build can't silently rot.
15. As the maintainer, I want the portable unit suite and the CUDA byte-identity test running under GCC, so that platform-neutral regressions surface without hardware.
16. As the maintainer, I want to drive the Linux build, install and logs from my Mac over SSH, so that I only walk to the Linux box to restart Resolve and press play.
17. As the maintainer, I want Linux artifacts attached by the same publish flow as macOS and Windows, with one `VERSION` and one `CHANGELOG.md`.
18. As the maintainer, I want the NDI attribution and licensing obligations honored in the Linux tarball, so that redistribution stays compliant.

## Implementation Decisions

**Product shape**

1. **One codebase, one version** (same as Windows decision 1). Artifacts are platform-suffixed (`NDIOutput-<V>-linux-x86_64.tar.gz`). A release may ship with Linux lagging, stated in the notes.
2. **Branching:** `feature/linux-<name>` off `dev`, merged into `dev` by PR once the Linux testing loop passes. There is no long-lived `linux-port` branch (the 2026-09-01 lesson). The release bar (decision 3) gates the first `dev` → `master` release that carries Linux artifacts, not integration into `dev`.
3. **Release bar for the first Linux release:**
   - install from the draft-release tarball on the Ubuntu box, with no NDI SDK on the loader path, through to a visible stream
   - GPU-native CUDA path real-time at 8K stereo
   - every parity feature verified in Tier 2
   - the Rocky 8 container load check green
   - support statement in the release notes

**Build & ABI**

4. **CMake drives the Linux build.** Extend the existing `CMakeLists.txt`, dropping its `if(NOT WIN32) FATAL_ERROR` and branching per platform, rather than extending the macOS Makefile: CMake already owns the CUDA build and the test list. The Makefile stays the macOS harness; BUILD.md remains the contract.
5. **One binary, built on an old glibc.** Release builds happen inside a **Rocky Linux 8 container** (`nvidia/cuda:12.9.x-devel-rockylinux8` or equivalent; glibc 2.28) with GCC from `gcc-toolset`, never directly on the Ubuntu box, where the glibc is too new for Rocky. Docker on the workstation runs the container, and CI uses the same image.
6. **Isolate the C++ runtime from Resolve's.** Resolve ships its own libraries under `/opt/resolve/libs`, so link with `-static-libstdc++ -static-libgcc` and `-fvisibility=hidden`, and use a version script that exports only `OfxGetNumberOfPlugins` and `OfxGetPlugin`. Add `-Wl,--no-undefined` and `-Wl,-z,defs`. zlib links statically. CUDA: `CUDA_RUNTIME_LIBRARY Static` (as on Windows).
7. **Bundle layout follows the OFX spec:** `NDIOutput.ofx.bundle/Contents/Linux-x86-64/NDIOutput.ofx`, installed under `/usr/OFX/Plugins/`. Resources (watcher scripts, icon) go in `Contents/Resources/` as on the other platforms.

**NDI runtime & compliance**

8. **Bundle the Standard NDI SDK for Linux runtime** (`libndi.so.6`, plus its licenses file) beside the `.ofx`, and **resolve it at runtime with `dlopen` by sibling path** through the SDK's `Processing.NDI.DynamicLoad.h`, rather than a link-time import. *Amended 2026-10-30 (#71): the plan was a normal link with `RUNPATH=$ORIGIN`, but `libndi.so.6` itself has `NEEDED libavahi-client.so.3`/`libavahi-common.so.3`. A link-time import would make the plugin fail to load, and vanish from the Effects Library, on any machine without the Avahi client libraries.* With `dlopen`, a missing runtime or missing Avahi means a pass-through plugin with a logged reason and a Stream Status message, never a vanished plugin. This is the Linux equivalent of Windows decision 12, and it's simpler: no `NDIRuntimeLoader.h` delay-load machinery is needed. The bundle path comes from `dladdr` on a plugin symbol.
9. **CI links a stub `libndi.so.6`** generated from the checked-in export list (`third_party/ndi/`, the same list the Windows stub uses), so CI never ships NDI code. The stub artifact is named `-STUB` (the LEARNINGS lesson from the Windows installer).
10. **Avahi is a runtime prerequisite**: both the client libraries (`libavahi-client3` on Ubuntu, `avahi-libs` on Rocky), which `libndi` links, and a running `avahi-daemon`. `install.sh` checks for both and warns. The README documents them along with the firewall rules: mDNS 5353/udp plus NDI's 5959–5999, 6960–6999 and 7960–7999 tcp+udp, **for IPv6 link-local (`fe80::/10`) as well as the IPv4 LAN**. *Verified on the box 2026-10-30 (#71): avahi advertises `fe80::` addresses, macOS receivers connect over them, and with IPv4-only rules the source appears but no video arrives.* Rocky's `firewalld` is on by default; `ufw` was active on the Ubuntu test box.
11. **Attribution** (same as Windows decision 13): the licenses file in the bundle, the ndi.video link and the trademark line in the tarball README and release notes, and an SDK no more than 30 days old at release.

**GPU**

12. **CUDA-native from v1:** lift the `#ifdef _WIN32` gate on `src/CudaGPUAcceleration.{cu,h}` (and its `windows.h` include) so the module builds on Linux. `NDI_HAS_CUDA` already gates the plugin's GPU-native seam. Same contract, kernels, slot ring and host-stream discipline (never `cudaDeviceSynchronize`/`cudaDeviceReset`; enqueue on the host stream).
13. **CUDA 12.9, same architecture list** (`60-real;75-real;86-real;86-virtual`). The test box (RTX 30/40) is covered natively, and newer GPUs JIT from the PTX.
14. **Byte identity under GCC:** keep `--fmad=false` for nvcc, and keep the CPU references free of FMA contraction (no `-march=native`/`-mfma`; x86-64 baseline only). `test_cuda_downscale` enforces this on the box.
15. **OpenCL/AMD stays deferred** (same as Windows decision 4); CPU fallback covers it.

**Platform plumbing** (platform-prefixed files behind the existing seams; capability macros, not raw `__linux__`, at call sites)

16. **Logging:** on Linux, `NDI_LOG` goes to `syslog(3)` with ident `NDIOutput`, readable live with `journalctl -f -t NDIOutput` over SSH. Add the same optional `NDI_OUTPUT_LOG_FILE` file sink Windows has. This replaces today's `printf` fallback, whose output goes nowhere when Resolve is launched from the desktop.
17. **Timeline (Auto) + Timeline Color = Auto:** **macOS code stays untouched** (Matt, 2026-10-30). Linux gets its own implementation of the `TimelineClipWatcher.h` API in `src/LinuxTimelineWatch.cpp`, the way Windows has `WinTimelineWatch.cpp`. It ports the `posix_spawn`/pipe/reader-thread logic from the macOS `src/TimelineClipWatcher.cpp` rather than sharing it, and adds a `src/LinuxTimelineWatch.h` discovery seam:
    - Host executable from `/proc/self/exe`.
    - fuscript at `<resolve>/libs/Fusion/fuscript` (path to verify).
    - Python fallback is `/usr/bin/python3`, which is real on Linux, with `RESOLVE_SCRIPT_API`/`RESOLVE_SCRIPT_LIB` set for the child.
    - Extend `NDI_TIMELINE_WATCH` to Linux.
18. **Browse buttons:** `src/LinuxFileDialog.cpp` behind the existing `native_open_file_dialog` contract (UTF-8 in/out; cancel or failure returns false with the buffer untouched).
    - Mechanism: spawn **`zenity --file-selection`**, falling back to **`kdialog --getopenfilename`**. If neither exists, the button soft-fails with a Stream Status message naming the package to install.
    - Linking against Resolve's Qt was rejected: ABI coupling to the host's private Qt is the clash decision 6 exists to avoid.
    - The xdg-desktop-portal D-Bus route was rejected as far heavier for the same result. Both helpers speak to the portal themselves under Wayland.
    - Unit tests cover argv construction and output parsing.
    - Extend `NDI_HAS_BROWSE_DIALOGS` to Linux.
19. **BRAW Camera Metadata:** add a Linux branch to `src/BRAWImmersiveReader.cpp`. The Linux BRAW SDK uses the same C++ (non-COM-IDL) header as macOS. Vendor the Linux dispatch shim under `third_party/braw/Linux/` and bind Resolve's own `libBlackmagicRawAPI.so`, located from `/proc/self/exe` (path to verify), so nothing Blackmagic is bundled. Introduce an `NDI_HAS_BRAW_READER` capability macro to replace the raw `__APPLE__ || _WIN32` checks in `BRAWImmersiveReader.h` and `NDIOutputPlugin.cpp`, per the CLAUDE.md platform-isolation rule.
20. **Paths:** OFX hands the plugin UTF-8 and Linux file APIs are UTF-8, so `PlatformPaths.h` stays a passthrough. Add a test for a non-ASCII STMap path anyway.

**Packaging & release**

21. **Tarball plus `install.sh`/`uninstall.sh`:**
    - The tarball holds `NDIOutput.ofx.bundle/`, `install.sh`, `uninstall.sh`, `README.txt` and the NDI licenses file.
    - `install.sh` checks for root and replaces any existing bundle in `/usr/OFX/Plugins/`.
    - It checks for `avahi-daemon` and for an NVIDIA driver (informational only).
    - It prints the Resolve-restart and cache-reset hints.
    - Plus `SHA256SUMS.txt`. No .deb/.rpm, and no signing for v1.
22. **`scripts/package_linux_release.sh`** builds inside the Rocky 8 container with the real SDK and refuses to package a stub. `scripts/publish_github_release.sh` attaches the Linux artifacts. `docs/RELEASING.md` gains a Linux hand-off that is **SSH-driven from the Mac session**: build, install and logs over SSH; Matt restarts Resolve and starts playback at the box.

## Testing Decisions

- **No new seams beyond the platform-prefixed ones above.** The GPU seam is the existing GPU-module contract, unchanged. The CUDA module's byte-identity test is the oracle on Linux, as on Windows.
- **Tier 0 (Linux)** is a CMake build plus `ctest` under GCC. CI runs it on every push: the portable suite, the Linux watcher and dialog seam tests, and an ELF-hygiene test. The ELF-hygiene test is the Linux counterpart of `test_plugin_delayload`. It asserts:
  - `NEEDED` libraries are only glibc/libm/libdl/libpthread/librt (no `libstdc++`, no `libz`, no `libcudart`, and since #73 no `libndi.so.6`, which decision 8 `dlopen`s)
  - no `GLIBC_` symbol version above 2.28
  - no `RUNPATH` or `RPATH` (*amended by #73: `$ORIGIN` was only there for the link-time `libndi`*)
  - only the two OFX entry points are exported

  It also `dlopen`s the `.ofx` alone (no `libndi.so.6` beside it) in a Rocky 8 container and in an Ubuntu container, and calls `OfxGetNumberOfPlugins`.
- **Tiers 1–2 (Linux)** run on the Ubuntu box, driven over SSH from the Mac:
  1. rsync, build, then install to `/usr/OFX/Plugins` (`scripts/linux_remote.sh`; `matt` owns the directory on the test box, so no `sudo`).
  2. Matt fully restarts Resolve and starts playback at the box.
  3. The stream is verified from a **second machine** (Mac NDI Video Monitor / Studio Monitor), so avahi/firewall behaviour is part of the test.
  4. `ndi_verify` and the patch and resolution charts run unchanged.
- **Host-behaviour probes re-run on Linux before trusting them:** the Log Render Calls matrix (Render Cache, proxy, stereo per-eye instances) and the colour-management probe, including whether Resolve on Linux hands the plugin CUDA buffers and the CUDA stream.
- **The NDI-runtime test runs with no NDI SDK on the loader path** (`ldconfig -p | grep ndi` empty), the end-user machine the bundled `.so` exists for.
- **The release-shaped test** installs from the draft GitHub release tarball, checks the stream, and checks that uninstall removes everything.

## Out of Scope

- OpenCL backend for AMD/ROCm full-resolution GPU support (deferred, as on Windows).
- Linux ARM64 (Resolve ships no Linux ARM build).
- .deb/.rpm packages, a package repository, Flatpak/Snap, GPG/code signing for v1.
- Tier 1–2 on Rocky (CI load check only; revisit if Rocky users report problems).
- Free (non-Studio) Resolve as a tested configuration. The base stream should work, but Timeline (Auto) needs Studio's external scripting, as on the other platforms.
- Migrating the macOS build to CMake.
- Transport control (unchanged: playback is human-started on every platform).

## Further Notes

**Host facts to verify on the box (ticket #71)** before code depends on them. All are unconfirmed for Linux:
- Location of `OFXPluginCacheV2.xml`. Blackmagic documents only macOS/Windows; find it with `find ~ -name OFXPluginCacheV2.xml` after Resolve runs once.
- fuscript path.
- `libBlackmagicRawAPI.so` path.
- Whether `/usr/OFX/Plugins` is the only scan path.
- The CUDA OFX property behaviour.
- Whether `libndi` hard-requires a running avahi-daemon or only degrades discovery.

**Estimate:** about 2–3.5 focused weeks to the first Linux release at full parity. That is less than Windows (2.5–4.5 weeks without parity) because the CUDA module, the POSIX watcher code, the test strategy and the release model already exist. The hardware already exists.

**Seam check (resolved 2026-10-30):** Matt chose to leave shipping macOS code untouched. The Linux watcher is therefore a third per-platform implementation (`LinuxTimelineWatch.cpp`) rather than a shared POSIX watcher, and duplication with the macOS file is accepted. The same principle applies throughout the port: Linux work adds `Linux*` files and widens capability macros, and does not restructure macOS sources.
