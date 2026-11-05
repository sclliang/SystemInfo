#include "SsdInfo.h"
#include "Disk.h"
#include "Smart.h"
#include "Wmi.h"
#include "stdafx.h"
#include "AtaSmart.h"

#include <cfgmgr32.h>
#include <devpropdef.h>
#include <windows.h>
#include <winioctl.h>
#ifndef DEVICE_TYPE
#define DEVICE_TYPE DWORD
#define SYSTEM_INFO_CHECK_DEFINED_DEVICE_TYPE
#endif
#include <ntdddisk.h>
#ifdef SYSTEM_INFO_CHECK_DEFINED_DEVICE_TYPE
#undef SYSTEM_INFO_CHECK_DEFINED_DEVICE_TYPE
#undef DEVICE_TYPE
#endif

#include <algorithm>
#include <array>
#include <cstring>
#include <cwctype>
#include <limits>
#include <optional>
#include <set>
#include <string_view>
#include <utility>
#include <vector>

namespace system_info::storage
{
namespace
{
class UniqueHandle final
{
public:
    explicit UniqueHandle(const HANDLE value = INVALID_HANDLE_VALUE) noexcept
        : value_(value)
    {
    }

    ~UniqueHandle()
    {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr)
        {
            CloseHandle(value_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;

    [[nodiscard]] HANDLE Get() const noexcept
    {
        return value_;
    }

    [[nodiscard]] bool IsValid() const noexcept
    {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }

private:
    HANDLE value_;
};

[[nodiscard]] std::wstring Trim(std::wstring value)
{
    const auto nonSpace = [](const wchar_t character) {
        return std::iswspace(character) == 0;
    };
    const auto first = std::find_if(value.begin(), value.end(), nonSpace);
    const auto last = std::find_if(value.rbegin(), value.rend(), nonSpace).base();
    return first < last ? std::wstring(first, last) : std::wstring{};
}

[[nodiscard]] std::wstring ToLower(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](const wchar_t character) {
        return static_cast<wchar_t>(std::towlower(character));
    });
    return value;
}

[[nodiscard]] bool EqualsIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    if (left.size() != right.size())
    {
        return false;
    }
    for (std::size_t index = 0U; index < left.size(); ++index)
    {
        if (std::towlower(left[index]) != std::towlower(right[index]))
        {
            return false;
        }
    }
    return true;
}

bool ParseUnsigned64(const std::wstring_view text, std::uint64_t& value) noexcept
{
    std::size_t index = 0U;
    if (!text.empty() && text.front() == L'+')
    {
        index = 1U;
    }
    if (index == text.size())
    {
        return false;
    }

    std::uint64_t parsed = 0U;
    for (; index < text.size(); ++index)
    {
        if (text[index] < L'0' || text[index] > L'9')
        {
            return false;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(text[index] - L'0');
        if (parsed > ((std::numeric_limits<std::uint64_t>::max)() - digit) / 10U)
        {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    value = parsed;
    return true;
}

[[nodiscard]] std::optional<int> ParsePhysicalDriveId(
    const std::wstring_view text) noexcept
{
    std::uint64_t parsed = 0U;
    if (!ParseUnsigned64(text, parsed) ||
        parsed > static_cast<std::uint64_t>((std::numeric_limits<int>::max)()))
    {
        return std::nullopt;
    }
    return static_cast<int>(parsed);
}

[[nodiscard]] std::uint64_t LittleEndian64(
    const unsigned char* const data,
    const std::size_t count) noexcept
{
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < (std::min)(count, sizeof(value)); ++index)
    {
        value |= static_cast<std::uint64_t>(data[index]) << (index * 8U);
    }
    return value;
}

[[nodiscard]] long double AtaSectorsToGb(const std::uint64_t sectors) noexcept
{
    return static_cast<long double>(sectors) * 512.0L /
        (1024.0L * 1024.0L * 1024.0L);
}

[[nodiscard]] bool QueryNvmeSmart(
    const HANDLE diskHandle,
    Disk& disk) noexcept
{
    constexpr DWORD dataLength = 512U;
    const DWORD headerLength = FIELD_OFFSET(
        STORAGE_PROPERTY_QUERY, AdditionalParameters);
    const DWORD bufferSize = headerLength +
        static_cast<DWORD>(sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA)) + dataLength;
    std::vector<unsigned char> buffer(bufferSize);
    auto* const query = reinterpret_cast<STORAGE_PROPERTY_QUERY*>(buffer.data());
    query->PropertyId = StorageDeviceProtocolSpecificProperty;
    query->QueryType = PropertyStandardQuery;
    auto* const protocol = reinterpret_cast<STORAGE_PROTOCOL_SPECIFIC_DATA*>(
        query->AdditionalParameters);
    protocol->ProtocolType = ProtocolTypeNvme;
    protocol->DataType = NVMeDataTypeLogPage;
    protocol->ProtocolDataRequestValue = 2U;
    protocol->ProtocolDataRequestSubValue = 0U;
    protocol->ProtocolDataOffset = sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA);
    protocol->ProtocolDataLength = dataLength;
    std::fill(
        buffer.begin() + headerLength + sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA),
        buffer.end(),
        static_cast<unsigned char>(0xA5U));

