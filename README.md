# SystemInfo

Windows x64 电脑信息工具。2.1 使用 Tauri 2 桌面窗口、本地 HTML/CSS/JavaScript 和静态链接的 C++ 采集核心。浅色窗口使用 32 个逻辑像素高的自定义标题栏，支持拖动、双击最大化或还原。启动时窗口居中显示，宽和高各为鼠标所在显示器可用工作区的三分之二。

全部 17 项电脑信息在同一页展开。宽窗口分两栏阅读，窄窗口按顺序纵向排列。支持 F5 刷新、文字选择及复制全部。SSD 数据独立加载，各盘的容量、链路、上电数据和累计读写量分组显示。

电池与电源分区使用 Windows 系统电源状态和电池类驱动的只读接口，启动和刷新时更新。可显示供电来源、充放电状态、电量及系统估计的剩余续航，每块系统电池分别列出名称、容量、健康度和循环次数。健康度按满充容量除以设计容量计算。只提供相对容量的电池保留驱动单位。台式机按系统返回值显示供电情况。

点击主窗口的 **收集日志** 即开始后台采集，期间可查看电脑信息或取消。每次在程序旁的 `LOG/Collection_时间戳-进程号/` 中保存独立结果。完成后可点击 **打开报告** 查看离线 `manifest.html`，或点击 **打开目录** 取得原始文件。部分项目不可用时，报告会逐项说明原因。

一键收集沿用 EventLogCollector 1.4.0.0 的默认 20 项，包括五个标准 Windows 事件通道的全量留存 EVTX，以及转储、WER、设备与驱动、安装与维护日志、可靠性、存储健康、专用事件、故障时间线和电源报告等 15 类诊断资料。近期资料默认取 7 天，使用原项目的默认复制限额。采集源码位于 `src/eventlog`，离线 HTML 模板随 EXE 内嵌。

## 运行与编译

正式程序为 `package/SystemInfo.exe`。前端资源、SVG、原厂固件工具、驱动和报告模板均嵌入 EXE。Windows 图形界面使用 [Microsoft Edge WebView2 Runtime](https://v2.tauri.app/start/prerequisites/#webview2)。

与 FactoryTestCases 一样，正式 EXE 通过清单请求管理员权限，以运行 AMIDEWIN 的固件读取和访问 TPM 等受保护的 WMI 信息。程序只读取信息。

`AMIDEWINx64.EXE` 与配套 `amifldrv64.sys`、`amigendrv64.sys`、`amifldrv32.sys` 已原样嵌入主 EXE；运行时释放到受限的临时目录，采集结束后清理，无需在主程序旁放置这些文件。主板型号、BIOS、EC、主板 SN、系统 SN、UUID 优先采用 AMIDEWIN，有效数据缺失时逐项使用原采集结果。详见 [docs/ami-firmware.md](docs/ami-firmware.md)。

UUID、系统 SN、主板 SN 分别通过 `AMIDEWINx64.EXE /SU`、`/SS`、`/BS` 读取，只有读取失败的字段才回退。UUID 在界面、剪贴板和报告里均按来源顺序显示 32 个十六进制字符，省略工具的 `h` 后缀和分隔符。

存储分区按物理盘列出内置 SSD 的名称、容量、协议与当前链路、上电时长、上电次数、累计读取量和写入量。SMART 使用 FactoryTestCases 的 SystemInfoCheck 模块所用的 AtaSmartCore、NVMe 健康日志和 ATA SMART 读取方式。程序结合总线、PnP 父设备和 SMART 设备类型排除 USB/UASP 外接盘。基本信息和 SSD 分别在后台采集，各自完成后立即显示。单项计数不可用时显示 `N/A`，有效零值显示为零。

构建环境为 Windows x64、Visual Studio 2026 C++ 工具链（含 ATL）、Windows SDK、CMake 3.25+、Rust MSVC 1.92+ 和 Node.js 22+。Rust 依赖锁定在 `src-tauri/Cargo.lock`。前端是离线静态资源，Node.js 用于数据契约测试和整理第三方许可。

~~~powershell
.\build-release.ps1
~~~

构建脚本编译 C++ 采集库、链接 Tauri 2 正式程序，校验嵌入资源并运行回归测试，然后更新 `package`。发布元数据使用作者 `sclliang`、公司 `sixunited`、版权 `Copyright (c) 2026 sixunited. Author: sclliang`。

## 采集基线

`src/hardware_info` 源自 FactoryTestCases 的同名模块，负责基本硬件信息和固件回退。`src/firmware` 负责 AMIDEWIN 优先采集。从 1.4 起，SSD 读取由 `src/storage` 独立完成，随程序静态编译的 AtaSmartCore 及其许可位于 `third_party/AtaSmartCore`。详细字段和来源见 [docs/collection-baseline.md](docs/collection-baseline.md)。

## 开发验证

新版入口位于 `src-tauri/src/main.rs`，数据桥接见 `src/NativeBridge.cpp`，界面和 SVG 插画位于 `ui/`。Rust 只对本地应用窗口开放状态读取、窗口控制和指定操作。打开日志报告、目录时使用采集核心记录的路径。

诊断参数（路径支持中文和空格，输出目录须存在）：

- `--report <file.txt>`：不创建窗口，写入全部 17 项信息及诊断信息，使用 UTF-8 BOM。
- `--ami-report <file.txt>`：只验证内嵌 AMIDEWIN 的读取，写入原始输出、解析字段数、EC 和诊断信息，供管理员实机核对。
- `--smoke --snapshot <file.png>`：采集完成后通过 WebView2 导出页面 PNG 并退出。单独运行 `--smoke` 可在无窗口模式下验证 17 个字段和电池详情的数据链路。
- `--smoke-logs --snapshot <file.png>`：通过窗口按钮的激活路径执行一次真实日志采集，保存常规 LOG 结果及完成状态的界面图，用于集成验证。
- `--size <width> <height>`：覆盖默认窗口大小，单位 DIP，仅用于布局检查。
- `--scale <1..2>`：应用缩放，用于检查较大字号，不改变 Windows 设置。

退出码：`0` 完成；`1` 采集整体失败或运行异常；`2` 参数无效；`3` 报告写入失败或界面验证超时。单项不可用通过 `N/A` 及诊断表达，不作为整体失败。

应用程序和窗口使用 `resources/six.ico`，图标取自 FactoryTestCases 共用的 `Six6.ico`。
