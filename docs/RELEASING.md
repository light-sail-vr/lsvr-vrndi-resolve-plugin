# Releasing

How a version goes from `dev` to a public GitHub release with **macOS, Windows and Linux parity**. There is one `VERSION`, one `CHANGELOG.md` section and one release event, and each platform's artifacts are built on its own machine. Follow this guide whenever something merges into `master` or Matt asks to strike a release. Build and install mechanics live in [BUILD.md](../BUILD.md); this guide covers the order, the hand-offs and the human gates.

## Machines and who does what

| Machine | Builds | Holds | Human-only work there |
|---|---|---|---|
| **Mac** | universal macOS pkg + zip | Developer ID Application + Installer certs, `NDI_NOTARY` notarytool profile, Standard NDI SDK at `/Library/NDI SDK for Apple` | Tier 1–2: install the pkg, restart Resolve, check the stream in NDI Video Monitor |
| **Windows workstation** | Inno Setup `.exe` + zip | VS2022, CUDA 12.9, Standard NDI 6 SDK at `C:\Program Files\NDI\NDI 6 SDK` | Tier 1–2: run the `.exe`, restart Resolve, check the stream in NDI Studio Monitor (ideally from a second machine) |
| **Linux box** (COMFYBOX, `ssh matt@100.80.25.1`) | `.tar.gz` + `SHA256SUMS-Linux.txt`, in the Rocky 8 container | Docker image `ndioutput-build:rocky8`, Standard NDI SDK for Linux at `~/sdks/NDI SDK for Linux` | `sudo ./install.sh`, restart Resolve, press play. **Driven over SSH from the Mac session**: nobody runs an agent on the box |

All three machines build against the **Standard (royalty-free) NDI SDK**. A Windows or Linux build without the SDK is a stub: it carries a `-STUB` name and must never be released (`publish_github_release.sh` refuses any `-STUB` file).

The **Mac publishes the release** (it creates the tag, the release and the notes). The Mac session builds the **Linux** artifacts on the Linux box over SSH and copies them into `dist/vX.Y.Z/` before publishing, so `publish_github_release.sh` attaches them with the macOS files (step 4a). The **workstation then uploads** its three files to that release and adds the Windows notes section.

## Human gates

The agent stops and waits for Matt at each of these. Nothing gets past one without his explicit reply.

In the order they come up:

1. **Strike the release.** Only Matt starts one.
2. **Merge the release PR** (`dev` → `master`).
3. **Mac Tier 1–2** on the pkg-installed plugin, before publishing (step 4).
4. **Linux Tier 1–2** on the tarball-installed plugin, before publishing (step 4a). Matt runs `sudo ./install.sh`, restarts Resolve and presses play at the box; the agent can't use sudo there.
5. **Publish.** The agent confirms with Matt right before anything goes public: before `publish_github_release.sh --publish`, or, for the first Linux release (step 4a), before running `publish_github_release.sh` for the draft (it pushes the tag) and again before `gh release edit vX.Y.Z --draft=false`.
6. **Windows Tier 1–2** on the installed `.exe`, before uploading (step 5).

Other human-only steps when needed: signing setup on a new Mac (BUILD.md → "One-time signing setup"; Matt creates the certs in Xcode and runs `xcrun notarytool store-credentials NDI_NOTARY --apple-id <apple-id> --team-id 647FZYVR5Q` himself), and installing either NDI SDK.

## Steps

### 1. Parity check (agent, either machine)

List every PR merged into `dev` since the last tag (`git log --merges --oneline v<last>..dev`). For each one, record whether Tier 1–2 passed on macOS, on Windows and on Linux, or whether the change doesn't touch that platform. **Done when** every PR has a status for all three platforms. A platform may lag only if the CHANGELOG entry says so in words (CLAUDE.md allows one platform lagging, stated in the notes). Report the table to Matt.

### 2. Release prep PR (agent)

