# Contributing to Dormison

Dormison is Wine with the changes that make it Sevoflurane's engine. The app's
[CONTRIBUTING](https://github.com/kageroumado/sevoflurane/blob/main/CONTRIBUTING.md)
covers the rules shared by both repositories: comments describe the code in the
present tense, US English, a pull request says who wrote the change and how it
was verified. This file covers what is specific to the engine.

## Reporting an engine problem

Use the issue template. It asks for the diagnostics zip the app writes
(**Settings › About › Save Diagnostics…** or `sevo diag`) with the Wine
diagnostics log turned on, the engine version from `engine-info.json`, the
renderer, and the options in play.

## Changing the engine

- `git diff wine-staging-base` is the whole change against upstream. Keep it
  readable: one concern per commit, upstream style in upstream files, and no
  app internals in Wine comments.
- `build-macos/README.md` is the build guide. The driver's Swift half is built
  before wine; `package-engine.sh` refuses a tree where it is stale.
- Test on a real program before opening a pull request: a game, the DirectX 12
  samples, winecfg. Say which ones in the pull request.
- msync lives in `server/msync.c` and `dlls/ntdll/unix/msync.c` as commits on
  `main`; changes to it are ordinary commits there.

## Rebasing onto the next wine-staging

The recipe is the last section of `build-macos/README.md`: tag the new staging
commit, rebase `main` onto it with `--onto`, resolve, rebuild, test, publish.
