// RamDiskBox.exe - Oberflaeche
#include "gui.h"
#include "widgets.h"
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
    CMD_ABOUT, CMD_KOFI, CMD_EXIT, CMD_SHOW, CMD_REFRESH, CMD_STATS,
    CMD_TRAY_TOGGLE = 41000,   // + Buchstabe
};

constexpr int ID_LIST = 5100;
constexpr int ID_STATUS = 5200;
constexpr int ID_MEMBAR = 5300;
constexpr int ID_BTN_FIRST = 5001;
constexpr UINT TIMER_REFRESH = 1;

// Symbolleiste: Befehl, Kurzhilfe, Symbol, Farbe; Trennstriche nach den Gruppen
const int kBtnCmd[] = { CMD_NEW, CMD_EDIT, CMD_REMOVE, CMD_MOUNT, CMD_DISMOUNT, CMD_SAVE, CMD_EXPLORE, CMD_SETTINGS };
const Str kBtnText[] = { S_NEW, S_EDIT, S_REMOVE, S_MOUNT, S_DISMOUNT, S_SAVE, S_OPEN, S_SD_TITLE };
const wchar_t kBtnGlyph[] = { Glyph::Add, Glyph::Edit, Glyph::Cancel, Glyph::CheckMark, Glyph::Stop, Glyph::Save,
                              Glyph::FolderOpen, Glyph::Settings };
const Tint kBtnTint[] = { Tint::Green, Tint::Blue, Tint::Red, Tint::Green, Tint::Gray, Tint::Blue, Tint::Amber, Tint::Gray };
constexpr int kBtnCount = 8;
constexpr bool kSepAfter[] = { false, false, true, false, true, false, true, false };

// Spalten der Liste
enum Col { COL_DRIVE, COL_LABEL, COL_SIZE, COL_USAGE, COL_COMP, COL_IMAGE, COL_COUNT };
// Status-Symbole (Reihenfolge im Bildersatz)
enum Img { IMG_MOUNTED, IMG_DISMOUNTED, IMG_WARNING, IMG_SAVING };

struct RowInfo { bool mounted = false; unsigned long long used = 0, size = 0; };

HWND g_list = nullptr, g_status = nullptr, g_membar = nullptr, g_tip = nullptr;
HWND g_btn[kBtnCount] = {};
HIMAGELIST g_icons = nullptr;
std::vector<int> g_sepX;          // x-Positionen der Trennstriche in der Symbolleiste
std::vector<RowInfo> g_rows;      // Belegung je Zeile (fuer den gezeichneten Balken)
int g_toolTop = 0, g_toolBottom = 0;
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

// Status-Symbole der Liste (Farben je Hell/Dunkel, Groesse je DPI)
void BuildIcons()
{
    int px = S(16);
    HIMAGELIST il = ImageList_Create(px, px, ILC_COLOR32, 4, 0);
    const wchar_t glyphs[] = { Glyph::CompletedSolid, Glyph::RadioOff, Glyph::Warning, Glyph::Save };
    const Tint tints[] = { Tint::Green, Tint::Gray, Tint::Amber, Tint::Blue };
    for (int i = 0; i < 4; i++) ImageList_Add(il, GlyphBitmap(glyphs[i], px, TintColor(tints[i])), nullptr);
    ListView_SetImageList(g_list, il, LVSIL_SMALL);
    if (g_icons) ImageList_Destroy(g_icons);
    g_icons = il;
}

