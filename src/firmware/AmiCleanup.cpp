#include <Windows.h>
#include <TlHelp32.h>
#include <objbase.h>
#include "AmiCleanup.h"
#include <algorithm>
#include <array>

namespace system_info::firmware::detail
{
namespace
{
class Service final
{
public:
    explicit Service(SC_HANDLE handle) : handle_(handle) {}
    ~Service() { if (handle_) CloseServiceHandle(handle_); }
    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    SC_HANDLE Get() const { return handle_; }
private:
    SC_HANDLE handle_;
};
bool Equal(std::wstring_view left, std::wstring_view right)
{
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()), right.data(),
        static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}
void Append(std::wstring& text, const std::wstring& line)
{
    if (!text.empty()) text += L"\r\n";
    text += line;
}
std::wstring LastError(const std::wstring& step)
{
    return step + L"失败，Win32=" + std::to_wstring(GetLastError());
}
std::wstring ImagePath(SC_HANDLE service)
{
    DWORD size = 0;
    QueryServiceConfigW(service, nullptr, 0, &size);
    if (!size) return {};
    std::vector<unsigned char> bytes(size);
    const auto config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(bytes.data());
    if (!QueryServiceConfigW(service, config, size, &size) || config->dwServiceType != SERVICE_KERNEL_DRIVER) return {};
    return config->lpBinaryPathName ? config->lpBinaryPathName : L"";
}
bool StopAndDelete(SC_HANDLE service, const std::wstring& name, std::wstring& error)
{
    SERVICE_STATUS_PROCESS status{};
    DWORD needed = 0;
    const auto query = [&] { return QueryServiceStatusEx(service, SC_STATUS_PROCESS_INFO,
        reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) != FALSE; };
    if (!query()) { Append(error, LastError(L"查询驱动 " + name)); return false; }
    if (status.dwCurrentState != SERVICE_STOPPED && status.dwCurrentState != SERVICE_STOP_PENDING)
    {
        SERVICE_STATUS ignored{};
        if (!ControlService(service, SERVICE_CONTROL_STOP, &ignored) && GetLastError() != ERROR_SERVICE_NOT_ACTIVE)
        { Append(error, LastError(L"停止本程序驱动 " + name)); return false; }
    }
    const auto deadline = GetTickCount64() + 5000;
    do
    {
        if (!query()) { Append(error, LastError(L"等待驱动停止 " + name)); return false; }
        if (status.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(50);
    } while (GetTickCount64() < deadline);
    if (status.dwCurrentState != SERVICE_STOPPED)
    { Append(error, L"驱动 " + name + L" 尚未停止，保留临时文件。"); return false; }
    if (!DeleteService(service) && GetLastError() != ERROR_SERVICE_MARKED_FOR_DELETE)
    { Append(error, LastError(L"移除本程序驱动服务 " + name)); return false; }
    return true;
}
}

std::optional<std::filesystem::path> OwnedDriverDirectory(
    std::wstring imagePath, const std::filesystem::path& temporaryRoot)
{
    if (imagePath.size() >= 2 && imagePath.front() == L'"' && imagePath.back() == L'"')
        imagePath = imagePath.substr(1, imagePath.size() - 2);
    if (imagePath.starts_with(L"\\??\\") || imagePath.starts_with(L"\\\\?\\")) imagePath.erase(0, 4);
    auto path = std::filesystem::path(imagePath).lexically_normal();
    path.make_preferred();
    const auto leaf = path.filename().wstring();
    if (!Equal(leaf, L"amigendrv64.sys") && !Equal(leaf, L"amifldrv64.sys") && !Equal(leaf, L"amifldrv32.sys")) return {};
    auto directory = path.parent_path();
    auto root = temporaryRoot.lexically_normal();
    root.make_preferred();
    if (root.filename().empty()) root = root.parent_path();
    if (!directory.is_absolute() || !Equal(directory.parent_path().wstring(), root.wstring())) return {};
    const auto name = directory.filename().wstring();
    constexpr std::wstring_view prefix = L"SystemInfo-AMI-";
    if (name.size() != prefix.size() + 38 || !Equal(name.substr(0, prefix.size()), prefix)) return {};
    GUID guid{};
    if (FAILED(CLSIDFromString(name.substr(prefix.size()).c_str(), &guid))) return {};
    return directory;
}

bool AnotherAmiProcessRunning()
{
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return true;
    PROCESSENTRY32W entry{sizeof(entry)};
    bool running = false;
    for (BOOL valid = Process32FirstW(snapshot, &entry); valid; valid = Process32NextW(snapshot, &entry))
        if (Equal(entry.szExeFile, L"AMIDEWINx64.EXE")) { running = true; break; }
    CloseHandle(snapshot);
    return running;
}

bool CleanupOwnedDrivers(const std::filesystem::path& onlyDirectory,
    std::vector<std::filesystem::path>& cleared, std::wstring& error)
{
    wchar_t temporary[32768]{};
    const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
    if (!length || length >= std::size(temporary)) { Append(error, LastError(L"获取驱动临时根目录")); return false; }
    Service manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT | SC_MANAGER_ENUMERATE_SERVICE));
    if (!manager.Get()) { Append(error, LastError(L"查询本程序驱动服务")); return false; }
    DWORD needed = 0, count = 0, resume = 0;
    if (!EnumServicesStatusExW(manager.Get(), SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL,
        nullptr, 0, &needed, &count, &resume, nullptr) && GetLastError() != ERROR_MORE_DATA)
    { Append(error, LastError(L"枚举驱动服务大小")); return false; }
    if (!needed) return true;
    std::vector<BYTE> bytes(needed);
    resume = 0;
    if (!EnumServicesStatusExW(manager.Get(), SC_ENUM_PROCESS_INFO, SERVICE_DRIVER, SERVICE_STATE_ALL,
        bytes.data(), static_cast<DWORD>(bytes.size()), &needed, &count, &resume, nullptr))
    { Append(error, LastError(L"枚举驱动服务")); return false; }
    bool success = true;
    const auto services = reinterpret_cast<ENUM_SERVICE_STATUS_PROCESSW*>(bytes.data());
    for (DWORD index = 0; index < count; ++index)
    {
        const std::wstring name = services[index].lpServiceName;
        Service query(OpenServiceW(manager.Get(), name.c_str(), SERVICE_QUERY_CONFIG));
        if (!query.Get()) continue;
        const auto owned = OwnedDriverDirectory(ImagePath(query.Get()), temporary);
        if (!owned || (!onlyDirectory.empty() && !Equal(owned->wstring(), onlyDirectory.wstring()))) continue;
        const DWORD attributes = GetFileAttributesW(owned->c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT))
        { Append(error, L"驱动目录为重解析点，未清理：" + owned->wstring()); success = false; continue; }
        if (AnotherAmiProcessRunning())
        { Append(error, L"AMIDEWIN 正在运行，未停止驱动 " + name); success = false; continue; }
        Service control(OpenServiceW(manager.Get(), name.c_str(), SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | SERVICE_STOP | DELETE));
        if (!control.Get()) { Append(error, LastError(L"打开本程序驱动 " + name)); success = false; continue; }
        const auto rechecked = OwnedDriverDirectory(ImagePath(control.Get()), temporary);
        if (!rechecked || !Equal(rechecked->wstring(), owned->wstring()))
        { Append(error, L"驱动路径已变化，未清理 " + name); success = false; continue; }
        if (!StopAndDelete(control.Get(), name, error)) { success = false; continue; }
        if (std::find(cleared.begin(), cleared.end(), *owned) == cleared.end()) cleared.push_back(*owned);
    }
    return success;
}

std::wstring DeleteOwnedFiles(const std::filesystem::path& directory)
{
    if (directory.empty()) return {};
    // Exact filenames only. Unknown content is preserved; no recursive deletion.
    constexpr std::array<const wchar_t*, 8> files{L"AMIDEWINx64.EXE", L"amifldrv64.sys",
        L"amigendrv64.sys", L"amifldrv32.sys", L"output.txt", L"console.txt", L"strings.txt", L"dump.txt"};
    std::wstring error;
    for (const auto name : files)
    {
        const auto file = directory / name;
        if (!DeleteFileW(file.c_str()))
        {
            const DWORD code = GetLastError();
            if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND)
                Append(error, L"删除 " + file.wstring() + L" 失败，Win32=" + std::to_wstring(code));
        }
    }
    if (!RemoveDirectoryW(directory.c_str()))
    {
        const DWORD code = GetLastError();
        if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND && error.empty())
            Append(error, L"删除目录 " + directory.wstring() + L" 失败，Win32=" + std::to_wstring(code));
    }
    return error;
}
}
