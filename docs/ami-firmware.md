# AMIDEWIN 固件优先采集

用户指定来源：`D:/常用工具/AMIDEWINx64.EXE` 及同目录驱动。四个文件原样保存在 `resources/ami`，以 `RCDATA` 201–204 编入 `SystemInfo.exe`。原厂文件未改写版本信息或签名，应用自身的版本资源独立维护。

| 文件 | SHA-256 |
| --- | --- |
| AMIDEWINx64.EXE | `AE8C870C64E72C1CF88F120D8D754BA60238DC2911C3CA36CC2C16138C326798` |
| amifldrv64.sys | `E7CBFB16261DE1C7F009431D374D90E9EB049BA78246E38BC4C8B9E06F324B6F` |
| amigendrv64.sys | `FFC72F0BDE21BA20AA97BEE99D9E96870E5AA40CCE9884E44C612757F939494F` |
| amifldrv32.sys | `45045DD6C9F7CBCD438EF7B730703AB10EAED402D49E6EC83C074872636861E2` |

CMake 配置时验证源文件 SHA-256；CTest 从编译后的正式 EXE 读取四段资源，与源文件逐字节比较。当前签名检查结果：三份驱动为有效 AMI 签名；EXE 本身无 Authenticode 签名。

## 读取与回退

仅使用固定只读命令，不接受外部命令或固件写入参数：

```text
AMIDEWINx64.EXE /SU
AMIDEWINx64.EXE /SS
AMIDEWINx64.EXE /BS
AMIDEWINx64.EXE /ALL strings.txt
AMIDEWINx64.EXE /DUMPALL dump.txt
```

按上述顺序先分别调用 `/SU`、`/SS`、`/BS`，读取 UUID、系统 SN、主板 SN；这三个开关后不附加任何值或文件名，因此只读取，不写入固件。每项单独解析其成功返回行，任一项失败不影响另外两项。后续 `/ALL` 只补充主板型号和 BIOS 信息，不覆盖三个直接查询的结果；直接查询失败的那一项按原逻辑回退。

上述语义及输出格式依据用户提供的 5.27 工具自身内嵌帮助和格式字符串：`/ALL [FileName]` 输出 SMBIOS 字符串，`/DUMPALL [FileName]` 输出全部 SMBIOS 数据。批量读取显式指定结果文件，避免依赖屏幕输出和控制台翻页输入；解析 EC 时只读取 Type 0。文件名不是固件写入值。

| 目标字段 | 优先来源 | 缺失时 |
| --- | --- | --- |
| 主板型号 | `/ALL` 中 `/BP` 的 `R Done` 行，首个有效主板实例 | 原 `Win32_BaseBoard.Product` |
| BIOS版本 | `/IVN`、`/IV`、`/ID` 三项均完整，按原 vendor/version/date 格式组合 | 三项任一缺失时保留原完整 BIOS 行 |
| EC版本 | `/DUMPALL` 的 Type 0 中 Embedded Controller Firmware Major / Minor Release | 原 EC 查询 |
| 主板SN | 直接执行 `/BS` 的 `R Done` 行 | 原主板 SN 查询 |
| 系统SN | 直接执行 `/SS` 的 `R Done` 行 | 原 BIOS SerialNumber 查询 |
| UUID | 直接执行 `/SU` 的 `R Done` 行 | 原 ComputerSystemProduct.UUID |

只接受成功读取行，跳过写入应答、Invalid、空值、无效 UUID 和不可用占位符。AMIDEWIN 的 `h` 或 `H` 后缀表示十六进制。有效 UUID 仅保留来源顺序中的 32 个十六进制字符，回退到 WMI 时也采用此显示格式。诊断报告的原始输出区保留工具返回的完整文本。全零、全 F 及长度错误的 AMIDEWIN UUID 会回退。EC 的十六进制字节转换为原来两位十进制格式，保留 `255.255` 行为。其他硬件信息及 TPM 继续使用原查询。

先执行 AMIDEWIN，再调用原采集模块生成基础信息，并用有效的 AMI 固件值覆盖对应行；3.0 WPF 界面继续使用同一采集核心，加上电池信息共 17 项。无管理员权限、资源释放失败、工具报错、超时或解析不到对应字段时，原结果仍然可用。工具错误保留在采集提示中。

## 进程与临时资源

- 正式程序清单请求管理员权限。普通权限预览直接回退，不尝试安装或加载驱动。
- 每次读取使用新的 GUID 临时目录，目录所有者为 Administrators，受保护 ACL 只允许 Administrators / SYSTEM。调用期间持有目录句柄，不再用禁止写入共享的文件句柄干扰原厂驱动加载器；不调用 PATH 中的同名工具。
- `CreateProcessW` 使用绝对路径、固定参数、隐藏窗口及受限的标准句柄继承列表。子进程先挂起，加入 `KILL_ON_JOB_CLOSE` 作业后才恢复执行。
- 每条命令上限 15 秒，输出上限 1 MiB；关闭窗口时请求取消并终止本次工具进程。超时、取消和非零退出仍保留有界控制台/文件输出、耗时、退出码。进程尚未退出时不卸载驱动。
- 原厂工具正常退出后，或进程被终止后，检查残留内核驱动服务。只停止并删除 `ImagePath` 严格指向当前用户临时根目录下 `SystemInfo-AMI-{有效GUID}` 中已知 AMI 驱动文件的服务，并等待停止。外部工具目录、其他程序目录和不匹配路径完全跳过。
- 新采集开始前，在确认没有 AMIDEWIN 进程运行后，用同样的范围规则回收旧版留下的驱动。通过命名互斥量串行化本程序的多实例 AMI 读取。
- 卸载后只删除四个嵌入文件和已知输出文件，再删除空目录。未知文件保留；文件/目录删除失败报告具体操作和 Win32 错误码，不再把所有清理错误笼统称为“文件被占用”。

## 验证边界

解析用例根据此二进制中的实际格式字符串构造，包含成功、无效、写入应答、空字段、多个主板、UUID 的 `h` 后缀、EC 字节解析及逐项回退；不把合成夹具描述为实机 AMIDEWIN 输出。子进程测试使用与正式程序相同的查询编排，验证三个只读参数、调用顺序、返回值优先级和每项独立回退。`--ami-report` 单列三个直接查询的解析值，并保留每条命令的原始输出。

普通权限运行原厂工具返回 `16 / Unable to load driver`，该权限下程序按原逻辑回退。2026-11-05 已通过正常 Windows 管理员启动流程验证 1.2.11.5 正式程序：三个直接查询均成功，7 个字符串字段及 EC 均可解析；最终采集中的 UUID 和两项 SN 与工具输出一致，退出后没有新增临时目录或残留驱动服务。详细记录见 `verification.md`；其他机型仍需实测。
