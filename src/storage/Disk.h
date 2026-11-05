#pragma once
#include <windows.h>
#include <winioctl.h>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace system_info::storage
{
struct Disk
{
    int physicalDriveId{-1};
    std::wstring model, pnpId, protocol, transferMode;
    std::optional<std::uint64_t> capacityBytes, powerHours, powerCounts;
    std::optional<long double> dataUnitsReadGb, dataUnitsWrittenGb;
    bool isNvme{}, solidState{};
};
bool IsExternalStorage(std::wstring_view interfaceName, std::wstring_view deviceId, STORAGE_BUS_TYPE bus);
void RemoveExternalDisk(std::vector<Disk>& disks, int physicalDriveId, std::wstring_view pnpId);
std::wstring FormatDisks(const std::vector<Disk>& disks);
}
