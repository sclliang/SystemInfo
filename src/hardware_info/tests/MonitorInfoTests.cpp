// Compile the private implementation in this test TU, without adding any
// hardware-query or EDID-parser entry point to the production public contract.
#include "../HardwareInfo.cpp"
#include <iostream>
#include <stdexcept>

using namespace ftc::hardware_info;
namespace
{
void Check(bool value, const char* reason)
{
    if (!value) throw std::runtime_error(reason);
}
std::vector<BYTE> BaseEdid()
{
    std::vector<BYTE> data(128U);
    const BYTE header[]{0, 255, 255, 255, 255, 255, 255, 0};
    std::copy(std::begin(header), std::end(header), data.begin());
    data[18] = 1;
    data[19] = 4;
    data[21] = 3;
    data[22] = 2;
    data[72 + 3] = 0xFC;
    const std::string name = "NE160QA1-M01\n";
    std::copy(name.begin(), name.end(), data.begin() + 72 + 5);
    return data;
}
void SetTiming(std::vector<BYTE>& data, std::size_t offset, unsigned width, unsigned height)
{
    data[offset] = 1; // Nonzero pixel clock identifies a real DTD.
    data[offset + 12] = static_cast<BYTE>(width & 0xFFU);
    data[offset + 13] = static_cast<BYTE>(height & 0xFFU);
    data[offset + 14] = static_cast<BYTE>(((width >> 8U) << 4U) | (height >> 8U));
}
void CheckEdidDimensions()
{
    auto data = BaseEdid();
    SetTiming(data, 54, 344, 215);
    auto details = MonitorDetailsFromEdid(data);
    Check(details.name == L"NE160QA1-M01", "EDID descriptor name lost");
    Check(details.widthMillimeters == 344 && details.heightMillimeters == 215,
        "DTD must override erroneous 3 x 2 cm base values");
    Check(FormatMonitorDetails(details) == L"NE160QA1-M01 (16.0 inch (34.4 x 21.5 cm))",
        "Millimeter conversion or diagonal incorrect");
    SetTiming(data, 54, 1200, 675);
    details = MonitorDetailsFromEdid(data);
    Check(details.widthMillimeters == 1200 && details.heightMillimeters == 675,
        "DTD 12-bit physical dimensions were truncated");
    SetTiming(data, 54, 0, 215);
    SetTiming(data, 90, 344, 215);
    details = MonitorDetailsFromEdid(data);
    Check(details.widthMillimeters == 344, "Unspecified first DTD prevented later valid timing");
    data = BaseEdid();
    details = MonitorDetailsFromEdid(data);
    Check(details.widthMillimeters == 0 && details.heightMillimeters == 0,
        "Coarse base size bypassed WMI fallback");
    data[21] = 0;
    details = MonitorDetailsFromEdid(data);
    Check(details.widthMillimeters == 0 && details.heightMillimeters == 0, "Aspect ratio treated as physical size");
    for (const std::size_t length : {0U, 8U, 54U, 127U})
    {
        auto truncated = BaseEdid();
        truncated.resize(length);
        const auto empty = MonitorDetailsFromEdid(truncated);
        Check(empty.name.empty() && empty.widthMillimeters == 0, "Truncated EDID accepted");
    }
    data = BaseEdid();
    data[0] = 1;
    Check(MonitorDetailsFromEdid(data).name.empty(), "Invalid EDID header accepted");
}
void CheckExtensions()
{
    auto data = BaseEdid();
    data[126] = 1;
    data.resize(256);
    data[128] = 0x02;
    data[130] = 4;
    SetTiming(data, 132, 344, 215);
    Check(MonitorDetailsFromEdid(data).widthMillimeters == 344, "CTA DTD not used");
    data[130] = 110;
    Check(MonitorDetailsFromEdid(data).widthMillimeters == 0, "Out-of-range CTA DTD accepted");
    data[130] = 4;
    data[128] = 0x70;
    Check(MonitorDetailsFromEdid(data).widthMillimeters == 0, "Unknown extension parsed as CTA");
    data[128] = 0x02;
    data[126] = 255;
    data.resize(255); // A partial extension must not be read.
    Check(MonitorDetailsFromEdid(data).widthMillimeters == 0, "Truncated extension accepted");
}
void CheckFallbackMatching()
{
    const std::wstring path = L"DISPLAY\\BOE0001\\5&ABC&0&UID1";
    const WmiRows names{
        {L"Unrelated display", L"DISPLAY\\OTHER\\UID_0", L"True"},
        {L"Inactive name", path + L"_0", L"False"},
        {L"WMI panel name", L"display\\boe0001\\5&abc&0&uid1_0", L"True"}};
    const WmiRows sizes{
        {L"DISPLAY\\OTHER\\UID_0", L"60", L"34", L"True"},
        {path + L"_0", L"80", L"45", L"False"},
        {path + L"_0", L"3", L"2", L"True"}};
    auto data = BaseEdid();
    SetTiming(data, 54, 344, 215);
    auto details = MonitorDetailsFromEdid(data);
    ApplyMonitorWmiFallback(details, path, names, sizes);
    Check(details.name == L"NE160QA1-M01" && details.widthMillimeters == 344,
        "WMI replaced EDID data");
    details = {};
    ApplyMonitorWmiFallback(details, path, names, sizes);
    Check(details.name == L"WMI panel name" && details.widthMillimeters == 30 &&
        details.heightMillimeters == 20, "Same-instance WMI fallback failed");
    details = {L"EDID name", 0, 0};
    ApplyMonitorWmiFallback(details, path, names, {{path + L"_0", L"34", L"21", L"True"}});
    Check(details.name == L"EDID name" && details.widthMillimeters == 340, "Per-field fallback failed");
    details = {};
    ApplyMonitorWmiFallback(details, L"DISPLAY\\MISSING\\UID", names, sizes);
    Check(details.name.empty() && details.widthMillimeters == 0, "Different monitors were mixed");
    ApplyMonitorWmiFallback(details, path, {},
        {{path + L"_0", L"0", L"21", L"True"}, {path + L"_0", L"256", L"21", L"True"}});
    Check(details.widthMillimeters == 0, "Invalid WMI dimensions accepted");
}
void CheckIdentityPaths()
{
    const std::wstring path = L"DISPLAY\\BOE0001\\5&ABC&0&UID1";
    Check(NormalizeDisplayRegistryInstancePath(
        L"\\\\?\\DISPLAY#BOE0001#5&ABC&0&UID1#{e6f07b5f-ee97-4a90-b076-33f57bf4eaa7}") == path,
        "GDI monitor interface not resolved to registry instance");
    Check(NormalizeDisplayRegistryInstancePath(path + L"_0", true) == path, "WMI suffix not removed");
    Check(NormalizeDisplayRegistryInstancePath(path + L"_0") == path + L"_0",
        "Real device instance suffix was removed");
    Check(NormalizeDisplayRegistryInstancePath(path + L"_SERIAL", true) == path + L"_SERIAL",
        "Nonnumeric serial suffix was removed");
    std::vector<std::wstring> paths;
    AddActiveMonitorInstancePath(paths, path);
    AddActiveMonitorInstancePath(paths, L"display\\boe0001\\5&abc&0&uid1_0", true);
    Check(paths.size() == 1, "Case-insensitive monitor identity deduplication failed");
}
}
int wmain(int argc, wchar_t** argv)
{
    try
    {
        if (argc > 1 && std::wstring_view(argv[1]) == L"--snapshot")
        {
            const auto snapshot = CollectSnapshot();
            for (const auto& row : snapshot.rows)
                if (row.label == L"显示器信息") std::wcout << row.value << L'\n';
            if (!snapshot.diagnostic.empty()) std::wcout << snapshot.diagnostic << L'\n';
            if (!snapshot.error.empty()) std::wcout << snapshot.error << L'\n';
            return snapshot.error.empty() ? 0 : 1;
        }
        CheckEdidDimensions();
        CheckExtensions();
        CheckFallbackMatching();
        CheckIdentityPaths();
        std::cout << "Hardware info monitor checks passed.\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
