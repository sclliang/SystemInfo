#pragma once

#include "Battery.h"
#include <windows.h>
#include <winioctl.h>
#include <poclass.h>

namespace system_info::power::detail
{
struct Device
{
    std::wstring name;
    std::wstring manufacturer;
    BATTERY_INFORMATION info{};
    std::optional<BATTERY_STATUS> status;
};

Reading Describe(const SYSTEM_POWER_STATUS* system, const std::vector<Device>& devices);
}
