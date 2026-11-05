#pragma once

#include <stop_token>
#include <string>
#include <vector>

namespace ftc::hardware_info
{

struct Row
{
    std::wstring label;
    std::wstring value;
};

struct Snapshot
{
    std::vector<Row> rows;
    // Non-fatal collection details. Successful rows remain displayable when
    // this field is populated.
    std::wstring diagnostic;
    // Fatal snapshot failure only (for example, CIMV2 could not be opened).
    std::wstring error;
};

// 同步采集一份只用于主窗口展示的硬件信息快照。rows 始终按旧 Win32
// 电脑信息区的固定顺序返回 16 项；单项失败时该项值为 N/A。
// 调用方必须在后台线程执行；测试模块不得调用此接口。
Snapshot CollectSnapshot(std::stop_token stopToken = {});

} // namespace ftc::hardware_info
