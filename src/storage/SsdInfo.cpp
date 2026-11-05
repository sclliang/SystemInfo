#include "SsdInfo.h"
#include "Disk.h"
#include <algorithm>
#include <format>

namespace system_info::storage
{
namespace
{
std::wstring Capacity(const std::optional<std::uint64_t> bytes)
{
    if (!bytes || !*bytes) return L"N/A";
    if (*bytes >= 1'000'000'000'000ULL)
        return std::format(L"{:.2f} TB", *bytes / 1.0e12L);
    return std::format(L"{:.2f} GB", *bytes / 1.0e9L);
}

std::wstring Traffic(const std::optional<long double> gib)
{
    if (!gib) return L"N/A";
    return *gib >= 1024.0L ? std::format(L"{:.2f} TiB", *gib / 1024.0L) : std::format(L"{:.2f} GiB", *gib);
}

std::wstring Protocol(const Disk& disk)
{
    if (disk.isNvme)
        return disk.transferMode.empty() ? L"NVMe" : L"NVMe · " + disk.transferMode;
    if (!disk.transferMode.empty()) return disk.transferMode;
    return disk.protocol.empty() ? L"N/A" : disk.protocol;
}
}

std::wstring FormatDisks(const std::vector<Disk>& disks)
{
    std::wstring text;
    for (const auto& disk : disks)
    {
        if (!disk.solidState) continue;
        if (!text.empty()) text += L"\r\n\r\n";
        text += disk.model.empty() ? L"未提供名称的 SSD" : disk.model;
        text += L"\r\n" + Capacity(disk.capacityBytes) + L"  ·  " + Protocol(disk);
        text += L"\r\n上电 " + (disk.powerHours ? std::to_wstring(*disk.powerHours) + L" 小时" : L"N/A") +
            L"  ·  " + (disk.powerCounts ? std::to_wstring(*disk.powerCounts) + L" 次" : L"N/A 次");
        text += L"\r\n累计读取 " + Traffic(disk.dataUnitsReadGb) + L"  ·  写入 " + Traffic(disk.dataUnitsWrittenGb);
    }
    return text;
}

void ApplySsdInfo(ftc::hardware_info::Snapshot& snapshot, const SsdResult& result)
{
    const auto row = std::find_if(snapshot.rows.begin(), snapshot.rows.end(),
        [](const auto& item) { return item.label == L"SSD信息"; });
    if (row == snapshot.rows.end()) snapshot.rows.push_back({L"SSD信息", result.value});
    else row->value = result.value;
    if (!result.diagnostic.empty())
    {
        if (!snapshot.diagnostic.empty()) snapshot.diagnostic += L"\r\n";
        snapshot.diagnostic += result.diagnostic;
    }
}
}
