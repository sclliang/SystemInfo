#include <Windows.h>
#include "../src/firmware/AmiFirmware.h"
#include "../src/firmware/AmiProcess.h"
#include "../src/firmware/AmiCleanup.h"
#include "../src/firmware/AmiReader.h"
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace
{
void Check(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }

void CheckReadRows()
{
    // Synthetic fixtures based on the exact format strings in the supplied 5.27 binary.
    const auto values = system_info::firmware::ParseAmiStrings(LR"ami(
          Name              R/W  Status  Information
(/IVN)BIOS vendor name.      R    Done   "American Megatrends International, LLC."
(/IV)BIOS version            R    Done   "SER9T201"
(/ID)BIOS release date       R    Done   "02/12/2025"
(/BP)Baseboard product       #1   R    Done   "SER9"
(/BP)Baseboard product       #2   R    Done   "SECOND BOARD"
(/BS)Baseboard Serial number R    Done   "BOARD-001"
(/SS)System Serial number    R    Done   "SYSTEM-002"
(/SU)System UUID             R    Done   "12345678-1234-5678-9ABC-123456789ABC"
(/SM)System manufacture      R    Done   "Not part of the display contract"
)ami");
    Check(values.commands.size() == 7, "Expected the seven supported AMI string fields");
    Check(values.commands.at(L"BP") == L"SER9", "Must retain first usable board instance");
    Check(values.commands.at(L"IVN").ends_with(L"LLC."), "Vendor punctuation lost");
    Check(values.commands.at(L"SU") == L"12345678123456789ABC123456789ABC", "UUID digits must stay in source order");

    const auto invalid = system_info::firmware::ParseAmiStrings(LR"ami(
(/BS)Baseboard Serial number W    Done   "NOT-A-READ"
(/SS)System Serial number    R  Invalid  "NOT-VALID"
(/BP)Baseboard product       R    Done   "<empty>"
(/IV)BIOS version            R    Done   "N/A"
(/SU)System UUID             R    Done   "00000000000000000000000000000000"
(/SU)System UUID             R    Done   "00000000000000000000000000000000h"
(/SU)System UUID             R    Done   "FFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFFh"
(/SU)System UUID             R    Done   "this-is-not-a-uuid"
(/BS)Baseboard Serial number W    Done   "x R Done "INJECTED""
Error: Unable to load driver.
)ami");
    Check(invalid.commands.empty(), "Error, placeholder or write response accepted as firmware data");

    const auto validRaw = system_info::firmware::ParseAmiStrings(
        L"(/SU)System UUID             R    Done   \"12345678123456789ABC123456789ABC\"\r\n");
    Check(validRaw.commands.at(L"SU").size() == 32, "Raw AMI UUID must be retained without byte swapping");
    for (const auto suffix : {L"h", L"H"})
    {
        const auto hexSuffix = system_info::firmware::ParseAmiStrings(
            std::wstring(L"(/SU)System UUID R Done \"78563412341278569ABC123456789ABC") + suffix + L"\"\r\n"
            L"(/SS)System Serial number R Done \"SERIAL-h\"\r\n");
        Check(hexSuffix.commands.at(L"SU") == L"78563412341278569ABC123456789ABC",
            "Only the 32 UUID digits belong in the parsed result");
        Check(hexSuffix.commands.at(L"SS") == L"SERIAL-h", "A serial number ending in h must remain intact");
    }
}

void CheckEc()
{
    const auto output = LR"ami([Type 000] -- BIOS Information
    Name  : Embedded Controller Firmware Major Release
    Style : BYTE
    Data  : 01h
    Name  : Embedded Controller Firmware Minor Release
    Style : BYTE
    Data  : 0Ah
[Type 001] -- System Information
    Name  : Embedded Controller Firmware Major Release
    Data  : 99h
)ami";
    Check(system_info::firmware::ParseEcDump(output) == L"01.10", "Type 0 EC hex decoding failed");
    // Same-line Name/Style columns observed in the supplied tool's real file dump.
    Check(system_info::firmware::ParseEcDump(
        L"[Type 000] -- BIOS Information\n"
        L"    Name  : Embedded Controller Firmware Major Release  Style : UINT8\n    Data  : FFh\n"
        L"    Name  : Embedded Controller Firmware Minor Release  Style : UINT8\n    Data  : FFh\n") == L"255.255",
        "Actual file-dump Name/Style columns must not hide the EC version");
    Check(system_info::firmware::ParseEcDump(
        L"[Type 000] -- BIOS Information\nName : Embedded Controller Firmware Major Release\nData : FFh\n"
        L"Name : Embedded Controller Firmware Minor Release\nData : FFh\n") == L"255.255",
        "Must preserve reference EC 255.255 behavior");
    Check(system_info::firmware::ParseEcDump(
        L"[Type 000]\nName : Embedded Controller Firmware Major Release\nData : 01h\n").empty(),
        "Partial EC version must fall back");
    Check(system_info::firmware::ParseEcDump(
        L"Name : Embedded Controller Firmware Major Release\nData : 01h\n"
        L"Name : Embedded Controller Firmware Minor Release\nData : 02h\n").empty(),
        "Unscoped EC fields must not be trusted");
    Check(system_info::firmware::ParseEcDump(
        L"[Type 000]\nName : Embedded Controller Firmware Major Release\nData : 999\n"
        L"Name : Embedded Controller Firmware Minor Release\nData : 02h\n").empty(),
        "Out of range EC byte accepted");
}