On `feature/release-X.Y.Z`, branched off `dev`:
- Bump the version with `./scripts/increment_version.sh` for a patch, or `./scripts/set_version.sh X.Y.Z`. They update `VERSION`, the `#define`s and `Info.plist`.
- Add the `## [X.Y.Z] - YYYY-MM-DD` section to `CHANGELOG.md`. The publish script turns it into the release notes, so write it for end users and include any lagging-platform note from step 1.
- Run `make dev && make test` on the Mac (plus `scripts/linux_remote.sh sync` and `build` for the Linux box, or the CMake build plus `ctest` on Windows).

Open a PR into `dev`. **Done when** the CI checks (linux.yml and windows.yml) are green and the PR is merged.

### 3. Release PR (agent opens, Matt merges)

`gh pr create --base master --head dev --title "Release vX.Y.Z — <summary>"`. In the body: what's in the release, the parity table from step 1, and the remaining steps of this guide. **Done when** Matt has merged it.

### 4. macOS build and publish (Mac)

```bash
git checkout master && git pull && cat VERSION     # must print X.Y.Z
./scripts/package_release.sh                       # build, sign, notarize, staple → dist/vX.Y.Z/
```
**Done when** the script ends with a Gatekeeper `accepted / source=Notarized Developer ID` and `(cd dist/vX.Y.Z && shasum -a 256 -c SHA256SUMS.txt)` passes. Run it from inside the directory: the file lists bare file names, so a check from the repo root reports every file missing. Then the **Mac Tier 1–2** gate: Matt installs `dist/vX.Y.Z/NDIOutput-X.Y.Z-macOS.pkg` and confirms the stream. Then **step 4a** (Linux build and the Linux Tier 1–2 gate), unless Linux lags this release. Then the **publish** gate, then:
```bash
./scripts/publish_github_release.sh --publish
```
With no Windows files in `dist/vX.Y.Z/` (the normal path: the workstation adds them in step 5) it reports "releasing macOS only", or "No Windows artifacts" and then "Linux artifacts found" when the Linux pair is there. For the **first** Linux release, run it without `--publish` instead (step 4a). **Done when** `gh release view vX.Y.Z` shows the pkg, zip and `SHA256SUMS.txt`, plus the Linux tarball and `SHA256SUMS-Linux.txt` unless Linux lags this release, and the release is not a draft.

### 4a. Linux build and Tier 1–2 (Mac session, over SSH, before publishing)

Run this between the Mac Tier 1–2 gate and the publish gate in step 4, from the same `master` checkout on the Mac. Everything happens over SSH; only the install (sudo), the Resolve restart and pressing play need Matt at the box. Build and packaging details: BUILD.md → Linux → "Release packaging".

