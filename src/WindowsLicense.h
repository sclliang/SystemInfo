#pragma once
#include "hardware_info/HardwareInfo.h"

namespace system_info
{
struct WindowsLicenseInfo
{
    std::wstring oemKey, productId;
};
void ApplyWindowsLicenseInfo(ftc::hardware_info::Snapshot& snapshot, const WindowsLicenseInfo& info);
ftc::hardware_info::Snapshot CollectSystemInfo(std::stop_token token = {});
}
