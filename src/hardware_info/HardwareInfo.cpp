#include "HardwareInfo.h"

#include <Windows.h>
#include <Wbemidl.h>
#include <setupapi.h>
#include <oleauto.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ftc::hardware_info
{
namespace
{

using Microsoft::WRL::ComPtr;
using WmiRow = std::vector<std::wstring>;
using WmiRows = std::vector<WmiRow>;

constexpr wchar_t kUnavailable[] = L"N/A";
constexpr wchar_t kCimv2Namespace[] = L"ROOT\\CIMV2";
constexpr wchar_t kWmiNamespace[] = L"ROOT\\WMI";
constexpr wchar_t kTpmNamespace[] = L"ROOT\\CIMV2\\Security\\MicrosoftTpm";
constexpr wchar_t kWindowsVersionRegistryPath[] =
    L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";

class ComApartment final
{
public:
    ComApartment()
    {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (result == S_OK || result == S_FALSE)
        {
            initialized_ = true;
            ownsInitialization_ = true;
            return;
        }

        // 如果调用线程已经采用另一种单元模型初始化，仍可使用现有单元。
        initialized_ = result == RPC_E_CHANGED_MODE;
        failure_ = result;
    }

    ~ComApartment()
    {
        if (ownsInitialization_)
        {
            CoUninitialize();
        }
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] bool IsInitialized() const noexcept
    {
        return initialized_;
    }

    [[nodiscard]] HRESULT Failure() const noexcept
    {
        return failure_;
    }

private:
    bool initialized_ = false;
    bool ownsInitialization_ = false;
    HRESULT failure_ = E_FAIL;
};

class ScopedBstr final
{
public:
    explicit ScopedBstr(const wchar_t* const value)
        : value_(SysAllocString(value))
    {
    }

    ~ScopedBstr()
    {
        SysFreeString(value_);
    }

    ScopedBstr(const ScopedBstr&) = delete;
    ScopedBstr& operator=(const ScopedBstr&) = delete;

    [[nodiscard]] BSTR Get() const noexcept
    {
        return value_;
    }

private:
    BSTR value_ = nullptr;
};

class ScopedVariant final
{
public:
    ScopedVariant()
    {
        VariantInit(&value_);
    }

    ~ScopedVariant()
    {
        VariantClear(&value_);
    }

    ScopedVariant(const ScopedVariant&) = delete;
    ScopedVariant& operator=(const ScopedVariant&) = delete;

    [[nodiscard]] VARIANT* Get() noexcept
    {
        return &value_;
    }

    [[nodiscard]] const VARIANT& Value() const noexcept
    {
        return value_;
    }

private:
    VARIANT value_{};
};

std::wstring Trim(std::wstring value)
{
    const auto isNotWhitespace = [](const wchar_t character)
    {
        return std::iswspace(static_cast<wint_t>(character)) == 0;
    };

    const auto first = std::find_if(value.begin(), value.end(), isNotWhitespace);
    if (first == value.end())
    {
        return {};
    }
    value.erase(value.begin(), first);

    const auto last = std::find_if(value.rbegin(), value.rend(), isNotWhitespace).base();
    value.erase(last, value.end());
    return value;
}

bool HasText(const std::wstring& value)
{
    const std::wstring trimmed = Trim(value);
    return !trimmed.empty() && _wcsicmp(trimmed.c_str(), kUnavailable) != 0;
}

std::wstring DisplayValue(std::wstring value)
{
    value = Trim(std::move(value));
    return HasText(value) ? value : kUnavailable;
}

std::wstring HresultText(const HRESULT value)
{
    wchar_t buffer[16]{};
    swprintf_s(buffer, L"0x%08X", static_cast<unsigned int>(value));
    return buffer;
}

std::wstring VariantToText(const VARIANT& value)
{
    if (value.vt == VT_EMPTY || value.vt == VT_NULL)
    {
        return {};
    }
    if (value.vt == VT_BSTR)
    {
        return value.bstrVal == nullptr ? std::wstring{} : Trim(value.bstrVal);
    }
    if (value.vt == VT_BOOL)
    {
        return value.boolVal == VARIANT_TRUE ? L"True" : L"False";
    }
    if ((value.vt & VT_ARRAY) != 0 && value.parray != nullptr)
    {
        const VARTYPE elementType = value.vt & VT_TYPEMASK;
        if (elementType == VT_UI2 || elementType == VT_UI1)
        {
            LONG lowerBound = 0;
            LONG upperBound = -1;
            if (FAILED(SafeArrayGetLBound(value.parray, 1, &lowerBound)) ||
                FAILED(SafeArrayGetUBound(value.parray, 1, &upperBound)))
            {
                return {};
            }

            std::wstring text;
            for (LONG index = lowerBound; index <= upperBound; ++index)
            {
                wchar_t character = L'\0';
                if (elementType == VT_UI2)
                {
                    USHORT value16 = 0;
                    if (FAILED(SafeArrayGetElement(value.parray, &index, &value16)))
                    {
                        return {};
                    }
                    character = static_cast<wchar_t>(value16);
                }
                else
                {
                    BYTE value8 = 0;
                    if (FAILED(SafeArrayGetElement(value.parray, &index, &value8)))
                    {
                        return {};
                    }
                    character = static_cast<wchar_t>(value8);
                }
                if (character == L'\0')
                {
                    break;
                }
                text.push_back(character);
            }
            return Trim(std::move(text));
        }
    }

    ScopedVariant converted;
    const HRESULT result = VariantChangeType(converted.Get(), &value, 0, VT_BSTR);
    if (FAILED(result) || converted.Value().bstrVal == nullptr)
    {
        return {};
    }
    return Trim(converted.Value().bstrVal);
}

class WmiConnection final
{
public:
    WmiConnection(std::stop_token stopToken, std::vector<std::wstring>& diagnostics) noexcept
        : stopToken_(stopToken), diagnostics_(diagnostics)
    {
    }

    bool Connect(const wchar_t* const namespacePath, std::wstring& error)
    {
        HRESULT result = CoInitializeSecurity(
            nullptr,
            -1,
            nullptr,
            nullptr,
            RPC_C_AUTHN_LEVEL_DEFAULT,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE,
            nullptr);
        if (FAILED(result) && result != RPC_E_TOO_LATE)
        {
            error = L"初始化 WMI 安全设置失败：" + HresultText(result);
            return false;
        }

        result = CoCreateInstance(
            CLSID_WbemLocator,
            nullptr,
            CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(locator_.ReleaseAndGetAddressOf()));
        if (FAILED(result) || locator_ == nullptr)
        {
            error = L"创建 WMI 定位器失败：" + HresultText(result);
            return false;
        }

        const ScopedBstr wmiNamespace(namespacePath);
        if (wmiNamespace.Get() == nullptr)
        {
            error = L"创建 WMI 命名空间字符串失败。";
            return false;
        }

        result = locator_->ConnectServer(
            wmiNamespace.Get(),
            nullptr,
            nullptr,
            nullptr,
            0,
            nullptr,
            nullptr,
            services_.ReleaseAndGetAddressOf());
        if (FAILED(result) || services_ == nullptr)
        {
            error = L"连接 WMI ";
            error += namespacePath == nullptr ? L"<null>" : namespacePath;
            error += L" 失败：" + HresultText(result);
            return false;
        }

        result = CoSetProxyBlanket(
            services_.Get(),
            RPC_C_AUTHN_WINNT,
            RPC_C_AUTHZ_NONE,
            nullptr,
            RPC_C_AUTHN_LEVEL_CALL,
            RPC_C_IMP_LEVEL_IMPERSONATE,
            nullptr,
            EOAC_NONE);
        if (FAILED(result))
        {
            error = L"设置 WMI 代理安全参数失败：" + HresultText(result);
            services_.Reset();
            return false;
        }
        return true;
    }

    [[nodiscard]] WmiRows Query(
        const wchar_t* const wql,
        const std::vector<const wchar_t*>& properties) const
    {
        WmiRows rows;
        if (services_ == nullptr || wql == nullptr)
        {
            return rows;
        }

        const ScopedBstr language(L"WQL");
        const ScopedBstr query(wql);
        if (language.Get() == nullptr || query.Get() == nullptr)
        {
            return rows;
        }

        ComPtr<IEnumWbemClassObject> enumerator;
        const HRESULT queryResult = services_->ExecQuery(
            language.Get(),
            query.Get(),
            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
            nullptr,
            enumerator.ReleaseAndGetAddressOf());
        if (FAILED(queryResult) || enumerator == nullptr)
        {
            ReportQueryFailure(L"ExecQuery", wql, queryResult);
            return rows;
        }

        constexpr ULONG pollMilliseconds = 250U;
        for (;;)
        {
            if (stopToken_.stop_requested())
            {
                return rows;
            }

            ComPtr<IWbemClassObject> object;
            ULONG returned = 0;
            const HRESULT nextResult = enumerator->Next(
                pollMilliseconds,
                1,
                object.ReleaseAndGetAddressOf(),
                &returned);
            if (nextResult == WBEM_S_TIMEDOUT)
            {
                continue;
            }
            if (FAILED(nextResult))
            {
                ReportQueryFailure(L"IEnumWbemClassObject::Next", wql, nextResult);
                return rows;
            }
            if (nextResult == WBEM_S_FALSE || returned == 0)
            {
                break;
            }
            if (object == nullptr)
            {
                ReportQueryFailure(L"IEnumWbemClassObject::Next", wql, nextResult);
                return rows;
            }

            WmiRow row;
            row.reserve(properties.size());
            for (const wchar_t* const property : properties)
            {
                ScopedVariant value;
                const HRESULT getResult = object->Get(property, 0, value.Get(), nullptr, nullptr);
                row.push_back(SUCCEEDED(getResult) ? VariantToText(value.Value()) : std::wstring{});
            }
            rows.push_back(std::move(row));
        }
        return rows;
    }

private:
    void ReportQueryFailure(
        const wchar_t* const stage,
        const wchar_t* const wql,
        const HRESULT result) const
    {
        std::wstring message = L"WMI ";
        message += stage;
        message += L" 失败 ";
        message += HresultText(result);
        if (wql != nullptr)
        {
            message += L"：";
            message += wql;
        }
        diagnostics_.push_back(std::move(message));
    }

    std::stop_token stopToken_;
    std::vector<std::wstring>& diagnostics_;
    ComPtr<IWbemLocator> locator_;
    ComPtr<IWbemServices> services_;
};

std::wstring FirstValue(
    const WmiConnection& wmi,
    const wchar_t* const query,
    const wchar_t* const property)
{
    const WmiRows rows = wmi.Query(query, {property});
    for (const WmiRow& row : rows)
    {
        if (!row.empty() && HasText(row[0]))
        {
            return Trim(row[0]);
        }
    }
    return {};
}

void AddUnique(std::vector<std::wstring>& values, std::wstring value)
{
    value = Trim(std::move(value));
    if (!HasText(value))
    {
        return;
    }
    const auto found = std::find_if(
        values.begin(),
        values.end(),
        [&value](const std::wstring& current)
        {
            return _wcsicmp(current.c_str(), value.c_str()) == 0;
        });
    if (found == values.end())
    {
        values.push_back(std::move(value));
    }
}

std::wstring Join(const std::vector<std::wstring>& values, const std::wstring_view separator)
{
    std::wstring result;
    for (const std::wstring& value : values)
    {
        if (!result.empty())
        {
            result.append(separator);
        }
        result.append(value);
    }
    return result;
}

std::wstring JoinColumn(
    const WmiConnection& wmi,
    const wchar_t* const query,
    const wchar_t* const property)
{
    const WmiRows rows = wmi.Query(query, {property});
    std::vector<std::wstring> values;
    for (const WmiRow& row : rows)
    {
        if (!row.empty())
        {
            AddUnique(values, row[0]);
        }
    }
    return Join(values, L"\r\n");
}

std::wstring ToLower(std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towlower(static_cast<wint_t>(character)));
        });
    return value;
}

