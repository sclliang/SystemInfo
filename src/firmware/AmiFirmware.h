#pragma once
#include "../hardware_info/HardwareInfo.h"
#include <map>
#include <string_view>

namespace system_info::firmware
{
// Only successful read rows are accepted. No write command is exposed.
struct AmiValues
{
    std::map<std::wstring, std::wstring> commands;
    std::wstring ecVersion;
};
AmiValues ParseAmiStrings(std::wstring_view output);
std::wstring ParseEcDump(std::wstring_view output);
void ApplyAmiValues(const AmiValues& values, ftc::hardware_info::Snapshot& snapshot);

struct AmiResult
{
    AmiValues values;
    std::wstring diagnostic;
    std::wstring evidence; // Explicit --ami-report diagnostics only; never shown in cards.
};
AmiResult ReadEmbeddedAmi(std::stop_token token);
ftc::hardware_info::Snapshot CollectComputerInfo(std::stop_token token = {});
}
