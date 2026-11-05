#include <Windows.h>
#include "AmiProcess.h"
#include <algorithm>
#include <array>
#include <vector>

namespace system_info::firmware::detail
{
namespace
{
class Handle final
{
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { if (Valid()) CloseHandle(value_); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE Get() const { return value_; }
    bool Valid() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
private:
    HANDLE value_;
};
constexpr LONGLONG kOutputLimit = 1024 * 1024;
struct ReadCommand
{
    const wchar_t* arguments;
    const wchar_t* outputFile;
};
ReadCommand CommandFor(ReadMode mode)
{
    switch (mode)
    {
    case ReadMode::SystemUuid: return {L"/SU", nullptr};
    case ReadMode::SystemSerial: return {L"/SS", nullptr};
    case ReadMode::BoardSerial: return {L"/BS", nullptr};
    case ReadMode::Strings: return {L"/ALL strings.txt", L"strings.txt"};
    case ReadMode::AllData: return {L"/DUMPALL dump.txt", L"dump.txt"};
    }
    return {L"", nullptr};
}
std::wstring Error(const wchar_t* operation)
{
    return std::wstring(operation) + L"失败，Win32=" + std::to_wstring(GetLastError());
}
class AttributeList final
{
public:
    ~AttributeList() { if (initialized_) DeleteProcThreadAttributeList(Get()); }
    bool Create(HANDLE output, HANDLE input)
    {
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        bytes_.resize(size);
        if (!InitializeProcThreadAttributeList(Get(), 1, 0, &size)) return false;
        initialized_ = true;
        handles_ = {output, input};
        return UpdateProcThreadAttribute(Get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            handles_.data(), sizeof(handles_), nullptr, nullptr) != FALSE;
    }
    LPPROC_THREAD_ATTRIBUTE_LIST Get() { return reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes_.data()); }
private:
    bool initialized_{};
    std::array<HANDLE, 2> handles_{};
    std::vector<unsigned char> bytes_;
};
std::wstring Decode(const std::string& bytes)
{
    if (bytes.empty()) return {};
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xfe)
    {
        std::wstring result;
        for (std::size_t i = 2; i + 1 < bytes.size(); i += 2)
            result += static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) |
                (static_cast<unsigned int>(static_cast<unsigned char>(bytes[i + 1])) << 8));
        return result;
    }
    const std::size_t offset = bytes.starts_with("\xEF\xBB\xBF") ? 3 : 0;
    const char* data = bytes.data() + offset;
    const int length = static_cast<int>(bytes.size() - offset);
    UINT encoding = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int size = MultiByteToWideChar(encoding, flags, data, length, nullptr, 0);
    if (!size)
    {
        encoding = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(encoding, flags, data, length, nullptr, 0);
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (size) MultiByteToWideChar(encoding, flags, data, length, result.data(), size);
    return result;
}
std::wstring ReadHandle(HANDLE file, std::wstring& error)
{
    LARGE_INTEGER size{}, origin{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || !SetFilePointerEx(file, origin, nullptr, FILE_BEGIN))
    { error = Error(L"读取输出文件长度"); return {}; }
    std::string bytes(static_cast<std::size_t>((std::min)(size.QuadPart, kOutputLimit)), '\0');
    DWORD count = 0;
    if (!bytes.empty() && (!ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &count, nullptr) || count != bytes.size()))
    { error = Error(L"读取工具输出"); return {}; }
    return Decode(bytes);
}
bool Oversized(const std::filesystem::path& path)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    return data.nFileSizeHigh != 0 || data.nFileSizeLow > kOutputLimit;
}
}

std::wstring ReadSmallTextFile(const std::filesystem::path& path)
{
    Handle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    std::wstring error;
    return file.Valid() ? ReadHandle(file.Get(), error) : std::wstring{};
}

