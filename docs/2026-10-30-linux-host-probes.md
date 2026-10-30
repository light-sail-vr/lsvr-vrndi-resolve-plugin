# Linux host probes: render calls, colour management and Avahi (ticket #73)

**Date:** 2026-10-30 · **Host:** DaVinci Resolve Studio 21.1.0 build 17 on Ubuntu 24.04 (COMFYBOX: RTX 3090 Ti, NVIDIA 610.43.02) · **Plugin:** 1.18.0, `feature/linux-first-stream`, **CPU-only build** (`-DNDI_ENABLE_CUDA=OFF`: the plugin declares no CUDA rendering until #74) · **Receiver:** `build/ndi_verify` on the Mac, over the LAN.

The spec (docs/linux-port-spec.md, Testing) asks for the macOS host-behaviour probes to be re-run on Linux before trusting them: the Log Render Calls matrix from [2026-08-28-render-call-probe-findings.md](2026-08-28-render-call-probe-findings.md) and the colour-management probe from [2026-10-19-ofx-colour-management-probe.md](2026-10-19-ofx-colour-management-probe.md). Fact labels as in LEARNINGS.md: (L) verified on the box.

Test project **NDI Testing**:
- `NDI_LINUX_TEST`: the patch chart (`scripts/gen_patch_chart.py --width 1920 --height 1080`) as a 1920×1080, 24 fps mono timeline.
- `NDI_LINUX_STEREO`: a stereo clip (`MediaPool:CreateStereoClip`) of two 2048² resolution charts (`gen_resolution_chart.py --size 2048 --eye L/R`), scaled into a 1920×1080 stereo timeline (`Timeline:ConvertTimelineToStereo`).

The node is on the Color page, with Render Cache off.

## Summary

| Question | Linux answer | macOS (for comparison) |
|---|---|---|
| Does Resolve hand **CPU buffers** when CUDA isn't declared? | **Yes** (L). Every render took the CPU path (`Render completed`, never the GPU branch; `CPU downscale` / `CPU RGBA->UYVY` lines), and the stream is byte-correct: the Passthrough patch readout is within 1 code on all 24 patches | Same contract (CPU unless Metal is declared and enabled) |
| Second eye with Stereo 3D **Vision: Mono**? | **Left only** (L): 398 renders, all `eye=L` | Left only |
| Second eye with **Vision: Stereo**? | **Both eyes every frame, each through its own plugin instance** (L): a second `Creating instance` when Vision went Stereo, and its probe counter starts at `#000001 eye=R` | Same |
| Pairing key | **`time` (and `src`)** (L): 1,048 of 1,049 R frames had an L with the same `time` within 2 s | Same |
| Arrival order | **R first** in 1,047 of 1,048 pairs, median 27 ms ahead (−104…+27 ms) at 1920×1080/eye on the CPU path | 4096²: R first, L +32 ms avg; 8K: either eye can lead |
| Vision switched mid-playback | The right eye **doesn't render until playback restarts** (L): the first `eye=R` came at the restart, 25 s after Vision: Stereo | Not recorded on macOS |
| Page property (`OfxImageEffectPropResolvePage`) | **Not delivered**: `page=?` on all 20,966 calls | Not delivered |
| Thumbnails | 92×46 and 288×162 `thumb=1` renders, correctly flagged (L) | 184×92 / 92×46, flagged |
| `src` vs `time` | Mono still clip: `src − time = 86400` (01:00:00:00 at 24 fps); the stereo clip: `src = time` | Constant clip offset |
| OFX 1.5 colour management | `style OfxImageEffectColourManagementFull, config ofx-native-v1.5_aces-v1.3_ocio-v2.3`; `source clip 'Raw', output clip 'Raw'` (L) | Identical (case 1) |
| Does Resolve convert the node's I/O? | **No** (L): Passthrough, `--patches --color bgra`, worst channel error 1 code | No (within 1 code) |
| Is `avahi-daemon` required? | **Yes, for discovery** (L): without it `libndi` initialises and sends, but the source is never discoverable | n/a (Bonjour is built in) |

## 1. Log Render Calls: mono timeline (case 1)

Capture: [captures/2026-10-30-linux-mono-cpu-color-page.log](captures/2026-10-30-linux-mono-cpu-color-page.log) (an excerpt: the first 300 of 18,470 probe lines from 15:38:09 to 15:58:09, then every Resolution change and status line).

- 18,453 renders `eye=L dim=1920x1080 scale=1.00x1.00 thumb=0`, plus 17 thumbnails `92x46 thumb=1`. All `page=?`.
- With Resolution Full, the cadence was **`dt` median 65.2 ms** (p10 63.9 ms), i.e. ~15 fps on a 24 fps timeline. The CPU path converts on the render thread, and Resolve waits for it: at Full 1080p the CPU path can't keep up on this box. At Half and Quarter the stream ran at the timeline's 24 fps (below). The GPU-native path (#74) is the fix. The same CPU path on macOS is only the fallback behind Metal.

## 2. Log Render Calls: stereo timeline (case 2)

Capture: [captures/2026-10-30-linux-stereo-cpu-color-page.log](captures/2026-10-30-linux-stereo-cpu-color-page.log) (an excerpt: the Vision: Mono stretch and the first 400 lines after Vision: Stereo).

- 16:17:13: Log Render Calls on, Vision: Mono. Only `eye=L` renders (398).
- Vision set to Stereo during playback. Nothing changed: still left only, and Stream Status stayed `Mono, sending 960x540`. That status was accurate.
- 16:17:38: playback restarted. Resolve created the right-eye instance (`Creating instance`) and its first render was `probe #000001 eye=R`. From there both eyes rendered every frame: 1,047 L and 1,049 R over 62 s, both instances at `dt` median 55.8 ms, with one small in-eye `time` reversal per eye.
- Stream Status: `Stereo (Side-by-Side), sending 1920x540 (960x540 per eye)`. The very first stereo status, in the same second, read `sending 960x540 (480x540 per eye)`. That is a one-update artifact of shared code: the status is composed when the pairer latches Stereo, before the first packed frame is sent, so it splits the last **mono** frame's size. The next update has the packed canvas. It's cosmetic and isn't Linux-specific.

So the pairer's assumptions (process-global, keyed on `time`, reorder-tolerant) hold on Linux unchanged.

## 3. Resolution Full / Half / Quarter (Tier 2)

`ndi_verify --source "COMFYBOX (VR.NDI Sender)" --patches --color bgra` in a 4-second loop, with Stream Status from the journal:

| Resolution | Stream Status | On the wire | Measured |
|---|---|---|---|
| Full | `Mono, sending 1920x1080` | 1920×1080 | ~15.5 fps (CPU ceiling, §1) |
| Half | `Mono, sending 960x540` | 960×540 | 24.4 fps, 0 dropped |
| Quarter | `Mono, sending 480x270` | 480×270 | 24.4 fps |

Every switch, including quick toggles 2 s apart, landed on the wire within one receive window. The stream declares 30 fps (`30000/1000`) on a 24 fps timeline: that's the plugin's **Frame Rate** parameter (default 30) on every platform, not a Linux difference.

## 4. Colour-management probe (case 1, unmanaged DaVinci YRGB, Rec.709 timeline)

```
NDI Plugin: OFX colour management: style OfxImageEffectColourManagementFull, config ofx-native-v1.5_aces-v1.3_ocio-v2.3
NDI Plugin: OFX colourspace: source clip 'Raw', output clip 'Raw'
```

Identical to macOS. Passthrough readout at Full (`--color bgra`): every patch within 1 code of the chart (0, 16, 235 and 255 exact). Frames carry **no** `lsvr_color` tag: Timeline Color = Auto has no scripting answer on Linux until the timeline watcher lands (#75), so the stream is unlabelled, as designed. Cases 2–4 (RCM, ACES) weren't re-run: the plugin code that decides them is shared and platform-neutral, and Resolve's answer on Linux matched macOS on case 1.

## 5. Is `avahi-daemon` required? (Avahi, spec decision 10)

**Don't stop the daemon under a running Resolve.** The first attempt (16:03:30) segfaulted Resolve in its own control-panel discovery (`libDaVinciPanelAPI.so` → its bundled `libdns_sd`/`libavahi-client`); see LEARNINGS 2026-10-30. And D-Bus re-activates the daemon at once unless it's masked.

The method with Resolve closed: a scratch sender that loads the bundle's `libndi.so.6` through the plugin's own loader (`src/LinuxNDIRuntime.cpp`) and streams a 640×360 test pattern under a given name. The Mac ran `ndi_verify --list` and a 3 s receive for each name in a loop.

- **`AVAHI UP`** was started at 16:10:56 with the daemon running. It was discoverable and received at 30 fps.
- 16:21:52: `systemctl mask --runtime` + `stop avahi-daemon.socket avahi-daemon.service` (Matt).
- `AVAHI UP` stayed listed and receivable from the Mac only until 16:23:10, which looks like the Mac's cached mDNS record. The sender logged a direct receiver connection at 16:23:14. From 16:23:21 it was gone from discovery for the rest of the outage.
- **`AVAHI DOWN`** was started at 16:22:13 with the daemon stopped. `NDIlib_initialize: true` and `send_create: ok`, but it was **never discoverable** in ten minutes.
- 16:33:38: unmask + start.

**Result:**
- `libndi` on Linux advertises through avahi-daemon only; it has no mDNS responder of its own to fall back on.
- With the daemon down, the plugin streams normally, and a receiver that already has the address (or uses an NDI Discovery Server) can still connect, but nobody can find the source.
- The daemon is therefore **required for discovery**, but not for the plugin to load or send. A missing daemon is invisible from inside the plugin (`NDIlib_initialize` and `send_create` both succeed), so `install.sh` (#78) must check for it, and the docs say so.

An Ubuntu container without the Avahi client libraries showed the related load-time fact: the bundled `libndi.so.6` fails to `dlopen` with `libavahi-common.so.3: cannot open shared object file`, while the plugin `.ofx` still loads (`test_plugin_elf` passes there).

## 6. The NDI runtime failure path in Resolve

To test it, the bundle's `libndi.so.6` had its `NEEDED libavahi-common.so.3` renamed to a non-existent `libavahi-commoX.so.3` (same length, so the ELF stays valid).

- The plugin was listed, applied and passed frames through.
- The journal said `NDI runtime NOT LOADED - streaming disabled. Bundle: libavahi-commoX.so.3: cannot open shared object file: No such file or directory | system search: libndi.so.6: cannot open shared object file: No such file or directory`.
- Stream Status read `NDI off …` (Matt).
- Resolve's plugin cache recorded the entry with `status="0"`.

The first run of this test found every render retrying and logging the failure (273 times in 30 s). Fixed in the same branch by latching NDI off for the process. On the re-run (16:49) the reason was logged once and the following 350 renders logged no retry. The working build reinstalled afterwards streamed 960×540 at 24.19 fps with 0 drops (`--expect 960x540`, patches within 1 code).
