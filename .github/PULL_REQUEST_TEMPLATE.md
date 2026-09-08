<!-- Thanks for contributing to Dormison. Fill in what applies; delete the rest. -->

## Summary

<!-- One or two sentences: what this changes in the engine, and why. -->

## Related issue(s)

## Changes

-

## How it was tested

<!-- Built and run, on what. The engine runs games under Steam; say which. -->

- **macOS / Mac**:
- **Built with**: <!-- the configure line from build-macos/README.md, or what differs -->
- **Run against**: <!-- winemine, winecfg, a DirectX 12 sample, a game and its renderer -->
- **Behavior observed**:

## Risk

<!-- Rosetta paths, the presenter, msync, the D3DMetal host glue and the Steam client's boot each deserve a sentence when touched. -->

## Checklist

- [ ] `make -C dlls/winemac.drv/swift` before wine, when a Swift file changed
- [ ] The touched modules build (`make -C $DORMISON_BUILD/build dlls/<module>/<module>.so`)
- [ ] Code carried from CrossOver or upstream Wine is left as it is; our comments say what the code does, in the present tense
- [ ] No unrelated changes bundled in

---

## Authorship

<!-- Many PRs here are written with an agent. Record who wrote this one and how; a maintainer reviews an unattended run differently from an attended one. -->

- **Author**: <!-- the human, or the agent's name -->
- **Model**: <!-- the model the agent runs on; leave blank if human-authored -->
- **Session**: <!-- "attended" (a human participated or reviewed live) or "automatic" (unattended agent run) -->
- **Verification**: <!-- what the agent actually built and ran, or "none beyond the build" -->
