#include "AmiFirmware.h"
#include <algorithm>
#include <cwctype>
#include <optional>
#include <sstream>
#include <regex>

namespace system_info::firmware
{
namespace
{
std::wstring Trim(std::wstring text)
{
    const auto begin = text.find_first_not_of(L" \r\n\t");
    if (begin == std::wstring::npos) return {};
    return text.substr(begin, text.find_last_not_of(L" \r\n\t") - begin + 1);
}
std::wstring Lower(std::wstring text)
{
    for (auto& character : text) character = static_cast<wchar_t>(std::towlower(character));
    return text;
}
bool Usable(const std::wstring& text)
{
    const auto normalized = Lower(Trim(text));
    return !normalized.empty() && normalized != L"<empty>" && normalized != L"n/a" &&
        normalized != L"not supported" && normalized != L"not specified" && normalized != L"unknown";
}
std::wstring UuidDigits(std::wstring_view value)
{
    // /SU appends a radix marker. WMI commonly separates the same digit groups with hyphens.
    if (value.ends_with(L'h') || value.ends_with(L'H')) value.remove_suffix(1);
    std::wstring digits;
    for (const wchar_t c : value)
    {
        if (c == L'-' || c == L' ') continue;
        if (!std::iswxdigit(c)) return {};
        digits += c;
    }
    if (digits.size() != 32 || digits.find_first_not_of(L'0') == std::wstring::npos ||
        digits.find_first_not_of(L"Ff") == std::wstring::npos) return {};
    return digits;
}
std::wstring Get(const AmiValues& values, const wchar_t* command)
{
    const auto found = values.commands.find(command);
    return found == values.commands.end() ? std::wstring{} : found->second;
}
void Replace(ftc::hardware_info::Snapshot& snapshot, const wchar_t* label, const std::wstring& value)
{
    if (!Usable(value)) return;
    for (auto& row : snapshot.rows)
        if (row.label == label) { row.value = value; return; }
}
std::wstring DateText(const std::wstring& date)
{
    static const std::wregex pattern(LR"(^(\d{2})/(\d{2})/(\d{4})$)");
    std::wsmatch match;
    if (std::regex_match(date, match, pattern)) return match[3].str() + L"/" + match[1].str() + L"/" + match[2].str();
    return date;
}
std::optional<unsigned int> ByteValue(std::wstring text)
{
    text = Lower(Trim(std::move(text)));
    int base = 10;
    if (text.ends_with(L'h')) { text.pop_back(); base = 16; }
    if (text.starts_with(L"0x")) { text.erase(0, 2); base = 16; }
    if (text.empty() || text.size() > 3) return {};
    if (!std::all_of(text.begin(), text.end(), [base](wchar_t c) {
        return (c >= L'0' && c <= L'9') || (base == 16 && c >= L'a' && c <= L'f');
    })) return {};
    try
    {
        const auto value = std::stoul(text, nullptr, base);
        if (value <= 255) return static_cast<unsigned int>(value);
    }
    catch (...) {}
    return {};
}
}

AmiValues ParseAmiStrings(std::wstring_view output)
{
    AmiValues result;
    // AMIDEWIN 5.27's embedded format: (/<key>)<name>[#n] R Done "<value>".
    // Requiring R + Done rejects write acknowledgements, errors and empty rows.
    static const std::wregex rowPattern(
        LR"ami(^\s*\(/(IVN|IV|ID|BP|BS|SS|SU)\)[^"\r\n]*?\sR\s+Done\s+"(.*)"\s*$)ami",
        std::regex_constants::icase);
    std::wistringstream lines{std::wstring(output)};
    std::wstring line;
    while (std::getline(lines, line))
    {
        if (line.size() > 4096) continue;
        std::wsmatch match;
        if (!std::regex_match(line, match, rowPattern)) continue;
        auto command = match[1].str();
        for (auto& c : command) c = static_cast<wchar_t>(std::towupper(c));
        auto value = Trim(match[2].str());
        if (command == L"SU") value = UuidDigits(value);
        if (!Usable(value)) continue;
        result.commands.try_emplace(std::move(command), std::move(value)); // First usable board instance, as before.
    }
    return result;
}

