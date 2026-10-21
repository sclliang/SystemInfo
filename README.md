# EventLogCollector · 事件日志收集器

C++20 / 原生 Win32 + FluentUI SDK，Windows 10 / 11 x64，版本 **1.1.0.0**。

## 一键采集

双击 package/EventLogCollector.exe，在 Windows 管理员权限提示中选择“是”，程序立即显示浅色加载窗口，在后台收集事件日志和故障诊断信息。无需选择通道或点击开始。

每次创建一个独立目录：**EXE 所在目录/Log/Collection_时间戳-进程号/**。时间戳包含毫秒，同名目录自动追加序号；重复运行保留旧结果。命令行指定输出位置时，也在该位置下创建本次独立目录。

| 类别 | 内容 |
|---|---|
| 标准事件日志 | System、Application、Security、Setup、ForwardedEvents 五个通道中当前仍保留的全部事件，保存原始 EVTX |
| 蓝屏转储 | 按 CrashControl 配置查找 MinidumpDir 与 DumpFile，缺失配置时使用 Windows 默认路径；默认复制近期小型转储，并记录系统内存转储位置、大小、修改时间和跳过原因 |
| 实时内核转储 | 递归查找 Windows/LiveKernelReports 的近期 DMP，覆盖 WATCHDOG 等组件目录 |
| Windows 故障报告 | ProgramData 及当前运行账户 LocalAppData 下 WER/ReportArchive、ReportQueue 的近期 WER、XML 和转储文件，保留来源与目录层级 |
| 系统与硬件 | Windows 内部版本号、部分补丁清单、机型、主板、BIOS、CPU、内存、显卡、磁盘、卷空间、当前电源状态 |
| 设备状态 | 设备实例 ID、硬件 ID、设备类、服务、设备节点状态、问题代码及关联的驱动版本、INF 等；包括系统保留的非在场设备 |
| 驱动清单 | 设备关联驱动的版本、供应商、INF、系统提供的签名字段，以及系统驱动服务的启动、运行状态 |
| 转储生成条件 | CrashControl、分页文件注册表值及 WMI 状态，Windows 所在卷剩余空间 |
| 设备安装历史 | setupapi.dev.log、setupapi.dev.log.old、setupapi.app.log，保留滚动历史，不按近期时间筛掉旧安装日志 |
| 专用诊断事件 | 设备、用户态驱动、代码完整性、Windows Update、启动与性能等八个候选通道；只读取存在且已启用的通道 |
| 故障时间线 | 从本次已导出的 System、Application 和专用通道提取近期警告、错误及关机、重启、WHEA、WER 等事件，保留来源、ID、UTC/本地时间、消息与原始 XML |
| 电源报告（可选） | 使用 --power 生成适用的电源状态、最后唤醒、请求、电池和 Modern Standby 报告 |

原始五个通道始终导出全量留存事件。近期转储、WER、专用事件和时间线默认使用 **7 天**范围，支持 --days 1..365。时间线使用采集开始时固定的 UTC 上界，按事件写入时间排序；事件 41、6008 等可能在重启后补记，不能将其写入时间直接当成故障发生时间。

~~~text
package/
├── EventLogCollector.exe
└── Log/
    └── Collection_20261021_103000_001-1234/
        ├── manifest.json
        ├── Events/
        │   ├── System.evtx
        │   ├── Application.evtx
        │   └── ...
        ├── CrashDumps/index.json
        ├── LiveKernelReports/index.json
        ├── WerReports/index.json
        ├── SystemInfo.json
        ├── Devices.json
        ├── Drivers.json
        ├── DumpConfiguration.json
        ├── DriverInstallation/index.json
        ├── DiagnosticEvents/index.json
        └── Timeline.json
~~~

索引目录中按条件保存实际文件；没有找到证据时仍保存索引。电源采集未开启时不生成 PowerReports 目录。

## 状态、限制与诊断边界

界面可滚动查看全部 **16 项**，区分已保存、未找到、已跳过、部分完成、失败和取消。展开“查看详情”可查看原因和路径。

- 未找到可选转储、WER 报告或专用通道，不作为采集失败。
- 未启用通道、不适用的电源报告、未选择的大转储，以及达到文件复制限额的条目，记录为跳过并说明原因。
- 无权限、文件被占用、读取失败或达到扫描/时间线截断上限，明确报告失败或部分完成；继续尝试后面的项目。
- manifest.json 记录机器名、程序版本、开始/结束 UTC、时区、参数、每项结果、文件来源、路径、大小、修改时间及错误。清单写入失败也会导致整体失败。
- 取消会保留已完成文件，尝试保存包含取消状态的清单；当前系统导出/WMI 调用需要返回后才能继续响应。正常复制失败和取消清理当前临时文件，强制结束可能遗留临时文件。

默认转储复制限制为单文件 **64 MiB**、三类来源合计 **256 MiB**；--include-large-dumps 同时允许复制配置的系统内存转储，并提升为单文件 **4 GiB**、合计 **8 GiB**。超过限额仍记录元数据。WER 非转储报告与安装日志单文件限制 **32 MiB**，合计 **128 MiB**。每类最多复制 100 个文件，近期文件优先。每类最多扫描 10000 个目录项，递归深度为 5；不会跟随源目录或文件的重解析点。

时间线每个通道最多 5000 条、合计 20000 条，消息与 XML 合计最多 32M 个 UTF-16 字符；达到限额会明确标记部分完成，原始 EVTX 仍保留。WMI 每个查询的结果读取限时 20 秒，连接由 Windows WMI 的最大等待机制控制；电源工具每条命令限时 60 秒。

所有采集读取源数据，不清除事件、不修改转储/分页文件/电源配置、不启用日志通道、不安装或卸载驱动。只有用户显式调用原有 clear 命令才会清除指定通道。

这些资料用于人工或 WinDbg 等工具分析，不自动认定故障根因：Kernel-Power 41 不等同于电源损坏，没有 DMP 也不能排除蓝屏。设备状态是采集时的快照；DriverDate 是驱动包日期，不是实际安装时间；签名字段来自系统清单，未做独立密码学验证。QuickFixEngineering 清单不代表全部更新历史，磁盘 Status 不替代完整 SMART/NVMe 检查。当前运行账户的 WER 不包含其他用户的私有目录。

工具不会补回故障前未记录的温度、电压和功耗，也不包含后台持续监控或预先开启的 ETW 跟踪。

## 运行与命令行

FluentUI 与 C/C++ 运行库静态链接，字体、图标内嵌，发布程序是单 EXE；无需安装开发环境、VC++ 运行库或 bin 目录。系统快照使用 Windows 自带 WMI，设备状态使用 SetupAPI/Configuration Manager；仅在指定 --power 时启动系统目录中的 powercfg.exe。

最终 EXE 的清单为 requireAdministrator。双击由 Windows 请求管理员权限；拒绝 UAC 不会开始采集。管理员也可能受到设备策略或文件 ACL 限制，实际结果会记录到清单。

在管理员 PowerShell 中：

~~~powershell
# 使用默认的一键采集范围，输出清单
.\package\EventLogCollector.exe collect --json | Out-String

# 在指定位置创建独立采集目录，近期范围改为 14 天
.\package\EventLogCollector.exe collect --output 'D:\Collected Logs' --days 14 --json | Out-String

# 允许大转储，并追加适用的电源报告
.\package\EventLogCollector.exe collect --include-large-dumps --power --json | Out-String

.\package\EventLogCollector.exe --help | Out-String
~~~

无参数或 gui 打开自动采集窗口；collect 执行相同采集而不显示窗口。JSON 清单为 schemaVersion 2，保留原 logs 数组，同时增加 diagnostics、时间与参数等字段；directory 现在指本次独立目录，rootDirectory 为父目录。

退出码：0 按所选范围完成（允许可选证据不存在或明确跳过），2 参数错误，4 有失败/部分完成或清单无法写入，5 取消。

原有 channels、list、export、clear --channel NAME --yes 功能继续保留；单通道导出支持 EVTX / CSV / JSON / XML，过滤选项见 --help。

程序为 GUI 子系统。交互式 PowerShell 加 | Out-String 可等待执行并读取输出；自动化脚本应等待进程结束并检查退出码。重定向输出与 JSON 为 UTF-8，powercfg 原始文本使用系统工具自己的编码。第三方许可通过 --licenses 查看。

## 构建与元数据

使用 Visual Studio 桌面 C++ 工具和 Windows SDK。打开 EventLogCollector.sln，选择 Release / x64，或运行：

~~~powershell
.\build.ps1
.\build.ps1 -Configuration Debug
.\build.ps1 -FluentUISdk 'D:\source\C++\FluentUI\sdk'
~~~

SDK 默认位置为相邻项目 ../FluentUI/sdk。Release 自动发布到 package/EventLogCollector.exe。已安装 SDK 是 Release 静态库，因此 Debug 使用 /Od /Zi、/MT 和 _ITERATOR_DEBUG_LEVEL=0 保持 ABI 兼容。

可执行文件和项目元数据：Author/Authors 为 sclliang，CompanyName/Company 为 sixunited，版权为 Copyright (c) 2026 sixunited. Author: sclliang，FileVersion / ProductVersion 为 1.1.0.0。界面不显示作者或版权归属，默认浅色且不跟随系统切换为深色。

主要源码：Collector.* 编排采集与清单；Diagnostics.* 负责文件、专用事件、时间线和电源工具；SystemSnapshot.cpp 负责 WMI、设备与注册表；CollectorWindow.* 负责界面；EventLogService.* 封装事件 API；CommandLine.* 负责命令行。

Git 保存源码、图标、脚本和测试；FluentUI SDK 另行准备。build/、package/、Log/ 及 EVTX 不提交。

## 验证

~~~powershell
# 在管理员 PowerShell 中验证最终发布程序
.\tests\CollectorSmoke.ps1

# 无管理员权限时，单独构建 asInvoker 测试程序，验证实际读取和权限失败
.\tests\CollectorSmoke.ps1 -UseNonElevatedTestHost
.\tests\Smoke.ps1 -Executable .\build\x64\Release\EventLogCollector.CoreSmoke.exe

# 隔离文件样本：时间、数量/大小限制、锁定文件、重解析点、取消和写入失败
.\tests\DiagnosticsSmoke.ps1

# 额外验证本机适用的 powercfg 报告及其权限/不适用结果
.\tests\PowerSmoke.ps1

# 加载、完成、失败、部分完成、取消、清单失败、诊断行滚动与缩放
.\tests\UiSmoke.ps1
~~~

文件样本仅在 build/diagnostic-fixtures 的独立目录创建，不修改系统配置。界面测试用确定的样本生成 D2D 截图到 build/ui；样本截图不代表真实机器的采集结果。asInvoker 测试 EXE 只保留在 build，最终发布 EXE 仍要求管理员权限。
