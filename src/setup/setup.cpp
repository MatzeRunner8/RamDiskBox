// RamDiskBox - Setup und Deinstallation in einer EXE
//
// Zwei Installationsarten, passend zu den Betriebsarten des Programms:
//   Fuer alle Benutzer (Standard, Administratorrechte): %ProgramFiles%\RamDiskBox, Windows-Dienst.
//     Die Markierungsdatei RamDiskBox.machine schaltet das Programm auf "installiert": Dienstdaten
//     (INI, Protokoll, Images) liegen in %ProgramData%\RamDiskBox, Programm- und Datenordner sind nur
//     fuer SYSTEM/Administratoren schreibbar (der Dienst laeuft als SYSTEM und vertraut beiden).
//     Eintrag unter HKLM; das Setup startet sich dafuer mit UAC-Rueckfrage erhoeht neu (/elevated).
//   Nur fuer mich (ohne Adminrechte): %LOCALAPPDATA%\Programs\RamDiskBox, ohne Dienst; der
//     Hintergrundprozess laeuft, solange der Benutzer angemeldet ist (Autostart der Oberflaeche).
//     Daten neben den Programmen wie bei der portablen Fassung, Eintrag unter HKCU.
// Der Deinstaller ist eine Kopie dieser EXE (Uninstall.exe); er arbeitet aus einer Temp-Kopie, damit er
// den Programmordner samt sich selbst entfernen kann. Laufwerks-Einstellungen und Images bleiben nur
// auf Wunsch nicht erhalten.
//
// Befehlszeile:
//   /quiet               ohne Dialog (vorhandene Installation wird aktualisiert)
//   /user                nur fuer den angemeldeten Benutzer (sonst fuer alle Benutzer)
//   /dir=PFAD            Zielordner
//   /desktop             Desktop-Verknuepfung        /nostartmenu   keine Startmenue-Verknuepfung
//   /autostart=0|1       Oberflaeche mit Windows starten
//   /launch              nach stiller Installation oeffnen
//   /lang=de|en          Sprache (sonst Windows-Anzeigesprache)
//   /uninstall [/quiet] [/purge]   entfernen; /purge loescht auch Einstellungen, Protokoll und Images
// Intern: /elevated, /confirmed, /waitpid=N, /startmenu=0|1, /desktop=0|1, /delgui=PFAD
// Exitcodes: 0 ok, 1 Fehler, 2 abgebrochen, 3 WinFsp fehlt
#include "../common/common.h"
#include "../common/version.h"
#include "../gui/theme.h"
#include "setup_res.h"
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <tlhelp32.h>
#include <cwctype>
#include <cstring>
#include <string>
#include <vector>

using namespace rdb;
using namespace gui;
using std::wstring;

