#include "HtmlReport.h"
#include "resource.h"
#include <array>

namespace elm
{
    namespace
    {
        std::wstring HtmlText(const std::wstring &text)
        {
            std::wstring escaped;
            for (const auto ch : text)
                switch (ch)
                {
                case L'&': escaped += L"&amp;"; break;
                case L'<': escaped += L"&lt;"; break;
                case L'>': escaped += L"&gt;"; break;
                case L'\"': escaped += L"&quot;"; break;
                case L'\'': escaped += L"&#39;"; break;
                default: escaped += ch;
                }
            return escaped;
        }
        std::wstring ScriptData(const std::wstring &json)
        {
            std::wstring escaped;
            escaped.reserve(json.size());
            // Even application/json script elements are terminated by an HTML </script>.
            for (const auto ch : json)
                switch (ch)
                {
                case L'<': escaped += L"\\u003c"; break;
                case L'>': escaped += L"\\u003e"; break;
                case L'&': escaped += L"\\u0026"; break;
                case 0x2028: escaped += L"\\u2028"; break;
                case 0x2029: escaped += L"\\u2029"; break;
                default: escaped += ch;
                }
            return escaped;
        }
        const std::wstring &ReportTemplate()
        {
            static const auto text = []
            {
                const auto module = GetModuleHandleW(nullptr);
                const auto resource = FindResourceW(module, MAKEINTRESOURCEW(IDR_HTML_REPORT), RT_RCDATA);
                if (!resource)
                    throw Error(GetLastError(), L"读取 HTML 报告模板");
                const auto loaded = LoadResource(module, resource);
                const auto bytes = static_cast<const char *>(LockResource(loaded));
                const auto length = SizeofResource(module, resource);
                if (!bytes || !length)
                    throw Error(ERROR_RESOURCE_DATA_NOT_FOUND, L"读取 HTML 报告模板");
                const int characters = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, static_cast<int>(length), nullptr, 0);
                if (!characters)
                    throw Error(GetLastError(), L"解码 HTML 报告模板");
                std::wstring value(characters, L'\0');
                MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, static_cast<int>(length), value.data(), characters);
                return value;
            }();
            return text;
        }
    }

    std::wstring ReportHtml(const std::wstring &title, const std::wstring &json,
                            const std::filesystem::path &root, const std::filesystem::path &path,
                            const std::wstring &kind, const std::wstring &status, const std::wstring &message)
    {
        auto base = root.lexically_relative(path.parent_path()).generic_wstring();
        if (base.empty())
            base = L".";
        base += L"/";
        const auto settings = L"{\"title\":" + JsonQuote(title) + L",\"root\":" + JsonQuote(root.wstring()) +
                              L",\"base\":" + JsonQuote(base) + L",\"kind\":" + JsonQuote(kind) +
                              L",\"status\":" + JsonQuote(status) + L",\"message\":" + JsonQuote(message) + L"}";
        const std::array<std::pair<std::wstring, std::wstring>, 3> tokens{{
            {L"@@TITLE@@", HtmlText(title)}, {L"@@SETTINGS@@", ScriptData(settings)}, {L"@@DATA@@", ScriptData(json)}}};
        const auto &source = ReportTemplate();
        std::wstring html;
        html.reserve(source.size() + json.size() + settings.size());
        size_t cursor = 0;
        while (cursor < source.size())
        {
            const auto marker = source.find(L"@@", cursor);
            if (marker == std::wstring::npos)
            {
                html.append(source, cursor);
                break;
            }
            html.append(source, cursor, marker - cursor);
            bool found = false;
            for (const auto &[token, value] : tokens)
                if (source.compare(marker, token.size(), token) == 0)
                {
                    html += value;
                    cursor = marker + token.size();
                    found = true;
                    break;
                }
            if (!found)
                throw Error(ERROR_INVALID_DATA, L"无效的 HTML 报告模板");
        }
        return html;
    }
}
