# Dormison

Dormison adds resizable game windows, presentation-time upscaling, and macOS menu-bar and Dock integration to Wine. It powers [Sevoflurane](https://github.com/kageroumado/sevoflurane), with D3DMetal host support and msync+ synchronization.

[Install through Sevoflurane](https://github.com/kageroumado/sevoflurane)

## Highlights

- Resize supported games independently of their rendering resolution.
- Apply Lanczos, MetalFX, Anime4K, or CuNNy at presentation time.
- Integrate game menus, icons, and supported NW.js games with macOS.

<details>
<summary>Full feature list</summary>

| Feature | What you can use |
|---|---|
| Graphics | DXMT for Direct3D 10/11, DXVK over MoltenVK, D3DMetal from Apple's Game Porting Toolkit, or Wine's wined3d. Choose a renderer per bottle or game in Sevoflurane. |
| Window scaling | Resize supported game windows while the game keeps drawing at its original resolution. Scale the picture with Lanczos, MetalFX Spatial, Anime4K or CuNNy. |
| Frame rate | Show a frame-rate counter, a frame-time card, or a card with the game's CPU, the GPU's load, power and temperature from the game's View menu. Cap the frame rate per game or while it runs. |
| Input | Use raw mouse movement when a game holds the cursor for mouse-look. Play with an Xbox controller over Bluetooth through XInput or DirectInput, rumble included. |
| Game settings | Change per-game settings for the next launch while Steam stays open. |
| Mac integration | Show a game's title and icon in the Dock, show a program's own menus in the macOS menu bar, and run supported NW.js games with native macOS NW.js. |

</details>

## Notes

- x86_64 macOS engine; runs under Rosetta on Apple silicon.
- [Build and packaging guide](build-macos/README.md) · [Technical reference](REFERENCE.md) · [Contributing](CONTRIBUTING.md).
- [English](README.md) · [简体中文](README.zh-CN.md).
