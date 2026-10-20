#include "EventLogService.h"
#include <winevt.h>
#include <sddl.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <map>
#include <memory>
#include <sstream>
#include <utility>

namespace elm {
namespace {
class EvtHandle {
public:
    explicit EvtHandle(EVT_HANDLE handle = nullptr) : value(handle) {}
    ~EvtHandle() { if (value) EvtClose(value); }
    EvtHandle(const EvtHandle&) = delete;
    EvtHandle& operator=(const EvtHandle&) = delete;
    EvtHandle(EvtHandle&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    operator EVT_HANDLE() const { return value; }
private:
    EVT_HANDLE value;
};

void CheckCancel(const std::atomic_bool* cancel) {
    if (cancel && cancel->load()) throw Error(ERROR_CANCELLED, L"Operation cancelled");
}

std::wstring VariantString(const EVT_VARIANT& value) {
    return value.Type == EvtVarTypeString && value.StringVal ? value.StringVal : L"";
}

std::wstring FormatTime(uint64_t ticks, bool utc) {
    ULARGE_INTEGER value{};
    value.QuadPart = ticks;
    FILETIME ft{ value.LowPart, value.HighPart };
    SYSTEMTIME st{}, local{};
    if (!FileTimeToSystemTime(&ft, &st)) return L"";
    if (!utc && SystemTimeToTzSpecificLocalTime(nullptr, &st, &local)) st = local;
    wchar_t buffer[48]{};
    swprintf_s(buffer, utc ? L"%04u-%02u-%02uT%02u:%02u:%02u.%03uZ" : L"%04u-%02u-%02u %02u:%02u:%02u.%03u",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return buffer;
}

std::wstring RenderXml(EVT_HANDLE event) {
    DWORD size = 0, properties = 0;
    if (!EvtRender(nullptr, event, EvtRenderEventXml, 0, nullptr, &size, &properties) &&
        GetLastError() != ERROR_INSUFFICIENT_BUFFER) throw Error(GetLastError(), L"EvtRender XML");
    std::vector<wchar_t> buffer(size / sizeof(wchar_t) + 1);
    if (!EvtRender(nullptr, event, EvtRenderEventXml, size, buffer.data(), &size, &properties))
        throw Error(GetLastError(), L"EvtRender XML");
    return buffer.data();
}

class Renderer {
public:
    explicit Renderer(const Query& query)
        : context(EvtCreateRenderContext(0, nullptr, EvtRenderContextSystem)),
          archive(query.file ? query.source : L"") {
        if (!context) throw Error(GetLastError(), L"EvtCreateRenderContext");
    }
    Event Render(EVT_HANDLE handle) {
        DWORD size = 0, count = 0;
        if (!EvtRender(context, handle, EvtRenderEventValues, 0, nullptr, &size, &count) &&
            GetLastError() != ERROR_INSUFFICIENT_BUFFER) throw Error(GetLastError(), L"EvtRender values");
        // EVT_VARIANT storage guarantees alignment, including the trailing variable-size data.
        std::vector<EVT_VARIANT> storage((size + sizeof(EVT_VARIANT) - 1) / sizeof(EVT_VARIANT));
        if (!EvtRender(context, handle, EvtRenderEventValues, size, storage.data(), &size, &count))
            throw Error(GetLastError(), L"EvtRender values");
        if (count < EvtSystemPropertyIdEND) throw Error(ERROR_INVALID_DATA, L"Incomplete event system data");
        const auto* values = storage.data();
        Event result;
        result.provider = VariantString(values[EvtSystemProviderName]);
        result.channel = VariantString(values[EvtSystemChannel]);
        result.computer = VariantString(values[EvtSystemComputer]);
        if (values[EvtSystemEventID].Type != EvtVarTypeNull) result.eventId = values[EvtSystemEventID].UInt16Val;
        if (values[EvtSystemLevel].Type != EvtVarTypeNull) result.level = values[EvtSystemLevel].ByteVal;
        if (values[EvtSystemEventRecordId].Type != EvtVarTypeNull) result.recordId = values[EvtSystemEventRecordId].UInt64Val;
        if (values[EvtSystemTimeCreated].Type != EvtVarTypeNull) {
            result.time = FormatTime(values[EvtSystemTimeCreated].FileTimeVal, false);
            result.timeUtc = FormatTime(values[EvtSystemTimeCreated].FileTimeVal, true);
        }
        if (values[EvtSystemUserID].Type == EvtVarTypeSid && values[EvtSystemUserID].SidVal) {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(values[EvtSystemUserID].SidVal, &sid)) {
                result.user = sid;
                LocalFree(sid);
            }
        }
        result.xml = RenderXml(handle);
        auto found = publishers.find(result.provider);
        if (found == publishers.end()) {
            EVT_HANDLE metadata = EvtOpenPublisherMetadata(nullptr, result.provider.c_str(),
                archive.empty() ? nullptr : archive.c_str(), 0, 0);
            if (!metadata && !archive.empty())
                metadata = EvtOpenPublisherMetadata(nullptr, result.provider.c_str(), nullptr, 0, 0);
            found = publishers.emplace(result.provider, EvtHandle(metadata)).first;
        }
        DWORD used = 0;
        EvtFormatMessage(found->second, handle, 0, 0, nullptr, EvtFormatMessageEvent, 0, nullptr, &used);
        if (used) {
            std::vector<wchar_t> message(used + 1);
            if (EvtFormatMessage(found->second, handle, 0, 0, nullptr, EvtFormatMessageEvent,
                                 static_cast<DWORD>(message.size()), message.data(), &used))
                result.message = message.data();
        }
        if (result.message.empty())
            result.message = L"无法加载此事件的消息资源。请查看 XML 获取完整事件数据。 / Message resource unavailable; see XML.";
        return result;
    }
private:
    EvtHandle context;
    std::wstring archive;
    std::map<std::wstring, EvtHandle> publishers;
};

std::wstring CsvField(std::wstring value) {
    // Prevent spreadsheet formula execution when opening exported CSV files.
    if (!value.empty() && (value[0] == L'=' || value[0] == L'+' || value[0] == L'-' ||
                          value[0] == L'@' || value[0] == L'\t' || value[0] == L'\r' || value[0] == L'\n'))
        value.insert(value.begin(), L'\'');
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        if (ch == L'\"') result += L'\"';
        result += ch;
    }
    return result + L"\"";
}

std::wstring SingleLine(std::wstring value) {
    for (auto& ch : value) if (ch < 32 || ch == 127) ch = L' ';
    return value;
}

bool SameFile(const std::wstring& left, const std::wstring& right) {
    if (_wcsicmp(left.c_str(), right.c_str()) == 0) return true;
    // Resolve short names, junctions and hard links by file identity as well as spelling.
    HANDLE a = CreateFileW(left.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    HANDLE b = CreateFileW(right.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    BY_HANDLE_FILE_INFORMATION ai{}, bi{};
    const bool same = a != INVALID_HANDLE_VALUE && b != INVALID_HANDLE_VALUE &&
        GetFileInformationByHandle(a, &ai) && GetFileInformationByHandle(b, &bi) &&
        ai.dwVolumeSerialNumber == bi.dwVolumeSerialNumber && ai.nFileIndexHigh == bi.nFileIndexHigh && ai.nFileIndexLow == bi.nFileIndexLow;
    if (a != INVALID_HANDLE_VALUE) CloseHandle(a);
    if (b != INVALID_HANDLE_VALUE) CloseHandle(b);
    return same;
}

class Destination {
public:
    Destination(const Query& query, const std::wstring& path, bool replace)
        : target(AbsolutePath(path)), overwrite(replace) {
        if (query.file && SameFile(AbsolutePath(query.source), target))
            throw Error(ERROR_INVALID_PARAMETER, L"Output must differ from the source file");
        if (!overwrite && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
            throw Error(ERROR_FILE_EXISTS, L"Output already exists: " + target);
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) throw Error(ERROR_GEN_FAILURE, L"Create temporary filename");
        wchar_t id[40]{};
        StringFromGUID2(guid, id, 40);
        temporary = target + L"." + id + L".tmp.evtx";
    }
    ~Destination() { if (!committed) DeleteFileW(temporary.c_str()); }
    void Commit() {
        const DWORD flags = MOVEFILE_WRITE_THROUGH | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
        if (!MoveFileExW(temporary.c_str(), target.c_str(), flags))
            throw Error(GetLastError(), L"Commit output: " + target);
        committed = true;
    }
    std::wstring temporary;
private:
    std::wstring target;
    bool overwrite;
    bool committed = false;
};

class FileWriter {
public:
    explicit FileWriter(const std::wstring& path) {
        handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE) throw Error(GetLastError(), L"Create output: " + path);
    }
    ~FileWriter() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    void Write(const std::wstring& text) {
        const std::string bytes = Utf8(text);
        size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            DWORD chunk = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1024 * 1024));
            if (!WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr))
                throw Error(GetLastError(), L"Write output");
            if (!written) throw Error(ERROR_WRITE_FAULT, L"Write output");
            offset += written;
        }
    }
    void Finish() {
        if (!FlushFileBuffers(handle)) throw Error(GetLastError(), L"Flush output");
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
private:
    HANDLE handle = INVALID_HANDLE_VALUE;
};
} // namespace

