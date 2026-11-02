#include "Diagnostics.h"
#include <sddl.h>
#include <algorithm>
#include <cwctype>
#include <map>

namespace elm
{
    namespace
    {
        namespace fs = std::filesystem;
        class Key
        {
        public:
            HKEY value = nullptr;
            ~Key() { if (value) RegCloseKey(value); }
        };
        std::wstring Lower(std::wstring text)
        {
            std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) { return static_cast<wchar_t>(towlower(ch)); });
            return text;
        }
        void Problem(std::vector<CollectedFile> &files, const std::wstring &source, DWORD error)
        {
            CollectedFile file;
            file.source = source;
            file.error = error;
            file.state = CollectionState::Failed;
            file.message = SystemError(error);
            files.push_back(std::move(file));
        }
        std::vector<std::wstring> Subkeys(HKEY key, const std::wstring &source, std::vector<CollectedFile> &issues,
                                         const DiagnosticContext &context)
        {
            std::vector<std::wstring> names;
            for (DWORD index = 0;; ++index)
            {
                CheckDiagnosticCancel(context);
                wchar_t name[256]{};
                DWORD size = static_cast<DWORD>(std::size(name));
                const DWORD error = RegEnumKeyExW(key, index, name, &size, nullptr, nullptr, nullptr, nullptr);
                if (error == ERROR_NO_MORE_ITEMS)
                    break;
                if (error || index >= 256)
                {
                    Problem(issues, source, error ? error : ERROR_MORE_DATA);
                    break;
                }
                names.emplace_back(name, size);
            }
            return names;
        }
    }

    std::wstring ExpandDiagnosticUserPath(const std::wstring &value, const DiagnosticUser &user, const DiagnosticContext &context)
    {
        const std::map<std::wstring, fs::path> variables{
            {L"userprofile", user.profile}, {L"localappdata", user.localAppData},
            {L"appdata", user.profile / L"AppData" / L"Roaming"},
            {L"temp", user.localAppData / L"Temp"}, {L"tmp", user.localAppData / L"Temp"},
            {L"homedrive", user.profile.root_name()}, {L"homepath", user.profile.relative_path().empty() ? fs::path{} : fs::path(L"\\") / user.profile.relative_path()},
            {L"systemroot", context.windows}, {L"windir", context.windows}, {L"systemdrive", context.windows.root_name()},
            {L"programdata", context.programData}, {L"allusersprofile", context.programData}};
        std::wstring expanded;
        for (size_t offset = 0; offset < value.size();)
        {
            if (value[offset] != L'%')
            {
                expanded += value[offset++];
                continue;
            }
            const auto end = value.find(L'%', offset + 1);
            if (end == std::wstring::npos)
                throw Error(ERROR_BAD_ENVIRONMENT, L"路径包含未解析的环境变量");
            const auto found = variables.find(Lower(value.substr(offset + 1, end - offset - 1)));
            if (found != variables.end() && !found->second.empty())
                expanded += found->second.wstring();
            else if (user.id == L"CurrentUser")
            {
                const auto token = value.substr(offset, end - offset + 1);
                const auto replacement = ExpandedEnvironment(token);
                if (replacement.find(L'%') != std::wstring::npos)
                    throw Error(ERROR_BAD_ENVIRONMENT, L"无法解析当前账户路径变量：" + token);
                expanded += replacement;
            }
            else
                throw Error(ERROR_BAD_ENVIRONMENT, L"无法为账户 " + user.id + L" 解析路径变量");
            offset = end + 1;
        }
        const fs::path path(expanded);
        if (!path.is_absolute())
            throw Error(ERROR_BAD_PATHNAME, L"诊断文件夹必须是绝对路径：" + expanded);
        return path.lexically_normal().wstring();
    }

    std::vector<DiagnosticUser> DiagnosticUsers(const DiagnosticContext &context)
    {
        std::vector<DiagnosticUser> users;
        if (!context.localAppData.empty())
            users.push_back({L"CurrentUser", context.currentUserProfile, context.localAppData});
        for (const auto &user : context.otherUsers)
            if (!user.localAppData.empty() && std::none_of(users.begin(), users.end(), [&](const auto &existing)
                { return Lower(existing.localAppData.lexically_normal().wstring()) == Lower(user.localAppData.lexically_normal().wstring()); }))
                users.push_back(user);
        return users;
    }

    void DiscoverDiagnosticUsers(DiagnosticContext &context)
    {
        context.userProfilesResolved = true;
        constexpr auto source = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ProfileList";
        Key profiles;
        const DWORD error = RegOpenKeyExW(HKEY_LOCAL_MACHINE, source, 0, KEY_READ | KEY_WOW64_64KEY, &profiles.value);
        if (error)
        {
            Problem(context.userProfileIssues, source, error);
            return;
        }
        for (const auto &sid : Subkeys(profiles.value, source, context.userProfileIssues, context))
        {
            PSID parsed = nullptr;
            if (!ConvertStringSidToSidW(sid.c_str(), &parsed))
                continue; // ProfileList also contains backup/non-profile subkeys.
            LocalFree(parsed);
            try
            {
                const auto configured = ReadRegistryString(profiles.value, sid.c_str(), L"ProfileImagePath");
                if (configured.empty())
                    continue;
                DiagnosticUser user{sid, ExpandedEnvironment(configured), {}};
                if (!user.profile.is_absolute())
                    throw Error(ERROR_BAD_PATHNAME, L"用户配置文件路径无效");
                if (!context.currentUserProfile.empty() && Lower(user.profile.lexically_normal().wstring()) ==
                    Lower(context.currentUserProfile.lexically_normal().wstring()))
                    continue;
                user.localAppData = user.profile / L"AppData" / L"Local";
                // Read only already loaded user hives; never mount NTUSER.DAT or impersonate an account.
                const auto shell = sid + L"\\Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\User Shell Folders";
                try
                {
                    const auto local = ReadRegistryString(HKEY_USERS, shell.c_str(), L"Local AppData");
                    if (!local.empty())
                        user.localAppData = ExpandDiagnosticUserPath(local, user, context);
                }
                catch (const Error &failure)
                {
                    Problem(context.userProfileIssues, shell, failure.code);
                }
                context.otherUsers.push_back(std::move(user));
            }
            catch (const Error &failure)
            {
                Problem(context.userProfileIssues, std::wstring(source) + L"\\" + sid, failure.code);
            }
        }
    }

    std::vector<DiagnosticDumpFolder> DiagnosticUserDumpFolders(CollectedLog &item, const DiagnosticContext &context)
    {
        const auto users = DiagnosticUsers(context);
        std::vector<DiagnosticDumpFolder> folders;
        bool truncated = false;
        auto add = [&](const fs::path &directory, const fs::path &prefix, const std::wstring &source)
        {
            const auto normalized = Lower(directory.lexically_normal().wstring());
            for (auto &folder : folders)
                if (Lower(folder.directory.lexically_normal().wstring()) == normalized)
                {
                    folder.source += L"; " + source;
                    return;
                }
            if (folders.size() >= 1024)
            {
                if (!truncated)
                    Problem(item.files, L"LocalDumps: folder discovery limit", ERROR_MORE_DATA);
                truncated = true;
                return;
            }
            folders.push_back({directory, prefix, source});
        };
        for (const auto &user : users)
            add(user.localAppData / L"CrashDumps", user.id == L"CurrentUser" ? fs::path(user.id) : fs::path(L"Users") / user.id,
                user.id + L": %LOCALAPPDATA%\\CrashDumps");
        if (!context.resolveDumpPaths)
            return folders;
        for (const auto view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
        {
            const auto origin = context.localDumpsKey + (view == KEY_WOW64_64KEY ? L" [64-bit]" : L" [32-bit]");
            Key settings;
            const DWORD error = RegOpenKeyExW(context.localDumpsRegistry, context.localDumpsKey.c_str(), 0, KEY_READ | view, &settings.value);
            if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND)
                continue;
            if (error)
            {
                Problem(item.files, origin, error);
                continue;
            }
            auto names = Subkeys(settings.value, origin, item.files, context);
            names.insert(names.begin(), L""); // Global defaults, then per-application overrides.
            for (const auto &name : names)
            {
                if (truncated)
                    break;
                CheckDiagnosticCancel(context);
                std::wstring configured;
                const auto source = origin + (name.empty() ? L"" : L"\\" + name) + L"\\DumpFolder";
                try
                {
                    configured = ReadRegistryString(settings.value, name.empty() ? nullptr : name.c_str(), L"DumpFolder");
                }
                catch (const Error &failure)
                {
                    Problem(item.files, source, failure.code);
                    continue;
                }
                if (configured.empty())
                    continue;
                for (const auto &user : users)
                {
                    try
                    {
                        const auto expanded = ExpandDiagnosticUserPath(configured, user, context);
                        add(expanded, fs::path(L"Configured") / (L"Location_" + std::to_wstring(folders.size() + 1)), source + L" (" + user.id + L")");
                    }
                    catch (const Error &failure)
                    {
                        Problem(item.files, source + L" (" + user.id + L")", failure.code);
                    }
                }
            }
        }
        return folders;
    }
}
