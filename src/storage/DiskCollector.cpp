#include "SsdInfo.h"
#include "Disk.h"
#include "Wmi.h"
#include <cfgmgr32.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <optional>
#include <utility>

namespace system_info::storage
{
namespace
{
class UniqueHandle final
{
public:
    explicit UniqueHandle(HANDLE value = INVALID_HANDLE_VALUE) noexcept : value_(value) {}
    ~UniqueHandle() { if (Valid()) CloseHandle(value_); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE Get() const noexcept { return value_; }
    bool Valid() const noexcept { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};

std::wstring Trim(std::wstring value)
{
    const auto nonSpace = [](wchar_t ch) { return std::iswspace(ch) == 0; };
    const auto first = std::find_if(value.begin(), value.end(), nonSpace);
    const auto last = std::find_if(value.rbegin(), value.rend(), nonSpace).base();
    return first < last ? std::wstring(first, last) : std::wstring{};
}

std::wstring Lower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towlower(ch)); });
    return value;
}

bool Equal(std::wstring_view left, std::wstring_view right)
{
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(),
        [](wchar_t a, wchar_t b) { return std::towlower(a) == std::towlower(b); });
}

bool ParseUnsigned(std::wstring_view text, std::uint64_t& value)
{
    if (!text.empty() && text.front() == L'+') text.remove_prefix(1);
    if (text.empty()) return false;
    std::uint64_t parsed{};
    for (wchar_t ch : text)
    {
        if (ch < L'0' || ch > L'9') return false;
        const auto digit = static_cast<std::uint64_t>(ch - L'0');
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10) return false;
        parsed = parsed * 10 + digit;
    }
    value = parsed;
    return true;
}

std::optional<int> DriveId(std::wstring_view text)
{
    std::uint64_t value{};
    if (!ParseUnsigned(text, value) || value > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))
        return {};
    return static_cast<int>(value);
}

UniqueHandle OpenPhysicalDrive(int id)
{
    if (id < 0) return UniqueHandle{};
    const auto path = L"\\\\.\\PhysicalDrive" + std::to_wstring(id);
    // Property queries need no disk read/write access. No health-log, ATA or
    // vendor command is sent through this handle.
    return UniqueHandle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
}

std::optional<STORAGE_BUS_TYPE> QueryBus(HANDLE handle)
{
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::array<unsigned char, 1024> buffer{};
    DWORD returned{};
    if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
        buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr)) return {};
    constexpr auto required = FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(STORAGE_BUS_TYPE);
    if (returned < required) return {};
    const auto* descriptor = reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    if (descriptor->Version < required || descriptor->Size < required) return {};
    return descriptor->BusType;
}

bool NoSeekPenalty(HANDLE handle)
{
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR descriptor{};
    DWORD returned{};
    return DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
        &descriptor, sizeof(descriptor), &returned, nullptr) && returned >= sizeof(descriptor) &&
        descriptor.Version >= sizeof(descriptor) && descriptor.Size >= sizeof(descriptor) &&
        descriptor.IncursSeekPenalty == FALSE;
}

bool HasExternalParent(const std::wstring& deviceId)
{
    if (deviceId.empty()) return false;
    std::wstring id = deviceId;
    DEVINST node{};
    if (CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    // Bound traversal even if a malformed device tree contains a cycle.
    for (unsigned depth = 0; depth < 64; ++depth)
    {
        DEVINST parent{};
        if (CM_Get_Parent(&parent, node, 0) != CR_SUCCESS) break;
        wchar_t name[MAX_DEVICE_ID_LEN]{};
        if (CM_Get_Device_IDW(parent, name, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS &&
            IsExternalStorage({}, name, BusTypeUnknown)) return true;
        node = parent;
    }
    return false;
}
}

bool IsExternalStorage(std::wstring_view interfaceName, std::wstring_view deviceId, STORAGE_BUS_TYPE bus)
{
    if (bus == BusTypeUsb || bus == BusType1394 || Equal(interfaceName, L"USB") ||
        Equal(interfaceName, L"1394")) return true;
    const auto id = Lower(std::wstring(deviceId));
    return id.starts_with(L"usb\\") || id.starts_with(L"usbstor\\") || id.starts_with(L"uaspstor\\") ||
        id.starts_with(L"usb4\\") || id.starts_with(L"sbp2\\") || id.starts_with(L"1394\\");
}

SsdResult CollectSsdInfo(std::stop_token token)
{
    WmiSession wmi;
    if (!wmi.IsReady())
        return {L"N/A", L"SSD 设备清单初始化失败 " + FormatHresult(wmi.InitializationError())};
    const auto inventory = wmi.Query(L"ROOT\\CIMV2",
        L"SELECT Index,Model,Size,InterfaceType,MediaType,PNPDeviceID FROM Win32_DiskDrive",
        {L"Index", L"Model", L"Size", L"InterfaceType", L"MediaType", L"PNPDeviceID"}, token);
    if (token.stop_requested()) return {};
    std::vector<Disk> disks;
    for (const auto& row : inventory.rows)
    {
        if (token.stop_requested()) return {};
        if (row.size() < 6) continue;
        const auto id = DriveId(row[0]);
        if (!id) continue;
        Disk disk;
        disk.physicalDriveId = *id;
        disk.model = Trim(row[1]);
        disk.pnpId = Trim(row[5]);
        std::uint64_t bytes{};
        if (ParseUnsigned(row[2], bytes) && bytes) disk.capacityBytes = bytes;
        if (IsExternalStorage(row[3], disk.pnpId, BusTypeUnknown) || HasExternalParent(disk.pnpId)) continue;
        const auto handle = OpenPhysicalDrive(*id);
        const auto bus = handle.Valid() ? QueryBus(handle.Get()).value_or(BusTypeUnknown) : BusTypeUnknown;
        if (IsExternalStorage(row[3], disk.pnpId, bus)) continue;
        if (bus == BusTypeVirtual || bus == BusTypeFileBackedVirtual || bus == BusTypeSpaces ||
            bus == BusTypeSd || bus == BusTypeMmc) continue;
        const auto model = Lower(disk.model);
        const bool nvme = bus == BusTypeNvme || Equal(row[3], L"NVMe") ||
            Lower(disk.pnpId).find(L"nvme") != std::wstring::npos || model.find(L"nvme") != std::wstring::npos;
        disk.solidState = nvme || model.find(L"ssd") != std::wstring::npos ||
            Lower(row[4]).find(L"solid state") != std::wstring::npos ||
            (handle.Valid() && NoSeekPenalty(handle.Get()));
        disks.push_back(std::move(disk));
    }
    if (token.stop_requested()) return {};
    std::stable_sort(disks.begin(), disks.end(),
        [](const Disk& a, const Disk& b) { return a.physicalDriveId < b.physicalDriveId; });
    auto value = FormatDisks(disks);
    if (value.empty()) value = inventory.completed ? L"未检测到内置 SSD" : L"N/A";
    std::wstring diagnostic;
    if (FAILED(inventory.error)) diagnostic = L"SSD 设备清单读取失败 " + FormatHresult(inventory.error);
    return {std::move(value), std::move(diagnostic)};
}
}

