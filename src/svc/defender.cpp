// RamDiskBox - Ausnahme im Windows-Defender-Echtzeitschutz fuer ein RAM-Laufwerk (WMI, MSFT_MpPreference)
//
// Nur auf ausdruecklichen Wunsch je Laufwerk (Standard: aus). Die Ausnahme wird beim Einhaengen
// gesetzt und beim Aushaengen wieder entfernt, damit sie nie fuer ein anderes Laufwerk mit
// demselben Buchstaben gilt. Erfordert Administrator- bzw. SYSTEM-Rechte (Dienstmodus).
#define _WIN32_DCOM
#include "host.h"
#include <wbemidl.h>
#include <oleauto.h>
#include <thread>

namespace rdb {

namespace {

std::wstring HrText(HRESULT hr)
{
    if (hr == (HRESULT)WBEM_E_ACCESS_DENIED || hr == E_ACCESSDENIED)
        return Tr(L"Zugriff verweigert (nur im Dienstmodus bzw. mit Administratorrechten möglich)",
                  L"Access denied (only possible in service mode or with administrator rights)");
    if (hr == (HRESULT)WBEM_E_INVALID_NAMESPACE || hr == (HRESULT)WBEM_E_INVALID_CLASS || hr == (HRESULT)WBEM_E_NOT_FOUND)
        return Tr(L"Windows Defender ist nicht verfügbar (anderer Virenscanner aktiv?)",
                  L"Windows Defender is not available (another antivirus program active?)");
    return TrF(L"Fehler 0x%08X", L"Error 0x%08X", (unsigned)hr);
}

template <class T> void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

// Fehler, die ein neuer Versuch nicht behebt (keine Rechte, Defender nicht vorhanden)
bool PermanentHr(HRESULT hr)
{
    return hr == (HRESULT)WBEM_E_ACCESS_DENIED || hr == E_ACCESSDENIED || hr == (HRESULT)WBEM_E_INVALID_NAMESPACE ||
           hr == (HRESULT)WBEM_E_INVALID_CLASS || hr == (HRESULT)WBEM_E_NOT_FOUND;
}

} // namespace

bool DefenderExclusion(bool add, const std::wstring& path, std::wstring& err, bool* permanent)
{
    if (permanent) *permanent = false;
    // Ohne Administrator- bzw. SYSTEM-Rechte meldet Defender nur ein nichtssagendes WBEM_E_FAILED.
    if (!ProcessIsAdmin()) {
        err = Tr(L"Nur im Dienstmodus bzw. mit Administratorrechten möglich.",
                 L"Only possible in service mode or with administrator rights.");
        if (permanent) *permanent = true;
        return false;
    }
    HRESULT last = S_OK;
    bool ok = false;
    // Meldungen in der Sprache des Aufrufers: der neue Thread hat keine eigene Sprache.
    const Lang lang = CurrentLanguage();
    // Eigener Thread mit MTA: der COM-Zustand des aufrufenden Threads bleibt unberuehrt.
    std::thread t([&] {
        SetThreadLanguage(lang);
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) { err = HrText(hr); return; }
        IWbemLocator* loc = nullptr;
        IWbemServices* svc = nullptr;
        IWbemClassObject* cls = nullptr;
        IWbemClassObject* inDef = nullptr;
        IWbemClassObject* in = nullptr;
        IWbemClassObject* out = nullptr;
        BSTR ns = SysAllocString(L"ROOT\\Microsoft\\Windows\\Defender");
        BSTR clsName = SysAllocString(L"MSFT_MpPreference");
        BSTR method = SysAllocString(add ? L"Add" : L"Remove");
        do {
            hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&loc);
            if (FAILED(hr)) { err = Tr(L"WMI nicht verfügbar: ", L"WMI not available: ") + HrText(hr); break; }
            hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
            if (FAILED(hr)) { err = HrText(hr); break; }
            CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
            hr = svc->GetObject(clsName, 0, nullptr, &cls, nullptr);
            if (FAILED(hr)) { err = HrText(hr); break; }
            hr = cls->GetMethod(method, 0, &inDef, nullptr);
            if (FAILED(hr) || !inDef) { err = HrText(hr); break; }
            hr = inDef->SpawnInstance(0, &in);
            if (FAILED(hr)) { err = HrText(hr); break; }
            SAFEARRAY* sa = SafeArrayCreateVector(VT_BSTR, 0, 1);
            if (!sa) { err = Tr(L"Zu wenig Speicher.", L"Not enough memory."); break; }
            LONG idx = 0;
            BSTR p = SysAllocString(path.c_str());
            SafeArrayPutElement(sa, &idx, p);
            SysFreeString(p);
            VARIANT v;
            VariantInit(&v);
            v.vt = VT_ARRAY | VT_BSTR;
            v.parray = sa;
            hr = in->Put(L"ExclusionPath", 0, &v, 0);
            VariantClear(&v);
            if (FAILED(hr)) { err = HrText(hr); break; }
            hr = svc->ExecMethod(clsName, method, 0, nullptr, in, &out, nullptr);
            if (FAILED(hr)) { err = HrText(hr); break; }
            if (out) {
                VARIANT rv;
                VariantInit(&rv);
                if (SUCCEEDED(out->Get(L"ReturnValue", 0, &rv, nullptr, nullptr)) && (rv.vt == VT_I4 || rv.vt == VT_UI4) && rv.lVal != 0) {
                    hr = (HRESULT)rv.lVal;
                    err = HrText(hr);
                    VariantClear(&rv);
                    break;
                }
                VariantClear(&rv);
            }
            ok = true;
        } while (false);
        if (!ok) last = hr;
        Rel(out); Rel(in); Rel(inDef); Rel(cls); Rel(svc); Rel(loc);
        SysFreeString(ns);
        SysFreeString(clsName);
        SysFreeString(method);
        CoUninitialize();
    });
    t.join();
    if (!ok && permanent) *permanent = PermanentHr(last);
    return ok;
}

