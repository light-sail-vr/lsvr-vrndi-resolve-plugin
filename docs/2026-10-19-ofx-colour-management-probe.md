# Timeline colour detection: OFX 1.5 colour management and Resolve's settings (issue #57 Part 3)

Status: **partly verified.** The scripting route and the plugin's handling are verified (L); what Resolve 21.1 reports through the OFX 1.5 colour-management API is **(T)** until the in-Resolve probe below is run. Fact labels as in LEARNINGS.md: (L) verified locally, (P) primary source, (T) needs testing.

## Why the plugin needs to know

An OpenFX node on the Color page runs in "Timeline Processing". The Output LUT and the RCM output colour space conversion (or the ACES RRT/ODT) run after every node (Resolve 21 manual, processing-order chart, p. 3407) (P). So the node, and therefore the NDI stream, sees the **timeline** colour space. A display-to-display preset in the Output Color group needs that space as its input; the `lsvr_color` frame tag needs it to label a Passthrough stream.

`Timeline Color = Auto` resolves, in order:

1. the source clip's `kOfxImageClipPropColourspace` (OFX 1.5), when Resolve reports one;
2. otherwise the timeline's colour settings, read through Resolve's scripting API by the timeline-watch helper;
3. otherwise unknown. A display-space NDI Output then assumes Rec.709 Gamma 2.4, and Stream Status says so.

## 1. OFX 1.5 colour management

**What exists** (P): Resolve 21's release notes list "OpenFX 1.5 color management APIs for colorspace aware effects". Resolve's SDK ships `ofxColour.h` and the native config header `ofx-native-v1.5_aces-v1.3_ocio-v2.3.h` under `/Library/Application Support/Blackmagic Design/DaVinci Resolve/Developer/OpenFX/OpenFX-1.4/include`. Both are now vendored in `openfx/include/` (BSD-3, same project). The config's display spaces include `rec1886_rec709_display`, `srgb_display`, `displayp3_display`, `p3d65_display` (gamma 2.6), `rec1886_rec2020_display` and `davinci_intermediate_widegamut`. It has no P3-D65 gamma 2.2.

**What the plugin does** (L, `src/NDIOutputPlugin.cpp`):

- `describe` sets `kOfxImageEffectPropColourManagementStyle = kOfxImageEffectColourManagementFull` and `kOfxImageEffectPropColourManagementAvailableConfigs = ofx-native-v1.5_aces-v1.3_ocio-v2.3`.
- It sets **no** preferred colourspaces, and does not trap `kOfxImageEffectActionGetOutputColourspace`. The default reply means "the output clip has the source clip's colourspace". Per the API, the host then has no reason to convert the node's input or output.
- `createInstance` logs `OFX colour management: style <…>, config <…>`, i.e. what the host negotiated for the instance.
- `render` reads `kOfxImageClipPropColourspace` from the source clip. On every change it logs `OFX colourspace: source clip '<…>', output clip '<…>'` and re-resolves Auto. `src/TimelineColor.h` (`fromOfxColourspace`) maps the identifiers: display spaces to presets; ACES, log, linear and `davinci_intermediate_widegamut` to scene-referred; PQ/HLG to HDR; anything else, including the generic `ofx_display_sdr`, to unknown. An unknown OFX answer still lets the scripting answer in.

**Verified without Resolve** (L): a scratch OFX host loading the built plugin confirmed the style is declared at describe. With a host-reported `rec1886_rec709_display` on the source clip, Passthrough reads `color: passthrough (Rec.709 G2.4)` and every frame carries the Rec.709 G2.4 tag. Node output stayed bit-identical to input on all 210 renders in every mode.

### The in-Resolve probe (T, owed)

Run once on Resolve 21.1 with this build installed (`sudo make install`, full Resolve restart). Use a short clip with the patch chart (`scripts/gen_patch_chart.py --width 1920 --height 1080`) and the NDI Output node on the Color page. Turn **Log Render Calls** on, and run `./scripts/monitor_ndi_logs.sh` filtered to `OFX colour`.

| # | Project colour science | Timeline / output | Record |
|---|---|---|---|
| 1 | DaVinci YRGB (unmanaged) | Rec.709 Gamma 2.4 / same | `style …, config …` and `OFX colourspace: source clip '…'` |
| 2 | DaVinci YRGB Color Managed, SDR preset | Rec.709 Gamma 2.4 / same | same |
| 3 | DaVinci YRGB Color Managed | DaVinci WG/Intermediate / Rec.709 Gamma 2.4 | same |
| 4 | ACEScct | ACEScct / Rec.709 ODT | same |

For each case, also confirm **declaring support doesn't make Resolve convert the node's input or output**:

- Stream the chart with NDI Output = Passthrough and read it with `./build/ndi_verify --source "<name>" --patches --color bgra`. In case 1 every patch must be within ±2 codes of the source values, and on every case the readout must be the same with the node enabled and disabled.
- Compare the viewer with the node enabled vs. bypassed (they must match).

