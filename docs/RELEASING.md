# Releasing

How a version goes from `dev` to a public GitHub release with **macOS and Windows parity**. There is one `VERSION`, one `CHANGELOG.md` section and one release event, and each platform's artifacts are built on its own machine. Follow this guide whenever something merges into `master` or Matt asks to strike a release. Build and install mechanics live in [BUILD.md](../BUILD.md); this guide covers the order, the hand-offs and the human gates.

## Machines and who does what

| Machine | Builds | Holds | Human-only work there |
|---|---|---|---|
| **Mac** | universal macOS pkg + zip | Developer ID Application + Installer certs, `NDI_NOTARY` notarytool profile, Standard NDI SDK at `/Library/NDI SDK for Apple` | Tier 1–2: install the pkg, restart Resolve, check the stream in NDI Video Monitor |
| **Windows workstation** | Inno Setup `.exe` + zip | VS2022, CUDA 12.9, Standard NDI 6 SDK at `C:\Program Files\NDI\NDI 6 SDK` | Tier 1–2: run the `.exe`, restart Resolve, check the stream in NDI Studio Monitor (ideally from a second machine) |

Both machines use the **Standard (royalty-free) NDI SDK**. A Windows build without the SDK is stub-linked: it carries a `-STUB` name and must never be released.

The **Mac publishes the release** (it creates the tag, the release and the notes). The **workstation then uploads** its three files to that release and adds the Windows notes section.

## Human gates

The agent stops and waits for Matt at each of these. Nothing gets past one without his explicit reply.

1. **Strike the release.** Only Matt starts one.
2. **Merge the release PR** (`dev` → `master`).
3. **Mac Tier 1–2** on the pkg-installed plugin, before publishing.
4. **Publish.** The agent confirms with Matt right before `publish_github_release.sh --publish`.
5. **Windows Tier 1–2** on the installed `.exe`, before uploading.

Other human-only steps when needed: signing setup on a new Mac (BUILD.md → "One-time signing setup"; Matt creates the certs in Xcode and runs `xcrun notarytool store-credentials NDI_NOTARY --apple-id <apple-id> --team-id 647FZYVR5Q` himself), and installing either NDI SDK.

## Steps

### 1. Parity check (agent, either machine)

List every PR merged into `dev` since the last tag (`git log --merges --oneline v<last>..dev`). For each one, record whether Tier 1–2 passed on macOS, on Windows, or whether the change doesn't touch that platform. **Done when** every PR has a status for both platforms. A platform may lag only if the CHANGELOG entry says so in words (CLAUDE.md allows one platform lagging, stated in the notes). Report the table to Matt.

### 2. Release prep PR (agent)

On `feature/release-X.Y.Z`, branched off `dev`:
- Bump the version with `./scripts/increment_version.sh` for a patch, or `./scripts/set_version.sh X.Y.Z`. They update `VERSION`, the `#define`s and `Info.plist`.
- Add the `## [X.Y.Z] - YYYY-MM-DD` section to `CHANGELOG.md`. The publish script turns it into the release notes, so write it for end users and include any lagging-platform note from step 1.
- Run `make dev && make test` on the Mac, or the CMake build plus `ctest` on Windows.

Open a PR into `dev`. **Done when** both CI `build` checks are green and the PR is merged.

### 3. Release PR (agent opens, Matt merges)

`gh pr create --base master --head dev --title "Release vX.Y.Z — <summary>"`. In the body: what's in the release, the parity table from step 1, and the remaining steps of this guide. **Done when** Matt has merged it.

### 4. macOS build and publish (Mac)

