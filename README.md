# SystemInfo

Windows x64 电脑信息与日志收集工具。3.0 使用 **WPF、.NET Framework 4.8 和 WPF-UI 4.3.0**，保留已有 C++ 硬件采集核心。默认固定为浅色，Windows 11 使用 WPF-UI 的云母（Mica）背景及内置标题栏。系统不支持云母时使用浅色背景。

正式程序：[package/SystemInfo.exe](package/SystemInfo.exe)。便携包：[package/SystemInfo-3.0.0-win-x64.zip](package/SystemInfo-3.0.0-win-x64.zip)。运行只需单个 EXE，目标电脑仍需安装 **.NET Framework 4.8**。

## 界面与功能

17 项信息始终在同一页面展开，支持文字选择、F5 刷新、复制全部及 Ctrl+Shift+C。宽窗口左栏依次为硬件配置、网络连接、SSD 存储；右栏为电池与电源、固件与安全、设备标识。窄窗口自动改为单栏，操作按钮换行。

窗口默认在鼠标所在显示器的工作区居中，宽和高分别取工作区的三分之二。标题栏高 32 DIP，使用 WPF-UI 的拖动、最大化、还原和窗口按钮。关闭按钮悬停为红底白色叉号。程序、标题栏和任务栏统一使用系统“文档”文件夹图标：从本机文档文件夹 `desktop.ini` 指定的 `imageres.dll,-112` 提取，保留 16–256 像素的八种原始尺寸。

硬件和 SSD 分别在后台采集，完成一部分即可显示。SSD 按物理盘列出型号、容量、协议、当前链路、上电时间、上电次数及累计读写量，排除 USB/UASP 外接盘。有效零值显示为零，不可用值显示 `N/A`。

电池分区保留 Windows 电源状态与电池类驱动的只读采集，包括供电来源、充放电状态、电量、续航，以及各块电池的容量、健康度和循环次数。台式机按实际返回值显示供电状态。

点击“收集日志”开始后台采集，期间可以继续阅读电脑信息或取消。每次结果保存在程序旁独立的 `LOG/Collection_时间戳-进程号/`，包含默认 20 项诊断内容、五个标准事件通道的 EVTX 和离线 `manifest.html`。完成或取消后可打开报告、打开目录或重新收集，已保存的结果会保留。

固件信息继续优先使用内嵌 AMIDEWIN，缺失字段单独回退到原采集来源。正式程序请求管理员权限以读取固件、TPM 和受保护日志，固定命令均为只读。UUID 保持来源字节顺序，以 32 个十六进制字符显示。详见 [AMI 固件采集](docs/ami-firmware.md) 和 [采集基线](docs/collection-baseline.md)。

## 构建与打包

已验证环境为 Windows x64、Visual Studio 2026（C++、ATL 和 .NET 桌面开发）、Windows SDK、.NET SDK、.NET Framework 4.8 开发包和 CMake 3.25+。

```powershell
.\build-release.ps1
```

脚本使用锁定的 NuGet 依赖，依次编译 C++ 核心和 WPF 程序，运行原生及托管回归，再验证最终单 EXE 的启动、常规窗口、窄窗口和 150% 缩放。随后更新 `package` 和 ZIP。可用 `-SkipSmoke` 跳过实机启动检查，常规发布不应跳过。

可直接用 Visual Studio 打开 [SystemInfo.sln](SystemInfo.sln)。WPF 项目会自动调用 CMake 构建当前配置所需的原生核心。

Costura.Fody/Fody 将托管依赖嵌入 EXE；C++ 核心也作为资源嵌入，使用静态 MSVC 运行库。运行时只把本次进程的原生核心释放到受 ACL 保护的独立临时目录，退出后卸载并清理。AMI 原厂工具、驱动、图标、管理员清单和 HTML 报告模板均嵌入主程序。

Release 构建使用 ConfuserEx 1.6.0。配置位于 [wpf/SystemInfo.crproj](wpf/SystemInfo.crproj)，仅混淆应用逻辑，保护配置排除 XAML/BAML、数据绑定、JSON 契约、原生委托和 Costura 加载器。发布目录不需要 DLL、配置文件、PDB 或符号映射。

作者、公司与版权只写入项目元数据和可执行文件版本资源，不在应用界面显示：

- Author: `sclliang`
- Company: `sixunited`
- Copyright: `Copyright (c) 2026 sixunited. Author: sclliang`

## 诊断与验证

路径支持中文和空格；输出文件的父目录须已存在。

- `--report <file.txt>`：不显示窗口，导出 17 项信息及诊断，使用 UTF-8 BOM。
- `--ami-report <file.txt>`：只验证内嵌 AMIDEWIN，导出原始输出、解析结果及诊断。
- `--smoke`：无窗口验证真实 C++/C# 数据链路、17 项字段及电池结构。
- `--smoke --snapshot <file.png>`：采集后检查真实 WPF 绑定、云母状态、关闭按钮样式和分区顺序，保存界面图后退出。
- `--smoke-logs --snapshot <file.png>`：走实际“收集日志”按钮事件，完成一次真实日志采集并保存完成状态界面。
- `--size <width> <height>`、`--scale <1..2>`：检查 DIP 窗口大小和应用缩放。

界面 PNG 是 WPF 内容与浅色主题后备背景的合成图，不包含桌面合成器的云母像素。旁边的 `.checks.txt` 记录真实 DWM 背景值、主题、关闭按钮配色、分区顺序及绑定检查结果。失败时写入 `.error.txt`。

退出码：`0` 完成，`1` 采集或运行失败，`2` 参数无效，`3` 报告/截图失败或验证超时。单项不可用保留 `N/A` 与诊断说明，不作为整体失败。

验证记录见 [docs/verification.md](docs/verification.md)。第三方许可见 [docs/THIRD_PARTY_NOTICES.md](docs/THIRD_PARTY_NOTICES.md)。
