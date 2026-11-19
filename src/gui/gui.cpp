// RamDiskBox.exe - Oberflaeche
#include "gui.h"
#include "../common/version.h"
#include <shellapi.h>
#include <commdlg.h>
#include <uxtheme.h>
#include <shlwapi.h>
#include <windowsx.h>
#include <algorithm>
#include <thread>
#include <functional>
#include <cstdarg>
#include <cwctype>

#pragma comment(linker, "/ENTRY:wWinMainCRTStartup")

using namespace rdb;

namespace gui {

HINSTANCE g_inst = nullptr;
HWND g_main = nullptr;
GuiSettings g_set;
Message g_last;
bool g_online = false;

namespace {

enum : UINT {
    WM_APP_TRAY = WM_APP + 1,
    WM_APP_JOBDONE,
    WM_APP_SHOW,
    WM_APP_REFRESH,
};

enum : int {
    CMD_NEW = 40001, CMD_EDIT, CMD_REMOVE, CMD_MOUNT, CMD_DISMOUNT, CMD_SAVE, CMD_SAVEAS, CMD_EXPLORE,
    CMD_TEMP, CMD_SETTINGS, CMD_LOG, CMD_RECONNECT, CMD_QUITPORTABLE, CMD_RESETSTATS,
    CMD_ABOUT, CMD_KOFI, CMD_EXIT, CMD_SHOW, CMD_REFRESH,
    CMD_TRAY_TOGGLE = 41000,   // + Buchstabe
};

constexpr int ID_LIST = 5100;
constexpr int ID_STATUS = 5200;
constexpr int ID_BTN_FIRST = 5001;
constexpr UINT TIMER_REFRESH = 1;

const int kBtnCmd[] = { CMD_NEW, CMD_EDIT, CMD_REMOVE, CMD_MOUNT, CMD_DISMOUNT, CMD_SAVE, CMD_EXPLORE };
const Str kBtnText[] = { S_NEW, S_EDIT, S_REMOVE, S_MOUNT, S_DISMOUNT, S_SAVE, S_OPEN };
constexpr int kBtnCount = 7;

HWND g_list = nullptr, g_status = nullptr;
HWND g_btn[kBtnCount] = {};
HFONT g_font = nullptr;
UINT g_dpi = 96;
HACCEL g_accel = nullptr;
NOTIFYICONDATAW g_nid{};
UINT g_taskbarCreated = 0;
bool g_trayAdded = false;
int g_busy = 0;
std::wstring g_busyText;
std::wstring g_flash;           // kurze Meldung in der Statuszeile
unsigned long long g_flashUntil = 0;
bool g_exiting = false;

int S(int px) { return MulDiv(px, (int)g_dpi, 96); }

std::wstring Get(const KV& kv, const wchar_t* k)
{
    auto it = kv.find(k);
    return it == kv.end() ? std::wstring() : it->second;
}

// ----------------------------------------------------------------------- Hintergrundauftraege

struct Job {
    Message req, resp;
    bool ok = false;
    std::wstring err;
    std::function<void(Job&)> done;
};

void UpdateButtons();
void UpdateStatus();

void RunAsync(const Message& req, const std::wstring& busy, std::function<void(Job&)> done)
{
    Job* j = new Job;
    j->req = req;
    j->done = std::move(done);
    g_busy++;
    g_busyText = busy;
    UpdateButtons();
    UpdateStatus();
    std::thread([j] {
        j->ok = PipeCall(j->req, j->resp, &j->err, 30 * 60 * 1000);
        PostMessageW(g_main, WM_APP_JOBDONE, 0, (LPARAM)j);
    }).detach();
}

void Flash(const std::wstring& text)
{
    g_flash = text;
    g_flashUntil = GetTickCount64() + 6000;
    UpdateStatus();
}

// ----------------------------------------------------------------------- Auswahl/Zustand

wchar_t SelectedLetter()
{
    int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
    if (i < 0) return 0;
    LVITEMW it{};
    it.mask = LVIF_PARAM;
    it.iItem = i;
    ListView_GetItem(g_list, &it);
    return (wchar_t)it.lParam;
}

std::wstring StateOf(const KV* kv) { return kv ? Get(*kv, L"state") : L""; }

bool TempOnDrive(wchar_t letter)
{
    wchar_t buf[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"TEMP", buf, MAX_PATH);
    std::wstring reg = CurrentTempDir();
    auto onDrive = [&](const std::wstring& p) { return p.size() >= 2 && p[1] == L':' && towupper(p[0]) == towupper(letter); };
    return (n && onDrive(std::wstring(buf, n))) || onDrive(reg);
}

// ----------------------------------------------------------------------- Liste

void SetColumns()
{
    const Str cols[] = { S_COL_DRIVE, S_COL_LABEL, S_COL_STATE, S_COL_SIZE, S_COL_USED, S_COL_RAM, S_COL_COMP, S_COL_FILES, S_COL_IMAGE, S_COL_LASTSAVE };
    const int widths[] = { 64, 100, 85, 70, 70, 70, 145, 55, 150, 120 };
    const int fmt[] = { LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_RIGHT, LVCFMT_RIGHT, LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_LEFT, LVCFMT_LEFT };
    HWND hdr = ListView_GetHeader(g_list);
    int have = hdr ? Header_GetItemCount(hdr) : 0;
    for (int i = 0; i < 10; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH;
        c.pszText = (LPWSTR)T(cols[i]);
        c.fmt = fmt[i];
        c.cx = S(widths[i]);
        if (i < have) {
            c.mask = LVCF_TEXT;  // Breite des Benutzers behalten
            ListView_SetColumn(g_list, i, &c);
        } else {
            ListView_InsertColumn(g_list, i, &c);
        }
    }
}

void SetCell(int row, int col, const std::wstring& text)
{
    wchar_t cur[512] = {};
    ListView_GetItemText(g_list, row, col, cur, 511);
    if (text != cur) ListView_SetItemText(g_list, row, col, (LPWSTR)text.c_str());
}

void UpdateList()
{
    std::vector<wchar_t> letters;
    for (auto& [name, kv] : g_last.sections) if (!name.empty()) letters.push_back(name[0]);
    int count = ListView_GetItemCount(g_list);
    bool same = count == (int)letters.size();
    for (int i = 0; same && i < count; i++) {
        LVITEMW it{};
        it.mask = LVIF_PARAM;
        it.iItem = i;
        ListView_GetItem(g_list, &it);
        if ((wchar_t)it.lParam != letters[i]) same = false;
    }
    if (!same) {
        wchar_t sel = SelectedLetter();
        ListView_DeleteAllItems(g_list);
        for (size_t i = 0; i < letters.size(); i++) {
            std::wstring t = std::wstring(1, letters[i]) + L":";
            LVITEMW it{};
            it.mask = LVIF_TEXT | LVIF_PARAM;
            it.iItem = (int)i;
            it.pszText = (LPWSTR)t.c_str();
            it.lParam = letters[i];
            ListView_InsertItem(g_list, &it);
        }
        int pick = -1;
        for (size_t i = 0; i < letters.size(); i++) if (letters[i] == sel) pick = (int)i;
        if (pick < 0 && !letters.empty()) pick = 0;
        if (pick >= 0) ListView_SetItemState(g_list, pick, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
    for (size_t i = 0; i < g_last.sections.size(); i++) {
        const KV& kv = g_last.sections[i].second;
        int row = (int)i;
        std::wstring st = Get(kv, L"state");
        bool mounted = st == L"mounted" || st == L"saving";
        std::wstring label = mounted && !Get(kv, L"livelabel").empty() ? Get(kv, L"livelabel") : Get(kv, L"label");
        SetCell(row, 1, label);
        std::wstring stText = st == L"mounted" ? T(S_ST_MOUNTED) : st == L"saving" ? T(S_ST_SAVING) : T(S_ST_DISMOUNTED);
        if (!Get(kv, L"lasterror").empty()) stText += L" ⚠";
        SetCell(row, 2, stText);
        SetCell(row, 3, FormatSize(ToU64(Get(kv, L"size"))));
        SetCell(row, 4, mounted ? FormatSize(ToU64(Get(kv, L"used"))) : L"");
        SetCell(row, 5, mounted ? FormatSize(ToU64(Get(kv, L"memory"))) : L"");
        {
            int lvl = ParseCompress(Get(kv, L"compress"), 0);
            const Str names[] = { S_COMP_OFF, S_COMP_FAST, S_COMP_NORMAL, S_COMP_MAX };
            std::wstring ct = T(names[lvl < 0 || lvl > 3 ? 0 : lvl]);
            unsigned long long raw = ToU64(Get(kv, L"compraw")), stored = ToU64(Get(kv, L"compstored"));
            if (mounted && raw > 0) ct = Fmt(T(S_COMP_SAVED), ct.c_str(), (int)((raw > stored ? raw - stored : 0) * 100 / raw));
            SetCell(row, 6, ct);
        }
        SetCell(row, 7, mounted ? Get(kv, L"files") : L"");
        SetCell(row, 8, Get(kv, L"image").empty() ? T(S_NO_IMAGE) : Get(kv, L"image"));
        SetCell(row, 9, DisplayTime(Get(kv, L"lastsave")));
    }
}

// ----------------------------------------------------------------------- Status/Knoepfe/Infobereich

void UpdateStatus()
{
    if (!g_status) return;
    std::wstring p0, p1, p2, p3;
    if (g_online) {
        p0 = Fmt(g_last.Get(L"mode") == L"service" ? T(S_SVC_SERVICE) : T(S_SVC_PORTABLE), g_last.Get(L"version").c_str());
        p1 = Fmt(T(S_RAM), FormatSize(ToU64(g_last.Get(L"ramfree"))).c_str(), FormatSize(ToU64(g_last.Get(L"ramtotal"))).c_str());
        p2 = Fmt(T(S_IO), FormatSize(ToU64(g_last.Get(L"speedread"))).c_str(), FormatSize(ToU64(g_last.Get(L"speedwrite"))).c_str());
        p3 = Fmt(T(S_TOTAL), FormatSize(ToU64(g_last.Get(L"liferead"))).c_str(), FormatSize(ToU64(g_last.Get(L"lifewritten"))).c_str());
    } else {
        p0 = T(S_SVC_NONE);
    }
    // Warnung des ausgewaehlten Laufwerks (z. B. Image defekt, Defender-Ausnahme nicht moeglich)
    if (g_online && g_list) {
        wchar_t sel = SelectedLetter();
        if (const KV* kv = sel ? FindVolume(sel) : nullptr) {
            std::wstring le = Get(*kv, L"lasterror");
            if (!le.empty()) p0 = L"⚠ " + std::wstring(1, sel) + L": " + le;
        }
    }
    if (g_busy > 0) p0 = g_busyText;
    else if (!g_flash.empty() && GetTickCount64() < g_flashUntil) p0 = g_flash;
    // Teilbreiten nach Textbreite
    HDC dc = GetDC(g_status);
    HGDIOBJ of = SelectObject(dc, g_font);
    auto tw = [&](const std::wstring& s) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
        return (int)sz.cx + S(20);
    };
    RECT src{};
    GetClientRect(g_status, &src);
    int w0 = std::min(std::max(tw(p0), S(200)), std::max((int)src.right * 55 / 100, S(200))), w1 = tw(p1), w2 = tw(p2);
    SelectObject(dc, of);
    ReleaseDC(g_status, dc);
    int parts[4] = { w0, w0 + w1, w0 + w1 + w2, -1 };
    SendMessageW(g_status, SB_SETPARTS, 4, (LPARAM)parts);
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)p0.c_str());
    SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)p1.c_str());
    SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)p2.c_str());
    SendMessageW(g_status, SB_SETTEXTW, 3, (LPARAM)p3.c_str());
}