namespace {
std::wstring NormPath(std::wstring p)
{
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    return p;
}
} // namespace

// has: Pfad steht schon in den Ausnahmen. hidden: Defender zeigt die Ausnahmen nicht an ("N/A: ...",
// z. B. per Richtlinie vor lokalen Administratoren verborgen); dann laesst sich nicht feststellen, ob
// der Pfad schon eingetragen ist.
bool DefenderHasExclusion(const std::wstring& path, bool& has, bool& hidden, std::wstring& err, bool* permanent)
{
    has = hidden = false;
    if (permanent) *permanent = false;
    if (!ProcessIsAdmin()) {
        err = Tr(L"Nur im Dienstmodus bzw. mit Administratorrechten möglich.",
                 L"Only possible in service mode or with administrator rights.");
        if (permanent) *permanent = true;
        return false;
    }
    bool ok = false;
    HRESULT last = S_OK;
    const Lang lang = CurrentLanguage();
    const std::wstring want = NormPath(path);
    std::thread t([&] {
        SetThreadLanguage(lang);
        HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(hr)) { err = HrText(hr); return; }
        IWbemLocator* loc = nullptr;
        IWbemServices* svc = nullptr;
        IEnumWbemClassObject* en = nullptr;
        IWbemClassObject* obj = nullptr;
        BSTR ns = SysAllocString(L"ROOT\\Microsoft\\Windows\\Defender");
        BSTR lang2 = SysAllocString(L"WQL");
        BSTR query = SysAllocString(L"SELECT ExclusionPath FROM MSFT_MpPreference");
        do {
            hr = CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_IWbemLocator, (void**)&loc);
            if (FAILED(hr)) { err = Tr(L"WMI nicht verfügbar: ", L"WMI not available: ") + HrText(hr); break; }
            hr = loc->ConnectServer(ns, nullptr, nullptr, nullptr, 0, nullptr, nullptr, &svc);
            if (FAILED(hr)) { err = HrText(hr); break; }
            CoSetProxyBlanket(svc, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
            hr = svc->ExecQuery(lang2, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, &en);
            if (FAILED(hr)) { err = HrText(hr); break; }
            ULONG got = 0;
            hr = en->Next(10000, 1, &obj, &got);
            if (FAILED(hr)) { err = HrText(hr); break; }
            if (got == 0) {   // Zeitueberschreitung: spaeter erneut; keine Instanz: Defender nicht vorhanden
                hr = hr == WBEM_S_TIMEDOUT ? (HRESULT)WBEM_E_TIMED_OUT : (HRESULT)WBEM_E_NOT_FOUND;
                err = HrText(hr);
                break;
            }
            VARIANT v;
            VariantInit(&v);
            hr = obj->Get(L"ExclusionPath", 0, &v, nullptr, nullptr);
            if (FAILED(hr)) { err = HrText(hr); break; }
            if (v.vt == (VT_ARRAY | VT_BSTR) && v.parray) {
                LONG lo = 0, hi = -1;
                SafeArrayGetLBound(v.parray, 1, &lo);
                SafeArrayGetUBound(v.parray, 1, &hi);
                for (LONG i = lo; i <= hi && !has; i++) {
                    BSTR s = nullptr;
                    if (SUCCEEDED(SafeArrayGetElement(v.parray, &i, &s)) && s) {
                        std::wstring e = NormPath(s);
                        if (_wcsnicmp(e.c_str(), L"N/A", 3) == 0) hidden = true;
                        has = CompareStringOrdinal(e.c_str(), (int)e.size(), want.c_str(), (int)want.size(), TRUE) == CSTR_EQUAL;
                        SysFreeString(s);
                    }
                }
            }
            VariantClear(&v);
            ok = true;
        } while (false);
        if (!ok) last = hr;
        Rel(obj); Rel(en); Rel(svc); Rel(loc);
        SysFreeString(ns);
        SysFreeString(lang2);
        SysFreeString(query);
        CoUninitialize();
    });
    t.join();
    if (!ok && permanent) *permanent = PermanentHr(last);
    return ok;
}

} // namespace rdb