1. **SDK freshness.** The NDI SDK for Linux on the box must be no more than 30 days old (spec decision 11); `package_linux_release.sh` refuses an older one and names its date. If it does, Matt downloads the current Standard SDK for Linux from [ndi.video](https://ndi.video/for-developers/) and unpacks it over `~/sdks/NDI SDK for Linux` before you go on. Never ship with `--allow-old-sdk`.
2. **Build and package** in a release-only folder on the box, never a dev checkout:
   ```bash
   git status --porcelain     # must print nothing: the synced folder has no .git, so the Mac checkout is what's checked
   NDI_LINUX_DIR=ndioutput-release scripts/linux_remote.sh sync
   ssh matt@100.80.25.1 'cd ~/ndioutput-release && cat VERSION && scripts/package_linux_release.sh'
   scp 'matt@100.80.25.1:ndioutput-release/dist/vX.Y.Z/*' dist/vX.Y.Z/
   (cd dist/vX.Y.Z && shasum -a 256 -c SHA256SUMS-Linux.txt)
   ```
   A release build always includes the CUDA GPU-native path the release bar requires (spec decision 3). Only if Matt explicitly approves a CPU-only Linux release, pass `--cpu-only`: the tarball is then named `NDIOutput-X.Y.Z-linux-x86_64-cpu-only.tar.gz` and the release notes say CPU-only. **Done when** the script ends with `built: …/NDIOutput-X.Y.Z-linux-x86_64.tar.gz` (or `…-cpu-only.tar.gz`) (it has run `ctest` in the Rocky 8 container, refused a stub and run `test_plugin_elf` on the staged tree) and the checksum check passes on the Mac.
3. **Linux Tier 1–2 gate**, as numbered steps for Matt: quit Resolve fully; on the box, `mkdir -p /tmp/ndi-rel && tar -xzf ~/ndioutput-release/dist/vX.Y.Z/NDIOutput-X.Y.Z-linux-x86_64.tar.gz -C /tmp/ndi-rel && cd /tmp/ndi-rel/NDIOutput-X.Y.Z-linux-x86_64 && sudo ./install.sh`; start Resolve; open project **NDI Testing** → timeline `NDI_LINUX_TEST`, Color page, VR.NDI Sender on the clip; press play. Then on the Mac: `./build/ndi_verify --source "COMFYBOX (VR.NDI Sender)" --duration 5`. `ldconfig -p | grep ndi` on the box must print nothing (no NDI SDK on the loader path: the bundled runtime is what's under test).

**Done when** `dist/vX.Y.Z/` holds the tarball and `SHA256SUMS-Linux.txt`, and the stream verified. If Linux lags this release, leave both files out (`publish_github_release.sh` then releases without Linux) and say so in the CHANGELOG section; once its Tier 1–2 passes, confirm with Matt (as at the publish gate), then upload with `gh release upload vX.Y.Z <tarball> <sums>` and insert the `### Install — Linux` section before the notes' final `---` footer. Copy it from the Linux block of `scripts/publish_github_release.sh` (its heredocs, with the tarball name filled in and the `LNX_GPU` sentence as its **GPU:** line).

**The first Linux release** (spec decision 3) also needs the **release-shaped test** before it goes public. It runs under the publish gate: **the agent confirms with Matt before running `publish_github_release.sh`** (even as a draft it pushes the tag) **and again before publishing the draft**, exactly as for `--publish`.
1. After Matt's go-ahead, run step 4's publish **without** `--publish`, so the release is a draft.
2. Download the Linux tarball from the draft (`gh release download vX.Y.Z --pattern '*linux*' --dir /tmp/rel` on the Mac, then `scp` it to the box) and repeat the Tier 1–2 steps above from that file; then Matt runs `sudo ./uninstall.sh` and the agent confirms `/usr/OFX/Plugins/NDIOutput.ofx.bundle` is gone.
3. Report the result to Matt and **ask before publishing**. Only on his explicit go-ahead: `gh release edit vX.Y.Z --draft=false`.
4. Afterwards reinstall the current dev build on the box (`linux_remote.sh install`).

### 5. Windows build and upload (workstation)

Build from the tag so every platform ships the same commit:
```powershell
git fetch --tags; git checkout vX.Y.Z; Get-Content VERSION    # must print X.Y.Z
```
Build, test and stage following BUILD.md → Windows → "Build (Tier 0)" (CMake configure, build, `ctest`, `cmake --install … --prefix stage`). The configure output must say `NDI: using installed SDK`; anything else means a stub build, so stop. Then:
```powershell
powershell -ExecutionPolicy Bypass -File .\scripts\package_windows_release.ps1
```
Then the **Windows Tier 1–2** gate: Matt installs `dist\vX.Y.Z\NDIOutput-X.Y.Z-Windows-x64.exe` and confirms the stream. Then upload and patch the notes:
```powershell
gh release upload vX.Y.Z dist\vX.Y.Z\NDIOutput-X.Y.Z-Windows-x64.exe dist\vX.Y.Z\NDIOutput-X.Y.Z-Windows-x64.zip dist\vX.Y.Z\SHA256SUMS-Windows.txt
gh release view vX.Y.Z --json body -q .body > notes.md
```
In `notes.md`, insert the `### Install — Windows` section immediately before the final `---` / NDI® trademark footer. Copy it verbatim from the Windows heredoc in `scripts/publish_github_release.sh`; that script is the single source of the wording. If the CHANGELOG section says Windows lags (for example "macOS only" or "macOS and Linux only"), replace that line with the Windows status. Then run `gh release edit vX.Y.Z --notes-file notes.md`.

**Done when** the release lists six assets (macOS pkg, zip and SHA256SUMS; Windows exe, zip and SHA256SUMS-Windows), plus the Linux tarball and `SHA256SUMS-Linux.txt` unless Linux lags this release, and the notes have an Install section per platform shipped.

### 6. Close out (agent, either machine)

Check the repo homepage README (`gh api repos/light-sail-vr/lsvr-vrndi-resolve-plugin/readme`) matches `master`. Confirm `git log --no-merges origin/dev..origin/master` prints nothing. Release merge commits leave `master` "ahead" of `dev` with identical content, which is normal. A non-merge commit in that list landed on `master` outside a release PR; bring it back into `dev` by PR. Append a LEARNINGS.md entry for anything that went wrong. **Done when** Matt has a one-paragraph summary with the release URL.

## Hand-off prompts

Paste these on the other machine, filling in the version. Each one is self-contained.

**Mac, after Matt merges the release PR:**
> Strike release vX.Y.Z, macOS and Linux halves. Follow docs/RELEASING.md step 4 on this Mac: pull `master`, check that VERSION is X.Y.Z, run `package_release.sh`, then stop for my Tier 1–2 check of the pkg. Then step 4a: build the Linux tarball on the Linux box over SSH and give me numbered steps for its Tier 1–2 at the box. Ask me before you publish. After publishing, give me the Windows hand-off prompt from docs/RELEASING.md with the version filled in.

**Windows workstation, after the Mac has published:**
> Release vX.Y.Z is live with the macOS artifacts (and the Linux tarball, unless its notes say Linux lags) but no Windows ones yet. Follow docs/RELEASING.md step 5 on this workstation: check out tag vX.Y.Z, build against the Standard NDI 6 SDK (stop if it stub-links), package with `package_windows_release.ps1`, stop for my Tier 1–2 check of the installer, then upload the three files and add the Windows install section to the release notes. Report the final asset list.

**Cross-platform change, after a feature PR merges into `dev` on one machine:**
> PR #N merged into dev from the [Mac|workstation|Linux work on the Mac]: <one line on what it changes>. On this machine (for Linux: the Mac session, driving the Linux box with `scripts/linux_remote.sh`): pull dev, build and run the tests per BUILD.md, and tell me what each other platform needs: nothing (no platform code touched), a Tier 1–2 check by me, or a port, which goes on its own `feature/` (`feature/win-`, `feature/linux-`) branch off dev. Record the result so docs/RELEASING.md step 1 can find it.

## Gotchas

- **Repo URL:** the repo is `light-sail-vr/lsvr-vrndi-resolve-plugin`. The old `lightsailvr/ResolveOFX_NDIOutput` redirects for git, but `gh pr create` resolves the wrong repo through the redirect. Check `git remote -v` on a machine that hasn't been updated.
- **New Mac:** the certs and the notary profile don't migrate. `package_release.sh`'s preflight names whatever is missing; Matt redoes the setup above. The Team ID is `647FZYVR5Q`.
- **`make` and link flags:** after changing `LDFLAGS`/`CXXFLAGS`, force a relink. `make` doesn't track the Makefile (LEARNINGS 2026-10-01).
- **Plugin missing in Resolve after an install:** with Resolve closed, delete `OFXPluginCacheV2.xml` (path per platform in CLAUDE.md) and relaunch.
- **Verify a release zip with `ditto -x -k`**, never `unzip` (LEARNINGS 2026-09-01).
- **Linux checksums are `SHA256SUMS-Linux.txt`**, not `SHA256SUMS.txt`: the macOS file of that name sits in the same `dist/vX.Y.Z/` folder and on the same release.
- **Never package Linux on the Ubuntu host toolchain.** `package_linux_release.sh` builds in the Rocky 8 container; a host build links `GLIBC_2.38` symbols and won't load on Rocky (BUILD.md, Linux).
