# Dormison

Dormison 在 Wine 中加入可调整大小的游戏窗口、呈现阶段的超分与缩放，以及 macOS 菜单栏和程序坞集成。它是 [Sevoflurane](https://github.com/kageroumado/sevoflurane/blob/main/README.zh-CN.md) 使用的引擎，包含 D3DMetal 宿主支持和 msync+ 同步。

[通过 Sevoflurane 安装](https://github.com/kageroumado/sevoflurane)

## 特色

- 在保持游戏内部渲染分辨率的同时调整支持游戏的窗口大小。
- 显示画面时使用 Lanczos、MetalFX、Anime4K 或 CuNNy 放大。
- 将游戏菜单、图标和支持的 NW.js 游戏接入 macOS。

<details>
<summary>完整功能</summary>

- DXMT、DXVK/MoltenVK、D3DMetal 和 wined3d 渲染器。
- 可调整大小的窗口，以及 Lanczos、MetalFX Spatial、Anime4K 和 CuNNy 放大。
- 帧率、帧时间、CPU/GPU 负载、功耗和温度显示；按游戏限制帧率。
- 原始鼠标输入、光标限制，以及支持震动的蓝牙 Xbox 手柄。
- Steam 运行时修改下次启动使用的游戏设置。
- 独立程序坞图标、原生菜单栏，以及支持游戏的 macOS NW.js 运行时。

</details>

## 其他信息

- 面向 x86_64 macOS；在 Apple 芯片上通过 Rosetta 运行。
- [构建与打包](build-macos/README.md) · [技术参考](REFERENCE.md) · [贡献指南](CONTRIBUTING.md)。
- [English](README.md) · [简体中文](README.zh-CN.md)。