std::wstring ToUpper(std::wstring value)
{
    std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](const wchar_t character) {
            return static_cast<wchar_t>(std::towupper(static_cast<wint_t>(character)));
        });
    return value;
}

[[nodiscard]] bool EqualsIgnoreCase(
    const std::wstring_view left,
    const std::wstring_view right) noexcept
{
    if (left.size() > static_cast<std::size_t>(INT_MAX) ||
        right.size() > static_cast<std::size_t>(INT_MAX))
    {
        return false;
    }
    return CompareStringOrdinal(
               left.data(),
               static_cast<int>(left.size()),
               right.data(),
               static_cast<int>(right.size()),
               TRUE) == CSTR_EQUAL;
}

std::size_t ReplaceAll(
    std::wstring& value,
    const std::wstring_view from,
    const std::wstring_view to)
{
    if (from.empty())
    {
        return 0U;
    }

    std::size_t count = 0U;
    std::size_t position = 0U;
    while ((position = value.find(from, position)) != std::wstring::npos)
    {
        value.replace(position, from.size(), to);
        position += to.size();
        ++count;
    }
    return count;
}

[[nodiscard]] bool IsVirtualDisplayAdapter(
    const std::wstring_view name,
    const std::wstring_view instanceId)
{
    const std::wstring loweredName = ToLower(std::wstring(name));
    const std::wstring loweredId = ToLower(std::wstring(instanceId));
    return loweredName.find(L"idd") != std::wstring::npos ||
        loweredName.find(L"oray") != std::wstring::npos ||
        loweredName.find(L"virtual") != std::wstring::npos ||
        loweredName.find(L"remote") != std::wstring::npos ||
        loweredId.rfind(L"root\\", 0U) == 0U ||
        loweredId.rfind(L"swd\\", 0U) == 0U;
}

std::wstring ReadSetupDeviceProperty(
    const HDEVINFO deviceInformation,
    SP_DEVINFO_DATA& deviceData,
    const DWORD property)
{
    DWORD type = 0;
    DWORD requiredBytes = 0;
    static_cast<void>(SetupDiGetDeviceRegistryPropertyW(
        deviceInformation,
        &deviceData,
        property,
        &type,
        nullptr,
        0,
        &requiredBytes));
    if (requiredBytes < sizeof(wchar_t))
    {
        return {};
    }

    std::vector<BYTE> buffer(requiredBytes + sizeof(wchar_t), 0U);
    if (SetupDiGetDeviceRegistryPropertyW(
            deviceInformation,
            &deviceData,
            property,
            &type,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr) == FALSE ||
        (type != REG_SZ && type != REG_EXPAND_SZ && type != REG_MULTI_SZ))
    {
        return {};
    }
    return Trim(reinterpret_cast<const wchar_t*>(buffer.data()));
}

std::wstring ReadSetupDeviceInstanceId(
    const HDEVINFO deviceInformation,
    SP_DEVINFO_DATA& deviceData)
{
    DWORD requiredCharacters = 0;
    static_cast<void>(SetupDiGetDeviceInstanceIdW(
        deviceInformation,
        &deviceData,
        nullptr,
        0,
        &requiredCharacters));
    if (requiredCharacters == 0U)
    {
        return {};
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(requiredCharacters) + 1U,
        L'\0');
    if (SetupDiGetDeviceInstanceIdW(
            deviceInformation,
            &deviceData,
            buffer.data(),
            static_cast<DWORD>(buffer.size()),
            nullptr) == FALSE)
    {
        return {};
    }
    return Trim(buffer.data());
}

unsigned long long ParseUnsigned(const std::wstring& text);

std::wstring ExtractPciIdentifier(
    const std::wstring_view hardwareId,
    const std::wstring_view marker,
    const std::size_t length)
{
    const std::wstring upper = ToUpper(std::wstring(hardwareId));
    const std::wstring upperMarker = ToUpper(std::wstring(marker));
    const std::size_t markerPosition = upper.find(upperMarker);
    if (markerPosition == std::wstring::npos)
    {
        return {};
    }
    const std::size_t valuePosition = markerPosition + upperMarker.size();
    if (valuePosition > upper.size() || length > upper.size() - valuePosition)
    {
        return {};
    }

    const std::wstring value = upper.substr(valuePosition, length);
    const bool allHexadecimal = std::all_of(
        value.begin(),
        value.end(),
        [](const wchar_t character) {
            return std::iswxdigit(static_cast<wint_t>(character)) != 0;
        });
    return allHexadecimal ? value : std::wstring{};
}

std::wstring GpuChipVendor(const std::wstring_view vendorId)
{
    if (EqualsIgnoreCase(vendorId, L"10DE")) return L"NVIDIA";
    if (EqualsIgnoreCase(vendorId, L"1002")) return L"AMD";
    if (EqualsIgnoreCase(vendorId, L"8086")) return L"Intel";
    if (EqualsIgnoreCase(vendorId, L"1ED5")) return L"Moore Threads";
    if (EqualsIgnoreCase(vendorId, L"0731")) return L"JingjiaMicro";
    return {};
}

std::wstring GpuBoardVendor(const std::wstring_view subsystemVendorId)
{
    static constexpr std::pair<const wchar_t*, const wchar_t*> vendors[] = {
        {L"7377", L"Colorful"}, {L"1028", L"Dell"},
        {L"1480", L"XFX"}, {L"17AA", L"Lenovo"},
        {L"1569", L"Tul"}, {L"1043", L"ASUS"},
        {L"152D", L"Quanta"}, {L"1B4C", L"GALAX"},
        {L"1558", L"Clevo"}, {L"1462", L"MSI"},
        {L"1682", L"XFX"}, {L"1EAE", L"XFX"},
        {L"19DA", L"ZOTAC"}, {L"103C", L"HP"},
        {L"1849", L"ASRock"}, {L"174B", L"Sapphire"},
        {L"1ACC", L"POINT OF VIEW"}, {L"1565", L"Biostar"},
        {L"1179", L"Toshiba"}, {L"106B", L"Apple"},
        {L"1014", L"IBM"}, {L"1A58", L"Razer"},
        {L"1019", L"ECS"}, {L"1025", L"Acer"},
        {L"3842", L"EVGA"}, {L"10CF", L"Fujitsu"},
        {L"1787", L"ATI AIB"}, {L"1002", L"AMD"},
        {L"144D", L"Samsung"}, {L"17FF", L"BenQ"},
        {L"1458", L"Gigabyte"}, {L"10DE", L"NVIDIA"},
        {L"8086", L"Intel"}, {L"0000", L"Unknown"},
        {L"1DA2", L"Sapphire"}, {L"10B0", L"Gainward"},
        {L"148C", L"PowerColor"}, {L"7333", L"Leadtek"},
        {L"1B35", L"Great Wall"}, {L"1E7B", L"XFX"},
        {L"1D05", L"Tsinghua Tongfang"}, {L"1D72", L"Xiaomi"},
        {L"1B0A", L"Pegatron"}, {L"0731", L"JingjiaMicro"},
        {L"10FD", L"SOYO"}, {L"1EC9", L"Wingtech"},
        {L"19E5", L"Huawei"}, {L"196E", L"PNY"},
        {L"107D", L"Leadtek"}, {L"1ED3", L"Yeston"},
        {L"1EF7", L"ASRock"}, {L"1ED5", L"Moore Threads"},
    };
    const auto found = std::find_if(
        std::begin(vendors),
        std::end(vendors),
        [subsystemVendorId](const auto& candidate) {
            return EqualsIgnoreCase(subsystemVendorId, candidate.first);
        });
    return found == std::end(vendors) ? std::wstring{} : std::wstring(found->second);
}

