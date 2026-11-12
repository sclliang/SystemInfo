# 电脑信息采集基线

复制日期：2026-11-05。参考程序：`D:/source/C++/FactoryTestCases`。

参考仓库中 HardwareInfo.cpp 最近一次提交：`7e4670231cb91c62da98eb1ef6dafc7ad52b1efd`。本次以实际工作区文件为准，原样复制 `.cpp`、`.h` 及 `tests/MonitorInfoTests.cpp`；未修改参考项目或 FluentUI。

| 文件 | SHA-256 |
| --- | --- |
| HardwareInfo.cpp | `1CDA584039667B55188256FB79F336BD5546F5A69E48E34E7FBD0209673684B4` |
| HardwareInfo.h | `F94E2AF2333F32AF1ACC0810DB558C3DEDBAC214FF418ED91BF04D73743D3102` |

界面以信息行展开，按硬件、网络、SSD、电池、固件、设备标识和 Windows 授权信息分区，不使用卡片。报告按参考程序的顺序输出原有 16 项，再附上电池信息与两项 Windows 授权信息，共 19 项。主板型号、BIOS、EC、主板 SN、系统 SN 和 UUID 优先由内嵌 AMIDEWIN 读取，缺失字段使用下表中的回退来源。详见 [AMI 固件优先采集](ami-firmware.md)。

| 字段 | 保留的获取方式 |
| --- | --- |
| 主板型号 | `Win32_BaseBoard.Product` |
| OS信息 | `Win32_OperatingSystem`，补充 CurrentVersion 注册表中的 DisplayVersion / ReleaseId、UBR |
| CPU型号 | `Win32_Processor.Name` |
| GPU型号 | SetupAPI 显示适配器与 PCI 标识、驱动注册表显存信息、`Win32_VideoController` 回退；保留原名称/厂商整理 |
| 显示器信息 | 当前显示器实例、注册表 EDID 名称和物理尺寸，回退 `WmiMonitorID`、`WmiMonitorBasicDisplayParams`、PnP / DesktopMonitor |
| 内存信息 | `GetSystemFirmwareTable` 的 SMBIOS Type 17 优先，回退 `Win32_PhysicalMemory`；保留容量、速率、厂商及组合规则 |
| SSD信息 | 3.0.2 使用 `Win32_DiskDrive` 读取名称和设备字节容量，结合 Windows 总线、PnP 父链和无机械寻道惩罚属性识别内置 SSD；排除 USB/UASP 等外接盘。已移除 SMART/Health、ATA、AtaSmartCore 和 PCIe 链路探测。|
| BIOS版本 | `Win32_BIOS` 的 Manufacturer、SMBIOSBIOSVersion、ReleaseDate |
| EC版本 | `Win32_BIOS` 的 EmbeddedControllerMajorVersion / MinorVersion |
| 主板SN | `Win32_BaseBoard.SerialNumber` |
| 系统SN | `Win32_BIOS.SerialNumber` |
| UUID | `Win32_ComputerSystemProduct.UUID` |
| 有线网卡 | `Win32_NetworkAdapter`，保留物理网卡判断、虚拟/无线/蓝牙过滤和 MAC 格式 |
| 无线网卡 | `Win32_NetworkAdapter`，保留无线名称判断和 MAC 格式 |
| 蓝牙地址 | `Win32_NetworkAdapter`，保留原蓝牙识别与物理属性条件 |
| TPM型号 | `ROOT/CIMV2/Security/MicrosoftTpm` 下的 `Win32_Tpm` |
| 电池信息 | `GetSystemPowerStatus` 提供系统供电、电量和预计续航。用 SetupAPI 枚举电池接口，按电池标签查询设备信息和实时容量。驱动提供设计容量和满充容量时计算健康度。参考 [Windows 电池枚举](https://learn.microsoft.com/en-us/windows/win32/power/enumerating-battery-devices) 和 [电池信息结构](https://learn.microsoft.com/en-us/windows/win32/power/battery-information-str)。 |
| Windows OEM 密钥 | `ROOT/CIMV2` 下 `SoftwareLicensingService.OA3xOriginalProductKey`；未提供密钥时显示 `N/A`。 |
| Windows 产品 ID | `ROOT/CIMV2` 下 `Win32_OperatingSystem.SerialNumber`；与 OEM 密钥分开读取和显示。 |

上表前的哈希记录初次复制时的基线。1.4 从基本采集工作线程移除了旧磁盘查询，改由 SSD 工作线程补齐同一行。容量按十进制 GB/TB 显示，NVMe 累计读写量按健康日志计数乘 512000 字节后换算为 GiB/TiB。ATA 主机读写计数采用参考模块的厂商归一化值。未取得的指标显示 `N/A`。

3.0.1 按用户要求将 SSD 信息简化为名称及标称容量：把设备字节容量换算为 GiB 后，选择可容纳该容量的常见二次幂档位，再显示为整数 GB/TB（1024 GB 档位显示为 1 TB）。例如 500.1 GB 显示 512 GB，1.02 TB 显示 1 TB。此规则用于统一容量档位显示；界面、剪贴板和文本报告均按“名称 · 标称容量”输出，每块内置 SSD 单独一行。采集仍沿用原有的设备识别与外接盘排除逻辑。

3.0.2 将主窗口启动和刷新中的 SSD 采集改为 Windows 基本信息查询；磁盘句柄使用零访问权限，只发送 `IOCTL_STORAGE_QUERY_PROPERTY` 的总线与寻道属性查询。日志收集功能的 Windows 存储健康诊断属于单独的诊断流程。

正式执行权限与参考程序一致，为 `requireAdministrator`。普通权限预览仅用于开发验证，不能据此宣称 TPM 管理员查询已通过实机验证。
