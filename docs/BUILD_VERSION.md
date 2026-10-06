# Waskin 日期版本构建

## 版本规则

- 普通构建使用北京时间 `yyyy.MM.dd`；发布时在本仓库独立分配当天补丁号 `p1`、`p2` 等。
- 分配器分页读取标签和 Release；草稿也占用版本号。发布从版本分配到上传共用并发组。
- DLL 的 `FileVersion`、`ProductVersion` 和 ZIP／Release 使用相同的完整字符串。
- 固定数字资源使用 `年,月,日,补丁号`：`2026.10.06p1` → `2026,10,6,1`。
- 补丁号限制为 1–65535，无补丁时为 0；拒绝无效日期和 `p0`。
- 版本选择优先级：显式参数、项目根目录 `BUILD_VERSION`、当前北京时间。`BUILD_VERSION` 不提交。

## 本地构建

```powershell
./build.ps1 -Package
./build.ps1 -Package -PackageVersion 2026.10.06p1
```

脚本在一次构建开始时确定版本，传给 CMake，并在打包前读取实际 DLL 验证。
包输出到 `build/Release/ttp_waskin-版本号.zip`。运行包只包含 `AddIn/ttp_waskin.dll` 和 `SHA256SUMS.txt`。

直接使用 CMake：

```powershell
cmake -S . -B build -G "Visual Studio 18 2026" -A Win32 -DTTP_WASKIN_BUILD_VERSION=2026.10.06p1
cmake --build build --config Release --parallel 4
```

`TTP_WASKIN_BUILD_VERSION` 缓存为空时，版本生成目标在每次编译时重新取日期，
因此长期复用同一个构建目录也会更新；显式固定版本则持续使用该值。
版本资源内容未变时不重写文件，避免无意义的重新链接。

## Actions 与体积

Actions 在编译前确定最终日期／补丁号，再传给构建和打包阶段。
不运行测试，发行包不包含测试、PDB、源码或许可证副本；许可证继续保留在仓库。
Release 使用 [size_release.cmake](../cmake/size_release.cmake) 的体积优先配置，
本项目当前内联默认 `/Ob2`；兼容运行库配置和 DLL 导出接口保持原样。

2026-10-06 本地验证：版本字符串与固定数字版本、非法日期／补丁上限、
模拟已有标签与草稿的补丁分配、Release 构建、发行包哈希和 XP／Win7 加载通过。
本次未触发远程 Actions，也未发布 Release。