std::wstring DetectGpuBoardVendor(const std::wstring_view adapterName)
{
    const std::wstring matcher = ToLower(std::wstring(adapterName));
    struct VendorName
    {
        const wchar_t* marker;
        const wchar_t* normalized;
    };
    static constexpr VendorName names[] = {
        {L"影驰", L"GALAX"}, {L"galax", L"GALAX"}, {L"kfa2", L"GALAX"},
        {L"华硕", L"ASUS"}, {L"asus", L"ASUS"},
        {L"微星", L"MSI"}, {L"msi", L"MSI"},
        {L"技嘉", L"Gigabyte"}, {L"gigabyte", L"Gigabyte"}, {L"aorus", L"Gigabyte"},
        {L"七彩虹", L"Colorful"}, {L"colorful", L"Colorful"},
        {L"索泰", L"ZOTAC"}, {L"zotac", L"ZOTAC"},
        {L"映众", L"Inno3D"}, {L"inno3d", L"Inno3D"},
        {L"耕升", L"Gainward"}, {L"gainward", L"Gainward"},
        {L"蓝宝石", L"Sapphire"}, {L"sapphire", L"Sapphire"},
        {L"讯景", L"XFX"}, {L"xfx", L"XFX"},
        {L"撼讯", L"PowerColor"}, {L"powercolor", L"PowerColor"},
        {L"盈通", L"Yeston"}, {L"yeston", L"Yeston"},
        {L"铭瑄", L"MAXSUN"}, {L"maxsun", L"MAXSUN"},
    };
    const auto found = std::find_if(
        std::begin(names),
        std::end(names),
        [&matcher](const VendorName& candidate) {
            return matcher.find(candidate.marker) != std::wstring::npos;
        });
    return found == std::end(names) ? std::wstring{} : std::wstring(found->normalized);
}

std::wstring NormalizeGpuChipVendor(const std::wstring_view adapterName)
{
    const std::wstring matcher = ToLower(std::wstring(adapterName));
    if (matcher.find(L"nvidia") != std::wstring::npos ||
        matcher.find(L"geforce") != std::wstring::npos ||
        matcher.find(L"quadro") != std::wstring::npos ||
        matcher.find(L"rtx") != std::wstring::npos)
    {
        return L"NVIDIA";
    }
    if (matcher.find(L"amd") != std::wstring::npos ||
        matcher.find(L"radeon") != std::wstring::npos ||
        matcher.find(L"advanced micro devices") != std::wstring::npos ||
        matcher.find(L"ati") != std::wstring::npos)
    {
        return L"AMD";
    }
    if (matcher.find(L"intel") != std::wstring::npos ||
        matcher.find(L"arc") != std::wstring::npos ||
        matcher.find(L"iris") != std::wstring::npos ||
        matcher.find(L"uhd") != std::wstring::npos)
    {
        return L"Intel";
    }
    return {};
}

std::wstring CleanGpuDeviceName(std::wstring name)
{
    name = Trim(std::move(name));
    static constexpr const wchar_t* suffixes[] = {
        L" (Microsoft Corporation - WDDM 1.1)",
        L" (Microsoft Corporation - WDDM 1.2)",
        L" (Microsoft Corporation - WDDM 1.3)",
        L" (Microsoft Corporation - WDDM 1.4)",
        L" (Microsoft Corporation - WDDM 1.5)",
        L" (Microsoft Corporation - WDDM 2.0)",
        L" (Microsoft Corporation - WDDM 2.1)",
        L" (Microsoft Corporation - WDDM 2.2)",
    };
    for (const wchar_t* const suffix : suffixes)
    {
        static_cast<void>(ReplaceAll(name, suffix, L""));
    }
    static_cast<void>(ReplaceAll(name, L"Chipset Family", L""));
    while (ReplaceAll(name, L"  ", L" ") > 0U)
    {
    }
    return Trim(std::move(name));
}

struct DisplayAdapterInfo
{
    std::wstring name;
    std::wstring instanceId;
    std::wstring chipVendor;
    std::wstring boardVendor;
};

std::vector<DisplayAdapterInfo> ReadDisplayAdapters()
{
    static const GUID displayClassGuid = {
        0x4d36e968,
        0xe325,
        0x11ce,
        {0xbf, 0xc1, 0x08, 0x00, 0x2b, 0xe1, 0x03, 0x18}};
    std::vector<DisplayAdapterInfo> adapters;
    const HDEVINFO deviceInformation = SetupDiGetClassDevsW(
        &displayClassGuid,
        nullptr,
        nullptr,
        DIGCF_PRESENT);
    if (deviceInformation == INVALID_HANDLE_VALUE)
    {
        return adapters;
    }

    for (DWORD index = 0U;; ++index)
    {
        SP_DEVINFO_DATA deviceData{};
        deviceData.cbSize = sizeof(deviceData);
        if (SetupDiEnumDeviceInfo(deviceInformation, index, &deviceData) == FALSE)
        {
            if (GetLastError() == ERROR_NO_MORE_ITEMS)
            {
                break;
            }
            continue;
        }

        DisplayAdapterInfo adapter;
        adapter.name = ReadSetupDeviceProperty(
            deviceInformation,
            deviceData,
            SPDRP_FRIENDLYNAME);
        if (!HasText(adapter.name))
        {
            adapter.name = ReadSetupDeviceProperty(
                deviceInformation,
                deviceData,
                SPDRP_DEVICEDESC);
        }
        adapter.name = CleanGpuDeviceName(std::move(adapter.name));
        adapter.instanceId = ReadSetupDeviceInstanceId(deviceInformation, deviceData);
        std::wstring hardwareId = ReadSetupDeviceProperty(
            deviceInformation,
            deviceData,
            SPDRP_HARDWAREID);
        if (!HasText(hardwareId))
        {
            hardwareId = adapter.instanceId;
        }
        if (!HasText(adapter.name) ||
            IsVirtualDisplayAdapter(adapter.name, adapter.instanceId))
        {
            continue;
        }

        const std::wstring vendorId = ExtractPciIdentifier(hardwareId, L"VEN_", 4U);
        const std::wstring subsystem = ExtractPciIdentifier(hardwareId, L"SUBSYS_", 8U);
        adapter.chipVendor = GpuChipVendor(vendorId);
        if (!HasText(adapter.chipVendor))
        {
            adapter.chipVendor = NormalizeGpuChipVendor(adapter.name);
        }
        if (subsystem.size() == 8U)
        {
            adapter.boardVendor = GpuBoardVendor(subsystem.substr(4U));
        }
        if (!HasText(adapter.boardVendor))
        {
            adapter.boardVendor = DetectGpuBoardVendor(adapter.name);
        }
        adapters.push_back(std::move(adapter));
    }
    static_cast<void>(SetupDiDestroyDeviceInfoList(deviceInformation));
    return adapters;
}

unsigned long long ResolveDisplayAdapterMemoryBytes(
    const std::wstring_view instanceId,
    const std::wstring_view adapterRam)
{
    const std::wstring deviceParametersPath =
        L"SYSTEM\\CurrentControlSet\\Enum\\" + std::wstring(instanceId) +
        L"\\Device Parameters";
    wchar_t videoId[128]{};
    DWORD videoIdBytes = sizeof(videoId);
    if (RegGetValueW(
            HKEY_LOCAL_MACHINE,
            deviceParametersPath.c_str(),
            L"VideoID",
            RRF_RT_REG_SZ,
            nullptr,
            videoId,
            &videoIdBytes) == ERROR_SUCCESS &&
        videoId[0] != L'\0')
    {
        const std::wstring directXPath =
            L"SOFTWARE\\Microsoft\\DirectX\\" + std::wstring(videoId);
        ULONGLONG dedicatedVideoMemory = 0U;
        DWORD valueType = 0U;
        DWORD valueBytes = sizeof(dedicatedVideoMemory);
        LSTATUS status = RegGetValueW(
            HKEY_LOCAL_MACHINE,
            directXPath.c_str(),
            L"DedicatedVideoMemory",
            RRF_RT_REG_QWORD | RRF_RT_REG_DWORD | RRF_SUBKEY_WOW6464KEY,
            &valueType,
            &dedicatedVideoMemory,
            &valueBytes);
        if (status != ERROR_SUCCESS)
        {
            dedicatedVideoMemory = 0U;
            valueType = 0U;
            valueBytes = sizeof(dedicatedVideoMemory);
            status = RegGetValueW(
                HKEY_LOCAL_MACHINE,
                directXPath.c_str(),
                L"DedicatedVideoMemory",
                RRF_RT_REG_QWORD | RRF_RT_REG_DWORD,
                &valueType,
                &dedicatedVideoMemory,
                &valueBytes);
        }
        if (status == ERROR_SUCCESS && dedicatedVideoMemory > 0U)
        {
            return static_cast<unsigned long long>(dedicatedVideoMemory);
        }
    }

    ULONGLONG qwordValue = 0U;
    DWORD valueBytes = sizeof(qwordValue);
    LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        deviceParametersPath.c_str(),
        L"HardwareInformation.qwMemorySize",
        RRF_RT_REG_QWORD,
        nullptr,
        &qwordValue,
        &valueBytes);
    if (status == ERROR_SUCCESS && qwordValue > 0U)
    {
        return static_cast<unsigned long long>(qwordValue);
    }

    DWORD dwordValue = 0U;
    valueBytes = sizeof(dwordValue);
    status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        deviceParametersPath.c_str(),
        L"HardwareInformation.MemorySize",
        RRF_RT_REG_DWORD,
        nullptr,
        &dwordValue,
        &valueBytes);
    if (status == ERROR_SUCCESS && dwordValue > 0U)
    {
        return static_cast<unsigned long long>(dwordValue);
    }

    const unsigned long long bytes = ParseUnsigned(std::wstring(adapterRam));
    constexpr unsigned long long minimumReasonable = 32ULL * 1024ULL * 1024ULL;
    constexpr unsigned long long maximumReasonable =
        128ULL * 1024ULL * 1024ULL * 1024ULL;
    return bytes >= minimumReasonable && bytes <= maximumReasonable ? bytes : 0U;
}

