#pragma once
#include "Collector.h"
#include <filesystem>

namespace elm {
// Explicit roots and limits allow isolated file fixtures without changing the machine.
struct DiagnosticContext {
    std::filesystem::path output, windows, programData, localAppData;
    CollectionOptions options;
    const std::atomic_bool* cancel = nullptr;
    uint64_t now = 0, dumpBytesCopied = 0, auxiliaryBytesCopied = 0;
    uint64_t maxDumpFileBytes = 64ULL * 1024 * 1024;
    uint64_t maxDumpTotalBytes = 256ULL * 1024 * 1024;
    uint64_t maxAuxiliaryTotalBytes = 128ULL * 1024 * 1024;
    size_t maxFiles = 100, maxScanEntries = 10000, maxEventsPerChannel = 5000;
    bool resolveDumpPaths = true;
};
DiagnosticContext MakeDiagnosticContext(const CollectionResult& result, const std::atomic_bool* cancel);
void CollectDiagnostic(CollectedLog& item, DiagnosticContext& context, const CollectionResult& result);
void CollectSystemSnapshot(CollectedLog& item, const DiagnosticContext& context);
void CollectDevices(CollectedLog& item, const DiagnosticContext& context);
void CollectDrivers(CollectedLog& item, const DiagnosticContext& context);
void CollectDumpConfiguration(CollectedLog& item, const DiagnosticContext& context);
void CollectDumpFiles(CollectedLog& item, DiagnosticContext& context, bool live);
void CollectWerReports(CollectedLog& item, DiagnosticContext& context);
void CollectDriverInstallation(CollectedLog& item, DiagnosticContext& context);
void CollectDiagnosticEvents(CollectedLog& item, const DiagnosticContext& context);
void CollectTimeline(CollectedLog& item, const DiagnosticContext& context, const CollectionResult& result);
void CollectPowerReports(CollectedLog& item, const DiagnosticContext& context);
void CheckDiagnosticCancel(const DiagnosticContext& context);
uint64_t FileTimeValue(FILETIME time);
std::wstring DiagnosticUtc(uint64_t ticks);
std::wstring ExpandedEnvironment(const std::wstring& value);
std::wstring ReadRegistryString(HKEY root, const wchar_t* key, const wchar_t* name);
void WriteDiagnosticFile(const std::filesystem::path& path, const std::wstring& text);
void SaveDiagnosticJson(CollectedLog& item, const std::wstring& text);
}