Write the results into this section, and an entry into LEARNINGS.md. If Resolve reports nothing (style and colourspace both "(not set)"), Auto runs entirely on step 2 below, which is already verified. If Resolve does convert, remove the two `propSetString` lines in `describe`; nothing else depends on them.

## 2. Resolve's timeline settings via scripting (verified)

The watcher helper (`src/ndi_timeline_watch.lua`, Python fallback `src/ndi_timeline_watch.py`) reads `Timeline:GetSetting(k)` for `colorScienceMode`, `separateColorSpaceAndGamma`, `colorSpaceTimeline`, `colorSpaceTimelineGamma`, `colorSpaceOutput` and `colorSpaceOutputGamma`. It falls back to the project's value when the timeline's is empty. It sends them every ~2 s, only on change, as one protocol line:

```
@color<TAB>colorScienceMode<TAB>separate<TAB>timeline<TAB>timelineGamma<TAB>output<TAB>outputGamma
```

`'!'` lines are still status messages; every other line is still the clip path. The plugin starts the watcher whenever Timeline Color is Auto and NDI Output isn't LUT File. Passthrough needs the answer to label the stream, a display space needs it as the conversion's input. LUT File doesn't need it: the LUT defines the conversion, and LUT Produces sets the label. Verified (L) with a live Resolve 21.1 on its default unmanaged project: a default node (Passthrough, Auto) streamed `color: passthrough (Rec.709 G2.4)` with the Rec.709 G2.4 tag on every frame.

**Values Resolve 21.1 returns** (L, probed 2026-10-19 on a scratch project through the scripting API, then deleted):

- `colorScienceMode`: `davinciYRGB`, `davinciYRGBColorManaged`, `davinciYRGBColorManagedv2`, `acescct`, `acescc`. `rcmPresetMode` accepts only `SDR Rec.709` and `Custom`.
- Combined names (`separateColorSpaceAndGamma = 0`): `Rec.709 Gamma 2.4`, `Rec.709 Gamma 2.2`, `Rec.709-A`, `sRGB`, `P3-D65` (gamma 2.6), `P3-DCI`, `Rec.2020 Gamma 2.4`, `Rec.2020 (Scene)`, `Rec.709 (Scene)` (the default), `DaVinci WG/Intermediate`, `ACEScct`, `Linear`, `Rec.2100 ST2084`. The setter rejects `P3-D65 Gamma 2.2` and `Display P3`.
- Split mode (`= 1`): gamuts `Rec.709`, `P3-D65`, `Rec.2020`, `sRGB`, `P3-DCI`; gammas `Gamma 2.4`, `Gamma 2.2`, `Gamma 2.6`, `sRGB`, `Rec.709`, `DaVinci Intermediate`, `Linear`, `ST2084`, `ST2084 1000 nit`, `Rec.2100 HLG`.

**Quirks the parser encodes** (`src/TimelineColor.h`, unit-tested in `tests/test_timeline_color.cpp`):

- In combined mode, `colorSpaceTimelineGamma` still returns the last split-mode value (observed: `ST2084 1000 nit` next to a `DaVinci WG/Intermediate` timeline). The gamma field is ignored unless `separateColorSpaceAndGamma` is `1`.
- Under `acescct`/`acescc` the gamut/gamma fields are stale RCM values. The timeline is ACEScct/ACEScc regardless.
- Unmanaged YRGB labels its timeline `Rec.709 (Scene)` by default, but has no output conversion: the pixels are graded for a BT.1886 display. So `(Scene)` there resolves to Rec.709 (or Rec.2020) Gamma 2.4. Under RCM the same name really is scene-referred and triggers the warning.

**Verified live** (L): both helpers run standalone against a running Resolve 21.1 and print `@color⇥davinciYRGB⇥0⇥Rec.709 (Scene)⇥⇥Rec.709 (Scene)⇥`. Through the scratch OFX host with Resolve running, Auto on that unmanaged project re-resolved from "unknown, assuming Rec.709 G2.4" to `color: Rec.709 G2.4 → sRGB [timeline colour from Resolve timeline settings (davinciYRGB)]` within ~2 s. On an RCM `DaVinci WG/Intermediate` → `Rec.709 Gamma 2.4` project it showed `WARNING: timeline is DaVinci WG/Intermediate (scene-referred/log), use LUT File`. External scripting on and off (owed with the in-Resolve run): with it off the helper prints `!scriptapp('Resolve') returned nothing`, Auto stays unknown, and presets assume Rec.709 G2.4 with the warning.

## 3. What NDI can and can't carry (from the 2026-10-19 audit)

`ndi_color_info` (SDK 6.0 docs p. 39) has only bt_601/709/2020/2100 primaries and bt_601/709/2020/HLG/PQ transfers: no P3-D65, no gamma 2.2/2.6. The plugin never sends it. Every frame whose space is known carries `<lsvr_color primaries="…" transfer="…" matrix="bt_709" range="limited"/>` in `p_metadata` instead. Verified (L) through the Standard SDK 6.0.1 with SpeedHQ: `ndi_verify` reads the tag on every frame (`--expect-color` asserts it). Making VR.NDI honour it is a separate ticket in the Quest repo.