std::wstring FormatGpuMemory(const unsigned long long bytes)
{
    if (bytes == 0U)
    {
        return {};
    }
    constexpr unsigned long long megabyte = 1024ULL * 1024ULL;
    constexpr unsigned long long gigabyte = 1024ULL * 1024ULL * 1024ULL;
    return bytes >= gigabyte
        ? std::to_wstring((bytes + (gigabyte / 2U)) / gigabyte) + L"GB"
        : std::to_wstring((bytes + (megabyte / 2U)) / megabyte) + L"MB";
}

std::wstring CollectGpuInfo(const WmiConnection& wmi)
{
    const std::vector<DisplayAdapterInfo> adapters = ReadDisplayAdapters();
    if (!adapters.empty())
    {
        const WmiRows wmiRows = wmi.Query(
            L"SELECT PNPDeviceID, AdapterRAM FROM Win32_VideoController",
            {L"PNPDeviceID", L"AdapterRAM"});
        std::vector<std::wstring> models;
        for (const DisplayAdapterInfo& adapter : adapters)
        {
            std::wstring adapterRam;
            for (const WmiRow& row : wmiRows)
            {
                if (row.size() >= 2U && EqualsIgnoreCase(adapter.instanceId, row[0]))
                {
                    adapterRam = row[1];
                    break;
                }
            }

            const std::wstring memory = FormatGpuMemory(
                ResolveDisplayAdapterMemoryBytes(adapter.instanceId, adapterRam));
            const std::wstring brand = HasText(adapter.boardVendor)
                ? adapter.boardVendor
                : adapter.chipVendor;
            std::wstring model = adapter.name;
            if (HasText(memory) || HasText(brand))
            {
                model += L" (";
                if (HasText(memory))
                {
                    model += memory;
                }
                if (HasText(memory) && HasText(brand))
                {
                    model += L" / ";
                }
                if (HasText(brand))
                {
                    model += brand;
                }
                model += L")";
            }
            AddUnique(models, std::move(model));
        }
        if (!models.empty())
        {
            return Join(models, L" / ");
        }
    }

    const WmiRows rows = wmi.Query(
        L"SELECT Name, PNPDeviceID FROM Win32_VideoController",
        {L"Name", L"PNPDeviceID"});
    std::vector<std::wstring> names;
    for (const WmiRow& row : rows)
    {
        if (row.size() >= 2U && HasText(row[0]) &&
            !IsVirtualDisplayAdapter(row[0], row[1]))
        {
            AddUnique(names, row[0]);
        }
    }
    return Join(names, L"\r\n");
}

unsigned long long ParseUnsigned(const std::wstring& text)
{
    const std::wstring value = Trim(text);
    if (value.empty())
    {
        return 0;
    }
    wchar_t* end = nullptr;
    const unsigned long long parsed = wcstoull(value.c_str(), &end, 10);
    return end != nullptr && *end == L'\0' ? parsed : 0;
}

[[nodiscard]] bool IsGenericMonitorLabel(const std::wstring_view value)
{
    const std::wstring lowered = ToLower(Trim(std::wstring(value)));
    return lowered.empty() || lowered == L"generic pnp monitor" ||
        lowered == L"generic monitor" || lowered == L"default monitor" ||
        lowered == L"通用即插即用监视器" || lowered == L"默认监视器";
}

std::wstring NormalizeMonitorLabel(std::wstring value)
{
    value = Trim(std::move(value));
    if (!HasText(value))
    {
        return {};
    }

    const std::wstring lowered = ToLower(value);
    const std::size_t leftParenthesis = value.find(L'(');
    const std::size_t rightParenthesis = value.rfind(L')');
    if ((lowered.rfind(L"generic monitor", 0U) == 0U ||
         lowered.rfind(L"generic pnp monitor", 0U) == 0U ||
         lowered.rfind(L"通用", 0U) == 0U) &&
        leftParenthesis != std::wstring::npos &&
        rightParenthesis != std::wstring::npos &&
        rightParenthesis > leftParenthesis)
    {
        std::wstring inner = Trim(value.substr(
            leftParenthesis + 1U,
            rightParenthesis - leftParenthesis - 1U));
        if (HasText(inner) && !IsGenericMonitorLabel(inner))
        {
            return inner;
        }
    }
    return IsGenericMonitorLabel(value) ? std::wstring{} : value;
}

[[nodiscard]] bool IsLikelyMonitorIdToken(const std::wstring_view value)
{
    const std::wstring token = Trim(std::wstring(value));
    if (token.size() < 6U || token.size() > 12U)
    {
        return false;
    }
    if (token[0] < L'A' || token[0] > L'Z' ||
        token[1] < L'A' || token[1] > L'Z' ||
        token[2] < L'A' || token[2] > L'Z')
    {
        return false;
    }

    bool hasDigit = false;
    for (std::size_t index = 3U; index < token.size(); ++index)
    {
        const wchar_t character = token[index];
        if (character >= L'0' && character <= L'9')
        {
            hasDigit = true;
        }
        else if (std::iswalnum(static_cast<wint_t>(character)) == 0)
        {
            return false;
        }
    }
    return hasDigit;
}

std::wstring DecodeEdidDescriptorText(const BYTE* const descriptor)
{
    if (descriptor == nullptr)
    {
        return {};
    }

    std::wstring text;
    for (std::size_t index = 5U; index < 18U; ++index)
    {
        const BYTE character = descriptor[index];
        if (character == 0x0AU || character == 0x00U)
        {
            break;
        }
        text.push_back(static_cast<wchar_t>(character));
    }
    return Trim(std::move(text));
}

bool HasEdidBaseBlock(const std::vector<BYTE>& edid)
{
    static constexpr BYTE expectedHeader[] = {
        0x00U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x00U};
    return edid.size() >= 128U &&
        memcmp(edid.data(), expectedHeader, sizeof(expectedHeader)) == 0;
}

std::wstring MonitorNameFromEdid(const std::vector<BYTE>& edid)
{
    if (!HasEdidBaseBlock(edid))
    {
        return {};
    }

    for (std::size_t offset = 54U; offset <= 108U; offset += 18U)
    {
        if (edid[offset] != 0x00U || edid[offset + 1U] != 0x00U ||
            edid[offset + 2U] != 0x00U || edid[offset + 3U] != 0xFCU)
        {
            continue;
        }

        std::wstring name = DecodeEdidDescriptorText(edid.data() + offset);
        if (HasText(name) && !IsLikelyMonitorIdToken(name))
        {
            return name;
        }
    }
    return {};
}

struct MonitorDetails final
{
    std::wstring name;
    unsigned widthMillimeters{};
    unsigned heightMillimeters{};
};

bool ReadDetailedMonitorSize(const BYTE* const timing, MonitorDetails& details)
{
    // A zero pixel clock denotes a monitor descriptor, not a detailed timing.
    if (timing[0] == 0U && timing[1] == 0U) return false;
    const unsigned width = timing[12] | ((timing[14] & 0xF0U) << 4U);
    const unsigned height = timing[13] | ((timing[14] & 0x0FU) << 8U);
    if (width == 0U || height == 0U) return false;
    details.widthMillimeters = width;
    details.heightMillimeters = height;
    return true;
}

MonitorDetails MonitorDetailsFromEdid(const std::vector<BYTE>& edid)
{
    MonitorDetails details;
    if (!HasEdidBaseBlock(edid)) return details;
    details.name = MonitorNameFromEdid(edid);
    // Prefer the millimeter image size in the first usable detailed timing.
    // The base block's centimeter fields can be rounded or incorrect (3 x 2).
    for (std::size_t offset = 54U; offset <= 108U; offset += 18U)
    {
        if (ReadDetailedMonitorSize(edid.data() + offset, details)) return details;
    }
    const std::size_t blocks = (std::min)(
        static_cast<std::size_t>(edid[126]), edid.size() / 128U - 1U);
    for (std::size_t index = 1U; index <= blocks; ++index)
    {
        const BYTE* const block = edid.data() + index * 128U;
        // CTA extension DTDs stop before its checksum byte. Other extension
        // formats must not be interpreted as 18-byte timing descriptors.
        const std::size_t start = block[2];
        if (block[0] != 0x02U || start < 4U || start > 109U) continue;
        for (std::size_t offset = start; offset + 18U <= 127U; offset += 18U)
        {
            if (ReadDetailedMonitorSize(block + offset, details)) return details;
        }
    }
    // No usable detailed size: leave it unknown for the same-monitor WMI
    // fallback, rather than reusing the coarse base fields behind this issue.
    return details;
}

std::wstring FormatMonitorDetails(const MonitorDetails& details)
{
    std::wstring value = details.name;
    if (details.widthMillimeters != 0U && details.heightMillimeters != 0U)
    {
        const double width = static_cast<double>(details.widthMillimeters);
        const double height = static_cast<double>(details.heightMillimeters);
        const double diagonal = std::hypot(width, height) / 25.4;
        wchar_t size[96]{};
        swprintf_s(size, L"%.1f inch (%.1f x %.1f cm)", diagonal, width / 10.0, height / 10.0);
        if (HasText(value)) value += L" (" + std::wstring(size) + L")";
        else value = size;
    }
    return value;
}

