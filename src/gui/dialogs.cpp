// RamDiskBox - Dialoge und Hilfsfunktionen der Oberflaeche
#include "gui.h"
#include "../common/version.h"
#include <shellapi.h>
#include <commdlg.h>
#include <shobjidl.h>
#include <cstdarg>
#include <cwctype>
#include <memory>
#include <algorithm>

using namespace rdb;

namespace gui {

// ----------------------------------------------------------------------- Hilfen

Ini& GuiIni()
{
    static Ini ini(GuiDataDir() + L"\\" + kGuiConfigFile);
    return ini;
}

std::wstring ImagesFolder()
{
    std::wstring d = g_last.Get(L"imagesdir");   // Ordner des laufenden Hintergrundprozesses
    return d.empty() ? ImagesDir() : d;
}

void SaveGuiSettings()
{
    Ini& ini = GuiIni();
    ini.Set(L"GUI", L"Language", g_set.lang);
    ini.Set(L"GUI", L"Theme", g_set.theme);
    ini.Set(L"GUI", L"TrayClose", g_set.trayClose ? L"1" : L"0");
    ini.Set(L"GUI", L"StartMinimized", g_set.startMin ? L"1" : L"0");
}

std::wstring Fmt(const wchar_t* fmt, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

int MsgBox(HWND parent, const std::wstring& text, UINT flags)
{
    // Windows-Meldungsfenster sind immer hell; im Dunkelmodus ein eigenes
    if (IsDark())
        return DarkMessageBox(parent, text, T(S_APP), flags, T(S_BTN_YES), T(S_BTN_NO), T(S_BTN_OK), T(S_BTN_CANCEL));
    return MessageBoxW(parent, text.c_str(), T(S_APP), flags);
}

void OpenUrl(const wchar_t* url)
{
    // ueber den Explorer, damit der Browser nie mit erhoehten Rechten startet
    ShellExecuteW(nullptr, L"open", L"explorer.exe", url, nullptr, SW_SHOWNORMAL);
}

static const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

bool IsAutostart()
{
    wchar_t buf[1024];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return false;
    std::wstring v = Lower(buf);
    return v.find(Lower(ExePath(kGuiExe))) != std::wstring::npos;
}

void SetAutostart(bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        std::wstring v = L"\"" + ExePath(kGuiExe) + L"\" /tray";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

bool ServiceInstalled()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    bool ok = svc != nullptr;
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

bool WinFspInstalled()
{
    wchar_t buf[MAX_PATH];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", L"InstallDir", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return false;
    std::wstring dll = std::wstring(buf) + L"bin\\winfsp-x64.dll";
    return GetFileAttributesW(dll.c_str()) != INVALID_FILE_ATTRIBUTES;
}

int RunElevated(HWND parent, const std::wstring& exe, const std::wstring& args)
{
    SHELLEXECUTEINFOW sei{ sizeof sei };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = parent;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.lpDirectory = nullptr;
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) return -1;
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    for (;;) {
        DWORD w = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            if (m.message == WM_KEYDOWN || m.message == WM_LBUTTONDOWN || m.message == WM_RBUTTONDOWN) continue;
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    SetCursor(old);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

static bool WaitReachable(int ms)
{
    for (int t = 0; t < ms; t += 250) {
        if (ServiceReachable()) return true;
        Sleep(250);
    }
    return ServiceReachable();
}

bool EnsureHost(bool startPortable, std::wstring* err)
{
    if (ServiceReachable()) return true;
    if (ServiceInstalled()) {
        SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
        SC_HANDLE svc = scm ? OpenServiceW(scm, kServiceName, SERVICE_START | SERVICE_QUERY_STATUS) : nullptr;
        bool started = false;
        if (svc) {
            started = StartServiceW(svc, 0, nullptr) || GetLastError() == ERROR_SERVICE_ALREADY_RUNNING;
            if (!started && err) *err = Fmt(T(S_HOST_START_FAILED), Win32Message(GetLastError()).c_str());
            CloseServiceHandle(svc);
        } else if (err) {
            *err = T(S_HOST_NO_RIGHTS);
        }
        if (scm) CloseServiceHandle(scm);
        return started && WaitReachable(15000);
    }
    if (!startPortable) return false;
    std::wstring exe = ExePath(kSvcExe);
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        if (err) *err = Fmt(T(S_HOST_MISSING), exe.c_str());
        return false;
    }
    std::wstring cmd = L"\"" + exe + L"\" run";
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_NEW_PROCESS_GROUP,
            nullptr, ExeDir().c_str(), &si, &pi)) {
        if (err) *err = Win32Message(GetLastError());
        return false;
    }
    CloseHandle(pi.hThread);
    bool ok = false;
    for (int t = 0; t < 10000; t += 200) {
        if (ServiceReachable()) { ok = true; break; }
        if (WaitForSingleObject(pi.hProcess, 200) == WAIT_OBJECT_0) break;
    }
    if (!ok && err) {
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        *err = Fmt(T(S_HOST_EXITED), (unsigned long)code);
    }
    CloseHandle(pi.hProcess);
    return ok;
}

std::vector<wchar_t> FreeLetters(wchar_t include)
{
    std::vector<wchar_t> r;
    for (wchar_t c = L'D'; c <= L'Z'; c++) {
        if (c == towupper(include)) { r.push_back(c); continue; }
        if (FindVolume(c)) continue;          // fuer ein anderes RAM-Laufwerk reserviert
        if (IsDriveLetterUsed(c)) continue;
        r.push_back(c);
    }
    return r;
}

std::wstring CurrentTempDir()
{
    wchar_t buf[2048];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", L"TEMP", RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
            nullptr, buf, &sz) == ERROR_SUCCESS)
        return buf;
    return L"";
}

