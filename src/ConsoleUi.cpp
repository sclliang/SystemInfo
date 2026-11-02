#include "ConsoleUi.h"
#include "CommandLine.h"
#include <conio.h>
#include <algorithm>
#include <climits>
#include <map>
#include <mutex>
#include <thread>

namespace elm
{
    namespace
    {
        std::atomic_bool cancelRequested{false};

        BOOL WINAPI OnConsoleControl(DWORD type)
        {
            if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT)
                return FALSE;
            cancelRequested = true;
            return TRUE;
        }

        // Console fonts on non-Chinese systems usually lack CJK glyphs, so Chinese is shown
        // only when the Windows display language is Chinese; everything else uses English.
        bool UseChinese()
        {
            static const bool chinese = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
            return chinese;
        }

        const wchar_t *Text(const wchar_t *chinese, const wchar_t *english)
        {
            return UseChinese() ? chinese : english;
        }

        bool HasCjk(const std::wstring &text)
        {
            return std::any_of(text.begin(), text.end(), [](wchar_t ch)
                               { return ch >= 0x2E80; });
        }

        // Collector messages are Chinese; in English mode they are dropped rather than shown garbled.
        std::wstring Localized(const std::wstring &text)
        {
            return UseChinese() || !HasCjk(text) ? text : std::wstring();
        }

        std::wstring Caption(const CollectedLog &item)
        {
            if (UseChinese())
                return item.caption;
            static const std::map<std::wstring, std::wstring> captions{
                {L"UserDumps", L"Application crash dumps"}, {L"ServicingLogs", L"Servicing and upgrade logs"},
                {L"Reliability", L"Reliability history"}, {L"StorageHealth", L"Storage health snapshot"},
                {L"System", L"System events"}, {L"Application", L"Application events"}, {L"Security", L"Security events"}, {L"Setup", L"Setup events"}, {L"ForwardedEvents", L"Forwarded events"}, {L"CrashDumps", L"Crash dumps"}, {L"LiveKernelReports", L"Live kernel reports"}, {L"WerReports", L"Windows Error Reporting"}, {L"SystemInfo", L"System and hardware"}, {L"Devices", L"Devices and problem codes"}, {L"Drivers", L"Drivers and signatures"}, {L"DumpConfiguration", L"Dump and page file setup"}, {L"DriverInstallation", L"Driver installation logs"}, {L"DiagnosticEvents", L"Device/driver/update events"}, {L"Timeline", L"Fault timeline"}, {L"PowerReports", L"Power and sleep reports"}};
            const auto found = captions.find(item.channel);
            return found != captions.end() ? found->second : item.channel;
        }

        // Ambiguous-width box drawing counts as wide so a line never wraps in legacy CJK consoles.
        int CharWidth(wchar_t ch)
        {
            if ((ch >= 0x1100 && ch <= 0x115F) || (ch >= 0x2500 && ch <= 0x259F) || (ch >= 0x2E80 && ch <= 0xA4CF) ||
                (ch >= 0xAC00 && ch <= 0xD7A3) || (ch >= 0xF900 && ch <= 0xFAFF) || (ch >= 0xFE30 && ch <= 0xFE4F) ||
                (ch >= 0xFF00 && ch <= 0xFF60) || (ch >= 0xFFE0 && ch <= 0xFFE6))
                return 2;
            return 1;
        }

        int TextWidth(const std::wstring &text)
        {
            int width = 0;
            for (wchar_t ch : text)
                width += CharWidth(ch);
            return width;
        }

        std::wstring Fit(const std::wstring &text, int columns)
        {
            if (TextWidth(text) <= columns)
                return text;
            if (columns <= 3)
                return {};
            std::wstring fitted;
            int used = 0;
            for (wchar_t ch : text)
            {
                if (used + CharWidth(ch) > columns - 3)
                    break;
                fitted += ch;
                used += CharWidth(ch);
            }
            return fitted + L"...";
        }

