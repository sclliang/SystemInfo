#include <windows.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
std::string ReadFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Check(input.good(), "Cannot read expected resource file");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string Resource(HMODULE module, int id, LPCWSTR type)
{
    const auto resource = FindResourceW(module, MAKEINTRESOURCEW(id), type);
    Check(resource != nullptr, "Host resource missing");
    const auto data = static_cast<const char*>(LockResource(LoadResource(module, resource)));
    const DWORD length = SizeofResource(module, resource);
    Check(data && length, "Host resource is empty");
    return {data, length};
}
template<typename T> T Read(const std::string& bytes, size_t offset)
{
    Check(offset + sizeof(T) <= bytes.size(), "Invalid ICO header");
    T value{};
    std::memcpy(&value, bytes.data() + offset, sizeof(value));
    return value;
}
void CheckIcon(HMODULE module, const std::filesystem::path& icon)
{
    const auto group = Resource(module, 101, RT_GROUP_ICON);
    const auto expected = ReadFile(icon);
    const auto count = Read<std::uint16_t>(expected, 4);
    Check(count == 8 && Read<std::uint16_t>(group, 4) == count, "Documents icon sizes missing");
    for (size_t index = 0; index < count; ++index)
    {
        const size_t actualEntry = 6 + index * 14, expectedEntry = 6 + index * 16;
        Check(group.size() >= actualEntry + 14 && expected.size() >= expectedEntry + 16, "Truncated icon group");
        Check(std::memcmp(group.data() + actualEntry, expected.data() + expectedEntry, 12) == 0, "Icon group attributes changed");
        const auto image = Resource(module, Read<std::uint16_t>(group, actualEntry + 12), RT_ICON);
        const auto length = Read<std::uint32_t>(expected, expectedEntry + 8);
        const auto offset = Read<std::uint32_t>(expected, expectedEntry + 12);
        Check(offset <= expected.size() && length <= expected.size() - offset, "Icon image exceeds file");
        Check(image.size() == length && std::memcmp(image.data(), expected.data() + offset, length) == 0, "Documents icon pixels changed");
    }
}
void CheckVersion(const wchar_t* executable)
{
    DWORD unused{};
    const DWORD length = GetFileVersionInfoSizeW(executable, &unused);
    Check(length != 0, "Version info missing");
    std::vector<char> bytes(length);
    Check(GetFileVersionInfoW(executable, 0, length, bytes.data()) != FALSE, "Cannot read version info");
    for (const auto& [key, expected] : {
        std::pair{L"Author", L"sclliang"}, std::pair{L"CompanyName", L"sixunited"},
        std::pair{L"LegalCopyright", L"Copyright (c) 2026 sixunited. Author: sclliang"},
        std::pair{L"FileVersion", L"3.0.0.0"}})
    {
        void* value{};
        UINT size{};
        const auto query = std::wstring(L"\\StringFileInfo\\040904B0\\") + key;
        Check(VerQueryValueW(bytes.data(), query.c_str(), &value, &size) && value && size, "Version string missing");
        Check(std::wstring(static_cast<wchar_t*>(value)) == expected, "Version string changed");
    }
}
}
int wmain(int count, wchar_t** arguments)
{
    if (count != 3) return 2;
    const auto module = LoadLibraryExW(arguments[1], nullptr, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE);
    if (!module) return 1;
    int result = 0;
    try
    {
        const std::filesystem::path resources(arguments[2]);
        CheckIcon(module, resources / L"SystemInfo.ico");
        const auto manifest = Resource(module, 1, RT_MANIFEST);
        Check(manifest.find("requireAdministrator") != std::string::npos && manifest.find("PerMonitorV2") != std::string::npos,
            "Administrator or DPI manifest missing");
        Check(Resource(module, 103, RT_RCDATA) == ReadFile(resources / L"eventlog" / L"report.html"), "Offline report template changed");
        CheckVersion(arguments[1]);
        std::cout << "Host icon, metadata, manifest and offline report checks passed.\n";
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; result = 1; }
    FreeLibrary(module);
    return result;
}