void UpdateButtons()
{
    wchar_t l = SelectedLetter();
    const KV* kv = l ? FindVolume(l) : nullptr;
    std::wstring st = StateOf(kv);
    bool idle = g_busy == 0 && g_online;
    bool mounted = st == L"mounted";
    bool en[kBtnCount] = {
        idle,                         // Neu
        idle && kv,                   // Bearbeiten
        idle && kv,                   // Entfernen
        idle && kv && st == L"dismounted",
        idle && kv && mounted,
        idle && kv && mounted,        // Speichern
        kv && (mounted || st == L"saving"),  // Oeffnen
    };
    for (int i = 0; i < kBtnCount; i++) EnableWindow(g_btn[i], en[i]);
}

int MountedCount()
{
    int n = 0;
    for (auto& [name, kv] : g_last.sections) {
        std::wstring s = Get(kv, L"state");
        if (s == L"mounted" || s == L"saving") n++;
    }
    return n;
}

void TrayUpdate(bool add)
{
    g_nid.cbSize = sizeof g_nid;
    g_nid.hWnd = g_main;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    if (!g_nid.hIcon) {
        LoadIconMetric(g_inst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &g_nid.hIcon);
    }
    std::wstring tip = Fmt(T(S_TRAY_TIP), MountedCount());
    wcsncpy_s(g_nid.szTip, tip.c_str(), _TRUNCATE);
    if (add || !g_trayAdded) {
        g_trayAdded = Shell_NotifyIconW(NIM_ADD, &g_nid) != FALSE;
        g_nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &g_nid);
    }
}

