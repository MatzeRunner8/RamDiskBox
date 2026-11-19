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

// Laeuft der Prozess mit Administratorrechten (erhoeht) oder als SYSTEM? Ohne diese Rechte
// meldet Defender nur ein nichtssagendes WBEM_E_FAILED (0x80041001).
bool ProcessIsAdmin()
{
    BYTE sid[SECURITY_MAX_SID_SIZE];
    DWORD sz = sizeof sid;
    BOOL member = FALSE;
    return CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid, &sz) && CheckTokenMembership(nullptr, sid, &member) && member;
}

} // namespace

bool DefenderExclusion(bool add, const std::wstring& path, std::wstring& err)
{
    if (!ProcessIsAdmin()) {
        err = Tr(L"Nur im Dienstmodus bzw. mit Administratorrechten möglich.",
                 L"Only possible in service mode or with administrator rights.");
        return false;
    }
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
                    err = HrText((HRESULT)rv.lVal);
                    VariantClear(&rv);
                    break;
                }
                VariantClear(&rv);
            }
            ok = true;
        } while (false);
        Rel(out); Rel(in); Rel(inDef); Rel(cls); Rel(svc); Rel(loc);
        SysFreeString(ns);
        SysFreeString(clsName);
        SysFreeString(method);
        CoUninitialize();
    });
    t.join();
    return ok;
}

} // namespace rdb