    DWORD returned = 0U;
    if (!DeviceIoControl(
            diskHandle,
            IOCTL_STORAGE_QUERY_PROPERTY,
            buffer.data(),
            bufferSize,
            buffer.data(),
            bufferSize,
            &returned,
            nullptr) ||
        returned < sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR))
    {
        return false;
    }

    const auto* const descriptor =
        reinterpret_cast<const STORAGE_PROTOCOL_DATA_DESCRIPTOR*>(buffer.data());
    if (descriptor->Version != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR) ||
        descriptor->Size != sizeof(STORAGE_PROTOCOL_DATA_DESCRIPTOR))
    {
        return false;
    }
    const STORAGE_PROTOCOL_SPECIFIC_DATA& output = descriptor->ProtocolSpecificData;
    const std::size_t protocolBase =
        FIELD_OFFSET(STORAGE_PROTOCOL_DATA_DESCRIPTOR, ProtocolSpecificData);
    const std::size_t offset = output.ProtocolDataOffset;
    if (output.ProtocolDataLength < dataLength ||
        offset < sizeof(STORAGE_PROTOCOL_SPECIFIC_DATA) ||
        protocolBase + offset + dataLength > returned ||
        protocolBase + offset + dataLength > buffer.size())
    {
        return false;
    }

    const unsigned char* const smart =
        reinterpret_cast<const unsigned char*>(&output) + offset;
    if (std::all_of(smart, smart + dataLength, [](const unsigned char value) {
            return value == 0xA5U;
        }))
    {
        return false;
    }
    return DecodeNvmeHealthLog(
        true,
        std::span<const std::uint8_t>(smart, dataLength),
        disk);
}

[[nodiscard]] UniqueHandle OpenPhysicalDrive(
    const int physicalDriveId,
    DWORD& error) noexcept
{
    error = ERROR_INVALID_PARAMETER;
    if (physicalDriveId < 0)
    {
        return UniqueHandle{};
    }
    const std::wstring path =
        L"\\\\.\\PhysicalDrive" + std::to_wstring(physicalDriveId);
    HANDLE value = CreateFileW(
        path.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr);
    if (value == INVALID_HANDLE_VALUE)
    {
        value = CreateFileW(
            path.c_str(),
            GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr,
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            nullptr);
    }
    if (value == INVALID_HANDLE_VALUE)
    {
        error = GetLastError();
    }
    else
    {
        error = ERROR_SUCCESS;
    }
    return UniqueHandle(value);
}