std::wstring SystemError(DWORD code) {
    LPWSTR buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result = buffer ? buffer : L"Windows API error";
    if (buffer) LocalFree(buffer);
    while (!result.empty() && iswspace(result.back())) result.pop_back();
    return result;
}

Error::Error(DWORD value, const std::wstring& context)
    : std::runtime_error("Windows event log operation failed"), code(value),
      text(context + L"\r\n" + SystemError(value) + L" (" + std::to_wstring(value) + L")") {}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring AbsolutePath(const std::wstring& path) {
    if (path.empty()) throw Error(ERROR_INVALID_PARAMETER, L"Empty file path");
    DWORD size = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (!size) throw Error(GetLastError(), L"Resolve path");
    std::vector<wchar_t> buffer(size);
    DWORD used = GetFullPathNameW(path.c_str(), size, buffer.data(), nullptr);
    if (!used || used >= size) throw Error(ERROR_INVALID_NAME, L"Resolve path");
    return buffer.data();
}

std::wstring LevelName(unsigned level, bool chinese) {
    static const wchar_t* en[] = {L"LogAlways", L"Critical", L"Error", L"Warning", L"Information", L"Verbose"};
    static const wchar_t* zh[] = {L"未指定", L"关键", L"错误", L"警告", L"信息", L"详细"};
    return level < 6 ? (chinese ? zh[level] : en[level]) : std::to_wstring(level);
}