namespace {

const wchar_t kUninstName[] = L"Uninstall.exe";
const wchar_t kLinkName[] = L"RamDiskBox.lnk";
const wchar_t kTempPrefix[] = L"RamDiskBox-Uninstall-";
const wchar_t kUninstKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\RamDiskBox";
const wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
const wchar_t kPublisher[] = L"justbox";
const UINT kCmdExitGui = 40017;   // CMD_EXIT der Oberflaeche (seit 1.0 unveraendert)

enum { EXIT_OK = 0, EXIT_FAILED = 1, EXIT_CANCELLED = 2, EXIT_NOWINFSP = 3 };

struct PayloadFile { int id; const wchar_t* name; };
const PayloadFile kFiles[] = {
    { IDR_FILE_GUI, kGuiExe }, { IDR_FILE_SVC, kSvcExe }, { IDR_FILE_CTL, L"rdbctl.exe" },
    { IDR_FILE_LIESMICH, L"LIESMICH.md" }, { IDR_FILE_README, L"README.md" }, { IDR_FILE_LICENSE, L"LICENSE" },
    { IDR_FILE_LIC_LZ4, L"LICENSE-lz4.txt" }, { IDR_FILE_LIC_ZSTD, L"LICENSE-zstd.txt" },
};
// Daten der Installation "nur fuer mich" (liegen neben den Programmen)
const wchar_t* const kUserDataFiles[] = { kConfigFile, kGuiConfigFile, kLogFile, L"RamDiskBox.log.1" };

struct Options {
    wstring dir, lang, delGui;
    bool allUsers = true, startMenu = true, desktop = false, autostart = false, autostartSet = false;
    bool launch = true, launchSet = false, quiet = false, uninstall = false, elevated = false, confirmed = false, purge = false;
    bool userSet = false;
    DWORD waitPid = 0;
};

struct Installed {
    bool found = false, allUsers = false, startMenu = true, desktop = false;
    wstring dir, version;
};

HINSTANCE g_inst;
bool g_quiet;
Options* g_opt;
bool g_guiWasRunning = false;

// ----------------------------------------------------------------------- Hilfen

wstring Title() { return TrF(L"RamDiskBox %s – Setup", L"RamDiskBox %s Setup", RDB_VERSION_W); }

int Box(HWND owner, const wstring& text, UINT flags)
{
    if (IsDark())
        return DarkMessageBox(owner, text, Title().c_str(), flags, Tr(L"&Ja", L"&Yes"), Tr(L"&Nein", L"&No"), L"OK", Tr(L"Abbrechen", L"Cancel"));
    return MessageBoxW(owner, text.c_str(), Title().c_str(), flags | MB_SETFOREGROUND);
}

bool RetryBox(HWND owner, const wstring& text)
{
    if (IsDark())
        return DarkMessageBox(owner, text, Title().c_str(), MB_OKCANCEL | MB_ICONWARNING, nullptr, nullptr,
                              Tr(L"&Wiederholen", L"&Retry"), Tr(L"Abbrechen", L"Cancel")) == IDOK;
    return MessageBoxW(owner, text.c_str(), Title().c_str(), MB_RETRYCANCEL | MB_ICONWARNING | MB_SETFOREGROUND) == IDRETRY;
}

wstring Join(const wstring& a, const wstring& b) { return a.empty() || a.back() == L'\\' ? a + b : a + L"\\" + b; }
wstring ParentDir(const wstring& p) { size_t i = p.find_last_of(L'\\'); return i == wstring::npos ? wstring() : p.substr(0, i); }
bool Exists(const wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
bool SamePath(const wstring& a, const wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool InDir(const wstring& p, const wstring& dir)
{
    return p.size() > dir.size() + 1 && _wcsnicmp(p.c_str(), dir.c_str(), dir.size()) == 0 && p[dir.size()] == L'\\';
}

wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

wstring DefaultDir(bool allUsers) { return Join(KnownFolder(allUsers ? FOLDERID_ProgramFiles : FOLDERID_UserProgramFiles), kAppName); }
wstring MachineDataDir() { return Join(KnownFolder(FOLDERID_ProgramData), kAppName); }
wstring UserGuiDir() { return Join(KnownFolder(FOLDERID_RoamingAppData), kAppName); }

wstring SelfPath()
{
    wchar_t b[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, b, ARRAYSIZE(b));
    return wstring(b, n);
}

bool IsElevated()
{
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    bool r = GetTokenInformation(t, TokenElevation, &e, sizeof e, &n) && e.TokenIsElevated;
    CloseHandle(t);
    return r;
}

// Vollstaendiger, absoluter Ordnerpfad (Laufwerk oder UNC), kein Laufwerksstamm.
bool NormalizeDir(wstring& d)
{
    d = Trim(d);
    if (d.size() >= 2 && d.front() == L'"' && d.back() == L'"') d = d.substr(1, d.size() - 2);
    if (d.empty() || d.find_first_of(L"\"<>|*?") != wstring::npos || PathIsRelativeW(d.c_str())) return false;
    bool drive = d.size() >= 3 && iswalpha(d[0]) && d[1] == L':' && d[2] == L'\\';
    if (!drive && d.rfind(L"\\\\", 0) != 0) return false;
    wchar_t full[MAX_PATH];
    DWORD n = GetFullPathNameW(d.c_str(), ARRAYSIZE(full), full, nullptr);
    if (!n || n >= ARRAYSIZE(full)) return false;
    d = full;
    while (d.size() > 3 && d.back() == L'\\') d.pop_back();
    return !PathIsRootW(d.c_str()) && d.size() < MAX_PATH - 40;
}

bool WinFspInstalled()
{
    wchar_t buf[MAX_PATH];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp", L"InstallDir", RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS)
        return false;
    return Exists(wstring(buf) + L"bin\\winfsp-x64.dll");
}

void OpenUrl(const wchar_t* url) { ShellExecuteW(nullptr, L"open", L"explorer.exe", url, nullptr, SW_SHOWNORMAL); }

// ----------------------------------------------------------------------- laufende Teile

// Prozesse, deren Programmdatei im Ordner liegt (ausser diesem Setup)
std::vector<wstring> ProcessesIn(const wstring& dir)
{
    std::vector<wstring> r;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return r;
    PROCESSENTRY32W pe{ sizeof pe };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == GetCurrentProcessId()) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t path[MAX_PATH * 4];
        DWORD n = ARRAYSIZE(path);
        if (QueryFullProcessImageNameW(p, 0, path, &n) && InDir(path, dir)) r.push_back(pe.szExeFile);
        CloseHandle(p);
    }
    CloseHandle(snap);
    return r;
}

bool WaitNotRunning(const wstring& dir, HWND owner)
{
    for (;;) {
        std::vector<wstring> run = ProcessesIn(dir);
        if (run.empty()) return true;
        if (g_quiet) {   // still: kurz warten, dann aufgeben
            for (int i = 0; i < 40 && !ProcessesIn(dir).empty(); i++) Sleep(250);
            return ProcessesIn(dir).empty();
        }
        wstring list;
        for (auto& n : run) list += L"\n  " + n;
        if (!RetryBox(owner, TrF(L"Aus %s laufen noch Programme:%s\n\nBitte beenden und dann „Wiederholen“ wählen.",
                                 L"Programs from %s are still running:%s\n\nPlease close them, then choose \"Retry\".", dir.c_str(), list.c_str())))
            return false;
    }
}

// Oberflaechen (dieser Sitzung) beenden; onlyDir: nur solche aus diesem Ordner. true = es lief eine.
bool CloseGuis(const wstring& onlyDir)
{
    struct Ctx { wstring cls, dir; std::vector<HWND> found; } ctx{ L"RamDiskBoxMainWnd" + InstanceSuffix(), onlyDir, {} };
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        Ctx& c = *(Ctx*)lp;
        wchar_t cls[128];
        if (!GetClassNameW(h, cls, 128) || _wcsicmp(cls, c.cls.c_str()) != 0) return TRUE;
        if (!c.dir.empty()) {
            DWORD pid = 0;
            GetWindowThreadProcessId(h, &pid);
            HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
            wchar_t path[MAX_PATH * 4];
            DWORD n = ARRAYSIZE(path);
            bool in = p && QueryFullProcessImageNameW(p, 0, path, &n) && InDir(path, c.dir);
            if (p) CloseHandle(p);
            if (!in) return TRUE;
        }
        c.found.push_back(h);
        return TRUE;
    }, (LPARAM)&ctx);
    for (HWND h : ctx.found) PostMessageW(h, WM_COMMAND, kCmdExitGui, 0);
    for (int i = 0; i < 40; i++) {
        bool any = false;
        for (HWND h : ctx.found) if (IsWindow(h)) any = true;
        if (!any) break;
        Sleep(125);
    }
    return !ctx.found.empty();
}

struct HostInfo { bool running = false, service = false; wstring version, exe; int mounted = 0; };

HostInfo QueryHost()
{
    HostInfo h;
    Message req, resp;
    req.cmd = L"LIST";
    if (!PipeCall(req, resp, nullptr, 2000)) return h;
    h.running = true;
    h.service = resp.Get(L"mode") == L"service";
    h.version = resp.Get(L"version");
    h.exe = resp.Get(L"exe");
    for (auto& [name, kv] : resp.sections) {
        auto it = kv.find(L"state");
        if (it != kv.end() && it->second != L"dismounted") h.mounted++;
    }
    return h;
}

// Portablen Hintergrundprozess geordnet beenden (haengt aus und speichert Images wie eingestellt)
bool QuitPortable()
{
    Message q, r;
    q.cmd = L"QUIT";
    PipeCall(q, r, nullptr, 10000);
    for (int i = 0; i < 1200 && ServiceReachable(); i++) Sleep(250);
    return !ServiceReachable();
}

bool ServiceExists()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return false;
    SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_QUERY_STATUS);
    if (s) CloseServiceHandle(s);
    CloseServiceHandle(scm);
    return s != nullptr;
}

// Dienst anhalten (er haengt die Laufwerke aus und speichert die Images); bis zu 10 Minuten
void StopService()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) return;
    SC_HANDLE s = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS);
    if (s) {
        SERVICE_STATUS ss{};
        if (ControlService(s, SERVICE_CONTROL_STOP, &ss) || GetLastError() == ERROR_SERVICE_CANNOT_ACCEPT_CTRL) {
            unsigned long long end = GetTickCount64() + 600000;
            while (QueryServiceStatus(s, &ss) && ss.dwCurrentState != SERVICE_STOPPED && GetTickCount64() < end) Sleep(250);
        }
        CloseServiceHandle(s);
    }
    CloseServiceHandle(scm);
}

// Befehl ausfuehren und Ausgabe (UTF-8 oder Konsole) einsammeln; Rueckgabe Exitcode oder -1
int RunCapture(const wstring& cmdLine, wstring& out, DWORD timeoutMs)
{
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = wr;
    si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    wstring cmd = cmdLine;
    BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (!ok) { CloseHandle(rd); return -1; }
    std::string bytes;
    char buf[4096];
    DWORD n;
    while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) bytes.append(buf, n);
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, timeoutMs);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    int wl = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), nullptr, 0);
    out.assign(wl, L'\0');
    if (wl) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), (int)bytes.size(), out.data(), wl);
    out = Trim(out);
    return (int)code;
}

// ----------------------------------------------------------------------- Dateien