[[nodiscard]] std::optional<STORAGE_BUS_TYPE> QueryStorageBusType(
    const HANDLE diskHandle) noexcept
{
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceProperty;
    query.QueryType = PropertyStandardQuery;
    std::array<unsigned char, 1024U> buffer{};
    DWORD returned = 0U;
    if (!DeviceIoControl(
            diskHandle,
            IOCTL_STORAGE_QUERY_PROPERTY,
            &query,
            sizeof(query),
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            &returned,
            nullptr))
    {
        return std::nullopt;
    }
    constexpr std::size_t required =
        FIELD_OFFSET(STORAGE_DEVICE_DESCRIPTOR, BusType) + sizeof(STORAGE_BUS_TYPE);
    if (returned < required)
    {
        return std::nullopt;
    }
    const auto* const descriptor =
        reinterpret_cast<const STORAGE_DEVICE_DESCRIPTOR*>(buffer.data());
    if (descriptor->Version < required || descriptor->Size < required)
    {
        return std::nullopt;
    }
    return descriptor->BusType;
}

[[nodiscard]] bool ReadAtaBuffer(
    const HANDLE diskHandle,
    const int physicalDriveId,
    const unsigned char command,
    const unsigned char feature,
    std::array<unsigned char, 512U>& output) noexcept
{
    SENDCMDINPARAMS input{};
    input.cBufferSize = static_cast<DWORD>(output.size());
    input.irDriveRegs.bFeaturesReg = feature;
    input.irDriveRegs.bSectorCountReg = 1U;
    input.irDriveRegs.bSectorNumberReg = 1U;
    input.irDriveRegs.bCylLowReg = feature != 0U ? SMART_CYL_LOW : 0U;
    input.irDriveRegs.bCylHighReg = feature != 0U ? SMART_CYL_HI : 0U;
    input.irDriveRegs.bDriveHeadReg = 0xA0U;
    input.irDriveRegs.bCommandReg = command;
    input.bDriveNumber = static_cast<unsigned char>(physicalDriveId);

    std::array<unsigned char, sizeof(SENDCMDOUTPARAMS) + 511U> raw{};
    const std::size_t dataOffset = FIELD_OFFSET(SENDCMDOUTPARAMS, bBuffer);
    std::fill_n(raw.begin() + dataOffset, output.size(), static_cast<unsigned char>(0xA5U));
    DWORD returned = 0U;
    if (!DeviceIoControl(
            diskHandle,
            SMART_RCV_DRIVE_DATA,
            &input,
            sizeof(SENDCMDINPARAMS) - 1U,
            raw.data(),
            static_cast<DWORD>(raw.size()),
            &returned,
            nullptr) ||
        returned < dataOffset + output.size())
    {
        return false;
    }
    const auto* const result = reinterpret_cast<const SENDCMDOUTPARAMS*>(raw.data());
    if (result->DriverStatus.bDriverError != 0U ||
        std::all_of(
            result->bBuffer,
            result->bBuffer + output.size(),
            [](const unsigned char value) { return value == 0xA5U; }))
    {
        return false;
    }
    std::copy_n(result->bBuffer, output.size(), output.begin());
    return true;
}

[[nodiscard]] bool QueryAtaSmart(
    const HANDLE diskHandle,
    const int physicalDriveId,
    Disk& disk) noexcept
{
    std::array<unsigned char, 512U> smart{};
    if (!ReadAtaBuffer(
            diskHandle,
            physicalDriveId,
            SMART_CMD,
            READ_ATTRIBUTES,
            smart))
    {
        return false;
    }

    for (std::size_t offset = 2U; offset + 12U <= smart.size(); offset += 12U)
    {
        const unsigned char id = smart[offset];
        if (id == 0U)
        {
            continue;
        }
        const std::uint64_t raw = LittleEndian64(smart.data() + offset + 5U, 6U);
        if (id == 0x09U)
        {
            disk.powerHours = raw;
        }
        else if (id == 0x0CU)
        {
            disk.powerCounts = raw;
        }
        else if (id == 0xF1U)
        {
            disk.dataUnitsWrittenGb = AtaSectorsToGb(raw);
        }
        else if (id == 0xF2U)
        {
            disk.dataUnitsReadGb = AtaSectorsToGb(raw);
        }
    }

    std::array<unsigned char, 512U> identify{};
    if (ReadAtaBuffer(diskHandle, physicalDriveId, ID_CMD, 0U, identify))
    {
        const auto word = [&identify](const std::size_t index) {
            return static_cast<unsigned int>(identify[index * 2U]) |
                (static_cast<unsigned int>(identify[index * 2U + 1U]) << 8U);
        };
        disk.transferMode = DecodeAtaTransferMode(word(77U), word(88U));
        if (word(217U) == 1U) disk.solidState = true;
    }
    return true;
}

