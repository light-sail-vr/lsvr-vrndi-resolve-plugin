# NDI Output Plugin for DaVinci Resolve

A modern OpenFX plugin that sends video frames from DaVinci Resolve to NDI (Network Device Interface) for streaming over the network.

> **Download**: grab the signed installer from the [latest release](https://github.com/lightsailvr/ResolveOFX_NDIOutput/releases/latest) — no SDK or other software needed.  
> **Build & Install from source**: [BUILD.md](BUILD.md) · **Dev workflow & testing loop**: [LEARNINGS.md](LEARNINGS.md)

## Features

- **Modern OFX Implementation**: Uses C API directly for maximum compatibility with DaVinci Resolve 20+
- **Stereo Timelines**: On a native stereo timeline (Stereo 3D palette at **Vision: Stereo**), the plugin pairs the left/right eye renders per frame and streams **one packed frame** — Side-by-Side or Top-Bottom (Stereo group) — for VR receivers such as VR.NDI on Quest. Choose **Mono** instead to stream only the left eye as a plain single frame for monoscopic 360 receivers; right-eye renders are skipped. The packed canvas is locked for the stream's lifetime: brief eye stalls re-send the last packed frame, and a sustained stall degrades to the flowing eye duplicated into both halves (labeled in the **Stream Status** parameter) — the frame dimensions never change mid-stream, so the geometry never pops in-headset. Mono timelines are unaffected. **Stream Status** also shows the frame size on the wire ("sending 7200x3600 (3600x3600 per eye)"): the frame Resolve feeds — or the map, in the Equirect modes — divided by **Resolution**. If that number is what you expect, anything softer in a receiver is on the receiving side.
- **Real-time Streaming**: Low-latency video streaming over network
- **Pass-through Design**: Maintains original video quality while streaming
- **User-friendly Controls**: Easy-to-use parameters for configuration

## Requirements

- macOS 13+ (Ventura or later), Apple Silicon or Intel — or 64-bit Windows 10/11 (NVIDIA GPU for the GPU-native path; the CPU path is the automatic fallback)
- DaVinci Resolve 17+ (tested with DaVinci Resolve 20)
- Building from source additionally needs the NDI SDK 6.x ([download](https://ndi.video/for-developers/)) — see [BUILD.md](BUILD.md) for setup. The installers ship the NDI runtime inside the plugin, so end users need nothing else

## Installation

Grab the installer for your platform from the [latest release](https://github.com/lightsailvr/ResolveOFX_NDIOutput/releases/latest), then **restart DaVinci Resolve** — OFX plugins are only scanned at startup — and find the plugin on the Color page under **OpenFX → LSVR → NDIOutput**.

**macOS**: download `NDIOutput-<version>-macOS.pkg` and double-click it (signed and notarized; installs to `/Library/OFX/Plugins`).

**Windows** (v1.14.1 and later): download `NDIOutput-<version>-Windows-x64.exe` and run it (quit Resolve first — the installer refuses to replace a loaded plugin; installs to `C:\Program Files\Common Files\OFX\Plugins`). x64 only, not Windows on ARM. Uninstall from **Add or Remove Programs**; `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART` installs unattended for fleet deployment (it exits non-zero without installing if Resolve is running).

> **SmartScreen**: the Windows installer is **not code-signed**, so Windows shows *"Windows protected your PC"* the first time you run it. Click **More info**, then **Run anyway** — this is expected, not a malware verdict. To verify the download without relying on a signature, compare its SHA-256 against `SHA256SUMS-Windows.txt` on the release page: `Get-FileHash .\NDIOutput-<version>-Windows-x64.exe -Algorithm SHA256`.

The first NDI send raises a firewall prompt for Resolve on Windows — allow it on private networks, or the source is discoverable but its video is unreachable from other machines.

**From source**: full instructions (prerequisites, build, install, verification, troubleshooting, Windows status) live in **[BUILD.md](BUILD.md)**. The short version for macOS:

```bash
make dev            # build
sudo make install   # install to /Library/OFX/Plugins
```

## Usage

1. **Add the Effect**: In DaVinci Resolve, go to the Color page and add "NDI Output" from the LSVR category in the OpenFX panel.

2. **Configure Parameters**:
   - **NDI Source Name**: Set the name that will appear on the network (default: "DaVinci Resolve NDI Output")
   - **Enable NDI Output**: Toggle to start/stop streaming (default: enabled)
   - **Frame Rate**: Set the output frame rate (default: 30 fps)
   - **Resolution**: Stream at Full, Half, or Quarter of the incoming frame size (default: Half)

3. **Receive the Stream**: Use any NDI-compatible receiver (NDI Video Monitor, OBS Studio, etc.) to receive the stream on the network.

## Technical Details

### Architecture

- **Modern OFX C API**: Direct use of OpenFX C API for maximum host compatibility
- **Efficient Processing**: Minimal overhead pass-through design

### Build System

The project uses a modern, streamlined build system:

- **Modern C API**: Direct use of OpenFX C API for maximum compatibility
- **Automatic Versioning**: Semantic versioning with automatic patch increment
- **Clean Dependencies**: No legacy wrapper dependencies

## Development

- **Branching:** `master` is stable; all work happens on `dev` (or `feature/*` off `dev`) and merges to `master` by PR once validated. See [LEARNINGS.md](LEARNINGS.md) §1.
- **Building:** `make dev` / `make clean` — details in [BUILD.md](BUILD.md). No make target changes the version.
- **Versioning:** semantic versioning; bump only via `./scripts/increment_version.sh` (patch) or `./scripts/set_version.sh X.Y.Z`, which keep `VERSION` and the source `#define`s in sync.
- **Testing:** every change goes through the tiered testing loop in [LEARNINGS.md](LEARNINGS.md) §2 (compile → load in Resolve → verify stream in an NDI receiver → feature-specific checks) before it's considered done.

### Project Structure

```
├── src/
│   ├── NDIOutputPlugin.cpp          # Main plugin implementation (modern C API)
│   ├── NDIOutputPlugin.h            # Header file
│   └── LSVR.NDIOutput.png           # Plugin icon
├── scripts/
│   ├── increment_version.sh         # Auto-increment patch version
│   └── set_version.sh               # Manual version management
├── openfx/                          # OpenFX SDK
├── VERSION                          # Current semantic version
├── CHANGELOG.md                     # Version history
├── Makefile                         # Build configuration
├── Info.plist                       # Plugin metadata
└── README.md                        # This file
```

## Troubleshooting

### Plugin Not Loading

1. **Verify Plugin Installation**:
   ```bash
   ls -la "/Library/OFX/Plugins/NDIOutput.ofx.bundle"
   ```

2. **Check Library Dependencies**:
   ```bash
   otool -L "/Library/OFX/Plugins/NDIOutput.ofx.bundle/Contents/macOS/NDIOutput.ofx"
   ```

### No NDI Source Visible

1. **Check Plugin Parameters**: Ensure "Enable NDI Output" is checked
2. **Check Stream Status** (Stereo group): "No NDI sender — name unavailable" means the source name is already advertised on this machine (another app, or a leaked registration from a crashed session — `dns-sd -B _ndi._tcp local.` shows it). Change the NDI Source Name, or quit the process holding it
3. **Verify Network**: Ensure devices are on the same network
4. **Check NDI Tools**: Use NDI Video Monitor to verify source availability
5. **Restart DaVinci Resolve**: Sometimes required after parameter changes

## Version History

The current version is in [`VERSION`](VERSION). Per-release details live in [CHANGELOG.md](CHANGELOG.md) and the `RELEASE_NOTES` files under [`Releases/`](Releases/). Notable fixes and the rules they taught us are logged in [LEARNINGS.md](LEARNINGS.md) §3.

## License

This project is licensed under the BSD 3-Clause License - see the LICENSE file for details.

## Contributing

1. Branch off `dev` (never work on `master`)
2. Make your changes; build with `make dev`
3. Validate through the testing loop in [LEARNINGS.md](LEARNINGS.md) §2
4. If you fixed a bug, add an entry to the log in [LEARNINGS.md](LEARNINGS.md) §3
5. Open a pull request into `master`

## Support

For issues and questions:
- Check the troubleshooting section above
- Review DaVinci Resolve console output for error messages
- Verify NDI SDK installation and network configuration

## Acknowledgments

NDI® is a registered trademark of Vizrt NDI AB — learn more at [ndi.video](https://ndi.video/). Receive the stream with the free [NDI Tools](https://ndi.video/tools/). Third-party license notices ship inside the plugin at `Contents/Resources/libndi_licenses.txt`.
