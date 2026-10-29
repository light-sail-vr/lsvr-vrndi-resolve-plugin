# Changelog

All notable changes to the NDI Output Plugin will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Fixed
- **macOS: Resolve's scripting no longer breaks after you quit and reopen Resolve.** With **Camera Clip Source: Timeline (Auto)** or **Timeline Color: Auto**, the plugin's background helper inherited Resolve's network listeners: the scripting port and NDI's. When Resolve quit, the helper kept running and held those ports. On the next launch Timeline (Auto) and Timeline Color stopped following the timeline, and scripts and scripting tools couldn't reach Resolve until the leftover `fuscript` process was killed. The helper now starts without any of Resolve's open files or sockets and exits with Resolve. Windows was not affected.

## [1.17.0] - 2026-10-20

### Fixed
- **Stream levels: every UYVY stream now changes visibly** (#57). The encoder wrote full-range Y'CbCr (black at 0, white at 255), but NDI is a limited-range format, so NDI's decoder (and VR.NDI) crushed everything below about 6% to black, clipped everything above about 92% to white, added about 16% extra contrast and saturation, and tinted neutrals slightly green. The plugin now encodes BT.709 limited range (black 16, white 235, neutral chroma 128) on every path — CPU, Metal and CUDA. **Existing streams look lower in contrast than before: that is the fix.** Shadows and highlights that used to vanish are back, and greys are neutral. Measured through NDI's own decoder, every patch of the new test chart now lands within 1 code of the source (it was off by up to 20). Optimal Color Format off is converted by NDI itself (see Changed).
- **Tiny thumbnail frames on the stream** (#58). With **Stereo Packing** set to Side-by-Side (the default) or Top-Bottom on a mono (2D) timeline, the small preview thumbnail Resolve renders after every parameter change and after you pause went out on the NDI stream. Receivers saw the picture shrink to 92×46 (46×22 at Resolution: Half) and back, and after a pause the stream could be left showing it. Thumbnails are no longer streamed in any Stereo Packing mode. They still render inside Resolve. Mono packing already skipped them.

### Changed
- **Optimal Color Format off now sends RGBX instead of RGBA** (#57). NDI converts that path itself, and with an alpha channel its decoder was off by up to 16 codes on saturated colours (blue 255 arrived as 239). Without alpha the worst error is 4 codes. The stream never carried a meaningful alpha; receivers show it opaque either way. Optimal Color Format on (the default) is the accurate path.
- **The macOS plugin is now built with compiler optimisation.** Earlier releases shipped unoptimised, which made the CPU paths (frames Resolve hands over in CPU memory, Optimal Color Format off, the CPU fallback) roughly 17 times slower than necessary. The Metal path was not affected.

### Added
- **Output Color group** (#57). Choose what colour the stream carries, independently of Resolve's own output: **Passthrough** (default, unchanged behaviour), one of seven display spaces (Rec.709 Gamma 2.4 and 2.2, sRGB, P3-D65 Gamma 2.2 and 2.6, Display P3, Rec.2020 Gamma 2.4), or a 3D **LUT File** (`.cube`, up to 65 points, with a Browse button). An OpenFX node sees the *timeline* colour space, not the output: Resolve's output colour conversion runs after every node, so on a log timeline (DaVinci Wide Gamut/Intermediate, ACEScct) the stream is the flat log picture. For those, generate a LUT from a Color Space Transform node in Resolve and use LUT File. Display spaces convert display to display, and colours outside the target gamut clip. The node's own output back to Resolve never changes. On the GPU path a display space costs about 0.3 ms per eye at 8K; on the CPU fallback path it adds about 9 ms per 1080p frame.
- **Timeline Color: Auto** (#57). The plugin works out the timeline colour space from Resolve: the OpenFX colour-management property when Resolve reports one, otherwise the timeline's colour settings through external scripting (the same setting Timeline (Auto) camera clips use). **Stream Status** shows the conversion in use (e.g. `color: Rec.709 G2.4 → P3-D65 G2.2`) and warns when the timeline is log or HDR while a display space is selected, or when the timeline colour can't be determined (Rec.709 Gamma 2.4 is then assumed).
- **Colour label on every frame** (#57). When the stream's colour space is known, each frame carries an `lsvr_color` tag in its NDI metadata (primaries, transfer, matrix, range), because NDI's own colour metadata can't name P3-D65 or gamma 2.2. Default Passthrough streams are labelled with the timeline's colour space when Resolve reports it, so with external scripting enabled the plugin now runs its small timeline helper by default, the same one Timeline (Auto) camera clips use. Receivers that don't read it assume Rec.709 as before; VR.NDI support is tracked in the Quest repo. `ndi_verify` prints the tag and can assert it (`--expect-color`).
- **Stream Status shows the frame size on the wire** (#55), e.g. "Stereo (Side-by-Side), sending 7200x3600 (3600x3600 per eye)". It is the frame Resolve feeds (or the map, in the Equirect modes) divided by **Resolution**. If that number is what you expect, a soft picture is on the receiving side. Background: with Resolution: Half on a 7200×7200 stereo timeline the plugin sends 3600×3600 per eye, and reports of it looking like quarter resolution traced to the VR.NDI Quest app drawing every source into a fixed 1920-per-eye surface. **VR.NDI 4.2.0** (native Android) sizes its surface from the source; pair this release with it.
- Developer tooling, not part of the installed plugin (#54, #57): `make ndi-verify` builds a reference receiver that reports a source's on-wire geometry, frame rate, drops and colour label, with `--expect` assertions. `scripts/gen_resolution_chart.py` makes a labelled resolution test chart, and `scripts/gen_patch_chart.py` a levels/primaries/grey-ramp chart that `ndi_verify --patches` reads back off the wire, failing when a patch is more than 2 codes off. `tools/ndi_color_check.py` checks every display space the same way, against values computed from the published standards. See BUILD.md, "Receiving the stream".

### Notes
- There was no 1.16.0 release: the changes prepared for it (the frame-size readout and the developer tooling) are in this release.
- In the Equirect (STMap / Camera Metadata) modes the stream is the **map size** divided by Resolution, not the timeline divided by Resolution: Metadata Map Size 2048 with Half sends 1024 per eye. For a 180° headset view at full detail use 4096 with Half, or 2048 with Full.
- Windows: built from the same source. The Windows installer follows once its Tier 1–2 check passes, so it may lag the macOS artifacts.

## [1.15.0] - 2026-10-01

### Added
- **Mono option for 360 streams** (#49). **Stereo Packing** now offers Side-by-Side, Top-Bottom and **Mono**. On a stereo timeline, Mono streams only the left eye as a normal single frame, for monoscopic 360 receivers. Right-eye renders are skipped, so they cost no GPU time, and Resolve's own output is unaffected. **Stream Status** reads "Mono (left eye of stereo pair)". Mono timelines stream exactly as before.

### Fixed
- **Stereo playback on the Edit page streamed nothing** (scrubbing worked). Resolve renders the right eye well ahead of the left there, and the plugin gave up waiting for the matching left frame. It now waits long enough to pair them. The bug was also present in 1.14.2.

### Removed
- **HDR output** (#48). The plugin is built on the Standard NDI SDK and now streams SDR only (8-bit, Rec.709). The **HDR Settings** group (Enable HDR, Color Space, Transfer Function, Max CLL/FALL) is gone. Projects saved with HDR switched on still open and stream SDR.

### Known issues
- **Half timeline resolution: the left eye zooms in during stereo playback.** With the timeline resolution set to half, Resolve renders the left eye at full resolution into a half-resolution frame during playback, so it shows that eye's top-left quarter at 2× (Resolve's own viewer jumps too). Scrubbing is unaffected, and 1.14.2 does the same. Workaround: keep the timeline at full resolution and use the plugin's **Resolution: Half** to shrink the stream.

## [1.14.2] - 2026-10-01

### Changed
- **Built on the Standard (royalty-free) NDI SDK** instead of NDI Advanced (#43). The installers now bundle the Standard NDI runtime: `libndi.dylib` on macOS and `Processing.NDI.Lib.x64.dll` on Windows. End users still need nothing else installed, and NDI receivers work as before.
- The README and installer text no longer advertise HDR (#43, #44).

### Fixed
- **Version scripts on macOS**: `scripts/set_version.sh` and `scripts/increment_version.sh` recursed forever in their BSD `sed` branch. They wrote `VERSION`, then crashed before updating the version `#define`s. Developer tooling only; shipped binaries were not affected.


## [1.14.1] - 2026-09-01

First release for **Windows** alongside macOS — one codebase, one version, per-platform installers. Windows build: NVIDIA GPUs get the CUDA-native pipeline (verified through 8K stereo playback); other GPUs fall back to the CPU path (HD/UHD-viable — an OpenCL backend for full-resolution AMD/Intel support is deferred until there is demand). **x64 only — Windows on ARM is not supported** (no CUDA exists for it). The Windows installer is not code-signed; see the SmartScreen note in the install section below.

### Added
- **Windows installer** (ticket #23): Inno Setup installer to `C:\Program Files\Common Files\OFX\Plugins`, Add-or-Remove-Programs entry, `/VERYSILENT` fleet deployment, NDI attribution readme and third-party licenses beside the binaries. Ships unsigned with the SmartScreen "More info → Run anyway" flow documented. Release flow attaches installer + bare-bundle zip + `SHA256SUMS-Windows.txt` next to the macOS artifacts.
- **Timeline (Auto) is one-click on both platforms**: the camera-clip watcher now runs a Lua helper under Resolve's own bundled script interpreter (`fuscript`) — no Python install required. The Python helper remains an automatic fallback.

### Fixed
- **macOS: Timeline (Auto) no longer dies silently on Macs without the Xcode Command Line Tools** (#34): the watcher used to spawn `/usr/bin/python3`, which on a stock Mac is Apple's CLT shim — it exits immediately, so the helper looped every 30 s with nothing in the log. The Python fallback now resolves a real interpreter (Xcode developer dir or Homebrew), probes it once, and never the shim. When no interpreter exists at all the log names the problem once instead of looping, and a dead helper's exit status is logged.
- **Windows: watcher no longer loops silently on machines without Python**: discovery rejects the Microsoft Store's `python.exe` App Execution Alias stub, and the helper-exit log now carries the child's exit code.


## [1.14.0] - 2026-08-30

### Changed
- **New-instance defaults tuned for the common monitoring setup**: Frame Rate 30 (was 25), Resolution Half (was Full), STMap Layout Packed Side-by-Side (was Per-Eye Files). Saved projects keep their stored values.

### Added
- **Single "STMap" field with its own "Browse for STMap..." button** for the packed side-by-side layout — the per-eye left/right fields now appear only when the layout is Per-Eye Files. Projects that stored a packed map in the left-eye slot before this release keep streaming (the packed field falls back to it while empty).
- **Mode-aware panel**: the Camera Clip (.braw) field and its browse button show only when Camera Clip Source is Manual Path; Timeline (Auto) hides them.

## [1.13.0] - 2026-08-30

First public release.

### Added
- **Signed macOS installer**: `NDIOutput-<version>-macOS.pkg` — Developer ID signed, notarized, and stapled; installs to `/Library/OFX/Plugins`. A signed `.zip` of the bare bundle is published alongside it for manual installs. Universal binary (Apple Silicon + Intel), macOS 13+.
- **The NDI runtime ships inside the plugin** (`Contents/Frameworks/libndi_advanced.dylib`, referenced via `@loader_path`) — end users no longer need any NDI SDK installed. NDI® is a registered trademark of Vizrt NDI AB; third-party notices are included in `Contents/Resources/libndi_licenses.txt`.
- Release tooling: `scripts/package_release.sh` (build → sign → pkg → notarize) and `scripts/publish_github_release.sh` (tag + draft GitHub release). See BUILD.md "Release packaging".

### Fixed
- **Minimum macOS is now correctly 13.0**: binaries were previously stamped with the build machine's OS version and would refuse to load on anything older.
- **Bundle layout corrected for code signing**: executable directory renamed to the canonical `Contents/MacOS/` spelling and the icon PNG moved into `Contents/Resources/` — both broke codesign's resource seal (harmless to Resolve, fatal to signing).

## [1.12.0] - 2026-08-30

### Fixed
- **Stereo↔mono geometry pops on eye stalls** (#12): the stream's canvas is now locked once stereo latches — an eye starving no longer shrinks the frame to mono (a severe nausea trigger in-headset). Degraded mode packs the flowing eye into both halves on the unchanged canvas, degrade/recover both have hysteresis (≈4 s sustained silence to degrade, 3 clean frames to recover), and a parked timeline can no longer degrade at all. During stalls the last packed frame is re-sent so receivers keep flowing.

## [1.11.0] - 2026-08-30

### Added
- **Timeline (Auto) camera-clip source** (#11): Camera Metadata projection now follows the playhead. A bundled helper (`ndi_timeline_watch.py`, spawned with the system python3) polls the Resolve scripting API for the clip under the playhead and swaps lens calibrations automatically at cuts — per-camera maps are cached, and gaps/uncalibrated clips keep the last camera's warp so the stream never pops. Requires Resolve's external-scripting preference (Local); Manual Path mode is unaffected without it.

## [1.10.0] - 2026-08-30

### Added
- **Equirect (Camera Metadata) projection** (#11): warps URSA Cine Immersive BRAW footage to equirect directly from the lens calibration embedded in the clip (Mei-Rives model) — no STMap files needed. The Blackmagic RAW API is resolved at runtime from Resolve's own bundle; nothing Blackmagic is linked or shipped, and clips without calibration fall back soft to passthrough with a Stream Status message.

## [1.9.0] - 2026-08-30

### Added
- **Browse buttons for the STMap paths** (macOS): Resolve draws no browse control on OFX filePath string params (confirmed in the first v1.8.0 session — plain text field), so the plugin now provides **Browse for Left/Right-Eye STMap…** push buttons that pop a native macOS open panel (filtered to .exr, starting in the current path's folder) and fill the field. Cancel changes nothing; pasting a path still works. The panel runs modally inside the parameter-change action — flagged for a stability check at the next Tier 1 pass.

## [1.8.1] - 2026-08-30

### Fixed
- **Packed side-by-side split misclassified the Canon EOS R5C RF5.2mm STMap's left half** (found in Matt's first Tier-2 session with v1.8.0): the left eye of the warped stream showed a large black disc. The map's left-eye half samples the packed frame's right half (Canon's eye swap) — but ~1% of its texels are (0,0) *filler* padding the unused corners, and the U-convention auto-detect used absolute min/max bounds, so those zeros defeated the packed-frame classification and the half went out un-rescaled. Detection now keys on where the **mass** of the valid values sits (up to 5% spill tolerated); filler texels rescale to out-of-range and render black, which is correct for content-free regions. Verified against the real Canon map: both halves classify as packed-frame and the left eye's black coverage drops to the healthy baseline.

## [1.8.0] - 2026-08-30

> Tier 0 + `make test` (179 assertions; 21 new for the packed-map split) + `make test-metal` pass. Tier 1–3 pending alongside 1.7.0.

### Added
- **STMap Layout** parameter (Projection group): **Per-Eye Files** (as in 1.7.0) or **Packed Side-by-Side** — one EXR whose left half maps the left eye and right half the right eye (Canon VR-style authoring). In packed layout the single file goes in the left-eye slot and the plugin splits it into per-eye maps; the right-eye slot is ignored. Split halves are cached process-wide like whole files, and the full packed image is only held while splitting.
- Each half's U-coordinate convention is **auto-detected** (per-eye [0,1] vs packed-frame [0,0.5]/[0.5,1]) and rescaled when needed; the decision is logged per half. Detection is per half, so maps that bake in the Canon eye swap (a destination half sampling the other source half) rescale correctly too. Eye assignment always comes from the timeline's stereo tracks — the map halves define geometry only.
- Note: for a packed-frame source on a **mono** timeline (e.g. a dual-fisheye clip), keep **Per-Eye Files** and load the one packed map in the left slot — it warps the whole frame and streams packed SbS equirect (this worked in 1.7.0 already; now documented in the hints).

### Changed
- The STMap path params leave `FilePathExists` at its spec default, so a host that renders a picker for filePath strings shows an open-existing dialog. Whether Resolve draws a browse button is still the pending Tier-1 check; pasting a path works regardless.

## [1.7.0] - 2026-08-30

> Tier 0 + `make test` (158 assertions incl. the 41 new STMap-seam checks) + `make test-metal` (25 checks incl. the warp kernels) pass. Tier 1–3 (Resolve install, stream check, fisheye timeline verified in the Quest stereo-180 player against a Fusion-authored STMap) pending.

### Added
- **Projection normalization** (issue #7): new **Projection** parameter group with a **Passthrough / Equirect (STMap)** mode. In Equirect mode the plugin warps each eye on the GPU through a per-eye 32-bit float (or half) EXR STMap — the Fusion-authored maps of the VR.NDI lens pipeline — **before** stereo packing, so an Apple Immersive fisheye timeline displays geometrically correct in the existing Quest stereo-180 mode with zero receiver changes. The warped output resolution is the STMap's resolution (the map defines the destination image); the Resolution control then applies on top.
- **STMap (Left Eye) / STMap (Right Eye)** file parameters. The left map is required for Equirect mode and serves both eyes (and mono timelines) when no right map is set. Loaded maps are shared process-wide (both stereo eye instances read the same files once) and reload when the file changes on disk.
- Minimal, fully bounds-checked EXR reader (`src/STMap.h`): single-part scanline, float/half channels, None/RLE/ZIPS/ZIP compression. Anything else — or a missing, truncated, or hostile file — **fails soft**: the stream continues in passthrough and Stream Status says why (e.g. "STMap invalid — passthrough (see log)"). Validated against third-party EXRs (ffmpeg-encoded fixtures) as well as in-test-built ones.
- STMap warp Metal kernels (UYVY + P216) fused with the downscale and running through the same non-blocking slot-ring submit path as v1.6.0 — the render action still only encodes. Every fallback keeps the corrected geometry: no Metal map upload → full readback + CPU warp; CPU render path → CPU warp.
- Stream Status now appends the projection state: ", Equirect (STMap)" while warping, or the passthrough reason (invalid map, L/R map size mismatch).

### Notes
- Passthrough mode takes exactly the pre-1.7.0 code path — output is bit-identical to 1.6.1.
- L/R maps must agree on resolution; a mismatch would give the two eyes different frame sizes (unpairable), so it falls back to passthrough with a status message instead.
- The plugin now links zlib (`-lz`, macOS SDK) for Zip-compressed EXR chunks.

## [1.6.1] - 2026-08-28

*(backfilled: 1.6.0/1.6.1 shipped without changelog entries; full detail in LEARNINGS.md, issue #5)*

### Fixed / Performance
- **1.6.0 — non-blocking GPU fast path:** the render action now only encodes the fused downscale+convert kernel into a ring of CPU-visible staging slots and returns (~0 ms); Metal's completion callback wakes a per-instance pump worker that pairs, packs, and sends off the render thread. Backpressure drops frames instead of ever blocking the host (8K stereo playback had collapsed 30→5 fps on ~90 ms of per-eye render-action blocking).
- **1.6.1 — the wait had moved, not died:** render threads were queueing on the sender-hub mutex while pump workers held it through a cold-page pairer copy plus a sync send. A lock-free `senderReady` atomic now answers the render path's only question, and the pairer recycles hold-payload buffers so the hold is a warm-page memcpy.

## [1.5.0] - 2026-08-28

> Validated 2026-08-28 through the LEARNINGS.md testing loop: unit tests, Tier 0, and Tier 1–2 in Resolve (stereo timeline streams packed stereo; mono unchanged).

### Added
- **Stereo eye pairing** (issue #6): on a native stereo timeline with the Stereo 3D palette at Vision: Stereo, Resolve renders each eye through its own plugin instance; the plugin now pairs the L/R renders for the same frame time — process-globally, across instances — and sends exactly **one packed NDI frame per pair**. Pairing follows the probe findings: keyed on frame time (never arrival order), tolerant of either eye leading by hundreds of ms and of in-eye time reversal, duplicate parked re-renders replace the held frame, and unmated frames age out bounded (`src/StereoPair.h`, unit-tested host-free via `make test`).
- **Stereo Packing** parameter (new Stereo group): Side-by-Side (left eye left) or Top-Bottom (left eye top), matching VR.NDI's projection modes on Quest.
- **Stream Status** parameter: live display of what the stream carries — Mono, Stereo (SbS/TB), a labeled single-eye fallback ("right eye missing — sending left eye only") when the partner eye starves (~1.5 s), or the sender-creation failure. Starvation never freezes or deadlocks the stream; the partner returning restores stereo automatically.
- Filmstrip thumbnail renders are dropped while stereo is active (a 184×92 frame must not hijack the packed stream); mono behavior is unchanged.

### Fixed
- **Stereo mode blacked out the NDI feed, then locked the source name machine-wide** (found in the issue #3 probe session; root-caused in LEARNINGS.md): both per-eye instances created senders under the same name — NDI 6.2 fails a duplicate same-machine name outright — and the failure path's `NDIlib_destroy()` tore the process-wide NDI library out from under the healthy sender, leaking its Bonjour advertisement until Resolve exited. Senders are now **process-shared and refcounted** (one sender per source name, which stereo requires anyway), `NDIlib_destroy()` is never called, and sender-create retries are throttled to every 3 s with the failure surfaced in Stream Status.
- A saved project's NDI Source Name is honored at project load: parameter values are read at createInstance, so the first sender no longer briefly registers under the default name.

## [1.4.0] - 2026-08-28

### Added
- **GPU-native fast path** (issue #5): the plugin now declares Metal render support (`OfxImageEffectPropMetalRenderSupported`), so Resolve hands frames as Metal device buffers instead of full-resolution float32 CPU copies. Fused Metal kernels box-downscale, vertically flip, and color-convert (UYVY, or P216 for HDR) on the GPU **before** any readback — only the small converted frame crosses to the CPU. At 8160×7200/eye that removes a ~940 MB/frame float RGBA transfer.
- **Resolution** control (Basic Settings): stream at **Full**, **Half**, or **Quarter** of the incoming frame. Effective on every path — GPU-native, GPU upload-convert, and pure CPU (box filter, `src/StreamResolution.h`).
- CPU fallback under Metal render: with GPU Acceleration off — or on any kernel failure, or the legacy RGBA output format — the frame is read back whole and the existing CPU path runs, so the stream works in every combination. Log evidence distinguishes the paths (`GPU-native path:` vs `Metal frame full readback -> CPU fallback path`).
- `make test-metal` — GPU kernel correctness tests against the CPU reference on a real Metal device (no Resolve or NDI SDK needed). `make test` additionally runs the new stream-resolution unit tests.

### Fixed
- Latent use-after-free: an async NDI send keeps reading the submitted buffer until the next send call, but send buffers were `resize()`d whenever the frame size changed (routine once the Resolution control lands mid-stream switches). In-flight async sends are now completed (NULL-frame flush) before any send-buffer reallocation.

## [1.3.0] - 2026-08-28

### Added
- Render-call diagnostic probe (issue #3): **Log Render Calls** toggle in a new Diagnostics parameter group, off by default. When on, every render action logs one `NDI Plugin: probe …` line carrying the Resolve page, eye (`OfxImageEffectPropEyeToRender`), frame time, source frame, render-window dimensions, render scale, thumbnail flag (`kOfxImageClipPropThumbnail`), and wall-clock spacing since the previous call. When off, the cost is one parameter read per render.
- Host-property plumbing the stereo work builds on: the instantiating page (`OfxImageEffectPropResolvePage`) is captured at createInstance, and eye/source-frame/thumbnail are read per render.
- `scripts/capture_probe_log.sh` — tees probe lines into `docs/captures/` for the findings report (`docs/2026-08-28-render-call-probe-findings.md`).
- `make test` — host-independent unit tests (probe log-line formatter), no Resolve or NDI SDK required.
- Vendored `openfx/include/ofxImageEffectExt.h` synced with the Resolve 21.0.4 SDK additions (`EyeToRender`, `SrcFrame`, `SrcFilePath`).

## [1.0.3] - 2024-05-27

### Removed
- Legacy build variants (NDIOutputSimple, NDIOutputModern)
- Legacy OFX wrapper class dependencies
- Unused build system complexity
- Old plugin installations to prevent conflicts

### Changed
- Streamlined build system to single modern implementation
- Simplified Makefile with clean targets
- Updated documentation to reflect simplified build process

### Fixed
- Removed potential plugin conflicts from multiple installed versions

## [1.0.2] - 2024-05-27

### Added
- Semantic versioning system with automatic patch increment on builds
- Version increment script (`scripts/increment_version.sh`)
- Version display in plugin description
- `make show-version` command to display current version
- `make dev` command for development builds without version increment
- Comprehensive version management documentation

### Changed
- Build system now automatically increments patch version on `make`
- Plugin description now includes version number
- Updated README with version management section

## [1.0.1] - 2024-05-27

### Added
- Initial version tracking system
- VERSION file for semantic versioning

## [1.0.0] - 2024-05-27

### Added
- Modern OFX C API implementation for maximum compatibility
- Comprehensive HDR support:
  - PQ (ST.2084) and HLG (Hybrid Log-Gamma) transfer functions
  - Rec.2020, DCI-P3, and Rec.709 color spaces
  - Configurable Max Content Light Level (CLL) and Max Frame Average Light Level (FALL)
  - HDR metadata embedding for proper downstream handling
- NDI Advanced SDK v6.1.1 integration
- Real-time streaming with pass-through design
- User-friendly parameter controls
- Professional-grade build system

### Changed
- Complete rewrite from legacy OFX wrapper classes to modern C API
- Improved compatibility with DaVinci Resolve 20+
- Enhanced performance and stability

### Removed
- Dependency on deprecated OFX C++ wrapper classes
- Legacy build system complexity

## [0.x] - Legacy Versions

### Features
- Basic NDI streaming functionality
- Original implementation using OFX wrapper classes
- Limited HDR support 