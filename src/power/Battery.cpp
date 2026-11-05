#include <windows.h>
#include <setupapi.h>
#include <initguid.h>
#include "BatteryDevice.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <format>
#include <memory>

namespace system_info::power
{
namespace
{
using detail::Device;

struct CloseDevice
{
    void operator()(HANDLE h) const noexcept { CloseHandle(h); }
};
struct CloseDeviceList
{
    void operator()(HDEVINFO h) const noexcept { SetupDiDestroyDeviceInfoList(h); }
};

template<class Input, class Output>
bool Query(HANDLE h, DWORD code, Input& input, Output& output)
{
    DWORD bytes{};
    return DeviceIoControl(h, code, &input, sizeof(input), &output, sizeof(output), &bytes, nullptr)
        && bytes >= sizeof(output);
}

std::wstring QueryText(HANDLE h, ULONG tag, BATTERY_QUERY_INFORMATION_LEVEL level)
{
    BATTERY_QUERY_INFORMATION query{tag, level, 0};
    std::array<wchar_t, MAX_BATTERY_STRING_SIZE> buffer{};
    DWORD bytes{};
    if (!DeviceIoControl(h, IOCTL_BATTERY_QUERY_INFORMATION, &query, sizeof(query),
        buffer.data(), sizeof(buffer), &bytes, nullptr)) return {};
    const auto end = buffer.begin() + std::min<std::size_t>(buffer.size(), bytes / sizeof(wchar_t));
    return {buffer.begin(), std::find(buffer.begin(), end, L'\0')};
}

std::optional<Device> ReadDevice(const wchar_t* path)
{
    const HANDLE raw = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return {};
    std::unique_ptr<void, CloseDevice> handle(raw);
    ULONG wait{}, tag{};
    if (!Query(raw, IOCTL_BATTERY_QUERY_TAG, wait, tag) || tag == BATTERY_TAG_INVALID) return {};
    BATTERY_QUERY_INFORMATION query{tag, BatteryInformation, 0};
    Device device;
    if (!Query(raw, IOCTL_BATTERY_QUERY_INFORMATION, query, device.info)
        || !(device.info.Capabilities & BATTERY_SYSTEM_BATTERY)
        || (device.info.Capabilities & BATTERY_IS_SHORT_TERM)) return {};
    device.name = QueryText(raw, tag, BatteryDeviceName);
    device.manufacturer = QueryText(raw, tag, BatteryManufactureName);
    BATTERY_WAIT_STATUS request{};
    request.BatteryTag = tag;
    BATTERY_STATUS status{};
    if (Query(raw, IOCTL_BATTERY_QUERY_STATUS, request, status)) device.status = status;
    return device;
}

std::vector<Device> Enumerate()
{
    std::vector<Device> devices;
    const auto raw = SetupDiGetClassDevsW(&GUID_DEVICE_BATTERY, nullptr, nullptr,
        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (raw == INVALID_HANDLE_VALUE) return devices;
    std::unique_ptr<void, CloseDeviceList> list(raw);
    SP_DEVICE_INTERFACE_DATA iface{};
    iface.cbSize = sizeof(iface);
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(raw, nullptr, &GUID_DEVICE_BATTERY, i, &iface); ++i)
    {
        DWORD bytes{};
        SetupDiGetDeviceInterfaceDetailW(raw, &iface, nullptr, 0, &bytes, nullptr);
        if (bytes < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W)) continue;
        std::vector<std::byte> storage(bytes);
        auto* entry = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(storage.data());
        entry->cbSize = sizeof(*entry);
        if (SetupDiGetDeviceInterfaceDetailW(raw, &iface, entry, bytes, nullptr, nullptr))
            if (auto device = ReadDevice(entry->DevicePath)) devices.push_back(std::move(*device));
    }
    return devices;
}

bool ValidFull(ULONG value) { return value != 0 && value != BATTERY_UNKNOWN_CAPACITY; }

std::optional<unsigned> Charge(const std::vector<Device>& devices)
{
    double current{}, full{};
    for (const auto& device : devices)
    {
        // Relative capacities from different batteries can use different scales.
        if (!device.status || device.status->Capacity == BATTERY_UNKNOWN_CAPACITY
            || !ValidFull(device.info.FullChargedCapacity)
            || (devices.size() > 1 && (device.info.Capabilities & BATTERY_CAPACITY_RELATIVE))) return {};
        current += device.status->Capacity;
        full += device.info.FullChargedCapacity;
    }
    if (full == 0) return {};
    return static_cast<unsigned>(std::lround(std::min(100.0, current * 100 / full)));
}

std::wstring Capacity(ULONG value, bool relative)
{
    if (value == BATTERY_UNKNOWN_CAPACITY) return L"未提供";
    return relative ? std::format(L"{} 相对单位", value) : std::format(L"{:.2f} Wh", value / 1000.0);
}

Pack DescribePack(const Device& device, std::size_t index)
{
    const auto& info = device.info;
    const bool relative = (info.Capabilities & BATTERY_CAPACITY_RELATIVE) != 0;
    Pack pack{device.name.empty() ? std::format(L"电池 {}", index + 1) : device.name, {}};
    if (!device.manufacturer.empty()) pack.fields.emplace_back(L"制造商", device.manufacturer);
    if (device.status && device.status->Capacity != BATTERY_UNKNOWN_CAPACITY)
        pack.fields.emplace_back(L"当前容量", Capacity(device.status->Capacity, relative));
    pack.fields.emplace_back(L"设计容量", ValidFull(info.DesignedCapacity) ? Capacity(info.DesignedCapacity, relative) : L"未提供");
    pack.fields.emplace_back(L"满充容量", ValidFull(info.FullChargedCapacity) ? Capacity(info.FullChargedCapacity, relative) : L"未提供");
    pack.fields.emplace_back(L"健康度", ValidFull(info.FullChargedCapacity) && ValidFull(info.DesignedCapacity)
        ? std::format(L"{:.1f}%", 100.0 * info.FullChargedCapacity / info.DesignedCapacity) : L"未提供");
    pack.fields.emplace_back(L"循环次数", info.CycleCount && info.CycleCount != 0xffffffffUL
        ? std::format(L"{} 次", info.CycleCount) : L"未提供");
    return pack;
}

std::wstring Duration(ULONG seconds)
{
    const auto minutes = seconds / 60;
    if (minutes >= 60) return std::format(L"{} 小时 {} 分钟", minutes / 60, minutes % 60);
    return seconds && !minutes ? L"少于 1 分钟" : std::format(L"{} 分钟", minutes);
}
}

