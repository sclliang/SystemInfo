#include "WindowsLicense.h"
#include "firmware/AmiFirmware.h"
#include "storage/Wmi.h"
#include <algorithm>
#include <utility>

namespace system_info
{
void ApplyWindowsLicenseInfo(ftc::hardware_info::Snapshot& snapshot, const WindowsLicenseInfo& info)
{
    for (const auto& [label, value] : {
        std::pair{L"Windows OEM 密钥", info.oemKey}, std::pair{L"Windows 产品 ID", info.productId}})
    {
        const auto found = std::find_if(snapshot.rows.begin(), snapshot.rows.end(),
            [&](const auto& row) { return row.label == label; });
        const auto text = value.empty() ? L"N/A" : value;
        if (found == snapshot.rows.end()) snapshot.rows.push_back({label, text});
        else found->value = text;
    }
}

ftc::hardware_info::Snapshot CollectSystemInfo(std::stop_token token)
{
    auto snapshot = firmware::CollectComputerInfo(token);
    if (token.stop_requested()) return snapshot;
    WindowsLicenseInfo info;
    const auto diagnostic = [&](const std::wstring& message) {
        if (!snapshot.diagnostic.empty()) snapshot.diagnostic += L"\r\n";
        snapshot.diagnostic += message;
    };
    try
    {
        storage::WmiSession wmi;
        if (!wmi.IsReady()) diagnostic(L"Windows 授权信息初始化失败 " + storage::FormatHresult(wmi.InitializationError()));
        else
        {
            const auto read = [&](const wchar_t* query, const wchar_t* property, const wchar_t* label) {
                const auto result = wmi.Query(L"ROOT\\CIMV2", query, {property}, token);
                if (FAILED(result.error)) diagnostic(std::wstring(label) + L"读取失败 " + storage::FormatHresult(result.error));
                return !result.rows.empty() && !result.rows.front().empty() ? result.rows.front().front() : std::wstring{};
            };
            info.oemKey = read(L"SELECT OA3xOriginalProductKey FROM SoftwareLicensingService",
                L"OA3xOriginalProductKey", L"Windows OEM 密钥");
            if (!token.stop_requested()) info.productId = read(L"SELECT SerialNumber FROM Win32_OperatingSystem",
                L"SerialNumber", L"Windows 产品 ID");
        }
    }
    catch (...) { diagnostic(L"Windows 授权信息读取失败。"); }
    if (!token.stop_requested()) ApplyWindowsLicenseInfo(snapshot, info);
    return snapshot;
}
}
