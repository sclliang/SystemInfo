#include "Diagnostics.h"
#include <winevt.h>
#include <shlobj.h>
#include <powrprof.h>
#include <algorithm>
#include <cwctype>
#include <functional>
#include <tuple>
#include <utility>

namespace elm
{
    namespace
    {
        namespace fs = std::filesystem;
        constexpr uint64_t DayTicks = 86400ULL * 10000000;
        class Handle
        {
        public:
            explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
            ~Handle()
            {
                if (value_ && value_ != INVALID_HANDLE_VALUE)
                    CloseHandle(value_);
            }
            Handle(const Handle &) = delete;
            Handle &operator=(const Handle &) = delete;
            operator HANDLE() const { return value_; }

        private:
            HANDLE value_;
        };
        class EventHandle
        {
        public:
            explicit EventHandle(EVT_HANDLE value) : value_(value) {}
            ~EventHandle()
            {
                if (value_)
                    EvtClose(value_);
            }
            operator EVT_HANDLE() const { return value_; }

        private:
            EVT_HANDLE value_;
        };
        bool Missing(DWORD code) { return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND || code == ERROR_EVT_CHANNEL_NOT_FOUND; }
        std::wstring Lower(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
                           { return static_cast<wchar_t>(towlower(ch)); });
            return value;
        }
        uint64_t Cutoff(const DiagnosticContext &context)
        {
            return context.now - std::min(context.now, static_cast<uint64_t>(context.options.days) * DayTicks);
        }
        void Issue(CollectedLog &item, const fs::path &source, DWORD code, const std::wstring &message)
        {
            CollectedFile file;
            file.source = source.wstring();
            file.error = code;
            file.message = message;
            file.state = Missing(code) ? CollectionState::NotFound : CollectionState::Failed;
            item.files.push_back(std::move(file));
        }
        void FinishIndex(CollectedLog &item, const DiagnosticContext &context, const std::wstring &extraJson = L"")
        {
            size_t saved = 0, failed = 0, skipped = 0;
            for (const auto &file : item.files)
            {
                if (file.state == CollectionState::Saved)
                    ++saved;
                if (file.state == CollectionState::Skipped)
                    ++skipped;
                if (file.state == CollectionState::Failed || file.state == CollectionState::Partial)
                {
                    ++failed;
                    if (!item.error)
                        item.error = file.error;
                }
            }
            item.state = failed ? CollectionState::Partial : saved ? CollectionState::Saved
                                                         : skipped ? CollectionState::Skipped
                                                                   : CollectionState::NotFound;
            item.message = L"已保存 " + std::to_wstring(saved) + L" 项，跳过 " + std::to_wstring(skipped) +
                           L" 项，错误 " + std::to_wstring(failed) + L" 项；详细原因见索引。";
            SaveDiagnosticJson(item, L"{\"windowStartUtc\":" + JsonQuote(DiagnosticUtc(Cutoff(context))) +
                                         L",\"collectedAtUtc\":" + JsonQuote(DiagnosticUtc(context.now)) +
                                         L",\"maxDumpFileBytes\":" + std::to_wstring(context.maxDumpFileBytes) +
                                         L",\"maxDumpTotalBytes\":" + std::to_wstring(context.maxDumpTotalBytes) +
                                         L",\"maxAuxiliaryTotalBytes\":" + std::to_wstring(context.maxAuxiliaryTotalBytes) +
                                         L",\"maxFiles\":" + std::to_wstring(context.maxFiles) +
                                         extraJson + L",\"files\":" + CollectedFilesJson(item.files) + L"}\n");
        }
        struct Candidate
        {
            fs::path source, relative;
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            bool dump = false, optIn = false;
        };
        bool Attributes(const fs::path &path, WIN32_FILE_ATTRIBUTE_DATA &attributes)
        {
            return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes) != FALSE;
        }
        void Scan(CollectedLog &item, const DiagnosticContext &context, const fs::path &root, const fs::path &prefix,
                  const std::function<bool(const fs::path &)> &select, std::vector<Candidate> &candidates, size_t &scanned,
                  const fs::path &relative = {}, unsigned depth = 0)
        {
            CheckDiagnosticCancel(context);
            const auto directory = root / relative;
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (!Attributes(directory, attributes))
            {
                const auto code = GetLastError();
                Issue(item, directory, code, SystemError(code));
                return;
            }
            if (attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
            {
                CollectedFile file;
                file.source = directory.wstring();
                file.state = CollectionState::Skipped;
                file.message = L"跳过重解析点，避免跟随到其他目录。";
                item.files.push_back(std::move(file));
                return;
            }
            if (!(attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            {
                Issue(item, directory, ERROR_DIRECTORY, L"源路径不是目录。");
                return;
            }
            WIN32_FIND_DATAW data{};
            HANDLE search = FindFirstFileW((directory / L"*").c_str(), &data);
            if (search == INVALID_HANDLE_VALUE)
            {
                const auto code = GetLastError();
                if (code != ERROR_FILE_NOT_FOUND)
                    Issue(item, directory, code, SystemError(code));
                return;
            }
            struct SearchCloser
            {
                HANDLE value;
                ~SearchCloser() { FindClose(value); }
            } closer{search};
            do
            {
                CheckDiagnosticCancel(context);
                if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0)
                    continue;
                if (++scanned > context.maxScanEntries)
                {
                    Issue(item, directory, ERROR_MORE_DATA, L"达到目录扫描上限，索引可能不完整。");
                    return;
                }
                const auto child = relative / data.cFileName;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                {
                    CollectedFile file;
                    file.source = (root / child).wstring();
                    file.state = CollectionState::Skipped;
                    file.message = L"跳过重解析点。";
                    item.files.push_back(std::move(file));
                    continue;
                }
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                {
                    if (depth < 5)
                        Scan(item, context, root, prefix, select, candidates, scanned, child, depth + 1);
                    else
                        Issue(item, root / child, ERROR_MORE_DATA, L"达到子目录深度上限。");
                    if (scanned > context.maxScanEntries)
                        return;
                }
                else if (select(child) && FileTimeValue(data.ftLastWriteTime) >= Cutoff(context))
                {
                    Candidate candidate;
                    candidate.source = root / child;
                    candidate.relative = prefix / child;
                    candidate.attributes.dwFileAttributes = data.dwFileAttributes;
                    candidate.attributes.ftLastWriteTime = data.ftLastWriteTime;
                    candidate.attributes.nFileSizeHigh = data.nFileSizeHigh;
                    candidate.attributes.nFileSizeLow = data.nFileSizeLow;
                    const auto ext = Lower(child.extension().wstring());
                    candidate.dump = ext == L".dmp" || ext == L".mdmp" || ext == L".hdmp";
                    candidates.push_back(std::move(candidate));
                }
            } while (FindNextFileW(search, &data));
            const auto code = GetLastError();
            if (code != ERROR_NO_MORE_FILES)
                Issue(item, directory, code, SystemError(code));
        }
        DWORD CALLBACK CopyProgress(LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID data)
        {
            const auto *cancel = static_cast<const std::atomic_bool *>(data);
            return cancel && cancel->load() ? PROGRESS_CANCEL : PROGRESS_CONTINUE;
        }
        void CopyCandidates(CollectedLog &item, DiagnosticContext &context, std::vector<Candidate> &candidates, const fs::path &folder)
        {
            std::stable_sort(candidates.begin(), candidates.end(), [](const auto &a, const auto &b)
                             { return FileTimeValue(a.attributes.ftLastWriteTime) > FileTimeValue(b.attributes.ftLastWriteTime); });
            size_t copied = 0;
            for (const auto &candidate : candidates)
            {
                CheckDiagnosticCancel(context);
                CollectedFile file;
                file.source = candidate.source.wstring();
                file.bytes = (static_cast<uint64_t>(candidate.attributes.nFileSizeHigh) << 32) | candidate.attributes.nFileSizeLow;
                file.modifiedUtc = DiagnosticUtc(FileTimeValue(candidate.attributes.ftLastWriteTime));
                std::wstring reason;
                if (candidate.attributes.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                    reason = L"跳过重解析点。";
                else if (FileTimeValue(candidate.attributes.ftLastWriteTime) < Cutoff(context))
                    reason = L"早于本次采集时间范围。";
                else if (candidate.optIn && !context.options.includeLargeDumps)
                    reason = L"仅记录位置；复制系统内存转储需要 --include-large-dumps。";
                else if (copied >= context.maxFiles)
                    reason = L"达到此类文件数量上限。";
                else if (file.bytes > (candidate.dump ? context.maxDumpFileBytes : 32ULL * 1024 * 1024))
                    reason = L"超过单文件大小上限，仅保留元数据。";
                else if (candidate.dump && file.bytes > context.maxDumpTotalBytes - std::min(context.dumpBytesCopied, context.maxDumpTotalBytes))
                    reason = L"达到转储文件总大小上限，仅保留元数据。";
                else if (!candidate.dump && file.bytes > context.maxAuxiliaryTotalBytes - std::min(context.auxiliaryBytesCopied, context.maxAuxiliaryTotalBytes))
                    reason = L"达到报告和安装日志总大小上限，仅保留元数据。";
                if (!reason.empty())
                {
                    file.state = CollectionState::Skipped;
                    file.message = reason;
                    item.files.push_back(std::move(file));
                    continue;
                }
                const auto destination = folder / candidate.relative;
                const fs::path temporary = destination.wstring() + L".tmp";
                bool ownsTemporary = false;
                try
                {
                    fs::create_directories(destination.parent_path());
                    CheckDiagnosticCancel(context);
                    BOOL abort = FALSE;
                    if (!CopyFileExW(candidate.source.c_str(), temporary.c_str(), CopyProgress,
                                     const_cast<std::atomic_bool *>(context.cancel), &abort, COPY_FILE_FAIL_IF_EXISTS))
                    {
                        const auto code = GetLastError();
                        // CopyFileEx removes its incomplete destination after PROGRESS_CANCEL.
                        if (code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS)
                            DeleteFileW(temporary.c_str());
                        if (code == ERROR_REQUEST_ABORTED && context.cancel && context.cancel->load())
                            throw Error(ERROR_CANCELLED, L"复制已取消");
                        throw Error(code, L"复制：" + candidate.source.wstring());
                    }
                    ownsTemporary = true;
                    CheckDiagnosticCancel(context);
                    WIN32_FILE_ATTRIBUTE_DATA after{};
                    if (!Attributes(candidate.source, after))
                        throw Error(GetLastError(), L"复查源文件");
                    if (after.nFileSizeHigh != candidate.attributes.nFileSizeHigh || after.nFileSizeLow != candidate.attributes.nFileSizeLow ||
                        FileTimeValue(after.ftLastWriteTime) != FileTimeValue(candidate.attributes.ftLastWriteTime))
                        throw Error(ERROR_FILE_INVALID, L"复制期间源文件发生变化，请重试");
                    if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH))
                        throw Error(GetLastError(), L"提交采集文件");
                    ownsTemporary = false;
                    file.path = destination.wstring();
                    file.state = CollectionState::Saved;
                    ++copied;
                    if (candidate.dump)
                        context.dumpBytesCopied += file.bytes;
                    else
                        context.auxiliaryBytesCopied += file.bytes;
                }
                catch (const Error &error)
                {
                    if (ownsTemporary)
                        DeleteFileW(temporary.c_str());
                    if (error.code == ERROR_CANCELLED)
                        throw;
                    file.error = error.code;
                    file.state = CollectionState::Failed;
                    file.message = error.text;
                }
                catch (const fs::filesystem_error &error)
                {
                    if (ownsTemporary)
                        DeleteFileW(temporary.c_str());
                    file.error = static_cast<DWORD>(error.code().value());
                    file.state = CollectionState::Failed;
                    file.message = SystemError(file.error);
                }
                item.files.push_back(std::move(file));
            }
        }
        void AddSingle(CollectedLog &item, const fs::path &source, const fs::path &relative, bool dump,
                       bool optIn, std::vector<Candidate> &candidates)
        {
            Candidate candidate;
            candidate.source = source;
            candidate.relative = relative;
            candidate.dump = dump;
            candidate.optIn = optIn;
            if (!Attributes(source, candidate.attributes))
            {
                const auto code = GetLastError();
                Issue(item, source, code, SystemError(code));
                return;
            }
            if (candidate.attributes.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                Issue(item, source, ERROR_DIRECTORY, L"源路径不是文件。");
                return;
            }
            candidates.push_back(std::move(candidate));
        }
        fs::path KnownFolder(REFKNOWNFOLDERID id)
        {
            PWSTR path = nullptr;
            const HRESULT result = SHGetKnownFolderPath(id, KF_FLAG_DONT_VERIFY, nullptr, &path);
            fs::path value;
            if (SUCCEEDED(result) && path)
                value = path;
            CoTaskMemFree(path);
            return value;
        }
        std::wstring TimelineCategory(const Event &event)
        {
            if (event.provider == L"Microsoft-Windows-Kernel-Power" && event.eventId == 41)
                return L"unexpected_restart";
            if (event.provider == L"EventLog" && event.eventId == 6008)
                return L"unexpected_shutdown";
            if ((event.provider == L"Microsoft-Windows-WER-SystemErrorReporting" || event.provider == L"BugCheck") && event.eventId == 1001)
                return L"bugcheck";
            if (event.provider == L"Microsoft-Windows-WHEA-Logger")
                return L"hardware_error";
            if (Lower(event.provider) == L"volmgr")
                return L"dump_or_volume";
            return L"context";
        }
    }

    uint64_t FileTimeValue(FILETIME time) { return (static_cast<uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime; }
    std::wstring DiagnosticUtc(uint64_t ticks)
    {
        FILETIME time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
        SYSTEMTIME value{};
        if (!FileTimeToSystemTime(&time, &value))
            return L"";
        wchar_t text[48]{};
        swprintf_s(text, L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
                   value.wYear, value.wMonth, value.wDay, value.wHour, value.wMinute, value.wSecond, value.wMilliseconds);
        return text;
    }
    void CheckDiagnosticCancel(const DiagnosticContext &context)
    {
        if (context.cancel && context.cancel->load())
            throw Error(ERROR_CANCELLED, L"采集已取消");
    }
    std::wstring ExpandedEnvironment(const std::wstring &value)
    {
        DWORD length = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
        if (!length || length > 32768)
            throw Error(length ? ERROR_FILENAME_EXCED_RANGE : GetLastError(), L"展开系统路径");
        std::wstring text(length, L'\0');
        const DWORD actual = ExpandEnvironmentStringsW(value.c_str(), text.data(), length);
        if (!actual || actual > length)
            throw Error(ERROR_INVALID_DATA, L"展开系统路径");
        text.resize(actual - 1);
        return text;
    }
    void WriteDiagnosticFile(const fs::path &path, const std::wstring &text)
    {
        fs::create_directories(path.parent_path());
        const auto bytes = Utf8(text);
        const fs::path temporary = path.wstring() + L".tmp";
        bool ownsTemporary = false;
        try
        {
            Handle file(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
            if (file == INVALID_HANDLE_VALUE)
                throw Error(GetLastError(), L"创建诊断文件");
            ownsTemporary = true;
            size_t offset = 0;
            while (offset < bytes.size())
            {
                DWORD written = 0;
                if (!WriteFile(file, bytes.data() + offset, static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1024 * 1024)), &written, nullptr))
                    throw Error(GetLastError(), L"写入诊断文件");
                if (!written)
                    throw Error(ERROR_WRITE_FAULT, L"写入诊断文件");
                offset += written;
            }
            if (!FlushFileBuffers(file))
                throw Error(GetLastError(), L"保存诊断文件");
        }
        catch (...)
        {
            if (ownsTemporary)
                DeleteFileW(temporary.c_str());
            throw;
        }
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const auto code = GetLastError();
            DeleteFileW(temporary.c_str());
            throw Error(code, L"提交诊断文件");
        }
    }
    void SaveDiagnosticJson(CollectedLog &item, const std::wstring &text)
    {
        WriteDiagnosticFile(item.path, text);
        item.bytes = fs::file_size(item.path);
        for (const auto &file : item.files)
            if (file.state == CollectionState::Saved)
                item.bytes += file.bytes;
        if (item.state == CollectionState::Running || item.state == CollectionState::Waiting)
            item.state = CollectionState::Saved;
    }
    DiagnosticContext MakeDiagnosticContext(const CollectionResult &result, const std::atomic_bool *cancel)
    {
        DiagnosticContext context;
        context.output = result.directory;
        context.options = result.options;
        context.cancel = cancel;
        wchar_t windows[32768]{};
        const auto count = GetWindowsDirectoryW(windows, static_cast<UINT>(std::size(windows)));
        if (!count || count >= std::size(windows))
            throw Error(GetLastError(), L"获取 Windows 目录");
        context.windows = windows;
        context.programData = KnownFolder(FOLDERID_ProgramData);
        context.localAppData = KnownFolder(FOLDERID_LocalAppData);
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        context.now = FileTimeValue(now);
        if (context.options.includeLargeDumps)
        {
            context.maxDumpFileBytes = 4ULL * 1024 * 1024 * 1024;
            context.maxDumpTotalBytes = 8ULL * 1024 * 1024 * 1024;
        }
        return context;
    }
    void CollectDumpFiles(CollectedLog &item, DiagnosticContext &context, bool live)
    {
        const auto folder = context.output / (live ? L"LiveKernelReports" : L"CrashDumps");
        item.path = (folder / L"index.json").wstring();
        std::vector<Candidate> candidates;
        size_t scanned = 0;
        if (live)
            Scan(item, context, context.windows / L"LiveKernelReports", {}, [](const auto &path)
                 { return Lower(path.extension().wstring()) == L".dmp"; }, candidates, scanned);
        else
        {
            fs::path mini = context.windows / L"Minidump", memory = context.windows / L"MEMORY.DMP";
            if (context.resolveDumpPaths)
            {
                try
                {
                    constexpr auto key = L"SYSTEM\\CurrentControlSet\\Control\\CrashControl";
                    const auto miniSetting = ReadRegistryString(HKEY_LOCAL_MACHINE, key, L"MinidumpDir");
                    const auto memorySetting = ReadRegistryString(HKEY_LOCAL_MACHINE, key, L"DumpFile");
                    if (!miniSetting.empty())
                        mini = ExpandedEnvironment(miniSetting);
                    if (!memorySetting.empty())
                        memory = ExpandedEnvironment(memorySetting);
                }
                catch (const Error &error)
                {
                    Issue(item, L"CrashControl", error.code, error.text);
                }
            }
            Scan(item, context, mini, L"Minidump", [](const auto &path)
                 { return Lower(path.extension().wstring()) == L".dmp"; }, candidates, scanned);
            AddSingle(item, memory, L"SystemMemory.dmp", true, true, candidates);
        }
        CopyCandidates(item, context, candidates, folder);
        FinishIndex(item, context);
    }
    void CollectWerReports(CollectedLog &item, DiagnosticContext &context)
    {
        const auto folder = context.output / L"WerReports";
        item.path = (folder / L"index.json").wstring();
        std::vector<Candidate> candidates;
        size_t scanned = 0;
        const auto select = [](const fs::path &path)
        {
            const auto ext = Lower(path.extension().wstring());
            return ext == L".wer" || ext == L".xml" || ext == L".dmp" || ext == L".mdmp" || ext == L".hdmp";
        };
        for (const auto &[root, prefix] : std::vector<std::pair<fs::path, fs::path>>{{context.programData, L"Machine"}, {context.localAppData, L"CurrentUser"}})
        {
            if (root.empty())
            {
                Issue(item, prefix, ERROR_NOT_READY, L"无法解析已知文件夹位置。");
                continue;
            }
            for (const auto *name : {L"ReportArchive", L"ReportQueue"})
            {
                if (scanned > context.maxScanEntries)
                    break;
                Scan(item, context, root / L"Microsoft" / L"Windows" / L"WER" / name, prefix / name, select, candidates, scanned);
            }
        }
        CopyCandidates(item, context, candidates, folder);
        FinishIndex(item, context);
    }
    void CollectDriverInstallation(CollectedLog &item, DiagnosticContext &context)
    {
        const auto folder = context.output / L"DriverInstallation";
        item.path = (folder / L"index.json").wstring();
        std::vector<Candidate> candidates;
        for (const auto *name : {L"setupapi.dev.log", L"setupapi.dev.log.old", L"setupapi.app.log"})
            AddSingle(item, context.windows / L"INF" / name, name, false, false, candidates);
        // Installation logs are a rolling history, so retain them regardless of age.
        for (auto &candidate : candidates)
            candidate.optIn = false;
        auto historyContext = context;
        historyContext.now = 0;
        CopyCandidates(item, historyContext, candidates, folder);
        context.auxiliaryBytesCopied = historyContext.auxiliaryBytesCopied;
        FinishIndex(item, context);
    }
    void CollectDiagnosticEvents(CollectedLog &item, const DiagnosticContext &context)
    {
        item.path = (context.output / L"DiagnosticEvents" / L"index.json").wstring();
        for (const auto *channel : {
                 L"Microsoft-Windows-Kernel-PnP/Configuration", L"Microsoft-Windows-DriverFrameworks-UserMode/Operational",
                 L"Microsoft-Windows-DeviceSetupManager/Admin", L"Microsoft-Windows-DeviceSetupManager/Operational",
                 L"Microsoft-Windows-CodeIntegrity/Operational", L"Microsoft-Windows-WindowsUpdateClient/Operational",
                 L"Microsoft-Windows-Diagnostics-Performance/Operational", L"Microsoft-Windows-Kernel-Boot/Operational"})
        {
            CheckDiagnosticCancel(context);
            CollectedFile file;
            file.source = channel;
            try
            {
                EventHandle config(EvtOpenChannelConfig(nullptr, channel, 0));
                if (!config)
                    throw Error(GetLastError(), L"打开诊断通道");
                EVT_VARIANT enabled{};
                DWORD used = 0;
                if (!EvtGetChannelConfigProperty(config, EvtChannelConfigEnabled, 0, sizeof(enabled), &enabled, &used))
                    throw Error(GetLastError(), L"读取通道配置");
                if (enabled.Type != EvtVarTypeBoolean)
                    throw Error(ERROR_INVALID_DATA, L"无效的通道配置");
                if (!enabled.BooleanVal)
                {
                    file.state = CollectionState::Skipped;
                    file.message = L"通道未启用；采集不会更改通道配置。";
                }
                else
                {
                    std::wstring name = channel;
                    std::replace(name.begin(), name.end(), L'/', L'_');
                    const auto destination = context.output / L"DiagnosticEvents" / (name + L".evtx");
                    fs::create_directories(destination.parent_path());
                    Query query;
                    query.source = channel;
                    query.limit = 0;
                    query.hours = context.options.days * 24ULL;
                    Export(query, destination.wstring(), Format::Evtx, false, context.cancel);
                    file.state = CollectionState::Saved;
                    file.path = destination.wstring();
                    file.bytes = fs::file_size(destination);
                }
            }
            catch (const Error &error)
            {
                if (error.code == ERROR_CANCELLED)
                    throw;
                file.error = error.code;
                file.message = error.text;
                file.state = Missing(error.code) ? CollectionState::NotFound : CollectionState::Failed;
            }
            item.files.push_back(std::move(file));
        }
        FinishIndex(item, context);
    }
    void CollectTimeline(CollectedLog &item, const DiagnosticContext &context, const CollectionResult &result)
    {
        item.path = (context.output / L"Timeline.json").wstring();
        std::vector<Event> events;
        size_t characters = 0;
        std::vector<std::pair<std::wstring, std::wstring>> sources;
        for (const auto &log : result.logs)
            if (log.channel == L"System" || log.channel == L"Application")
            {
                if (log.state == CollectionState::Saved)
                    sources.emplace_back(log.channel, log.path);
                else
                {
                    CollectedFile file;
                    file.source = log.channel;
                    file.state = CollectionState::Failed;
                    file.error = log.error;
                    file.message = L"原始日志未能导出，时间线缺少此来源：" + log.message;
                    item.files.push_back(std::move(file));
                }
            }
        for (const auto &diagnostic : result.diagnostics)
            if (diagnostic.channel == L"DiagnosticEvents")
                for (const auto &file : diagnostic.files)
                    if (file.state == CollectionState::Saved)
                        sources.emplace_back(file.source, file.path);
        const auto start = DiagnosticUtc(Cutoff(context)), end = DiagnosticUtc(context.now);
        for (const auto &[channel, path] : sources)
        {
            CheckDiagnosticCancel(context);
            CollectedFile source;
            source.source = channel;
            source.path = path;
            source.state = CollectionState::Saved;
            try
            {
                Query query;
                query.source = path;
                query.file = true;
                query.limit = context.maxEventsPerChannel + 1;
                query.xpath = L"*[System[TimeCreated[@SystemTime >= '" + start + L"' and @SystemTime <= '" + end +
                              L"'] and ((Level >= 1 and Level <= 3) or Provider[@Name='Microsoft-Windows-WHEA-Logger'] or "
                              L"Provider[@Name='Microsoft-Windows-WER-SystemErrorReporting'] or Provider[@Name='Windows Error Reporting'] or "
                              L"Provider[@Name='User32'] or (Provider[@Name='EventLog'] and (EventID=6005 or EventID=6006 or EventID=6008)))]]";
                size_t count = 0;
                Read(query, [&](Event &&event)
                     {
                CheckDiagnosticCancel(context);
                if (++count > context.maxEventsPerChannel || events.size() >= 20000 ||
                    event.xml.size() + event.message.size() > 32ULL * 1024 * 1024 - std::min<size_t>(characters, 32ULL * 1024 * 1024))
                    throw Error(ERROR_MORE_DATA, L"达到时间线条数或大小上限；原始 EVTX 保留完整事件。");
                characters += event.xml.size() + event.message.size(); events.push_back(std::move(event)); }, context.cancel);
            }
            catch (const Error &error)
            {
                if (error.code == ERROR_CANCELLED)
                    throw;
                source.state = CollectionState::Partial;
                source.error = error.code;
                source.message = error.text;
            }
            item.files.push_back(std::move(source));
        }
        std::sort(events.begin(), events.end(), [](const auto &a, const auto &b)
                  { return std::tie(a.timeUtc, a.channel, a.recordId) < std::tie(b.timeUtc, b.channel, b.recordId); });
        std::wstring json = L"{\"windowStartUtc\":" + JsonQuote(start) + L",\"windowEndUtc\":" + JsonQuote(end) +
                            L",\"note\":\"按事件写入时间排序。Kernel-Power 41、6008 等可能在重启后补记；事件相关性不等于根因。"
                            L"实际故障时间与 BugCheck 参数请结合消息、原始 XML 和转储分析。\",\"sources\":" +
                            CollectedFilesJson(item.files) + L",\"events\":[";
        for (size_t i = 0; i < events.size(); ++i)
        {
            if (i)
                json += L",";
            const auto &event = events[i];
            json += L"\n{\"timeUtc\":" + JsonQuote(event.timeUtc) + L",\"timeLocal\":" + JsonQuote(event.time) +
                    L",\"channel\":" + JsonQuote(event.channel) + L",\"provider\":" + JsonQuote(event.provider) +
                    L",\"recordId\":" + std::to_wstring(event.recordId) + L",\"eventId\":" + std::to_wstring(event.eventId) +
                    L",\"level\":" + std::to_wstring(event.level) + L",\"category\":" + JsonQuote(TimelineCategory(event)) +
                    L",\"message\":" + JsonQuote(event.message) + L",\"xml\":" + JsonQuote(event.xml) + L"}";
        }
        bool failed = false;
        for (const auto &source : item.files)
            if (source.state != CollectionState::Saved)
            {
                failed = true;
                if (!item.error)
                    item.error = source.error;
            }
        item.state = failed ? CollectionState::Partial : CollectionState::Saved;
        item.message = L"已整理 " + std::to_wstring(events.size()) + L" 条事件；以来源、事件 ID 和时间共同判断。";
        SaveDiagnosticJson(item, json + L"\n]}\n");
    }
    void CollectPowerReports(CollectedLog &item, const DiagnosticContext &context)
    {
        if (!context.options.powerReports)
        {
            item.state = CollectionState::Skipped;
            item.message = L"已通过 --no-power 关闭电源报告采集。";
            return;
        }
        const auto folder = context.output / L"PowerReports";
        item.path = (folder / L"index.json").wstring();
        fs::create_directories(folder);
        wchar_t system[32768]{};
        const auto systemLength = GetSystemDirectoryW(system, static_cast<UINT>(std::size(system)));
        if (!systemLength || systemLength >= std::size(system))
            throw Error(systemLength ? ERROR_INSUFFICIENT_BUFFER : GetLastError(), L"获取系统工具路径");
        const auto executable = context.powercfgExecutable.empty() ? fs::path(system) / L"powercfg.exe" : context.powercfgExecutable;
        SYSTEM_POWER_STATUS battery{};
        // Unknown capabilities must not be misreported as unsupported: let powercfg decide.
        const bool noBattery = GetSystemPowerStatus(&battery) && battery.BatteryFlag != 255 && (battery.BatteryFlag & 128);
        SYSTEM_POWER_CAPABILITIES capabilities{};
        const bool noModernStandby = GetPwrCapabilities(&capabilities) && !capabilities.AoAc;
        const auto transitionDays = std::min(context.options.days, 28U);
        const std::wstring metadata = L",\"requestedDays\":" + std::to_wstring(context.options.days) +
            L",\"transitionReportDays\":" + std::to_wstring(transitionDays) +
            L",\"energyDurationSeconds\":" + (context.options.energyReport ? L"60" : L"0") +
            L",\"note\":\"时间范围为请求范围，实际历史取决于 Windows 保留的数据；energy 是现场采样，不是历史日志。\"";
        struct Command
        {
            const wchar_t *name;
            std::wstring arguments;
            bool html;
            const wchar_t *skipReason;
            DWORD timeoutMs;
        };
        const auto timeout = context.powerCommandTimeoutMs;
        const std::vector<Command> commands{
            {L"sleep-states", L"/a", false, nullptr, timeout},
            {L"last-wake", L"/lastwake", false, nullptr, timeout},
            {L"power-requests", L"/requests", false, nullptr, timeout},
            {L"system-power", L"/systempowerreport /duration " + std::to_wstring(transitionDays), true, nullptr, timeout},
            {L"battery", L"/batteryreport /duration " + std::to_wstring(context.options.days), true,
                noBattery ? L"未检测到系统电池，跳过电池报告。" : nullptr, timeout},
            {L"sleep-study", L"/sleepstudy /duration " + std::to_wstring(transitionDays), true,
                noModernStandby ? L"此设备不支持 Modern Standby，跳过 SleepStudy。" : nullptr, timeout},
            {L"energy", L"/energy /duration 60", true,
                context.options.energyReport ? nullptr : L"现场能效诊断按需运行：命令行增加 --energy（采样 60 秒，建议保持电脑空闲）。",
                context.energyCommandTimeoutMs}};
        for (const auto &command : commands)
        {
            CollectedFile file;
            file.source = L"powercfg " + command.arguments;
            if (context.cancel && context.cancel->load())
            {
                file.state = CollectionState::Cancelled;
                file.error = ERROR_CANCELLED;
                file.message = L"电源诊断已取消。";
                item.files.push_back(std::move(file));
                FinishIndex(item, context, metadata);
                throw Error(ERROR_CANCELLED, L"电源诊断已取消");
            }
            if (command.skipReason)
            {
                file.state = CollectionState::Skipped;
                file.message = command.skipReason;
                item.files.push_back(std::move(file));
                continue;
            }
            const auto output = folder / (std::wstring(command.name) + (command.html ? L".html" : L".txt"));
            const auto logPath = command.html ? folder / (std::wstring(command.name) + L"-command.txt") : output;
            file.path = logPath.wstring();
            try
            {
                SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
                Handle log(CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &security, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (log == INVALID_HANDLE_VALUE)
                    throw Error(GetLastError(), L"创建电源诊断输出");
                Handle input(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
                if (input == INVALID_HANDLE_VALUE)
                    throw Error(GetLastError(), L"打开电源诊断标准输入");
                STARTUPINFOEXW startup{};
                startup.StartupInfo.cb = sizeof(startup);
                startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
                startup.StartupInfo.wShowWindow = SW_HIDE;
                startup.StartupInfo.hStdOutput = log;
                startup.StartupInfo.hStdError = log;
                startup.StartupInfo.hStdInput = input;
                SIZE_T attributeBytes = 0;
                InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
                std::vector<unsigned char> storage(attributeBytes);
                startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
                if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes))
                    throw Error(GetLastError(), L"初始化工具进程");
                struct AttributeCloser
                {
                    LPPROC_THREAD_ATTRIBUTE_LIST value;
                    ~AttributeCloser() { DeleteProcThreadAttributeList(value); }
                } attributes{startup.lpAttributeList};
                HANDLE inherited[]{log, input};
                if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, sizeof(inherited), nullptr, nullptr))
                    throw Error(GetLastError(), L"配置工具输出");
                std::wstring commandLine = L"\"" + executable.wstring() + L"\" " + command.arguments;
                if (command.html)
                    commandLine += L" /output \"" + output.wstring() + L"\"";
                PROCESS_INFORMATION process{};
                if (!CreateProcessW(executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                                    nullptr, folder.c_str(), &startup.StartupInfo, &process))
                {
                    file.state = CollectionState::Failed;
                    file.error = GetLastError();
                    file.message = SystemError(file.error);
                }
                else
                {
                    Handle processHandle(process.hProcess), threadHandle(process.hThread);
                    const auto began = GetTickCount64();
                    DWORD wait = WAIT_TIMEOUT;
                    while ((wait = WaitForSingleObject(processHandle, 100)) == WAIT_TIMEOUT)
                    {
                        if ((context.cancel && context.cancel->load()) || GetTickCount64() - began >= command.timeoutMs)
                        {
                            const bool cancelled = context.cancel && context.cancel->load();
                            TerminateProcess(processHandle, cancelled ? ERROR_CANCELLED : ERROR_TIMEOUT);
                            WaitForSingleObject(processHandle, 5000);
                            file.error = cancelled ? ERROR_CANCELLED : ERROR_TIMEOUT;
                            file.message = cancelled ? L"电源诊断已取消。" :
                                L"电源诊断工具超时（" + std::to_wstring(command.timeoutMs) + L" 毫秒）；详细输出：" + logPath.wstring();
                            break;
                        }
                    }
                    DWORD code = 0;
                    if (!file.error && (wait != WAIT_OBJECT_0 || !GetExitCodeProcess(processHandle, &code)))
                    {
                        file.error = GetLastError();
                        if (!file.error)
                            file.error = ERROR_GEN_FAILURE;
                    }
                    if (!file.error && code)
                    {
                        file.error = code;
                        file.message = (code == ERROR_NOT_SUPPORTED || code == ERROR_CALL_NOT_IMPLEMENTED ?
                            L"系统工具明确报告不支持此报告，详细输出：" : L"工具返回失败，详细输出：") + logPath.wstring();
                    }
                    std::error_code sizeError;
                    const auto bytes = fs::file_size(output, sizeError);
                    if (!file.error && (sizeError || !bytes))
                    {
                        file.error = sizeError ? static_cast<DWORD>(sizeError.value()) : ERROR_INVALID_DATA;
                        file.message = L"工具没有生成非空的预期报告，详细输出：" + logPath.wstring();
                    }
                    if (!file.error)
                        file.path = output.wstring();
                    else if (file.message.empty())
                        file.message = SystemError(file.error) + L"；详细输出：" + logPath.wstring();
                }
            }
            catch (const Error &error)
            {
                file.error = error.code;
                file.message = error.text;
            }
            file.state = !file.error ? CollectionState::Saved : file.error == ERROR_CANCELLED ? CollectionState::Cancelled :
                (file.error == ERROR_NOT_SUPPORTED || file.error == ERROR_CALL_NOT_IMPLEMENTED) ? CollectionState::Skipped : CollectionState::Failed;
            // A timed-out/cancelled tool may leave an incomplete HTML; never present it as a report.
            if (file.error && command.html)
                DeleteFileW(output.c_str());
            WIN32_FILE_ATTRIBUTE_DATA attributes{};
            if (Attributes(file.path, attributes))
            {
                file.bytes = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
                file.modifiedUtc = DiagnosticUtc(FileTimeValue(attributes.ftLastWriteTime));
            }
            const bool cancelled = file.state == CollectionState::Cancelled;
            item.files.push_back(std::move(file));
            if (cancelled)
            {
                FinishIndex(item, context, metadata);
                throw Error(ERROR_CANCELLED, L"电源诊断已取消");
            }
        }
        FinishIndex(item, context, metadata);
    }
    void CollectDiagnostic(CollectedLog &item, DiagnosticContext &context, const CollectionResult &result)
    {
        CheckDiagnosticCancel(context);
        if (item.channel == L"CrashDumps")
            CollectDumpFiles(item, context, false);
        else if (item.channel == L"LiveKernelReports")
            CollectDumpFiles(item, context, true);
        else if (item.channel == L"WerReports")
            CollectWerReports(item, context);
        else if (item.channel == L"SystemInfo")
            CollectSystemSnapshot(item, context);
        else if (item.channel == L"Devices")
            CollectDevices(item, context);
        else if (item.channel == L"Drivers")
            CollectDrivers(item, context);
        else if (item.channel == L"DumpConfiguration")
            CollectDumpConfiguration(item, context);
        else if (item.channel == L"DriverInstallation")
            CollectDriverInstallation(item, context);
        else if (item.channel == L"DiagnosticEvents")
            CollectDiagnosticEvents(item, context);
        else if (item.channel == L"Timeline")
            CollectTimeline(item, context, result);
        else if (item.channel == L"PowerReports")
            CollectPowerReports(item, context);
        else
            throw Error(ERROR_INVALID_PARAMETER, L"未知的诊断采集项");
    }
}
