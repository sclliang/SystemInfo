#pragma once

#include <Windows.h>
#include <shellapi.h>

#include <atlbase.h>
#include <atlstr.h>
#include <comutil.h>

#include <vector>

#include "CompileOptions.h"
#include "Priscilla\CommonFx.h"

// MFC-isms in the vendored sources, replaced for this non-MFC build: the app
// links no MFC, so AfxMessageBox maps to a plain message box, CStringArray
// maps to a vector-backed stand-in, and CArray gets a minimal template shim
// covering the members the library actually uses (GetCount/GetAt/operator[]/
// Add/Append/RemoveAt/RemoveAll/GetData).
template <typename T, typename TArg = T>
class CArray
{
public:
    int GetCount() const { return static_cast<int>(items_.size()); }
    int GetSize() const { return static_cast<int>(items_.size()); }
    T& operator[](int index) { return items_[static_cast<size_t>(index)]; }
    const T& operator[](int index) const { return items_[static_cast<size_t>(index)]; }
    T& GetAt(int index) { return items_[static_cast<size_t>(index)]; }
    int Add(TArg value) { items_.push_back(value); return static_cast<int>(items_.size()) - 1; }
    void Append(const CArray& other) { items_.insert(items_.end(), other.items_.begin(), other.items_.end()); }
    void RemoveAt(int index) { items_.erase(items_.begin() + index); }
    void RemoveAll() { items_.clear(); }
    T* GetData() { return items_.data(); }
private:
    std::vector<T> items_;
};

class CStringArray
{
public:
    void Add(const CString& value) { items_.push_back(value); }
    int GetSize() const { return static_cast<int>(items_.size()); }
    int GetCount() const { return static_cast<int>(items_.size()); }
    void RemoveAll() { items_.clear(); }
    CString& operator[](int index) { return items_[static_cast<size_t>(index)]; }
    CString& GetAt(int index) { return items_[static_cast<size_t>(index)]; }
private:
    std::vector<CString> items_;
};
inline int AfxMessageBox(LPCTSTR text, UINT nType = MB_OK, UINT nIDHelp = 0)
{
    UNREFERENCED_PARAMETER(nType);
    UNREFERENCED_PARAMETER(nIDHelp);
    return MessageBoxW(nullptr, text, L"AtaSmart", MB_OK);
}
inline BOOL AfxExtractSubString(CString& rString, LPCTSTR lpszFullString, int iSubString, TCHAR chSep = L'\n')
{
    if (lpszFullString == nullptr)
    {
        return FALSE;
    }
    const CString full(lpszFullString);
    int current = 0;
    for (int i = 0; i < iSubString; ++i)
    {
        current = full.Find(chSep, current);
        if (current == -1)
        {
            rString.Empty();
            return FALSE;
        }
        ++current;
    }
    const int next = full.Find(chSep, current);
    rString = next == -1 ? full.Mid(current) : full.Mid(current, next - current);
    return TRUE;
}

#define CERTNAME_JMS56X    L"Open Source Developer, Noriyuki Miyazaki"
#define CERTNAME_JMB39X    L"Open Source Developer, Noriyuki Miyazaki"
#define CERTNAME_JMS586_20 L"Open Source Developer, Noriyuki Miyazaki"
#define CERTNAME_JMS586_40 L"Open Source Developer, Noriyuki Miyazaki"
#define CERTNAME_JMS59X    L"CrystalMark Inc."

#pragma warning(disable : 4996)
