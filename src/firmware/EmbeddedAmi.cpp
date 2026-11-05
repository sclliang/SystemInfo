#include <Windows.h>
#include <sddl.h>
#include <objbase.h>
#include "AmiFirmware.h"
#include "AmiProcess.h"
#include "AmiCleanup.h"
#include "AmiReader.h"
#include <array>
#include <filesystem>
#include <vector>

namespace system_info::firmware
{
namespace
{
class Handle final
{
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { Reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
    bool Valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void Reset(HANDLE value = nullptr) { if (Valid()) CloseHandle(value_); value_ = value; }
private:
    HANDLE value_;
};
class ReadLock final
{
public:
    ReadLock() : mutex_(CreateMutexW(nullptr, FALSE, L"Global\\sixunited.SystemInfo.AmiRead"))
    {
        if (mutex_.Valid())
        {
            const DWORD result = WaitForSingleObject(mutex_.Get(), 0);
            owned_ = result == WAIT_OBJECT_0 || result == WAIT_ABANDONED;
        }
    }
    ~ReadLock() { if (owned_) ReleaseMutex(mutex_.Get()); }
    bool Owned() const { return owned_; }
private:
    Handle mutex_;
    bool owned_{};
};
struct ResourceFile { int id; const wchar_t* name; };
constexpr std::array<ResourceFile, 4> kFiles{{
    {201, L"AMIDEWINx64.EXE"}, {202, L"amifldrv64.sys"},
    {203, L"amigendrv64.sys"}, {204, L"amifldrv32.sys"}}};

bool IsAdministrator()
{
    SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
    PSID group = nullptr;
    BOOL member = FALSE;
    if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
        DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &group))
    {
        CheckTokenMembership(nullptr, group, &member);
        FreeSid(group);
    }
    return member != FALSE;
}
std::wstring Error(const wchar_t* operation)
{
    return std::wstring(operation) + L"失败，Win32=" + std::to_wstring(GetLastError());
}
void Append(std::wstring& text, const std::wstring& line)
{
    if (line.empty()) return;
    if (!text.empty()) text += L"\r\n";
    text += line;
}

class PrivateDirectory final
{
public:
    ~PrivateDirectory() { if (!finished_ && allowCleanup_) Finish(); }
    std::filesystem::path path;
    void Preserve() { allowCleanup_ = false; }
    bool Create(std::wstring& error)
    {
        wchar_t temporary[32768]{};
        const DWORD length = GetTempPathW(static_cast<DWORD>(std::size(temporary)), temporary);
        if (length == 0 || length >= std::size(temporary)) { error = Error(L"获取临时目录"); return false; }
        GUID id{};
        wchar_t suffix[40]{};
        if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, suffix, static_cast<int>(std::size(suffix))))
        { error = L"无法创建 AMIDEWIN 临时目录标识。"; return false; }
        path = std::filesystem::path(temporary) / (std::wstring(L"SystemInfo-AMI-") + suffix);
        // Admin ownership/protected ACL prevents unelevated replacement without
        // holding restrictive file sharing locks against the vendor's loader.
        PSECURITY_DESCRIPTOR descriptor = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"O:BAG:BAD:P(A;;FA;;;SY)(A;;FA;;;BA)", SDDL_REVISION_1, &descriptor, nullptr))
        { error = Error(L"设置临时目录权限"); path.clear(); return false; }
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
        const BOOL created = CreateDirectoryW(path.c_str(), &attributes);
        const DWORD status = GetLastError();
        LocalFree(descriptor);
        if (!created) { error = L"创建临时目录失败，Win32=" + std::to_wstring(status); path.clear(); return false; }
        directory_.Reset(CreateFileW(path.c_str(), FILE_LIST_DIRECTORY,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        if (!directory_.Valid()) { error = Error(L"保护临时目录"); return false; }
        for (const auto file : kFiles)
        {
            HRSRC resource = FindResourceW(nullptr, MAKEINTRESOURCEW(file.id), RT_RCDATA);
            const DWORD size = resource ? SizeofResource(nullptr, resource) : 0;
            HGLOBAL loaded = resource ? LoadResource(nullptr, resource) : nullptr;
            const void* data = loaded ? LockResource(loaded) : nullptr;
            if (!data || !size) { error = L"缺少内嵌 AMIDEWIN 资源。"; return false; }
            const auto destination = path / file.name;
            Handle output(CreateFileW(destination.c_str(), GENERIC_WRITE, 0, nullptr,
                CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (!output.Valid()) { error = Error(L"释放内嵌工具"); return false; }
            DWORD written = 0;
            if (!WriteFile(output.Get(), data, size, &written, nullptr) || written != size)
            { error = Error(L"写入内嵌工具"); return false; }
        }
        return true;
    }
    std::wstring Finish()
    {
        finished_ = true;
        if (path.empty()) return {};
        if (!allowCleanup_) return L"工具进程尚未退出，保留目录：" + path.wstring();
        // A terminated process cannot unload its kernel driver. Stop only driver
        // services whose ImagePath points to this exact private directory.
        std::vector<std::filesystem::path> cleared;
        std::wstring error;
        if (!detail::CleanupOwnedDrivers(path, cleared, error))
            return error + L"\r\n保留驱动目录：" + path.wstring();
        directory_.Reset();
        error = detail::DeleteOwnedFiles(path);
        if (error.empty()) path.clear();
        return error;
    }
private:
    Handle directory_;
    bool allowCleanup_{true}, finished_{};
};
}

AmiResult ReadEmbeddedAmi(std::stop_token token)
{
    AmiResult result;
    if (token.stop_requested()) return result;
    if (!IsAdministrator())
    {
        result.diagnostic = L"AMIDEWIN 需要管理员权限，固件信息已回退到原采集方案。";
        return result;
    }
    ReadLock readLock;
    if (!readLock.Owned() || detail::AnotherAmiProcessRunning())
    {
        result.diagnostic = L"另一个 AMIDEWIN 读取尚未结束，固件信息使用原采集方案。";
        return result;
    }
    std::vector<std::filesystem::path> recovered;
    if (!detail::CleanupOwnedDrivers({}, recovered, result.diagnostic))
    {
        Append(result.diagnostic, L"未启动新的 AMIDEWIN，固件信息使用原采集方案。");
        return result;
    }
    for (const auto& directory : recovered)
    {
        Append(result.evidence, L"已停止上次残留的本程序驱动：" + directory.wstring());
        const auto oldOutput = detail::ReadSmallTextFile(directory / L"output.txt");
        if (!oldOutput.empty()) Append(result.evidence, L"[上次输出]\r\n" + oldOutput);
        Append(result.diagnostic, detail::DeleteOwnedFiles(directory));
    }
    if (token.stop_requested()) return result;
    PrivateDirectory directory;
    std::wstring createError;
    if (!directory.Create(createError))
    {
        Append(result.diagnostic, createError);
        Append(result.diagnostic, directory.Finish());
        Append(result.diagnostic, L"固件信息已回退到原采集方案。");
        return result;
    }
    const auto executable = directory.path / L"AMIDEWINx64.EXE";
    auto read = detail::ReadAmiFirmware(executable, directory.path, token);
    result.values = std::move(read.result.values);
    Append(result.evidence, read.result.evidence);
    Append(result.diagnostic, read.result.diagnostic);
    if (!read.allProcessesExited) directory.Preserve();
    Append(result.diagnostic, directory.Finish());
    return result;
}
}