std::wstring NormalizeDisplayRegistryInstancePath(std::wstring instanceName, bool fromWmi = false)
{
    instanceName = Trim(std::move(instanceName));
    // EnumDisplayDevices(EDD_GET_DEVICE_INTERFACE_NAME) supplies a monitor
    // interface path. Resolve it to the same instance used by ROOT\\WMI.
    if (instanceName.rfind(L"\\\\?\\", 0U) == 0U) instanceName.erase(0U, 4U);
    const std::size_t interfaceSuffix = instanceName.find(L"#{");
    if (interfaceSuffix != std::wstring::npos) instanceName.resize(interfaceSuffix);
    std::replace(instanceName.begin(), instanceName.end(), L'#', L'\\');
    static_cast<void>(ReplaceAll(instanceName, L"\\\\", L"\\"));
    const std::size_t suffix = instanceName.rfind(L'_');
    if (fromWmi && suffix != std::wstring::npos && suffix > 0U &&
        suffix + 1U < instanceName.size() &&
        std::all_of(instanceName.begin() + static_cast<std::ptrdiff_t>(suffix + 1U), instanceName.end(),
            [](wchar_t value) { return value >= L'0' && value <= L'9'; }))
    {
        instanceName.resize(suffix);
    }
    return instanceName;
}

bool ReadEdidFromRegistryInstancePath(
    const std::wstring_view instancePath,
    std::vector<BYTE>& edid,
    LSTATUS& error)
{
    edid.clear();
    const std::wstring parametersPath =
        L"SYSTEM\\CurrentControlSet\\Enum\\" + std::wstring(instancePath) +
        L"\\Device Parameters";

    DWORD valueType = 0U;
    DWORD byteCount = 0U;
    LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        parametersPath.c_str(),
        L"EDID",
        RRF_RT_REG_BINARY,
        &valueType,
        nullptr,
        &byteCount);
    if (status != ERROR_SUCCESS || valueType != REG_BINARY || byteCount < 128U)
    {
        error = status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status;
        return false;
    }

    edid.assign(byteCount, 0U);
    status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        parametersPath.c_str(),
        L"EDID",
        RRF_RT_REG_BINARY,
        &valueType,
        edid.data(),
        &byteCount);
    if (status != ERROR_SUCCESS || valueType != REG_BINARY || byteCount < 128U)
    {
        error = status == ERROR_SUCCESS ? ERROR_INVALID_DATA : status;
        edid.clear();
        return false;
    }
    edid.resize(byteCount);
    error = ERROR_SUCCESS;
    return true;
}

void AddActiveMonitorInstancePath(
    std::vector<std::wstring>& paths,
    std::wstring path,
    bool fromWmi = false)
{
    path = NormalizeDisplayRegistryInstancePath(std::move(path), fromWmi);
    if (!HasText(path) || _wcsnicmp(path.c_str(), L"DISPLAY\\", 8U) != 0)
    {
        return;
    }
    const auto duplicate = std::find_if(
        paths.begin(),
        paths.end(),
        [&path](const std::wstring& existing) {
            return EqualsIgnoreCase(existing, path);
        });
    if (duplicate == paths.end())
    {
        paths.push_back(std::move(path));
    }
}

std::vector<std::wstring> CollectActiveMonitorInstancePaths(
    const WmiRows& monitorRows, const WmiRows& sizeRows)
{
    std::vector<std::wstring> instancePaths;
    for (DWORD adapterIndex = 0U;; ++adapterIndex)
    {
        DISPLAY_DEVICEW adapter{};
        adapter.cb = sizeof(adapter);
        if (EnumDisplayDevicesW(nullptr, adapterIndex, &adapter, 0U) == FALSE)
        {
            break;
        }

        for (DWORD monitorIndex = 0U;; ++monitorIndex)
        {
            DISPLAY_DEVICEW monitor{};
            monitor.cb = sizeof(monitor);
            if (EnumDisplayDevicesW(adapter.DeviceName, monitorIndex, &monitor,
                    EDD_GET_DEVICE_INTERFACE_NAME) == FALSE)
            {
                break;
            }
            if ((monitor.StateFlags & DISPLAY_DEVICE_ACTIVE) != 0U)
            {
                AddActiveMonitorInstancePath(instancePaths, monitor.DeviceID);
            }
        }
    }

    // WMI identities supplement GDI enumeration; their values remain fallback
    // data and never replace an available EDID name or physical dimension.
    for (const WmiRow& row : monitorRows)
    {
        if (row.size() >= 3U && EqualsIgnoreCase(Trim(row[2]), L"True"))
        {
            AddActiveMonitorInstancePath(instancePaths, row[1], true);
        }
    }
    for (const WmiRow& row : sizeRows)
    {
        if (row.size() >= 4U && EqualsIgnoreCase(Trim(row[3]), L"True"))
        {
            AddActiveMonitorInstancePath(instancePaths, row[0], true);
        }
    }
    return instancePaths;
}

std::wstring ExtractMonitorCode(const std::wstring_view instanceName)
{
    const std::wstring source = Trim(std::wstring(instanceName));
    const std::wstring lowered = ToLower(source);
    std::size_t marker = lowered.find(L"display\\");
    if (marker == std::wstring::npos)
    {
        marker = lowered.find(L"monitor\\");
    }
    if (marker == std::wstring::npos)
    {
        return {};
    }

    const std::size_t start = marker + 8U;
    std::size_t end = source.find(L'\\', start);
    if (end == std::wstring::npos)
    {
        end = source.find(L'_', start);
    }
    if (end == std::wstring::npos)
    {
        end = source.size();
    }
    if (end <= start)
    {
        return {};
    }
    std::wstring code = Trim(source.substr(start, end - start));
    return IsGenericMonitorLabel(code) ? std::wstring{} : code;
}

void ApplyMonitorWmiFallback(
    MonitorDetails& details,
    const std::wstring& instancePath,
    const WmiRows& monitorRows,
    const WmiRows& sizeRows)
{
    if (!HasText(details.name))
    {
        for (const WmiRow& row : monitorRows)
        {
            if (row.size() < 3U || !EqualsIgnoreCase(Trim(row[2]), L"True") ||
                !EqualsIgnoreCase(NormalizeDisplayRegistryInstancePath(row[1], true), instancePath))
                continue;
            const std::wstring name = NormalizeMonitorLabel(row[0]);
            if (HasText(name)) { details.name = name; break; }
        }
    }
    if (details.widthMillimeters != 0U && details.heightMillimeters != 0U) return;
    for (const WmiRow& row : sizeRows)
    {
        if (row.size() < 4U || !EqualsIgnoreCase(Trim(row[3]), L"True") ||
            !EqualsIgnoreCase(NormalizeDisplayRegistryInstancePath(row[0], true), instancePath))
            continue;
        const auto width = ParseUnsigned(row[1]);
        const auto height = ParseUnsigned(row[2]);
        // These WMI properties are uint8 centimeters. Zero denotes unknown.
        if (width == 0U || height == 0U || width > 255U || height > 255U)
            continue;
        details.widthMillimeters = static_cast<unsigned>(width) * 10U;
        details.heightMillimeters = static_cast<unsigned>(height) * 10U;
        break;
    }
}

std::wstring MonitorNameForInstance(const WmiRows& rows, const std::wstring& instancePath)
{
    for (const WmiRow& row : rows)
    {
        if (row.size() >= 2U && EqualsIgnoreCase(
                NormalizeDisplayRegistryInstancePath(row[1]), instancePath))
        {
            const auto name = NormalizeMonitorLabel(row[0]);
            if (HasText(name)) return name;
        }
    }
    return {};
}

std::wstring CollectMonitorInfo(
    const WmiConnection& cimv2,
    const WmiConnection* const rootWmi,
    std::vector<std::wstring>& diagnostics)
{
    WmiRows monitorRows;
    WmiRows sizeRows;
    if (rootWmi != nullptr)
    {
        monitorRows = rootWmi->Query(
            L"SELECT UserFriendlyName, InstanceName, Active FROM WmiMonitorID",
            {L"UserFriendlyName", L"InstanceName", L"Active"});
        sizeRows = rootWmi->Query(
            L"SELECT InstanceName, MaxHorizontalImageSize, MaxVerticalImageSize, Active "
            L"FROM WmiMonitorBasicDisplayParams",
            {L"InstanceName", L"MaxHorizontalImageSize", L"MaxVerticalImageSize", L"Active"});
    }

    WmiRows pnpRows;
    WmiRows desktopRows;
    bool fallbackNamesLoaded = false;
    const auto loadFallbackNames = [&] {
        if (fallbackNamesLoaded) return;
        fallbackNamesLoaded = true;
        pnpRows = cimv2.Query(
            L"SELECT Name, PNPDeviceID FROM Win32_PnPEntity WHERE PNPClass='Monitor'",
            {L"Name", L"PNPDeviceID"});
        desktopRows = cimv2.Query(
            L"SELECT Name, PNPDeviceID FROM Win32_DesktopMonitor", {L"Name", L"PNPDeviceID"});
    };
    const auto paths = CollectActiveMonitorInstancePaths(monitorRows, sizeRows);
    std::vector<std::wstring> values;
    for (const auto& path : paths)
    {
        std::vector<BYTE> edid;
        LSTATUS error{};
        MonitorDetails details;
        if (ReadEdidFromRegistryInstancePath(path, edid, error))
        {
            details = MonitorDetailsFromEdid(edid);
            if (!HasEdidBaseBlock(edid))
                diagnostics.push_back(L"显示器 EDID 头无效，回退 WMI：" + path);
        }
        else
        {
            diagnostics.push_back(L"显示器 EDID RegGetValueW 失败，Win32=" +
                std::to_wstring(error) + L"，回退 WMI：" + path);
        }
        ApplyMonitorWmiFallback(details, path, monitorRows, sizeRows);
        if (!HasText(details.name))
        {
            loadFallbackNames();
            details.name = MonitorNameForInstance(pnpRows, path);
            if (!HasText(details.name)) details.name = MonitorNameForInstance(desktopRows, path);
            if (!HasText(details.name)) details.name = ExtractMonitorCode(path);
        }
        AddUnique(values, FormatMonitorDetails(details));
    }
    if (paths.empty())
    {
        // Preserve the existing WMI name fallback if no usable active instance
        // can be identified. Never attach a different monitor's dimensions.
        loadFallbackNames();
        for (const auto* rows : {&pnpRows, &desktopRows})
        {
            for (const auto& row : *rows)
                if (!row.empty()) AddUnique(values, NormalizeMonitorLabel(row[0]));
            if (!values.empty()) break;
        }
    }
    return Join(values, L"\r\n");
}

