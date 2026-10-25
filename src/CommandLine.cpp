#include "CommandLine.h"
#include "EventLogService.h"
#include "Collector.h"
#include <fluentui/fluentui.hpp>
#include <algorithm>
#include <map>
#include <set>
#include <cwctype>

namespace elm
{
    namespace
    {
        class UsageError : public std::exception
        {
        public:
            explicit UsageError(std::wstring value) : message(std::move(value)) {}
            std::wstring message;
        };

        void AttachOutput()
        {
            // AttachConsole may replace inherited pipe/file handles. Preserve redirection.
            HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE), err = GetStdHandle(STD_ERROR_HANDLE);
            const bool keepOut = out && out != INVALID_HANDLE_VALUE && GetFileType(out) != FILE_TYPE_UNKNOWN;
            const bool keepErr = err && err != INVALID_HANDLE_VALUE && GetFileType(err) != FILE_TYPE_UNKNOWN;
            AttachConsole(ATTACH_PARENT_PROCESS);
            if (keepOut)
                SetStdHandle(STD_OUTPUT_HANDLE, out);
            if (keepErr)
                SetStdHandle(STD_ERROR_HANDLE, err);
        }

        void WriteOutput(const std::wstring &value, bool error = false)
        {
            HANDLE handle = GetStdHandle(error ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
            if (!handle || handle == INVALID_HANDLE_VALUE)
                return;
            DWORD mode = 0;
            if (GetConsoleMode(handle, &mode))
            {
                size_t offset = 0;
                while (offset < value.size())
                {
                    DWORD written = 0;
                    DWORD length = static_cast<DWORD>(std::min<size_t>(value.size() - offset, 16000));
                    if (!WriteConsoleW(handle, value.data() + offset, length, &written, nullptr))
                        throw Error(GetLastError(), L"Write console");
                    if (!written)
                        throw Error(ERROR_WRITE_FAULT, L"Write console");
                    offset += written;
                }
            }
            else
            {
                auto bytes = Utf8(value);
                size_t offset = 0;
                while (offset < bytes.size())
                {
                    DWORD written = 0;
                    DWORD length = static_cast<DWORD>(std::min<size_t>(bytes.size() - offset, 1024 * 1024));
                    if (!WriteFile(handle, bytes.data() + offset, length, &written, nullptr))
                        throw Error(GetLastError(), L"Write stdout/stderr");
                    if (!written)
                        throw Error(ERROR_WRITE_FAULT, L"Write stdout/stderr");
                    offset += written;
                }
            }
        }

        uint64_t Number(const std::wstring &text, const std::wstring &option, uint64_t maximum)
        {
            if (text.empty() || text.find_first_not_of(L"0123456789") != std::wstring::npos)
                throw UsageError(option + L" requires an unsigned integer.");
            uint64_t value = 0;
            for (wchar_t ch : text)
            {
                if (value > (maximum - static_cast<unsigned>(ch - L'0')) / 10)
                    throw UsageError(option + L" is out of range.");
                value = value * 10 + (ch - L'0');
            }
            if (value > maximum)
                throw UsageError(option + L" is out of range.");
            return value;
        }

        Format ParseFormat(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
                           { return static_cast<wchar_t>(towlower(ch)); });
            if (value == L"table")
                return Format::Table;
            if (value == L"csv")
                return Format::Csv;
            if (value == L"json")
                return Format::Json;
            if (value == L"xml")
                return Format::Xml;
            if (value == L"evtx")
                return Format::Evtx;
            throw UsageError(L"Unknown format: " + value);
        }

