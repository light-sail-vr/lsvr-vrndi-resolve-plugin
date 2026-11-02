# Blackmagic RAW API — Linux (ticket #77)

What the Linux Camera Metadata reader (`src/BRAWImmersiveReader.cpp`, Linux
branch) compiles against. Nothing Blackmagic is linked or bundled: at runtime
the plugin `dlopen`s the host's own `libBlackmagicRawAPI.so` (inside Resolve,
`/opt/resolve/libs/libBlackmagicRawAPI.so`, located from `/proc/self/exe`)
through its own dispatch, `src/LinuxBRAWRuntime.h`.

| File | Provenance | License |
|---|---|---|
| `LinuxCOM.h` | Blackmagic Design's `LinuxCOM.h` (© 2009), verbatim. **This is the DeckLink SDK's copy, not one from the Blackmagic RAW SDK:** it comes from the DeckLink SDK's `Linux/include/` as redistributed in the NDI SDK for Linux (`examples/C++/NDIlib_Send_BMD/BMDSDK/Linux/include/LinuxCOM.h`). The minimal COM types (`HRESULT`, `REFIID`, `IUnknown`, `S_OK`, …) Blackmagic's Linux SDKs share. | Blackmagic Boost-style license, notice intact |
| `BlackmagicRawAPI.h` | **Derived** from the vendored Blackmagic RAW SDK 5.1 macOS header (`../BlackmagicRawAPI.h`): the CoreFoundation / `CFPlugInCOM.h` includes are replaced by `LinuxCOM.h`, and every `CFStringRef` by `const char*` (UTF-8). Nothing else differs; the derivation note sits under the license block. | Same Blackmagic Boost-style license (it permits derivative works), notice intact |

## Why derived, and why no SDK dispatch shim

The spec (decision 19) assumed the Linux SDK shares the macOS header. It does
not: the macOS header includes CoreFoundation and uses `CFStringRef` for every
string, so it cannot compile on Linux. The Linux Blackmagic RAW SDK (a
registration-form download from Blackmagic, not on the dev machines) ships its
own header, `LinuxCOM.h` and a `dlopen`-based `BlackmagicRawAPIDispatch.cpp`.

- **Header:** derived from the licensed macOS header, as above, on the
  assumption that the Linux API has the same interfaces, IIDs, enums and
  vtable order, so that only the string and COM base types change.
  - **Verified** against Resolve 21.1's `libBlackmagicRawAPI.so` on the box:
    only the calls the reader makes. These are
    `IBlackmagicRawFactory::CreateCodec`, `IBlackmagicRaw::OpenClip`,
    `QueryInterface` for `IBlackmagicRawClipImmersiveVideo`,
    `GetImmersiveAttribute` and `Release`, plus the three entry points below.
    The calibration JSON they return is byte-identical to the macOS reader's
    on the same clip (`test_braw_reader` with `NDI_TEST_BRAW_CLIP` and
    `NDI_TEST_BRAW_DUMP`; LEARNINGS 2026-11-01).
  - **Unverified:** the rest of the derived header (every other interface,
    method slot, struct layout and enum value) has never been checked against
    Blackmagic's Linux SDK. Don't call anything else through it until the
    Linux SDK's own header replaces it (below).
  - **Leftover macOS-only enums:** the Metal values
    (`blackmagicRawResourceTypeBufferMetal`, `blackmagicRawPipelineMetal`)
    are kept as-is from the macOS header. They are meaningless on Linux and
    the reader never uses them; the Linux SDK's header may omit them or add
    CUDA/OpenCL values the macOS header lacks.
- **Dispatch:** the plugin's own (`src/LinuxBRAWRuntime.h/.cpp`), not the SDK
  shim. It binds the three entry points the reader uses
  (`CreateBlackmagicRawFactoryInstance`, `VariantInit`, `VariantClear`) by
  full path, reports `dlerror` for every path tried, and its failure handling
  is unit-tested through injected loader ops (`test_linux_braw_runtime`).
  The SDK shim exports global `VariantInit`-style definitions and swallows the
  loader's reason. The dispatch is plain `dlopen`/`dlsym` written for this
  plugin, so there is no licensing question.

**To update:** when the Linux Blackmagic RAW SDK is at hand, replace
`BlackmagicRawAPI.h` (and `LinuxCOM.h`) with the copies from its
`Linux/Include/`, then rebuild and run `test_braw_reader` with
`NDI_TEST_BRAW_CLIP` (and `NDI_TEST_BRAW_DUMP`, diffed against the macOS
dump) on the box. The reader uses only `const char*` paths/attributes, the
methods listed above and the three entry points, so no code change should
follow.
