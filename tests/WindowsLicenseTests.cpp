#include "../src/WindowsLicense.h"
#include "../src/Report.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>

namespace
{
void Check(bool valid, const char* message) { if (!valid) throw std::runtime_error(message); }
std::wstring Value(const ftc::hardware_info::Snapshot& snapshot, const wchar_t* label)
{
    const auto row = std::find_if(snapshot.rows.begin(), snapshot.rows.end(),
        [&](const auto& item) { return item.label == label; });
    Check(row != snapshot.rows.end(), "License field missing");
    return row->value;
}
}
int main()
{
    try
    {
        ftc::hardware_info::Snapshot snapshot;
        snapshot.rows.push_back({L"OS信息", L"Test Windows"});
        const system_info::WindowsLicenseInfo example{L"TEST-OEM-KEY", L"TEST-PRODUCT-ID"};
        system_info::ApplyWindowsLicenseInfo(snapshot, example);
        Check(snapshot.rows.size() == 3, "License fields changed existing hardware rows");
        Check(Value(snapshot, L"Windows OEM 密钥") == example.oemKey &&
            Value(snapshot, L"Windows 产品 ID") == example.productId, "OEM key and product ID were swapped");
        const auto report = system_info::FormatReport(snapshot);
        Check(report.find(L"Windows OEM 密钥：\r\nTEST-OEM-KEY") != report.npos &&
            report.find(L"Windows 产品 ID：\r\nTEST-PRODUCT-ID") != report.npos, "Report omitted or confused the two fields");
        system_info::ApplyWindowsLicenseInfo(snapshot, {});
        Check(snapshot.rows.size() == 3 && Value(snapshot, L"Windows OEM 密钥") == L"N/A" &&
            Value(snapshot, L"Windows 产品 ID") == L"N/A", "Missing values reused an old key or duplicated fields");
        Check(Value(snapshot, L"OS信息") == L"Test Windows", "Existing OS information changed");
        std::cout << "OEM key, product ID, empty values and report checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