        int ParseLevel(std::wstring value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch)
                           { return static_cast<wchar_t>(towlower(ch)); });
            const std::map<std::wstring, int> levels = {{L"all", -1}, {L"0", 0}, {L"critical", 1}, {L"1", 1}, {L"error", 2}, {L"2", 2}, {L"warning", 3}, {L"3", 3}, {L"information", 4}, {L"info", 4}, {L"4", 4}, {L"verbose", 5}, {L"5", 5}};
            auto found = levels.find(value);
            if (found == levels.end())
                throw UsageError(L"Unknown level: " + value);
            return found->second;
        }
    } // namespace

    std::wstring CommandLineHelp()
    {
        return LR"HELP(EventLogCollector 1.2.0.0
Windows Event Log and Diagnostic Collector (Win32 + FluentUI)

Usage:
  EventLogCollector.exe                         Collect logs and diagnostics with progress UI
  EventLogCollector.exe gui                     Collect logs and diagnostics with progress UI
  EventLogCollector.exe collect [--output DIR] [--days N] [--include-large-dumps] [--no-power | --energy] [--json]
  EventLogCollector.exe channels [--json]        List local event channels
  EventLogCollector.exe list [options]           Display events, newest first
  EventLogCollector.exe export [options]         Export matching events
  EventLogCollector.exe clear --channel NAME --yes [--backup PATH.evtx]
  EventLogCollector.exe --help | --version | --licenses

Collection:
  Collect five complete event channels, recent dumps and WER reports, hardware,
  devices, drivers, dump settings, installation logs, diagnostic channels, a timeline
  and applicable powercfg HTML reports (system power, battery, Modern Standby).
  Each run creates a NEW Collection_timestamp directory under LOG next to the EXE.
  --output DIR       Override the parent directory (created if needed)
  --days N           Recent file/diagnostic event window, 1..365 (default 7)
                     The five standard event channels are always exported in full.
                     Power HTML reports use this window; power transitions/SleepStudy
                     are capped at 28 days. Actual history depends on Windows retention.
  --include-large-dumps
                     Include configured system dump; raise dump limits from 64/256 MiB
                     per-file/total to 4/8 GiB. Skipped files remain in the index.
  --no-power         Skip all power reports (otherwise enabled; 60 s per command)
  --power            Compatibility flag: power reports are now enabled by default
  --energy           Also sample energy efficiency for 60 s (120 s timeout), last.
                     Run with the computer idle. Cannot be combined with --no-power.
  --json             Emit the result also saved as manifest.json (schema version 2)
  Missing optional evidence and documented skips are not collection failures.

Source (list/export):
  --channel NAME      Local channel, default: System
  --file PATH.evtx    Read an offline EVTX file (exclusive with --channel)

Filters (list/export):
  --level LEVEL       all, critical, error, warning, information, verbose, 0..5
  --id NUMBER         Event ID (0..65535)
  --provider NAME     Exact provider name
  --hours NUMBER      Events within the last N hours (0 = unrestricted)
  --query XPATH       Custom query; exclusive with the other filters
  --limit NUMBER      list: 200 by default; export: 0 (all) by default
                      EVTX requires 0. Text formats stream without buffering.

Output:
  --format FORMAT    list: table/json/csv/xml (default table)
                     export: evtx/csv/json/xml (inferred from file extension)
  --output PATH      Required for export; optional for list
  --overwrite        Explicitly allow replacing an existing output file
                     An input EVTX file cannot be overwritten.
  --backup PATH      With clear: back up and clear in one Windows API call
                     Backup must be a NEW .evtx file; never overwritten.
  --yes              Required for clear; clears the ENTIRE specified channel

Examples:
  EventLogCollector.exe channels --json
  EventLogCollector.exe list --channel System --level error --limit 50
  EventLogCollector.exe list --file "D:\Logs\system.evtx" --format json
  EventLogCollector.exe export --channel Application --output "D:\Logs\app.evtx"
  EventLogCollector.exe export --channel System --hours 24 --output "D:\Logs\system.csv"
  EventLogCollector.exe clear --channel Application --backup "D:\Logs\before-clear.evtx" --yes

Exit codes: 0 success; 2 usage; 3 access denied; 4 operation failed; 5 cancelled.
Redirected output is UTF-8. File CSV includes a BOM and escapes spreadsheet formulas.
JSON uses UTC timestamps and includes the original event XML.
Run elevated to access restricted logs or clear protected channels.
The release EXE requires administrator rights. Run CLI from an elevated terminal.
In interactive cmd.exe, use start /wait to wait for this GUI executable.
In PowerShell, pipe stdout to Out-String when invoking interactively.
)HELP";
    }

    int RunCommandLine(const std::vector<std::wstring> &args)
    {
        AttachOutput();
        try
        {
            if (args.empty())
                return 0;
            const auto &command = args[0];
            if ((command == L"--help" || command == L"-h" || command == L"/?" || command == L"help") && args.size() == 1)
            {
                WriteOutput(CommandLineHelp());
                return 0;
            }
            if (command == L"--version" && args.size() == 1)
            {
                WriteOutput(L"EventLogCollector 1.2.0.0\n");
                return 0;
            }
            if (command == L"--licenses" && args.size() == 1)
            {
                WriteOutput(std::wstring(fluent::thirdPartyNotices()));
                return 0;
            }
            if (command != L"channels" && command != L"list" && command != L"export" && command != L"clear" && command != L"collect")
                throw UsageError(L"Unknown command: " + command);
            if (args.size() == 2 && args[1] == L"--help")
            {
                WriteOutput(CommandLineHelp());
                return 0;
            }

            const std::set<std::wstring> flags{L"--yes", L"--overwrite", L"--json", L"--include-large-dumps", L"--power", L"--no-power", L"--energy"};
            const std::set<std::wstring> known{L"--channel", L"--file", L"--level", L"--id", L"--provider", L"--hours",
                                               L"--query", L"--limit", L"--format", L"--output", L"--backup", L"--yes", L"--overwrite", L"--json",
                                               L"--days", L"--include-large-dumps", L"--power", L"--no-power", L"--energy"};
            std::map<std::wstring, std::wstring> options;
            for (size_t i = 1; i < args.size(); ++i)
            {
                const auto key = args[i];
                if (!known.count(key))
                    throw UsageError(L"Unknown option: " + key);
                if (options.count(key))
                    throw UsageError(L"Duplicate option: " + key);
                if (flags.count(key))
                    options[key] = L"true";
                else
                {
                    if (i + 1 >= args.size() || args[i + 1].rfind(L"--", 0) == 0 || args[i + 1].empty())
                        throw UsageError(L"Missing value for " + key);
                    options[key] = args[++i];
                }
            }
            auto has = [&](const wchar_t *key)
            { return options.count(key) != 0; };
            const std::set<std::wstring> readOptions{L"--channel", L"--file", L"--level", L"--id", L"--provider", L"--hours",
                                                     L"--query", L"--limit", L"--format", L"--output", L"--overwrite"};
            for (const auto &pair : options)
            {
                bool allowed = command == L"collect"    ? (pair.first == L"--output" || pair.first == L"--json" ||
                                                           pair.first == L"--days" || pair.first == L"--include-large-dumps" || pair.first == L"--power" ||
                                                           pair.first == L"--no-power" || pair.first == L"--energy")
                               : command == L"channels" ? pair.first == L"--json"
                               : command == L"clear"    ? (pair.first == L"--channel" || pair.first == L"--yes" || pair.first == L"--backup")
                                                        : readOptions.count(pair.first) != 0;
                if (!allowed)
                    throw UsageError(pair.first + L" is not valid for " + command);
            }
            if (command == L"collect")
            {
                CollectionOptions collection;
                if (has(L"--days"))
                {
                    collection.days = static_cast<unsigned>(Number(options[L"--days"], L"--days", 365));
                    if (!collection.days)
                        throw UsageError(L"--days must be between 1 and 365.");
                }
                collection.includeLargeDumps = has(L"--include-large-dumps");
                if (has(L"--no-power") && (has(L"--power") || has(L"--energy")))
                    throw UsageError(L"--no-power cannot be combined with --power or --energy.");
                collection.powerReports = !has(L"--no-power");
                collection.energyReport = has(L"--energy");
                const auto result = CollectWindowsLogs(has(L"--output") ? options[L"--output"] : L"", {}, nullptr, collection);
                if (has(L"--json"))
                    WriteOutput(CollectionJson(result));
                else
                {
                    for (const auto *item : CollectionEntries(result))
                        WriteOutput(L"[" + CollectionStateName(item->state) + L"] " + item->channel + L": " +
                                    (item->state == CollectionState::Saved ? item->path : item->message) + L"\n");
                    WriteOutput(L"Saved " + std::to_wstring(result.saved()) + L" / " + std::to_wstring(result.total()) +
                                L" items. Directory: " + result.directory + L"\nManifest: " + result.manifestPath + L"\n");
                }
                return result.success() ? 0 : result.cancelled ? 5
                                                               : 4;
            }
            if (command == L"channels")
            {
                const auto names = Channels();
                if (has(L"--json"))
                {
                    WriteOutput(L"[\n");
                    for (size_t i = 0; i < names.size(); ++i)
                        WriteOutput(L"  " + JsonQuote(names[i]) + (i + 1 == names.size() ? L"\n" : L",\n"));
                    WriteOutput(L"]\n");
                }
                else
                    for (const auto &name : names)
                        WriteOutput(name + L"\n");
                return 0;
            }
            if (command == L"clear")
            {
                if (!has(L"--channel") || !has(L"--yes"))
                    throw UsageError(L"Clear requires an explicit --channel NAME and --yes. It clears the ENTIRE channel.");
                if (has(L"--backup"))
                {
                    const auto &path = options[L"--backup"];
                    if (path.size() < 5 || _wcsicmp(path.c_str() + path.size() - 5, L".evtx") != 0)
                        throw UsageError(L"--backup must use the .evtx extension.");
                }
                Clear(options[L"--channel"], has(L"--backup") ? options[L"--backup"] : L"");
                WriteOutput(L"Cleared channel: " + options[L"--channel"] + L"\n");
                if (has(L"--backup"))
                    WriteOutput(L"Backup: " + AbsolutePath(options[L"--backup"]) + L"\n");
                return 0;
            }
            Query query;
            if (has(L"--channel") && has(L"--file"))
                throw UsageError(L"--channel and --file are mutually exclusive.");
            if (has(L"--channel"))
                query.source = options[L"--channel"];
            if (has(L"--file"))
            {
                query.source = options[L"--file"];
                query.file = true;
            }
            if (has(L"--level"))
                query.level = ParseLevel(options[L"--level"]);
            if (has(L"--id"))
                query.eventId = static_cast<int>(Number(options[L"--id"], L"--id", 65535));
            if (has(L"--hours"))
                query.hours = Number(options[L"--hours"], L"--hours", 8760000);
            if (has(L"--provider"))
                query.provider = options[L"--provider"];
            if (has(L"--query"))
            {
                if (has(L"--level") || has(L"--id") || has(L"--hours") || has(L"--provider"))
                    throw UsageError(L"--query cannot be combined with --level, --id, --hours, or --provider.");
                query.xpath = options[L"--query"];
            }
            query.limit = command == L"export" ? 0 : 200;
            if (has(L"--limit"))
                query.limit = static_cast<size_t>(Number(options[L"--limit"], L"--limit", 1000000000));
            if (command == L"export" && !has(L"--output"))
                throw UsageError(L"Export requires --output PATH.");
            if (has(L"--overwrite") && !has(L"--output"))
                throw UsageError(L"--overwrite requires --output.");
            Format format = Format::Table;
            if (has(L"--format"))
                format = ParseFormat(options[L"--format"]);
            else if (command == L"export")
            {
                const auto &path = options[L"--output"];
                const auto dot = path.find_last_of(L'.');
                if (dot == std::wstring::npos)
                    throw UsageError(L"Specify --format or a .evtx/.csv/.json/.xml output extension.");
                format = ParseFormat(path.substr(dot + 1));
            }
            if (command == L"list" && format == Format::Evtx)
                throw UsageError(L"Use export to write EVTX files.");
            if (command == L"export" && format == Format::Table)
                throw UsageError(L"Export format must be evtx, csv, json or xml.");
            if (format == Format::Evtx && query.limit != 0)
                throw UsageError(L"EVTX exports all matching events; --limit must be 0.");
            if (has(L"--output"))
            {
                const auto count = Export(query, options[L"--output"], format, has(L"--overwrite"));
                WriteOutput(L"Exported " + (format == Format::Evtx ? std::wstring(L"matching events") : std::to_wstring(count) + L" events") +
                                L" to " + AbsolutePath(options[L"--output"]) + L"\n",
                            true);
            }
            else
                Serialize(query, format, [](const std::wstring &text)
                          { WriteOutput(text); });
            return 0;
        }
        catch (const UsageError &error)
        {
            try
            {
                WriteOutput(L"Error: " + error.message + L"\nUse --help for usage.\n", true);
            }
            catch (...)
            {
            }
            return 2;
        }
        catch (const Error &error)
        {
            try
            {
                WriteOutput(L"Error: " + error.text + L"\n", true);
                if (error.code == ERROR_ACCESS_DENIED || error.code == ERROR_PRIVILEGE_NOT_HELD)
                    WriteOutput(L"Run from an elevated terminal for this operation.\n", true);
            }
            catch (...)
            {
            }
            return error.code == ERROR_ACCESS_DENIED || error.code == ERROR_PRIVILEGE_NOT_HELD ? 3 : error.code == ERROR_CANCELLED ? 5
                                                                                                                                   : 4;
        }
        catch (const std::exception &)
        {
            try
            {
                WriteOutput(L"Unexpected error while processing the command.\n", true);
            }
            catch (...)
            {
            }
            return 4;
        }
    }
} // namespace elm
