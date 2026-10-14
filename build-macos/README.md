# Building Dormison on macOS

These scripts assemble Sevoflurane’s Wine engine, including its Swift Mac driver, native wineserver, and renderer payloads. Packaging verifies source build IDs and server protocols so stale components fail before release.

`main` contains wine-staging plus Dormison’s patches; `git diff wine-staging-base` shows the changes. Builds live outside the checkout in `$DORMISON_BUILD`, defaulting to `$HOME/dormison-build`.


<details>
<summary>Build and packaging reference</summary>

## Prepare

Install mingw-w64 and bison, then prepare the Intel dependency bottles and GStreamer as described in the [toolchain reference](REFERENCE.md#toolchain). Packaging also requires an installed Sevoflurane engine as a donor, or `SEVO_LIVE_ENGINE` pointing to one.

**Use Xcode 27.0’s linker or validate Genshin scene loads before publishing.** Some builds corrupt asset reads despite intact files on disk; see the [failure evidence and working configuration](REFERENCE.md#a-build-that-corrupts-genshins-asset-reads).

## Build

From the repository root:

```bash
W=${DORMISON_BUILD:-$HOME/dormison-build}
build-macos/fetch-gstreamer.sh
build-macos/configure.sh
make -C "$W/wine-src/dlls/winemac.drv/swift"
make -j10 -C "$W/build"
make -j10 -C "$W/build" install-lib
```

Build the Swift archive before Wine whenever its sources change. To use dependency libraries from an installed engine, see [configure-from-engine.sh](REFERENCE.md#configuring-against-an-installed-engine).

## Package and release

```bash
build-macos/package-engine.sh dormison-r5
build-macos/publish-engine.sh r5 --key <ed25519.pem> --dry-run
build-macos/publish-engine.sh r5 --key <ed25519.pem>
```

Choose a new `r<N>` for each release. Publishing requires a clean, committed tree on `origin/main`; `r5` above illustrates the format. Run games and DirectX 12 samples before installing over the active engine, with the client stopped and the previous engine retained.

The [reference](REFERENCE.md) covers packaging gates, manifests, fonts, msync+, DXMT, renderer provenance, native-server builds, release signatures, and rebasing.

</details>