static void SetText(HWND dlg, int id, const std::wstring& t) { SetDlgItemTextW(dlg, id, t.c_str()); }
static bool Checked(HWND dlg, int id) { return IsDlgButtonChecked(dlg, id) == BST_CHECKED; }
static void Check(HWND dlg, int id, bool on) { CheckDlgButton(dlg, id, on ? BST_CHECKED : BST_UNCHECKED); }
static std::wstring GetText(HWND dlg, int id)
{
    wchar_t buf[2048];
    GetDlgItemTextW(dlg, id, buf, 2048);
    return buf;
}

static bool PickFolder(HWND owner, std::wstring& path)
{
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return false;
    DWORD opt = 0;
    d->GetOptions(&opt);
    d->SetOptions(opt | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(T(S_PICK_SOURCE));
    if (!path.empty()) {
        IShellItem* si = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&si)))) {
            d->SetFolder(si);
            si->Release();
        }
    }
    bool ok = false;
    if (SUCCEEDED(d->Show(owner))) {
        IShellItem* res = nullptr;
        if (SUCCEEDED(d->GetResult(&res))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                path = p;
                CoTaskMemFree(p);
                ok = true;
            }
            res->Release();
        }
    }
    d->Release();
    return ok;
}

// ----------------------------------------------------------------------- Laufwerksdialog

struct VolCtx {
    VolumeConfig* cfg;
    bool isNew;
    bool mounted;
};

static std::wstring DefaultImage(wchar_t letter)
{
    return ImagesFolder() + L"\\" + std::wstring(1, letter) + L".img";
}

static wchar_t DlgLetter(HWND dlg)
{
    HWND cb = GetDlgItem(dlg, IDC_LETTER);
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    if (i < 0) return 0;
    return (wchar_t)SendMessageW(cb, CB_GETITEMDATA, i, 0);
}

static void EnableImageControls(HWND dlg)
{
    bool on = Checked(dlg, IDC_USEIMAGE);
    for (int id : { IDC_IMAGE, IDC_BROWSE, IDC_LOADIMG, IDC_SAVEIMG, IDC_COMPRESS, IDC_AUTOSAVE })
        EnableWindow(GetDlgItem(dlg, id), on);
    for (int id : { IDC_L_IMAGE, IDC_L_AUTOSAVE, IDC_L_MINUTES })
        ThemeEnableLabel(GetDlgItem(dlg, id), on);
}

