# Engine releases

One section per release, written when the work lands. `publish-engine.sh`
uses the section for `r<N>` as the GitHub release body.

## r1

The first public release of Dormison: Wine 11.16 with wine-staging 11.16, plus the changes
Sevoflurane needs to run Windows Steam games on an Apple silicon Mac.

- Graphics through DXMT (Direct3D 10/11), DXVK over MoltenVK, D3DMetal from Apple's Game
  Porting Toolkit 3.0 and 4.0 (the user's own copy), or wined3d, chosen per bottle or game.
- Resizable game windows that keep the game's resolution and aspect ratio, with a
  present-time upscaler for Metal, OpenGL and GDI windows: Lanczos, MetalFX Spatial, Anime4K
  and CuNNy, switched from the game's View menu.
- A frame-rate counter and frame-time graph from the View menu, and per-run frame traces the
  app reads.
- Raw mouse movement for games that hold the cursor for mouse-look.
- msync synchronization with a wineserver that commits WaitAll sets whole.
- Per-bottle and per-game settings read from env files at each launch, so a change needs no
  Steam restart.
- Each game's title and icon in the Dock, and native macOS NW.js for supported RPG Maker MV
  and MZ games.
- CoreAudio streams that apply WASAPI per-channel volume, and a GStreamer media back end.

`git diff wine-staging-base` is the complete change; the README's "Changes from Wine" lists
it by area.
