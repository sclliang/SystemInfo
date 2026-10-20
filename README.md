# EventLogCollector · 事件日志收集器

C++20 / 原生 Win32 + `D:\Code\FluentUI` SDK。版本 **1.0.0.0**，公司 **sixunited**，作者 **sclliang**。

## 一键采集

双击 `package/EventLogCollector.exe`，在 Windows 管理员权限提示中选择“是”，程序立即显示加载窗口，在后台依次导出以下五项日志：

| 通道 | 内容 |
|---|---|
| System | 系统日志 |
| Application | 应用程序日志 |
| Security | 安全日志 |
| Setup | 安装日志 |
| ForwardedEvents | 转发的事件 |

默认输出到 **EXE 所在目录的 `Log` 文件夹**，而不是快捷方式或终端的工作目录。目录自动创建；不需要手动选择通道或点击开始。

```text
package/
├── EventLogCollector.exe
└── Log/                         首次采集时自动创建
    ├── System_时间戳-进程号.evtx
    ├── Application_时间戳-进程号.evtx
    ├── Security_时间戳-进程号.evtx
    ├── Setup_时间戳-进程号.evtx
    └── ForwardedEvents_时间戳-进程号.evtx
```

文件名中的时间戳包含年月日、时分秒和毫秒。重复运行保留旧结果。采集使用 Windows 原生 `EvtExportLog`，保存各通道的全部事件；空通道也会导出有效 EVTX 文件。

五项全部成功后显示“采集成功”，并提供“打开日志文件夹”和“完成”按钮。某项失败时继续采集其他通道，结果窗口显示失败项目，可点击“查看详情”查看原因。源日志不会被清除或修改。

关闭采集中的窗口会请求停止，等待当前系统导出调用结束后退出；已完成的文件保留。正常失败或取消会清理当前临时文件，强制结束进程可能遗留临时文件。

## 运行环境与权限

目标系统：Windows 10 / Windows 11，x64。当前在本机 Windows 11 验证。FluentUI 和 C/C++ 运行库静态链接，字体与图标内嵌，发布程序是单 EXE，无须安装开发环境或 VC++ 运行库，无须 `bin/` 目录。

最终 EXE 的嵌入清单设置为 `requireAdministrator`。双击时由 Windows 请求管理员权限；如果拒绝 UAC，则程序不会开始采集。即使是管理员，设备的日志访问策略仍可能限制部分通道，程序会报告实际结果。

## 命令行

在**管理员终端**运行。无参数或 `gui` 打开自动采集窗口；`collect` 执行无界面的五通道采集。

```powershell
# 自动采集到 EXE 旁边的 Log 文件夹
.\package\EventLogCollector.exe collect | Out-String

# 指定输出目录，并输出逐通道的 JSON 结果
.\package\EventLogCollector.exe collect --output 'D:\Collected Logs' --json | Out-String

# 完整参数说明
.\package\EventLogCollector.exe --help | Out-String
```

`collect` 仅接受 `--output DIR` 和 `--json`。显式指定的相对目录以终端当前目录解析。JSON 包含输出目录、整体成功状态、耗时，以及每项的状态、文件路径、大小和错误原因。采集退出码：`0` 全部成功，`2` 参数错误，`4` 部分或全部采集失败，`5` 取消。

原有命令行功能继续保留：`channels` 枚举、`list` 查看、`export` 单通道导出（EVTX / CSV / JSON / XML）、`clear --channel NAME --yes` 显式清除。自动采集流程不会调用清除功能；详细参数见 `--help`。

这是 GUI 子系统 EXE。交互式 PowerShell 加 `| Out-String` 可等待执行并读取输出；自动化脚本应等待进程结束并检查退出码。重定向输出使用 UTF-8。

FluentUI 的第三方许可内嵌在 EXE 中，可通过 `--licenses` 查看。

## 构建

使用 Visual Studio 桌面 C++ 工具和 Windows SDK，打开 `EventLogCollector.sln`，选择 `Release | x64`。本机使用 VS 2026 / v145，不需要 MFC。

```powershell
.\build.ps1
.\build.ps1 -Configuration Debug
.\build.ps1 -FluentUISdk 'D:\Code\FluentUI\sdk'
```

SDK 默认位置是相邻目录 `..\FluentUI\sdk`。Release 构建自动发布到 `package/EventLogCollector.exe`。已安装的 FluentUI SDK 是 Release 静态库，因此 Debug 使用 `/Od /Zi`、`/MT` 和 `_ITERATOR_DEBUG_LEVEL=0` 保持 ABI 兼容。

Git 仓库保存源码、图标、构建脚本和测试；FluentUI SDK 需单独准备。`build/`、`package/`、`Log/` 及采集的 EVTX 文件不提交。克隆仓库后运行 `build.ps1` 生成发布程序。

主要源码：`Collector.*` 负责五通道采集与结果；`CollectorWindow.*` 负责加载和完成界面；`EventLogService.*` 封装 Windows 日志 API；`CommandLine.*` 负责命令行。

EXE 属性：CompanyName `sixunited`，FileDescription `Windows Event Log Collector`，ProductName / InternalName `EventLogCollector`，OriginalFilename `EventLogCollector.exe`，FileVersion / ProductVersion `1.0.0.0`。资源语言为 English (United States)，版权为 `Copyright (c) 2026 sixunited. Author: sclliang`。

## 验证

管理员 PowerShell 7 中测试最终发布程序：

```powershell
.\tests\CollectorSmoke.ps1
```

测试五通道采集、默认输出位置、独立工作目录和系统 PATH、EVTX 回读（包含 Windows `Get-WinEvent`）、重复采集保留、错误结果与参数校验。所有测试仅导出，不清除源日志。

开发会话没有管理员权限时，可生成独立的 `asInvoker` 测试程序验证核心及权限不足场景：

```powershell
.\tests\CollectorSmoke.ps1 -UseNonElevatedTestHost
.\tests\Smoke.ps1 -Executable .\build\x64\Release\EventLogCollector.CoreSmoke.exe
.\tests\UiSmoke.ps1
```

界面测试使用明确的加载、成功、失败样本和缩放场景，验证结果绑定并生成 D2D 截图至 `build/ui/`。样本截图不代表真实采集结果；真实导出由 CollectorSmoke 测试验证。测试程序只保留在 `build/`，不放入发布包；不会改变最终 EXE 的管理员权限要求。

2026-10-21 本机验证：Release / Debug 构建成功；采集测试 30 项、原有命令行测试 50 项及四种界面场景通过。最终 `package/EventLogCollector.exe` 已从独立工作目录以管理员权限启动，窗口显示“采集成功”，五个 EVTX 均通过 Windows `Get-WinEvent` 回读，其中 ForwardedEvents 为空日志。EXE 权限清单、版本属性及仅依赖 Windows 系统 DLL 的发布布局已核对。
