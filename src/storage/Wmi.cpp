#include "Wmi.h"

#include <oleauto.h>
#include <wbemidl.h>

#include <algorithm>
#include <cwctype>
#include <memory>
#include <utility>

namespace system_info::storage
{
namespace
{
constexpr long kPollMilliseconds = 200L;

template <typename Interface>
struct ComReleaser final
{
    void operator()(Interface* const value) const noexcept
    {
        if (value != nullptr)
        {
            value->Release();
        }
    }
};

template <typename Interface>
using ComPtr = std::unique_ptr<Interface, ComReleaser<Interface>>;

class Bstr final
{
public:
    explicit Bstr(const std::wstring_view text)
        : value_(SysAllocStringLen(text.data(), static_cast<UINT>(text.size())))
    {
    }

    ~Bstr()
    {
        SysFreeString(value_);
    }

    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;

    [[nodiscard]] BSTR Get() const noexcept
    {
        return value_;
    }

private:
    BSTR value_{};
};

[[nodiscard]] std::wstring Trim(std::wstring value)
{
    const auto nonSpace = [](const wchar_t character) {
        return std::iswspace(character) == 0;
    };
    const auto first = std::find_if(value.begin(), value.end(), nonSpace);
    const auto last = std::find_if(value.rbegin(), value.rend(), nonSpace).base();
    return first < last ? std::wstring(first, last) : std::wstring{};
}

[[nodiscard]] std::wstring SafeArrayCharacters(
    SAFEARRAY* const values,
    const VARTYPE elementType)
{
    if (values == nullptr || SafeArrayGetDim(values) != 1U)
    {
        return {};
    }
    long lower = 0L;
    long upper = -1L;
    if (FAILED(SafeArrayGetLBound(values, 1U, &lower)) ||
        FAILED(SafeArrayGetUBound(values, 1U, &upper)))
    {
        return {};
    }

    std::wstring text;
    if (upper >= lower)
    {
        text.reserve(static_cast<std::size_t>(upper - lower + 1L));
    }
    for (long index = lower; index <= upper; ++index)
    {
        wchar_t character = L'\0';
        HRESULT result = E_FAIL;
        if (elementType == VT_UI2)
        {
            unsigned short value = 0U;
            result = SafeArrayGetElement(values, &index, &value);
            character = static_cast<wchar_t>(value);
        }
        else
        {
            unsigned char value = 0U;
            result = SafeArrayGetElement(values, &index, &value);
            character = static_cast<wchar_t>(value);
        }
        if (FAILED(result))
        {
            return {};
        }
        if (character == L'\0')
        {
            break;
        }
        text.push_back(character);
    }
    return Trim(std::move(text));
}

[[nodiscard]] std::wstring VariantToString(VARIANT& value)
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
    if ((value.vt & VT_ARRAY) != 0U)
    {
        const VARTYPE elementType = value.vt & VT_TYPEMASK;
        if (elementType == VT_UI1 || elementType == VT_UI2)
        {
            return SafeArrayCharacters(value.parray, elementType);
        }
    }

    VARIANT converted;
    VariantInit(&converted);
    const HRESULT result = VariantChangeType(&converted, &value, 0U, VT_BSTR);
    std::wstring text;
    if (SUCCEEDED(result) && converted.bstrVal != nullptr)
    {
        text = Trim(converted.bstrVal);
    }
    VariantClear(&converted);
    return text;
}
} // namespace

WmiSession::WmiSession() noexcept
{
    const HRESULT apartment = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (SUCCEEDED(apartment))
    {
        uninitialize_ = true;
    }
    else if (apartment != RPC_E_CHANGED_MODE)
    {
        initializationError_ = apartment;
        return;
    }

    const HRESULT security = CoInitializeSecurity(
        nullptr,
        -1L,
        nullptr,
        nullptr,
        RPC_C_AUTHN_LEVEL_DEFAULT,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE,
        nullptr);
    if (FAILED(security) && security != RPC_E_TOO_LATE)
    {
        initializationError_ = security;
        return;
    }

    initializationError_ = S_OK;
    ready_ = true;
}

WmiSession::~WmiSession()
{
    if (uninitialize_)
    {
        CoUninitialize();
    }
}

bool WmiSession::IsReady() const noexcept
{
    return ready_;
}

