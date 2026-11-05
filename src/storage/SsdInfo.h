#pragma once
#include "../hardware_info/HardwareInfo.h"

namespace system_info::storage
{
struct SsdResult
{
    std::wstring value, diagnostic;
};
SsdResult CollectSsdInfo(std::stop_token token = {});
void ApplySsdInfo(ftc::hardware_info::Snapshot& snapshot, const SsdResult& result);
}