static INT_PTR CALLBACK VolProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    VolCtx* ctx = (VolCtx*)GetWindowLongPtrW(dlg, DWLP_USER);
    INT_PTR themed;
    if (ThemeDialogMessage(dlg, msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = (VolCtx*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)ctx);
        VolumeConfig& c = *ctx->cfg;
        SetWindowTextW(dlg, ctx->isNew ? T(S_VD_TITLE_NEW) : Fmt(T(S_VD_TITLE_EDIT), c.letter).c_str());
        SetText(dlg, IDC_G_DRIVE, T(S_VD_DRIVE));
        SetText(dlg, IDC_L_LETTER, T(S_VD_LETTER));
        SetText(dlg, IDC_L_SIZE, T(S_VD_SIZE));
        SetText(dlg, IDC_L_LABEL, T(S_VD_LABEL));
        SetText(dlg, IDC_L_FOLDERS, T(S_VD_FOLDERS));
        SetText(dlg, IDC_L_SOURCE, T(S_VD_SOURCE));
        SetText(dlg, IDC_AVEXCLUDE, T(S_VD_AVEXCLUDE));
        SetText(dlg, IDC_HINT, (!ctx->isNew && ctx->mounted) ? T(S_VD_LETTER_MOUNTED) : T(S_VD_HINT));
        SetText(dlg, IDC_G_IMAGE, T(S_VD_IMAGE));
        SetText(dlg, IDC_USEIMAGE, T(S_VD_USEIMAGE));
        SetText(dlg, IDC_L_IMAGE, T(S_VD_FILE));
        SetText(dlg, IDC_LOADIMG, T(S_VD_LOAD));
        SetText(dlg, IDC_SAVEIMG, T(S_VD_SAVE));
        SetText(dlg, IDC_COMPRESS, T(S_VD_COMPRESS));
        SetText(dlg, IDC_L_AUTOSAVE, T(S_VD_AUTOSAVE));
        SetText(dlg, IDC_L_MINUTES, T(S_VD_MINUTES));
        SetText(dlg, IDC_G_OPTIONS, T(S_VD_OPTIONS));
        SetText(dlg, IDC_AUTOMOUNT, T(S_VD_AUTOMOUNT));
        SetText(dlg, IDC_NTFS, T(S_VD_NTFS));
        SetText(dlg, IDC_CACHE, T(S_VD_CACHE));
        SetText(dlg, IDC_WIPE, T(S_VD_WIPE));
        SetText(dlg, IDC_L_RAMCOMP, T(S_VD_RAMCOMP));
        {
            HWND rc = GetDlgItem(dlg, IDC_RAMCOMP);
            for (Str s : { S_COMP_OFF, S_COMP_FAST, S_COMP_NORMAL, S_COMP_MAX }) SendMessageW(rc, CB_ADDSTRING, 0, (LPARAM)T(s));
            SendMessageW(rc, CB_SETCURSEL, (c.compress >= 0 && c.compress <= 3) ? c.compress : 0, 0);
        }
        SetText(dlg, IDOK, T(S_BTN_OK));
        SetText(dlg, IDCANCEL, T(S_BTN_CANCEL));

        HWND cb = GetDlgItem(dlg, IDC_LETTER);
        std::vector<wchar_t> letters = ctx->mounted ? std::vector<wchar_t>{ c.letter } : FreeLetters(c.letter);
        int sel = 0;
        for (size_t i = 0; i < letters.size(); i++) {
            std::wstring t = std::wstring(1, letters[i]) + L":";
            int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t.c_str());
            SendMessageW(cb, CB_SETITEMDATA, idx, letters[i]);
            if (letters[i] == c.letter) sel = idx;
        }
        SendMessageW(cb, CB_SETCURSEL, sel, 0);
        EnableWindow(cb, !ctx->mounted);

        HWND unit = GetDlgItem(dlg, IDC_UNIT);
        SendMessageW(unit, CB_ADDSTRING, 0, (LPARAM)L"MB");
        SendMessageW(unit, CB_ADDSTRING, 0, (LPARAM)L"GB");
        const unsigned long long GB = 1ull << 30, MB = 1ull << 20;
        if (c.size >= GB && c.size % (GB / 4) == 0) {
            double g = (double)c.size / GB;
            wchar_t b[32];
            swprintf_s(b, (c.size % GB == 0) ? L"%.0f" : L"%.2f", g);
            std::wstring s = b;
            for (auto& ch : s) if (ch == L'.') ch = L',';
            SetText(dlg, IDC_VSIZE, s);
            SendMessageW(unit, CB_SETCURSEL, 1, 0);
        } else {
            SetText(dlg, IDC_VSIZE, std::to_wstring(c.size / MB));
            SendMessageW(unit, CB_SETCURSEL, 0, 0);
        }
        SetText(dlg, IDC_LABEL, c.label);
        SendDlgItemMessageW(dlg, IDC_LABEL, EM_LIMITTEXT, 32, 0);
        SetText(dlg, IDC_FOLDERS, c.folders);
        SetText(dlg, IDC_SOURCE, c.source);
        Check(dlg, IDC_AVEXCLUDE, c.avExclude);
        Check(dlg, IDC_USEIMAGE, !c.image.empty());
        SetText(dlg, IDC_IMAGE, c.image.empty() ? DefaultImage(c.letter) : c.image);
        Check(dlg, IDC_LOADIMG, c.loadImage);
        Check(dlg, IDC_SAVEIMG, c.saveImage);
        Check(dlg, IDC_COMPRESS, c.compressImage);
        SetText(dlg, IDC_AUTOSAVE, std::to_wstring(c.autoSaveMin));
        Check(dlg, IDC_AUTOMOUNT, c.autoMount);
        Check(dlg, IDC_NTFS, c.reportNtfs);
        Check(dlg, IDC_CACHE, c.useCache);
        Check(dlg, IDC_WIPE, c.wipe);
        EnableImageControls(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_USEIMAGE:
            EnableImageControls(dlg);
            return TRUE;
        case IDC_BROWSE_SRC: {
            std::wstring p = Trim(GetText(dlg, IDC_SOURCE));
            if (PickFolder(dlg, p)) SetText(dlg, IDC_SOURCE, p);
            return TRUE;
        }
        case IDC_AVEXCLUDE:
            if (HIWORD(wp) == BN_CLICKED && Checked(dlg, IDC_AVEXCLUDE) && !ctx->cfg->avExclude &&
                MsgBox(dlg, T(S_AV_WARN), MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                Check(dlg, IDC_AVEXCLUDE, false);
            return TRUE;
        case IDC_LETTER:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                // Standard-Imagepfad dem neuen Buchstaben folgen lassen
                std::wstring img = GetText(dlg, IDC_IMAGE);
                for (wchar_t c = L'D'; c <= L'Z'; c++)
                    if (_wcsicmp(img.c_str(), DefaultImage(c).c_str()) == 0) { SetText(dlg, IDC_IMAGE, DefaultImage(DlgLetter(dlg))); break; }
            }
            return TRUE;
        case IDC_BROWSE: {
            std::wstring filter = T(S_IMAGE_FILTER);
            for (auto& ch : filter) if (ch == L'|') ch = L'\0';
            wchar_t file[MAX_PATH] = {};
            wcsncpy_s(file, GetText(dlg, IDC_IMAGE).c_str(), _TRUNCATE);
            std::wstring dir = ImagesFolder();
            CreateDirectoryW(dir.c_str(), nullptr);
            OPENFILENAMEW ofn{ sizeof ofn };
            ofn.hwndOwner = dlg;
            ofn.lpstrFilter = filter.c_str();
            ofn.lpstrFile = file;
            ofn.nMaxFile = MAX_PATH;
            ofn.lpstrDefExt = L"img";
            ofn.lpstrInitialDir = dir.c_str();
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | OFN_HIDEREADONLY;
            if (GetSaveFileNameW(&ofn)) SetText(dlg, IDC_IMAGE, file);
            return TRUE;
        }
        case IDOK: {
            VolumeConfig c = *ctx->cfg;
            c.letter = DlgLetter(dlg);
            unsigned long long size = 0;
            std::wstring st = GetText(dlg, IDC_VSIZE);
            bool gb = SendDlgItemMessageW(dlg, IDC_UNIT, CB_GETCURSEL, 0, 0) == 1;
            if (!ParseSize(st + (gb ? L" GB" : L" MB"), size) && !ParseSize(st, size)) size = 0;
            if (!c.letter || size < 16ull * 1024 * 1024) { MsgBox(dlg, T(S_SIZE_INVALID), MB_ICONWARNING); return TRUE; }
            MEMORYSTATUSEX ms{ sizeof ms };
            GlobalMemoryStatusEx(&ms);
            if (size > ms.ullTotalPhys && size != ctx->cfg->size &&
                MsgBox(dlg, Fmt(T(S_SIZE_RAM_WARN), FormatSize(size).c_str(), FormatSize(ms.ullTotalPhys).c_str()),
                    MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES)
                return TRUE;
            c.size = size;
            c.label = Trim(GetText(dlg, IDC_LABEL)).substr(0, 32);
            c.folders = Trim(GetText(dlg, IDC_FOLDERS));
            c.source = Trim(GetText(dlg, IDC_SOURCE));
            while (c.source.size() > 3 && (c.source.back() == L'\\' || c.source.back() == L'/')) c.source.pop_back();
            if (!c.source.empty()) {
                const std::wstring& s = c.source;
                bool drive = s.size() >= 3 && iswalpha(s[0]) && s[1] == L':' && s[2] == L'\\';
                bool unc = s.size() > 2 && s[0] == L'\\' && s[1] == L'\\';
                if ((!drive && !unc) || (drive && (wchar_t)towupper(s[0]) == c.letter)) {
                    MsgBox(dlg, T(S_SOURCE_INVALID), MB_ICONWARNING);
                    return TRUE;
                }
            }
            c.avExclude = Checked(dlg, IDC_AVEXCLUDE);
            if (Checked(dlg, IDC_USEIMAGE)) {
                std::wstring img = Trim(GetText(dlg, IDC_IMAGE));
                if (img.size() < 4 || img[1] != L':' || img[2] != L'\\' || towupper(img[0]) == c.letter) {
                    MsgBox(dlg, T(S_IMAGE_NEEDED), MB_ICONWARNING);
                    return TRUE;
                }
                c.image = img;
            } else {
                c.image.clear();
            }
            c.loadImage = Checked(dlg, IDC_LOADIMG);
            c.saveImage = Checked(dlg, IDC_SAVEIMG);
            c.compressImage = Checked(dlg, IDC_COMPRESS);
            c.autoSaveMin = (unsigned)ToU64(GetText(dlg, IDC_AUTOSAVE));
            c.autoMount = Checked(dlg, IDC_AUTOMOUNT);
            c.reportNtfs = Checked(dlg, IDC_NTFS);
            c.useCache = Checked(dlg, IDC_CACHE);
            c.wipe = Checked(dlg, IDC_WIPE);
            {
                int sel = (int)SendDlgItemMessageW(dlg, IDC_RAMCOMP, CB_GETCURSEL, 0, 0);
                c.compress = sel < 0 ? 0 : sel;
            }
            *ctx->cfg = c;
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool DlgVolume(HWND parent, VolumeConfig& cfg, bool isNew, bool mounted)
{
    VolCtx ctx{ &cfg, isNew, mounted };
    return DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_VOLUME), parent, VolProc, (LPARAM)&ctx) == IDOK;
}

// ----------------------------------------------------------------------- Einstellungen

struct SetCtx {
    bool changed = false;   // Sprache oder Darstellung
};

static void UpdateModeText(HWND dlg)
{
    std::wstring mode = g_last.Get(L"mode");
    bool installed = ServiceInstalled();
    SetText(dlg, IDC_MODE_TEXT, mode == L"service" ? T(S_SD_MODE_SERVICE) : mode == L"portable" ? T(S_SD_MODE_PORTABLE) : T(S_SD_MODE_NONE));
    // Bei einer Installation fuer alle Benutzer verwaltet das Setup den Dienst.
    EnableWindow(GetDlgItem(dlg, IDC_INSTALL), !installed && !MachineInstall());
    EnableWindow(GetDlgItem(dlg, IDC_UNINSTALL), installed && !MachineInstall());
    EnableWindow(GetDlgItem(dlg, IDC_ALLOWUSER), mode == L"service");
    Check(dlg, IDC_ALLOWUSER, g_last.Get(L"allowuser", L"1") == L"1");
}

static void RefreshLast()
{
    Message req, resp;
    req.cmd = L"LIST";
    g_online = PipeCall(req, resp, nullptr, 2000);
    g_last = g_online ? resp : Message();
}

static INT_PTR CALLBACK SetProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    SetCtx* ctx = (SetCtx*)GetWindowLongPtrW(dlg, DWLP_USER);
    INT_PTR themed;
    if (ThemeDialogMessage(dlg, msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_INITDIALOG: {
        ctx = (SetCtx*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)ctx);
        SetWindowTextW(dlg, T(S_SD_TITLE));
        SetText(dlg, IDC_G_MODE, T(S_SD_MODE));
        SetText(dlg, IDC_INSTALL, T(S_SD_INSTALL));
        SetText(dlg, IDC_UNINSTALL, T(S_SD_UNINSTALL));
        SetText(dlg, IDC_ALLOWUSER, T(S_SD_ALLOWUSER));
        SetText(dlg, IDC_G_GUI, T(S_SD_GUI));
        SetText(dlg, IDC_AUTOSTART, T(S_SD_AUTOSTART));
        SetText(dlg, IDC_STARTMIN, T(S_SD_STARTMIN));
        SetText(dlg, IDC_TRAYCLOSE, T(S_SD_TRAYCLOSE));
        SetText(dlg, IDC_L_LANG, T(S_SD_LANG));
        SetText(dlg, IDC_L_THEME, T(S_SD_THEME));
        SetText(dlg, IDOK, T(S_BTN_OK));
        SetText(dlg, IDCANCEL, T(S_BTN_CANCEL));
        Check(dlg, IDC_AUTOSTART, IsAutostart());
        Check(dlg, IDC_STARTMIN, g_set.startMin);
        Check(dlg, IDC_TRAYCLOSE, g_set.trayClose);
        HWND cb = GetDlgItem(dlg, IDC_LANG);   // Reihenfolge: auto, de, en
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)T(S_LANG_AUTO));
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"Deutsch");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)L"English");
        SendMessageW(cb, CB_SETCURSEL, g_set.lang == L"de" ? 1 : g_set.lang == L"en" ? 2 : 0, 0);
        HWND th = GetDlgItem(dlg, IDC_THEME);  // Reihenfolge wie ThemeMode: system, light, dark
        for (Str s : { S_THEME_SYSTEM, S_THEME_LIGHT, S_THEME_DARK }) SendMessageW(th, CB_ADDSTRING, 0, (LPARAM)T(s));
        SendMessageW(th, CB_SETCURSEL, (WPARAM)ParseThemeMode(g_set.theme), 0);
        UpdateModeText(dlg);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_INSTALL: {
            if (MsgBox(dlg, T(S_SD_INSTALL_Q), MB_ICONQUESTION | MB_YESNO) != IDYES) return TRUE;
            int code = RunElevated(dlg, ExePath(kSvcExe), L"install");
            if (code == 0) MsgBox(dlg, T(S_SD_INSTALL_OK), MB_ICONINFORMATION);
            else if (code != -1) MsgBox(dlg, Fmt(T(S_SD_INSTALL_FAIL), code), MB_ICONERROR);
            RefreshLast();
            UpdateModeText(dlg);
            return TRUE;
        }
        case IDC_UNINSTALL: {
            if (MsgBox(dlg, T(S_SD_UNINSTALL_Q), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return TRUE;
            int code = RunElevated(dlg, ExePath(kSvcExe), L"uninstall");
            if (code == 0) {
                std::wstring err;
                EnsureHost(true, &err);
                MsgBox(dlg, T(S_SD_UNINSTALL_OK), MB_ICONINFORMATION);
            } else if (code != -1) {
                MsgBox(dlg, Fmt(T(S_SD_INSTALL_FAIL), code), MB_ICONERROR);
            }
            RefreshLast();
            UpdateModeText(dlg);
            return TRUE;
        }
        case IDOK: {
            bool allow = Checked(dlg, IDC_ALLOWUSER);
            if (g_last.Get(L"mode") == L"service" && allow != (g_last.Get(L"allowuser", L"1") == L"1")) {
                Message req, resp;
                req.cmd = L"SETTINGS";
                req.args[L"allowuser"] = allow ? L"1" : L"0";
                std::wstring err;
                if (!PipeCall(req, resp, &err)) MsgBox(dlg, err, MB_ICONERROR);
            }
            if (Checked(dlg, IDC_AUTOSTART) != IsAutostart()) SetAutostart(Checked(dlg, IDC_AUTOSTART));
            g_set.startMin = Checked(dlg, IDC_STARTMIN);
            g_set.trayClose = Checked(dlg, IDC_TRAYCLOSE);
            LRESULT li = SendDlgItemMessageW(dlg, IDC_LANG, CB_GETCURSEL, 0, 0);
            std::wstring lang = li == 1 ? L"de" : li == 2 ? L"en" : L"auto";
            if (lang != g_set.lang) {
                g_set.lang = lang;
                SetLanguage(lang.c_str());
                ctx->changed = true;
            }
            LRESULT ti = SendDlgItemMessageW(dlg, IDC_THEME, CB_GETCURSEL, 0, 0);
            ThemeMode tm = ti == 1 ? ThemeMode::Light : ti == 2 ? ThemeMode::Dark : ThemeMode::System;
            if (tm != ParseThemeMode(g_set.theme)) {
                g_set.theme = ThemeModeName(tm);
                ThemeSetMode(tm);
                ctx->changed = true;
            }
            SaveGuiSettings();
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

bool DlgSettings(HWND parent)
{
    SetCtx ctx;
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_SETTINGS), parent, SetProc, (LPARAM)&ctx);
    return ctx.changed;
}

// ----------------------------------------------------------------------- TEMP-Umleitung

static void BroadcastEnvironment()
{
    DWORD_PTR r;
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, (LPARAM)L"Environment", SMTO_ABORTIFHUNG, 3000, &r);
}