[[nodiscard]] bool IsTrueText(const std::wstring_view value)
{
    const std::wstring normalized = Trim(std::wstring(value));
    return _wcsicmp(normalized.c_str(), L"True") == 0;
}

[[nodiscard]] bool IsVirtualNetworkAdapter(const std::wstring_view description)
{
    const std::wstring matcher = ToLower(std::wstring(description));
    static constexpr const wchar_t* virtualMarkers[] = {
        L"virtual", L"hyper-v", L"virtualbox", L"vmware", L"wan miniport",
        L"kernel debug", L"isatap", L"teredo", L"6to4", L"loopback",
        L"tap-", L"tap ", L"vpn", L"tunnel", L"pseudo", L"miniport",
        L"debugger", L"ndis", L"host"};
    return std::any_of(
        std::begin(virtualMarkers),
        std::end(virtualMarkers),
        [&matcher](const wchar_t* const marker) {
            return matcher.find(marker) != std::wstring::npos;
        });
}

[[nodiscard]] bool IsWirelessNetworkAdapter(const std::wstring_view description)
{
    const std::wstring matcher = ToLower(std::wstring(description));
    return matcher.find(L"wireless") != std::wstring::npos ||
        matcher.find(L"wi-fi") != std::wstring::npos ||
        matcher.find(L"wifi") != std::wstring::npos ||
        matcher.find(L"802.11") != std::wstring::npos ||
        matcher.find(L"wlan") != std::wstring::npos;
}

std::wstring CollectNetworkAddress(const WmiConnection& wmi, const bool wireless)
{
    const WmiRows rows = wmi.Query(
        L"SELECT Name, MACAddress, PhysicalAdapter, PNPDeviceID "
        L"FROM Win32_NetworkAdapter WHERE MACAddress IS NOT NULL",
        {L"Name", L"MACAddress", L"PhysicalAdapter", L"PNPDeviceID"});
    std::vector<std::wstring> adapters;
    for (const WmiRow& row : rows)
    {
        if (row.size() < 4U || !HasText(row[1]) || !IsTrueText(row[2]))
        {
            continue;
        }

        const std::wstring description = row[0] + L" " + row[3];
        const std::wstring lowered = ToLower(description);
        if (IsVirtualNetworkAdapter(description))
        {
            continue;
        }
        const bool isWireless = IsWirelessNetworkAdapter(description);
        const bool isBluetooth = lowered.find(L"bluetooth") != std::wstring::npos;
        if ((wireless && !isWireless) || (!wireless && (isWireless || isBluetooth)))
        {
            continue;
        }

        std::wstring adapter = HasText(row[0]) ? Trim(row[0]) : L"Unknown Adapter";
        adapter += L" (" + Trim(row[1]) + L")";
        AddUnique(adapters, std::move(adapter));
    }
    return Join(adapters, L"\r\n");
}

std::wstring CollectBluetoothAddress(const WmiConnection& wmi)
{
    const WmiRows rows = wmi.Query(
        L"SELECT Name, MACAddress, PhysicalAdapter, PNPDeviceID "
        L"FROM Win32_NetworkAdapter WHERE MACAddress IS NOT NULL",
        {L"Name", L"MACAddress", L"PhysicalAdapter", L"PNPDeviceID"});
    std::vector<std::wstring> addresses;
    for (const WmiRow& row : rows)
    {
        if (row.size() < 4U || !HasText(row[1]))
        {
            continue;
        }
        const std::wstring matcher = ToLower(row[0] + L" " + row[3]);
        if (matcher.find(L"bluetooth") != std::wstring::npos &&
            (IsTrueText(row[2]) || Trim(row[2]).empty()))
        {
            AddUnique(addresses, row[1]);
        }
    }
    return Join(addresses, L"\r\n");
}

std::wstring CollectTpmInfo(const WmiConnection* const tpmWmi)
{
    if (tpmWmi == nullptr)
    {
        return {};
    }
    const WmiRows rows = tpmWmi->Query(
        L"SELECT ManufacturerIdTxt, ManufacturerVersionInfo, SpecVersion FROM Win32_Tpm",
        {L"ManufacturerIdTxt", L"ManufacturerVersionInfo", L"SpecVersion"});
    if (rows.empty() || rows.front().size() < 3U)
    {
        return {};
    }

    std::wstring result;
    if (HasText(rows.front()[0]))
    {
        result = Trim(rows.front()[0]);
    }
    if (HasText(rows.front()[1]))
    {
        if (!result.empty())
        {
            result += L" ";
        }
        result += Trim(rows.front()[1]);
    }
    if (HasText(rows.front()[2]))
    {
        if (!result.empty())
        {
            result += L" ";
        }
        result += L"(Spec " + Trim(rows.front()[2]) + L")";
    }
    return result;
}

std::wstring ReadRegistryString(const wchar_t* const valueName)
{
    DWORD byteCount = 0;
    DWORD type = 0;
    LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        kWindowsVersionRegistryPath,
        valueName,
        RRF_RT_REG_SZ,
        &type,
        nullptr,
        &byteCount);
    if (status != ERROR_SUCCESS || byteCount < sizeof(wchar_t))
    {
        return {};
    }

    std::vector<wchar_t> buffer(
        static_cast<std::size_t>(byteCount / sizeof(wchar_t)) + 1U,
        L'\0');
    status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        kWindowsVersionRegistryPath,
        valueName,
        RRF_RT_REG_SZ,
        &type,
        buffer.data(),
        &byteCount);
    return status == ERROR_SUCCESS ? Trim(buffer.data()) : std::wstring{};
}

bool ReadRegistryDword(const wchar_t* const valueName, DWORD& value)
{
    value = 0;
    DWORD byteCount = sizeof(value);
    const LSTATUS status = RegGetValueW(
        HKEY_LOCAL_MACHINE,
        kWindowsVersionRegistryPath,
        valueName,
        RRF_RT_REG_DWORD,
        nullptr,
        &value,
        &byteCount);
    return status == ERROR_SUCCESS;
}

std::wstring CollectOsInfo(const WmiConnection& wmi)
{
    const WmiRows rows = wmi.Query(
        L"SELECT Caption, Version, BuildNumber FROM Win32_OperatingSystem",
        {L"Caption", L"Version", L"BuildNumber"});
    if (rows.empty() || rows.front().size() < 3U)
    {
        return {};
    }

    const WmiRow& row = rows.front();
    std::wstring result = Trim(row[0]);
    std::wstring displayVersion = ReadRegistryString(L"DisplayVersion");
    if (!HasText(displayVersion))
    {
        displayVersion = ReadRegistryString(L"ReleaseId");
    }
    if (HasText(displayVersion))
    {
        if (!result.empty())
        {
            result += L"  ";
        }
        result += displayVersion;
    }

    std::wstring numericVersion = Trim(row[1]);
    if (!HasText(numericVersion))
    {
        numericVersion = Trim(row[2]);
    }
    if (HasText(numericVersion))
    {
        DWORD updateBuildRevision = 0;
        if (ReadRegistryDword(L"UBR", updateBuildRevision))
        {
            numericVersion += L"." + std::to_wstring(updateBuildRevision);
        }
        if (!result.empty())
        {
            result += L"  ";
        }
        result += numericVersion;
    }
    return result;
}

std::wstring CollectBiosVersion(const WmiConnection& wmi)
{
    const WmiRows rows = wmi.Query(
        L"SELECT Manufacturer, SMBIOSBIOSVersion, ReleaseDate FROM Win32_BIOS",
        {L"Manufacturer", L"SMBIOSBIOSVersion", L"ReleaseDate"});
    if (rows.empty() || rows.front().size() < 3U)
    {
        return {};
    }

    std::wstring result;
    if (HasText(rows.front()[0]))
    {
        result = Trim(rows.front()[0]);
    }
    if (HasText(rows.front()[1]))
    {
        if (!result.empty())
        {
            result += L" ";
        }
        result += Trim(rows.front()[1]);
    }
    const std::wstring releaseDate = Trim(rows.front()[2]);
    if (releaseDate.size() >= 8U)
    {
        if (!result.empty())
        {
            result += L", ";
        }
        result += releaseDate.substr(0U, 4U) + L"/" +
            releaseDate.substr(4U, 2U) + L"/" +
            releaseDate.substr(6U, 2U);
    }
    return result;
}

std::wstring CollectEcVersion(const WmiConnection& wmi)
{
    const WmiRows rows = wmi.Query(
        L"SELECT EmbeddedControllerMajorVersion, EmbeddedControllerMinorVersion FROM Win32_BIOS",
        {L"EmbeddedControllerMajorVersion", L"EmbeddedControllerMinorVersion"});
    if (rows.empty() || rows.front().size() < 2U ||
        !HasText(rows.front()[0]) || !HasText(rows.front()[1]))
    {
        return {};
    }

    const unsigned long long major = ParseUnsigned(rows.front()[0]);
    const unsigned long long minor = ParseUnsigned(rows.front()[1]);
    if (major > 255U || minor > 255U)
    {
        return Trim(rows.front()[0]) + L"." + Trim(rows.front()[1]);
    }

    wchar_t value[16]{};
    swprintf_s(
        value,
        L"%02u.%02u",
        static_cast<unsigned int>(major),
        static_cast<unsigned int>(minor));
    return value;
}