void SetColumns()
{
    const Str cols[] = { S_COL_DRIVE, S_COL_LABEL, S_COL_SIZE, S_COL_USAGE, S_COL_COMP, S_COL_IMAGE };
    const int widths[] = { 96, 130, 80, 190, 170, 240 };
    const int fmt[] = { LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_LEFT };
    HWND hdr = ListView_GetHeader(g_list);
    int have = hdr ? Header_GetItemCount(hdr) : 0;
    for (int i = 0; i < COL_COUNT; i++) {
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
            std::wstring t = L" " + std::wstring(1, letters[i]) + L":";   // Abstand zum Status-Symbol
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
    g_rows.assign(g_last.sections.size(), RowInfo{});
    for (size_t i = 0; i < g_last.sections.size(); i++) {
        const KV& kv = g_last.sections[i].second;
        int row = (int)i;
        std::wstring st = Get(kv, L"state");
        bool mounted = st == L"mounted" || st == L"saving";
        // Status als Symbol vor dem Buchstaben
        int img = st == L"saving" ? IMG_SAVING : !Get(kv, L"lasterror").empty() ? IMG_WARNING : mounted ? IMG_MOUNTED : IMG_DISMOUNTED;
        LVITEMW it{};
        it.mask = LVIF_IMAGE;
        it.iItem = row;
        ListView_GetItem(g_list, &it);
        if (it.iImage != img) {
            it.iImage = img;
            ListView_SetItem(g_list, &it);
        }
        std::wstring label = mounted && !Get(kv, L"livelabel").empty() ? Get(kv, L"livelabel") : Get(kv, L"label");
        SetCell(row, COL_LABEL, label);
        SetCell(row, COL_SIZE, FormatSize(ToU64(Get(kv, L"size"))));
        g_rows[i] = { mounted, ToU64(Get(kv, L"used")), ToU64(Get(kv, L"size")) };
        SetCell(row, COL_USAGE, L"");   // Balken und Text zeichnet ListCustomDraw
        {
            int lvl = ParseCompress(Get(kv, L"compress"), 0);
            const Str names[] = { S_COMP_OFF, S_COMP_FAST, S_COMP_NORMAL, S_COMP_MAX };
            std::wstring ct = T(names[lvl < 0 || lvl > 3 ? 0 : lvl]);
            unsigned long long raw = ToU64(Get(kv, L"compraw")), stored = ToU64(Get(kv, L"compstored"));
            if (mounted && raw > 0) ct = Fmt(T(S_COMP_SAVED), ct.c_str(), (int)((raw > stored ? raw - stored : 0) * 100 / raw));
            SetCell(row, COL_COMP, ct);
        }
        SetCell(row, COL_IMAGE, Get(kv, L"image").empty() ? T(S_NO_IMAGE) : Get(kv, L"image"));
    }
    ListView_RedrawItems(g_list, 0, (int)g_rows.size());
}

// Belegungsbalken in der Spalte "Belegung" (nach dem normalen Zeichnen, damit Auswahl und Hover bleiben)
LRESULT ListCustomDraw(NMLVCUSTOMDRAW* cd)
{
    switch (cd->nmcd.dwDrawStage) {
    case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
    case CDDS_ITEMPREPAINT: return CDRF_NOTIFYPOSTPAINT;
    case CDDS_ITEMPOSTPAINT: {
        size_t row = cd->nmcd.dwItemSpec;
        if (row >= g_rows.size() || !g_rows[row].mounted || !g_rows[row].size) return CDRF_DODEFAULT;
        RECT cell{};
        ListView_GetSubItemRect(g_list, (int)row, COL_USAGE, LVIR_BOUNDS, &cell);
        HDC dc = cd->nmcd.hdc;
        int pct = (int)std::min<unsigned long long>(100, g_rows[row].used * 100 / g_rows[row].size);
        int barW = std::min(S(70), (int)(cell.right - cell.left) / 2), barH = S(8);
        RECT bar{ cell.left + S(6), (cell.top + cell.bottom - barH) / 2, 0, 0 };
        bar.right = bar.left + barW;
        bar.bottom = bar.top + barH;
        bool dark = IsDark();
        HBRUSH track = CreateSolidBrush(dark ? RGB(62, 62, 62) : RGB(222, 222, 222));
        FillRect(dc, &bar, track);
        DeleteObject(track);
        RECT fill = bar;
        fill.right = bar.left + std::max(pct > 0 ? 2 : 0, barW * pct / 100);
        HBRUSH fb = CreateSolidBrush(pct >= 95 ? TintColor(Tint::Red) : pct >= 80 ? TintColor(Tint::Amber) : TintColor(Tint::Green));
        FillRect(dc, &fill, fb);
        DeleteObject(fb);
        std::wstring t = FormatSize(g_rows[row].used) + L" · " + std::to_wstring(pct) + L" %";
        RECT tr{ bar.right + S(8), cell.top, cell.right - S(4), cell.bottom };
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, dark ? Colors().text : GetSysColor(COLOR_WINDOWTEXT));
        DrawTextW(dc, t.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        return CDRF_DODEFAULT;
    }
    }
    return CDRF_DODEFAULT;
}

// ----------------------------------------------------------------------- Status/Knoepfe/Infobereich

void UpdateStatus()
{
    if (!g_status) return;
    // Teil 0: Angaben zum ausgewaehlten Laufwerk (bzw. Warnung, Fortschritt, kurze Meldung)
    // Teil 1: Betriebsart, Teil 2: aktuelle Lese-/Schreibrate
    std::wstring p0, p1, p2;
    if (g_online) {
        p1 = Fmt(g_last.Get(L"mode") == L"service" ? T(S_SVC_SERVICE) : T(S_SVC_PORTABLE), g_last.Get(L"version").c_str());
        p2 = Fmt(T(S_IO), FormatSize(ToU64(g_last.Get(L"speedread"))).c_str(), FormatSize(ToU64(g_last.Get(L"speedwrite"))).c_str());
        wchar_t sel = g_list ? SelectedLetter() : 0;
        if (const KV* kv = sel ? FindVolume(sel) : nullptr) {
            std::wstring le = Get(*kv, L"lasterror");
            std::wstring st = Get(*kv, L"state");
            if (!le.empty()) p0 = L"⚠ " + std::wstring(1, sel) + L": " + le;
            else if (st == L"dismounted") p0 = Fmt(T(S_SEL_DISMOUNTED), sel);
            else {
                p0 = Fmt(T(S_SEL_INFO), sel, FormatSize(ToU64(Get(*kv, L"used"))).c_str(), FormatSize(ToU64(Get(*kv, L"memory"))).c_str(),
                         Get(*kv, L"files").c_str());
                std::wstring ls = Get(*kv, L"lastsave");
                if (!ls.empty()) p0 += Fmt(T(S_SEL_SAVED), DisplayTime(ls).c_str());
            }
        }
    } else {
        p0 = T(S_SVC_NONE);
    }
    if (g_busy > 0) p0 = g_busyText;
    else if (!g_flash.empty() && GetTickCount64() < g_flashUntil) p0 = g_flash;
    HDC dc = GetDC(g_status);
    HGDIOBJ of = SelectObject(dc, g_font);
    auto tw = [&](const std::wstring& s) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
        return (int)sz.cx + S(24);
    };
    RECT src{};
    GetClientRect(g_status, &src);
    int w2 = tw(p2), w1 = tw(p1);
    SelectObject(dc, of);
    ReleaseDC(g_status, dc);
    int w0 = std::max(S(120), (int)src.right - w1 - w2 - S(16));
    int parts[3] = { w0, w0 + w1, -1 };
    SendMessageW(g_status, SB_SETPARTS, 3, (LPARAM)parts);
    SendMessageW(g_status, SB_SETTEXTW, 0, (LPARAM)p0.c_str());
    SendMessageW(g_status, SB_SETTEXTW, 1, (LPARAM)p1.c_str());
    SendMessageW(g_status, SB_SETTEXTW, 2, (LPARAM)p2.c_str());

    // Speicherleiste: RAM-Laufwerke | frei | andere Programme
    if (g_membar) {
        unsigned long long disks = 0;
        for (auto& [name, kv] : g_last.sections) disks += ToU64(Get(kv, L"memory"));
        unsigned long long total = ToU64(g_last.Get(L"ramtotal")), freeMem = ToU64(g_last.Get(L"ramfree"));
        if (!g_online) {
            MEMORYSTATUSEX ms{ sizeof ms };
            GlobalMemoryStatusEx(&ms);
            total = ms.ullTotalPhys;
            freeMem = ms.ullAvailPhys;
        }
        unsigned long long other = total > freeMem + disks ? total - freeMem - disks : 0;
        MemBarSet(g_membar, disks, freeMem, total, Fmt(T(S_MEM_DISKS), FormatSize(disks).c_str()),
                  Fmt(T(S_MEM_FREE), FormatSize(freeMem).c_str()), Fmt(T(S_MEM_OTHER), FormatSize(other).c_str()));
    }
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
        true,                         // Einstellungen
    };
    for (int i = 0; i < kBtnCount; i++)
        if ((IsWindowEnabled(g_btn[i]) != FALSE) != en[i]) EnableWindow(g_btn[i], en[i]);
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
    AppendMenuW(tools, MF_STRING, CMD_STATS, T(S_M_STATS));
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
    en(CMD_STATS, g_online);
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
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_membar, WM_SETFONT, (WPARAM)g_font, TRUE);
}

