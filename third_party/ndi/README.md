# Vendored NDI SDK pieces (CI compile support only)

The plugin builds against the **Standard NDI SDK** (royalty-free) — never the
NDI Advanced SDK, which is a commercial license. Hosted CI has no SDK
installed, so this directory carries the minimum needed for CI to *compile and
link* the plugin without it (see `CMakeLists.txt`, "CI mode"):

- `include/` — the `Processing.NDI.*.h` headers of the NDI 6 SDK. Each file
  carries its own MIT license notice from Vizrt NDI AB ("The following MIT
  license applies to this file ONLY and not to the SDK as a whole"), which is
  what permits checking them in. No non-MIT SDK file may be added here.
  (Checked 2026-10-01: identical, apart from line endings, to the Standard NDI SDK 6.0.1 for
  Apple headers, except `utilities.h`, which adds newer audio helpers.)
- `Processing.NDI.Lib.x64.def` — the export-name list of
  `Processing.NDI.Lib.x64.dll`, restricted to the functions the headers above
  declare. CMake turns it into a stub import library so the linker can
  resolve NDI calls. The stub contains no NDI code, is never shipped, and
  never loads at runtime.

Builds on a machine with the real SDK installed (the default
`NDI_SDK_PATH`) use the SDK's own headers and import library instead; any
binary that actually streams must be built that way, and release builds must
use an SDK no more than 30 days old (NDI SDK License Agreement §2.b).

When a new SDK version lands, refresh both the headers and the .def:

```bash
python scripts/dump_ndi_exports.py "C:/Program Files/NDI/NDI 6 SDK/Bin/x64/Processing.NDI.Lib.x64.dll"
```

NDI® is a registered trademark of Vizrt NDI AB — https://ndi.video