[[nodiscard]] unsigned short ReadFirmwareWord(
    const BYTE* const data,
    const std::size_t length,
    const std::size_t offset) noexcept
{
    if (data == nullptr || offset > length || length - offset < sizeof(unsigned short))
    {
        return 0;
    }
    unsigned short value = 0;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

[[nodiscard]] unsigned int ReadFirmwareDword(
    const BYTE* const data,
    const std::size_t length,
    const std::size_t offset) noexcept
{
    if (data == nullptr || offset > length || length - offset < sizeof(unsigned int))
    {
        return 0;
    }
    unsigned int value = 0;
    memcpy(&value, data + offset, sizeof(value));
    return value;
}

std::wstring DecodeFirmwareString(const char* const text, const std::size_t length)
{
    if (text == nullptr || length == 0U || length > static_cast<std::size_t>(INT_MAX))
    {
        return {};
    }

    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int wideLength = MultiByteToWideChar(
        codePage,
        flags,
        text,
        static_cast<int>(length),
        nullptr,
        0);
    if (wideLength <= 0)
    {
        codePage = CP_ACP;
        flags = 0U;
        wideLength = MultiByteToWideChar(
            codePage,
            flags,
            text,
            static_cast<int>(length),
            nullptr,
            0);
    }
    if (wideLength <= 0)
    {
        return {};
    }

    std::wstring value(static_cast<std::size_t>(wideLength), L'\0');
    const int converted = MultiByteToWideChar(
        codePage,
        flags,
        text,
        static_cast<int>(length),
        value.data(),
        wideLength);
    return converted == wideLength ? Trim(std::move(value)) : std::wstring{};
}

std::wstring FirmwareStringAt(
    const BYTE* const stringsBegin,
    const BYTE* const stringsEnd,
    const BYTE stringIndex)
{
    if (stringsBegin == nullptr || stringsEnd == nullptr ||
        stringsBegin >= stringsEnd || stringIndex == 0U)
    {
        return {};
    }

    const BYTE* cursor = stringsBegin;
    for (BYTE currentIndex = 1U;
         currentIndex <= stringIndex && cursor < stringsEnd;
         ++currentIndex)
    {
        const BYTE* terminator = cursor;
        while (terminator < stringsEnd && *terminator != 0U)
        {
            ++terminator;
        }
        if (currentIndex == stringIndex)
        {
            return DecodeFirmwareString(
                reinterpret_cast<const char*>(cursor),
                static_cast<std::size_t>(terminator - cursor));
        }
        if (terminator >= stringsEnd)
        {
            break;
        }
        cursor = terminator + 1U;
    }
    return {};
}

std::wstring NormalizeMemoryManufacturer(std::wstring manufacturer)
{
    manufacturer = Trim(std::move(manufacturer));
    if (!HasText(manufacturer))
    {
        return {};
    }

    std::wstring compact = manufacturer;
    static_cast<void>(ReplaceAll(compact, L"0x", L""));
    static_cast<void>(ReplaceAll(compact, L"0X", L""));
    if (EqualsIgnoreCase(compact, L"0B92")) return L"KINGBANK";
    if (EqualsIgnoreCase(compact, L"2C00")) return L"Micron";
    if (EqualsIgnoreCase(compact, L"CE00")) return L"Samsung";
    if (EqualsIgnoreCase(compact, L"AD00")) return L"SK hynix";

    const std::wstring matcher = ToLower(manufacturer);
    static constexpr std::pair<const wchar_t*, const wchar_t*> aliases[] = {
        {L"samsung", L"Samsung"}, {L"crucial", L"Crucial"},
        {L"micron", L"Micron"}, {L"sk hynix", L"SK hynix"},
        {L"skhynix", L"SK hynix"}, {L"hynix", L"SK hynix"},
        {L"hyundai", L"SK hynix"}, {L"kingston", L"Kingston"},
        {L"apacer", L"Apacer"}, {L"corsair", L"Corsair"},
        {L"geil", L"GeIL"}, {L"g.skill", L"G.SKILL"},
        {L"kingmax", L"KINGMAX"}, {L"transcend", L"Transcend"},
        {L"adata", L"ADATA"}, {L"team", L"Team"},
        {L"hyperx", L"HyperX"}, {L"tigo", L"Tigo"},
        {L"kingbank", L"KINGBANK"}, {L"klevv", L"KLEVV"},
        {L"asgard", L"Asgard"}, {L"gloway", L"Gloway"},
        {L"ramaxel", L"Ramaxel"}, {L"unilc", L"UnilC"},
        {L"juhor", L"JUHOR"}, {L"nanya", L"Nanya"},
        {L"elpida", L"Elpida"}, {L"colorful", L"Colorful"},
    };
    for (const auto& [marker, normalized] : aliases)
    {
        if (matcher.find(marker) != std::wstring::npos)
        {
            return normalized;
        }
    }
    if (matcher == L"unknown" || matcher == L"not specified" ||
        matcher == L"to be filled by o.e.m.")
    {
        return L"Unknown";
    }
    return manufacturer;
}

std::wstring MemoryTypeText(const unsigned int memoryType)
{
    switch (memoryType)
    {
    case 0x03U: return L"DRAM";
    case 0x04U: return L"EDRAM";
    case 0x05U: return L"VRAM";
    case 0x06U: return L"SRAM";
    case 0x07U: return L"RAM";
    case 0x0FU: return L"SDRAM";
    case 0x10U: return L"SGRAM";
    case 0x11U: return L"RDRAM";
    case 0x12U: return L"DDR";
    case 0x13U: return L"DDR2";
    case 0x14U: return L"DDR2 FB-DIMM";
    case 0x18U: return L"DDR3";
    case 0x1AU: return L"DDR4";
    case 0x1BU: return L"LPDDR";
    case 0x1CU: return L"LPDDR2";
    case 0x1DU: return L"LPDDR3";
    case 0x1EU: return L"LPDDR4";
    case 0x20U: return L"HBM";
    case 0x21U: return L"HBM2";
    case 0x22U: return L"DDR5";
    case 0x23U: return L"LPDDR5";
    case 0x24U: return L"HBM3";
    default: return memoryType == 0U ? std::wstring{} : std::wstring{L"Unknown"};
    }
}

struct MemoryModuleInfo
{
    unsigned long long capacityBytes = 0U;
    unsigned int speedMhz = 0U;
    unsigned int configuredSpeedMhz = 0U;
    unsigned int memoryType = 0U;
    std::wstring manufacturer;
    std::wstring partNumber;
};

std::vector<MemoryModuleInfo> CollectSmbiosMemoryModules()
{
    constexpr DWORD rawSmbiosProvider = 0x52534D42U; // 'RSMB'
    const UINT requiredBytes = GetSystemFirmwareTable(rawSmbiosProvider, 0, nullptr, 0);
    if (requiredBytes < 8U)
    {
        return {};
    }

    std::vector<BYTE> rawTable(requiredBytes, 0U);
    const UINT returnedBytes = GetSystemFirmwareTable(
        rawSmbiosProvider,
        0,
        rawTable.data(),
        static_cast<DWORD>(rawTable.size()));
    if (returnedBytes < 8U || returnedBytes > rawTable.size())
    {
        return {};
    }

    const unsigned int declaredLength = ReadFirmwareDword(rawTable.data(), returnedBytes, 4U);
    const std::size_t availableLength = static_cast<std::size_t>(returnedBytes) - 8U;
    const std::size_t tableLength =
        (std::min)(static_cast<std::size_t>(declaredLength), availableLength);
    const BYTE* cursor = rawTable.data() + 8U;
    const BYTE* const tableEnd = cursor + tableLength;
    std::vector<MemoryModuleInfo> modules;

    while (cursor <= tableEnd &&
           static_cast<std::size_t>(tableEnd - cursor) >= 4U)
    {
        const BYTE structureType = cursor[0];
        const BYTE formattedLength = cursor[1];
        if (formattedLength < 4U ||
            static_cast<std::size_t>(tableEnd - cursor) < formattedLength)
        {
            break;
        }

        const BYTE* doubleNull = cursor + formattedLength;
        while (doubleNull <= tableEnd &&
               static_cast<std::size_t>(tableEnd - doubleNull) >= 2U &&
               !(doubleNull[0] == 0U && doubleNull[1] == 0U))
        {
            ++doubleNull;
        }
        if (doubleNull > tableEnd ||
            static_cast<std::size_t>(tableEnd - doubleNull) < 2U)
        {
            break;
        }

        if (structureType == 17U)
        {
            MemoryModuleInfo module;
            const unsigned short sizeValue =
                ReadFirmwareWord(cursor, formattedLength, 0x0CU);
            if (sizeValue == 0x7FFFU)
            {
                const unsigned int extendedMegabytes =
                    ReadFirmwareDword(cursor, formattedLength, 0x1CU) & 0x7FFFFFFFU;
                module.capacityBytes =
                    static_cast<unsigned long long>(extendedMegabytes) * 1024ULL * 1024ULL;
            }
            else if (sizeValue != 0U && sizeValue != 0xFFFFU)
            {
                module.capacityBytes = (sizeValue & 0x8000U) != 0U
                    ? static_cast<unsigned long long>(sizeValue & 0x7FFFU) * 1024ULL
                    : static_cast<unsigned long long>(sizeValue) * 1024ULL * 1024ULL;
            }

            module.memoryType = formattedLength > 0x12U ? cursor[0x12U] : 0U;
            module.speedMhz = ReadFirmwareWord(cursor, formattedLength, 0x15U);
            module.configuredSpeedMhz = ReadFirmwareWord(cursor, formattedLength, 0x20U);
            if (module.speedMhz == 0xFFFFU)
            {
                module.speedMhz = ReadFirmwareDword(cursor, formattedLength, 0x54U);
            }
            if (module.configuredSpeedMhz == 0xFFFFU)
            {
                module.configuredSpeedMhz =
                    ReadFirmwareDword(cursor, formattedLength, 0x58U);
            }

            const BYTE* const stringsBegin = cursor + formattedLength;
            const BYTE* const stringsEnd = doubleNull + 1U;
            if (formattedLength > 0x17U)
            {
                module.manufacturer = NormalizeMemoryManufacturer(
                    FirmwareStringAt(stringsBegin, stringsEnd, cursor[0x17U]));
            }
            if (formattedLength > 0x1AU)
            {
                module.partNumber = FirmwareStringAt(
                    stringsBegin,
                    stringsEnd,
                    cursor[0x1AU]);
            }
            if (module.capacityBytes > 0U || HasText(module.manufacturer) ||
                HasText(module.partNumber))
            {
                modules.push_back(std::move(module));
            }
        }

        if (structureType == 127U)
        {
            break;
        }
        cursor = doubleNull + 2U;
    }

    return modules;
}

std::wstring FormatMemoryCapacity(const unsigned long long bytes)
{
    if (bytes == 0U)
    {
        return L"Unknown Capacity";
    }
    constexpr unsigned long long megabyte = 1024ULL * 1024ULL;
    constexpr unsigned long long gigabyte = 1024ULL * 1024ULL * 1024ULL;
    if (bytes >= gigabyte)
    {
        if ((bytes % gigabyte) == 0U)
        {
            return std::to_wstring(bytes / gigabyte) + L"GB";
        }
        wchar_t formatted[32]{};
        swprintf_s(formatted, L"%.1fGB", static_cast<double>(bytes) / gigabyte);
        return formatted;
    }
    return std::to_wstring((bytes + (megabyte / 2U)) / megabyte) + L"MB";
}

std::wstring BuildMemoryOverview(const std::vector<MemoryModuleInfo>& modules)
{
    struct MemoryGroup
    {
        std::wstring manufacturer;
        unsigned int memoryType = 0U;
        unsigned int effectiveSpeedMhz = 0U;
        unsigned long long moduleCapacityBytes = 0U;
        unsigned long long totalCapacityBytes = 0U;
        unsigned int count = 0U;
    };

    std::vector<MemoryGroup> groups;
    for (const MemoryModuleInfo& module : modules)
    {
        const unsigned int effectiveSpeed = module.configuredSpeedMhz > 0U
            ? module.configuredSpeedMhz
            : module.speedMhz;
        auto group = std::find_if(
            groups.begin(),
            groups.end(),
            [&module, effectiveSpeed](const MemoryGroup& candidate) {
                return EqualsIgnoreCase(candidate.manufacturer, module.manufacturer) &&
                    candidate.memoryType == module.memoryType &&
                    candidate.effectiveSpeedMhz == effectiveSpeed &&
                    candidate.moduleCapacityBytes == module.capacityBytes;
            });
        if (group == groups.end())
        {
            groups.push_back(MemoryGroup{
                module.manufacturer,
                module.memoryType,
                effectiveSpeed,
                module.capacityBytes,
                0U,
                0U});
            group = std::prev(groups.end());
        }
        if (module.capacityBytes <=
            (std::numeric_limits<unsigned long long>::max)() - group->totalCapacityBytes)
        {
            group->totalCapacityBytes += module.capacityBytes;
        }
        ++group->count;
    }

    std::vector<std::wstring> lines;
    for (const MemoryGroup& group : groups)
    {
        std::wstring line;
        if (HasText(group.manufacturer) &&
            !EqualsIgnoreCase(group.manufacturer, L"Unknown"))
        {
            line = group.manufacturer + L" ";
        }
        line += FormatMemoryCapacity(group.totalCapacityBytes);
        const std::wstring type = MemoryTypeText(group.memoryType);
        if (HasText(type))
        {
            line += L" " + type;
        }
        if (group.effectiveSpeedMhz > 0U)
        {
            line += L" " + std::to_wstring(group.effectiveSpeedMhz) + L"MHz";
        }
        if (group.count > 1U && group.moduleCapacityBytes > 0U)
        {
            line += L" (" + FormatMemoryCapacity(group.moduleCapacityBytes) +
                L" x " + std::to_wstring(group.count) + L")";
        }
        lines.push_back(Trim(std::move(line)));
    }
    return Join(lines, L"\r\n");
}

std::wstring CollectMemoryInfo(const WmiConnection& wmi)
{
    const std::vector<MemoryModuleInfo> smbiosModules = CollectSmbiosMemoryModules();
    if (!smbiosModules.empty())
    {
        return BuildMemoryOverview(smbiosModules);
    }

    const WmiRows rows = wmi.Query(
        L"SELECT Capacity, Speed, ConfiguredClockSpeed, Manufacturer, PartNumber, "
        L"SMBIOSMemoryType FROM Win32_PhysicalMemory",
        {L"Capacity", L"Speed", L"ConfiguredClockSpeed", L"Manufacturer",
         L"PartNumber", L"SMBIOSMemoryType"});
    if (rows.empty())
    {
        return {};
    }

    std::vector<MemoryModuleInfo> modules;
    for (const WmiRow& row : rows)
    {
        if (row.size() < 6U)
        {
            continue;
        }
        modules.push_back(MemoryModuleInfo{
            ParseUnsigned(row[0]),
            static_cast<unsigned int>(ParseUnsigned(row[1])),
            static_cast<unsigned int>(ParseUnsigned(row[2])),
            static_cast<unsigned int>(ParseUnsigned(row[5])),
            NormalizeMemoryManufacturer(row[3]),
            Trim(row[4])});
    }
    return BuildMemoryOverview(modules);
}

void AddRow(Snapshot& snapshot, const wchar_t* const label, std::wstring value)
{
    snapshot.rows.push_back(Row{label, DisplayValue(std::move(value))});
}

void AddUnavailableRows(Snapshot& snapshot)
{
    static constexpr const wchar_t* labels[] = {
        L"主板型号",
        L"OS信息",
        L"CPU型号",
        L"GPU型号",
        L"显示器信息",
        L"内存信息",
        L"SSD信息",
        L"BIOS版本",
        L"EC版本",
        L"主板SN",
        L"系统SN",
        L"UUID",
        L"有线网卡",
        L"无线网卡",
        L"蓝牙地址",
        L"TPM型号",
    };
    for (const wchar_t* const label : labels)
    {
        AddRow(snapshot, label, {});
    }
}

} // namespace