// Aufbau: Symbolleiste | Liste | Speicherleiste | Statuszeile
void Layout()
{
    RECT rc;
    GetClientRect(g_main, &rc);
    SendMessageW(g_status, WM_SIZE, 0, 0);
    RECT sr;
    GetWindowRect(g_status, &sr);
    int sh = sr.bottom - sr.top;
    int m = S(8), bs = S(42);
    g_toolTop = S(6);
    g_toolBottom = g_toolTop + bs;
    int x = m;
    g_sepX.clear();
    for (int i = 0; i < kBtnCount; i++) {
        MoveWindow(g_btn[i], x, g_toolTop, bs, bs, TRUE);
        x += bs + S(4);
        if (kSepAfter[i]) {
            g_sepX.push_back(x + S(5));
            x += S(14);
        }
    }
    int top = g_toolBottom + S(6);
    int barH = S(30);
    int barTop = (int)rc.bottom - sh - S(8) - barH;
    MoveWindow(g_list, m, top, rc.right - 2 * m, std::max(0, barTop - S(8) - top), TRUE);
    MoveWindow(g_membar, m, barTop, rc.right - 2 * m, barH, TRUE);
    RECT tb{ 0, 0, rc.right, g_toolBottom + S(2) };
    InvalidateRect(g_main, &tb, TRUE);
    UpdateStatus();
}

