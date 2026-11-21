// RamDiskBox - Cache von Browsern und Programmen auf ein RAM-Laufwerk verlagern (Extras -> Cache verlagern)
//
// Der Cache-Ordner eines Programms wird durch eine Verzeichnisverbindung (Junction) ersetzt, die auf einen
// eigenen Unterordner je Programm und Profil auf einem RamDiskBox-Laufwerk zeigt (z. B. S:\Cache\Chrome-Default).
// Junctions brauchen keine Administratorrechte. Der Zielordner wird in die Liste "Ordner anlegen" des
// Laufwerks eingetragen, damit er nach jedem Einhaengen sofort wieder da ist (sonst laufen Browser ohne
// Datei-Cache). Geloescht wird nur innerhalb des Cache-Ordners, nie ueber Verknuepfungen hinweg.
#include "gui.h"
#include <tlhelp32.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <winioctl.h>
#include <algorithm>
#include <atomic>
#include <memory>
#include <thread>

using namespace rdb;

namespace gui {

namespace {

enum class Rule { Chromium, Fixed, Package };

struct AppDef {
    const wchar_t* key;    // Name des Zielordners
    const wchar_t* name;   // Anzeigename
    Rule rule;
    bool roaming;          // unter %APPDATA% statt %LOCALAPPDATA%
    const wchar_t* path;   // Chromium: User-Data-Ordner; Fixed: Cache-Ordner; Package: "Paketmaske|Unterpfad"
    const wchar_t* exes;   // Prozessnamen, durch ';' getrennt
};

const AppDef kApps[] = {
    { L"Chrome", L"Google Chrome", Rule::Chromium, false, L"Google\\Chrome\\User Data", L"chrome.exe" },
    { L"ChromeBeta", L"Google Chrome Beta", Rule::Chromium, false, L"Google\\Chrome Beta\\User Data", L"chrome.exe" },
    { L"ChromeCanary", L"Google Chrome Canary", Rule::Chromium, false, L"Google\\Chrome SxS\\User Data", L"chrome.exe" },
    { L"Chromium", L"Chromium", Rule::Chromium, false, L"Chromium\\User Data", L"chrome.exe" },
    { L"Edge", L"Microsoft Edge", Rule::Chromium, false, L"Microsoft\\Edge\\User Data", L"msedge.exe" },
    { L"EdgeBeta", L"Microsoft Edge Beta", Rule::Chromium, false, L"Microsoft\\Edge Beta\\User Data", L"msedge.exe" },
    { L"EdgeDev", L"Microsoft Edge Dev", Rule::Chromium, false, L"Microsoft\\Edge Dev\\User Data", L"msedge.exe" },
    { L"EdgeCanary", L"Microsoft Edge Canary", Rule::Chromium, false, L"Microsoft\\Edge SxS\\User Data", L"msedge.exe" },
    { L"Brave", L"Brave", Rule::Chromium, false, L"BraveSoftware\\Brave-Browser\\User Data", L"brave.exe" },
    { L"Vivaldi", L"Vivaldi", Rule::Chromium, false, L"Vivaldi\\User Data", L"vivaldi.exe" },
    { L"Yandex", L"Yandex Browser", Rule::Chromium, false, L"Yandex\\YandexBrowser\\User Data", L"browser.exe" },
    { L"Cent", L"Cent Browser", Rule::Chromium, false, L"CentBrowser\\User Data", L"chrome.exe" },
    { L"Dragon", L"Comodo Dragon", Rule::Chromium, false, L"Comodo\\Dragon\\User Data", L"dragon.exe" },
    { L"Ulaa", L"Ulaa", Rule::Chromium, false, L"Zoho\\Ulaa\\User Data", L"ulaa.exe" },
    { L"Opera", L"Opera", Rule::Fixed, false, L"Opera Software\\Opera Stable\\Cache", L"opera.exe" },
    { L"OperaGX", L"Opera GX", Rule::Fixed, false, L"Opera Software\\Opera GX Stable\\Cache", L"opera.exe" },
    { L"Firefox", L"Mozilla Firefox", Rule::Fixed, false, L"Mozilla\\Firefox\\Profiles", L"firefox.exe" },
    { L"LibreWolf", L"LibreWolf", Rule::Fixed, false, L"librewolf\\Profiles", L"librewolf.exe" },
    { L"Waterfox", L"Waterfox", Rule::Fixed, false, L"Waterfox\\Profiles", L"waterfox.exe" },
    { L"PaleMoon", L"Pale Moon", Rule::Fixed, false, L"Moonchild Productions\\Pale Moon\\Profiles", L"palemoon.exe" },
    { L"Thunderbird", L"Mozilla Thunderbird", Rule::Fixed, false, L"Thunderbird\\Profiles", L"thunderbird.exe" },
    { L"VSCode", L"Visual Studio Code", Rule::Fixed, true, L"Code\\Cache", L"Code.exe" },
    { L"VSCodeInsiders", L"Visual Studio Code Insiders", Rule::Fixed, true, L"Code - Insiders\\Cache", L"Code - Insiders.exe" },
    { L"Discord", L"Discord", Rule::Fixed, true, L"discord\\Cache", L"Discord.exe" },
    { L"Slack", L"Slack", Rule::Fixed, true, L"Slack\\Cache", L"slack.exe" },
    { L"Teams", L"Microsoft Teams classic", Rule::Fixed, true, L"Microsoft\\Teams\\Cache", L"Teams.exe" },
    { L"Telegram", L"Telegram", Rule::Fixed, true, L"Telegram Desktop\\tdata\\user_data", L"Telegram.exe" },
    { L"Steam", L"Steam", Rule::Fixed, false, L"Steam\\htmlcache\\Cache", L"steam.exe;steamwebhelper.exe" },
    { L"DuckDuckGo", L"DuckDuckGo", Rule::Package, false, L"DuckDuckGo.*|LocalState\\EBWebView\\Default\\Cache", L"DuckDuckGo.exe" },
};

enum class State { Missing, Folder, Linked, Dangling };

struct Entry {
    const AppDef* app = nullptr;
    std::wstring profile;   // Chromium-Profilordner (Default, Profile 1, ...)
    std::wstring path;      // Cache-Ordner
    std::wstring display;
    std::wstring key;       // Zielunterordner, z. B. Chrome-Default
    State state = State::Missing;
    std::wstring target;    // Ziel einer Verknuepfung
    long long size = -1, files = -1;   // -1 = wird noch gezaehlt
};

constexpr UINT WM_SIZE_DONE = WM_APP + 40;
struct SizeResult { unsigned gen; size_t index; long long size, files; };

struct DlgState {
    std::vector<Entry> entries;
    std::shared_ptr<std::atomic<bool>> cancel;
    unsigned gen = 0;
};

// Fuer Tests: RDB_CACHE_TEST_ROOT ersetzt %LOCALAPPDATA% und %APPDATA% durch <Wurzel>\Local bzw. \Roaming
std::wstring TestRoot()
{
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"RDB_CACHE_TEST_ROOT", buf, MAX_PATH);
    return n && n < MAX_PATH ? std::wstring(buf, n) : std::wstring();
}

std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    std::wstring test = TestRoot();
    if (!test.empty()) return test + (IsEqualGUID(id, FOLDERID_LocalAppData) ? L"\\Local" : L"\\Roaming");
    PWSTR p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p)) && p) r = p;
    CoTaskMemFree(p);
    return r;
}

