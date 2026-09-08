---
name: Engine problem
about: A crash, a hang or wrong rendering inside Wine itself, a DirectX 12 sample that draws wrong, or a build problem.
title: ""
labels: bug
assignees: kageroumado
---

## What happens

<!-- One or two sentences. Name the program: a game and its Steam app id, a DirectX 12 sample, winecfg, the Steam client. -->

## Where it happens

- **Engine**: <!-- Dormison r<N>, from engine-info.json -->
- **Renderer**: <!-- D3DMetal <version>, DXMT <version>, DXVK, wined3d -->
- **Options in play**: <!-- Upscaler, ResizableWindows, LinearMouse, msync on/off, env files; or "defaults" -->
- **macOS and Mac**: <!-- e.g. 26.1 on a Mac Studio M2 Ultra -->

## Logs

Attach the diagnostics zip from the app (**Settings › About › Save
Diagnostics…** or `sevo diag`) with **Wine diagnostics log** turned on before
reproducing, so the Wine log carries `err+all,+seh` for the process. For a
presenter problem, add `PresenterLog=Y` (`SEVO_PRESENTER_LOG=1`); for window
geometry, `PresentationLog=Y`. Paste the lines that matter in the issue as
well.

## If you built it

<!-- The commit, the configure line if it differs from build-macos/README.md, and whether the driver's Swift archive was rebuilt before winemac.so. -->

## Access

A game problem is fixed by running the game. If I do not own it, I will ask
here for access: a gift copy through Steam, or a donation that covers it.