        std::wstring Pad(const std::wstring &text, int columns)
        {
            return text + std::wstring(std::max(0, columns - TextWidth(text)), L' ');
        }

        std::wstring FileSize(uint64_t bytes)
        {
            wchar_t text[48]{};
            if (bytes >= 1024ull * 1024 * 1024)
                swprintf_s(text, L"%.2f GB", bytes / (1024. * 1024. * 1024.));
            else if (bytes >= 1024 * 1024)
                swprintf_s(text, L"%.1f MB", bytes / (1024. * 1024.));
            else
                swprintf_s(text, L"%.0f KB", std::max(1., bytes / 1024.));
            return text;
        }

        std::wstring Duration(uint64_t milliseconds)
        {
            const auto seconds = milliseconds / 1000;
            wchar_t text[32]{};
            swprintf_s(text, L"%02llu:%02llu", seconds / 60, seconds % 60);
            return text;
        }

        class Console
        {
        public:
            Console()
            {
                output_ = GetStdHandle(STD_OUTPUT_HANDLE);
                DWORD mode = 0;
                interactive = GetConsoleMode(output_, &mode) != 0;
                color = interactive && SetConsoleMode(output_, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
            }
            int width() const
            {
                CONSOLE_SCREEN_BUFFER_INFO info{};
                if (!color || !GetConsoleScreenBufferInfo(output_, &info))
                    return INT_MAX / 2;
                return info.srWindow.Right - info.srWindow.Left + 1;
            }
            std::wstring paint(const wchar_t *style, const std::wstring &text) const
            {
                return color ? L"\x1b[" + std::wstring(style) + L"m" + text + L"\x1b[0m" : text;
            }
            std::wstring rule() const
            {
                return L"  " + paint(L"90", std::wstring(std::clamp((width() - 4) / 2, 10, 56), L'─')) + L"\n";
            }
            bool interactive = false, color = false;

        private:
            HANDLE output_{};
        };

        const wchar_t *StateColor(CollectionState state)
        {
            switch (state)
            {
            case CollectionState::Saved:
                return L"92";
            case CollectionState::Failed:
                return L"91";
            case CollectionState::Partial:
            case CollectionState::Cancelled:
                return L"93";
            default:
                return L"97";
            }
        }

        std::wstring Badge(const Console &console, CollectionState state)
        {
            const wchar_t *label = Text(L"等待", L"Waiting"), *style = L"90";
            switch (state)
            {
            case CollectionState::Saved:
                label = Text(L"已保存", L"Saved"), style = L"92;1";
                break;
            case CollectionState::Partial:
                label = Text(L"部分完成", L"Partial"), style = L"93;1";
                break;
            case CollectionState::Failed:
                label = Text(L"失败", L"Failed"), style = L"91;1";
                break;
            case CollectionState::Cancelled:
                label = Text(L"已取消", L"Cancelled"), style = L"93;1";
                break;
            case CollectionState::NotFound:
                label = Text(L"未找到", L"Not found"), style = L"90";
                break;
            case CollectionState::Skipped:
                label = Text(L"已跳过", L"Skipped"), style = L"90";
                break;
            case CollectionState::Running:
                label = Text(L"采集中", L"Running"), style = L"96";
                break;
            default:
                break;
            }
            constexpr int width = 11;
            if (!console.color)
                return Pad(L"[" + std::wstring(label) + L"]", width);
            return console.paint(style, label) + std::wstring(width - TextWidth(label), L' ');
        }