std::vector<std::wstring> Channels() {
    EvtHandle enumeration(EvtOpenChannelEnum(nullptr, 0));
    if (!enumeration) throw Error(GetLastError(), L"EvtOpenChannelEnum");
    std::vector<std::wstring> result;
    for (;;) {
        DWORD used = 0;
        if (!EvtNextChannelPath(enumeration, 0, nullptr, &used)) {
            DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_ITEMS) break;
            if (code != ERROR_INSUFFICIENT_BUFFER) throw Error(code, L"EvtNextChannelPath");
        }
        std::vector<wchar_t> buffer(used + 1);
        if (!EvtNextChannelPath(enumeration, static_cast<DWORD>(buffer.size()), buffer.data(), &used))
            throw Error(GetLastError(), L"EvtNextChannelPath");
        result.emplace_back(buffer.data());
    }
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return result;
}

std::wstring BuildXPath(const Query& query) {
    if (!query.xpath.empty()) {
        if (query.level >= 0 || query.eventId >= 0 || !query.provider.empty() || query.hours)
            throw Error(ERROR_INVALID_PARAMETER, L"--query cannot be combined with other filters");
        return query.xpath;
    }
    std::vector<std::wstring> terms;
    if (query.level >= 0) terms.push_back(L"Level=" + std::to_wstring(query.level));
    if (query.eventId >= 0) terms.push_back(L"EventID=" + std::to_wstring(query.eventId));
    if (!query.provider.empty()) {
        wchar_t quote = query.provider.find(L'\'') == std::wstring::npos ? L'\'' : L'\"';
        if (query.provider.find(quote) != std::wstring::npos)
            throw Error(ERROR_INVALID_PARAMETER, L"Provider name cannot contain both quote characters");
        terms.push_back(L"Provider[@Name=" + std::wstring(1, quote) + query.provider + quote + L"]");
    }
    if (query.hours) terms.push_back(L"TimeCreated[timediff(@SystemTime) <= " + std::to_wstring(query.hours * 3600000ULL) + L"]");
    if (terms.empty()) return L"*";
    std::wstring result = L"*[System[";
    for (size_t i = 0; i < terms.size(); ++i) { if (i) result += L" and "; result += terms[i]; }
    return result + L"]]";
}