HRESULT WmiSession::InitializationError() const noexcept
{
    return initializationError_;
}

WmiQueryResult WmiSession::Query(
    const std::wstring_view wmiNamespace,
    const std::wstring_view wql,
    const std::vector<std::wstring>& properties,
    const std::stop_token stopToken) const
{
    WmiQueryResult result;
    if (stopToken.stop_requested())
    {
        result.cancelled = true;
        return result;
    }
    if (!ready_)
    {
        result.error = initializationError_;
        return result;
    }

    IWbemLocator* rawLocator = nullptr;
    HRESULT status = CoCreateInstance(
        CLSID_WbemLocator,
        nullptr,
        CLSCTX_INPROC_SERVER,
        IID_IWbemLocator,
        reinterpret_cast<void**>(&rawLocator));
    ComPtr<IWbemLocator> locator(rawLocator);
    if (FAILED(status) || locator == nullptr)
    {
        result.error = FAILED(status) ? status : E_POINTER;
        return result;
    }

    const Bstr namespaceText(wmiNamespace);
    IWbemServices* rawServices = nullptr;
    status = locator->ConnectServer(
        namespaceText.Get(),
        nullptr,
        nullptr,
        nullptr,
        0L,
        nullptr,
        nullptr,
        &rawServices);
    ComPtr<IWbemServices> services(rawServices);
    if (FAILED(status) || services == nullptr)
    {
        result.error = FAILED(status) ? status : E_POINTER;
        return result;
    }
    if (stopToken.stop_requested())
    {
        result.cancelled = true;
        return result;
    }

    status = CoSetProxyBlanket(
        services.get(),
        RPC_C_AUTHN_WINNT,
        RPC_C_AUTHZ_NONE,
        nullptr,
        RPC_C_AUTHN_LEVEL_CALL,
        RPC_C_IMP_LEVEL_IMPERSONATE,
        nullptr,
        EOAC_NONE);
    if (FAILED(status))
    {
        result.error = status;
        return result;
    }

    const Bstr queryLanguage(L"WQL");
    const Bstr queryText(wql);
    IEnumWbemClassObject* rawEnumerator = nullptr;
    status = services->ExecQuery(
        queryLanguage.Get(),
        queryText.Get(),
        WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY,
        nullptr,
        &rawEnumerator);
    ComPtr<IEnumWbemClassObject> enumerator(rawEnumerator);
    if (FAILED(status) || enumerator == nullptr)
    {
        result.error = FAILED(status) ? status : E_POINTER;
        return result;
    }

    while (!stopToken.stop_requested())
    {
        IWbemClassObject* rawObject = nullptr;
        unsigned long returned = 0UL;
        status = enumerator->Next(
            kPollMilliseconds,
            1UL,
            &rawObject,
            &returned);
        ComPtr<IWbemClassObject> object(rawObject);
        if (status == WBEM_S_TIMEDOUT)
        {
            continue;
        }
        if (FAILED(status))
        {
            result.error = status;
            return result;
        }
        if (returned == 0UL)
        {
            result.completed = true;
            return result;
        }
        if (object == nullptr)
        {
            result.error = E_POINTER;
            return result;
        }

        std::vector<std::wstring> row;
        row.reserve(properties.size());
        for (const std::wstring& property : properties)
        {
            VARIANT value;
            VariantInit(&value);
            const Bstr propertyName(property);
            const HRESULT propertyStatus = object->Get(
                propertyName.Get(), 0L, &value, nullptr, nullptr);
            row.push_back(SUCCEEDED(propertyStatus) ? VariantToString(value) : L"");
            VariantClear(&value);
        }
        result.rows.push_back(std::move(row));
    }

    result.cancelled = true;
    return result;
}

std::wstring FormatHresult(const HRESULT value)
{
    wchar_t* message = nullptr;
    const DWORD count = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(value),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<wchar_t*>(&message),
        0U,
        nullptr);
    std::wstring text;
    if (count != 0U && message != nullptr)
    {
        text = Trim(std::wstring(message, count));
    }
    if (message != nullptr)
    {
        LocalFree(message);
    }
    wchar_t code[16]{};
    swprintf_s(code, L"0x%08X", static_cast<unsigned int>(value));
    return text.empty() ? std::wstring(code) : std::wstring(code) + L" (" + text + L")";
}
} // namespace system_info::storage
