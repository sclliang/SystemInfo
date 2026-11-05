# AtaSmartCore 来源

- 上游项目：CrystalDiskInfo（hiyohiyo）
- 上游地址：https://github.com/hiyohiyo/CrystalDiskInfo
- 许可：MIT，见同目录 `LICENSE.txt`
- 本目录基线：从老 Win32 参考项目
  `D:\source\C++\FactoryTestCases\third_party\AtaSmartCore` 复制；参考项目提交
  `40f49cde0adeac37c226b3f37577981944e0a274`。
- 本项目用途：仅作为 `SystemInfoCheck` 测试项的模块私有静态源码，禁止其他测试项引用或链接。

本地基线包含为非 MFC 静态构建准备的 `stdafx.h` 兼容层，以及对每次 SMART
读取有效性的显式标记；这些行为用于避免把读取失败后的默认零值误判为有效数据。