void Refresh()
{
    Message req, resp;
    req.cmd = L"LIST";
    std::wstring err;
    g_online = PipeCall(req, resp, &err, 2000);
    if (g_online) g_last = resp;
    else g_last = Message();
    UpdateList();
    UpdateStatus();
    UpdateButtons();
    if (g_trayAdded) TrayUpdate(false);
}

// ----------------------------------------------------------------------- Menue

HMENU BuildMenu()
{
    HMENU bar = CreateMenu();
    HMENU file = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, CMD_EXIT, T(S_M_EXIT));
    HMENU drive = CreatePopupMenu();
    AppendMenuW(drive, MF_STRING, CMD_NEW, T(S_M_NEW));
    AppendMenuW(drive, MF_STRING, CMD_EDIT, T(S_M_EDIT));
    AppendMenuW(drive, MF_STRING, CMD_REMOVE, T(S_M_REMOVE));
    AppendMenuW(drive, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(drive, MF_STRING, CMD_MOUNT, T(S_M_MOUNT));
    AppendMenuW(drive, MF_STRING, CMD_DISMOUNT, T(S_M_DISMOUNT));
    AppendMenuW(drive, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(drive, MF_STRING, CMD_SAVE, T(S_M_SAVE));
    AppendMenuW(drive, MF_STRING, CMD_SAVEAS, T(S_M_SAVEAS));
    AppendMenuW(drive, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(drive, MF_STRING, CMD_EXPLORE, T(S_M_EXPLORE));
    AppendMenuW(drive, MF_STRING, CMD_REFRESH, T(S_M_REFRESH));
    HMENU tools = CreatePopupMenu();
    AppendMenuW(tools, MF_STRING, CMD_TEMP, T(S_M_TEMP));
    AppendMenuW(tools, MF_STRING, CMD_SETTINGS, T(S_M_SETTINGS));
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, CMD_LOG, T(S_M_LOG));
    AppendMenuW(tools, MF_STRING, CMD_RESETSTATS, T(S_M_RESETSTATS));
    AppendMenuW(tools, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(tools, MF_STRING, CMD_RECONNECT, T(S_M_RECONNECT));
    AppendMenuW(tools, MF_STRING, CMD_QUITPORTABLE, T(S_M_QUITPORTABLE));
    HMENU help = CreatePopupMenu();
    AppendMenuW(help, MF_STRING, CMD_KOFI, T(S_M_KOFI));
    AppendMenuW(help, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(help, MF_STRING, CMD_ABOUT, T(S_M_ABOUT));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)file, T(S_M_FILE));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)drive, T(S_M_DRIVE));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)tools, T(S_M_TOOLS));
    AppendMenuW(bar, MF_POPUP, (UINT_PTR)help, T(S_M_HELP));
    return bar;
}