std::wstring ParseEcDump(std::wstring_view output)
{
    std::optional<unsigned int> major, minor;
    std::wistringstream lines{std::wstring(output)};
    std::wstring line, field;
    bool typeZero = false;
    while (std::getline(lines, line))
    {
        const auto trimmed = Trim(line);
        if (trimmed.starts_with(L"[Type "))
        {
            typeZero = trimmed.starts_with(L"[Type 000]") || trimmed.starts_with(L"[Type 0]");
            field.clear();
        }
        if (!typeZero) continue;
        const auto colon = trimmed.find(L':');
        if (colon == std::wstring::npos) continue;
        const auto key = Lower(Trim(trimmed.substr(0, colon)));
        const auto value = Trim(trimmed.substr(colon + 1));
        if (key == L"name")
        {
            field = Lower(value);
            // The file dump places "Style : UINT8" in a second column on
            // the same line as Name, unlike the separate-line screen form.
            const auto style = field.find(L"style");
            if (style != std::wstring::npos && style > 0 && std::iswspace(field[style - 1]))
                field = Trim(field.substr(0, style));
        }
        else if (key == L"data")
        {
            if (field == L"embedded controller firmware major release" && !major) major = ByteValue(value);
            if (field == L"embedded controller firmware minor release" && !minor) minor = ByteValue(value);
            field.clear();
        }
    }
    if (!major || !minor) return {};
    const auto number = [](unsigned int value) {
        return (value < 10 ? L"0" : L"") + std::to_wstring(value);
    };
    return number(*major) + L"." + number(*minor);
}

void ApplyAmiValues(const AmiValues& values, ftc::hardware_info::Snapshot& snapshot)
{
    Replace(snapshot, L"主板型号", Get(values, L"BP"));
    Replace(snapshot, L"主板SN", Get(values, L"BS"));
    Replace(snapshot, L"系统SN", Get(values, L"SS"));
    for (auto& row : snapshot.rows)
    {
        if (row.label != L"UUID") continue;
        auto uuid = UuidDigits(Get(values, L"SU"));
        if (uuid.empty()) uuid = UuidDigits(row.value);
        if (!uuid.empty()) row.value = std::move(uuid);
        break;
    }
    Replace(snapshot, L"EC版本", values.ecVersion);
    const auto vendor = Get(values, L"IVN"), version = Get(values, L"IV"), date = Get(values, L"ID");
    // A composite BIOS row falls back as a whole if a component is unavailable.
    if (!vendor.empty() && !version.empty() && !date.empty())
        Replace(snapshot, L"BIOS版本", vendor + L" " + version + L", " + DateText(date));
}

ftc::hardware_info::Snapshot CollectComputerInfo(std::stop_token token)
{
    AmiResult ami;
    try { ami = ReadEmbeddedAmi(token); }
    catch (...) { ami.diagnostic = L"AMIDEWIN 读取异常，固件信息已回退到原采集方案。"; }
    if (token.stop_requested()) return {};
    auto snapshot = ftc::hardware_info::CollectSnapshot(token);
    ApplyAmiValues(ami.values, snapshot);
    if (!ami.diagnostic.empty() && !token.stop_requested())
    {
        if (!snapshot.diagnostic.empty()) snapshot.diagnostic += L"\r\n";
        snapshot.diagnostic += ami.diagnostic;
    }
    if (!token.stop_requested())
    {
        try { snapshot.battery = power::Read(); }
        catch (...) { /* Other hardware rows are still valid. */ }
        snapshot.rows.push_back({L"电池信息", snapshot.battery ? power::FormatReport(*snapshot.battery) : L"N/A"});
    }
    return snapshot;
}
}
