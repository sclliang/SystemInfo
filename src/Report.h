#pragma once
#include <Windows.h>
#include "hardware_info/HardwareInfo.h"
#include <filesystem>

namespace system_info
{
std::wstring FormatReport(const ftc::hardware_info::Snapshot& snapshot);
bool WriteText(const std::filesystem::path& path, const std::wstring& text);
bool WriteReport(const std::filesystem::path& path, const ftc::hardware_info::Snapshot& snapshot);
bool CopyText(HWND owner, const std::wstring& text);
}