        std::wstring ItemLine(const Console &console, const CollectedLog &item, int columns)
        {
            std::wstring detail;
            if (item.state == CollectionState::Saved || item.state == CollectionState::Partial)
            {
                const auto files = std::count_if(item.files.begin(), item.files.end(), [](const CollectedFile &file)
                                                 { return file.state == CollectionState::Saved; });
                if (files)
                    detail = std::to_wstring(files) + Text(L" 个文件", files == 1 ? L" file" : L" files");
                if (item.bytes)
                    detail += (detail.empty() ? L"" : L" · ") + FileSize(item.bytes);
            }
            const auto message = Localized(item.message.substr(0, item.message.find_first_of(L"\r\n")));
            if (item.state != CollectionState::Saved && !message.empty())
                detail = message;
            else if (item.state == CollectionState::Failed || item.state == CollectionState::Partial)
                detail = Text(L"详细原因见 manifest.html", L"See manifest.html for details");
            const auto caption = Caption(item);
            const int captionWidth = UseChinese() ? 22 : 29;
            const int used = 2 + 11 + 2 + std::max(captionWidth, TextWidth(caption));
            return L"  " + Badge(console, item.state) + L"  " + Pad(caption, captionWidth) +
                   console.paint(item.state == CollectionState::Failed ? L"91" : L"90", Fit(detail, columns - used - 1)) + L"\n";
        }

        std::wstring LiveLine(const Console &console, const CollectionResult &view, size_t frame, uint64_t elapsed, int columns)
        {
            static constexpr wchar_t spinner[] = L"|/-\\";
            constexpr int barWidth = 24;
            const auto total = view.total(), done = view.completed();
            const int filled = total ? static_cast<int>(barWidth * done / total) : 0;
            std::wstring status = Text(L"正在准备...", L"Preparing...");
            for (const auto *item : CollectionEntries(view))
                if (item->state == CollectionState::Running)
                {
                    status = Text(L"正在采集 ", L"Collecting ") + Caption(*item);
                    break;
                }
            if (cancelRequested)
                status = Text(L"正在取消，等待当前项目结束...", L"Cancelling, waiting for the current item...");
            const auto counter = std::to_wstring(done) + L"/" + std::to_wstring(total), clock = Duration(elapsed);
            const int used = 2 + 2 + barWidth * 2 + 2 + TextWidth(counter) + 2 + TextWidth(clock) + 2;
            // Heavy lines exist in Chinese console fonts; block shades exist in Western ones (code page 437).
            const wchar_t full = UseChinese() ? L'━' : L'█', empty = UseChinese() ? L'━' : L'░';
            return L"  " + console.paint(L"96", std::wstring(1, spinner[frame % 4])) + L" " +
                   console.paint(L"96", std::wstring(filled, full)) + console.paint(L"90", std::wstring(barWidth - filled, empty)) + L"  " +
                   console.paint(L"1", counter) + L"  " + console.paint(L"90", clock) + L"  " +
                   console.paint(cancelRequested ? L"93" : L"97", Fit(status, columns - used - 1));
        }

        std::wstring Field(const Console &console, const wchar_t *label, const std::wstring &value)
        {
            return L"    " + console.paint(L"90", Pad(label, UseChinese() ? 10 : 15)) + value + L"\n";
        }

        std::wstring Banner(const Console &console, const CollectionResult &initial, const CollectionOptions &options)
        {
            wchar_t computer[256]{};
            DWORD length = static_cast<DWORD>(std::size(computer));
            GetComputerNameExW(ComputerNameDnsHostname, computer, &length);
            const auto bar = console.paint(L"96", L"▌");
            std::wstring text = L"\n  " + bar + L" " + console.paint(L"97;1", L"EventLogCollector") + L"  " + console.paint(L"90", L"v1.4.0.0") +
                                L"\n  " + bar + L" " + console.paint(L"37", Text(L"Windows 事件日志与诊断信息收集器", L"Windows Event Log and Diagnostic Collector")) + L"\n\n";
            text += Field(console, Text(L"计算机", L"Computer"), computer);
            text += Field(console, Text(L"输出位置", L"Output"), initial.rootDirectory);
            text += Field(console, Text(L"近期范围", L"Recent window"), std::to_wstring(options.days) +
                                                                         Text(L" 天（标准事件通道全量导出）", L" days (standard event channels are exported in full)"));
            text += Field(console, Text(L"电源报告", L"Power reports"), !options.powerReports ? Text(L"关闭", L"Off") : options.energyReport ? Text(L"开启，含 60 秒能效采样", L"On, with 60 s energy sampling")
                                                                                                                                             : Text(L"开启", L"On"));
            if (options.includeLargeDumps)
                text += Field(console, Text(L"大型转储", L"Large dumps"), Text(L"包含", L"Included"));
            text += L"\n" + console.rule();
            if (console.interactive)
                text += L"  " + console.paint(L"90", Text(L"按 Ctrl+C 可取消采集，已完成的文件会保留", L"Press Ctrl+C to cancel; completed files are kept")) + L"\n";
            return text + L"\n";
        }

