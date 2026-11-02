#include "Diagnostics.h"
#include <wbemidl.h>
#include <wrl/client.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <sstream>

namespace elm
{
    namespace
    {
        using Microsoft::WRL::ComPtr;
        class RegistryKey
        {
        public:
            HKEY value = nullptr;
            ~RegistryKey()
            {
                if (value && value != INVALID_HANDLE_VALUE)
                    RegCloseKey(value);
            }
        };
        struct RegistryValue
        {
            DWORD type = REG_NONE, error = ERROR_SUCCESS;
            std::vector<unsigned char> data;
        };
        RegistryValue ReadValue(HKEY root, const wchar_t *key, const wchar_t *name)
        {
            RegistryValue value;
            DWORD bytes = 0;
            value.error = RegGetValueW(root, key, name, RRF_RT_ANY | RRF_NOEXPAND, &value.type, nullptr, &bytes);
            if (value.error != ERROR_SUCCESS)
                return value;
            if (bytes > 1024 * 1024)
            {
                value.error = ERROR_MORE_DATA;
                return value;
            }
            value.data.resize(static_cast<size_t>(bytes) + 2 * sizeof(wchar_t));
            value.error = RegGetValueW(root, key, name, RRF_RT_ANY | RRF_NOEXPAND, &value.type, value.data.data(), &bytes);
            return value;
        }
        std::wstring MultiStringJson(const wchar_t *data, size_t characters)
        {
            std::wstring json = L"[";
            size_t start = 0;
            bool first = true;
            while (start < characters && data[start])
            {
                size_t end = start;
                while (end < characters && data[end])
                    ++end;
                if (!first)
                    json += L",";
                first = false;
                json += JsonQuote(std::wstring(data + start, end - start));
                start = end + 1;
            }
            return json + L"]";
        }
        std::wstring RegistryJson(const RegistryValue &value)
        {
            if (value.error != ERROR_SUCCESS)
                return L"null";
            if (value.type == REG_DWORD && value.data.size() >= sizeof(DWORD))
            {
                DWORD number{};
                memcpy(&number, value.data.data(), sizeof(number));
                return std::to_wstring(number);
            }
            if (value.type == REG_QWORD && value.data.size() >= sizeof(uint64_t))
            {
                uint64_t number{};
                memcpy(&number, value.data.data(), sizeof(number));
                return std::to_wstring(number);
            }
            if (value.type == REG_SZ || value.type == REG_EXPAND_SZ)
                return JsonQuote(reinterpret_cast<const wchar_t *>(value.data.data()));
            if (value.type == REG_MULTI_SZ)
                return MultiStringJson(reinterpret_cast<const wchar_t *>(value.data.data()), value.data.size() / sizeof(wchar_t));
            return L"null";
        }
        void HResult(HRESULT result, const std::wstring &operation)
        {
            if (FAILED(result))
                throw Error(static_cast<DWORD>(result), operation);
        }
        std::wstring VariantJson(const VARIANT &value)
        {
            if (value.vt == VT_NULL || value.vt == VT_EMPTY)
                return L"null";
            if (value.vt == VT_BOOL)
                return value.boolVal == VARIANT_FALSE ? L"false" : L"true";
            if (value.vt & VT_ARRAY)
            {
                const VARTYPE type = value.vt & VT_TYPEMASK;
                LONG first = 0, last = -1;
                if (!value.parray || SafeArrayGetDim(value.parray) != 1 ||
                    FAILED(SafeArrayGetLBound(value.parray, 1, &first)) || FAILED(SafeArrayGetUBound(value.parray, 1, &last)))
                    return L"null";
                std::wstring json = L"[";
                for (LONG index = first; index <= last && index - first < 4096; ++index)
                {
                    if (index != first)
                        json += L",";
                    if (type == VT_BSTR)
                    {
                        BSTR text = nullptr;
                        const auto result = SafeArrayGetElement(value.parray, &index, &text);
                        json += SUCCEEDED(result) && text ? JsonQuote(std::wstring(text, SysStringLen(text))) : L"null";
                        SysFreeString(text);
                    }
                    else if (type == VT_I4)
                    {
                        LONG number = 0;
                        json += SUCCEEDED(SafeArrayGetElement(value.parray, &index, &number)) ? std::to_wstring(number) : L"null";
                    }
                    else
                        json += L"null";
                }
                return json + L"]";
            }
            switch (value.vt)
            {
            case VT_I1:
                return std::to_wstring(value.cVal);
            case VT_UI1:
                return std::to_wstring(value.bVal);
            case VT_I2:
                return std::to_wstring(value.iVal);
            case VT_UI2:
                return std::to_wstring(value.uiVal);
            case VT_I4:
            case VT_INT:
                return std::to_wstring(value.lVal);
            case VT_UI4:
            case VT_UINT:
                return std::to_wstring(value.ulVal);
            case VT_I8:
                return std::to_wstring(value.llVal);
            case VT_UI8:
                return std::to_wstring(value.ullVal);
            case VT_BSTR:
                return value.bstrVal ? JsonQuote(std::wstring(value.bstrVal, SysStringLen(value.bstrVal))) : L"null";
            default:
            {
                VARIANT converted{};
                VariantInit(&converted);
                const auto result = VariantChangeType(&converted, const_cast<VARIANT *>(&value), VARIANT_ALPHABOOL, VT_BSTR);
                const auto json = SUCCEEDED(result) && converted.bstrVal ? JsonQuote(std::wstring(converted.bstrVal, SysStringLen(converted.bstrVal))) : L"null";
                VariantClear(&converted);
                return json;
            }
            }
        }
        struct Section
        {
            const wchar_t *name;
            std::vector<const wchar_t *> fields;
            std::wstring filter;
            bool fullObject = false;
        };
        struct QueryStats
        {
            size_t rows = 0;
            bool unsupported = false;
        };
        bool Unsupported(DWORD code)
        {
            return code == static_cast<DWORD>(WBEM_E_INVALID_CLASS) || code == static_cast<DWORD>(WBEM_E_NOT_SUPPORTED) ||
                   code == static_cast<DWORD>(WBEM_E_INVALID_NAMESPACE) || code == static_cast<DWORD>(WBEM_E_INVALID_METHOD) ||
                   code == static_cast<DWORD>(WBEM_E_METHOD_NOT_IMPLEMENTED) || code == ERROR_NOT_SUPPORTED;
        }
        class Bstr
        {
        public:
            explicit Bstr(const wchar_t *text) : value_(SysAllocString(text))
            {
                if (!value_)
                    throw Error(ERROR_NOT_ENOUGH_MEMORY, L"创建 WMI 字符串");
            }
            ~Bstr() { SysFreeString(value_); }
            Bstr(const Bstr &) = delete;
            Bstr &operator=(const Bstr &) = delete;
            operator BSTR() const { return value_; }
        private:
            BSTR value_;
        };
        std::wstring ObjectProperty(IWbemClassObject *object, const wchar_t *name)
        {
            if (!object)
                return L"null";
            VARIANT value{};
            const auto result = object->Get(name, 0, &value, nullptr, nullptr);
            const auto json = SUCCEEDED(result) ? VariantJson(value) : L"null";
            VariantClear(&value);
            return json;
        }
        class WmiSession
        {
        public:
            explicit WmiSession(const wchar_t *nameSpace = L"ROOT\\CIMV2")
            {
                const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                if (FAILED(initialized) && initialized != RPC_E_CHANGED_MODE)
                    HResult(initialized, L"初始化 WMI COM");
                uninitialize_ = SUCCEEDED(initialized);
                try
                {
                    static std::once_flag security;
                    static HRESULT securityResult = S_OK;
                    std::call_once(security, []
                                   { securityResult = CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_DEFAULT,
                                                                           RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE, nullptr); });
                    if (securityResult != RPC_E_TOO_LATE)
                        HResult(securityResult, L"初始化 WMI 安全上下文");
                    ComPtr<IWbemLocator> locator;
                    HResult(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator)), L"创建 WMI 连接");
                    BSTR name = SysAllocString(nameSpace);
                    if (!name)
                        throw Error(ERROR_NOT_ENOUGH_MEMORY, L"创建 WMI 命名空间");
                    const auto result = locator->ConnectServer(name, nullptr, nullptr, nullptr, WBEM_FLAG_CONNECT_USE_MAX_WAIT, nullptr, nullptr, &services_);
                    SysFreeString(name);
                    HResult(result, L"连接本机 WMI");
                    HResult(CoSetProxyBlanket(services_.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                                              RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE),
                            L"配置 WMI 代理");
                }
                catch (...)
                {
                    services_.Reset();
                    if (uninitialize_)
                        CoUninitialize();
                    uninitialize_ = false;
                    throw;
                }
            }
            ~WmiSession()
            {
                services_.Reset();
                if (uninitialize_)
                    CoUninitialize();
            }
            std::wstring Query(const Section &section, const DiagnosticContext &context, bool &incomplete,
                               QueryStats *stats = nullptr, const std::function<void(IWbemClassObject *)> &visitor = {})
            {
                std::wstring rows = L"[";
                DWORD code = 0;
                std::wstring message;
                size_t count = 0;
                try
                {
                    CheckDiagnosticCancel(context);
                    std::wstring query = L"SELECT ";
                    if (section.fullObject)
                        query += L"*";
                    else
                    {
                        for (size_t i = 0; i < section.fields.size(); ++i)
                        {
                            if (i)
                                query += L",";
                            query += section.fields[i];
                        }
                    }
                    query += L" FROM " + std::wstring(section.name);
                    if (!section.filter.empty())
                        query += L" WHERE " + section.filter;
                    BSTR language = SysAllocString(L"WQL"), text = SysAllocString(query.c_str());
                    if (!language || !text)
                    {
                        SysFreeString(language);
                        SysFreeString(text);
                        throw Error(ERROR_NOT_ENOUGH_MEMORY, L"创建 WMI 查询");
                    }
                    ComPtr<IEnumWbemClassObject> enumeration;
                    const auto result = services_->ExecQuery(language, text, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &enumeration);
                    SysFreeString(language);
                    SysFreeString(text);
                    HResult(result, L"查询 " + std::wstring(section.name));
                    const auto started = GetTickCount64();
                    for (;;)
                    {
                        CheckDiagnosticCancel(context);
                        if (GetTickCount64() - started > 20000)
                            throw Error(ERROR_TIMEOUT, L"WMI 查询超时（20 秒）");
                        ComPtr<IWbemClassObject> object;
                        ULONG returned = 0;
                        const auto next = enumeration->Next(500, 1, &object, &returned);
                        HResult(next, L"读取 WMI 结果");
                        if (returned)
                        {
                            if (count >= 10000 || rows.size() >= 16ULL * 1024 * 1024)
                                throw Error(ERROR_MORE_DATA, L"WMI 返回条数达到上限");
                            if (count++)
                                rows += L",";
                            rows += L"\n{";
                            for (size_t i = 0; i < section.fields.size(); ++i)
                            {
                                if (i)
                                    rows += L",";
                                VARIANT value{};
                                VariantInit(&value);
                                const auto get = object->Get(section.fields[i], 0, &value, nullptr, nullptr);
                                rows += JsonQuote(section.fields[i]) + L":" + (SUCCEEDED(get) ? VariantJson(value) : L"null");
                                VariantClear(&value);
                            }
                            rows += L"}";
                            if (visitor)
                                visitor(object.Get());
                        }
                        if (next == WBEM_S_FALSE)
                            break;
                        // WBEM_S_TIMEDOUT is not end of enumeration.
                    }
                }
                catch (const Error &error)
                {
                    if (error.code == ERROR_CANCELLED)
                        throw;
                    code = error.code;
                    message = error.text;
                }
                const bool unsupported = Unsupported(code);
                if (stats)
                {
                    stats->rows = count;
                    stats->unsupported = unsupported;
                }
                if (code && !unsupported)
                    incomplete = true;
                return L"{\"class\":" + JsonQuote(section.name) + L",\"status\":" +
                       JsonQuote(!code ? L"saved" : unsupported ? L"skipped"
                                                                : L"partial") +
                       L",\"errorCode\":" + std::to_wstring(code) + L",\"message\":" + JsonQuote(message) +
                       L",\"rows\":" + rows + L"\n]}";
            }

            std::wstring StorageCounter(IWbemClassObject *disk, const DiagnosticContext &context, bool &incomplete)
            {
                ComPtr<IWbemClassObject> counter;
                DWORD code = 0;
                std::wstring message;
                try
                {
                    CheckDiagnosticCancel(context);
                    Bstr className(L"PS_StorageCmdlets"), method(L"GetStorageReliabilityCounter");
                    ComPtr<IWbemClassObject> definition, inputDefinition, input;
                    HResult(services_->GetObject(className, 0, nullptr, &definition, nullptr), L"读取存储诊断接口");
                    HResult(definition->GetMethod(method, 0, &inputDefinition, nullptr), L"读取磁盘计数器方法");
                    HResult(inputDefinition->SpawnInstance(0, &input), L"创建磁盘计数器参数");
                    VARIANT argument{};
                    argument.vt = VT_UNKNOWN;
                    argument.punkVal = disk; // Put copies the value; the caller retains ownership of disk.
                    HResult(input->Put(L"PhysicalDisk", 0, &argument, 0), L"设置磁盘计数器参数");
                    ComPtr<IWbemCallResult> pending;
                    HResult(services_->ExecMethod(className, method, WBEM_FLAG_RETURN_IMMEDIATELY, nullptr,
                                                 input.Get(), nullptr, &pending), L"读取存储可靠性计数器");
                    if (!pending)
                        throw Error(ERROR_INVALID_DATA, L"存储计数器接口未返回操作句柄");
                    const auto started = GetTickCount64();
                    for (;;)
                    {
                        CheckDiagnosticCancel(context);
                        LONG operation = 0;
                        const auto wait = pending->GetCallStatus(500, &operation);
                        HResult(wait, L"等待存储可靠性计数器");
                        if (wait != WBEM_S_TIMEDOUT)
                        {
                            HResult(operation, L"读取存储可靠性计数器");
                            break;
                        }
                        if (GetTickCount64() - started >= 20000)
                            throw Error(ERROR_TIMEOUT, L"存储计数器读取超时（20 秒）");
                    }
                    ComPtr<IWbemClassObject> output;
                    HResult(pending->GetResultObject(0, &output), L"读取存储计数器结果");
                    if (!output)
                        throw Error(ERROR_INVALID_DATA, L"存储计数器接口未返回结果对象");
                    VARIANT value{};
                    if (SUCCEEDED(output->Get(L"ReturnValue", 0, &value, nullptr, nullptr)))
                    {
                        if (value.vt == VT_I4 || value.vt == VT_UI4)
                            code = value.ulVal;
                        VariantClear(&value);
                        if (code)
                            throw Error(code, L"存储计数器方法返回失败");
                    }
                    const auto read = output->Get(L"StorageReliabilityCounter", 0, &value, nullptr, nullptr);
                    if (SUCCEEDED(read) && value.vt == VT_UNKNOWN && value.punkVal)
                        value.punkVal->QueryInterface(IID_PPV_ARGS(&counter));
                    else if (SUCCEEDED(read) && value.vt == (VT_ARRAY | VT_UNKNOWN) && value.parray)
                    {
                        LONG first = 0, last = -1;
                        if (SUCCEEDED(SafeArrayGetLBound(value.parray, 1, &first)) && SUCCEEDED(SafeArrayGetUBound(value.parray, 1, &last)) && first <= last)
                        {
                            ComPtr<IUnknown> embedded;
                            if (SUCCEEDED(SafeArrayGetElement(value.parray, &first, embedded.GetAddressOf())) && embedded)
                                embedded.As(&counter);
                        }
                    }
                    VariantClear(&value);
                    HResult(read, L"读取存储可靠性数据");
                    if (!counter)
                        message = L"设备或存储提供程序未返回可靠性计数器。";
                }
                catch (const Error &error)
                {
                    if (error.code == ERROR_CANCELLED)
                        throw;
                    code = error.code;
                    message = error.text;
                }
                if (code && !Unsupported(code))
                    incomplete = true;
                std::wstring json = L"{\"DeviceId\":" + ObjectProperty(disk, L"DeviceId") +
                    L",\"FriendlyName\":" + ObjectProperty(disk, L"FriendlyName") + L",\"status\":" +
                    JsonQuote(code ? Unsupported(code) ? L"skipped" : L"failed" : counter ? L"saved" : L"not_found") +
                    L",\"errorCode\":" + std::to_wstring(code) + L",\"message\":" + JsonQuote(message);
                for (const auto *field : {L"Temperature", L"TemperatureMax", L"Wear", L"PowerOnHours", L"ReadErrorsTotal",
                    L"ReadErrorsCorrected", L"ReadErrorsUncorrected", L"WriteErrorsTotal", L"WriteErrorsCorrected",
                    L"WriteErrorsUncorrected", L"ReadLatencyMax", L"WriteLatencyMax", L"FlushLatencyMax",
                    L"StartStopCycleCount", L"LoadUnloadCycleCount"})
                    json += L"," + JsonQuote(field) + L":" + ObjectProperty(counter.Get(), field);
                return json + L"}";
            }

        private:
            ComPtr<IWbemServices> services_;
            bool uninitialize_ = false;
        };
        std::wstring WmiSections(const std::vector<Section> &sections, const DiagnosticContext &context, bool &incomplete)
        {
            WmiSession session;
            std::wstring json = L"[";
            for (size_t i = 0; i < sections.size(); ++i)
            {
                if (i)
                    json += L",";
                json += session.Query(sections[i], context, incomplete);
            }
            return json + L"]";
        }
        std::wstring DeviceProperty(HDEVINFO devices, SP_DEVINFO_DATA &device, DWORD property, bool multi = false)
        {
            DWORD type = 0, bytes = 0;
            SetupDiGetDeviceRegistryPropertyW(devices, &device, property, &type, nullptr, 0, &bytes);
            if (!bytes || bytes > 1024 * 1024)
                return multi ? L"[]" : L"null";
            std::vector<unsigned char> data(static_cast<size_t>(bytes) + 2 * sizeof(wchar_t));
            if (!SetupDiGetDeviceRegistryPropertyW(devices, &device, property, &type, data.data(), bytes, nullptr))
                return multi ? L"[]" : L"null";
            if (multi && type == REG_MULTI_SZ)
                return MultiStringJson(reinterpret_cast<const wchar_t *>(data.data()), data.size() / sizeof(wchar_t));
            return type == REG_SZ ? JsonQuote(reinterpret_cast<const wchar_t *>(data.data())) : L"null";
        }
        std::wstring DriverProperties(HDEVINFO devices, SP_DEVINFO_DATA &device)
        {
            RegistryKey key;
            key.value = SetupDiOpenDevRegKey(devices, &device, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
            if (key.value == INVALID_HANDLE_VALUE)
            {
                key.value = nullptr;
                return L"null";
            }
            std::wstring json = L"{";
            bool first = true;
            for (const auto *name : {L"DriverVersion", L"DriverDate", L"ProviderName", L"InfPath", L"InfSection", L"MatchingDeviceId"})
            {
                if (!first)
                    json += L",";
                first = false;
                json += JsonQuote(name) + L":" + RegistryJson(ReadValue(key.value, nullptr, name));
            }
            return json + L"}";
        }
    }

    std::wstring ReadRegistryString(HKEY root, const wchar_t *key, const wchar_t *name)
    {
        const auto value = ReadValue(root, key, name);
        if (value.error == ERROR_FILE_NOT_FOUND || value.error == ERROR_PATH_NOT_FOUND)
            return L"";
        if (value.error)
            throw Error(value.error, L"读取注册表：" + std::wstring(name));
        if (value.type != REG_SZ && value.type != REG_EXPAND_SZ)
            throw Error(ERROR_DATATYPE_MISMATCH, L"注册表路径类型无效");
        return reinterpret_cast<const wchar_t *>(value.data.data());
    }
    void CollectSystemSnapshot(CollectedLog &item, const DiagnosticContext &context)
    {
        item.path = (context.output / L"SystemInfo.html").wstring();
        bool incomplete = false;
        std::wstring version = L"{";
        constexpr auto versionKey = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
        bool first = true;
        for (const auto *name : {L"ProductName", L"DisplayVersion", L"CurrentBuildNumber", L"UBR", L"BuildLabEx", L"InstallationType"})
        {
            if (!first)
                version += L",";
            first = false;
            const auto value = ReadValue(HKEY_LOCAL_MACHINE, versionKey, name);
            if (value.error && value.error != ERROR_FILE_NOT_FOUND)
                incomplete = true;
            version += JsonQuote(name) + L":" + RegistryJson(value);
        }
        version += L"}";
        SYSTEM_POWER_STATUS power{};
        const bool powerKnown = GetSystemPowerStatus(&power) != FALSE;
        const auto sections = WmiSections({{L"Win32_OperatingSystem", {L"Caption", L"Version", L"BuildNumber", L"OSArchitecture", L"LastBootUpTime", L"InstallDate", L"TotalVisibleMemorySize", L"FreePhysicalMemory"}},
                                           {L"Win32_ComputerSystem", {L"Name", L"Manufacturer", L"Model", L"SystemType", L"TotalPhysicalMemory", L"HypervisorPresent"}},
                                           {L"Win32_BIOS", {L"Manufacturer", L"SMBIOSBIOSVersion", L"ReleaseDate"}},
                                           {L"Win32_BaseBoard", {L"Manufacturer", L"Product", L"Version"}},
                                           {L"Win32_Processor", {L"Name", L"Manufacturer", L"NumberOfCores", L"NumberOfLogicalProcessors", L"MaxClockSpeed"}},
                                           {L"Win32_PhysicalMemory", {L"DeviceLocator", L"Manufacturer", L"PartNumber", L"Capacity", L"Speed", L"ConfiguredClockSpeed"}},
                                           {L"Win32_VideoController", {L"Name", L"PNPDeviceID", L"DriverVersion", L"DriverDate", L"Status", L"ConfigManagerErrorCode"}},
                                           {L"Win32_DiskDrive", {L"Model", L"FirmwareRevision", L"InterfaceType", L"Size", L"Status", L"PNPDeviceID"}},
                                           {L"Win32_LogicalDisk", {L"DeviceID", L"DriveType", L"FileSystem", L"Size", L"FreeSpace"}},
                                           {L"Win32_QuickFixEngineering", {L"HotFixID", L"Description", L"InstalledOn"}}},
                                          context, incomplete);
        item.state = incomplete ? CollectionState::Partial : CollectionState::Saved;
        if (incomplete)
        {
            item.error = ERROR_PARTIAL_COPY;
            item.message = L"部分系统信息未能读取，逐项状态见 HTML 报告。";
        }
        SaveDiagnosticReport(item, context, L"{\"collectedAtUtc\":" + JsonQuote(DiagnosticUtc(context.now)) +
                                     L",\"windowsVersion\":" + version + L",\"powerStatus\":{\"available\":" + (powerKnown ? L"true" : L"false") +
                                     L",\"acLineStatus\":" + std::to_wstring(power.ACLineStatus) + L",\"batteryFlag\":" + std::to_wstring(power.BatteryFlag) +
                                     L",\"batteryLifePercent\":" + std::to_wstring(power.BatteryLifePercent) +
                                     L"},\"note\":\"这是采集时的快照，不能还原故障前的温度、电压或设备状态。WMI 时间保留 DMTF 格式，64 位数值可能为字符串。"
                                     L"QuickFixEngineering 仅列出该接口提供的更新，不代表全部更新历史。磁盘 Status 不能替代完整的 SMART/NVMe 检测。\",\"sections\":" +
                                     sections + L"}\n");
    }
    void CollectDrivers(CollectedLog &item, const DiagnosticContext &context)
    {
        item.path = (context.output / L"Drivers.html").wstring();
        bool incomplete = false;
        const auto sections = WmiSections({{L"Win32_PnPSignedDriver", {L"DeviceID", L"DeviceName", L"DeviceClass", L"Manufacturer", L"DriverProviderName", L"DriverVersion", L"DriverDate", L"InfName", L"IsSigned", L"Signer"}},
                                           {L"Win32_SystemDriver", {L"Name", L"DisplayName", L"PathName", L"State", L"Started", L"StartMode", L"ServiceType", L"Status"}}},
                                          context, incomplete);
        item.state = incomplete ? CollectionState::Partial : CollectionState::Saved;
        if (incomplete)
        {
            item.error = ERROR_PARTIAL_COPY;
            item.message = L"部分驱动信息未能读取，逐项状态见 HTML 报告。";
        }
        SaveDiagnosticReport(item, context, L"{\"collectedAtUtc\":" + JsonQuote(DiagnosticUtc(context.now)) +
                                     L",\"note\":\"包含设备关联的签名驱动信息及系统驱动服务状态；不等同于驱动仓库中全部未使用的驱动包。"
                                     L"DriverDate 是驱动包日期，不代表实际安装时间；签名字段来自系统清单，未执行独立签名验证。\",\"sections\":" +
                                     sections + L"}\n");
    }
    void CollectReliability(CollectedLog &item, const DiagnosticContext &context)
    {
        item.path = (context.output / L"Reliability.html").wstring();
        const auto start = context.now - std::min(context.now, static_cast<uint64_t>(context.options.days) * 86400ULL * 10000000);
        auto dmtf = [](uint64_t ticks)
        {
            FILETIME time{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)};
            SYSTEMTIME value{};
            if (!FileTimeToSystemTime(&time, &value))
                throw Error(GetLastError(), L"转换可靠性查询时间");
            wchar_t text[32]{};
            swprintf_s(text, L"%04u%02u%02u%02u%02u%02u.%03u000+000", value.wYear, value.wMonth, value.wDay,
                       value.wHour, value.wMinute, value.wSecond, value.wMilliseconds);
            return std::wstring(text);
        };
        std::wstring section;
        try
        {
            bool incomplete = false;
            QueryStats stats;
            WmiSession session;
            section = session.Query({L"Win32_ReliabilityRecords", {L"TimeGenerated", L"SourceName", L"ProductName", L"EventIdentifier",
                L"Message", L"RecordNumber", L"Logfile", L"ComputerName", L"User", L"InsertionStrings"},
                L"TimeGenerated >= '" + dmtf(start) + L"' AND TimeGenerated <= '" + dmtf(context.now) + L"'"}, context, incomplete, &stats);
            item.state = incomplete ? CollectionState::Partial : stats.unsupported ? CollectionState::Skipped :
                stats.rows ? CollectionState::Saved : CollectionState::NotFound;
            item.error = incomplete ? ERROR_PARTIAL_COPY : 0;
            item.message = incomplete ? L"可靠性历史未能完整读取，详细原因见报告。" : stats.unsupported ? L"系统未提供可靠性历史接口。" :
                stats.rows ? L"已保存 " + std::to_wstring(stats.rows) + L" 条可靠性记录。" : L"指定范围内没有可用的可靠性历史。";
        }
        catch (const Error &error)
        {
            if (error.code == ERROR_CANCELLED)
                throw;
            item.state = Unsupported(error.code) ? CollectionState::Skipped : CollectionState::Failed;
            item.error = error.code;
            item.message = error.text;
            section = L"{\"class\":\"Win32_ReliabilityRecords\",\"status\":" + JsonQuote(CollectionStateName(item.state)) +
                      L",\"errorCode\":" + std::to_wstring(error.code) + L",\"message\":" + JsonQuote(error.text) + L",\"rows\":[]}";
        }
        SaveDiagnosticReport(item, context, L"{\"windowStartUtc\":" + JsonQuote(DiagnosticUtc(start)) +
            L",\"windowEndUtc\":" + JsonQuote(DiagnosticUtc(context.now)) + L",\"sections\":[" + section +
            L"],\"note\":\"可靠性历史取决于 Windows 已保留的数据及可靠性提供程序配置；TimeGenerated 保留 WMI DMTF 时间与时区。\"}");
    }

    void CollectStorageHealth(CollectedLog &item, const DiagnosticContext &context)
    {
        item.path = (context.output / L"StorageHealth.html").wstring();
        std::wstring sections;
        try
        {
            WmiSession session(L"ROOT\\Microsoft\\Windows\\Storage");
            bool incomplete = false;
            QueryStats stats;
            std::wstring counters = L"[";
            size_t count = 0;
            // The method requires the full provider object (including ObjectId), not a projected inventory row.
            const auto disks = session.Query({L"MSFT_PhysicalDisk", {L"DeviceId", L"FriendlyName", L"SerialNumber", L"FirmwareVersion",
                L"MediaType", L"BusType", L"HealthStatus", L"OperationalStatus", L"Size"}, {}, true}, context, incomplete, &stats,
                [&](IWbemClassObject *disk)
                {
                    if (count++)
                        counters += L",";
                    counters += session.StorageCounter(disk, context, incomplete);
                });
            item.state = incomplete ? CollectionState::Partial : stats.unsupported ? CollectionState::Skipped :
                stats.rows ? CollectionState::Saved : CollectionState::NotFound;
            item.error = incomplete ? ERROR_PARTIAL_COPY : 0;
            item.message = incomplete ? L"部分磁盘或可靠性计数器无法读取，逐盘结果见报告。" :
                stats.unsupported ? L"系统未提供存储健康接口。" : !stats.rows ? L"存储提供程序未返回物理磁盘。" : L"已保存磁盘状态及可用的可靠性计数器。";
            sections = disks + L",{\"class\":\"MSFT_StorageReliabilityCounter\",\"status\":" +
                JsonQuote(CollectionStateName(item.state)) + L",\"rows\":" + counters + L"]}";
        }
        catch (const Error &error)
        {
            if (error.code == ERROR_CANCELLED)
                throw;
            item.state = Unsupported(error.code) ? CollectionState::Skipped : CollectionState::Failed;
            item.error = error.code;
            item.message = error.text;
            sections = L"{\"class\":\"MSFT_PhysicalDisk\",\"status\":" + JsonQuote(CollectionStateName(item.state)) +
                L",\"errorCode\":" + std::to_wstring(error.code) + L",\"message\":" + JsonQuote(error.text) + L",\"rows\":[]}";
        }
        SaveDiagnosticReport(item, context, L"{\"collectedAtUtc\":" + JsonQuote(DiagnosticUtc(context.now)) +
            L",\"sections\":[" + sections + L"],\"note\":\"采集时的存储健康快照；温度、磨损及错误计数由硬件和驱动提供，不可用字段保留 null。"
            L"TemperatureMax 是正常工作温度上限，不是历史最高温；Wear 为已消耗寿命百分比。此接口不保证提供全部 SMART/NVMe 属性。\"}");
    }

    void CollectDevices(CollectedLog &item, const DiagnosticContext &context)
    {
        item.path = (context.output / L"Devices.html").wstring();
        HDEVINFO devices = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, DIGCF_ALLCLASSES);
        if (devices == INVALID_HANDLE_VALUE)
            throw Error(GetLastError(), L"枚举设备");
        struct DeviceCloser
        {
            HDEVINFO value;
            ~DeviceCloser() { SetupDiDestroyDeviceInfoList(value); }
        } closer{devices};
        std::wstring json = L"{\"collectedAtUtc\":" + JsonQuote(DiagnosticUtc(context.now)) +
                            L",\"note\":\"包含当前及保留的非在场设备。没有问题代码不代表设备在故障时正常。\",\"devices\":[";
        bool first = true;
        size_t issues = 0;
        for (DWORD index = 0;; ++index)
        {
            CheckDiagnosticCancel(context);
            SP_DEVINFO_DATA device{};
            device.cbSize = sizeof(device);
            if (!SetupDiEnumDeviceInfo(devices, index, &device))
            {
                const auto code = GetLastError();
                if (code != ERROR_NO_MORE_ITEMS)
                {
                    item.error = code;
                    ++issues;
                }
                break;
            }
            if (index >= 20000)
            {
                item.error = ERROR_MORE_DATA;
                ++issues;
                break;
            }
            wchar_t id[MAX_DEVICE_ID_LEN]{};
            const bool idKnown = SetupDiGetDeviceInstanceIdW(devices, &device, id, static_cast<DWORD>(std::size(id)), nullptr) != FALSE;
            ULONG status = 0, problem = 0;
            const auto code = CM_Get_DevNode_Status(&status, &problem, device.DevInst, 0);
            if (!idKnown || (code != CR_SUCCESS && code != CR_NO_SUCH_DEVNODE))
                ++issues;
            if (!first)
                json += L",";
            first = false;
            json += L"\n{\"instanceId\":" + (idKnown ? JsonQuote(id) : L"null") +
                    L",\"name\":" + DeviceProperty(devices, device, SPDRP_FRIENDLYNAME) +
                    L",\"description\":" + DeviceProperty(devices, device, SPDRP_DEVICEDESC) +
                    L",\"class\":" + DeviceProperty(devices, device, SPDRP_CLASS) +
                    L",\"manufacturer\":" + DeviceProperty(devices, device, SPDRP_MFG) +
                    L",\"service\":" + DeviceProperty(devices, device, SPDRP_SERVICE) +
                    L",\"hardwareIds\":" + DeviceProperty(devices, device, SPDRP_HARDWAREID, true) +
                    L",\"statusAvailable\":" + (code == CR_SUCCESS ? L"true" : L"false") +
                    L",\"configRet\":" + std::to_wstring(code) +
                    L",\"devNodeStatus\":" + (code == CR_SUCCESS ? std::to_wstring(status) : L"null") +
                    L",\"problemCode\":" + (code == CR_SUCCESS ? std::to_wstring(problem) : L"null") +
                    L",\"driver\":" + DriverProperties(devices, device) + L"}";
        }
        item.state = issues ? CollectionState::Partial : CollectionState::Saved;
        if (issues)
        {
            if (!item.error)
                item.error = ERROR_PARTIAL_COPY;
            item.message = L"部分设备状态无法读取，见 configRet 与 statusAvailable。";
        }
        SaveDiagnosticReport(item, context, json + L"\n]}\n");
    }
    void CollectDumpConfiguration(CollectedLog &item, const DiagnosticContext &context)
    {
        item.path = (context.output / L"DumpConfiguration.html").wstring();
        bool incomplete = false;
        std::wstring json = L"{\"collectedAtUtc\":" + JsonQuote(DiagnosticUtc(context.now)) + L",\"registry\":[";
        const std::vector<std::pair<const wchar_t *, std::vector<const wchar_t *>>> keys{
            {L"SYSTEM\\CurrentControlSet\\Control\\CrashControl", {L"CrashDumpEnabled", L"DumpFile", L"MinidumpDir", L"DedicatedDumpFile", L"DumpFileSize", L"AutoReboot", L"LogEvent", L"Overwrite", L"AlwaysKeepMemoryDump", L"FilterPages"}},
            {L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management", {L"PagingFiles", L"ExistingPageFiles", L"TempPageFile"}}};
        bool first = true;
        for (const auto &[key, names] : keys)
            for (const auto *name : names)
            {
                const auto value = ReadValue(HKEY_LOCAL_MACHINE, key, name);
                if (!first)
                    json += L",";
                first = false;
                const bool missing = value.error == ERROR_FILE_NOT_FOUND || value.error == ERROR_PATH_NOT_FOUND;
                if (value.error && !missing)
                    incomplete = true;
                json += L"{\"key\":" + JsonQuote(key) + L",\"name\":" + JsonQuote(name) +
                        L",\"status\":" + JsonQuote(!value.error ? L"saved" : missing ? L"not_found"
                                                                                      : L"failed") +
                        L",\"registryType\":" + std::to_wstring(value.type) + L",\"errorCode\":" + std::to_wstring(value.error) +
                        L",\"value\":" + RegistryJson(value) + L"}";
            }
        json += L"],\"sections\":" + WmiSections({{L"Win32_OSRecoveryConfiguration", {L"DebugInfoType", L"DebugFilePath", L"MiniDumpDirectory", L"AutoReboot", L"WriteToSystemLog", L"OverwriteExistingDebugFile"}},
                                                  {L"Win32_PageFileSetting", {L"Name", L"InitialSize", L"MaximumSize"}},
                                                  {L"Win32_PageFileUsage", {L"Name", L"AllocatedBaseSize", L"CurrentUsage", L"PeakUsage", L"TempPageFile"}},
                                                  {L"Win32_ComputerSystem", {L"AutomaticManagedPagefile"}}},
                                                 context, incomplete);
        const auto windowsRoot = context.windows.root_path();
        ULARGE_INTEGER available{}, total{}, free{};
        const bool diskKnown = GetDiskFreeSpaceExW(windowsRoot.c_str(), &available, &total, &free) != FALSE;
        json += L",\"windowsVolume\":{\"path\":" + JsonQuote(windowsRoot.wstring()) + L",\"available\":" + (diskKnown ? L"true" : L"false") +
                L",\"freeBytes\":" + (diskKnown ? std::to_wstring(free.QuadPart) : L"null") +
                L"},\"note\":\"只读取配置。未找到转储不能排除蓝屏，应结合 volmgr、分页文件、实际转储所在卷空间与故障事件判断。"
                L"注册表值缺失不表示功能禁用，可能采用 Windows 默认值。\"}\n";
        item.state = incomplete ? CollectionState::Partial : CollectionState::Saved;
        if (incomplete)
        {
            item.error = ERROR_PARTIAL_COPY;
            item.message = L"部分配置或分页文件状态无法读取。";
        }
        SaveDiagnosticReport(item, context, json);
    }
}
