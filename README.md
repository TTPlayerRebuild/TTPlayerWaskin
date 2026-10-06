# ttp_waskin

<img width="550" height="475" alt="image" src="https://github.com/user-attachments/assets/62f4d05d-9628-43b7-a0b1-114e4359c375" />


TTPlayerRebuild 的可选 Winamp 皮肤插件，输出名称为 **`ttp_waskin.dll`**。经典 WSZ 可独立使用；HeadAMP WAL 子集可通过独立的 **`ttp_maki.dll`** 启用。

## 使用

1. 使用已接入皮肤插件接口的新版 `TTPlayerRebuild.exe`。
2. 将 `ttp_waskin.dll` 放入 **该 EXE 同目录下的 `AddIn`**。普通版与 XP／Win7 版共用完全相同的 DLL。
3. 启动后可直接选择 Winamp 列表里的 `<默认皮肤>`。其它经典 `.wsz` 放入该运行目录的 **`Skin/waskin`**，从皮肤菜单或选项中的皮肤页选择。此前放在 `Skin` 或 `Skin/new` 的 Winamp 包需要移入此目录。
4. 也可以将 `.wsz` 拖入主窗口安装，或通过命令行打开 `.wsz`，程序会先复制至 `Skin/waskin` 再加载。
5. 使用 HeadAMP WAL 时，将独立 `makivm` 工程生成的 `ttp_maki.dll` 放在同一 `AddIn` 目录，再把 WAL 放入 `Skin/waskin` 并重启。缺少或无法加载 VM 时不列出 WAL；WSZ 不受影响。

选项 → 皮肤：检测到有效 Plugin Skin 接口后，顶部显示“原生”和各 DLL 定义的标签，打开时默认选中当前使用的插件。右键菜单的皮肤分组也使用同一名称。本 DLL 声明名称 **“Winamp”**、子目录 **`waskin`**、后缀 **`.wsz`**（VM 可用时增加 **`.wal`**），宿主因此读取 `Skin/waskin` 直属目录。列表顶部显示 DLL 内置的 **`<默认皮肤>`**（`base-2.91.wsz`）；目录为空或不存在时也可以选择和预览，不能删除。没有可用插件时不显示标签。

其它 WSZ 缺少主窗口、均衡器、播放列表或视频窗口的位图时，DLL 使用内置默认皮肤的对应资源补齐。已有的有效图集仍按原图绘制，包括作者刻意裁短的图集；ZIP 损坏或没有任何有效经典位图的包仍会被拒绝。详见 [内置默认皮肤与资源回退](docs/BUILTIN_DEFAULT_SKIN.md)。

插件皮肤的配置名称保存为相对于 `Skin` 的完整路径，例如 `waskin/文件名.wsz`。目录与类型由 DLL 声明，宿主不再硬编码 `waskin` 或 `.wsz/.wal`。

外部 WSZ 可通过 `skininfo.xml` 提供皮肤名称、作者、邮箱和网站。缺失字段留空；未填写名称时，列表和菜单使用包文件名显示，名称元数据仍为空。不会从 `readme.txt` 猜测作者或联系方式。解析范围与接口兼容说明见 [WSZ 元数据](docs/WSZ_METADATA.md)。