        std::wstring Summary(const Console &console, const CollectionResult &result)
        {
            std::wstring text = L"\n" + console.rule() + L"\n  ";
            if (result.success())
                text += console.paint(L"92;1", Text(L"√ 采集完成", L"√ Collection complete"));
            else if (result.cancelled)
                text += console.paint(L"93;1", Text(L"■ 采集已取消，已完成的文件已保留", L"■ Collection cancelled; completed files were kept"));
            else
                text += console.paint(L"93;1", Text(L"! 采集结束，部分项目失败或不完整", L"! Collection finished; some items failed or are incomplete"));
            text += L"\n\n  " + std::to_wstring(result.total()) + Text(L" 项", L" items");
            const std::pair<CollectionState, const wchar_t *> counts[]{
                {CollectionState::Saved, Text(L"已保存", L"saved")}, {CollectionState::Partial, Text(L"部分完成", L"partial")}, {CollectionState::NotFound, Text(L"未找到", L"not found")}, {CollectionState::Skipped, Text(L"已跳过", L"skipped")}, {CollectionState::Failed, Text(L"失败", L"failed")}, {CollectionState::Cancelled, Text(L"已取消", L"cancelled")}};
            const auto separator = console.paint(L"90", L"  ·  ");
            for (const auto &[state, label] : counts)
                if (const auto count = result.count(state); count || state == CollectionState::Saved)
                    text += separator + label + L" " + console.paint(StateColor(state), std::to_wstring(count));
            text += separator + Text(L"用时 ", L"time ") + Duration(result.elapsedMs) + L"\n\n";
            text += Field(console, Text(L"日志目录", L"Log folder"), console.paint(L"96;1", result.directory));
            text += Field(console, Text(L"总览报告", L"Report"), result.manifestSaved ? console.paint(L"96", result.manifestPath) : console.paint(L"91", Text(L"未能保存", L"Not saved")));
            if (!result.success() && !result.message.empty())
            {
                std::wstring lines;
                for (size_t start = 0; start < result.message.size();)
                {
                    const auto end = std::min(result.message.find(L'\n', start), result.message.size());
                    if (const auto line = Localized(result.message.substr(start, end - start)); !line.empty())
                        lines += L"    " + console.paint(L"93", line) + L"\n";
                    start = end + 1;
                }
                if (!lines.empty())
                    text += L"\n" + lines;
            }
            return text;
        }

        std::wstring Failure(const Console &console, const std::wstring &message, DWORD code)
        {
            std::wstring text = L"\n  " + console.paint(L"91;1", Text(L"× 采集失败", L"× Collection failed")) + L"\n\n";
            if (const auto shown = Localized(message); !shown.empty())
                text += L"    " + console.paint(L"91", shown) + L"\n";
            if (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD)
                text += L"    " + console.paint(L"90", Text(L"请以管理员身份运行。", L"Run as administrator.")) + L"\n";
            return text;
        }

        int FailureCode(DWORD code)
        {
            return code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD ? 3 : code == ERROR_CANCELLED ? 5
                                                                                                                  : 4;
        }

        void WaitForKey(const Console &console)
        {
            HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
            DWORD mode = 0;
            if (!console.interactive || !GetConsoleMode(input, &mode))
                return;
            WriteOutput(L"\n  " + console.paint(L"90", Text(L"按任意键退出...", L"Press any key to exit...")));
            FlushConsoleInputBuffer(input);
            const auto key = _getwch();
            if (key == 0 || key == 0xE0)
                _getwch(); // Function and arrow keys arrive as two codes.
            WriteOutput(L"\n");
        }
    } // namespace

