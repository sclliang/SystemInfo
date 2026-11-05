#include "../src/power/BatteryDevice.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

using namespace system_info::power;
using detail::Describe;
using detail::Device;

namespace
{
void Check(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}
std::wstring Value(const Pack& pack, const wchar_t* key)
{
    const auto item = std::find_if(pack.fields.begin(), pack.fields.end(),
        [key](const auto& field) { return field.first == key; });
    return item == pack.fields.end() ? L"" : item->second;
}
}

int main()
{
    try
    {
        SYSTEM_POWER_STATUS status{};
        status.ACLineStatus = 1;
        status.BatteryFlag = 128;
        status.BatteryLifePercent = 100; // Some desktops return this with the no-battery flag.
        auto reading = Describe(&status, {});
        Check(reading.present == false && !reading.percent && reading.packs.empty(), "desktop");
        Check(reading.source == L"外部电源" && reading.state == L"未检测到系统电池", "external power");
        status.BatteryFlag = status.BatteryLifePercent = status.ACLineStatus = 255;
        Check(!Describe(&status, {}).present && !Describe(nullptr, {}).present, "unknown vs absent");

        Device first;
        first.name = L"Primary";
        first.info.DesignedCapacity = 50'000;
        first.info.FullChargedCapacity = 40'000;
        first.info.CycleCount = 150;
        first.status.emplace();
        first.status->PowerState = BATTERY_DISCHARGING;
        first.status->Capacity = 0;
        status.ACLineStatus = 0;
        status.BatteryLifeTime = 3690;
        reading = Describe(&status, {first});
        Check(reading.present == true && reading.percent == 0u && reading.state == L"正在放电", "valid zero");
        Check(reading.remaining == L"1 小时 1 分钟" && reading.source == L"电池供电", "runtime");
        Check(Value(reading.packs[0], L"设计容量") == L"50.00 Wh"
            && Value(reading.packs[0], L"健康度") == L"80.0%"
            && Value(reading.packs[0], L"循环次数") == L"150 次", "pack health");
        Check(FormatReport(reading).find(L"Primary") != std::wstring::npos, "copy/report");

        Device second = first;
        second.info.FullChargedCapacity = 20'000;
        second.status->Capacity = 15'000;
        first.status->Capacity = 30'000;
        reading = Describe(nullptr, {first, second});
        Check(reading.percent == 75u && reading.packs.size() == 2, "weighted multi-pack charge");
        first.info.Capabilities = BATTERY_CAPACITY_RELATIVE;
        Check(!Describe(nullptr, {first, second}).percent, "mixed capacity scales");
        first.info.CycleCount = 0;
        first.status->PowerState = BATTERY_POWER_ON_LINE | BATTERY_CHARGING;
        reading = Describe(nullptr, {first});
        Check(reading.percent == 75u && reading.remaining.empty() && reading.state == L"正在充电", "charging");
        Check(Value(reading.packs[0], L"满充容量") == L"40000 相对单位"
            && Value(reading.packs[0], L"循环次数") == L"未提供", "relative units and unsupported count");
        first.info.FullChargedCapacity = BATTERY_UNKNOWN_CAPACITY;
        first.status->Capacity = BATTERY_UNKNOWN_CAPACITY;
        reading = Describe(nullptr, {first});
        Check(!reading.percent && Value(reading.packs[0], L"健康度") == L"未提供", "unknown capacity");
        status.ACLineStatus = 1;
        status.BatteryFlag = 1;
        status.BatteryLifePercent = 80;
        reading = Describe(&status, {});
        Check(reading.percent == 80u && reading.state == L"已接电，未充电" && reading.remaining.empty(), "charge limit");
        status.BatteryLifePercent = 100;
        Check(Describe(&status, {}).state == L"已充满", "fully charged");
        return 0;
    }
    catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