[[nodiscard]] std::optional<unsigned int> ReadPciLinkProperty(
    const DEVINST device,
    const unsigned long propertyId) noexcept
{
    constexpr GUID propertySet = {
        0x3AB22E31,
        0x8264,
        0x4B4E,
        {0x9A, 0xF5, 0xA8, 0xD2, 0xD8, 0xE3, 0x3E, 0x62}};
    const DEVPROPKEY key{propertySet, propertyId};
    std::array<unsigned char, sizeof(unsigned int)> buffer{};
    unsigned long size = static_cast<unsigned long>(buffer.size());
    DEVPROPTYPE type = 0U;
    if (CM_Get_DevNode_PropertyW(
            device,
            &key,
            &type,
            buffer.data(),
            &size,
            0U) != CR_SUCCESS ||
        size == 0U)
    {
        return std::nullopt;
    }
    unsigned int value = 0U;
    std::memcpy(&value, buffer.data(), (std::min)(static_cast<std::size_t>(size), sizeof(value)));
    return value == 0U ? std::nullopt : std::optional<unsigned int>{value};
}

[[nodiscard]] std::wstring CurrentPcieMode(const std::wstring& pnpDeviceId)
{
    if (pnpDeviceId.empty())
    {
        return {};
    }
    std::vector<wchar_t> deviceId(pnpDeviceId.begin(), pnpDeviceId.end());
    deviceId.push_back(L'\0');
    DEVINST device = 0U;
    if (CM_Locate_DevNodeW(&device, deviceId.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
    {
        return {};
    }
    for (unsigned int depth = 0U; depth < 16U; ++depth)
    {
        const auto speed = ReadPciLinkProperty(device, 9U);
        const auto width = ReadPciLinkProperty(device, 10U);
        if (speed.has_value() && width.has_value())
        {
            return L"PCIe " + std::to_wstring(*speed) + L".0 x" +
                std::to_wstring(*width);
        }
        DEVINST parent = 0U;
        if (CM_Get_Parent(&parent, device, 0U) != CR_SUCCESS)
        {
            break;
        }
        device = parent;
    }
    return {};
}

[[nodiscard]] std::optional<std::uint64_t> AtaSmartPowerHours(
    CAtaSmart& source,
    const CAtaSmart::ATA_SMART_INFO& information) noexcept
{
    if (information.MeasuredPowerOnHours > 0)
    {
        return static_cast<std::uint64_t>(information.MeasuredPowerOnHours);
    }
    if (information.DetectedPowerOnHours > 0)
    {
        return static_cast<std::uint64_t>(information.DetectedPowerOnHours);
    }
    if (information.PowerOnRawValue > 0)
    {
        return static_cast<std::uint64_t>(source.GetPowerOnHours(
            static_cast<DWORD>(information.PowerOnRawValue),
            information.DetectedTimeUnitType));
    }
    return information.PowerOnRawValue == 0
        ? std::optional<std::uint64_t>{0U}
        : std::nullopt;
}

[[nodiscard]] bool IsAtaSmartSsd(
    const CAtaSmart::ATA_SMART_INFO& information) noexcept
{
    return information.IsSsd == TRUE || information.IsNVMe == TRUE ||
        information.DiskVendorId != CAtaSmart::HDD_GENERAL ||
        information.NominalMediaRotationRate == 1U;
}

} // namespace

bool IsExternalStorage(const std::wstring_view interfaceName, const std::wstring_view deviceId,
    const STORAGE_BUS_TYPE bus)
{
    if (bus == BusTypeUsb || bus == BusType1394 || EqualsIgnoreCase(interfaceName, L"USB") ||
        EqualsIgnoreCase(interfaceName, L"1394")) return true;
    const auto id = ToLower(std::wstring(deviceId));
    return id.starts_with(L"usb\\") || id.starts_with(L"usbstor\\") || id.starts_with(L"uaspstor\\") ||
        id.starts_with(L"usb4\\") || id.starts_with(L"sbp2\\") || id.starts_with(L"1394\\");
}

void RemoveExternalDisk(std::vector<Disk>& disks, int physicalDriveId, std::wstring_view pnpId)
{
    std::erase_if(disks, [&](const Disk& disk) {
        return (physicalDriveId >= 0 && disk.physicalDriveId == physicalDriveId) ||
            (!pnpId.empty() && EqualsIgnoreCase(disk.pnpId, pnpId));
    });
}

namespace
{
bool HasExternalParent(const std::wstring& deviceId)
{
    if (deviceId.empty()) return false;
    std::wstring id = deviceId;
    DEVINST node{};
    if (CM_Locate_DevNodeW(&node, id.data(), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return false;
    DEVINST parent{};
    while (CM_Get_Parent(&parent, node, 0) == CR_SUCCESS)
    {
        wchar_t name[MAX_DEVICE_ID_LEN]{};
        if (CM_Get_Device_IDW(parent, name, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS &&
            IsExternalStorage({}, name, BusTypeUnknown)) return true;
        node = parent;
    }
    return false;
}

bool NoSeekPenalty(HANDLE handle)
{
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    query.QueryType = PropertyStandardQuery;
    DEVICE_SEEK_PENALTY_DESCRIPTOR output{};
    DWORD returned{};
    return DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
        &output, sizeof(output), &returned, nullptr) && returned >= sizeof(output) &&
        output.Size >= sizeof(output) && output.IncursSeekPenalty == FALSE;
}

struct Inventory
{
    std::vector<Disk> disks;
    std::set<int> externalIds;
    std::set<std::wstring> externalModels;
    bool completed{};
    std::wstring diagnostic;
};

void ReadNativeDisks(Inventory& inventory, const WmiSession& wmi, std::stop_token token)
{
    const auto rows = wmi.Query(L"ROOT\\CIMV2",
        L"SELECT Index,Model,Size,InterfaceType,MediaType,PNPDeviceID FROM Win32_DiskDrive",
        {L"Index", L"Model", L"Size", L"InterfaceType", L"MediaType", L"PNPDeviceID"}, token);
    inventory.completed = rows.completed;
    if (FAILED(rows.error)) inventory.diagnostic = L"SSD 设备清单读取失败 " + FormatHresult(rows.error);
    for (const auto& row : rows.rows)
    {
        if (token.stop_requested()) return;
        if (row.size() < 6) continue;
        const auto id = ParsePhysicalDriveId(row[0]);
        if (!id) continue;
        Disk disk;
        disk.physicalDriveId = *id;
        disk.model = Trim(row[1]);
        std::uint64_t bytes{};
        if (ParseUnsigned64(row[2], bytes) && bytes) disk.capacityBytes = bytes;
        disk.pnpId = Trim(row[5]);
        const auto exclude = [&] {
            inventory.externalIds.insert(*id);
            inventory.externalModels.insert(ToLower(disk.model));
        };
        if (IsExternalStorage(row[3], disk.pnpId, BusTypeUnknown) || HasExternalParent(disk.pnpId))
        { exclude(); continue; }

        DWORD error{};
        const auto handle = OpenPhysicalDrive(*id, error);
        const auto bus = handle.IsValid() ? QueryStorageBusType(handle.Get()).value_or(BusTypeUnknown) : BusTypeUnknown;
        if (IsExternalStorage(row[3], disk.pnpId, bus)) { exclude(); continue; }
        if (bus == BusTypeVirtual || bus == BusTypeFileBackedVirtual || bus == BusTypeSpaces ||
            bus == BusTypeSd || bus == BusTypeMmc) continue;
        const auto model = ToLower(disk.model);
        disk.isNvme = bus == BusTypeNvme || EqualsIgnoreCase(row[3], L"NVMe") ||
            ToLower(disk.pnpId).find(L"nvme") != std::wstring::npos || model.find(L"nvme") != std::wstring::npos;
        disk.solidState = disk.isNvme || model.find(L"ssd") != std::wstring::npos ||
            ToLower(row[4]).find(L"solid state") != std::wstring::npos;
        if (bus == BusTypeSata) disk.protocol = L"SATA";
        else if (bus == BusTypeAta) disk.protocol = L"ATA";
        else if (bus == BusTypeSas) disk.protocol = L"SAS";
        else if (bus == BusTypeRAID) disk.protocol = L"RAID";
        if (disk.isNvme) disk.transferMode = CurrentPcieMode(disk.pnpId);
        if (handle.IsValid() && !token.stop_requested())
        {
            if (disk.isNvme) static_cast<void>(QueryNvmeSmart(handle.Get(), disk));
            else
            {
                disk.solidState = disk.solidState || NoSeekPenalty(handle.Get());
                static_cast<void>(QueryAtaSmart(handle.Get(), *id, disk));
            }
        }
        inventory.disks.push_back(std::move(disk));
    }
}

void ApplyCoreSmart(CAtaSmart& core, const CAtaSmart::ATA_SMART_INFO& info, Disk& disk)
{
    const auto flags = info.SmartValidityFlags;
    if (!(flags & CAtaSmart::SMART_VALID_SNAPSHOT)) return;
    if (info.IsNVMe)
    {
        constexpr DWORD counters = CAtaSmart::SMART_VALID_POWER_HOURS | CAtaSmart::SMART_VALID_POWER_COUNT |
            CAtaSmart::SMART_VALID_HOST_READS | CAtaSmart::SMART_VALID_HOST_WRITES;
        if ((flags & counters) == counters)
            static_cast<void>(DecodeNvmeHealthLog(true,
                std::span<const std::uint8_t>(info.SmartReadData, sizeof(info.SmartReadData)), disk));
        return;
    }
    if (flags & CAtaSmart::SMART_VALID_POWER_HOURS)
        if (const auto value = AtaSmartPowerHours(core, info)) disk.powerHours = value;
    if (flags & CAtaSmart::SMART_VALID_POWER_COUNT) disk.powerCounts = info.PowerOnCount;
    if (flags & CAtaSmart::SMART_VALID_HOST_READS)
        if (const auto value = AtaSmartHostGb(info.HostReads)) disk.dataUnitsReadGb = value;
    if (flags & CAtaSmart::SMART_VALID_HOST_WRITES)
        if (const auto value = AtaSmartHostGb(info.HostWrites)) disk.dataUnitsWrittenGb = value;
}

Disk* MatchCoreDisk(Inventory& inventory, int id, const std::wstring& pnp, const std::wstring& model)
{
    Disk* match = nullptr;
    for (auto& disk : inventory.disks)
    {
        if (id >= 0 && disk.physicalDriveId == id) return &disk;
        if (id < 0 && !pnp.empty() && EqualsIgnoreCase(disk.pnpId, pnp)) return &disk;
    }
    // A core-only RAID entry can lack a physical number. Only merge by model
    // when the native inventory identifies exactly one possible internal disk.
    if (id < 0 && !model.empty() && !inventory.externalModels.contains(ToLower(model)))
    {
        for (auto& disk : inventory.disks)
        {
            if (!EqualsIgnoreCase(disk.model, model)) continue;
            if (match) return nullptr;
            match = &disk;
        }
    }
    return match;
}

void ReadCoreDisks(Inventory& inventory, std::stop_token token)
{
    CAtaSmart core;
    BOOL changed = FALSE;
    core.Init(TRUE, TRUE, &changed, FALSE, FALSE, FALSE, TRUE, FALSE);
    if (core.vars.GetCount() == 0 && !token.stop_requested())
        core.Init(FALSE, TRUE, &changed, FALSE, FALSE, FALSE, TRUE, FALSE);
    for (int i = 0; i < core.vars.GetCount(); ++i)
    {
        if (token.stop_requested()) return;
        const auto& candidate = core.vars[i];
        const int id = candidate.PhysicalDriveId;
        const auto pnp = Trim(candidate.PnpDeviceId.GetString());
        const auto model = Trim(candidate.Model.GetString());
        if (inventory.externalIds.contains(id) || candidate.IsUasp ||
            candidate.InterfaceType == CAtaSmart::INTERFACE_TYPE_USB ||
            candidate.InterfaceType == CAtaSmart::INTERFACE_TYPE_IEEE1394 ||
            IsExternalStorage({}, pnp, BusTypeUnknown) || HasExternalParent(pnp))
        {
            if (id >= 0) inventory.externalIds.insert(id);
            inventory.externalModels.insert(ToLower(model));
            RemoveExternalDisk(inventory.disks, id, pnp);
            continue;
        }
        if (!IsAtaSmartSsd(candidate)) continue;
        Disk* disk = MatchCoreDisk(inventory, id, pnp, model);
        if (!disk)
        {
            // A successful Windows inventory is authoritative about which
            // physical disks are present. Never resurrect an excluded bridge.
            if (inventory.completed || id < 0) continue;
            DWORD openError{};
            const auto handle = OpenPhysicalDrive(id, openError);
            if (!handle.IsValid()) continue;
            const auto bus = QueryStorageBusType(handle.Get());
            if (!bus || IsExternalStorage({}, pnp, *bus)) continue;
            disk = &inventory.disks.emplace_back();
            disk->physicalDriveId = id;
            disk->pnpId = pnp;
        }
        static_cast<void>(core.UpdateSmartInfo(static_cast<DWORD>(i)));
        const auto& info = core.vars[i];
        disk->solidState = true;
        disk->isNvme = disk->isNvme || info.IsNVMe == TRUE;
        if (!model.empty()) disk->model = model;
        if (!disk->capacityBytes && info.TotalDiskSize)
            disk->capacityBytes = static_cast<std::uint64_t>(info.TotalDiskSize) * 1'000'000U;
        if (info.InterfaceType == CAtaSmart::INTERFACE_TYPE_SATA) disk->protocol = L"SATA";
        const auto mode = Trim(info.CurrentTransferMode.GetString());
        if (!mode.empty() && mode != L"----") disk->transferMode = mode;
        ApplyCoreSmart(core, info, *disk);
    }
}
} // namespace

SsdResult CollectSsdInfo(std::stop_token token)
{
    Inventory inventory;
    WmiSession wmi;
    if (wmi.IsReady()) ReadNativeDisks(inventory, wmi, token);
    else inventory.diagnostic = L"SSD 设备清单初始化失败 " + FormatHresult(wmi.InitializationError());
    if (token.stop_requested()) return {};
    try
    {
        if (!inventory.completed || !inventory.disks.empty()) ReadCoreDisks(inventory, token);
    }
    catch (...)
    {
        if (!inventory.diagnostic.empty()) inventory.diagnostic += L"\r\n";
        inventory.diagnostic += L"SSD SMART 扩展读取失败，已保留 Windows 取得的磁盘信息。";
    }
    if (token.stop_requested()) return {};
    std::stable_sort(inventory.disks.begin(), inventory.disks.end(),
        [](const Disk& a, const Disk& b) { return a.physicalDriveId < b.physicalDriveId; });
    auto value = FormatDisks(inventory.disks);
    if (value.empty()) value = inventory.completed ? L"未检测到内置 SSD" : L"N/A";
    return {std::move(value), std::move(inventory.diagnostic)};
}
} // namespace system_info::storage