std::wstring Long(const std::wstring& p)   // fuer tiefe Cache-Baeume ueber MAX_PATH
{
    if (p.size() >= 2 && p[1] == L':') return L"\\\\?\\" + p;
    return p;
}

bool IsDir(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(Long(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

// Reparse-Daten (nur die benoetigten Teile von REPARSE_DATA_BUFFER; die Struktur steht nur in ntifs.h)
struct ReparseBuf {
    DWORD tag;
    WORD dataLength, reserved;
    WORD subOffset, subLength, printOffset, printLength;
    // Symlink: danach ULONG Flags, dann PathBuffer; Junction: direkt PathBuffer
};

// Ziel einer Junction bzw. eines Verzeichnis-Symlinks lesen, ohne ihm zu folgen
bool ReadLink(const std::wstring& path, std::wstring& target)
{
    HANDLE h = CreateFileW(Long(path).c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<BYTE> buf(MAXIMUM_REPARSE_DATA_BUFFER_SIZE);
    DWORD got = 0;
    BOOL ok = DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf.data(), (DWORD)buf.size(), &got, nullptr);
    CloseHandle(h);
    if (!ok || got < sizeof(ReparseBuf)) return false;
    const ReparseBuf* rb = (const ReparseBuf*)buf.data();
    const BYTE* names;
    bool relative = false;
    if (rb->tag == IO_REPARSE_TAG_MOUNT_POINT) {
        names = buf.data() + sizeof(ReparseBuf);
    } else if (rb->tag == IO_REPARSE_TAG_SYMLINK) {
        relative = (*(const ULONG*)(buf.data() + sizeof(ReparseBuf)) & 1) != 0;
        names = buf.data() + sizeof(ReparseBuf) + sizeof(ULONG);
    } else {
        return false;
    }
    auto name = [&](WORD off, WORD len) {
        if ((size_t)(names - buf.data()) + off + len > got) return std::wstring();
        return std::wstring((const wchar_t*)(names + off), len / sizeof(wchar_t));
    };
    std::wstring t = name(rb->printOffset, rb->printLength);
    if (t.empty()) {
        t = name(rb->subOffset, rb->subLength);
        if (t.rfind(L"\\??\\UNC\\", 0) == 0) t = L"\\\\" + t.substr(8);
        else if (t.rfind(L"\\??\\", 0) == 0) t = t.substr(4);
    }
    if (relative) {
        size_t p = path.find_last_of(L'\\');
        if (p != std::wstring::npos) t = path.substr(0, p + 1) + t;
    }
    while (t.size() > 3 && t.back() == L'\\') t.pop_back();
    target = t;
    return !t.empty();
}

// Junction anlegen: leerer Ordner + Reparse-Punkt IO_REPARSE_TAG_MOUNT_POINT (ohne Administratorrechte)
bool CreateJunction(const std::wstring& link, const std::wstring& target, DWORD& err)
{
    if (!CreateDirectoryW(Long(link).c_str(), nullptr)) { err = GetLastError(); return false; }
    std::wstring sub = L"\\??\\" + target, print = target;
    WORD subBytes = (WORD)(sub.size() * sizeof(wchar_t)), printBytes = (WORD)(print.size() * sizeof(wchar_t));
    std::vector<BYTE> buf(sizeof(ReparseBuf) + subBytes + sizeof(wchar_t) + printBytes + sizeof(wchar_t));
    ReparseBuf* rb = (ReparseBuf*)buf.data();
    rb->tag = IO_REPARSE_TAG_MOUNT_POINT;
    rb->dataLength = (WORD)(buf.size() - 8);   // ohne tag/dataLength/reserved
    rb->subOffset = 0;
    rb->subLength = subBytes;
    rb->printOffset = subBytes + sizeof(wchar_t);
    rb->printLength = printBytes;
    BYTE* names = buf.data() + sizeof(ReparseBuf);
    memcpy(names, sub.data(), subBytes);
    memcpy(names + rb->printOffset, print.data(), printBytes);
    HANDLE h = CreateFileW(Long(link).c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    bool ok = false;
    if (h != INVALID_HANDLE_VALUE) {
        DWORD ret = 0;
        ok = DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), (DWORD)buf.size(), nullptr, 0, &ret, nullptr) != FALSE;
        if (!ok) err = GetLastError();
        CloseHandle(h);
    } else {
        err = GetLastError();
    }
    if (!ok) RemoveDirectoryW(Long(link).c_str());
    return ok;
}

// Ordnerinhalt rekursiv loeschen, ohne Verknuepfungen zu folgen (Verknuepfungen selbst werden nur entfernt).
// removeSelf: den Ordner selbst ebenfalls entfernen. Liefert false, wenn etwas nicht geloescht werden konnte.
bool DeleteTree(const std::wstring& dir, bool removeSelf, DWORD& err)
{
    bool ok = true;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(Long(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            std::wstring p = dir + L"\\" + fd.cFileName;
            bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(Long(p).c_str(), FILE_ATTRIBUTE_NORMAL);
            BOOL r;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) r = isDir ? RemoveDirectoryW(Long(p).c_str()) : DeleteFileW(Long(p).c_str());
            else if (isDir) r = DeleteTree(p, true, err);
            else r = DeleteFileW(Long(p).c_str());
            if (!r) {
                if (ok && !err) err = GetLastError();
                ok = false;
            }
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    if (removeSelf && ok && !RemoveDirectoryW(Long(dir).c_str())) {
        err = GetLastError();
        ok = false;
    }
    return ok;
}

void DirSize(const std::wstring& dir, long long& size, long long& files, const std::atomic<bool>& cancel)
{
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW(Long(dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (cancel) break;
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;   // nicht in fremde Ziele zaehlen
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            DirSize(dir + L"\\" + fd.cFileName, size, files, cancel);
        } else {
            size += ((long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            files++;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

bool CreateDirs(const std::wstring& path)
{
    if (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return true;
    size_t p = path.find_last_of(L'\\');
    if (p == std::wstring::npos || p < 3) return false;
    return CreateDirs(path.substr(0, p)) && (CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
}

std::wstring SafeName(std::wstring s)
{
    for (auto& c : s) if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    return s;
}

void Detect(Entry& e)
{
    e.target.clear();
    e.size = e.files = -1;
    DWORD a = GetFileAttributesW(Long(e.path).c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        e.state = State::Missing;
    } else if (a & FILE_ATTRIBUTE_REPARSE_POINT) {
        if (ReadLink(e.path, e.target)) e.state = IsDir(e.target) ? State::Linked : State::Dangling;
        else e.state = State::Dangling;
    } else {
        e.state = State::Folder;
    }
}

// Alle installierten Programme und Profile mit Cache-Ordner
std::vector<Entry> Enumerate()
{
    std::vector<Entry> out;
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData), roaming = KnownFolder(FOLDERID_RoamingAppData);
    for (const AppDef& app : kApps) {
        const std::wstring& base = app.roaming ? roaming : local;
        if (base.empty()) continue;
        auto add = [&](const std::wstring& path, const std::wstring& profile) {
            Entry e;
            e.app = &app;
            e.profile = profile;
            e.path = path;
            e.display = profile.empty() ? std::wstring(app.name) : std::wstring(app.name) + L" – " + profile;
            e.key = profile.empty() ? std::wstring(app.key) : std::wstring(app.key) + L"-" + SafeName(profile);
            Detect(e);
            out.push_back(std::move(e));
        };
        if (app.rule == Rule::Chromium) {
            std::wstring ud = base + L"\\" + app.path;
            if (!IsDir(ud)) continue;
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileExW((ud + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
            if (f == INVALID_HANDLE_VALUE) continue;
            std::vector<std::wstring> profiles;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == L'.') continue;
                if (!_wcsicmp(fd.cFileName, L"System Profile")) continue;
                std::wstring pd = ud + L"\\" + fd.cFileName;
                // echtes Profil: hat "Preferences" (der Cache-Ordner selbst kann fehlen oder ins Leere zeigen)
                if (GetFileAttributesW((pd + L"\\Preferences").c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                profiles.push_back(fd.cFileName);
            } while (FindNextFileW(f, &fd));
            FindClose(f);
            std::sort(profiles.begin(), profiles.end(), [](const std::wstring& a, const std::wstring& b) {
                if (!_wcsicmp(a.c_str(), L"Default")) return _wcsicmp(b.c_str(), L"Default") != 0;
                if (!_wcsicmp(b.c_str(), L"Default")) return false;
                return StrCmpLogicalW(a.c_str(), b.c_str()) < 0;
            });
            for (auto& p : profiles) add(ud + L"\\" + p + L"\\Cache", p);
        } else if (app.rule == Rule::Fixed) {
            std::wstring p = base + L"\\" + app.path;
            DWORD a = GetFileAttributesW(Long(p).c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) add(p, L"");
        } else {   // Store-App: erster passender Paketordner
            std::wstring spec = app.path;
            size_t bar = spec.find(L'|');
            std::wstring mask = spec.substr(0, bar), sub = spec.substr(bar + 1);
            WIN32_FIND_DATAW fd;
            HANDLE f = FindFirstFileExW((base + L"\\Packages\\" + mask).c_str(), FindExInfoBasic, &fd, FindExSearchLimitToDirectories, nullptr, 0);
            if (f == INVALID_HANDLE_VALUE) continue;
            std::wstring pkg = fd.cFileName;
            FindClose(f);
            std::wstring p = base + L"\\Packages\\" + pkg + L"\\" + sub;
            DWORD a = GetFileAttributesW(Long(p).c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) add(p, L"");
        }
    }
    return out;
}

// Laufende Prozesse der ausgewaehlten Programme (auch im Hintergrund, z. B. Edge-Schnellstart)
std::vector<std::wstring> RunningApps(const std::vector<const Entry*>& sel)
{
    std::vector<std::wstring> running;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return running;
    std::vector<std::wstring> procs;
    PROCESSENTRY32W pe{ sizeof pe };
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) procs.push_back(pe.szExeFile);
    CloseHandle(snap);
    for (const Entry* e : sel) {
        for (auto& exe : Split(e->app->exes, L';')) {
            bool found = std::any_of(procs.begin(), procs.end(), [&](const std::wstring& p) { return !_wcsicmp(p.c_str(), exe.c_str()); });
            std::wstring line = std::wstring(e->app->name) + L" (" + exe + L")";
            if (found && std::find(running.begin(), running.end(), line) == running.end()) running.push_back(line);
        }
    }
    return running;
}

// Ordnerliste eines Laufwerks aendern ("Ordner anlegen"), damit das Ziel nach jedem Einhaengen da ist
bool UpdateFolders(wchar_t letter, const std::wstring& add, const std::wstring& remove, std::wstring& err)
{
    const KV* kv = FindVolume(letter);
    if (!kv) return true;
    VolumeConfig c;
    if (!VolumeConfig::FromKV(*kv, c, err)) return false;
    std::vector<std::wstring> list;
    bool changed = false;
    for (auto& f : Split(c.folders, L';')) {
        std::wstring t = Trim(f);
        if (t.empty()) continue;
        if (!remove.empty() && !_wcsicmp(t.c_str(), remove.c_str())) { changed = true; continue; }
        list.push_back(t);
    }
    if (!add.empty() && std::none_of(list.begin(), list.end(), [&](const std::wstring& t) { return !_wcsicmp(t.c_str(), add.c_str()); })) {
        list.push_back(add);
        changed = true;
    }
    if (!changed) return true;
    std::wstring joined;
    for (auto& t : list) joined += (joined.empty() ? L"" : L";") + t;
    c.folders = joined;
    Message req, resp;
    req.cmd = L"UPDATE";
    req.args = c.ToKV();
    if (!PipeCall(req, resp, &err)) return false;
    // FindVolume muss die neue Liste sehen, falls gleich noch ein Eintrag folgt
    for (auto& [name, k] : g_last.sections)
        if (!name.empty() && towupper(name[0]) == towupper(letter)) k[L"folders"] = joined;
    return true;
}

// Hat RamDiskBox dieses Ziel fuer genau dieses Programm angelegt? Letzter Teil = eigener Ordnername
// (z. B. Chrome-Default) und der Pfad steht in der Ordnerliste eines RamDiskBox-Laufwerks. So wird nie
// ein gemeinsam genutzter Ordner wie S:\Temp geleert.
bool OwnTarget(const std::wstring& target, const std::wstring& key, wchar_t& letter, std::wstring& rel)
{
    if (target.size() < 4 || target[1] != L':' || target[2] != L'\\') return false;
    size_t slash = target.find_last_of(L'\\');
    if (slash == std::wstring::npos || _wcsicmp(target.c_str() + slash + 1, key.c_str()) != 0) return false;
    letter = (wchar_t)towupper(target[0]);
    rel = target.substr(3);
    const KV* kv = FindVolume(letter);
    if (!kv) return false;
    auto it = kv->find(L"folders");
    if (it == kv->end()) return false;
    for (auto& f : Split(it->second, L';')) if (!_wcsicmp(Trim(f).c_str(), rel.c_str())) return true;
    return false;
}

std::wstring Win32Text(DWORD e) { return Win32Message(e ? e : ERROR_GEN_FAILURE); }

// ----------------------------------------------------------------------- Dialog

std::wstring StateText(const Entry& e)
{
    switch (e.state) {
    case State::Missing: return T(S_CD_NOCACHE);
    case State::Linked: return Fmt(T(S_CD_LINKED), e.target.c_str());
    case State::Dangling: return Fmt(T(S_CD_DANGLING), e.target.empty() ? L"?" : e.target.c_str());
    default:
        if (e.size < 0) return T(S_CD_COUNTING);
        return Fmt(T(S_CD_SIZE), FormatSize((unsigned long long)e.size).c_str(), std::to_wstring(e.files).c_str());
    }
}

void StartSizes(HWND dlg, DlgState* st)
{
    if (st->cancel) *st->cancel = true;
    st->cancel = std::make_shared<std::atomic<bool>>(false);
    st->gen++;
    std::vector<std::pair<size_t, std::wstring>> work;
    for (size_t i = 0; i < st->entries.size(); i++)
        if (st->entries[i].state == State::Folder) work.push_back({ i, st->entries[i].path });
    if (work.empty()) return;
    auto cancel = st->cancel;
    unsigned gen = st->gen;
    std::thread([dlg, cancel, gen, work] {
        for (auto& [i, path] : work) {
            long long size = 0, files = 0;
            DirSize(path, size, files, *cancel);
            if (*cancel) return;
            auto* r = new SizeResult{ gen, i, size, files };
            if (!PostMessageW(dlg, WM_SIZE_DONE, 0, (LPARAM)r)) { delete r; return; }
        }
    }).detach();
}

void FillList(HWND dlg, DlgState* st, bool rescan)
{
    HWND lv = GetDlgItem(dlg, IDC_CACHE_LIST);
    std::vector<std::wstring> selected;
    for (int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(lv, i, LVNI_SELECTED))
        if (i < (int)st->entries.size()) selected.push_back(st->entries[i].path);
    if (rescan) st->entries = Enumerate();
    else for (auto& e : st->entries) Detect(e);
    ListView_DeleteAllItems(lv);
    for (size_t i = 0; i < st->entries.size(); i++) {
        const Entry& e = st->entries[i];
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.iItem = (int)i;
        it.pszText = (LPWSTR)e.display.c_str();
        ListView_InsertItem(lv, &it);
        std::wstring s = StateText(e);
        ListView_SetItemText(lv, (int)i, 1, (LPWSTR)s.c_str());
        ListView_SetItemText(lv, (int)i, 2, (LPWSTR)e.path.c_str());
        if (std::find(selected.begin(), selected.end(), e.path) != selected.end() || (rescan && !TestRoot().empty()))
            ListView_SetItemState(lv, (int)i, LVIS_SELECTED, LVIS_SELECTED);   // im Test: alles ausgewaehlt
    }
    if (st->entries.empty()) {
        LVITEMW it{};
        it.mask = LVIF_TEXT;
        it.pszText = (LPWSTR)T(S_CD_NONE_FOUND);
        ListView_InsertItem(lv, &it);
    }
    StartSizes(dlg, st);
}

wchar_t SelectedDrive(HWND dlg)
{
    HWND cb = GetDlgItem(dlg, IDC_CACHE_DRIVE);
    int i = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
    return i < 0 ? 0 : (wchar_t)SendMessageW(cb, CB_GETITEMDATA, i, 0);
}

std::wstring BaseFolder(HWND dlg)
{
    wchar_t buf[MAX_PATH] = {};
    GetDlgItemTextW(dlg, IDC_CACHE_FOLDER, buf, MAX_PATH);
    std::wstring f = Trim(buf);
    while (!f.empty() && (f[0] == L'\\' || f[0] == L'/')) f.erase(0, 1);
    while (!f.empty() && (f.back() == L'\\' || f.back() == L'/')) f.pop_back();
    for (auto& c : f) if (c == L'/') c = L'\\';
    return f.empty() ? L"Cache" : f;
}

void UpdateHint(HWND dlg)
{
    wchar_t l = SelectedDrive(dlg);
    std::wstring t = l ? Fmt(T(S_CD_HINT), (std::wstring(1, l) + L":\\" + BaseFolder(dlg) + L"\\…").c_str()) : std::wstring(T(S_CD_NO_DRIVE));
    SetDlgItemTextW(dlg, IDC_CACHE_HINT, t.c_str());
}

std::vector<size_t> Selection(HWND dlg, DlgState* st)
{
    std::vector<size_t> sel;
    HWND lv = GetDlgItem(dlg, IDC_CACHE_LIST);
    for (int i = ListView_GetNextItem(lv, -1, LVNI_SELECTED); i >= 0; i = ListView_GetNextItem(lv, i, LVNI_SELECTED))
        if (i < (int)st->entries.size()) sel.push_back((size_t)i);
    return sel;
}

bool CheckRunning(HWND dlg, DlgState* st, const std::vector<size_t>& sel)
{
    std::vector<const Entry*> es;
    for (size_t i : sel) es.push_back(&st->entries[i]);
    auto running = RunningApps(es);
    if (running.empty()) return true;
    std::wstring list;
    for (auto& r : running) list += L"\n   " + r;
    MsgBox(dlg, Fmt(T(S_CD_RUNNING), list.c_str()), MB_ICONWARNING);
    return false;
}

// Altes, von RamDiskBox angelegtes Ziel aufraeumen (Ordnerliste und Inhalt)
void DropOwnTarget(const std::wstring& target, const std::wstring& key)
{
    wchar_t l;
    std::wstring rel, err;
    if (!OwnTarget(target, key, l, rel)) return;
    UpdateFolders(l, L"", rel, err);
    DWORD e = 0;
    if (IsDir(target)) DeleteTree(target, true, e);
}

void DoMove(HWND dlg, DlgState* st)
{
    auto sel = Selection(dlg, st);
    if (sel.empty()) { MsgBox(dlg, T(S_CD_SELECT), MB_ICONINFORMATION); return; }
    wchar_t l = SelectedDrive(dlg);
    if (!l) { MsgBox(dlg, T(S_CD_NO_DRIVE), MB_ICONWARNING); return; }
    std::wstring base = BaseFolder(dlg);
    if (base.find_first_of(L":*?\"<>|") != std::wstring::npos) { MsgBox(dlg, Fmt(T(S_CD_BADFOLDER), base.c_str()), MB_ICONWARNING); return; }
    if (!CheckRunning(dlg, st, sel)) return;
    std::wstring root = std::wstring(1, l) + L":\\" + base;
    if (MsgBox(dlg, Fmt(T(S_CD_CONFIRM_MOVE), (int)sel.size(), root.c_str()), MB_ICONQUESTION | MB_OKCANCEL) != IDOK) return;
    std::wstring report;
    int okCount = 0;
    for (size_t i : sel) {
        Entry& e = st->entries[i];
        Detect(e);
        std::wstring target = root + L"\\" + e.key;
        std::wstring rel = base + L"\\" + e.key;
        if ((e.state == State::Linked || e.state == State::Dangling) && !_wcsicmp(e.target.c_str(), target.c_str())) {
            okCount++;   // zeigt schon dorthin
            continue;
        }
        DWORD err = 0;
        std::wstring why;
        if (!CreateDirs(target)) {
            why = Win32Text(GetLastError());
        } else {
            std::wstring oldTarget;
            if (e.state == State::Linked || e.state == State::Dangling) {
                // nur die Verknuepfung entfernen, nie ihr Ziel (kann ein fremder, gemeinsam genutzter Ordner sein)
                oldTarget = e.target;
                if (!RemoveDirectoryW(Long(e.path).c_str())) why = Win32Text(GetLastError());
            } else if (e.state == State::Folder) {
                if (!DeleteTree(e.path, true, err)) why = Fmt(T(S_CD_INUSE), Win32Text(err).c_str());
            }
            if (why.empty() && !CreateJunction(e.path, target, err)) {
                why = Win32Text(err);
                CreateDirectoryW(Long(e.path).c_str(), nullptr);   // wenigstens wieder ein normaler Cache-Ordner
            }
            if (why.empty()) {
                std::wstring uerr;
                if (!UpdateFolders(l, rel, L"", uerr)) report += L"\n" + Fmt(T(S_CD_FOLDERS_FAIL), e.display.c_str(), uerr.c_str());
                if (!oldTarget.empty()) DropOwnTarget(oldTarget, e.key);
                okCount++;
            }
        }
        if (!why.empty()) report += L"\n" + Fmt(T(S_CD_FAILED_ITEM), e.display.c_str(), why.c_str());
    }
    RequestRefresh();
    FillList(dlg, st, false);
    MsgBox(dlg, Fmt(T(S_CD_DONE), okCount, (int)sel.size()) + report, report.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
}

void DoRestore(HWND dlg, DlgState* st)
{
    std::vector<size_t> sel;
    for (size_t i : Selection(dlg, st))
        if (st->entries[i].state == State::Linked || st->entries[i].state == State::Dangling) sel.push_back(i);
    if (sel.empty()) { MsgBox(dlg, T(S_CD_SELECT_LINKED), MB_ICONINFORMATION); return; }
    if (!CheckRunning(dlg, st, sel)) return;
    if (MsgBox(dlg, Fmt(T(S_CD_CONFIRM_RESTORE), (int)sel.size()), MB_ICONQUESTION | MB_OKCANCEL) != IDOK) return;
    std::wstring report;
    int okCount = 0;
    for (size_t i : sel) {
        Entry& e = st->entries[i];
        Detect(e);
        if (e.state != State::Linked && e.state != State::Dangling) { okCount++; continue; }
        std::wstring oldTarget = e.target;
        if (!RemoveDirectoryW(Long(e.path).c_str())) {   // nur die Verknuepfung
            report += L"\n" + Fmt(T(S_CD_FAILED_ITEM), e.display.c_str(), Win32Text(GetLastError()).c_str());
            continue;
        }
        if (!CreateDirectoryW(Long(e.path).c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            report += L"\n" + Fmt(T(S_CD_FAILED_ITEM), e.display.c_str(), Win32Text(GetLastError()).c_str());
            continue;
        }
        DropOwnTarget(oldTarget, e.key);   // nur eigene Ziele dieses Programms werden geleert
        okCount++;
    }
    RequestRefresh();
    FillList(dlg, st, false);
    MsgBox(dlg, Fmt(T(S_CD_RESTORED), okCount, (int)sel.size()) + report, report.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
}

INT_PTR CALLBACK CacheProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    INT_PTR themed;
    if (ThemeDialogMessage(dlg, msg, wp, lp, &themed)) return themed;
    DlgState* st = (DlgState*)GetWindowLongPtrW(dlg, DWLP_USER);
    switch (msg) {
    case WM_INITDIALOG: {
        st = new DlgState;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)st);
        SetWindowTextW(dlg, T(S_CD_TITLE));
        SetDlgItemTextW(dlg, IDC_CACHE_INFO, T(S_CD_INFO));
        SetDlgItemTextW(dlg, IDC_L_CACHE_TARGET, T(S_CD_TARGET));
        SetDlgItemTextW(dlg, IDC_CACHE_MOVE, T(S_CD_MOVE));
        SetDlgItemTextW(dlg, IDC_CACHE_RESTORE, T(S_CD_RESTORE));
        SetDlgItemTextW(dlg, IDC_CACHE_REFRESH, T(S_CD_REFRESH));
        SetDlgItemTextW(dlg, IDCANCEL, T(S_CD_CLOSE));
        SetDlgItemTextW(dlg, IDC_CACHE_FOLDER, L"Cache");
        HWND lv = GetDlgItem(dlg, IDC_CACHE_LIST);
        ListView_SetExtendedListViewStyle(lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        RECT rc;
        GetClientRect(lv, &rc);
        int w = rc.right - GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(dlg));
        const Str cols[] = { S_CD_COL_APP, S_CD_COL_STATE, S_CD_COL_PATH };
        const int part[] = { 28, 36, 36 };
        for (int i = 0; i < 3; i++) {
            LVCOLUMNW c{};
            c.mask = LVCF_TEXT | LVCF_WIDTH;
            c.pszText = (LPWSTR)T(cols[i]);
            c.cx = w * part[i] / 100;
            ListView_InsertColumn(lv, i, &c);
        }
        // Ziel: eingehaengte RamDiskBox-Laufwerke, das mit dem meisten freien Platz vorausgewaehlt
        HWND cb = GetDlgItem(dlg, IDC_CACHE_DRIVE);
        int best = -1;
        unsigned long long bestFree = 0;
        for (auto& [name, kv] : g_last.sections) {
            auto it = kv.find(L"state");
            if (name.empty() || it == kv.end() || it->second == L"dismounted") continue;
            std::wstring t = name + L":";
            int idx = (int)SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)t.c_str());
            SendMessageW(cb, CB_SETITEMDATA, idx, name[0]);
            auto get = [&](const wchar_t* k) { auto f = kv.find(k); return f == kv.end() ? 0ull : ToU64(f->second); };
            unsigned long long cap = get(L"capacity") ? get(L"capacity") : get(L"size"), used = get(L"used");
            unsigned long long freeB = cap > used ? cap - used : 0;
            if (best < 0 || freeB > bestFree) { best = idx; bestFree = freeB; }
        }
        if (best >= 0) SendMessageW(cb, CB_SETCURSEL, best, 0);
        EnableWindow(GetDlgItem(dlg, IDC_CACHE_MOVE), best >= 0);
        UpdateHint(dlg);
        FillList(dlg, st, true);
        return TRUE;
    }
    case WM_SIZE_DONE: {
        SizeResult* r = (SizeResult*)lp;
        if (st && r->gen == st->gen && r->index < st->entries.size()) {
            Entry& e = st->entries[r->index];
            e.size = r->size;
            e.files = r->files;
            std::wstring s = StateText(e);
            ListView_SetItemText(GetDlgItem(dlg, IDC_CACHE_LIST), (int)r->index, 1, (LPWSTR)s.c_str());
        }
        delete r;
        return TRUE;
    }
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->idFrom == IDC_CACHE_LIST && nh->code == NM_CUSTOMDRAW && st) {
            // Zustand farbig: gruen = umgeleitet, rot = Ziel fehlt
            NMLVCUSTOMDRAW* cd = (NMLVCUSTOMDRAW*)lp;
            LRESULT r = CDRF_DODEFAULT;
            if (cd->nmcd.dwDrawStage == CDDS_PREPAINT) r = CDRF_NOTIFYITEMDRAW;
            else if (cd->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) r = CDRF_NOTIFYSUBITEMDRAW;
            else if (cd->nmcd.dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
                size_t i = cd->nmcd.dwItemSpec;
                bool dark = IsDark();
                cd->clrText = dark ? Colors().text : GetSysColor(COLOR_WINDOWTEXT);
                if (cd->iSubItem == 1 && i < st->entries.size()) {
                    if (st->entries[i].state == State::Linked) cd->clrText = dark ? RGB(110, 210, 130) : RGB(0, 128, 0);
                    else if (st->entries[i].state == State::Dangling) cd->clrText = dark ? RGB(240, 120, 110) : RGB(196, 40, 30);
                }
                r = CDRF_NEWFONT;
            }
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, r);
            return TRUE;
        }
        break;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CACHE_MOVE: DoMove(dlg, st); return TRUE;
        case IDC_CACHE_RESTORE: DoRestore(dlg, st); return TRUE;
        case IDC_CACHE_REFRESH: FillList(dlg, st, true); return TRUE;
        case IDC_CACHE_DRIVE:
            if (HIWORD(wp) == CBN_SELCHANGE) UpdateHint(dlg);
            return TRUE;
        case IDC_CACHE_FOLDER:
            if (HIWORD(wp) == EN_CHANGE) UpdateHint(dlg);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    case WM_DESTROY:
        if (st) {
            if (st->cancel) *st->cancel = true;   // Zaehl-Thread beenden; spaete Ergebnisse verwirft PostMessage
            delete st;
            SetWindowLongPtrW(dlg, DWLP_USER, 0);
        }
        break;
    }
    return FALSE;
}

} // namespace

void DlgCache(HWND parent)
{
    DialogBoxParamW(g_inst, MAKEINTRESOURCEW(IDD_CACHE), parent, CacheProc, 0);
}

} // namespace gui