ReadResult RunAmiRead(const std::filesystem::path& executable,
    const std::filesystem::path& directory, ReadMode mode, std::stop_token token, unsigned long timeoutMilliseconds)
{
    ReadResult result;
    if (token.stop_requested()) { result.error = L"已取消 AMIDEWIN 读取。"; return result; }
    const auto readCommand = CommandFor(mode);
    const auto resultFile = readCommand.outputFile ? directory / readCommand.outputFile : std::filesystem::path{};
    // /ALL and /DUMPALL support their own file output; avoid the interactive
    // screen paginator of /ALL and /DUMP with a redirected NUL console input.
    std::wstring command = L"\"" + executable.wstring() + L"\" " + readCommand.arguments;
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    Handle output(CreateFileW((directory / L"console.txt").c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ, &security, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
    Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
        &security, OPEN_EXISTING, 0, nullptr));
    if (!output.Valid() || !input.Valid()) { result.error = Error(L"创建工具输出"); return result; }
    Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job.Valid() || !SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
    { result.error = Error(L"创建受控工具进程"); return result; }
    AttributeList attributes;
    if (!attributes.Create(output.Get(), input.Get())) { result.error = Error(L"设置工具句柄"); return result; }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.Get();
    startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = output.Get();
    startup.lpAttributeList = attributes.Get();
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
        directory.c_str(), &startup.StartupInfo, &info))
    { result.error = Error(L"启动 AMIDEWIN"); return result; }
    Handle process(info.hProcess), thread(info.hThread);
    result.started = true;
    const ULONGLONG start = GetTickCount64();
    if (!AssignProcessToJobObject(job.Get(), process.Get()))
    {
        result.error = Error(L"管理 AMIDEWIN 进程");
        TerminateProcess(process.Get(), ERROR_CANCELLED);
        result.exited = WaitForSingleObject(process.Get(), 5000) == WAIT_OBJECT_0;
        return result;
    }
    if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1)) result.error = Error(L"运行 AMIDEWIN");
    while (result.error.empty())
    {
        const DWORD wait = WaitForSingleObject(process.Get(), 25);
        if (wait == WAIT_OBJECT_0) { result.exited = true; break; }
        if (wait == WAIT_FAILED) { result.error = Error(L"等待 AMIDEWIN 进程"); break; }
        if (token.stop_requested()) { result.error = L"已取消 AMIDEWIN 读取。"; break; }
        if (GetTickCount64() - start >= timeoutMilliseconds)
        { result.error = L"AMIDEWIN 读取超时（" + std::to_wstring(timeoutMilliseconds) + L" ms）。"; break; }
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(output.Get(), &size)) { result.error = Error(L"检查工具输出长度"); break; }
        if (size.QuadPart > kOutputLimit || (!resultFile.empty() && Oversized(resultFile)))
        { result.error = L"AMIDEWIN 输出超过 1 MiB。"; break; }
    }
    if (!result.exited)
    {
        if (!TerminateJobObject(job.Get(), ERROR_CANCELLED)) result.error += L" " + Error(L"终止工具进程");
        result.exited = WaitForSingleObject(process.Get(), 5000) == WAIT_OBJECT_0;
        if (!result.exited) result.error += L" 工具进程尚未退出，暂不卸载驱动。";
    }
    result.elapsedMilliseconds = static_cast<unsigned long>(GetTickCount64() - start);
    if (!result.exited) return result;
    if (!GetExitCodeProcess(process.Get(), &result.exitCode)) result.error += L" " + Error(L"读取工具退出码");
    // Capture output even on cancellation, nonzero exit or timeout. In the old
    // runner these paths returned early, losing the only evidence of the hang.
    std::wstring outputError;
    result.console = ReadHandle(output.Get(), outputError);
    if (!resultFile.empty()) result.output = ReadSmallTextFile(resultFile);
    if (result.output.empty()) result.output = result.console;
    if (!outputError.empty()) result.error += L" " + outputError;
    if (result.error.empty() && result.exitCode != 0)
        result.error = L"AMIDEWIN 退出码 " + std::to_wstring(result.exitCode) + L"。";
    if (result.error.empty() && result.output.empty()) result.error = L"AMIDEWIN 没有生成读取结果。";
    if (result.error.empty() && ((!resultFile.empty() && Oversized(resultFile)) || Oversized(directory / L"console.txt")))
        result.error = L"AMIDEWIN 输出超过 1 MiB。";
    result.success = result.error.empty();
    return result;
}

std::wstring ReadEvidence(const ReadResult& result, ReadMode mode)
{
    auto text = std::wstring(L"AMIDEWINx64.EXE ") + CommandFor(mode).arguments +
        L"\r\n耗时=" + std::to_wstring(result.elapsedMilliseconds) + L" ms，退出码=" + std::to_wstring(result.exitCode) +
        L"，进程已退出=" + (result.exited ? L"是" : L"否") + L"\r\n" + result.error +
        L"\r\n[控制台输出]\r\n" + result.console;
    if (CommandFor(mode).outputFile) text += L"\r\n[文件输出]\r\n" + result.output;
    return text + L"\r\n";
}
}
