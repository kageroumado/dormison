# Dormison

[English](README.md) · [简体中文](README.zh-CN.md)

Dormison 是 [Sevoflurane](https://github.com/kageroumado/sevoflurane/blob/main/README.zh-CN.md)
使用的 Wine 引擎，用来在 macOS 上运行 Windows 版 Steam 游戏。

Sevoflurane 会下载引擎并管理它的设置。想直接用它玩游戏，可以从
[Sevoflurane 的使用说明](https://github.com/kageroumado/sevoflurane/blob/main/README.zh-CN.md#开始使用)
开始。Dormison 为 x86_64 macOS 构建，在 Apple 芯片上通过 Rosetta 运行。

和 Sevoflurane 一样，它的名字也来自一种麻醉药。

## 能做什么

| 功能 | 使用方式 |
|---|---|
| 图形 | 可选用于 Direct3D 10/11 的 DXMT、通过 MoltenVK 运行的 DXVK、Apple 游戏移植工具包（Game Porting Toolkit）中的 D3DMetal，或 Wine 自带的 wined3d。在 Sevoflurane 中可以为整个容器或单款游戏选择渲染器。 |
| 窗口缩放 | 让支持的游戏自由调整窗口大小，同时保留游戏原本的渲染分辨率。可以用 Lanczos、MetalFX Spatial、Anime4K 或 CuNNy 放大画面。 |
| 帧率 | 在游戏的 View（显示）菜单中显示帧率计数器、帧时间卡片，或带有游戏 CPU、GPU 负载、功耗和温度的卡片。可以为单款游戏或在游戏运行时限制帧率。 |
| 输入 | 游戏锁定光标、用鼠标控制视角时，可以使用原始鼠标位移。可以通过蓝牙连接 Xbox 手柄，以 XInput 或 DirectInput 游玩，支持震动。 |
| 游戏设置 | Steam 保持运行时也能修改单款游戏的设置，下次启动游戏时生效。 |
| Mac 集成 | 在程序坞显示游戏自己的名称和图标，把程序自己的菜单放进 macOS 菜单栏，并用 macOS 原生 NW.js 运行支持的 NW.js 游戏。 |

兼容性取决于游戏、Mac 和所用的图形转译组件。

## 构建与贡献

[构建指南](build-macos/README.md)介绍了工具链、配置、打包和发布流程。
[CONTRIBUTING.md](CONTRIBUTING.md)说明了如何报告问题、验证改动。

每个正式版本都有一个 `r<N>` 标签，每个测试版本都有一个 `b<N>` 标签，附带引擎 tar 包、校验和、Ed25519 签名、
`engine-info.json`，以及相对 `wine-staging-base` 的差异文件。
Sevoflurane 通过带签名的发布清单查找引擎版本。

## 技术细节

这个仓库以 [Wine](https://www.winehq.org) 11.16 为基础，应用了
[wine-staging](https://github.com/wine-staging/wine-staging) 11.16
（标签为 `wine-staging-base`），再加上 Dormison 的改动。
`git diff wine-staging-base` 可以查看完整差异，
[CHANGES.md](build-macos/CHANGES.md)记录了各个版本的内容。

### 在 Wine 基础上增加的功能

下面这些功能和改动是 Dormison 在 Wine 11.16 和 wine-staging 11.16 基础上增加的。

**图形**

- 为 Game Porting Toolkit 3.0 和 4.0 提供 D3DMetal 宿主支持，包括 4.0 的宿主回调表。
- 在画面呈现阶段，为 Metal、OpenGL 和 GDI 窗口提供超分与缩放
  （Lanczos、MetalFX Spatial、Anime4K、CuNNy），带最终滤镜和游戏内的 View（显示）菜单。
- wined3d 将游戏原始分辨率的帧交给画面缩放器。
- 调整游戏窗口大小时，保留游戏的渲染分辨率和宽高比。
- FPS 计数器和帧时间图，从每个进程的统计页读取数据；Sevoflurane 也读取同一份数据。
  更详细的显示级别还会加入游戏的 CPU 占用、GPU 负载、功耗和温度。
- 帧率限制器，覆盖所有 Metal 和 OpenGL 画面呈现路径，可以为单款游戏设置，
  也可以在游戏运行时从 View（显示）菜单切换。
- 通过 `WINEDLLPATH_PREPEND` 为单款游戏指定的渲染器能完整加载，
  因此容器使用其他渲染器时，单款游戏也能使用 DXMT。
- 通过 `SEVO_GPU_*` 配置显卡标识，并在 wined3d 中加入 RTX 50 和 RX 7000/9000 的设备 ID。
- 启用 Vulkan 可移植性枚举，让 Windows 程序能发现 MoltenVK。
- 在支持 Metal 4 的 GPU（M3 及更新芯片）上，关闭显示同步时，present 调用立即返回；
  画面缩放器每次编码一帧。

**窗口、输入与 Mac 集成**

- 每个游戏在程序坞中都有自己的图标和 Steam 游戏名。Steam 的辅助工具，以及通过
  `SEVO_QUIET=1` 启动的进程，会隐藏程序坞图标和屏幕上的窗口。
- 单独打开游戏的程序坞图标或访达中的副本时，会通过 Sevoflurane 启动游戏。
- NW.js 游戏使用原生 NW.js 运行；成就和统计数据由 Steamworks 桩接入，
  可加载 32 位和 64 位 Steamworks。
- 通过窗口服务器限制光标范围，并为鼠标视角控制提供原始位移。
- 游戏不响应退出操作时，显示“未响应”对话框；Wine 以外创建的线程发生异常时，也能生成崩溃报告。
- 程序自己的 Win32 菜单栏可以放进 macOS 菜单栏，窗口中的菜单条会被裁掉；
  Ctrl 快捷键显示为对应的 Command 快捷键，菜单选择按程序要求以 ID 或位置发回。
- winebus 的 SDL 总线关闭或启动失败时，手柄通过 IOHID 总线接入游戏。
  通过蓝牙 LE 连接的 Xbox One S、Elite 2 和 Adaptive 手柄会被识别为 Xbox 手柄。
- 可以隐藏菜单栏中的托盘图标。后台程序允许显示器正常休眠。

**进程、同步与内存**

- msync+：基于 Mach 的进程内同步（`WINEMSYNC=1`），是 Dormison 对 CrossOver msync 的分支。
- 临界区释放时让锁重新可用，与 Windows 的行为一致：正在运行的线程可以在被唤醒的等待线程
  接手前重新取得锁，`LockCount` 的值也按 Windows 的方式编码。
- 每个程序可以有自己的设置文件，在进程启动时读取。
- 原生 arm64 wineserver。
- 修复 Steam 启动时的 20 秒网络等待，以及 Unity/Mono 的 TLS 读取问题。
- 为 32 位游戏提供 `SEVO_LARGE_ADDRESS_AWARE`。

**音频、媒体与文字**

- 附带 GStreamer（LGPL 构建），用于播放 Media Foundation 和 DirectShow 视频，
  包括 MPEG 程序流。
- 为 Media Foundation 提供 HEVC 解码器转换（transform）。它和 H.264 解码器都通过
  VideoToolbox 进行硬件解码。
- CoreAudio 将每个音频流各声道的音量应用到它自己的采样数据上，
  保留其他程序和 Mac 的音量；将采集流静音时，录到的也是静音。
- Discord Rich Presence 桥接。
- 将日文字体族映射到附带的 IPAGothic/IPAPGothic，以及 Mac 自带的 Hiragino Mincho。

**发布**

- `wine --version` 会显示具体发布版本。
- 每个版本都有签名，附带相对 `wine-staging-base` 的差异文件，
  以及记录每个文件 sha256 和来源的清单。驱动或服务端与源码不匹配时，打包会中止。

### 与 CrossOver 的比较

这里对比的是 CrossOver 26.3 的源码发布版：以 Wine 11.0 为基础，
加入 CodeWeavers 的补丁，未应用 wine-staging。

**来自 CrossOver 的部分**

- msync，以 msync+ 分支维护，并加入了 CrossOver 26.3 尚未包含的修复：
  - wineserver 在冻结整个对象集合的状态下批准 WaitAll，确保获准的是同一时刻全部满足条件的集合，
    并在等待线程返回前一次性消耗；查询不会读到只消耗了一部分的状态。
  - WaitAll 能报告被遗弃的互斥锁。
  - `NtReleaseMutant` 返回 NT 语义下的前一次计数。
  - 用栈复用已释放的对象索引。
  - wineserver 终止时，其进程也会退出。
  - 线程若在设置对象之后、唤醒等待线程之前终止，等待线程会一直睡在一个已可用的对象上。
    进程终止时，wineserver 会唤醒它持有句柄的每个对象：一次在它最后一个线程离开时，
    一次在确认它已终止时；进程仍在运行、只有线程被终止时也会这样做。
  - 向 wineserver 发送 `SIGUSR2` 会扫描那些已可用且 100 毫秒内未变化、却仍有线程在等待的对象，
    唤醒这些线程，并报告对象、持有者以及与之共享该对象的最近终止的进程。
- Rosetta 兼容处理：通过 `lretq` 完成 32→64 位切换、恢复 MXCSR、处理
  XGETBV/CET/调试寄存器，以及重新转译被写入的代码。跨进程写入使转译失效时，
  Dormison 逐个内存区域处理；CrossOver 则将第一页的保护属性恢复到整个范围。
- D3DMetal 对接：`__wine_unix_call`、Microsoft ABI 跳板，
  以及保存在 Darwin 线程存储中的 GS 基址和 TEB、PEB 镜像。
- 位于 `%gs:0x1780` 的 Mono TLS 镜像。Dormison 还会预留该偏移对应的 pthread key。

**Dormison 相比 CrossOver 26.3 增加的部分**

- Wine 11.16 和 wine-staging。
- 画面缩放器、D3DMetal 4.0 回调表，以及 Metal 4 的 present 顺序控制；
  关闭显示同步时，present 无需等待。
- 临界区释放后让锁重新可用；CrossOver 和 Wine 一样，会把锁交给下一个等待线程。
- 将各声道音量应用到采样数据上；CrossOver 设置的是设备音量，会影响 Mac 的音量。
- 原生 arm64 wineserver。
- Steam 网络等待修复、Vulkan 可移植性枚举和 `SEVO_GPU_*`。
- 通过窗口服务器限制光标范围。
- ntdll 中的 `localtime_r`，让 PEB 镜像能安全工作。
- 程序坞适配层、NW.js 运行器、Discord 桥接、统计页和按程序设置。

**CrossOver 26.3 提供、Dormison 尚未包含的部分**

- WoW64 下仅运行 32 位程序的容器模式，以及 Rosetta 的 16 位 LDT 修复。
- Direct3D 10/11 默认使用基于 Vulkan 的 wined3d。
- 针对 GTA IV/V、Counter-Strike 2、Cities: Skylines II 的修复，
  以及 Tomb Raider I–III Remastered 的 BC7 解码。
- Epic、Battle.net、Rockstar、GOG Galaxy 和 Ubisoft Connect 启动器的修复。
- PlayStation 手柄蓝牙震动、Xbox 360 USB 总线，以及麦克风权限提示。
- CrossOver 的桌面和 Office 集成。

### 图形与画面呈现

引擎附带 [DXMT](https://github.com/3Shain/dxmt)，用于运行 64 位和 32 位
Direct3D 10/11 程序；也附带通过 MoltenVK 运行的
[DXVK](https://github.com/Gcenx/DXVK-macOS)。
Sevoflurane 可以从 Apple 游戏移植工具包导入 D3DMetal。

D3DMetal 支持包括 `__wine_unix_call` 导出、工具包回调的 Microsoft ABI 封装，
以及 D3DMetal 4.0 使用的宿主回调表。Wine 将 GS 基址保存在 Darwin 线程存储中，
建立 TEB 和 PEB 镜像，并在工具包解析符号之前映射 `win32u.so`。

可选的画面呈现器（presenter）负责缩放 Metal drawable、OpenGL 窗口 drawable
和 GDI 窗口表面。OpenGL drawable 是一个帧缓冲对象，交换缓冲区时写入 IOSurface，
再由呈现器读取，因此 wined3d 的 Direct3D 9 也能接入画面缩放器。
使用多重采样、立体显示、浮点或 10 位格式的 drawable 仍由 OpenGL 视图显示；
设置 `OpenGLPresenter=N` 可以让所有 OpenGL drawable 都走这条路径。
呈现器支持 Lanczos、MetalFX Spatial 和编译后的 mpv 着色器包，
包括 Anime4K 和 CuNNy 使用的片元及计算通道。最终滤镜会对输出再做一次重采样。
可通过 `Upscaler`、`FinalFilter`、`PresenterLog`，或
`SEVO_UPSCALER`、`SEVO_FINAL_FILTER`、`SEVO_PRESENTER_LOG` 和 `SEVO_SHADER_DIR` 配置。

程序运行时会有一个 View（显示）菜单，包含 Upscaler（画面缩放器）、Final Filter（最终滤镜）、
Show Frame Rate（显示帧率，设置 `FrameRate=Y` 或 `SEVO_FPS=1` 可从第一帧开始显示）、
Show Frame Time Graph（显示帧时间图，`FrameRateGraph=Y` 或 `SEVO_FPS_GRAPH=1`）、
Show Picture Details（显示画面详情）、Overlay Detail（叠加层详细程度）和
Frame Rate Limit（帧率限制）。Overlay Detail 可以只显示帧率、显示帧时间卡片，
或在卡片中再加一行游戏的 CPU 占用、GPU 负载、Mac 的功耗和 CPU 温度
（`OverlayLevel=1|2|3` 或 `SEVO_OVERLAY_LEVEL`）。Frame Rate Limit 提供关闭、
30、40、45、60、90 和 120 几档（`FrameRateLimit=<n>` 或 `SEVO_FPS_LIMIT=<n>`
可从启动时生效）；无论是否使用画面缩放器，每条 Metal 和 OpenGL 路径都按固定的截止时间送出帧。
计数器、图表和 Sevoflurane 读取的是同一份进程统计页。

设置 `NativeMenuBar=Y` 或 `SEVO_MENU_BAR=1` 后，程序的 Win32 菜单栏会出现在
macOS 菜单栏中，位于应用菜单和 View（显示）之间；窗口中的菜单条会被裁掉，
窗口的几何尺寸仍与 Windows 上一致。菜单会显示程序在打开菜单时设置的勾选标记和禁用项，
弹出模态对话框时菜单也会被禁用。菜单选择以带项目 ID 的 `WM_COMMAND` 发给程序；
菜单设置了 `MNS_NOTIFYBYPOS` 时，则以带项目位置和菜单句柄的 `WM_MENUCOMMAND` 发送。
程序自己有 View 菜单时，驱动的 View 菜单改名为 Picture（画面）。

`ResizableWindows` 允许调整显示窗口的大小，同时保留游戏的渲染尺寸和宽高比。
`LinearMouse` 在程序锁定光标、用鼠标控制视角时提供原始位移。

### 进程与兼容性

Dormison 包含 msync+，即它对 CrossOver msync 实现的分支，通过共享内存、`__ulock` 等待
和连接到 wineserver 的 Mach 端口，在进程内完成同步（`WINEMSYNC=1`）。
这些改动以提交的形式维护在 `main` 上。Rosetta 兼容处理涵盖 32→64 位切换、
被写入的可执行代码和信号上下文。位宽切换使用 `lretq`，
信号处理会保留线程的 MXCSR 状态。

进程启动时，Wine 先读取 `<prefix>/.sevo/bottle.env`，
再读取 `<prefix>/.sevo/apps/<exe>.env`，其中 `<exe>` 是可执行文件的文件名，统一为小写。
所以游戏设置会在下次启动时生效。
`WINEDLLPATH_PREPEND` 可以在内建 DLL 目录之前插入一个搜索目录。放在其中的渲染器会完整加载：
Wine 没有对应版本的 DLL 无需在容器中放置文件即可加载；
DLL 的 unix 部分不在该目录中时，使用引擎自带的那一份。

`SEVO_GPU_*` 变量设置 Windows 程序看到的显卡标识、显存和驱动信息。
没有指定驱动信息时，未知厂商会使用 NVIDIA 的驱动信息。

Steam 启动修复涵盖 Vulkan 可移植性枚举、初始网络通知、显示模式切换、
根设备枚举和可重入的本地时间转换。

程序坞适配层（`libsevodockshim.dylib`）会隐藏 Steam 辅助进程的程序坞图标，
并给游戏显示各自的名称和图标。对于支持的 NW.js 游戏，它会启动原生运行时。
[Steamworks 桩](build-macos/steam-stub/README.md)留在 Wine 中，
通过本机回环连接向原生游戏提供成就和统计数据。

## 许可证

沿用 Wine 的 LGPL 2.1 或更新版本，见 `LICENSE`。
`fonts/` 中的字体各有自己的许可证（`COPYING.*`）。msync 来自 CrossOver，采用 LGPL。
画面呈现器、程序坞适配层和 Steamworks 桩的版权为 © 2026 kageroumado，
采用 LGPL 2.1 或更新版本。