void UpdateMenuState(HMENU m)
{
    wchar_t l = SelectedLetter();
    const KV* kv = l ? FindVolume(l) : nullptr;
    std::wstring st = StateOf(kv);
    bool idle = g_busy == 0 && g_online;
    auto en = [&](int id, bool on) { EnableMenuItem(m, id, MF_BYCOMMAND | (on ? MF_ENABLED : MF_GRAYED)); };
    en(CMD_NEW, idle);
    en(CMD_EDIT, idle && kv);
    en(CMD_REMOVE, idle && kv);
    en(CMD_MOUNT, idle && kv && st == L"dismounted");
    en(CMD_DISMOUNT, idle && kv && st == L"mounted");
    en(CMD_SAVE, idle && kv && st == L"mounted");
    en(CMD_SAVEAS, idle && kv && st == L"mounted");
    en(CMD_EXPLORE, kv && st != L"dismounted");
    en(CMD_TEMP, idle);
    en(CMD_RESETSTATS, idle);
    en(CMD_QUITPORTABLE, idle && g_last.Get(L"mode") == L"portable");
    en(CMD_RECONNECT, !g_online);
}

void ApplyLanguage()
{
    HMENU old = GetMenu(g_main);
    SetMenu(g_main, BuildMenu());
    if (old) DestroyMenu(old);
    for (int i = 0; i < kBtnCount; i++) SetWindowTextW(g_btn[i], T(kBtnText[i]));
    SetColumns();
    UpdateList();
    UpdateStatus();
    SendMessageW(g_main, WM_SIZE, 0, 0);
}

// ----------------------------------------------------------------------- Layout