void CheckFallback()
{
    ftc::hardware_info::Snapshot baseline;
    baseline.rows = {{L"主板型号",L"WMI board"},{L"BIOS版本",L"WMI BIOS"},{L"EC版本",L"01.02"},
        {L"主板SN",L"WMI BS"},{L"系统SN",L"WMI SS"},{L"UUID",L"12345678-1234-5678-9ABC-123456789ABC"},{L"CPU型号",L"CPU"}};
    system_info::firmware::AmiValues ami;
    ami.commands = {{L"BP",L"AMI board"},{L"BS",L"AMI BS"},{L"IV",L"Incomplete BIOS"}};
    system_info::firmware::ApplyAmiValues(ami, baseline);
    Check(baseline.rows[0].value == L"AMI board" && baseline.rows[3].value == L"AMI BS", "AMI precedence lost");
    Check(baseline.rows[1].value == L"WMI BIOS" && baseline.rows[2].value == L"01.02" &&
        baseline.rows[4].value == L"WMI SS" && baseline.rows[5].value == L"12345678123456789ABC123456789ABC",
        "Fallback must preserve its source digits in the 32-character display format");
    Check(baseline.rows[6].value == L"CPU", "Unrelated hardware field changed");
    ami.commands[L"IVN"] = L"AMI";
    ami.commands[L"IV"] = L"1.0";
    ami.commands[L"ID"] = L"02/12/2025";
    ami.commands[L"SS"] = L"DIRECT-SS";
    ami.commands[L"SU"] = L"78563412341278569ABC123456789ABCh";
    ami.ecVersion = L"03.04";
    system_info::firmware::ApplyAmiValues(ami, baseline);
    Check(baseline.rows[1].value == L"AMI 1.0, 2025/02/12", "BIOS display format changed");
    Check(baseline.rows[2].value == L"03.04", "AMI EC precedence lost");
    Check(baseline.rows[4].value == L"DIRECT-SS" && baseline.rows[5].value == L"78563412341278569ABC123456789ABC",
        "Direct system SN or UUID did not replace the original collector value");
}

void CheckResources(const wchar_t* executable, const wchar_t* directory)
{
    const HMODULE module = LoadLibraryExW(executable, nullptr, LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE);
    Check(module != nullptr, "Cannot read EXE resources");
    try
    {
        const wchar_t* files[]{L"AMIDEWINx64.EXE", L"amifldrv64.sys", L"amigendrv64.sys", L"amifldrv32.sys"};
        for (int index = 0; index < 4; ++index)
        {
            const HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(201 + index), RT_RCDATA);
            Check(resource != nullptr, "Embedded tool or driver missing");
            const DWORD length = SizeofResource(module, resource);
            const auto data = LockResource(LoadResource(module, resource));
            Check(data != nullptr && length > 0, "Embedded resource empty");
            std::ifstream input(std::filesystem::path(directory) / files[index], std::ios::binary);
            const std::string expected{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
            Check(expected.size() == length && std::memcmp(expected.data(), data, length) == 0,
                "Embedded executable/driver bytes differ from the supplied original");
        }
    }
    catch (...) { FreeLibrary(module); throw; }
    FreeLibrary(module);
}

