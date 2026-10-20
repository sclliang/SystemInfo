#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace elm {

class Error : public std::runtime_error {
public:
    Error(DWORD code, const std::wstring& context);
    DWORD code;
    std::wstring text;
};

struct Query {
    std::wstring source = L"System";
    bool file = false;
    std::wstring xpath;
    int level = -1;
    int eventId = -1;
    std::wstring provider;
    uint64_t hours = 0;
    size_t limit = 200; // Zero means all matching events.
};

struct Event {
    uint64_t recordId = 0;
    unsigned eventId = 0;
    unsigned level = 0;
    std::wstring time;    // Local time, for the GUI.
    std::wstring timeUtc; // ISO 8601, for interchange formats.
    std::wstring provider;
    std::wstring channel;
    std::wstring computer;
    std::wstring user;
    std::wstring message;
    std::wstring xml;
};

enum class Format { Table, Csv, Json, Xml, Evtx };
using Sink = std::function<void(const std::wstring&)>;

std::vector<std::wstring> Channels();
std::wstring BuildXPath(const Query& query);
size_t Read(const Query& query, const std::function<void(Event&&)>& visitor,
            const std::atomic_bool* cancel = nullptr);
size_t Serialize(const Query& query, Format format, const Sink& sink,
                 const std::atomic_bool* cancel = nullptr);
size_t Export(const Query& query, const std::wstring& path, Format format,
              bool overwrite = false, const std::atomic_bool* cancel = nullptr);
void Clear(const std::wstring& channel, const std::wstring& backup = L"");
std::wstring AbsolutePath(const std::wstring& path);
std::wstring LevelName(unsigned level, bool chinese = false);
std::wstring JsonQuote(const std::wstring& value);
std::string Utf8(const std::wstring& value);
std::wstring SystemError(DWORD code);
bool IsElevated();
void EnableSecurityPrivilege();

} // namespace elm