void ApplyFont()
{
    if (g_font) DeleteObject(g_font);
    NONCLIENTMETRICSW ncm{ sizeof ncm };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    for (int i = 0; i < kBtnCount; i++) SendMessageW(g_btn[i], WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
}

void Layout()
{
    RECT rc;
    GetClientRect(g_main, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_status, &sr);
    int sh = sr.bottom - sr.top;
    int m = S(8), bh = S(30);
    HDC dc = GetDC(g_main);
    HGDIOBJ of = SelectObject(dc, g_font);
    int x = m;
    for (int i = 0; i < kBtnCount; i++) {
        wchar_t t[128];
        GetWindowTextW(g_btn[i], t, 128);
        SIZE sz{};
        GetTextExtentPoint32W(dc, t, (int)wcslen(t), &sz);
        int w = std::max<int>(sz.cx + S(24), S(84));
        MoveWindow(g_btn[i], x, m, w, bh, TRUE);
        x += w + S(6);
        if (i == 2 || i == 4) x += S(10);
    }
    SelectObject(dc, of);
    ReleaseDC(g_main, dc);
    int top = m + bh + m;
    MoveWindow(g_list, m, top, rc.right - 2 * m, std::max(0, (int)rc.bottom - sh - top - m), TRUE);
    UpdateStatus();
}

// ----------------------------------------------------------------------- Befehle

void ShowError(const std::wstring& text) { MsgBox(g_main, text, MB_ICONERROR); }

void JobResult(Job& j, const std::wstring& okFlash)
{
    if (!j.ok) ShowError(j.err);
    else {
        if (!j.resp.Get(L"warning").empty()) MsgBox(g_main, j.resp.Get(L"warning"), MB_ICONWARNING);
        if (!okFlash.empty()) Flash(okFlash);
    }
}

wchar_t SuggestLetter()
{
    std::vector<wchar_t> free = FreeLetters(0);
    const wchar_t order[] = L"RSTUVWXYZQPONMLKJIHGFED";
    for (wchar_t c : order)
        for (wchar_t f : free) if (f == c) return c;
    return free.empty() ? 0 : free[0];
}

void CmdNew()
{
    VolumeConfig c;
    c.letter = SuggestLetter();
    MEMORYSTATUSEX ms{ sizeof ms };
    GlobalMemoryStatusEx(&ms);
    c.size = ms.ullTotalPhys >= 16ull << 30 ? 4ull << 30 : ms.ullTotalPhys >= 8ull << 30 ? 2ull << 30 : 512ull << 20;
    c.label = L"RAMDisk";
    c.folders = L"Temp";
    if (!DlgVolume(g_main, c, true, false)) return;
    Message req;
    req.cmd = L"CREATE";
    req.args = c.ToKV();
    req.args[L"mount"] = L"1";
    wchar_t l = c.letter;
    RunAsync(req, Fmt(T(S_WORKING_CREATE), l), [](Job& j) { JobResult(j, T(S_DONE)); });
}

void CmdEdit()
{
    wchar_t l = SelectedLetter();
    const KV* kv = l ? FindVolume(l) : nullptr;
    if (!kv) return;
    VolumeConfig c;
    std::wstring err;
    if (!VolumeConfig::FromKV(*kv, c, err)) { ShowError(err); return; }
    bool mounted = StateOf(kv) != L"dismounted";
    if (mounted && !Get(*kv, L"livelabel").empty()) c.label = Get(*kv, L"livelabel");
    VolumeConfig old = c;
    if (!DlgVolume(g_main, c, false, mounted)) return;
    if (c.letter != old.letter) {
        // Buchstabe geaendert (nur ausgehaengt moeglich): erst neu anlegen, dann die alte Einstellung
        // entfernen, damit bei einer Ablehnung nichts verloren geht
        Message cr, resp;
        cr.cmd = L"CREATE";
        cr.args = c.ToKV();
        cr.args[L"mount"] = L"0";
        if (!PipeCall(cr, resp, &err)) { ShowError(err); Refresh(); return; }
        Message del;
        del.cmd = L"DELETE";
        del.args[L"letter"] = std::wstring(1, old.letter);
        if (!PipeCall(del, resp, &err)) ShowError(err);
        Refresh();
        return;
    }
    Message req, resp;
    req.cmd = L"UPDATE";
    req.args = c.ToKV();
    if (!PipeCall(req, resp, &err)) ShowError(err);
    Refresh();
}

void DoDismount(wchar_t l, bool remove, bool force)
{
    Message req;
    req.cmd = remove ? L"DELETE" : L"DISMOUNT";
    req.args[L"letter"] = std::wstring(1, l);
    if (force) req.args[L"force"] = L"1";
    RunAsync(req, Fmt(T(remove ? S_WORKING_REMOVE : S_WORKING_DISMOUNT), l), [l, remove, force](Job& j) {
        if (!j.ok && !force && !j.resp.Get(L"open").empty() && ToU64(j.resp.Get(L"open")) > 0) {
            if (MsgBox(g_main, Fmt(T(S_OPEN_FILES), j.err.c_str()), MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) == IDYES)
                DoDismount(l, remove, true);
            return;
        }
        JobResult(j, T(S_DONE));
    });
}

void CmdRemove()
{
    wchar_t l = SelectedLetter();
    const KV* kv = l ? FindVolume(l) : nullptr;
    if (!kv) return;
    if (MsgBox(g_main, Fmt(T(S_CONFIRM_REMOVE), l), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    if (StateOf(kv) != L"dismounted" && TempOnDrive(l) &&
        MsgBox(g_main, Fmt(T(S_TEMP_ON_DRIVE), l), MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    DoDismount(l, true, false);
}

void CmdMount()
{
    wchar_t l = SelectedLetter();
    if (!l) return;
    Message req;
    req.cmd = L"MOUNT";
    req.args[L"letter"] = std::wstring(1, l);
    RunAsync(req, Fmt(T(S_WORKING_MOUNT), l), [](Job& j) { JobResult(j, T(S_DONE)); });
}

void CmdDismount()
{
    wchar_t l = SelectedLetter();
    if (!l) return;
    if (TempOnDrive(l) && MsgBox(g_main, Fmt(T(S_TEMP_ON_DRIVE), l), MB_ICONWARNING | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    DoDismount(l, false, false);
}

void CmdSaveAs();

void CmdSave()
{
    wchar_t l = SelectedLetter();
    const KV* kv = l ? FindVolume(l) : nullptr;
    if (!kv) return;
    if (Get(*kv, L"image").empty()) { CmdSaveAs(); return; }
    Message req;
    req.cmd = L"SAVE";
    req.args[L"letter"] = std::wstring(1, l);
    RunAsync(req, Fmt(T(S_WORKING_SAVE), l), [l](Job& j) { JobResult(j, Fmt(T(S_SAVED), l)); });
}

void CmdSaveAs()
{
    wchar_t l = SelectedLetter();
    const KV* kv = l ? FindVolume(l) : nullptr;
    if (!kv) return;
    std::wstring filter = T(S_IMAGE_FILTER);
    for (auto& c : filter) if (c == L'|') c = L'\0';
    wchar_t file[MAX_PATH] = {};
    std::wstring cur = Get(*kv, L"image");
    if (cur.empty()) cur = ImagesFolder() + L"\\" + std::wstring(1, l) + L".img";
    wcsncpy_s(file, cur.c_str(), _TRUNCATE);
    std::wstring dir = ImagesFolder();
    CreateDirectoryW(dir.c_str(), nullptr);
    OPENFILENAMEW ofn{ sizeof ofn };
    ofn.hwndOwner = g_main;
    ofn.lpstrFilter = filter.c_str();
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrDefExt = L"img";
    ofn.lpstrTitle = T(S_SAVEAS_TITLE);
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER;
    if (!GetSaveFileNameW(&ofn)) return;
    Message req;
    req.cmd = L"SAVE";
    req.args[L"letter"] = std::wstring(1, l);
    req.args[L"path"] = file;
    req.args[L"bind"] = L"1";
    RunAsync(req, Fmt(T(S_WORKING_SAVE), l), [l](Job& j) { JobResult(j, Fmt(T(S_SAVED), l)); });
}

void CmdExplore()
{
    wchar_t l = SelectedLetter();
    if (!l) return;
    std::wstring p = std::wstring(1, l) + L":\\";
    ShellExecuteW(g_main, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void CmdLog()
{
    std::wstring p = g_last.Get(L"logfile");
    if (p.empty()) p = ServicePath(kLogFile);
    ShellExecuteW(g_main, L"open", L"notepad.exe", (L"\"" + p + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
}

void CmdReconnect()
{
    std::wstring err;
    if (!EnsureHost(!MachineInstall(), &err)) ShowError(Fmt(T(S_HOST_FAIL), err.c_str()));
    Refresh();
}

void CmdQuitPortable()
{
    if (MsgBox(g_main, T(S_QUIT_PORTABLE_Q), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    Message req;
    req.cmd = L"QUIT";
    RunAsync(req, T(S_ST_BUSY), [](Job& j) {
        if (!j.ok) ShowError(j.err);
        for (int i = 0; i < 600 && ServiceReachable(); i++) Sleep(100);
    });
}

void CmdResetStats()
{
    if (MsgBox(g_main, T(S_RESET_STATS_Q), MB_ICONQUESTION | MB_YESNO | MB_DEFBUTTON2) != IDYES) return;
    Message req, resp;
    req.cmd = L"LIFETIMERESET";
    std::wstring err;
    if (!PipeCall(req, resp, &err)) ShowError(err);
    Refresh();
}

void ShowMain()
{
    if (IsIconic(g_main)) ShowWindow(g_main, SW_RESTORE);
    else ShowWindow(g_main, SW_SHOW);
    SetForegroundWindow(g_main);
    Refresh();
}

void TrayMenu()
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING | MF_DEFAULT, CMD_SHOW, T(S_M_SHOW));
    if (!g_last.sections.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (auto& [name, kv] : g_last.sections) {
        if (name.empty()) continue;
        std::wstring st = Get(kv, L"state");
        bool mounted = st != L"dismounted";
        std::wstring t = name + L":  " + Get(kv, L"label") + L"  –  " + (mounted ? T(S_DISMOUNT) : T(S_MOUNT));
        AppendMenuW(m, MF_STRING | ((g_busy || st == L"saving") ? MF_GRAYED : 0), CMD_TRAY_TOGGLE + name[0], t.c_str());
    }
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EXIT, T(S_M_EXIT));
    POINT pt;
    GetCursorPos(&pt);
    SetForegroundWindow(g_main);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_main, nullptr);
    PostMessageW(g_main, WM_NULL, 0, 0);
    DestroyMenu(m);
}

void SelectLetter(wchar_t l)
{
    int n = ListView_GetItemCount(g_list);
    for (int i = 0; i < n; i++) {
        LVITEMW it{};
        it.mask = LVIF_PARAM;
        it.iItem = i;
        ListView_GetItem(g_list, &it);
        if ((wchar_t)it.lParam == l) ListView_SetItemState(g_list, i, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

void ListContextMenu(int x, int y)
{
    HMENU m = CreatePopupMenu();
    AppendMenuW(m, MF_STRING, CMD_EXPLORE, T(S_M_EXPLORE));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_MOUNT, T(S_M_MOUNT));
    AppendMenuW(m, MF_STRING, CMD_DISMOUNT, T(S_M_DISMOUNT));
    AppendMenuW(m, MF_STRING, CMD_SAVE, T(S_M_SAVE));
    AppendMenuW(m, MF_STRING, CMD_SAVEAS, T(S_M_SAVEAS));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_EDIT, T(S_M_EDIT));
    AppendMenuW(m, MF_STRING, CMD_REMOVE, T(S_M_REMOVE));
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, CMD_NEW, T(S_M_NEW));
    UpdateMenuState(m);
    if (x == -1 && y == -1) {
        int i = ListView_GetNextItem(g_list, -1, LVNI_SELECTED);
        RECT r{};
        if (i >= 0) ListView_GetItemRect(g_list, i, &r, LVIR_BOUNDS);
        POINT p{ r.left + S(20), r.bottom };
        ClientToScreen(g_list, &p);
        x = p.x; y = p.y;
    }
    TrackPopupMenu(m, TPM_RIGHTBUTTON, x, y, 0, g_main, nullptr);
    DestroyMenu(m);
}

void SavePlacement()
{
    WINDOWPLACEMENT wp{ sizeof wp };
    if (!GetWindowPlacement(g_main, &wp)) return;
    RECT& r = wp.rcNormalPosition;
    GuiIni().Set(L"Window", L"Pos", Fmt(L"%d,%d,%d,%d,%d", r.left, r.top, r.right, r.bottom, wp.showCmd == SW_SHOWMAXIMIZED ? 1 : 0));
}

void ExitApp()
{
    g_exiting = true;
    SavePlacement();
    if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
    DestroyWindow(g_main);
}

void OnCommand(int id)
{
    if (id >= CMD_TRAY_TOGGLE && id < CMD_TRAY_TOGGLE + 128) {
        wchar_t l = (wchar_t)(id - CMD_TRAY_TOGGLE);
        SelectLetter(l);
        const KV* kv = FindVolume(l);
        if (!kv) return;
        if (StateOf(kv) == L"dismounted") CmdMount(); else CmdDismount();
        return;
    }
    switch (id) {
    case CMD_NEW: CmdNew(); break;
    case CMD_EDIT: CmdEdit(); break;
    case CMD_REMOVE: CmdRemove(); break;
    case CMD_MOUNT: CmdMount(); break;
    case CMD_DISMOUNT: CmdDismount(); break;
    case CMD_SAVE: CmdSave(); break;
    case CMD_SAVEAS: CmdSaveAs(); break;
    case CMD_EXPLORE: CmdExplore(); break;
    case CMD_TEMP: DlgTemp(g_main); Refresh(); break;
    case CMD_SETTINGS:
        if (DlgSettings(g_main)) {
            ApplyLanguage();
            ThemeApply(g_main);
        }
        Refresh();
        break;
    case CMD_LOG: CmdLog(); break;
    case CMD_RECONNECT: CmdReconnect(); break;
    case CMD_QUITPORTABLE: CmdQuitPortable(); break;
    case CMD_RESETSTATS: CmdResetStats(); break;
    case CMD_ABOUT: DlgAbout(g_main); break;
    case CMD_KOFI: OpenUrl(L"https://ko-fi.com/C5H12AP4UV"); break;
    case CMD_SHOW: ShowMain(); break;
    case CMD_REFRESH: Refresh(); break;
    case CMD_EXIT: ExitApp(); break;
    }
}

// ----------------------------------------------------------------------- Fensterprozedur

LRESULT CALLBACK MainProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (msg == g_taskbarCreated && g_taskbarCreated) {
        g_trayAdded = false;
        TrayUpdate(true);
        return 0;
    }
    LRESULT themed = 0;
    if (ThemeWindowMessage(hwnd, msg, wp, lp, &themed)) return themed;
    switch (msg) {
    case WM_SETTINGCHANGE:
    case WM_SYSCOLORCHANGE:
        // Windows-Farbmodus oder Kontrastdesign geaendert: bei "Wie Windows" sofort umschalten
        if (ThemeUpdate()) ThemeApply(hwnd);
        break;
    case WM_CREATE: {
        g_main = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        for (int i = 0; i < kBtnCount; i++)
            g_btn[i] = CreateWindowExW(0, L"BUTTON", T(kBtnText[i]), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(ID_BTN_FIRST + i), g_inst, nullptr);
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
            LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_LIST, g_inst, nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowTheme(g_list, L"Explorer", nullptr);
        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
            (HMENU)(INT_PTR)ID_STATUS, g_inst, nullptr);
        ApplyFont();
        SetColumns();
        SetMenu(hwnd, BuildMenu());
        ThemeApply(hwnd);
        SetTimer(hwnd, TIMER_REFRESH, 1000, nullptr);
        return 0;
    }
    case WM_SIZE:
        Layout();
        return 0;
    case WM_DPICHANGED: {
        g_dpi = HIWORD(wp);
        RECT* r = (RECT*)lp;
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        ApplyFont();
        Layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        mm->ptMinTrackSize.x = S(640);
        mm->ptMinTrackSize.y = S(300);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_REFRESH && IsWindowVisible(hwnd) && !IsIconic(hwnd)) Refresh();
        else if (wp == TIMER_REFRESH && g_trayAdded) {
            // im Infobereich: seltener abfragen (alle 5 s) fuer den Tooltip
            static int n = 0;
            if (++n % 5 == 0) Refresh();
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id >= ID_BTN_FIRST && id < ID_BTN_FIRST + kBtnCount) id = kBtnCmd[id - ID_BTN_FIRST];
        OnCommand(id);
        return 0;
    }
    case WM_INITMENUPOPUP:
        UpdateMenuState((HMENU)wp);
        return 0;
    case WM_NOTIFY: {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->idFrom == ID_LIST) {
            if (nh->code == LVN_ITEMCHANGED) UpdateButtons();
            else if (nh->code == NM_DBLCLK) {
                if (SelectedLetter()) {
                    const KV* kv = FindVolume(SelectedLetter());
                    if (kv && StateOf(kv) != L"dismounted") CmdExplore(); else CmdEdit();
                }
            } else if (nh->code == LVN_KEYDOWN) {
                NMLVKEYDOWN* kd = (NMLVKEYDOWN*)lp;
                if (kd->wVKey == VK_DELETE) OnCommand(CMD_REMOVE);
                else if (kd->wVKey == VK_RETURN) OnCommand(CMD_EDIT);
            }
        }
        return 0;
    }
    case WM_CONTEXTMENU:
        if ((HWND)wp == g_list) { ListContextMenu(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0; }
        break;
    case WM_APP_JOBDONE: {
        Job* j = (Job*)lp;
        g_busy--;
        Refresh();
        j->done(*j);
        delete j;
        Refresh();
        return 0;
    }
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_LBUTTONDBLCLK:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowMain();
            break;
        case WM_CONTEXTMENU:
        case WM_RBUTTONUP:
            Refresh();
            TrayMenu();
            break;
        }
        return 0;
    case WM_APP_SHOW:
        ShowMain();
        return 0;
    case WM_APP_REFRESH:
        Refresh();
        return 0;
    case WM_CLOSE:
        if (g_set.trayClose && !g_exiting) {
            SavePlacement();
            if (!g_trayAdded) TrayUpdate(true);
            ShowWindow(hwnd, SW_HIDE);
            return 0;
        }
        ExitApp();
        return 0;
    case WM_QUERYENDSESSION:
        return TRUE;
    case WM_ENDSESSION:
        if (wp) SavePlacement();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_REFRESH);
        if (g_trayAdded) Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

void RequestRefresh() { if (g_main) PostMessageW(g_main, WM_APP_REFRESH, 0, 0); }

const KV* FindVolume(wchar_t letter)
{
    for (auto& [name, kv] : g_last.sections)
        if (!name.empty() && towupper(name[0]) == towupper(letter)) return &kv;
    return nullptr;
}

} // namespace gui

// ======================================================================= Einstieg

using namespace gui;

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, LPWSTR cmdLine, int)
{
    g_inst = inst;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    bool trayStart = cmdLine && (wcsstr(cmdLine, L"/tray") || wcsstr(cmdLine, L"-tray"));

    // Fensterklassen-Namen sind unabhaengig von Gross-/Kleinschreibung; der Mutex-Name nicht, er behaelt
    // die alte Schreibweise, damit auch neben einer laufenden Oberflaeche bis 1.2 keine zweite startet.
    static const std::wstring wndClass = L"RamDiskBoxMainWnd" + InstanceSuffix();
    HANDLE mutex = CreateMutexW(nullptr, TRUE, (L"Local\\RAMDiskBox.GUI" + InstanceSuffix()).c_str());
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(wndClass.c_str(), nullptr);
        if (other && !trayStart) PostMessageW(other, WM_APP + 3, 0, 0);
        return 0;
    }

    // Einstellungen
    {
        Ini& ini = GuiIni();
        std::wstring lang = Lower(Trim(ini.Get(L"GUI", L"Language", L"auto")));
        g_set.lang = (lang == L"de" || lang == L"en") ? lang : L"auto";
        g_set.theme = ThemeModeName(ParseThemeMode(ini.Get(L"GUI", L"Theme", L"system")));
        g_set.trayClose = ToBool(ini.Get(L"GUI", L"TrayClose", L"1"), true);
        g_set.startMin = ToBool(ini.Get(L"GUI", L"StartMinimized", L"0"), false);
        SetLanguage(g_set.lang.c_str());
    }

    INITCOMMONCONTROLSEX icc{ sizeof icc, ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    ThemeInit(ParseThemeMode(g_set.theme));

    if (!WinFspInstalled()) {
        if (MsgBox(nullptr, T(S_NO_WINFSP), MB_ICONWARNING | MB_YESNO) == IDYES) OpenUrl(L"https://winfsp.dev/rel/");
        return 1;
    }

    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSEXW wc{ sizeof wc };
    wc.lpfnWndProc = MainProc;
    wc.hInstance = inst;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP));
    wc.hIconSm = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = wndClass.c_str();
    RegisterClassExW(&wc);

    std::wstring title = std::wstring(kAppName) + L" " + RDB_VERSION_W + L" (Build " + RDB_BUILD_W + L")";
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr, inst, nullptr);
    if (!hwnd) return 1;

    // Fensterposition
    {
        std::vector<std::wstring> p = Split(GuiIni().Get(L"Window", L"Pos", L""), L',');
        UINT dpi = GetDpiForWindow(hwnd);
        if (p.size() == 5) {
            WINDOWPLACEMENT wpl{ sizeof wpl };
            wpl.rcNormalPosition = { _wtoi(p[0].c_str()), _wtoi(p[1].c_str()), _wtoi(p[2].c_str()), _wtoi(p[3].c_str()) };
            wpl.showCmd = SW_HIDE;
            SetWindowPlacement(hwnd, &wpl);
            if (p[4] == L"1") wpl.showCmd = SW_SHOWMAXIMIZED;
        } else {
            SetWindowPos(hwnd, nullptr, 0, 0, MulDiv(980, dpi, 96), MulDiv(440, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
        }
    }

    bool hidden = trayStart || g_set.startMin;
    if (hidden) TrayUpdate(true);
    else {
        std::vector<std::wstring> p = Split(GuiIni().Get(L"Window", L"Pos", L""), L',');
        ShowWindow(hwnd, (p.size() == 5 && p[4] == L"1") ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
        UpdateWindow(hwnd);
        if (g_set.trayClose) TrayUpdate(true);
    }

    std::wstring err;
    // Installation fuer alle Benutzer: nur den Dienst starten, nie einen portablen Prozess
    if (!EnsureHost(!MachineInstall(), &err) && !trayStart) MsgBox(hwnd, Fmt(T(S_HOST_FAIL), err.c_str()), MB_ICONERROR);
    Refresh();

    // Beschleuniger
    ACCEL acc[] = {
        { FCONTROL | FVIRTKEY, 'N', (WORD)CMD_NEW },
        { FCONTROL | FVIRTKEY, 'S', (WORD)CMD_SAVE },
        { FVIRTKEY, VK_F5, (WORD)CMD_REFRESH },
    };
    g_accel = CreateAcceleratorTableW(acc, (int)std::size(acc));

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) {
        if (TranslateAcceleratorW(g_main, g_accel, &m)) continue;
        if (IsDialogMessageW(g_main, &m)) continue;
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    if (mutex) CloseHandle(mutex);
    return 0;
}
