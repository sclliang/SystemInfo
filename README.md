# SystemInfo

使用本地 `../FluentUI/sdk` 原生 Win32/C++ UI 库的电脑信息程序。默认浅色主题，初始窗口宽、高分别为鼠标所在屏幕工作区的 50%，并居中显示。

界面由主板与系统、处理器、显卡、显示器、内存、存储、固件与安全、设备标识、网络连接九张卡片组成。常规窗口采用三行三列，缩窄窗口时改为两列或一列，内容可滚动。支持后台读取、刷新 / F5、文字选择及右键复制、复制全部。显示和复制使用相同的采集结果。缺失信息显示 `N/A`，采集错误显示在底部。

点击主窗口的 **收集日志** 即开始后台采集，期间可查看电脑信息或取消。每次在程序旁的 `LOG/Collection_时间戳-进程号/` 中保存独立结果。完成后可点击 **打开报告** 查看离线 `manifest.html`，或点击 **打开目录** 取得原始文件。部分项目不可用时，报告会逐项说明原因。

一键收集沿用 EventLogCollector 1.4.0.0 的默认 20 项，包括五个标准 Windows 事件通道的全量留存 EVTX，以及转储、WER、设备与驱动、安装与维护日志、可靠性、存储健康、专用事件、故障时间线和电源报告等 15 类诊断资料。近期资料默认取 7 天，使用原项目的默认复制限额。采集源码位于 `src/eventlog`，离线 HTML 模板随 EXE 内嵌。

## 运行与编译

正式程序：`package/SystemInfo.exe`。FluentUI 和 MSVC 运行库均静态链接，无需安装 Qt 或附带 UI DLL。

与 FactoryTestCases 一样，正式 EXE 通过清单请求管理员权限，以运行 AMIDEWIN 的固件读取和访问 TPM 等受保护的 WMI 信息。程序只读取信息。

`AMIDEWINx64.EXE` 与配套 `amifldrv64.sys`、`amigendrv64.sys`、`amifldrv32.sys` 已原样嵌入主 EXE；运行时释放到受限的临时目录，采集结束后清理，无需在主程序旁放置这些文件。主板型号、BIOS、EC、主板 SN、系统 SN、UUID 优先采用 AMIDEWIN，有效数据缺失时逐项使用原采集结果。详见 [docs/ami-firmware.md](docs/ami-firmware.md)。

UUID、系统 SN、主板 SN 分别通过 `AMIDEWINx64.EXE /SU`、`/SS`、`/BS` 读取，只有读取失败的字段才回退。UUID 在界面、剪贴板和报告里均按来源顺序显示 32 个十六进制字符，省略工具的 `h` 后缀和分隔符。

存储卡片按物理盘列出内置 SSD 的名称、容量、协议与当前链路、上电时长、上电次数、累计读取量和写入量。SMART 使用 FactoryTestCases 的 SystemInfoCheck 模块所用的 AtaSmartCore、NVMe 健康日志和 ATA SMART 读取方式。程序结合总线、PnP 父设备和 SMART 设备类型排除 USB/UASP 外接盘。基本信息和 SSD 分别在后台采集，各自完成后立即显示。单项计数不可用时显示 `N/A`，有效零值显示为零。

在 Windows x64、Visual Studio 2026 C++ 工具链、CMake 环境中执行：

```powershell
.\build-release.ps1
# 自定义本地 FluentUI SDK：
.\build-release.ps1 -FluentUISdk 'D:\source\C++\FluentUI\sdk'
```

脚本编译 Release，运行显示器回归测试、AMIDEWIN 解析与回退测试、内嵌文件完整性检查，再输出到 `package`。发布时保留同目录中的第三方许可资料。可执行文件的版本资源和 CMake 元数据包含 Author `sclliang`、Company `sixunited`、Copyright `Copyright (c) 2026 sixunited. Author: sclliang`；应用界面不显示署名或版权。

## 采集基线

`src/hardware_info` 源自 FactoryTestCases 的同名模块，负责基本硬件信息和固件回退。`src/firmware` 负责 AMIDEWIN 优先采集。从 1.4 起，SSD 读取由 `src/storage` 独立完成，随程序静态编译的 AtaSmartCore 及其许可位于 `third_party/AtaSmartCore`。详细字段和来源见 [docs/collection-baseline.md](docs/collection-baseline.md)。

## 开发验证

可单独构建普通权限的预览版本，用于不触发 UAC 的界面自动化。它与正式版共用全部 C++ 源码，仅执行权限清单不同，TPM 等信息可能因权限不足显示 `N/A`。

```powershell
cmake -S . -B build/preview -G 'Visual Studio 18 2026' -A x64 -DSYSTEMINFO_REQUIRE_ADMIN=OFF -DBUILD_TESTING=OFF
cmake --build build/preview --config Release --parallel
```

诊断参数（路径支持中文和空格，输出目录须存在）：

- `--report <file.txt>`：不创建窗口，写入全部 16 项信息及诊断信息，使用 UTF-8 BOM。
- `--ami-report <file.txt>`：只验证内嵌 AMIDEWIN 的读取，写入原始输出、解析字段数、EC 和诊断信息，供管理员实机核对。
- `--smoke --snapshot <file.png>`：采集完成后导出实际 D2D 画面、窗口几何记录，并自动退出。
- `--smoke-logs --snapshot <file.png>`：通过窗口按钮的激活路径执行一次真实日志采集，保存常规 LOG 结果及完成状态的界面图，用于集成验证。
- `--size <width> <height>`：覆盖默认窗口大小，单位 DIP，仅用于布局检查。
- `--scale <1..2>`：应用缩放，用于检查较大字号，不改变 Windows 设置。

退出码：`0` 完成；`1` 采集整体失败或运行异常；`2` 参数无效；`3` 报告写入失败或界面验证超时。单项不可用通过 `N/A` 及诊断表达，不作为整体失败。

应用程序和窗口使用 `resources/six.ico`，图标取自 FactoryTestCases 共用的 `Six6.ico`。
