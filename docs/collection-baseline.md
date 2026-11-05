# 电脑信息采集基线

复制日期：2026-11-05。参考程序：`D:/source/C++/FactoryTestCases`。

参考仓库中 HardwareInfo.cpp 最近一次提交：`7e4670231cb91c62da98eb1ef6dafc7ad52b1efd`。本次以实际工作区文件为准，原样复制 `.cpp`、`.h` 及 `tests/MonitorInfoTests.cpp`；未修改参考项目或 FluentUI。

| 文件 | SHA-256 |
| --- | --- |
| HardwareInfo.cpp | `1CDA584039667B55188256FB79F336BD5546F5A69E48E34E7FBD0209673684B4` |
| HardwareInfo.h | `F94E2AF2333F32AF1ACC0810DB558C3DEDBAC214FF418ED91BF04D73743D3102` |

界面按九张卡片排布；采集结果和复制报告仍按参考程序原来的 16 项顺序输出。根据用户后续要求，主板型号、BIOS、EC、主板 SN、系统 SN、UUID 优先由内嵌 AMIDEWIN 读取，以下对应方式作为回退；其他字段保持下表方式。详见 [AMI 固件优先采集](ami-firmware.md)。

| 字段 | 保留的获取方式 |
| --- | --- |
| 主板型号 | `Win32_BaseBoard.Product` |
| OS信息 | `Win32_OperatingSystem`，补充 CurrentVersion 注册表中的 DisplayVersion / ReleaseId、UBR |
| CPU型号 | `Win32_Processor.Name` |
| GPU型号 | SetupAPI 显示适配器与 PCI 标识、驱动注册表显存信息、`Win32_VideoController` 回退；保留原名称/厂商整理 |
| 显示器信息 | 当前显示器实例、注册表 EDID 名称和物理尺寸，回退 `WmiMonitorID`、`WmiMonitorBasicDisplayParams`、PnP / DesktopMonitor |
| 内存信息 | `GetSystemFirmwareTable` 的 SMBIOS Type 17 优先，回退 `Win32_PhysicalMemory`；保留容量、速率、厂商及组合规则 |
| SSD信息 | 1.4 起独立采集。用 WMI 物理盘编号及 PnP 身份匹配 AtaSmartCore 的设备，结合 Windows Storage 总线和 PnP 父链排除外接 USB。用 NVMe SMART / Health 或 ATA SMART 取得各盘上电与读写计数，读取当前 PCIe 或 SATA 传输模式。|
| BIOS版本 | `Win32_BIOS` 的 Manufacturer、SMBIOSBIOSVersion、ReleaseDate |
| EC版本 | `Win32_BIOS` 的 EmbeddedControllerMajorVersion / MinorVersion |
| 主板SN | `Win32_BaseBoard.SerialNumber` |
| 系统SN | `Win32_BIOS.SerialNumber` |
| UUID | `Win32_ComputerSystemProduct.UUID` |
| 有线网卡 | `Win32_NetworkAdapter`，保留物理网卡判断、虚拟/无线/蓝牙过滤和 MAC 格式 |
| 无线网卡 | `Win32_NetworkAdapter`，保留无线名称判断和 MAC 格式 |
| 蓝牙地址 | `Win32_NetworkAdapter`，保留原蓝牙识别与物理属性条件 |
| TPM型号 | `ROOT/CIMV2/Security/MicrosoftTpm` 下的 `Win32_Tpm` |

上表前的哈希记录初次复制时的基线。1.4 从基本采集工作线程移除了旧磁盘查询，改由 SSD 工作线程补齐同一行。容量按十进制 GB/TB 显示，NVMe 累计读写量按健康日志计数乘 512000 字节后换算为 GiB/TiB。ATA 主机读写计数采用参考模块的厂商归一化值。未取得的指标显示 `N/A`。

正式执行权限与参考程序一致，为 `requireAdministrator`。普通权限预览仅用于开发验证，不能据此宣称 TPM 管理员查询已通过实机验证。