size_t Read(const Query& input, const std::function<void(Event&&)>& visitor, const std::atomic_bool* cancel) {
    Query query = input;
    if (query.file) query.source = AbsolutePath(query.source);
    const auto xpath = BuildXPath(query);
    CheckCancel(cancel);
    EvtHandle results(EvtQuery(nullptr, query.source.c_str(), xpath.c_str(),
        (query.file ? EvtQueryFilePath : EvtQueryChannelPath) | EvtQueryReverseDirection));
    if (!results) throw Error(GetLastError(), L"Query: " + query.source);
    Renderer renderer(query);
    size_t count = 0;
    while (!query.limit || count < query.limit) {
        CheckCancel(cancel);
        struct Batch {
            EVT_HANDLE values[16]{};
            ~Batch() { for (auto handle : values) if (handle) EvtClose(handle); }
        } batch;
        DWORD returned = 0;
        const DWORD requested = static_cast<DWORD>(query.limit ? std::min<size_t>(16, query.limit - count) : 16);
        if (!EvtNext(results, requested, batch.values, 5000, 0, &returned)) {
            DWORD code = GetLastError();
            if (code == ERROR_NO_MORE_ITEMS) break;
            throw Error(code, L"Read events: " + query.source);
        }
        for (DWORD i = 0; i < returned; ++i) {
            CheckCancel(cancel);
            visitor(renderer.Render(batch.values[i]));
            ++count;
        }
    }
    return count;
}

std::wstring JsonQuote(const std::wstring& value) {
    std::wstring result = L"\"";
    for (wchar_t ch : value) {
        switch (ch) {
        case L'\"': result += L"\\\""; break;
        case L'\\': result += L"\\\\"; break;
        case L'\r': result += L"\\r"; break;
        case L'\n': result += L"\\n"; break;
        case L'\t': result += L"\\t"; break;
        default:
            if (ch < 32) { wchar_t escaped[7]{}; swprintf_s(escaped, L"\\u%04x", static_cast<unsigned>(ch)); result += escaped; }
            else result += ch;
        }
    }
    return result + L"\"";
}

