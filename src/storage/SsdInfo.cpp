#include "SsdInfo.h"
#include "Disk.h"
#include <algorithm>

namespace system_info::storage
{
namespace
{
std::wstring NominalCapacity(const std::optional<std::uint64_t> bytes)
{
    if (!bytes || !*bytes) return L"N/A";
    // The compact readout uses common capacity tiers, rather than a physical
    // device's decimal byte count: 500.x GB -> 512 GB, 1.02 TB -> 1 TB.
    // Divide before rounding so even the largest uint64_t cannot overflow.
    constexpr std::uint64_t gib = 1ULL << 30;
    const auto units = *bytes / gib + (*bytes % gib != 0);
    std::uint64_t nominalGb = 1;
    while (nominalGb < units) nominalGb <<= 1;
    return nominalGb < 1024 ? std::to_wstring(nominalGb) + L" GB"
        : std::to_wstring(nominalGb / 1024) + L" TB";
}
}

std::wstring FormatDisks(const std::vector<Disk>& disks)
{
    std::wstring text;
    for (const auto& disk : disks)
    {
        if (!disk.solidState) continue;
        if (!text.empty()) text += L"\r\n";
        text += disk.model.empty() ? L"未提供名称的 SSD" : disk.model;
        text += L" · " + NominalCapacity(disk.capacityBytes);
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