Snapshot CollectSnapshot(const std::stop_token stopToken)
{
    Snapshot snapshot;
    std::vector<std::wstring> diagnostics;

    ComApartment apartment;
    if (!apartment.IsInitialized())
    {
        snapshot.error = L"初始化 COM 失败：" + HresultText(apartment.Failure());
        AddUnavailableRows(snapshot);
        return snapshot;
    }

    WmiConnection wmi(stopToken, diagnostics);
    if (!wmi.Connect(kCimv2Namespace, snapshot.error))
    {
        AddUnavailableRows(snapshot);
        return snapshot;
    }

    WmiConnection rootWmi(stopToken, diagnostics);
    std::wstring rootWmiError;
    const bool rootWmiReady = rootWmi.Connect(kWmiNamespace, rootWmiError);
    if (!rootWmiReady && !rootWmiError.empty())
    {
        diagnostics.push_back(std::move(rootWmiError));
    }

    WmiConnection tpmWmi(stopToken, diagnostics);
    std::wstring tpmWmiError;
    const bool tpmWmiReady = tpmWmi.Connect(kTpmNamespace, tpmWmiError);
    if (!tpmWmiReady && !tpmWmiError.empty())
    {
        diagnostics.push_back(std::move(tpmWmiError));
    }

    AddRow(
        snapshot,
        L"主板型号",
        FirstValue(wmi, L"SELECT Product FROM Win32_BaseBoard", L"Product"));
    AddRow(snapshot, L"OS信息", CollectOsInfo(wmi));
    AddRow(
        snapshot,
        L"CPU型号",
        JoinColumn(wmi, L"SELECT Name FROM Win32_Processor", L"Name"));
    AddRow(
        snapshot,
        L"GPU型号",
        CollectGpuInfo(wmi));
    AddRow(
        snapshot,
        L"显示器信息",
        CollectMonitorInfo(wmi, rootWmiReady ? &rootWmi : nullptr, diagnostics));
    AddRow(snapshot, L"内存信息", CollectMemoryInfo(wmi));
    AddRow(snapshot, L"SSD信息", {}); // Populated by the independent SMART worker.
    AddRow(snapshot, L"BIOS版本", CollectBiosVersion(wmi));
    AddRow(snapshot, L"EC版本", CollectEcVersion(wmi));
    AddRow(
        snapshot,
        L"主板SN",
        FirstValue(wmi, L"SELECT SerialNumber FROM Win32_BaseBoard", L"SerialNumber"));
    AddRow(
        snapshot,
        L"系统SN",
        FirstValue(wmi, L"SELECT SerialNumber FROM Win32_BIOS", L"SerialNumber"));
    AddRow(
        snapshot,
        L"UUID",
        FirstValue(wmi, L"SELECT UUID FROM Win32_ComputerSystemProduct", L"UUID"));
    AddRow(snapshot, L"有线网卡", CollectNetworkAddress(wmi, false));
    AddRow(snapshot, L"无线网卡", CollectNetworkAddress(wmi, true));
    AddRow(snapshot, L"蓝牙地址", CollectBluetoothAddress(wmi));
    AddRow(
        snapshot,
        L"TPM型号",
        CollectTpmInfo(tpmWmiReady ? &tpmWmi : nullptr));

    if (!diagnostics.empty() && !stopToken.stop_requested())
    {
        snapshot.diagnostic = Join(diagnostics, L"\r\n");
    }

    return snapshot;
}

} // namespace ftc::hardware_info
