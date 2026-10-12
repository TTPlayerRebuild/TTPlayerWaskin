# Winamp 皮肤下原版控件露出修复（2026.10.13）

## 原因

这是宿主和插件之间的窗口显示管理问题。插件接管已有 HWND 绘制皮肤，原版的列表、按钮、歌词和视觉控件仍保留在这些 HWND 内。

- WSZ 以前在接管、调整大小及定时器中枚举并隐藏子控件。宿主随后调用显示或重绘接口时，控件仍能在下一次定时检查前出现。
- WAL 主窗口以前只在接管时隐藏一次，后续显示请求和新建控件没有同样的处理。
- `WM_SETREDRAW(TRUE)` 可以改变隐藏控件的 `WS_VISIBLE` 状态。主程序的备用 LISTBOX 刷新此前无条件发送这一消息。
- 父窗口隐藏时，Windows 的 `ShowWindow` 快速路径可能只给子控件发送 `WM_SHOWWINDOW`，没有 `WM_WINDOWPOSCHANGING`。因此还必须在父窗口重新显示之前统一检查。

用更新后的宿主配合旧版 DLL 运行同一项回归测试，WSZ、HeadAMP WAL 均在隐藏父窗口重新显示时复现原版 `VisualCtrl` 露出。

## 修复

`src/native_children.h` 为 WSZ 和 WAL 共用的子控件管理器：

1. 接管原版子控件，阻止其显示位置、可见样式和重绘启用请求；原版控件不再绘制到插件表面。
2. 处理 `WM_PARENTNOTIFY/WM_CREATE`，及时纳入接管后新建的控件；保留定时检查以处理没有父窗口创建通知的控件。
3. 在父窗口显示之前重新隐藏原版子控件，覆盖隐藏父窗口的消息快速路径。
4. `WM_PAINT` 使用 `BeginPaint/EndPaint` 消费内部绘制请求，避免仅验证矩形造成消息队列持续出现绘制消息。
5. 保留 `TTP_SKIN_CONTENT_CHILD` 标记的歌词编辑器和工具栏；已经分离的全屏歌词、视觉窗口不受拦截。
6. 解除插件接管时移除子类过程并恢复原版可见状态；销毁子控件时立即清理记录，避免 HWND 复用和悬空回调。带内容标记的缓存工具栏不由插件强制显示。

`rebuild/src/ui/player_window.cpp` 的备用列表刷新改用 `ScopedSkinRedraw`，仅对原本可见的列表配对暂停/恢复重绘。

不修改皮肤资源、标题区域或插件 ABI。

## 验证

本地 C++ 测试位于 `rebuild/tests/waskin/native_children_host_tests.inc`，不打包、不上传、不加入 Actions：

- 实际宿主中的 34 个原版子控件；不依赖等待插件定时器消除残影。
- 隐藏父窗口后恢复、`ShowWindow`、`SetWindowPos`、`WM_SETREDRAW`、可见样式修改、原先停放的控件重新调整大小。
- 接管后新建/销毁控件，显式绘制请求，切回原版时恢复显示状态。
- 歌词编辑器及工具栏正常显示，退出编辑后工具栏继续隐藏。
- 原生备用列表刷新保留原先隐藏状态。

XP 和 Win7 虚拟机均通过 WSZ、HeadAMP WAL 下的上述回归，以及歌词/视觉三种内容模式、全屏/桌面歌词、切换皮肤、歌词编辑器、托盘右键菜单和歌词模式切换测试。两台虚拟机的 Explorer 均接受了测试托盘图标。

测试记录：`rebuild/out/test-artifacts/native-children-20261013/`。DLL 继续通过 x86、XP 5.1 和 Win7 6.1 导入检查；本地构建完成后恢复 `BUILD_TESTING=OFF`。

## 更新文件

- 宿主更新包：`rebuild/build/Release/TTPlayerRebuild-2026.10.13.zip`
- 插件：`rebuild/build/Release/AddIn/ttp_waskin.dll`

更新时同时替换宿主和插件。MAKI DLL 无需更改。