int FakeAmi(int count, wchar_t** arguments)
{
    const std::wstring_view query(arguments[1]);
    const bool identity = query == L"/SU" || query == L"/SS" || query == L"/BS";
    const bool strings = query == L"/ALL";
    if (identity)
    {
        // An argument after /SU, /SS or /BS would be a firmware write request.
        if (count != 2) return 9;
    }
    else if (count != 3 || std::wstring_view(arguments[2]) != (strings ? L"strings.txt" : L"dump.txt")) return 9;
    std::string asciiQuery;
    for (wchar_t character : query) asciiQuery += static_cast<char>(character);
    std::ofstream("queries.txt", std::ios::app) << asciiQuery << '\n';
    std::ifstream modeFile("mode.txt");
    std::string mode;
    modeFile >> mode;
    const auto print = [](const std::string& text) {
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    };
    if (mode == "error") { print("Driver test failure.\r\n"); return 16; }
    if (identity)
    {
        if ((query == L"/SS" && mode == "ss-error") || (query == L"/BS" && mode == "bs-error"))
        { print("Read failed.\r\n"); return 16; }
        if (query == L"/SU") print(mode == "uuid-invalid" ?
            "(/SU)System UUID             R Done \"00000000000000000000000000000000h\"\r\n" :
            "(/SU)System UUID             R Done \"78563412341278569ABC123456789ABCh\"\r\n");
        else if (query == L"/SS") print("(/SS)System Serial number    R Done \"DIRECT-SS\"\r\n");
        else print("(/BS)Baseboard Serial number #1 R Done \"DIRECT-BS\"\r\n");
        return 0;
    }
    std::ofstream file(std::filesystem::path(arguments[2]), std::ios::binary);
    if (mode == "oversized") file << std::string(1024 * 1024 + 1, 'x');
    else if (strings) file << "(/BP)Baseboard product    R Done \"TEST-BOARD\"\r\n"
        "(/SU)System UUID R Done \"AABBCCDD123456789ABC123456789ABCh\"\r\n"
        "(/SS)System Serial number R Done \"BULK-SS\"\r\n"
        "(/BS)Baseboard Serial number R Done \"BULK-BS\"\r\n";
    else file << "[Type 000] -- BIOS Information\r\nName : Embedded Controller Firmware Major Release\r\nData : 01h\r\n"
        "Name : Embedded Controller Firmware Minor Release\r\nData : 02h\r\n";
    file.close();
    print(mode == "timeout" ? "-- More --\r\n<Press any key to continue>\r\n" : "Creating output file... Done\r\n");
    if (mode == "timeout" || mode == "oversized") Sleep(5000);
    return 0;
}

void CheckReadProcess()
{
    using namespace system_info::firmware::detail;
    wchar_t executable[32768]{};
    Check(GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable))) != 0, "Cannot locate test executable");
    const auto root = std::filesystem::temp_directory_path() /
        (L"SystemInfo-ProcessTest-" + std::to_wstring(GetCurrentProcessId()));
    Check(std::filesystem::create_directory(root), "Test directory must be newly created");
    const auto cleanup = [&] {
        for (const auto name : {L"mode.txt", L"strings.txt", L"dump.txt", L"console.txt", L"queries.txt"})
            std::filesystem::remove(root / name);
        Check(std::filesystem::remove(root), "Child output must not remain locked after return");
    };
    const auto mode = [&](const char* value) {
        std::filesystem::remove(root / L"strings.txt");
        std::filesystem::remove(root / L"dump.txt");
        std::filesystem::remove(root / L"queries.txt");
        std::ofstream(root / L"mode.txt") << value;
    };
    try
    {
        mode("success");
        auto result = RunAmiRead(executable, root, ReadMode::Strings, {}, 1500);
        Check(result.success && result.exited && result.exitCode == 0, "File-mode child failed");
        Check(system_info::firmware::ParseAmiStrings(result.output).commands.at(L"BP") == L"TEST-BOARD", "Dedicated result file was not read");
        Check(result.console.find(L"Creating output") != std::wstring::npos, "Console evidence lost");
        result = RunAmiRead(executable, root, ReadMode::AllData, {}, 1500);
        Check(result.success && system_info::firmware::ParseEcDump(result.output) == L"01.02", "DUMPALL file-mode EC read failed");

        mode("timeout");
        result = RunAmiRead(executable, root, ReadMode::Strings, {}, 250);
        Check(!result.success && result.exited && result.elapsedMilliseconds < 2000, "Timed-out child not terminated and reaped");
        Check(result.error.find(L"超时") != std::wstring::npos, "Timeout reason must be explicit");
        Check(result.console.find(L"-- More --") != std::wstring::npos, "Timeout discarded the pagination prompt");
        Check(result.output.find(L"TEST-BOARD") != std::wstring::npos, "Timeout discarded completed read rows");

        mode("error");
        result = RunAmiRead(executable, root, ReadMode::Strings, {}, 1500);
        Check(!result.success && result.exitCode == 16 && result.console.find(L"Driver test failure") != std::wstring::npos,
            "Nonzero exit evidence missing");

        mode("oversized");
        result = RunAmiRead(executable, root, ReadMode::Strings, {}, 1500);
        Check(!result.success && result.exited && result.output.size() <= 1024 * 1024 && result.error.find(L"1 MiB") != std::wstring::npos,
            "Output cap must terminate child and preserve bounded evidence");

        mode("timeout");
        std::stop_source stop;
        std::jthread cancel([&] { Sleep(100); stop.request_stop(); });
        result = RunAmiRead(executable, root, ReadMode::Strings, stop.get_token(), 3000);
        Check(!result.success && result.exited && result.error.find(L"取消") != std::wstring::npos,
            "Cancellation did not stop child");

        for (const auto scenario : {"success", "ss-error", "bs-error", "uuid-invalid"})
        {
            mode(scenario);
            const auto read = ReadAmiFirmware(executable, root, {}, 1500);
            Check(read.allProcessesExited, "Identity query child did not exit");
            ftc::hardware_info::Snapshot snapshot;
            snapshot.rows = {{L"UUID", L"WMI UUID"}, {L"系统SN", L"WMI SS"}, {L"主板SN", L"WMI BS"}};
            system_info::firmware::ApplyAmiValues(read.result.values, snapshot);
            const std::string_view name(scenario);
            Check(snapshot.rows[0].value == (name == "uuid-invalid" ? L"WMI UUID" : L"78563412341278569ABC123456789ABC"),
                "UUID must use /SU; only failed /SU may keep the WMI value");
            Check(snapshot.rows[1].value == (name == "ss-error" ? L"WMI SS" : L"DIRECT-SS"),
                "System SN must use /SS; another query failure must not change it");
            Check(snapshot.rows[2].value == (name == "bs-error" ? L"WMI BS" : L"DIRECT-BS"),
                "Board SN must use /BS; another query failure must not change it");
            std::ifstream queries(root / L"queries.txt", std::ios::binary);
            const std::string calls{std::istreambuf_iterator<char>(queries), std::istreambuf_iterator<char>()};
            Check(calls == "/SU\r\n/SS\r\n/BS\r\n/ALL\r\n/DUMPALL\r\n", "Direct read queries were skipped or reordered");
            Check(read.result.evidence.find(L"AMIDEWINx64.EXE /SU\r\n") != std::wstring::npos &&
                read.result.evidence.find(L"AMIDEWINx64.EXE /SS\r\n") != std::wstring::npos &&
                read.result.evidence.find(L"AMIDEWINx64.EXE /BS\r\n") != std::wstring::npos,
                "Identity query diagnostics did not retain actual tool output");
        }
        cleanup();
    }
    catch (...) { cleanup(); throw; }
}