    int RunConsoleCollection(const std::wstring &directory, const CollectionOptions &options, bool pause)
    {
        const Console console;
        cancelRequested = false;
        SetConsoleCtrlHandler(OnConsoleControl, TRUE);
        if (console.interactive)
            SetConsoleTitleW(Text(L"EventLogCollector - 正在采集", L"EventLogCollector - Collecting"));
        int exitCode = 4;
        try
        {
            if (console.color)
                WriteOutput(L"\x1b[?25l");
            CollectionResult snapshot = InitialCollection(directory);
            WriteOutput(Banner(console, snapshot, options));
            std::mutex lock;
            std::atomic_bool done{false};
            std::wstring failure;
            DWORD failureCode = 0;
            std::thread worker([&]
                               {
                try
                {
                    auto result = CollectWindowsLogs(directory, [&](const CollectionResult &value)
                                                     { std::lock_guard guard(lock); snapshot = value; }, &cancelRequested, options);
                    std::lock_guard guard(lock);
                    snapshot = std::move(result);
                }
                catch (const Error &error)
                {
                    std::lock_guard guard(lock);
                    failure = error.text;
                    failureCode = error.code;
                }
                catch (const std::exception &)
                {
                    std::lock_guard guard(lock);
                    failure = Text(L"采集过程中发生意外错误。", L"Unexpected error during collection.");
                    failureCode = ERROR_GEN_FAILURE;
                }
                done = true; });
            try
            {
                const auto started = GetTickCount64();
                std::vector<bool> printed(snapshot.total());
                for (size_t frame = 0;; ++frame)
                {
                    const bool finished = done;
                    CollectionResult view;
                    {
                        std::lock_guard guard(lock);
                        view = snapshot;
                    }
                    const int columns = console.width();
                    std::wstring text = console.color ? L"\r\x1b[2K" : L"";
                    const auto items = CollectionEntries(view);
                    for (size_t i = 0; i < items.size() && i < printed.size(); ++i)
                        if (!printed[i] && items[i]->state != CollectionState::Waiting && items[i]->state != CollectionState::Running)
                        {
                            text += ItemLine(console, *items[i], columns);
                            printed[i] = true;
                        }
                    if (!finished && console.color)
                        text += LiveLine(console, view, frame, GetTickCount64() - started, columns);
                    if (!text.empty())
                        WriteOutput(text);
                    if (finished)
                        break;
                    Sleep(100);
                }
            }
            catch (...)
            {
                cancelRequested = true;
                worker.join();
                throw;
            }
            worker.join();
            if (!failure.empty())
            {
                WriteOutput(Failure(console, failure, failureCode));
                exitCode = FailureCode(failureCode);
            }
            else
            {
                WriteOutput(Summary(console, snapshot));
                exitCode = snapshot.success() ? 0 : snapshot.cancelled ? 5
                                                                       : 4;
            }
        }
        catch (const Error &error)
        {
            try
            {
                WriteOutput(Failure(console, error.text, error.code));
            }
            catch (...)
            {
            }
            exitCode = FailureCode(error.code);
        }
        catch (const std::exception &)
        {
            try
            {
                WriteOutput(Failure(console, Text(L"采集过程中发生意外错误。", L"Unexpected error during collection."), ERROR_GEN_FAILURE));
            }
            catch (...)
            {
            }
            exitCode = 4;
        }
        try
        {
            if (console.color)
                WriteOutput(L"\x1b[?25h");
            if (console.interactive)
                SetConsoleTitleW(exitCode == 0 ? Text(L"EventLogCollector - 采集完成", L"EventLogCollector - Complete")
                                               : Text(L"EventLogCollector - 采集结束", L"EventLogCollector - Finished"));
            if (pause)
                WaitForKey(console);
        }
        catch (...)
        {
        }
        SetConsoleCtrlHandler(OnConsoleControl, FALSE);
        return exitCode;
    }
}