皮肤页的“下载更多皮肤”按当前选中的页签打开地址：本 DLL 通过可选接口字段 `skin_download_url` 提供 [Winamp Skin Museum](https://skins.webamp.org/)；原生页签，以及没有提供链接、链接为空或不支持该字段的插件，使用宿主默认地址 [千千静听皮肤](https://www.qianqian.plus/skin.html)。浏览器仅在点击链接时打开。

程序在启动时发现插件。运行期间新放入 DLL，需要重启后再选择 WSZ。插件缺失或 Winamp 皮肤损坏时，启动后使用已选原生皮肤；原生包也不可用时才回退到内置默认皮肤。

原生选择保存为 `Skin/PackageName`，插件选择单独保存为 `Skin/CustomPackageName`。未接管的窗口使用已选原生皮肤；应用原生皮肤会清空插件选择。**不读取或迁移旧的 `WinampPackageName`，也不从旧的合并字段恢复插件选择。** 配置示例见 [皮肤选择配置](docs/SKIN_SELECTION.md)。

当前支持经典 WSZ，以及通过独立 MAKI VM 运行的 **HeadAMP 所需 WAL 子集**。不使用 Wasabi，尚不支持任意 Modern / Bento 皮肤。VM 可用时声明 `.wsz;.wal`，否则只声明 `.wsz`；每份 WAL 还需通过控件与脚本检查。详见 [WAL 运行时和支持范围](docs/WAL_RUNTIME.md)。两工程分别构建，waskin 的单仓库 Action 仍只打包皮肤 DLL，VM 由独立工程提供。

## 实现边界

完整的资源、控件切图、输入状态及当前缺项对照见 [WSZ 控件解析分析](docs/WSZ_CONTROL_PARSING.md)。下面列出的是首期基础能力，不表示已完整还原全部经典控件。

DLL 内实现：

- ZIP 容器识别，Stored / DEFLATE 解压，CRC32 校验和资源大小限制；只在内存中读取，不解压到磁盘。
- 根目录或单个子目录中的经典资源识别，文件名大小写兼容。
- BMP RGB / Bitfields，以及 RLE4 / RLE8 解码。
- 主窗口、播放列表、均衡器的位图绘制及各自输入处理。
- 标准按钮图集、音量/平衡/定位、时间与标题、位图文字、播放列表颜色、窗口区域和折叠模式。
- 独立预览图生成，窗口消息子类化，原生子控件隐藏/恢复，换肤时的资源清理。
- 鼠标悬停在插件窗口时的滚轮转发，不要求先获得焦点。

EXE 中的通用接入层负责：

- 从 `AddIn/ttp_*.dll` 发现 `ttpGetSkinPlugin`，校验版本并保活模块。
- 将外部皮肤加入已有菜单、选项预览、拖入安装和启动恢复流程。
- 向 DLL 提供播放状态、可见歌曲列表，并执行 DLL 提交的播放/设置命令。
- 通过同步拖动回调复用 TTPlayer 原生的吸附、分离及相连窗口批量移动，统一使用常规选项中的吸附设置。

主程序没有 WSZ 解压器、Winamp 图集坐标表或经典绘制代码。DLL 直接绘制现有三个 HWND，不生成 TTPlayer `Skin.xml`，不替换音频引擎。歌词和选项界面继续使用 TTPlayer 的原生实现。

### 当前支持范围

已按控件分析修复主窗口／EQ／播放列表的主要绘制、三种折叠模式、滑块预览、列表多选与内部拖动、光标、资源回退和双倍尺寸。

功能优先复用千千静听：五组列表按钮打开原生添加／删除／编辑／排序／列表菜单；EQ 使用原生参数与预设；可视化使用千千已有渲染器。完整映射、验证与未支持项见 **[修复与原生功能替代表](docs/NATIVE_FUNCTION_MAPPING.md)**。

Auto 按歌曲自动装载 EQ、Winamp 视频／TV、Winamp 插件和现代 WAL 运行时没有等价原生替代，未伪装为其他功能。通用对话框与歌词继续使用所选千千原生皮肤。

随机按钮映射到 TTPlayer 随机模式（再次点击恢复顺序模式）；重复按钮在列表循环与顺序模式之间切换，已有单曲循环也显示为选中。皮肤使用宿主已有的循环与随机索引策略。

两个 EXE 与 DLL 需要配套更新才能使用新增功能。两版共用同一份 DLL；旧 v1 宿主仍可加载。窗口吸附依据见 [窗口吸附与分离](docs/WINDOW_DOCKING.md)。尚未声称所有 WSZ 变体、全部手势和像素完全一致，剩余边界已列入替代表。

## 构建

统一 x86 版本（兼容普通版和 XP／Win7 版宿主）：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32
cmake --build build --config Release --parallel 4
```

唯一分发产物：`build/Release/ttp_waskin.dll`。本仓库可单独检出并构建，无需同级 `rebuild`。构建始终使用 VC-LTL 5.3.1／YY-Thunks 1.2.2 旧系统工具链，首次配置时下载并校验固定 SHA-256；需要 Python 3，在链接后执行 XP、Win7 导入审计，不再提供普通／旧系统 DLL 的构建开关。接口不跨模块传递 CRT 内存或 STL 对象，因此同一份 DLL 可供两种运行库的宿主使用。第三方许可文本保存在仓库中，CMake 安装时还会附带文本说明。

可以安装至指定播放器目录：

```powershell
cmake --install build --config Release --prefix "D:/path/to/TTPlayer"
```

宿主接口定义见 [include/ttp_skin_plugin.h](include/ttp_skin_plugin.h)。目前宿主仓库保留相同内容的 SDK 头文件；变更 ABI 时需同步两端并升级版本。

### GitHub Actions 独立构建与发布

[Manual Waskin Build](.github/workflows/manual-build.yml) 仅构建本仓库的 x86 Release DLL，不构建播放器、不检出或运行测试。工作流进入默认分支后，在 Actions → Manual Waskin Build → Run workflow 中选择分支：

- 不勾选 **Release a Version**：只构建，下载 `ttp_waskin-版本号` artifact，保留 14 天，不创建 tag 或 Release。
- 勾选 **Release a Version**：构建成功后，独立的 **GitHub Release** 作业创建同名 tag 和 Release，发布 `ttp_waskin-版本号.zip`。tag 指向此次构建的提交，无需额外配置 token secret。
- 发布版本与重建版相同：按构建开始时的北京时间取 `yyyy.MM.dd`，当天首次例如 `2026.09.20`，再次为 `2026.09.20p1`、`2026.09.20p2`。读取全部 tag 和 Release（包括占用版本号的草稿），按数字递增；发布流程串行，已有版本不覆盖。仅构建时使用当天日期。

下载的 artifact ZIP 与 Release ZIP 均只含以下文件，可直接解压到播放器目录：

```text
AddIn/
  ttp_waskin.dll
SHA256SUMS.txt
```

`SHA256SUMS.txt` 记录 `AddIn/ttp_waskin.dll` 的 SHA-256。普通版和 XP／Win7 版使用同一包。工作流保留 XP／Win7 静态导入审计；该检查不替代旧系统上的实际运行验证。

## 本地验证

测试代码位于 `../rebuild/tests/waskin`，不进入此插件的构建目标、分发包或 Actions。

已验证的内容和限制见 [docs/WSZ_IMPLEMENTATION.md](docs/WSZ_IMPLEMENTATION.md)。本工程独立实现兼容逻辑；没有链接 `gen_ff`、Wasabi 或把 Winamp 默认皮肤素材嵌入 DLL。本地对照使用的 Winamp 资源仅保存在忽略的测试输出目录。

## 日期版本与 Release 体积优先构建

DLL 的文件版本和产品版本使用北京时间 `yyyy.MM.dd`，同日发布补丁使用 `pN`；
例如 `2026.10.06p1` 对应固定数字版本 `2026.10.6.1`。Actions 在编译前确定最终版本，
DLL、发行包和发布标签使用同一版本。各项目继续独立构建。

Release 的统一配置见 [cmake/size_release.cmake](cmake/size_release.cmake)：
`/O1 /Os /Gy /Gw /GF`、跨模块优化和链接去除未引用代码／折叠相同代码，关闭 Release 调试信息。
本项目经 `/Ob0`、`/Ob1`、`/Ob2` 对比，默认选择 `/Ob2`；
可用 `-DTTP_SIZE_INLINE_LEVEL=0|1|2` 重新测量不同内联策略。
保留正常浮点语义、异常处理及 VC-LTL／YY-Thunks 的 XP／Win7 兼容配置。
Actions 不编译、不运行测试；本次新增的测试仅位于本地 `rebuild/tests/dll_size_versions`，不进入发行包。

本地构建、补丁号分配及版本资源说明见 [日期版本构建](docs/BUILD_VERSION.md)。