```bash
git checkout master && git pull && cat VERSION     # must print X.Y.Z
./scripts/package_release.sh                       # build, sign, notarize, staple → dist/vX.Y.Z/
```
**Done when** the script ends with a Gatekeeper `accepted / source=Notarized Developer ID` and `shasum -a 256 -c dist/vX.Y.Z/SHA256SUMS.txt` passes. Then the **Mac Tier 1–2** gate: Matt installs `dist/vX.Y.Z/NDIOutput-X.Y.Z-macOS.pkg` and confirms the stream. Then the **publish** gate, then:
```bash
./scripts/publish_github_release.sh --publish
```
It reports "releasing macOS only" when `dist/vX.Y.Z/` has no Windows files. That is the normal path; the workstation adds them in step 5. **Done when** `gh release view vX.Y.Z` shows the pkg, zip and `SHA256SUMS.txt` and the release is not a draft.

### 5. Windows build and upload (workstation)

Build from the tag so both platforms ship the same commit:
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
In `notes.md`, insert the `### Install — Windows` section immediately before the final `---` / NDI® trademark footer. Copy it verbatim from the Windows heredoc in `scripts/publish_github_release.sh`; that script is the single source of the wording. If the CHANGELOG section says "macOS only", replace that line with the Windows status. Then run `gh release edit vX.Y.Z --notes-file notes.md`.

**Done when** the release lists six assets (macOS pkg, zip and SHA256SUMS; Windows exe, zip and SHA256SUMS-Windows) and the notes have both Install sections.

### 6. Close out (agent, either machine)

Check the repo homepage README (`gh api repos/light-sail-vr/lsvr-vrndi-resolve-plugin/readme`) matches `master`. Confirm `git log --no-merges origin/dev..origin/master` prints nothing. Release merge commits leave `master` "ahead" of `dev` with identical content, which is normal. A non-merge commit in that list landed on `master` outside a release PR; bring it back into `dev` by PR. Append a LEARNINGS.md entry for anything that went wrong. **Done when** Matt has a one-paragraph summary with the release URL.

## Hand-off prompts

Paste these on the other machine, filling in the version. Each one is self-contained.

**Mac, after Matt merges the release PR:**
> Strike release vX.Y.Z, macOS half. Follow docs/RELEASING.md step 4 on this Mac: pull `master`, check that VERSION is X.Y.Z, run `package_release.sh`, then stop for my Tier 1–2 check of the pkg and ask me before you publish. After publishing, give me the Windows hand-off prompt from docs/RELEASING.md with the version filled in.

**Windows workstation, after the Mac has published:**
> Release vX.Y.Z is live with macOS artifacts only. Follow docs/RELEASING.md step 5 on this workstation: check out tag vX.Y.Z, build against the Standard NDI 6 SDK (stop if it stub-links), package with `package_windows_release.ps1`, stop for my Tier 1–2 check of the installer, then upload the three files and add the Windows install section to the release notes. Report the final asset list.

**Cross-platform change, after a feature PR merges into `dev` on one machine:**
> PR #N merged into dev from the [Mac|workstation]: <one line on what it changes>. On this machine: pull dev, build and run the tests per BUILD.md, and tell me what this platform needs: nothing (no platform code touched), a Tier 1–2 check by me, or a port, which goes on its own `feature/` (or `feature/win-`) branch off dev. Record the result so docs/RELEASING.md step 1 can find it.

## Gotchas

- **Repo URL:** the repo is `light-sail-vr/lsvr-vrndi-resolve-plugin`. The old `lightsailvr/ResolveOFX_NDIOutput` redirects for git, but `gh pr create` resolves the wrong repo through the redirect. Check `git remote -v` on a machine that hasn't been updated.
- **New Mac:** the certs and the notary profile don't migrate. `package_release.sh`'s preflight names whatever is missing; Matt redoes the setup above. The Team ID is `647FZYVR5Q`.
- **`make` and link flags:** after changing `LDFLAGS`/`CXXFLAGS`, force a relink. `make` doesn't track the Makefile (LEARNINGS 2026-10-01).
- **Plugin missing in Resolve after an install:** with Resolve closed, delete `OFXPluginCacheV2.xml` (path per platform in CLAUDE.md) and relaunch.
- **Verify a release zip with `ditto -x -k`**, never `unzip` (LEARNINGS 2026-09-01).