static bool SetUserEnv(const wchar_t* name, const std::wstring& value)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Environment", 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return false;
    LSTATUS s = RegSetValueExW(k, name, 0, REG_EXPAND_SZ, (const BYTE*)value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(k);
    return s == ERROR_SUCCESS;
}

static std::wstring GetUserEnv(const wchar_t* name)
{
    wchar_t buf[2048];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Environment", name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | RRF_NOEXPAND,
            nullptr, buf, &sz) == ERROR_SUCCESS)
        return buf;
    return L"";
}

static bool CreateDirs(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    size_t p = path.find_last_of(L'\\');
    if (p == std::wstring::npos || p < 3) return false;
    return CreateDirs(path.substr(0, p)) && (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
}

static INT_PTR CALLBACK TempProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (ThemeDialogMessage(dlg, msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_INITDIALOG: {
        SetWindowTextW(dlg, T(S_TD_TITLE));
        SetText(dlg, IDC_L_TEMP_INFO, T(S_TD_INFO));
        SetText(dlg, IDC_TEMP_CURRENT, Fmt(T(S_TD_CURRENT), CurrentTempDir().c_str()));
        SetText(dlg, IDC_L_TEMP_DRIVE, T(S_TD_DRIVE));
        SetText(dlg, IDC_L_TEMP_FOLDER, T(S_TD_FOLDER));
        SetText(dlg, IDC_TEMP_RESET, T(S_TD_RESET));
        SetText(dlg, IDOK, T(S_TD_SET));
        SetText(dlg, IDCANCEL, T(S_BTN_CANCEL));
        SetText(dlg, IDC_TEMP_FOLDER, L"Temp");
        HWND cb = GetDlgItem(dlg, IDC_TEMP_DRIVE);
        for (auto& [name, kv] : g_last.sections) {
            auto it = kv.find(L"state");
            if (name.empty() || it == kv.end() || it->second == L"dismounted") continue;
            std::wstring t = name + L":";
            int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t.c_str());
            SendMessageW(cb, CB_SETITEMDATA, idx, name[0]);
        }
        if (SendMessageW(cb, CB_GETCOUNT, 0, 0) > 0) SendMessageW(cb, CB_SETCURSEL, 0, 0);
        else {
            EnableWindow(GetDlgItem(dlg, IDOK), FALSE);
            SetText(dlg, IDC_TEMP_CURRENT, Fmt(T(S_TD_CURRENT), CurrentTempDir().c_str()) + L"\n" + T(S_TD_NODRIVE));
        }
        EnableWindow(GetDlgItem(dlg, IDC_TEMP_RESET), !GuiIni().Get(L"Temp", L"BackupTEMP", L"").empty());
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDOK: {
            HWND cb = GetDlgItem(dlg, IDC_TEMP_DRIVE);
            int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
            if (i < 0) return TRUE;
            wchar_t l = (wchar_t)SendMessageW(cb, CB_GETITEMDATA, i, 0);
            std::wstring folder = Trim(GetText(dlg, IDC_TEMP_FOLDER));
            while (!folder.empty() && (folder[0] == L'\\' || folder[0] == L'/')) folder.erase(0, 1);
            if (folder.empty()) folder = L"Temp";
            std::wstring path = std::wstring(1, l) + L":\\" + folder;
            if (!CreateDirs(path)) { MsgBox(dlg, Win32Message(GetLastError()), MB_ICONERROR); return TRUE; }
            Ini& ini = GuiIni();
            if (ini.Get(L"Temp", L"BackupTEMP", L"").empty()) {
                ini.Set(L"Temp", L"BackupTEMP", GetUserEnv(L"TEMP"));
                ini.Set(L"Temp", L"BackupTMP", GetUserEnv(L"TMP"));
            }
            SetUserEnv(L"TEMP", path);
            SetUserEnv(L"TMP", path);
            BroadcastEnvironment();
            // Ordner beim Einhaengen automatisch anlegen lassen
            if (const KV* kv = FindVolume(l)) {
                VolumeConfig c;
                std::wstring err;
                if (VolumeConfig::FromKV(*kv, c, err)) {
                    bool have = false;
                    for (auto& f : Split(c.folders, L';')) if (_wcsicmp(f.c_str(), folder.c_str()) == 0) have = true;
                    if (!have) {
                        c.folders = c.folders.empty() ? folder : c.folders + L";" + folder;
                        Message req, resp;
                        req.cmd = L"UPDATE";
                        req.args = c.ToKV();
                        PipeCall(req, resp, nullptr);
                    }
                }
            }
            MsgBox(dlg, Fmt(T(S_TD_DONE), path.c_str()), MB_ICONINFORMATION);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDC_TEMP_RESET: {
            Ini& ini = GuiIni();
            std::wstring t = ini.Get(L"Temp", L"BackupTEMP", L"");
            std::wstring m = ini.Get(L"Temp", L"BackupTMP", L"");
            if (t.empty()) t = L"%USERPROFILE%\\AppData\\Local\\Temp";
            if (m.empty()) m = t;
            SetUserEnv(L"TEMP", t);
            SetUserEnv(L"TMP", m);
            BroadcastEnvironment();
            ini.Set(L"Temp", L"BackupTEMP", L"");
            ini.Set(L"Temp", L"BackupTMP", L"");
            MsgBox(dlg, Fmt(T(S_TD_RESET_DONE), t.c_str()), MB_ICONINFORMATION);
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void DlgTemp(HWND parent)
{
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_TEMP), parent, TempProc, 0);
}

// ----------------------------------------------------------------------- Info

static INT_PTR CALLBACK AboutProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    static HFONT bold = nullptr;
    INT_PTR themed;
    if (ThemeDialogMessage(dlg, msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_INITDIALOG: {
        {
            std::wstring t = T(S_M_ABOUT);
            t.erase(std::remove(t.begin(), t.end(), L'&'), t.end());
            SetWindowTextW(dlg, t.c_str());
        }
        SetText(dlg, IDC_ABOUT_TITLE, Fmt(T(S_AD_TITLE), RDB_VERSION_W, RDB_BUILD_W));
        SetText(dlg, IDC_ABOUT_TEXT, T(S_AD_TEXT));
        LOGFONTW lf{};
        GetObjectW((HFONT)SendMessageW(dlg, WM_GETFONT, 0, 0), sizeof lf, &lf);
        lf.lfWeight = FW_BOLD;
        bold = CreateFontIndirectW(&lf);
        SendDlgItemMessageW(dlg, IDC_ABOUT_TITLE, WM_SETFONT, (WPARAM)bold, TRUE);
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_KOFI) { OpenUrl(L"https://ko-fi.com/C5H12AP4UV"); return TRUE; }
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDOK); return TRUE; }
        break;
    case WM_DESTROY:
        if (bold) { DeleteObject(bold); bold = nullptr; }
        break;
    }
    return FALSE;
}

void DlgAbout(HWND parent)
{
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_ABOUT), parent, AboutProc, 0);
}

} // namespace gui