Reading detail::Describe(const SYSTEM_POWER_STATUS* system, const std::vector<Device>& devices)
{
    Reading result;
    std::optional<bool> online;
    const bool flagsKnown = system && system->BatteryFlag != 255;
    if (flagsKnown) result.present = !(system->BatteryFlag & 128);
    if (system && system->ACLineStatus <= 1) online = system->ACLineStatus == 1;
    if (system && system->BatteryLifePercent <= 100 && result.present != false)
    {
        result.percent = system->BatteryLifePercent;
        result.present = true;
    }
    if (!devices.empty()) result.present = true;
    if (!result.percent) result.percent = Charge(devices);

    bool charging = flagsKnown && (system->BatteryFlag & 8);
    bool discharging = false, anyOnline = false, anyStatus = false;
    for (const auto& device : devices)
    {
        result.packs.push_back(DescribePack(device, result.packs.size()));
        if (!device.status) continue;
        anyStatus = true;
        const auto flags = device.status->PowerState;
        charging |= (flags & BATTERY_CHARGING) != 0;
        discharging |= (flags & BATTERY_DISCHARGING) != 0;
        anyOnline |= (flags & BATTERY_POWER_ON_LINE) != 0;
    }
    if (!online && anyStatus) online = anyOnline;
    result.source = online == true ? L"外部电源" : online == false
        ? (result.present == true ? L"电池供电" : L"外部电源未连接") : L"供电来源未知";
    if (result.present == false) result.state = L"未检测到系统电池";
    else if (!result.present) result.state = L"无法确定电池状态";
    else if (charging) result.state = L"正在充电";
    else if (discharging || online == false)
    {
        result.state = L"正在放电";
        if (system && system->BatteryLifeTime != BATTERY_UNKNOWN_TIME) result.remaining = Duration(system->BatteryLifeTime);
    }
    else if (online == true && result.percent == 100u) result.state = L"已充满";
    else if (online == true && (anyStatus || flagsKnown)) result.state = L"已接电，未充电";
    else result.state = L"充放电状态未知";
    return result;
}

Reading Read()
{
    SYSTEM_POWER_STATUS status{};
    const auto available = GetSystemPowerStatus(&status);
    return detail::Describe(available ? &status : nullptr, Enumerate());
}

std::wstring FormatReport(const Reading& reading)
{
    auto text = reading.source + L" · " + reading.state;
    if (reading.percent) text += std::format(L" · {}%", *reading.percent);
    if (!reading.remaining.empty()) text += L"\r\n预计续航：" + reading.remaining;
    for (const auto& pack : reading.packs)
    {
        text += L"\r\n" + pack.name;
        for (const auto& [name, value] : pack.fields) text += L"\r\n  " + name + L"：" + value;
    }
    return text;
}
}
