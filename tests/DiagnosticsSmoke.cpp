#include "../src/Diagnostics.h"
#include <shellapi.h>
#include <fstream>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
std::wstring report;
void Assert(bool condition, const wchar_t* message) {
    if (!condition) throw std::runtime_error(elm::Utf8(message));
    report += L"PASS: " + std::wstring(message) + L"\n";
}
void CreateSample(const fs::path& path, const std::wstring& content, uint64_t modified) {
    elm::WriteDiagnosticFile(path, content);
    HANDLE file = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open fixture timestamp");
    FILETIME time{static_cast<DWORD>(modified), static_cast<DWORD>(modified >> 32)};
    const bool set = SetFileTime(file, nullptr, nullptr, &time) != FALSE; CloseHandle(file);
    if (!set) throw std::runtime_error("Could not set fixture timestamp");
}
const elm::CollectedFile* Find(const elm::CollectedLog& item, const wchar_t* suffix) {
    for (const auto& file : item.files) if (file.source.ends_with(suffix)) return &file;
    return nullptr;
}
size_t Count(const elm::CollectedLog& item, elm::CollectionState state) {
    size_t count = 0; for (const auto& file : item.files) if (file.state == state) ++count; return count;
}
void Run(const fs::path& root) {
    using namespace elm;
    DiagnosticContext context; context.output = root / L"Basic"; context.windows = root / L"Windows";
    context.programData = root / L"ProgramData"; context.localAppData = root / L"UserData"; context.resolveDumpPaths = false;
    FILETIME now{}; GetSystemTimeAsFileTime(&now); context.now = FileTimeValue(now);
    context.maxDumpFileBytes = 8; context.maxDumpTotalBytes = 6;
    const uint64_t day = 86400ULL * 10000000;
    CreateSample(context.windows / L"Minidump" / L"newer.dmp", L"1234", context.now - 100000);
    CreateSample(context.windows / L"Minidump" / L"older.dmp", L"5678", context.now - 200000);
    CreateSample(context.windows / L"Minidump" / L"too-large.dmp", L"01234567890123456789", context.now - 50000);
    CreateSample(context.windows / L"Minidump" / L"expired.dmp", L"old!", context.now - 30 * day);
    CreateSample(context.windows / L"MEMORY.DMP", L"memory", context.now - 25000);
    CollectedLog basic; basic.channel = L"CrashDumps"; CollectDumpFiles(basic, context, false);
    Assert(basic.state == CollectionState::Saved && Count(basic, CollectionState::Saved) == 1, L"Basic collection saves accessible recent evidence");
    Assert(Find(basic, L"newer.dmp") && Find(basic, L"newer.dmp")->state == CollectionState::Saved, L"Newest dumps are prioritized");
    Assert(Find(basic, L"older.dmp")->state == CollectionState::Skipped && context.dumpBytesCopied == 4, L"Aggregate dump budget is enforced without copying extra bytes");
    Assert(Find(basic, L"too-large.dmp")->state == CollectionState::Skipped && Find(basic, L"too-large.dmp")->bytes == 20, L"Oversized dump retains size and a skip reason");
    Assert(!Find(basic, L"expired.dmp"), L"Expired dump is excluded from recent evidence");
    Assert(Find(basic, L"MEMORY.DMP")->state == CollectionState::Skipped && Find(basic, L"MEMORY.DMP")->path.empty(), L"Configured system dump is indexed without a default copy");
    Assert(fs::file_size(context.windows / L"Minidump" / L"newer.dmp") == 4, L"Source evidence is preserved");
    auto deep = context; deep.output = root / L"Deep"; deep.options.includeLargeDumps = true;
    deep.maxDumpFileBytes = 64; deep.maxDumpTotalBytes = 64; deep.dumpBytesCopied = 0;
    CollectedLog full; CollectDumpFiles(full, deep, false);
    Assert(Find(full, L"MEMORY.DMP")->state == CollectionState::Saved && Count(full, CollectionState::Saved) == 4, L"Opt-in copies system dump within configured limits");
    auto countLimited = deep; countLimited.output = root / L"CountLimited"; countLimited.maxFiles = 1; countLimited.dumpBytesCopied = 0;
    CollectedLog limited; CollectDumpFiles(limited, countLimited, false);
    Assert(Count(limited, CollectionState::Saved) == 1 && Count(limited, CollectionState::Skipped) == 3, L"File count limit leaves remaining evidence indexed");
    CreateSample(context.windows / L"LiveKernelReports" / L"WATCHDOG" / L"live.dmp", L"live", context.now - 100000);
    auto liveContext = context; liveContext.output = root / L"Live"; liveContext.dumpBytesCopied = 0;
    CollectedLog live; CollectDumpFiles(live, liveContext, true);
    Assert(Find(live, L"live.dmp") && Find(live, L"live.dmp")->state == CollectionState::Saved, L"Component live dumps are collected recursively");
    Assert(Find(live, L"Linked") && Find(live, L"Linked")->state == CollectionState::Skipped, L"Directory junction is recorded and not traversed");
    Assert(!Find(live, L"outside.dmp"), L"External evidence behind a junction is not copied");
    auto absent = context; absent.output = root / L"Absent"; absent.windows = root / L"MissingWindows";
    CollectedLog missing; CollectDumpFiles(missing, absent, false);
    Assert(missing.state == CollectionState::NotFound && fs::exists(missing.path), L"Missing evidence has a persisted not-found index");
    const auto machine = context.programData / L"Microsoft" / L"Windows" / L"WER" / L"ReportQueue" / L"Report A";
    const auto user = context.localAppData / L"Microsoft" / L"Windows" / L"WER" / L"ReportArchive" / L"报告 B";
    CreateSample(machine / L"Report.wer", L"machine report", context.now - 100000);
    CreateSample(user / L"Report.wer", L"user report", context.now - 100000);
    CreateSample(machine / L"locked.wer", L"locked", context.now - 100000);
    HANDLE locked = CreateFileW((machine / L"locked.wer").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not lock fixture");
    CollectedLog wer; CollectWerReports(wer, context); CloseHandle(locked);
    Assert(wer.state == CollectionState::Partial && Count(wer, CollectionState::Saved) == 2, L"A locked report fails visibly while other reports are preserved");
    Assert(Find(wer, L"locked.wer") && Find(wer, L"locked.wer")->error != 0, L"Locked report retains the native error code");
    Assert(fs::exists(context.output / L"WerReports" / L"Machine" / L"ReportQueue" / L"Report A" / L"Report.wer") &&
        fs::exists(context.output / L"WerReports" / L"CurrentUser" / L"ReportArchive" / L"报告 B" / L"Report.wer"),
        L"Machine and user reports with the same filename remain distinct");
    CreateSample(context.windows / L"INF" / L"setupapi.dev.log", L"old driver installation history", context.now - 30 * day);
    CollectedLog installation; CollectDriverInstallation(installation, context);
    Assert(Find(installation, L"setupapi.dev.log")->state == CollectionState::Saved, L"Rolling SetupAPI history is retained even when its timestamp is old");
    std::atomic_bool stop{true}; auto cancelled = context; cancelled.cancel = &stop; cancelled.output = root / L"CancelledFiles";
    bool threw = false; CollectedLog interrupted;
    try { CollectDumpFiles(interrupted, cancelled, false); } catch (const Error& error) { threw = error.code == ERROR_CANCELLED; }
    Assert(threw && !fs::exists(cancelled.output), L"Cancellation is honored before copying evidence");
    auto result = CollectWindowsLogs((root / L"CancelledCollection").wstring(), {}, &stop);
    Assert(result.cancelled && !result.success() && result.count(CollectionState::Cancelled) == result.total(), L"Cancelled collection never reports success");
    Assert(result.manifestSaved && fs::exists(result.manifestPath), L"Cancelled collection still writes its manifest");
    auto manifestFailure = CollectWindowsLogs((root / L"ManifestFailure").wstring(), [](const CollectionResult& value) {
        if (!value.finished && value.completed() == value.total()) fs::create_directory(value.manifestPath);
    }, &stop);
    Assert(!manifestFailure.manifestSaved && manifestFailure.manifestError && !manifestFailure.success(),
        L"A real manifest write failure is surfaced in the final result");
    const auto atomic = root / L"Atomic" / L"result.json";
    WriteDiagnosticFile(atomic, L"{\"v\":1}\n");
    WriteDiagnosticFile(atomic.wstring() + L".tmp", L"preserve");
    bool refused = false;
    try { WriteDiagnosticFile(atomic, L"replacement"); } catch (const Error&) { refused = true; }
    Assert(refused && fs::file_size(atomic) == 8 && fs::file_size(atomic.wstring() + L".tmp") == 8,
        L"Failed atomic write preserves existing output and an unowned temporary file");
    DeleteFileW((atomic.wstring() + L".tmp").c_str());
    CollectionResult offline = InitialCollection((root / L"InvalidTimeline").wstring());
    for (auto& log : offline.logs) log.state = CollectionState::Failed;
    offline.logs[0].state = CollectionState::Saved; offline.logs[0].path = (root / L"invalid.evtx").wstring();
    CreateSample(offline.logs[0].path, L"invalid EVTX", context.now);
    auto invalidContext = context; invalidContext.output = root / L"InvalidTimeline";
    CollectedLog timeline; CollectTimeline(timeline, invalidContext, offline);
    Assert(timeline.state == CollectionState::Partial && fs::exists(timeline.path), L"Unreadable timeline sources produce a partial report, not a false empty success");
    auto capped = context; capped.output = root / L"ScanLimited"; capped.maxScanEntries = 1; capped.dumpBytesCopied = 0;
    CollectedLog cappedItem; CollectDumpFiles(cappedItem, capped, false);
    Assert(cappedItem.state == CollectionState::Partial, L"Directory scan truncation is reported as incomplete");
    bool clean = true;
    for (const auto& entry : fs::recursive_directory_iterator(root)) if (entry.path().extension() == L".tmp") clean = false;
    Assert(clean, L"No temporary copy or JSON file remains");
    WriteDiagnosticFile(root / L"report.txt", report);
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int count = 0; auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args || count != 2) { if (args) LocalFree(args); return 2; }
    const fs::path root = args[1]; LocalFree(args);
    try { Run(root); return 0; }
    catch (const std::exception& error) {
        std::ofstream output(root / L"failure.txt", std::ios::binary);
        output << elm::Utf8(report) << "FAIL: " << error.what(); return 4;
    }
}