size_t Serialize(const Query& query, Format format, const Sink& sink, const std::atomic_bool* cancel) {
    if (format == Format::Evtx) throw Error(ERROR_INVALID_PARAMETER, L"EVTX requires a file destination");
    if (format == Format::Json) sink(L"[\n");
    if (format == Format::Xml) sink(L"<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<Events>\n");
    if (format == Format::Csv) sink(L"TimeUtc,RecordId,EventId,Level,Provider,Channel,Computer,User,Message\r\n");
    if (format == Format::Table) sink(L"Time (local)\tRecordId\tEventId\tLevel\tProvider\tMessage\n");
    bool first = true;
    size_t count = Read(query, [&](Event&& event) {
        const auto record = std::to_wstring(event.recordId);
        const auto id = std::to_wstring(event.eventId);
        if (format == Format::Json) {
            sink(std::wstring(first ? L"" : L",\n") + L"  {\"timeUtc\":" + JsonQuote(event.timeUtc) +
                L",\"recordId\":" + record + L",\"eventId\":" + id + L",\"level\":" + std::to_wstring(event.level) +
                L",\"levelName\":" + JsonQuote(LevelName(event.level)) + L",\"provider\":" + JsonQuote(event.provider) +
                L",\"channel\":" + JsonQuote(event.channel) + L",\"computer\":" + JsonQuote(event.computer) +
                L",\"user\":" + JsonQuote(event.user) + L",\"message\":" + JsonQuote(event.message) +
                L",\"xml\":" + JsonQuote(event.xml) + L"}");
        } else if (format == Format::Xml) sink(event.xml + L"\n");
        else if (format == Format::Csv) {
            sink(CsvField(event.timeUtc) + L"," + record + L"," + id + L"," + CsvField(LevelName(event.level)) + L"," +
                CsvField(event.provider) + L"," + CsvField(event.channel) + L"," + CsvField(event.computer) + L"," +
                CsvField(event.user) + L"," + CsvField(event.message) + L"\r\n");
        } else {
            sink(event.time + L"\t" + record + L"\t" + id + L"\t" + LevelName(event.level) + L"\t" +
                SingleLine(event.provider) + L"\t" + SingleLine(event.message) + L"\n");
        }
        first = false;
    }, cancel);
    if (format == Format::Json) sink(L"\n]\n");
    if (format == Format::Xml) sink(L"</Events>\n");
    return count;
}

size_t Export(const Query& query, const std::wstring& path, Format format, bool overwrite, const std::atomic_bool* cancel) {
    CheckCancel(cancel);
    Destination destination(query, path, overwrite);
    size_t count = 0;
    if (format == Format::Evtx) {
        if (query.limit) throw Error(ERROR_INVALID_PARAMETER, L"EVTX exports all matching events; use --limit 0");
        const auto xpath = BuildXPath(query);
        const auto source = query.file ? AbsolutePath(query.source) : query.source;
        if (!EvtExportLog(nullptr, source.c_str(), xpath.c_str(), destination.temporary.c_str(),
                          query.file ? EvtExportLogFilePath : EvtExportLogChannelPath))
            throw Error(GetLastError(), L"Export EVTX: " + query.source);
    } else {
        FileWriter writer(destination.temporary);
        if (format == Format::Csv) writer.Write(L"\ufeff");
        count = Serialize(query, format, [&](const std::wstring& text) { writer.Write(text); }, cancel);
        writer.Finish();
    }
    CheckCancel(cancel);
    destination.Commit();
    return count;
}

void Clear(const std::wstring& channel, const std::wstring& backup) {
    if (channel.empty()) throw Error(ERROR_INVALID_PARAMETER, L"A channel is required");
    const std::wstring absolute = backup.empty() ? L"" : AbsolutePath(backup);
    if (!absolute.empty() && GetFileAttributesW(absolute.c_str()) != INVALID_FILE_ATTRIBUTES)
        throw Error(ERROR_FILE_EXISTS, L"Backup already exists: " + absolute);
    if (!EvtClearLog(nullptr, channel.c_str(), absolute.empty() ? nullptr : absolute.c_str(), 0))
        throw Error(GetLastError(), L"Clear channel: " + channel);
}

bool IsElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}

void EnableSecurityPrivilege() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    if (LookupPrivilegeValueW(nullptr, SE_SECURITY_NAME, &privileges.Privileges[0].Luid)) {
        privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        AdjustTokenPrivileges(token, FALSE, &privileges, sizeof(privileges), nullptr, nullptr);
    }
    CloseHandle(token);
}

} // namespace elm
