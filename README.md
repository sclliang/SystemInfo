# EventLogCollector · 事件日志收集器

C++20 / 原生 Win32 控制台程序，Windows 10 / 11 x64，版本 **1.3.0.0**。

## 一键采集

双击 package/EventLogCollector.exe，在 Windows 管理员权限提示中选择“是”，程序在控制台窗口中立即开始收集事件日志和故障诊断信息，无需选择通道或点击开始。界面以彩色文字逐项显示结果（已保存、部分完成、未找到、已跳过、失败），底部显示进度条、当前项目和用时；按 Ctrl+C 可取消，已完成的文件会保留。采集结束后显示汇总、**日志目录**和总览报告路径，按任意键退出。

控制台按 Windows 显示语言选择文字：中文系统显示中文，其他语言系统显示英文，避免缺少中文字体的控制台出现乱码。英文模式下不显示采集核心的中文说明，详细原因见 manifest.html。

每次创建一个独立目录：**EXE 所在目录/LOG/Collection_时间戳-进程号/**。时间戳包含毫秒，同名目录自动追加序号；重复运行保留旧结果。命令行指定输出位置时，也在该位置下创建本次独立目录。

采集结束后双击本次目录中的 **manifest.html** 打开中文总览。系统信息、设备、驱动、时间线和各类索引均生成 HTML，支持本页全文搜索、状态/事件级别/设备问题筛选、每页 50 条记录和展开完整字段。时间线详情保留消息与原始 XML。报告内嵌样式、脚本与数据，无需联网；使用启用 JavaScript 的 Edge、Chrome 或 Firefox 查看。复制整个采集目录后，相对文件链接仍可使用。打印按钮打印当前筛选和当前分页中可见的内容。

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
| 电源与睡眠报告 | 默认保存电源状态、最后唤醒、请求文本，以及系统电源转换、电池和 Modern Standby 的适用 HTML 报告；--energy 额外生成现场能效 HTML |

原始五个通道始终导出全量留存事件。近期转储、WER、专用事件和时间线默认使用 **7 天**范围，支持 --days 1..365。时间线使用采集开始时固定的 UTC 上界，按事件写入时间排序；事件 41、6008 等可能在重启后补记，不能将其写入时间直接当成故障发生时间。

~~~text
package/
├── EventLogCollector.exe
└── LOG/
    └── Collection_20261021_103000_001-1234/
        ├── manifest.html
        ├── Events/
        │   ├── System.evtx
        │   ├── Application.evtx
        │   └── ...
        ├── CrashDumps/index.html
        ├── LiveKernelReports/index.html
        ├── WerReports/index.html
        ├── SystemInfo.html
        ├── Devices.html
        ├── Drivers.html
        ├── DumpConfiguration.html
        ├── DriverInstallation/index.html
        ├── DiagnosticEvents/index.html
        ├── Timeline.html
        └── PowerReports/
            ├── index.html
            ├── system-power.html
            ├── battery.html         （有电池时）
            ├── sleep-study.html     （支持 Modern Standby 时）
            ├── energy.html          （指定 --energy 时）
            └── *.txt                （状态、唤醒、请求及命令原始输出）
~~~

索引目录中按条件保存实际文件；没有找到证据时仍保存索引。电源报告默认开启，--no-power 可关闭且不生成 PowerReports 目录；旧参数 --power 仍兼容。

系统电源转换报告使用 `/systempowerreport`，不调用本机新版 Windows 已标记弃用的 `/systemsleepdiagnostics`。历史 HTML 跟随 --days（默认 7 天）；系统电源转换和 SleepStudy 最多请求 28 天，电池报告使用指定天数。索引记录请求天数、实际传入的命令和适用性；实际历史取决于 Windows 已保留的数据，并不保证覆盖全部请求日期。无电池或不支持 Modern Standby 的设备跳过相应报告；权限不足和其他生成错误如实记录失败，不伪装为不支持。

`--energy` 在其他采集结束后额外观察系统 **60 秒**，生成 energy.html。它用于现场能效分析，不代表故障前的历史；建议电脑空闲且关闭其他程序和文档时使用。取消或超时会终止工具并清理未完成的 HTML，保留已完成报告、命令输出及索引。

## 状态、限制与诊断边界

控制台逐项显示全部 **16 项**，区分已保存、未找到、已跳过、部分完成、失败和取消，并显示简要原因；完整原因和路径见 manifest.html。

- 未找到可选转储、WER 报告或专用通道，不作为采集失败。
- 未启用通道、不适用的电源报告、未选择的大转储，以及达到文件复制限额的条目，记录为跳过并说明原因。
- 无权限、文件被占用、读取失败或达到扫描/时间线截断上限，明确报告失败或部分完成；继续尝试后面的项目。
- manifest.html 记录机器名、程序版本、开始/结束 UTC、时区、参数、每项结果、文件来源、路径、大小、修改时间及错误。清单写入失败也会导致整体失败。
- 取消会保留已完成文件，尝试保存包含取消状态的清单；当前系统导出/WMI 调用需要返回后才能继续响应。正常复制失败和取消清理当前临时文件，强制结束可能遗留临时文件。

默认转储复制限制为单文件 **64 MiB**、三类来源合计 **256 MiB**；--include-large-dumps 同时允许复制配置的系统内存转储，并提升为单文件 **4 GiB**、合计 **8 GiB**。超过限额仍记录元数据。WER 非转储报告与安装日志单文件限制 **32 MiB**，合计 **128 MiB**。每类最多复制 100 个文件，近期文件优先。每类最多扫描 10000 个目录项，递归深度为 5；不会跟随源目录或文件的重解析点。

时间线每个通道最多 5000 条、合计 20000 条，消息与 XML 合计最多 32M 个 UTF-16 字符；达到限额会明确标记部分完成，原始 EVTX 仍保留。WMI 每个查询的结果读取限时 20 秒，连接由 Windows WMI 的最大等待机制控制；常规电源命令限时 60 秒，energy 限时 120 秒，为采样和报告生成预留时间。

所有采集读取源数据，不清除事件、不修改转储/分页文件/电源配置、不启用日志通道、不安装或卸载驱动。只有用户显式调用原有 clear 命令才会清除指定通道。

这些资料用于人工或 WinDbg 等工具分析，不自动认定故障根因：Kernel-Power 41 不等同于电源损坏，没有 DMP 也不能排除蓝屏。设备状态是采集时的快照；DriverDate 是驱动包日期，不是实际安装时间；签名字段来自系统清单，未做独立密码学验证。QuickFixEngineering 清单不代表全部更新历史，磁盘 Status 不替代完整 SMART/NVMe 检查。当前运行账户的 WER 不包含其他用户的私有目录。

工具不会补回故障前未记录的温度、电压和功耗，也不包含后台持续监控或预先开启的 ETW 跟踪。

## 运行与命令行

C/C++ 运行库静态链接，图标内嵌，发布程序是单 EXE；无需安装开发环境、VC++ 运行库或 bin 目录。系统快照使用 Windows 自带 WMI，设备状态使用 SetupAPI/Configuration Manager；电源采集直接启动系统目录中的 powercfg.exe，不依赖 PATH 或工作目录。

最终 EXE 的清单为 requireAdministrator。双击由 Windows 请求管理员权限；拒绝 UAC 不会开始采集。管理员也可能受到设备策略或文件 ACL 限制，实际结果会记录到清单。

在管理员 PowerShell 中：

~~~powershell
# 使用默认的一键采集范围，输出清单
.\package\EventLogCollector.exe collect --json

# 在指定位置创建独立采集目录，近期范围改为 14 天
.\package\EventLogCollector.exe collect --output 'D:\Collected Logs' --days 14 --json

# 允许大转储，并追加现场能效采样（额外约 60 秒）
.\package\EventLogCollector.exe collect --include-large-dumps --energy --json

# 本次跳过全部电源报告
.\package\EventLogCollector.exe collect --no-power --json

.\package\EventLogCollector.exe --help
~~~

无参数（或兼容旧用法的 gui）执行一键采集并在结束后等待按键；collect 执行相同采集、显示相同进度，结束后直接退出，适合脚本。一键采集生成 HTML 报告，无独立 JSON 文件。命令行 --json 继续输出 schemaVersion 2 的完整结果；相同数据嵌入 manifest.html 的 report-data 元素中。保留 logs、diagnostics、时间与参数等字段；manifestPath 和各诊断项目 path 指向 HTML 文件。directory 指本次独立目录，rootDirectory 为父目录。旧采集目录保留原样。

退出码：0 按所选范围完成（允许可选证据不存在或明确跳过），2 参数错误，4 有失败/部分完成或清单无法写入，5 取消。

原有 channels、list、export、clear --channel NAME --yes 功能继续保留；单通道导出支持 EVTX / CSV / JSON / XML，过滤选项见 --help。

程序为控制台子系统，终端会等待执行结束；从非管理员终端启动时，Windows 会在新的管理员控制台窗口中运行。输出被重定向时不使用颜色和动画，也不等待按键。重定向输出与 JSON 为 UTF-8，powercfg 原始文本使用系统工具自己的编码。

## 构建与元数据

使用 Visual Studio 桌面 C++ 工具和 Windows SDK。打开 EventLogCollector.sln，选择 Release / x64，或运行：

~~~powershell
.\build.ps1
.\build.ps1 -Configuration Debug
~~~

Release 自动发布到 package/EventLogCollector.exe。Debug 使用 /Od /Zi、/MT 和 _ITERATOR_DEBUG_LEVEL=0。

可执行文件和项目元数据：Author/Authors 为 sclliang，CompanyName/Company 为 sixunited，版权为 Copyright (c) 2026 sixunited. Author: sclliang，FileVersion / ProductVersion 为 1.3.0.0。

主要源码：Collector.* 编排采集与清单；HtmlReport.* 和 assets/report.html 生成内嵌数据的离线报告；Diagnostics.* 负责文件、专用事件、时间线和电源工具；SystemSnapshot.cpp 负责 WMI、设备与注册表；ConsoleUi.* 负责控制台进度与汇总；EventLogService.* 封装事件 API；CommandLine.* 负责命令行。

Git 保存源码、图标、脚本和测试。build/、package/、LOG/ 及 EVTX 不提交。

## 验证

~~~powershell
# 在管理员 PowerShell 中验证最终发布程序
.\tests\CollectorSmoke.ps1

# 无管理员权限时，单独构建 asInvoker 测试程序，验证实际读取和权限失败
.\tests\CollectorSmoke.ps1 -UseNonElevatedTestHost
.\tests\Smoke.ps1 -Executable .\build\x64\Release\EventLogCollector.CoreSmoke.exe

# 隔离文件样本：时间、数量/大小限制、锁定文件、重解析点、取消和写入失败
.\tests\DiagnosticsSmoke.ps1

# 验证本机默认电源 HTML 及其权限/不适用结果
.\tests\PowerSmoke.ps1
.\tests\PowerSmoke.ps1 -IncludeEnergy