// Ressource als Datei schreiben (ueber .new und Umbenennen). Ist die alte Datei in Benutzung (z. B. eine
// Oberflaeche in einer anderen Sitzung), wird sie beiseite umbenannt; Windows erlaubt das bei laufenden EXEs.
bool ExtractFile(int id, const wstring& target, DWORD& size, DWORD& err)
{
    HRSRC r = FindResourceW(g_inst, MAKEINTRESOURCEW(id), RT_RCDATA);
    HGLOBAL g = r ? LoadResource(g_inst, r) : nullptr;
    const void* data = g ? LockResource(g) : nullptr;
    size = r ? SizeofResource(g_inst, r) : 0;
    if (!data || !size) { err = ERROR_RESOURCE_DATA_NOT_FOUND; return false; }
    wstring tmp = target + L".new";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = GetLastError(); return false; }
    DWORD wr = 0;
    bool ok = WriteFile(h, data, size, &wr, nullptr) && wr == size && FlushFileBuffers(h);
    err = ok ? 0 : GetLastError();
    CloseHandle(h);
    if (!ok) { DeleteFileW(tmp.c_str()); if (!err) err = ERROR_WRITE_FAULT; return false; }
    if (MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    err = GetLastError();
    wstring old = TrF(L"%s.old-%llu", L"%s.old-%llu", target.c_str(), GetTickCount64());
    if (Exists(target) && MoveFileExW(target.c_str(), old.c_str(), 0)) {
        if (MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH)) {
            if (!DeleteFileW(old.c_str())) MoveFileExW(old.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
            return true;
        }
        err = GetLastError();
        MoveFileExW(old.c_str(), target.c_str(), 0);
    }
    DeleteFileW(tmp.c_str());
    return false;
}

// Reste frueherer Aktualisierungen (*.old-*) entfernen
void CleanOldFiles(const wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(Join(dir, L"*.old-*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do DeleteFileW(Join(dir, fd.cFileName).c_str()); while (FindNextFileW(f, &fd));
    FindClose(f);
}

bool DeleteTree(const wstring& dir)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW(Join(dir, L"*").c_str(), &fd);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            wstring p = Join(dir, fd.cFileName);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) DeleteTree(p);
            else {
                SetFileAttributesW(p.c_str(), FILE_ATTRIBUTE_NORMAL);
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveDirectoryW(p.c_str());   // Verknuepfung, nicht folgen
                else DeleteFileW(p.c_str());
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return RemoveDirectoryW(dir.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
}

// ----------------------------------------------------------------------- Verknuepfungen, Autostart

wstring LinkPath(bool allUsers, bool desktop)
{
    REFKNOWNFOLDERID id = desktop ? (allUsers ? FOLDERID_PublicDesktop : FOLDERID_Desktop)
                                  : (allUsers ? FOLDERID_CommonPrograms : FOLDERID_Programs);
    wstring dir = KnownFolder(id);
    return dir.empty() ? wstring() : Join(dir, kLinkName);
}

bool MakeShortcut(const wstring& lnk, const wstring& target, const wstring& workDir)
{
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    sl->SetWorkingDirectory(workDir.c_str());
    sl->SetIconLocation(target.c_str(), 0);
    sl->SetDescription(Tr(L"RAM-Laufwerke verwalten", L"Manage RAM drives"));
    IPersistFile* pf = nullptr;
    HRESULT hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
    if (SUCCEEDED(hr)) { hr = pf->Save(lnk.c_str(), TRUE); pf->Release(); }
    sl->Release();
    return SUCCEEDED(hr);
}

wstring ShortcutTarget(const wstring& lnk)
{
    wstring r;
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return r;
    IPersistFile* pf = nullptr;
    if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) {
        wchar_t path[MAX_PATH];
        if (SUCCEEDED(pf->Load(lnk.c_str(), STGM_READ)) && SUCCEEDED(sl->GetPath(path, MAX_PATH, nullptr, SLGP_RAWPATH))) r = path;
        pf->Release();
    }
    sl->Release();
    return r;
}

void ApplyShortcut(bool want, const wstring& lnk, const wstring& exe, const wstring& dir)
{
    if (lnk.empty()) return;
    if (want) MakeShortcut(lnk, exe, dir);
    else if (Exists(lnk) && SamePath(ShortcutTarget(lnk), exe)) DeleteFileW(lnk.c_str());
}

wstring RunValue()
{
    wchar_t buf[2048];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, buf, &sz) != ERROR_SUCCESS) return L"";
    return buf;
}

// Autostart der Oberflaeche (wie die Einstellung im Programm, HKCU des angemeldeten Benutzers)
void SetAutostart(const wstring& exe, bool on)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wstring v = L"\"" + exe + L"\" /tray";
        RegSetValueExW(k, kAppName, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else if (Lower(RunValue()).find(Lower(exe)) != wstring::npos) {
        RegDeleteValueW(k, kAppName);   // nur unseren eigenen Eintrag
    }
    RegCloseKey(k);
}

// ----------------------------------------------------------------------- Registrierung

