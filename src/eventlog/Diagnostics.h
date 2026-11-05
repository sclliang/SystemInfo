#pragma once
#include "Collector.h"
#include <filesystem>

namespace elm
{
    struct DiagnosticUser
    {
        std::wstring id;
        std::filesystem::path profile, localAppData;
    };
    struct DiagnosticDumpFolder
    {
        std::filesystem::path directory, prefix;
        std::wstring source;
    };
    // Explicit roots and limits allow isolated file fixtures without changing the machine.
    struct DiagnosticContext
    {
        std::filesystem::path output, windows, programData, localAppData;
        std::filesystem::path currentUserProfile;
        std::vector<DiagnosticUser> otherUsers;
        std::vector<CollectedFile> userProfileIssues;
        CollectionOptions options;
        const std::atomic_bool *cancel = nullptr;
        uint64_t now = 0, dumpBytesCopied = 0, auxiliaryBytesCopied = 0;
        uint64_t maxDumpFileBytes = 64ULL * 1024 * 1024;
        uint64_t maxDumpTotalBytes = 256ULL * 1024 * 1024;
        uint64_t maxAuxiliaryTotalBytes = 128ULL * 1024 * 1024;
        uint64_t maxServicingTotalBytes = 128ULL * 1024 * 1024;
        size_t maxFiles = 100, maxScanEntries = 10000, maxEventsPerChannel = 5000;
        bool resolveDumpPaths = true;
        bool userProfilesResolved = false;
        HKEY localDumpsRegistry = HKEY_LOCAL_MACHINE;
        std::wstring localDumpsKey = L"SOFTWARE\\Microsoft\\Windows\\Windows Error Reporting\\LocalDumps";
        // Internal fixture overrides; production always resolves powercfg from System32.
        std::filesystem::path powercfgExecutable;
        DWORD powerCommandTimeoutMs = 60000, energyCommandTimeoutMs = 120000;
    };
    DiagnosticContext MakeDiagnosticContext(const CollectionResult &result, const std::atomic_bool *cancel);
    void CollectDiagnostic(CollectedLog &item, DiagnosticContext &context, const CollectionResult &result);
    void CollectSystemSnapshot(CollectedLog &item, const DiagnosticContext &context);
    void CollectDevices(CollectedLog &item, const DiagnosticContext &context);
    void CollectDrivers(CollectedLog &item, const DiagnosticContext &context);
    void CollectDumpConfiguration(CollectedLog &item, const DiagnosticContext &context);
    void CollectDumpFiles(CollectedLog &item, DiagnosticContext &context, bool live);
    void CollectUserDumps(CollectedLog &item, DiagnosticContext &context);
    void CollectWerReports(CollectedLog &item, DiagnosticContext &context);
    void CollectServicingLogs(CollectedLog &item, const DiagnosticContext &context);
    void CollectReliability(CollectedLog &item, const DiagnosticContext &context);
    void CollectStorageHealth(CollectedLog &item, const DiagnosticContext &context);
    void CollectDriverInstallation(CollectedLog &item, DiagnosticContext &context);
    void CollectDiagnosticEvents(CollectedLog &item, const DiagnosticContext &context);
    void CollectTimeline(CollectedLog &item, const DiagnosticContext &context, const CollectionResult &result);
    void CollectPowerReports(CollectedLog &item, const DiagnosticContext &context);
    void CollectEventResources(CollectedLog &item, const DiagnosticContext &context, const std::wstring &path);
    void DiscoverDiagnosticUsers(DiagnosticContext &context);
    std::vector<DiagnosticUser> DiagnosticUsers(const DiagnosticContext &context);
    std::vector<DiagnosticDumpFolder> DiagnosticUserDumpFolders(CollectedLog &item, const DiagnosticContext &context);
    std::wstring ExpandDiagnosticUserPath(const std::wstring &value, const DiagnosticUser &user, const DiagnosticContext &context);
    std::wstring DiagnosticTimelineXPath(const DiagnosticContext &context);
    void CheckDiagnosticCancel(const DiagnosticContext &context);
    uint64_t FileTimeValue(FILETIME time);
    std::wstring DiagnosticUtc(uint64_t ticks);
    std::wstring ExpandedEnvironment(const std::wstring &value);
    std::wstring ReadRegistryString(HKEY root, const wchar_t *key, const wchar_t *name);
    void WriteDiagnosticFile(const std::filesystem::path &path, const std::wstring &text);
    void SaveDiagnosticReport(CollectedLog &item, const DiagnosticContext &context, const std::wstring &text);
}