// Hell/Dunkel gewechselt: Fenster, Symbole und Kurzhilfe neu einfaerben
void ApplyThemeAll()
{
    ThemeApply(g_main);
    BuildIcons();
    if (g_tip) SetWindowTheme(g_tip, IsDark() ? L"DarkMode_Explorer" : nullptr, nullptr);
    for (HWND b : g_btn) InvalidateRect(b, nullptr, TRUE);
    InvalidateRect(g_membar, nullptr, FALSE);
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

// Ein Argument fuer die Befehlszeile (Regeln von CommandLineToArgvW: Backslashes vor Anfuehrungszeichen verdoppeln)
std::wstring ArgQuote(const std::wstring& s)
{
    if (!s.empty() && s.find_first_of(L" \t\"") == std::wstring::npos) return s;
    std::wstring r = L"\"";
    size_t bs = 0;
    for (wchar_t ch : s) {
        if (ch == L'\\') { bs++; continue; }
        if (ch == L'"') { r.append(bs * 2 + 1, L'\\'); r += L'"'; bs = 0; continue; }
        r.append(bs, L'\\');
        bs = 0;
        r += ch;
    }
    r.append(bs * 2, L'\\');
    return r + L"\"";
}

// Alle Einstellungen eines Laufwerks als rdbctl-Optionen
std::wstring CtlArgs(const VolumeConfig& c)
{
    std::wstring a = U64(c.size) + L"b";
    a += L" " + ArgQuote(L"label=" + c.label);
    a += L" " + ArgQuote(L"folders=" + c.folders);
    a += L" " + ArgQuote(L"image=" + c.image);
    a += L" " + ArgQuote(L"source=" + c.source);
    a += L" autosave=" + U64(c.autoSaveMin);
    a += std::wstring(L" compress=") + CompressName(c.compress);
    a += c.loadImage ? L" load" : L" noload";
    a += c.saveImage ? L" save" : L" nosave";
    a += c.compressImage ? L" compress" : L" nocompress";
    a += c.autoMount ? L" auto" : L" noauto";
    a += c.reportNtfs ? L" ntfs" : L" nontfs";
    a += c.useCache ? L" cache" : L" nocache";
    a += c.wipe ? L" wipe" : L" nowipe";
    a += c.avExclude ? L" avexclude" : L" noavexclude";
    return a;
}

// Der Dienst nimmt manche Einstellungen nur von Administratoren an (Defender-Ausnahme, Quellordner,
// Image ausserhalb des Image-Ordners): anbieten, sie per UAC ueber rdbctl zu uebernehmen.
bool ApplyElevated(const VolumeConfig& c, bool create, const std::wstring& reason, bool mount = true)
{
    if (MsgBox(g_main, Fmt(T(S_NEED_ADMIN_Q), reason.c_str()), MB_ICONQUESTION | MB_YESNO) != IDYES) return false;
    std::wstring args;
    std::wstring inst = InstanceSuffix();
    if (!inst.empty()) args += L"--instance=" + inst.substr(1) + L" ";
    args += std::wstring(L"--lang=") + LangCode(CurrentLanguage()) + L" ";
    args += create ? L"create " : L"update ";
    args += std::wstring(1, c.letter) + L" " + CtlArgs(c);
    if (create && !mount) args += L" nomount";
    int code = RunElevated(g_main, ExePath(L"rdbctl.exe"), args);
    if (code == 0) { Flash(T(S_DONE)); return true; }
    if (code != -1) ShowError(Fmt(T(S_ELEVATED_FAIL), code));
    return false;
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
    RunAsync(req, Fmt(T(S_WORKING_CREATE), l), [c](Job& j) {
        if (!j.ok && j.resp.Get(L"needadmin") == L"1") { ApplyElevated(c, true, j.err); return; }
        JobResult(j, T(S_DONE));
    });
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
        if (!PipeCall(cr, resp, &err)) {
            bool done = false;
            if (resp.Get(L"needadmin") == L"1") {
                done = ApplyElevated(c, true, err, false);
            } else {
                ShowError(err);
            }
            if (!done) { Refresh(); return; }
        }
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
    if (!PipeCall(req, resp, &err)) {
        if (resp.Get(L"needadmin") == L"1") ApplyElevated(c, false, err);
        else ShowError(err);
    }
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

// Gesamtzaehler und Hoechstwerte (frueher in der Statuszeile)
void CmdStats()
{
    Refresh();
    if (!g_online) { ShowError(T(S_SVC_NONE)); return; }
    unsigned long long up = ToU64(g_last.Get(L"uptime"));
    std::wstring upText = Fmt(L"%llu h %02llu min", up / 3600, (up / 60) % 60);
    MsgBox(g_main, Fmt(T(S_STATS_TEXT), DisplayTime(g_last.Get(L"since")).c_str(),
                       FormatSize(ToU64(g_last.Get(L"liferead"))).c_str(), FormatSize(ToU64(g_last.Get(L"lifewritten"))).c_str(),
                       FormatSize(ToU64(g_last.Get(L"peakread"))).c_str(), FormatSize(ToU64(g_last.Get(L"peakwrite"))).c_str(),
                       upText.c_str()), MB_ICONINFORMATION);
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
            ApplyThemeAll();
        }
        Refresh();
        break;
    case CMD_LOG: CmdLog(); break;
    case CMD_RECONNECT: CmdReconnect(); break;
    case CMD_QUITPORTABLE: CmdQuitPortable(); break;
    case CMD_RESETSTATS: CmdResetStats(); break;
    case CMD_STATS: CmdStats(); break;
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
        if (ThemeUpdate()) ApplyThemeAll();
        break;
    case WM_CREATE: {
        g_main = hwnd;
        g_dpi = GetDpiForWindow(hwnd);
        // Symbolleiste: gezeichnete Knoepfe mit Kurzhilfe (Text bleibt fuer Bildschirmleser gesetzt)
        g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, hwnd, nullptr, g_inst, nullptr);
        for (int i = 0; i < kBtnCount; i++) {
            g_btn[i] = CreateWindowExW(0, L"BUTTON", T(kBtnText[i]), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)(ID_BTN_FIRST + i), g_inst, nullptr);
            ToolButtonAttach(g_btn[i]);
            TTTOOLINFOW ti{ sizeof ti };
            ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
            ti.hwnd = hwnd;
            ti.uId = (UINT_PTR)g_btn[i];
            ti.lpszText = LPSTR_TEXTCALLBACKW;
            SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        g_list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT |
            LVS_SINGLESEL | LVS_SHOWSELALWAYS, 0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_LIST, g_inst, nullptr);
        ListView_SetExtendedListViewStyle(g_list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        SetWindowTheme(g_list, L"Explorer", nullptr);
        g_membar = MemBarCreate(g_inst, hwnd, ID_MEMBAR);
        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
            (HMENU)(INT_PTR)ID_STATUS, g_inst, nullptr);
        ApplyFont();
        SetColumns();
        SetMenu(hwnd, BuildMenu());
        ApplyThemeAll();
        SetTimer(hwnd, TIMER_REFRESH, 1000, nullptr);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        int i = (int)di->CtlID - ID_BTN_FIRST;
        if (i >= 0 && i < kBtnCount) {
            ToolButtonDraw(di, kBtnGlyph[i], TintColor(kBtnTint[i]));
            return TRUE;
        }
        break;
    }
    case WM_PAINT: {
        // Trennstriche zwischen den Gruppen der Symbolleiste
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(hwnd, &ps);
        HBRUSH br = CreateSolidBrush(IsDark() ? Colors().border : GetSysColor(COLOR_3DSHADOW));
        for (int x : g_sepX) {
            RECT r{ x, g_toolTop + S(8), x + std::max(1, S(1)), g_toolBottom - S(8) };
            FillRect(dc, &r, br);
        }
        DeleteObject(br);
        EndPaint(hwnd, &ps);
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
        BuildIcons();
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
        if (nh->code == TTN_GETDISPINFOW) {
            NMTTDISPINFOW* di = (NMTTDISPINFOW*)lp;
            for (int i = 0; i < kBtnCount; i++)
                if ((HWND)di->hdr.idFrom == g_btn[i]) di->lpszText = (LPWSTR)T(kBtnText[i]);
            return 0;
        }
        if (nh->hwndFrom == g_list && nh->code == NM_CUSTOMDRAW) return ListCustomDraw((NMLVCUSTOMDRAW*)lp);
        if (nh->idFrom == ID_LIST) {
            if (nh->code == LVN_ITEMCHANGED) { UpdateButtons(); UpdateStatus(); }
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
            SetWindowPos(hwnd, nullptr, 0, 0, MulDiv(940, dpi, 96), MulDiv(460, dpi, 96), SWP_NOMOVE | SWP_NOZORDER);
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