void RegStr(HKEY k, const wchar_t* name, const wstring& v)
{
    RegSetValueExW(k, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
}
void RegDw(HKEY k, const wchar_t* name, DWORD v) { RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE*)&v, sizeof v); }
wstring RegGetStr(HKEY k, const wchar_t* name)
{
    wchar_t buf[1024];
    DWORD size = sizeof buf;
    return RegGetValueW(k, nullptr, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? buf : L"";
}
DWORD RegGetDw(HKEY k, const wchar_t* name, DWORD def)
{
    DWORD v = 0, size = sizeof v;
    return RegGetValueW(k, nullptr, name, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS ? v : def;
}

Installed FindInstalled()
{
    Installed r;
    for (int all = 1; all >= 0 && !r.found; --all) {
        HKEY k;
        if (RegOpenKeyExW(all ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kUninstKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k)) continue;
        r.dir = RegGetStr(k, L"InstallLocation");
        r.version = RegGetStr(k, L"DisplayVersion");
        r.startMenu = RegGetDw(k, L"StartMenuShortcut", 1) != 0;
        r.desktop = RegGetDw(k, L"DesktopShortcut", 0) != 0;
        RegCloseKey(k);
        r.found = !r.dir.empty();
        r.allUsers = all != 0;
    }
    return r;
}

LSTATUS Register(const Options& o, DWORD sizeKb)
{
    HKEY k;
    LSTATUS s = RegCreateKeyExW(o.allUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, kUninstKey, 0, nullptr, 0,
                                KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
    if (s != ERROR_SUCCESS) return s;
    wstring un = L"\"" + Join(o.dir, kUninstName) + L"\" /uninstall";
    SYSTEMTIME t;
    GetLocalTime(&t);
    RegStr(k, L"DisplayName", kAppName);
    RegStr(k, L"DisplayVersion", RDB_VERSION_W);
    RegStr(k, L"Publisher", kPublisher);
    RegStr(k, L"DisplayIcon", Join(o.dir, kGuiExe) + L",0");
    RegStr(k, L"InstallLocation", o.dir);
    RegStr(k, L"UninstallString", un);
    RegStr(k, L"QuietUninstallString", un + L" /quiet");
    RegStr(k, L"InstallDate", TrF(L"%04u%02u%02u", L"%04u%02u%02u", t.wYear, t.wMonth, t.wDay));
    RegStr(k, L"Comments", Tr(L"RAM-Laufwerke für Windows (benötigt WinFsp)", L"RAM drives for Windows (requires WinFsp)"));
    RegDw(k, L"NoModify", 1);
    RegDw(k, L"NoRepair", 1);
    RegDw(k, L"EstimatedSize", sizeKb);
    RegDw(k, L"StartMenuShortcut", o.startMenu);
    RegDw(k, L"DesktopShortcut", o.desktop);
    RegCloseKey(k);
    return ERROR_SUCCESS;
}

// ----------------------------------------------------------------------- Installieren

int Fail(HWND owner, const wstring& what, DWORD err)
{
    if (!g_quiet)
        Box(owner, TrF(L"Die Installation ist fehlgeschlagen:\n%s\n\n%s", L"Installation failed:\n%s\n\n%s", what.c_str(), Win32Message(err).c_str()),
            MB_OK | MB_ICONERROR);
    return EXIT_FAILED;
}

// Die eigentliche Arbeit; bei "alle Benutzer" im erhoehten Prozess
int DoInstall(const Options& o, HWND owner)
{
    if (o.allUsers) StopService();   // Aktualisierung: Dienst gibt Laufwerke und Programmdatei frei
    int r = SHCreateDirectoryExW(nullptr, o.dir.c_str(), nullptr);
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) return Fail(owner, o.dir, r);
    // Der Dienst laeuft als SYSTEM aus diesem Ordner: Benutzer duerfen ihn nicht veraendern koennen
    // (wichtig bei einem eigenen Zielordner ausserhalb von "Programme").
    if (o.allUsers && !ProtectDir(o.dir)) return Fail(owner, o.dir, GetLastError() ? GetLastError() : ERROR_ACCESS_DENIED);
    CleanOldFiles(o.dir);
    DWORD total = 0;
    for (auto& f : kFiles) {
        DWORD size = 0, err = 0;
        wstring target = Join(o.dir, f.name);
        if (!ExtractFile(f.id, target, size, err)) return Fail(owner, target, err);
        total += size;
    }
    wstring un = Join(o.dir, kUninstName), self = SelfPath();
    if (!SamePath(self, un) && !CopyFileW(self.c_str(), un.c_str(), FALSE)) return Fail(owner, un, GetLastError());
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(un.c_str(), GetFileExInfoStandard, &fa)) total += fa.nFileSizeLow;

    wstring marker = Join(o.dir, kMachineMarker);
    if (o.allUsers) {
        HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Fail(owner, marker, GetLastError());
        const char txt[] = "RamDiskBox: Installation fuer alle Benutzer. Daten in %ProgramData%\\RamDiskBox,\r\n"
                           "Einstellungen der Oberflaeche in %APPDATA%\\RamDiskBox. Nicht loeschen.\r\n";
        DWORD w;
        WriteFile(h, txt, (DWORD)strlen(txt), &w, nullptr);
        CloseHandle(h);
        wstring data = MachineDataDir();
        CreateDirectoryW(data.c_str(), nullptr);
        if (!ProtectDir(data)) return Fail(owner, data, GetLastError() ? GetLastError() : ERROR_ACCESS_DENIED);
        CreateDirectoryW(Join(data, L"Images").c_str(), nullptr);
        // Dienst einrichten bzw. auf diesen Ordner umstellen und starten
        wstring out;
        int code = RunCapture(L"\"" + Join(o.dir, kSvcExe) + L"\" install", out, 600000);
        if (code != 0) {
            if (!g_quiet)
                Box(owner, TrF(L"Der Windows-Dienst konnte nicht eingerichtet werden (Code %d).\n\n%s",
                               L"The Windows service could not be set up (code %d).\n\n%s", code, out.c_str()), MB_OK | MB_ICONERROR);
            return EXIT_FAILED;
        }
    } else {
        DeleteFileW(marker.c_str());
    }
    wstring exe = Join(o.dir, kGuiExe);
    ApplyShortcut(o.startMenu, LinkPath(o.allUsers, false), exe, o.dir);
    ApplyShortcut(o.desktop, LinkPath(o.allUsers, true), exe, o.dir);
    LSTATUS s = Register(o, total / 1024);
    if (s != ERROR_SUCCESS) return Fail(owner, wstring(o.allUsers ? L"HKLM\\" : L"HKCU\\") + kUninstKey, s);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return EXIT_OK;
}

// Startet diese EXE (oder eine Kopie); mit wait wird gewartet (Fenster bleibt bedienbar gezeichnet).
// -1 = nicht gestartet (z. B. UAC abgelehnt)
int RunSelf(const wstring& exe, const wstring& args, bool elevate, bool wait, HWND owner)
{
    SHELLEXECUTEINFOW sei{ sizeof sei };
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.hwnd = owner;
    sei.lpVerb = elevate ? L"runas" : nullptr;
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return -1;
    DWORD code = 0;
    if (wait && sei.hProcess) {
        while (MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE, INFINITE, QS_ALLINPUT) == WAIT_OBJECT_0 + 1) {
            MSG m;
            while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        GetExitCodeProcess(sei.hProcess, &code);
    }
    if (sei.hProcess) CloseHandle(sei.hProcess);
    return (int)code;
}

int InstallWithElevation(HWND owner, const Options& o)
{
    if (!o.allUsers || IsElevated()) return DoInstall(o, owner);
    wstring args = L"/elevated /dir=\"" + o.dir + L"\" /startmenu=" + (o.startMenu ? L"1" : L"0") +
                   L" /desktop=" + (o.desktop ? L"1" : L"0") + L" /lang=" + LangCode(CurrentLanguage());
    if (g_quiet) args += L" /quiet";
    int r = RunSelf(SelfPath(), args, true, true, owner);
    return r < 0 ? EXIT_CANCELLED : r;
}

// Vorpruefungen ohne Rechte (Rueckfragen hier, nicht im erhoehten Prozess). false = nicht installieren.
bool Prepare(const Options& o, HWND owner, int& exitCode)
{
    exitCode = EXIT_CANCELLED;
    if (!WinFspInstalled()) {
        exitCode = EXIT_NOWINFSP;
        if (!g_quiet && Box(owner, Tr(L"WinFsp ist nicht installiert.\n\nRamDiskBox braucht WinFsp (kostenlos, Open Source) als Dateisystem-Treiber. "
                                      L"Bitte zuerst WinFsp installieren und danach dieses Setup erneut ausführen.\n\nDie WinFsp-Downloadseite jetzt öffnen?",
                                      L"WinFsp is not installed.\n\nRamDiskBox needs WinFsp (free, open source) as file system driver. "
                                      L"Please install WinFsp first, then run this setup again.\n\nOpen the WinFsp download page now?"),
                           MB_YESNO | MB_ICONWARNING) == IDYES)
            OpenUrl(L"https://winfsp.dev/rel/");
        return false;
    }
    Installed inst = FindInstalled();
    if (inst.found && inst.allUsers != o.allUsers) {
        if (!g_quiet)
            Box(owner, TrF(L"RamDiskBox ist bereits %s installiert (%s).\n\nBitte dieselbe Installationsart wählen oder die vorhandene Installation zuerst entfernen.",
                           L"RamDiskBox is already installed %s (%s).\n\nPlease choose the same installation type or remove the existing installation first.",
                           inst.allUsers ? Tr(L"für alle Benutzer", L"for all users") : Tr(L"nur für einen Benutzer", L"for a single user"),
                           inst.dir.c_str()), MB_OK | MB_ICONWARNING);
        return false;
    }
    if (!o.allUsers && ServiceExists()) {
        if (!g_quiet)
            Box(owner, Tr(L"RamDiskBox ist auf diesem PC bereits als Windows-Dienst eingerichtet. Eine zusätzliche Installation nur für dich würde damit kollidieren.\n\n"
                          L"Bitte „Für alle Benutzer“ wählen oder den Dienst zuerst entfernen.",
                          L"RamDiskBox is already set up as a Windows service on this PC. An additional installation just for you would conflict with it.\n\n"
                          L"Please choose \"For all users\" or remove the service first."), MB_OK | MB_ICONWARNING);
        return false;
    }
    HostInfo h = QueryHost();
    if (h.running && !h.service) {
        if (!g_quiet && Box(owner, TrF(L"Es läuft bereits ein RamDiskBox-Hintergrundprozess ohne Dienst (Version %s, %d Laufwerk(e) eingehängt).\n\n"
                                       L"Er wird für die Installation beendet: Die RAM-Laufwerke werden ausgehängt, Images wie eingestellt gespeichert. "
                                       L"Inhalte ohne Image-Datei gehen verloren.\n\nFortfahren?",
                                       L"A RamDiskBox background process without service is already running (version %s, %d drive(s) mounted).\n\n"
                                       L"It is stopped for the installation: the RAM drives are unmounted, images are saved as configured. "
                                       L"Contents without an image file are lost.\n\nContinue?",
                                       h.version.c_str(), h.mounted), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return false;
        g_guiWasRunning |= CloseGuis(L"");
        if (!QuitPortable()) {
            if (!g_quiet) Box(owner, Tr(L"Der laufende Hintergrundprozess ließ sich nicht beenden.", L"The running background process could not be stopped."),
                              MB_OK | MB_ICONERROR);
            exitCode = EXIT_FAILED;
            return false;
        }
    } else if (h.running && h.service && o.allUsers && h.mounted > 0) {
        if (!g_quiet && Box(owner, TrF(L"Der RamDiskBox-Dienst läuft mit %d eingehängten Laufwerk(en). Für die Aktualisierung wird er kurz beendet: "
                                       L"Die Laufwerke werden ausgehängt (Images wie eingestellt gespeichert) und danach wieder eingehängt. "
                                       L"Inhalte ohne Image-Datei gehen dabei verloren.\n\nFortfahren?",
                                       L"The RamDiskBox service is running with %d mounted drive(s). It is stopped briefly for the update: "
                                       L"the drives are unmounted (images saved as configured) and mounted again afterwards. "
                                       L"Contents without an image file are lost.\n\nContinue?", h.mounted), MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
            return false;
    }
    g_guiWasRunning |= CloseGuis(o.dir);
    if (!o.allUsers && !WaitNotRunning(o.dir, owner)) return false;
    exitCode = EXIT_OK;
    return true;
}

void Finish(const Options& o)
{
    wstring exe = Join(o.dir, kGuiExe);
    SetAutostart(exe, o.autostart);   // HKCU des angemeldeten (nicht erhoehten) Benutzers
    if (!g_quiet)
        Box(nullptr, TrF(L"RamDiskBox %s ist installiert.", L"RamDiskBox %s has been installed.", RDB_VERSION_W) + L"\n\n" +
                     (o.allUsers ? Tr(L"Der Windows-Dienst läuft. RAM-Laufwerke legst du in der Oberfläche an (Startmenü: RamDiskBox).",
                                      L"The Windows service is running. Create RAM drives in the user interface (Start menu: RamDiskBox).")
                                 : Tr(L"RAM-Laufwerke legst du in der Oberfläche an (Startmenü: RamDiskBox). Sie gibt es, solange du angemeldet bist.",
                                      L"Create RAM drives in the user interface (Start menu: RamDiskBox). They exist while you are signed in.")),
            MB_OK | MB_ICONINFORMATION);
    // Eine vorher laufende Oberflaeche wieder oeffnen, ausser es wurde ausdruecklich /launch=0 verlangt
    if (o.launch || (g_guiWasRunning && !o.launchSet)) ShellExecuteW(nullptr, nullptr, exe.c_str(), nullptr, o.dir.c_str(), SW_SHOWNORMAL);
}

// ----------------------------------------------------------------------- Dialog

wstring ItemText(HWND h, int id)
{
    HWND c = GetDlgItem(h, id);
    int n = GetWindowTextLengthW(c);
    std::vector<wchar_t> b(n + 1);
    GetWindowTextW(c, b.data(), n + 1);
    return b.data();
}

bool Checked(HWND h, int id) { return IsDlgButtonChecked(h, id) == BST_CHECKED; }
void Check(HWND h, int id, bool on) { CheckDlgButton(h, id, on ? BST_CHECKED : BST_UNCHECKED); }

void UpdateShield(HWND h) { SendDlgItemMessageW(h, IDOK, BCM_SETSHIELD, 0, Checked(h, IDC_ALLUSERS) && !IsElevated()); }

void UpdateStatus(HWND h)
{
    SetDlgItemTextW(h, IDC_STATUS, WinFspInstalled() ? Tr(L"WinFsp ist installiert.", L"WinFsp is installed.")
                                                     : Tr(L"WinFsp fehlt noch (Dateisystem-Treiber, kostenlos).", L"WinFsp is missing (file system driver, free)."));
}

void Browse(HWND h)
{
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD f = 0;
    d->GetOptions(&f);
    d->SetOptions(f | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    d->SetTitle(Tr(L"Zielordner wählen", L"Choose destination folder"));
    wstring cur = ItemText(h, IDC_DIR);
    while (!cur.empty() && !Exists(cur)) cur = ParentDir(cur);
    IShellItem* start = nullptr;
    if (!cur.empty() && SUCCEEDED(SHCreateItemFromParsingName(cur.c_str(), nullptr, IID_PPV_ARGS(&start)))) {
        d->SetFolder(start);
        start->Release();
    }
    IShellItem* res = nullptr;
    if (SUCCEEDED(d->Show(h)) && SUCCEEDED(d->GetResult(&res))) {
        PWSTR p = nullptr;
        if (SUCCEEDED(res->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            wstring dir = p;
            CoTaskMemFree(p);
            wstring leaf = dir.substr(dir.find_last_of(L'\\') + 1);   // eigenen Unterordner anhaengen
            if (!SamePath(leaf, kAppName) && !Exists(Join(dir, kGuiExe))) dir = Join(dir, kAppName);
            SetDlgItemTextW(h, IDC_DIR, dir.c_str());
        }
        res->Release();
    }
    d->Release();
}

void EnableControls(HWND h, bool on)
{
    for (int id : { IDC_ALLUSERS, IDC_JUSTME, IDC_DIR, IDC_BROWSE, IDC_STARTMENU, IDC_DESKTOP, IDC_AUTOSTART, IDC_LAUNCH, IDOK, IDCANCEL })
        EnableWindow(GetDlgItem(h, id), on);
}

void OnInstall(HWND h)
{
    Options& o = *g_opt;
    o.dir = ItemText(h, IDC_DIR);
    if (!NormalizeDir(o.dir)) {
        Box(h, Tr(L"Bitte einen vollständigen Ordnerpfad angeben, z. B. C:\\Programme\\RamDiskBox.",
                  L"Please enter a full folder path, e.g. C:\\Program Files\\RamDiskBox."), MB_OK | MB_ICONWARNING);
        SetFocus(GetDlgItem(h, IDC_DIR));
        return;
    }
    o.allUsers = Checked(h, IDC_ALLUSERS);
    o.startMenu = Checked(h, IDC_STARTMENU);
    o.desktop = Checked(h, IDC_DESKTOP);
    o.autostart = Checked(h, IDC_AUTOSTART);
    o.launch = Checked(h, IDC_LAUNCH);
    int code;
    if (!Prepare(o, h, code)) { UpdateStatus(h); return; }
    EnableControls(h, false);
    SetDlgItemTextW(h, IDC_STATUS, Tr(L"Wird installiert …", L"Installing …"));
    HCURSOR old = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    int r = InstallWithElevation(h, o);
    SetCursor(old);
    if (r == EXIT_OK) { EndDialog(h, EXIT_OK); return; }
    EnableControls(h, true);
    UpdateStatus(h);
    SetForegroundWindow(h);
}

INT_PTR CALLBACK SetupProc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (ThemeDialogMessage(h, m, wp, lp, &themed)) return themed;
    switch (m) {
    case WM_INITDIALOG: {
        Options& o = *g_opt;
        Installed inst = FindInstalled();
        SetWindowTextW(h, Title().c_str());
        HICON big = nullptr, smallIcon = nullptr;
        LoadIconWithScaleDown(g_inst, MAKEINTRESOURCEW(IDI_APP), GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), &big);
        LoadIconWithScaleDown(g_inst, MAKEINTRESOURCEW(IDI_APP), GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), &smallIcon);
        SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
        SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)smallIcon);
        RECT rc;
        GetClientRect(GetDlgItem(h, IDC_LOGO), &rc);
        HICON logo = nullptr;
        if (SUCCEEDED(LoadIconWithScaleDown(g_inst, MAKEINTRESOURCEW(IDI_APP), rc.right, rc.bottom, &logo)))
            SendDlgItemMessageW(h, IDC_LOGO, STM_SETICON, (WPARAM)logo, 0);
        {   // Ueberschrift fett
            LOGFONTW lf{};
            GetObjectW((HFONT)SendMessageW(h, WM_GETFONT, 0, 0), sizeof lf, &lf);
            lf.lfWeight = FW_BOLD;
            SendDlgItemMessageW(h, IDC_TITLE, WM_SETFONT, (WPARAM)CreateFontIndirectW(&lf), FALSE);
        }
        bool update = inst.found;
        SetDlgItemTextW(h, IDC_TITLE, (update ? TrF(L"RamDiskBox auf Version %s aktualisieren", L"Update RamDiskBox to version %s", RDB_VERSION_W)
                                              : TrF(L"RamDiskBox %s installieren", L"Install RamDiskBox %s", RDB_VERSION_W)).c_str());
        wstring intro = Tr(L"RAM-Laufwerke für Windows: Ein Teil des Arbeitsspeichers wird zu einem sehr schnellen Laufwerk, dessen Inhalt in einer Image-Datei gesichert werden kann.",
                           L"RAM drives for Windows: part of the memory becomes a very fast drive whose contents can be kept in an image file.");
        if (update) intro = TrF(L"Installiert ist Version %s in %s. Laufwerke und Einstellungen bleiben erhalten.",
                                L"Version %s is installed in %s. Drives and settings are kept.", inst.version.c_str(), inst.dir.c_str());
        SetDlgItemTextW(h, IDC_INTRO, intro.c_str());
        SetDlgItemTextW(h, IDC_G_TYPE, Tr(L"Installationsart", L"Installation type"));
        SetDlgItemTextW(h, IDC_ALLUSERS, Tr(L"Für alle Benutzer, mit Windows-Dienst (empfohlen)", L"For all users, with Windows service (recommended)"));
        SetDlgItemTextW(h, IDC_ALLUSERS_HINT, Tr(L"Laufwerke stehen schon vor der Anmeldung bereit und werden beim Herunterfahren gesichert. Braucht Administratorrechte.",
                                                 L"Drives are available before sign-in and saved at shutdown. Requires administrator rights."));
        SetDlgItemTextW(h, IDC_JUSTME, Tr(L"Nur für mich, ohne Dienst", L"Just for me, without service"));
        SetDlgItemTextW(h, IDC_JUSTME_HINT, Tr(L"Laufwerke gibt es, solange du angemeldet bist. Keine Administratorrechte nötig.",
                                               L"Drives exist while you are signed in. No administrator rights required."));
        SetDlgItemTextW(h, IDC_L_DIR, Tr(L"Zielordner:", L"Destination folder:"));
        SetDlgItemTextW(h, IDC_STARTMENU, Tr(L"Verknüpfung im Startmenü", L"Start menu shortcut"));
        SetDlgItemTextW(h, IDC_DESKTOP, Tr(L"Verknüpfung auf dem Desktop", L"Desktop shortcut"));
        SetDlgItemTextW(h, IDC_AUTOSTART, Tr(L"Oberfläche mit Windows starten (im Infobereich)", L"Start the user interface with Windows (notification area)"));
        SetDlgItemTextW(h, IDC_LAUNCH, Tr(L"RamDiskBox nach der Installation öffnen", L"Open RamDiskBox after installation"));
        SetDlgItemTextW(h, IDOK, update ? Tr(L"Aktualisieren", L"Update") : Tr(L"Installieren", L"Install"));
        SetDlgItemTextW(h, IDCANCEL, Tr(L"Abbrechen", L"Cancel"));
        if (o.dir.empty() && inst.found) {   // Aktualisierung: bisherigen Ort und Auswahl uebernehmen
            o.dir = inst.dir;
            o.allUsers = inst.allUsers;
            o.startMenu = inst.startMenu;
            o.desktop = inst.desktop;
        }
        if (!o.autostartSet) {   // Autostart vorbelegen: ohne Dienst noetig, damit die Laufwerke wiederkommen
            wstring run = RunValue();
            o.autostart = !o.allUsers || (inst.found && Lower(run).find(Lower(Join(inst.dir, kGuiExe))) != wstring::npos);
        }
        if (o.dir.empty()) o.dir = DefaultDir(o.allUsers);
        SetDlgItemTextW(h, IDC_DIR, o.dir.c_str());
        SHAutoComplete(GetDlgItem(h, IDC_DIR), SHACF_FILESYS_DIRS);
        Check(h, IDC_ALLUSERS, o.allUsers);
        Check(h, IDC_JUSTME, !o.allUsers);
        Check(h, IDC_STARTMENU, o.startMenu);
        Check(h, IDC_DESKTOP, o.desktop);
        Check(h, IDC_AUTOSTART, o.autostart);
        Check(h, IDC_LAUNCH, o.launch);
        if (inst.found) {   // Installationsart einer vorhandenen Installation nicht wechseln
            EnableWindow(GetDlgItem(h, inst.allUsers ? IDC_JUSTME : IDC_ALLUSERS), FALSE);
            ThemeEnableLabel(GetDlgItem(h, inst.allUsers ? IDC_JUSTME_HINT : IDC_ALLUSERS_HINT), false);
        }
        UpdateShield(h);
        UpdateStatus(h);
        SetForegroundWindow(h);
        SetFocus(GetDlgItem(h, IDOK));
        return FALSE;
    }
    case WM_ACTIVATE:
        if (LOWORD(wp) != WA_INACTIVE) UpdateStatus(h);   // z. B. nachdem WinFsp installiert wurde
        break;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ALLUSERS:
        case IDC_JUSTME: {
            bool all = Checked(h, IDC_ALLUSERS);   // Standardordner mitwechseln, eigenen Ordner lassen
            if (SamePath(ItemText(h, IDC_DIR), DefaultDir(!all))) SetDlgItemTextW(h, IDC_DIR, DefaultDir(all).c_str());
            Check(h, IDC_AUTOSTART, !all);
            UpdateShield(h);
            return TRUE;
        }
        case IDC_BROWSE: Browse(h); return TRUE;
        case IDOK: OnInstall(h); return TRUE;
        case IDCANCEL: EndDialog(h, EXIT_CANCELLED); return TRUE;
        }
        break;
    }
    return FALSE;
}

// ----------------------------------------------------------------------- Deinstallieren

void SelfDeleteTempCopy()
{
    wstring self = SelfPath();
    wstring name = self.substr(self.find_last_of(L'\\') + 1);
    if (_wcsnicmp(name.c_str(), kTempPrefix, wcslen(kTempPrefix)) != 0) return;
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    wstring cmd = TrF(L"\"%s\\cmd.exe\" /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\"", L"\"%s\\cmd.exe\" /d /c ping -n 3 127.0.0.1 >nul & del /f /q \"%s\"",
                      sys, self.c_str());
    STARTUPINFOW si{ sizeof si };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, sys, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    if (IsElevated()) MoveFileExW(self.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
}

int Uninstall(Options& o)
{
    wstring self = SelfPath();
    if (o.dir.empty()) o.dir = ParentDir(self);
    if (!NormalizeDir(o.dir)) return EXIT_FAILED;
    o.allUsers = Exists(Join(o.dir, kMachineMarker));
    wstring exe = Join(o.dir, kGuiExe);

    if (!o.confirmed) {   // Schritt 1 (Uninstall.exe, ohne Rechte): fragen, dann aus einer Temp-Kopie weiter
        if (!g_quiet && Box(nullptr, TrF(L"RamDiskBox entfernen?\n\nEingehängte RAM-Laufwerke werden ausgehängt, Images wie eingestellt gespeichert. "
                                         L"Inhalte ohne Image-Datei gehen verloren.",
                                         L"Remove RamDiskBox?\n\nMounted RAM drives are unmounted, images are saved as configured. "
                                         L"Contents without an image file are lost."), MB_YESNO | MB_ICONQUESTION) != IDYES)
            return EXIT_CANCELLED;
        wstring data = o.allUsers ? MachineDataDir() : o.dir;
        bool haveData = o.allUsers ? Exists(data) : (Exists(Join(o.dir, kConfigFile)) || Exists(Join(o.dir, L"Images")));
        if (!g_quiet && haveData)
            o.purge = Box(nullptr, TrF(L"Auch die Laufwerks-Einstellungen, das Protokoll und die Image-Dateien löschen?\n\n%s\n\n"
                                       L"Bei „Nein“ bleiben sie für eine spätere Installation erhalten.",
                                       L"Also delete the drive settings, the log and the image files?\n\n%s\n\n"
                                       L"With \"No\" they are kept for a later installation.", data.c_str()),
                          MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) == IDYES;
        CloseGuis(o.dir);
        wchar_t tmp[MAX_PATH];
        GetTempPathW(MAX_PATH, tmp);
        wstring copy = TrF(L"%s%s%lu.exe", L"%s%s%lu.exe", tmp, kTempPrefix, GetCurrentProcessId());
        if (!CopyFileW(self.c_str(), copy.c_str(), FALSE)) {
            if (!g_quiet) Box(nullptr, copy + L"\n" + Win32Message(GetLastError()), MB_OK | MB_ICONERROR);
            return EXIT_FAILED;
        }
        wstring args = L"/uninstall /confirmed /dir=\"" + o.dir + L"\" /waitpid=" + std::to_wstring(GetCurrentProcessId()) +
                       L" /lang=" + LangCode(CurrentLanguage());
        if (g_quiet) args += L" /quiet";
        if (o.purge) {
            args += L" /purge";
            if (o.allUsers && Exists(UserGuiDir())) args += L" /delgui=\"" + UserGuiDir() + L"\"";
        }
        if (RunSelf(copy, args, o.allUsers && !IsElevated(), false, nullptr) < 0) {
            DeleteFileW(copy.c_str());
            return EXIT_CANCELLED;
        }
        SetAutostart(exe, false);   // Autostart des angemeldeten Benutzers
        return EXIT_OK;
    }

    // Schritt 2 (Temp-Kopie, bei "alle Benutzer" erhoeht): warten, bis Uninstall.exe beendet ist
    if (o.waitPid) {
        HANDLE p = OpenProcess(SYNCHRONIZE, FALSE, o.waitPid);
        if (p) { WaitForSingleObject(p, 15000); CloseHandle(p); }
    }
    std::vector<wstring> failed;
    if (o.allUsers) {
        // Dienst beenden (haengt aus und speichert) und entfernen
        wstring out;
        int code = RunCapture(L"\"" + Join(o.dir, kSvcExe) + L"\" uninstall", out, 900000);
        if (code != 0 && ServiceExists()) failed.push_back(TrF(L"Dienst %s: %s", L"Service %s: %s", kServiceName, out.c_str()));
    } else {
        HostInfo h = QueryHost();
        bool ours = h.running && !h.service && (h.exe.empty() ? !ProcessesIn(o.dir).empty() : InDir(h.exe, o.dir));
        if (ours) QuitPortable();
    }
    CloseGuis(o.dir);
    if (!WaitNotRunning(o.dir, nullptr)) { SelfDeleteTempCopy(); return EXIT_CANCELLED; }
    auto remove = [&](const wstring& f) {
        if (!DeleteFileW(f.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND && GetLastError() != ERROR_PATH_NOT_FOUND)
            failed.push_back(f);
    };
    for (auto& f : kFiles) remove(Join(o.dir, f.name));
    remove(Join(o.dir, kUninstName));
    remove(Join(o.dir, kMachineMarker));
    CleanOldFiles(o.dir);
    for (int all = 0; all < 2; ++all)
        for (int desk = 0; desk < 2; ++desk) {
            wstring lnk = LinkPath(all != 0, desk != 0);
            if (!lnk.empty() && Exists(lnk) && SamePath(ShortcutTarget(lnk), exe)) remove(lnk);
        }
    for (int all = 0; all < 2; ++all) {   // nur den Eintrag dieser Installation
        HKEY root = all ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, k;
        if (RegOpenKeyExW(root, kUninstKey, 0, KEY_QUERY_VALUE | KEY_WOW64_64KEY, &k)) continue;
        wstring loc = RegGetStr(k, L"InstallLocation");
        RegCloseKey(k);
        if (!SamePath(loc, o.dir)) continue;
        LSTATUS s = RegDeleteKeyExW(root, kUninstKey, KEY_WOW64_64KEY, 0);
        if (s != ERROR_SUCCESS && s != ERROR_FILE_NOT_FOUND) failed.push_back(wstring(all ? L"HKLM\\" : L"HKCU\\") + kUninstKey);
    }
    wstring kept;
    if (o.allUsers) {
        wstring data = MachineDataDir();
        if (o.purge) { if (Exists(data) && !DeleteTree(data)) failed.push_back(data); }
        else if (Exists(data)) kept = data;
        // Einstellungen der Oberflaeche: nur ein ausdruecklich uebergebener Ordner namens RamDiskBox
        wstring gui = o.delGui;
        if (o.purge && !gui.empty() && NormalizeDir(gui) && SamePath(gui.substr(gui.find_last_of(L'\\') + 1), kAppName) && Exists(gui))
            DeleteTree(gui);
    } else if (o.purge) {
        for (auto f : kUserDataFiles) remove(Join(o.dir, f));
        wstring img = Join(o.dir, L"Images");
        if (Exists(img) && !DeleteTree(img)) failed.push_back(img);
    }
    if (!RemoveDirectoryW(o.dir.c_str()) && !o.purge && Exists(o.dir)) kept = o.dir;   // fremde oder eigene Daten bleiben
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    if (!g_quiet) {
        if (failed.empty()) {
            wstring msg = Tr(L"RamDiskBox wurde entfernt.", L"RamDiskBox has been removed.");
            if (!kept.empty()) msg += L"\n\n" + TrF(L"Einstellungen und Images blieben erhalten in:\n%s", L"Settings and images were kept in:\n%s", kept.c_str());
            msg += L"\n\n";
            msg += Tr(L"WinFsp bleibt installiert; es lässt sich bei Bedarf unter „Apps“ entfernen.",
                      L"WinFsp stays installed; it can be removed under \"Apps\" if no longer needed.");
            Box(nullptr, msg, MB_OK | MB_ICONINFORMATION);
        } else {
            wstring list;
            for (auto& f : failed) list += L"\n" + f;
            Box(nullptr, TrF(L"Einiges konnte nicht entfernt werden:%s", L"Some items could not be removed:%s", list.c_str()), MB_OK | MB_ICONWARNING);
        }
    }
    SelfDeleteTempCopy();
    return failed.empty() ? EXIT_OK : EXIT_FAILED;
}

// ----------------------------------------------------------------------- Start

bool Arg(const wstring& a, const wchar_t* name, wstring* val = nullptr)
{
    size_t n = wcslen(name);
    if (a.size() < n + 1 || (a[0] != L'/' && a[0] != L'-') || _wcsnicmp(a.c_str() + 1, name, n) != 0) return false;
    if (a.size() == n + 1) { if (val) val->clear(); return true; }
    if (a[n + 1] != L'=' || !val) return false;
    *val = a.substr(n + 2);
    return true;
}

Options ParseArgs()
{
    Options o;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; ++i) {
        wstring a = argv[i], v;
        if (Arg(a, L"S") || Arg(a, L"quiet") || Arg(a, L"silent")) o.quiet = true;
        else if (Arg(a, L"dir", &v) || Arg(a, L"D", &v)) o.dir = v;
        else if (Arg(a, L"user") || Arg(a, L"currentuser")) { o.allUsers = false; o.userSet = true; }
        else if (Arg(a, L"allusers")) { o.allUsers = true; o.userSet = true; }
        else if (Arg(a, L"nostartmenu")) o.startMenu = false;
        else if (Arg(a, L"startmenu", &v)) o.startMenu = v != L"0";
        else if (Arg(a, L"desktop", &v)) o.desktop = v != L"0";
        else if (Arg(a, L"autostart", &v)) { o.autostart = v != L"0"; o.autostartSet = true; }
        else if (Arg(a, L"noautostart")) { o.autostart = false; o.autostartSet = true; }
        else if (Arg(a, L"launch", &v)) { o.launch = v != L"0"; o.launchSet = true; }
        else if (Arg(a, L"uninstall")) o.uninstall = true;
        else if (Arg(a, L"purge")) o.purge = true;
        else if (Arg(a, L"elevated")) o.elevated = true;
        else if (Arg(a, L"confirmed")) o.confirmed = true;
        else if (Arg(a, L"waitpid", &v)) o.waitPid = wcstoul(v.c_str(), nullptr, 10);
        else if (Arg(a, L"delgui", &v)) o.delGui = v;
        else if (Arg(a, L"lang", &v)) o.lang = v;
    }
    if (argv) LocalFree(argv);
    if (o.quiet && !o.launchSet) o.launch = false;
    return o;
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    Options o = ParseArgs();
    g_opt = &o;
    g_quiet = o.quiet;
    Lang lang = DefaultLanguage();
    if (!o.lang.empty()) ParseLang(o.lang, lang);
    SetProcessLanguage(lang);
    ThemeInit(ThemeMode::System);
    int r;
    if (o.uninstall) {
        r = Uninstall(o);
    } else if (o.elevated) {   // vom Dialog mit runas gestartet: nur installieren, Ergebnis als Exitcode
        o.allUsers = true;
        r = NormalizeDir(o.dir) ? DoInstall(o, nullptr) : EXIT_FAILED;
    } else if (o.quiet) {
        Installed found = FindInstalled();
        if (found.found && !o.userSet) o.allUsers = found.allUsers;
        if (o.dir.empty() && found.found && found.allUsers == o.allUsers) o.dir = found.dir;
        if (o.dir.empty()) o.dir = DefaultDir(o.allUsers);
        if (!o.autostartSet) o.autostart = !o.allUsers;
        if (!NormalizeDir(o.dir)) r = EXIT_FAILED;
        else if (Prepare(o, nullptr, r)) {
            r = InstallWithElevation(nullptr, o);
            if (r == EXIT_OK) Finish(o);
        }
    } else {
        r = (int)DialogBoxParamW(inst, MAKEINTRESOURCEW(IDD_SETUP), nullptr, SetupProc, 0);
        if (r == EXIT_OK) Finish(o);
    }
    CoUninitialize();
    return r;
}
