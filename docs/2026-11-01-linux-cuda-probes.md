# Linux CUDA path on hardware: byte identity, Tier 2 and the probe matrix (ticket #74)

**Date:** 2026-11-01 · **Host:** DaVinci Resolve Studio 21.1.0 build 17 on Ubuntu 24.04 (COMFYBOX: RTX 3090 Ti, NVIDIA 610.43.02; Resolve logs "Compute API set to automatic, defaulting to CUDA") · **Plugin:** 1.18.0, `feature/linux-cuda`, **CUDA build** from the Rocky 8 container (declares CUDA render + CUDA-stream support) · **Receiver:** `build/ndi_verify` on the Mac, **over Tailscale** (below).

This continues [2026-10-30-linux-host-probes.md](2026-10-30-linux-host-probes.md), which ran the CPU-only build. Fact labels as in LEARNINGS.md: (L) verified on the box.

**Conditions that bias every number here:**
- **The GPU was shared.** ComfyUI held 11.7–18.6 GB of the 24 GB VRAM and up to 100% GPU utilisation for the whole session; it couldn't be stopped. Resolve hit `cudaErrorMemoryAllocation` ("GPU memory full" dialogs) on the 8K timeline (LEARNINGS 2026-11-01).
- **The receiver was off the LAN.** The Mac reached the box only over Tailscale, so discovery used an `NDI_CONFIG_DIR` extra-IP config, and the link's bandwidth caps receive-side fps for big frames. Avahi and the LAN firewall are **not** exercised by these receives (LEARNINGS 2026-11-01).

## Summary

| Item | Result |
|---|---|
| `test_cuda_downscale` on the 3090 Ti | **Pass** (L): all checks byte-identical to the GCC-built CPU references, including identity-STMap ≡ plain downscale, the 42 Output Color preset pairs over a 33³ sweep, curved LUTs, warp + colour, async ≡ blocking, the slot ring. Native Ubuntu build (GCC 13.3 + CUDA 12.9), because the Rocky container has no GPU |
| GPU-native path in Resolve | **Active** (L): `Render path: GPU-native (CUDA)` once per instance; 125,291 `GPU-native async` enqueues and **0** full readbacks over 125,785 render calls. Kernel time (`wtimer gpu=`) median 0.05 ms at 1080p, 1.27 ms per 8160×7200 eye; render action (`mtimer total`) 0.0 ms at 1080p |
| 1080p24, Resolution Full | **Real time on the plugin side** (L): Resolve reported 24 fps (Matt); probe `dt` median 42.0 ms, p90 47 ms over 289 renders, every one enqueued on the GPU path, 0 pump drops. The CPU build managed ~15.5 fps here (#73). One earlier window under heavy ComfyUI load had `dt` p90 82 ms (Resolve skipping frames) |
| Receive over Tailscale | 24.17 fps, 0 dropped at 1920×540; 14.4 fps at 3840×1080 while the box sent 21.8 (the link, not the plugin). Colour tag `Rec.709 Gamma 2.4` on every frame |
| `page=?` | **Still empty** (L): `page=?` on all 125,785 calls, including an instance added fresh from the UI on the Color page (09:16:31, `Instance created successfully on page '?'`). So on Linux, as on macOS, Resolve 21.1 doesn't deliver `OfxImageEffectPropResolvePage` even to a freshly added effect |
| Thumbnails | 92×46, 208×117, 288×162 `thumb=1` with scale < 1, correctly flagged; they take no Stream Status path note |
| 8K stereo (8160×7200/eye) real time | **Pending.** Both eyes ran GPU-native (`8160x7200 device frame -> … UYVY enqueued`), but only briefly, at Resolution Full, before the VRAM errors. Needs ComfyUI idle |
| HDR (the ticket's "HDR + every Output Color preset") | **Moot.** HDR send (P216 + colour metadata) was removed after v1.14.2: commit `f5f88f2` "Remove HDR output (P216 + color metadata) — Standard SDK streams SDR" (PR #48), CHANGELOG `[1.15.0] - 2026-10-01`, Removed: "**HDR output** (#48). The plugin is built on the Standard NDI SDK and now streams SDR only (8-bit, Rec.709)." The CUDA path has no HDR mode to test; round 2 checks the 7 display presets (Rec.2020 G2.4 is the wide-gamut one), Passthrough and optionally LUT File instead |
| Output Color presets (`tools/ndi_color_check.py`) | **Pending** |
| STMap warp on the GPU | **Pending.** Maps generated: `/var/tmp/ndi-test-media/stmap_identity_1920x1080.exr` (expect Passthrough patches) and `stmap_mirror_4096x4096.exr` (8K, labels mirrored), from `scripts/gen_stmap_exr.py` |
| Edit page vs Color page, Standard Immersive, proxy, Render Cache on | **Pending** |
| Fallback: Resolve set to OpenCL | **Pending** (expect `CPU path: Resolve sends CPU frames …` in Stream Status) |
| Fallback: forced CUDA init failure (`NDI_OUTPUT_FORCE_CUDA_FAIL=1`) | **Pending** (expect `CPU path: CUDA init failure forced …`, with device frames read back) |
| The plugin's CUDA runtime can't reach the GPU (Resolve's can) | **Simulated, not in Resolve** (L): `test_cuda_runtime_unavailable` hides the devices with `CUDA_VISIBLE_DEVICES=-1` on the 3090 Ti. The probe reports `no CUDA-capable device is detected`, init is refused, the passthrough copy and the readback of a CUDA frame both fail, and Stream Status reads `NDI off — the plugin's CUDA runtime can't use the GPU (no CUDA-capable device is detected): update the NVIDIA driver, or set Resolve's GPU processing to OpenCL` (latched once). Before the #74 review fix it read `CPU path: no CUDA device …` while no frame was sent |

## Observation: a mono timeline after a stereo one streams "Stereo degraded"

After the 8K **stereo** timeline had played, switching to the **mono** `NDI_LINUX_TEST` with Stereo Packing still Side-by-Side gave `Stereo degraded: right eye stalled — left eye in both halves (canvas held)`: a 3840×1080 (Full) or 1920×540 (Half) canvas with the left eye twice. The right-eye instance from the stereo timeline still exists, and the process-global pairer keeps the stereo canvas. This is shared, platform-neutral code (the canvas-hold rule), not a Linux difference; the fix for the user is Stereo Packing → Mono. Recorded here because it cost a Tier 2 round trip.

## Test media and timelines

- `NDI_LINUX_8K_STEREO` in project **NDI Testing**: a stereo clip of `chart_8160x7200_L/R.png` (`scripts/gen_resolution_chart.py --width 8160 --height 7200 --eye L|R`) on an 8160×7200 timeline (custom settings), converted to stereo. Built by fuscript; the still came in at 5 s (`AppendToTimeline`'s `endFrame` is ignored for stills), so loop playback.
- `NDI_LINUX_TEST` (1920×1080 patch chart) from #73.