void CheckDriverOwnership()
{
    using system_info::firmware::detail::OwnedDriverDirectory;
    const std::filesystem::path root = LR"(C:\Users\tester\AppData\Local\Temp\)";
    const std::wstring name = L"SystemInfo-AMI-{8F3DCE7F-E26B-4EDA-94FF-A707425A8F3C}";
    const auto own = root / name / L"amigendrv64.sys";
    Check(OwnedDriverDirectory(L"\\??\\" + own.wstring(), root).has_value(), "Actual GENERICDRV ImagePath must be recognized");
    Check(OwnedDriverDirectory(L"\"" + own.wstring() + L"\"", root).has_value(), "Quoted driver path rejected");
    Check(!OwnedDriverDirectory(LR"(D:\常用工具\amigendrv64.sys)", root), "Foreign AMI driver must not be cleaned");
    Check(!OwnedDriverDirectory((root / L"other" / L"amigendrv64.sys").wstring(), root), "Foreign temp driver must not be cleaned");
    Check(!OwnedDriverDirectory((root / L"SystemInfo-AMI-not-a-guid" / L"amigendrv64.sys").wstring(), root), "Malformed ownership marker accepted");
    Check(!OwnedDriverDirectory((root / name / L"other.sys").wstring(), root), "Unrelated driver accepted");
    Check(!OwnedDriverDirectory((root / name / L".." / L".." / name / L"amigendrv64.sys").wstring(), root), "Escaping path accepted");
}
}

int wmain(int count, wchar_t** arguments)
{
    try
    {
        if (count > 1 && (std::wstring_view(arguments[1]) == L"/ALL" || std::wstring_view(arguments[1]) == L"/DUMPALL" ||
            std::wstring_view(arguments[1]) == L"/SU" || std::wstring_view(arguments[1]) == L"/SS" ||
            std::wstring_view(arguments[1]) == L"/BS"))
            return FakeAmi(count, arguments);
        if (count == 4 && std::wstring_view(arguments[1]) == L"--resources")
            CheckResources(arguments[2], arguments[3]);
        else if (count == 2 && std::wstring_view(arguments[1]) == L"--process") { CheckReadProcess(); CheckDriverOwnership(); }
        else { CheckReadRows(); CheckEc(); CheckFallback(); }
        std::cout << "AMI firmware checks passed.\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
