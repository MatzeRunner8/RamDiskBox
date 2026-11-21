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
#include <climits>
#include <cstring>
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
    WM_APP_FITCOLS,   // neu am Ende: wWinMain schickt WM_APP_SHOW als Zahl (WM_APP + 3)
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

struct RowInfo { bool mounted = false; unsigned long long used = 0, size = 0; std::wstring image; int comp = 0; };

// Spalten: Breite nach Inhalt (weight 0) oder Anteil am restlichen Platz; nur Bezeichnung und Belegung
// lassen sich von Hand ziehen (Breite in DIP gemerkt, Doppelklick auf den Trenner = wieder automatisch).
struct ColDef { int weight; bool drag; };
const ColDef kCols[COL_COUNT] = { { 0, false }, { 1, true }, { 0, false }, { 1, true }, { 0, false }, { 2, false } };

// Fremde RAM-Laufwerke (nicht von RamDiskBox), fuer die Speicherleiste
struct ForeignDrive { wchar_t letter; const wchar_t* kind; unsigned long long cap, used; };

HWND g_list = nullptr, g_status = nullptr, g_membar = nullptr, g_tip = nullptr;
HWND g_btn[kBtnCount] = {};
HIMAGELIST g_icons = nullptr;
std::vector<int> g_sepX;          // x-Positionen der Trennstriche in der Symbolleiste
std::vector<RowInfo> g_rows;      // Belegung je Zeile (fuer den gezeichneten Balken)
std::vector<ForeignDrive> g_foreign;
int g_userW[COL_COUNT] = {};      // von Hand gezogene Breite in DIP, 0 = automatisch
int g_colMinSum = 0;              // Summe der Mindestbreiten (fuer die Mindestgroesse des Fensters)
bool g_fitPosted = false;
bool g_inFit = false;             // FitColumns setzt gerade Breiten (HDN_ITEMCHANGED nicht beantworten)
bool g_inDpiChange = false;       // waehrend WM_DPICHANGED: Mindestgroesse nicht aus Pixeln der alten DPI
int g_ioW = 0;                    // Breite des I/O-Teils der Statuszeile (fest, damit nichts springt)
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
void UpdateMemBar();
void Layout();
void DetectForeignDrives();

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

const Str kColTitle[COL_COUNT] = { S_COL_DRIVE, S_COL_LABEL, S_COL_SIZE, S_COL_USAGE, S_COL_COMP, S_COL_IMAGE };

int TextW(HDC dc, const std::wstring& s)
{
    if (s.empty()) return 0;
    SIZE z{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &z);
    return z.cx;
}

bool SetCell(int row, int col, const std::wstring& text)
{
    wchar_t cur[1024] = {};
    ListView_GetItemText(g_list, row, col, cur, 1023);
    if (text == cur) return false;
    ListView_SetItemText(g_list, row, col, (LPWSTR)text.c_str());
    return true;
}

// Image-Pfade in der Spalte so kuerzen, dass der Dateiname sichtbar bleibt (C:\...\S.img)
void SetImageCells()
{
    int width = ListView_GetColumnWidth(g_list, COL_IMAGE) - S(14);
    HDC dc = GetDC(g_list);
    HGDIOBJ of = SelectObject(dc, g_font);
    for (size_t i = 0; i < g_rows.size(); i++) {
        std::wstring t = g_rows[i].image.empty() ? std::wstring(T(S_NO_IMAGE)) : g_rows[i].image;
        if (!g_rows[i].image.empty() && t.size() < MAX_PATH && TextW(dc, t) > width) {
            wchar_t buf[MAX_PATH];
            wcscpy_s(buf, t.c_str());
            PathCompactPathW(dc, buf, (UINT)std::max(0, width));
            t = buf;
        }
        SetCell((int)i, COL_IMAGE, t);
    }
    SelectObject(dc, of);
    ReleaseDC(g_list, dc);
}

// Spaltenbreiten: Laufwerk, Groesse und Komprimierung nach Inhalt, der restliche Platz geht nach
// Gewicht an Bezeichnung, Belegung und Image (Image nimmt den Rest). Die Summe ist immer genau die
// Breite der Liste: keine leere Flaeche rechts, Rollbalken nur, wenn nicht einmal die Mindestbreiten passen.
void FitColumns(int avail = -1)
{
    if (!g_list || !g_font) return;
    if (avail < 0) {
        RECT rc;
        GetClientRect(g_list, &rc);   // ohne senkrechten Rollbalken
        avail = rc.right;
    }
    if (avail <= 0) return;   // minimiert bzw. noch ohne Groesse
    HDC dc = GetDC(g_list);
    HGDIOBJ of = SelectObject(dc, g_font);
    const int pad = S(14);
    int ix = 0, iy = 0;
    if (g_icons) ImageList_GetIconSize(g_icons, &ix, &iy);
    int nat[COL_COUNT], mn[COL_COUNT];
    for (int c = 0; c < COL_COUNT; c++) nat[c] = mn[c] = TextW(dc, T(kColTitle[c])) + pad;   // Kopf ist das Minimum
    wchar_t buf[1024];
    int rows = ListView_GetItemCount(g_list);
    for (int r = 0; r < rows; r++) {
        for (int c : { COL_DRIVE, COL_LABEL, COL_SIZE }) {
            ListView_GetItemText(g_list, r, c, buf, 1023);
            nat[c] = std::max(nat[c], TextW(dc, buf) + pad + (c == COL_DRIVE ? ix + S(4) : 0));
        }
        if (r < (int)g_rows.size()) nat[COL_IMAGE] = std::max(nat[COL_IMAGE], TextW(dc, g_rows[r].image) + pad);
    }
    // Komprimierung mit Platzhalter-Prozent messen, damit -9 % -> -10 % nicht jede Sekunde neu verteilt
    const Str lv[] = { S_COMP_OFF, S_COMP_FAST, S_COMP_NORMAL, S_COMP_MAX };
    for (auto& [name, kv] : g_last.sections) {
        int l = std::clamp(ParseCompress(Get(kv, L"compress"), 0), 0, 3);
        std::wstring t = l ? Fmt(T(S_COMP_SAVED), T(lv[l]), 88) : std::wstring(T(S_COMP_OFF));
        nat[COL_COMP] = std::max(nat[COL_COMP], TextW(dc, t) + pad);
    }
    // Belegung: Balken plus laengster moeglicher Text; schmal nur der Text
    int ut = TextW(dc, FormatSize(9999ull << 30) + L" · 100 %");
    nat[COL_USAGE] = std::max(nat[COL_USAGE], S(6) + S(70) + S(8) + ut + S(4));
    mn[COL_USAGE] = std::max(mn[COL_USAGE], S(6) + ut + S(4));
    mn[COL_DRIVE] = nat[COL_DRIVE];   // Buchstabe und Groesse nie kuerzen
    mn[COL_SIZE] = nat[COL_SIZE];
    mn[COL_COMP] = std::min(nat[COL_COMP], std::max(mn[COL_COMP], S(70)));
    mn[COL_LABEL] = std::min(nat[COL_LABEL], std::max(mn[COL_LABEL], S(60)));
    mn[COL_IMAGE] = std::max(mn[COL_IMAGE], S(60));
    SelectObject(dc, of);
    ReleaseDC(g_list, dc);

    int pref[COL_COUNT], w[COL_COUNT];
    long long sp = 0, sm = 0;
    for (int c = 0; c < COL_COUNT; c++) {
        pref[c] = g_userW[c] ? std::max(mn[c], S(g_userW[c])) : nat[c];
        sp += pref[c];
        sm += mn[c];
    }
    g_colMinSum = (int)sm;
    if (avail >= sp) {   // alles passt: Rest nach Gewicht an die automatischen, dehnbaren Spalten
        // Obergrenzen, damit kurze Bezeichnungen und der Balken nicht ausufern; der Rest geht an Image
        int cap[COL_COUNT];
        for (int c = 0; c < COL_COUNT; c++) cap[c] = INT_MAX;
        cap[COL_LABEL] = std::max(nat[COL_LABEL] * 2, S(240));
        cap[COL_USAGE] = std::max(nat[COL_USAGE], S(6) + S(160) + S(8) + ut + S(4));
        int extra = avail - (int)sp, ws = 0;
        for (int c = 0; c < COL_COUNT; c++) if (!g_userW[c]) ws += kCols[c].weight;
        for (int c = 0; c < COL_COUNT; c++) {
            w[c] = pref[c];
            if (ws && !g_userW[c] && kCols[c].weight) w[c] = std::min(cap[c], pref[c] + extra * kCols[c].weight / ws);
        }
    } else if (avail >= sm) {   // zu schmal: jede Spalte anteilig Richtung Minimum
        long long give = sp - sm, have = avail - sm;
        for (int c = 0; c < COL_COUNT; c++) w[c] = mn[c] + (int)(give ? (pref[c] - mn[c]) * have / give : 0);
    } else {
        for (int c = 0; c < COL_COUNT; c++) w[c] = mn[c];   // nur hier ein waagerechter Rollbalken
    }
    if (avail >= sm) {
        int s = 0;
        for (int c = 0; c < COL_COUNT; c++) s += w[c];
        w[COL_IMAGE] += avail - s;   // Rundungsrest: Summe == Breite
    }
    bool changed = false;
    g_inFit = true;
    for (int c = 0; c < COL_COUNT; c++)
        if (ListView_GetColumnWidth(g_list, c) != w[c]) { ListView_SetColumnWidth(g_list, c, w[c]); changed = true; }
    g_inFit = false;
    if (changed) SetImageCells();
}

// Zieht der Benutzer gerade eine Spaltengrenze? Dann nicht dazwischen neu einpassen.
bool HeaderDragging()
{
    return g_list && GetCapture() == ListView_GetHeader(g_list);
}

void PostFit()
{
    if (!g_fitPosted && g_main) {
        g_fitPosted = true;
        PostMessageW(g_main, WM_APP_FITCOLS, 0, 0);
    }
}

void SaveColumnWidths()
{
    GuiIni().Set(L"Window", L"Cols", Fmt(L"%d,%d", g_userW[COL_LABEL], g_userW[COL_USAGE]));
}

void LoadColumnWidths()
{
    std::vector<std::wstring> p = Split(GuiIni().Get(L"Window", L"Cols", L""), L',');
    if (p.size() == 2) {
        g_userW[COL_LABEL] = std::clamp(_wtoi(p[0].c_str()), 0, 2000);
        g_userW[COL_USAGE] = std::clamp(_wtoi(p[1].c_str()), 0, 2000);
    }
}

void SetColumns()
{
    const int fmt[] = { LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_RIGHT, LVCFMT_LEFT, LVCFMT_LEFT, LVCFMT_LEFT };
    HWND hdr = ListView_GetHeader(g_list);
    int have = hdr ? Header_GetItemCount(hdr) : 0;
    for (int i = 0; i < COL_COUNT; i++) {
        LVCOLUMNW c{};
        c.mask = LVCF_TEXT | LVCF_FMT | LVCF_WIDTH;
        c.pszText = (LPWSTR)T(kColTitle[i]);
        // Kein LVCFMT_FIXED_WIDTH: damit ignoriert die Liste auch LVM_SETCOLUMNWIDTH. Ziehen der festen
        // Spalten verhindert stattdessen HDN_BEGINTRACK (WM_NOTIFY), den Mauszeiger HeaderCursorProc.
        c.fmt = fmt[i];
        c.cx = S(60);
        if (i < have) {
            c.mask = LVCF_TEXT | LVCF_FMT;
            ListView_SetColumn(g_list, i, &c);
        } else {
            ListView_InsertColumn(g_list, i, &c);
        }
    }
    FitColumns();
}

// Kopfzeile: ueber den Trennern fester Spalten kein Pfeil zum Ziehen
LRESULT CALLBACK HeaderCursorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_SETCURSOR && LOWORD(lp) == HTCLIENT) {
        HDHITTESTINFO ht{};
        GetCursorPos(&ht.pt);
        ScreenToClient(h, &ht.pt);
        int item = (int)SendMessageW(h, HDM_HITTEST, 0, (LPARAM)&ht);
        if (item >= 0 && item < COL_COUNT && (ht.flags & (HHT_ONDIVIDER | HHT_ONDIVOPEN)) && !kCols[item].drag) {
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            return TRUE;
        }
    }
    if (msg == WM_CAPTURECHANGED) PostFit();   // Ziehen beendet oder abgebrochen (Esc)
    return DefSubclassProc(h, msg, wp, lp);
}

// Spalten nachziehen, wenn der senkrechte Rollbalken der Liste kommt oder geht
LRESULT CALLBACK ListSizeProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (msg == WM_SIZE && !HeaderDragging()) PostFit();
    return DefSubclassProc(h, msg, wp, lp);
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
    bool refit = !same;
    std::vector<std::wstring> oldImages;
    std::vector<int> oldComp;
    for (auto& r : g_rows) { oldImages.push_back(r.image); oldComp.push_back(r.comp); }
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
        refit |= SetCell(row, COL_LABEL, label);
        refit |= SetCell(row, COL_SIZE, FormatSize(ToU64(Get(kv, L"size"))));
        int comp = std::clamp(ParseCompress(Get(kv, L"compress"), 0), 0, 3);
        g_rows[i] = { mounted, ToU64(Get(kv, L"used")), ToU64(Get(kv, L"size")), Get(kv, L"image"), comp };
        if (i >= oldImages.size() || oldImages[i] != g_rows[i].image || oldComp[i] != comp) refit = true;
        // Balken und Text zeichnet ListCustomDraw; ausgehaengt: Strich
        SetCell(row, COL_USAGE, mounted && g_rows[i].size ? L"" : T(S_NO_IMAGE));
        {
            int lvl = ParseCompress(Get(kv, L"compress"), 0);
            const Str names[] = { S_COMP_OFF, S_COMP_FAST, S_COMP_NORMAL, S_COMP_MAX };
            std::wstring ct = T(names[lvl < 0 || lvl > 3 ? 0 : lvl]);
            unsigned long long raw = ToU64(Get(kv, L"compraw")), stored = ToU64(Get(kv, L"compstored"));
            if (mounted && raw > 0) ct = Fmt(T(S_COMP_SAVED), ct.c_str(), (int)((raw > stored ? raw - stored : 0) * 100 / raw));
            SetCell(row, COL_COMP, ct);
        }
    }
    if (refit) {
        if (HeaderDragging()) PostFit();   // nach dem Ziehen (WM_CAPTURECHANGED) nachholen
        else FitColumns();
    }
    SetImageCells();
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
        std::wstring t = FormatSize(g_rows[row].used) + L" · " + std::to_wstring(pct) + L" %";
        // Balken waechst mit der Spalte (bis 160 DIP); ist kaum Platz, nur der Text
        int cellW = (int)(cell.right - cell.left);
        int barW = std::clamp(cellW - S(6) - S(8) - S(4) - TextW(dc, t), 0, S(160)), barH = S(8);
        if (barW < S(24)) barW = 0;
        RECT bar{ cell.left + S(6), (cell.top + cell.bottom - barH) / 2, 0, 0 };
        bar.right = bar.left + barW;
        bar.bottom = bar.top + barH;
        bool dark = IsDark();
        if (barW > 0) {
            HBRUSH track = CreateSolidBrush(dark ? RGB(62, 62, 62) : RGB(222, 222, 222));
            FillRect(dc, &bar, track);
            DeleteObject(track);
            RECT fill = bar;
            fill.right = bar.left + std::max(pct > 0 ? 2 : 0, barW * pct / 100);
            HBRUSH fb = CreateSolidBrush(pct >= 95 ? TintColor(Tint::Red) : pct >= 80 ? TintColor(Tint::Amber) : TintColor(Tint::Green));
            FillRect(dc, &fill, fb);
            DeleteObject(fb);
        }
        RECT tr{ barW ? bar.right + S(8) : cell.left + S(6), cell.top, cell.right - S(4), cell.bottom };
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
    auto tw = [&](const std::wstring& s) { return TextW(dc, s) + S(24); };
    // I/O-Teil mit Platzhalter messen, damit die Grenzen bei wechselnden Raten nicht springen
    if (!g_ioW) g_ioW = tw(Fmt(T(S_IO), FormatSize(888ull << 20).c_str(), FormatSize(888ull << 20).c_str()));
    RECT src{};
    GetClientRect(g_status, &src);
    int want0 = tw(p0), w1 = tw(p1), w2 = g_ioW;
    // Platz fuer die Laufwerksangaben hat Vorrang: erst die Betriebsart weglassen, dann die Rate kuerzen
    int avail = (int)src.right - GetSystemMetricsForDpi(SM_CXVSCROLL, g_dpi);
    bool show1 = avail - w1 - w2 >= std::min(want0, S(260));
    if (!show1) w1 = 0;
    if (avail - w1 - w2 < std::min(want0, S(180))) w2 = std::max(S(90), avail - S(180));
    int w0 = std::max(S(80), avail - w1 - w2);
    // Selbst mit "..." kuerzen: die helle (native) Statuszeile schneidet sonst einfach ab
    auto ellipsize = [&](std::wstring& s, int width) {
        if (TextW(dc, s) <= width) return;
        while (!s.empty() && TextW(dc, s + L"…") > width) s.pop_back();
        s += L"…";
    };
    std::wstring full0 = p0;
    ellipsize(p0, w0 - S(16));
    ellipsize(p2, w2 - S(16));
    SelectObject(dc, of);
    ReleaseDC(g_status, dc);
    int cnt = show1 ? 3 : 2;
    int parts[3] = { w0, show1 ? w0 + w1 : -1, -1 };
    int old[3] = {};
    int oldCnt = (int)SendMessageW(g_status, SB_GETPARTS, 3, (LPARAM)old);
    if (oldCnt != cnt || memcmp(old, parts, cnt * sizeof(int)) != 0) SendMessageW(g_status, SB_SETPARTS, cnt, (LPARAM)parts);
    auto setText = [&](int part, const std::wstring& s) {
        wchar_t cur[512] = {};
        if (LOWORD(SendMessageW(g_status, SB_GETTEXTLENGTHW, part, 0)) < 511) SendMessageW(g_status, SB_GETTEXTW, part, (LPARAM)cur);
        if (s != cur) SendMessageW(g_status, SB_SETTEXTW, part, (LPARAM)s.c_str());
    };
    setText(0, p0);
    if (g_tip) {   // ungekuerzter Text als Kurzinfo, nur wenn gekuerzt wurde
        static std::wstring tipText;
        static RECT tipRect{};
        TTTOOLINFOW ti{ sizeof ti };
        ti.hwnd = g_status;
        ti.uId = 1;
        SendMessageW(g_status, SB_GETRECT, 0, (LPARAM)&ti.rect);
        if (!EqualRect(&ti.rect, &tipRect)) {
            tipRect = ti.rect;
            SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
        }
        std::wstring want = p0 != full0 ? full0 : std::wstring();
        if (want != tipText) {
            tipText = want;
            ti.lpszText = (LPWSTR)tipText.c_str();
            SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
        }
    }
    if (show1) {
        setText(1, p1);
        setText(2, p2);
    } else {
        setText(1, p2);
    }
    UpdateMemBar();
}

// Speicherleiste. RAM-Laufwerke = tatsaechlich belegter Arbeitsspeicher aller RAM-Laufwerke (eigene und
// fremde, z. B. SoftPerfect), Reserviert = noch freie Kapazitaet der eingehaengten RAM-Laufwerke, Frei =
// verfuegbarer Speicher abzueglich Reserve, Programme = Rest des belegten Speichers.
void UpdateMemBar()
{
    if (!g_membar) return;
    unsigned long long total = ToU64(g_last.Get(L"ramtotal")), avail = ToU64(g_last.Get(L"ramfree"));
    if (!g_online || !total) {
        MEMORYSTATUSEX ms{ sizeof ms };
        GlobalMemoryStatusEx(&ms);
        total = ms.ullTotalPhys;
        avail = ms.ullAvailPhys;
    }
    unsigned long long used = 0, reserved = 0;
    std::wstring drives;
    for (auto& [name, kv] : g_last.sections) {
        if (name.empty()) continue;
        std::wstring st = Get(kv, L"state");
        unsigned long long size = ToU64(Get(kv, L"size"));
        if (st == L"mounted" || st == L"saving") {
            unsigned long long cap = ToU64(Get(kv, L"capacity"));
            if (!cap) cap = size;
            unsigned long long mem = ToU64(Get(kv, L"memory"));
            used += mem;
            // schlimmstenfalls waechst ein Laufwerk bis zu seiner Groesse (auch komprimierte Daten, wenn
            // sie durch nicht komprimierbare ersetzt werden): Reserve = Kapazitaet minus belegter RAM
            reserved += cap > mem ? cap - mem : 0;
            drives += L"\n      " + Fmt(T(S_MEM_TIP_DRIVE), name[0], L"RamDiskBox", FormatSize(cap).c_str(), FormatSize(mem).c_str());
        } else {
            drives += L"\n      " + Fmt(T(S_MEM_TIP_DISMOUNTED), name[0], FormatSize(size).c_str());
        }
    }
    for (auto& f : g_foreign) {
        used += f.used;
        reserved += f.cap > f.used ? f.cap - f.used : 0;
        drives += L"\n      " + Fmt(T(S_MEM_TIP_DRIVE), f.letter, f.kind, FormatSize(f.cap).c_str(), FormatSize(f.used).c_str());
    }
    unsigned long long inUse = total > avail ? total - avail : 0;
    unsigned long long programs = inUse > used ? inUse - used : 0;
    bool over = reserved > avail;
    unsigned long long freeMem = over ? 0 : avail - reserved, overBy = over ? reserved - avail : 0;
    bool low = !over && reserved > 0 && freeMem < std::max(2ull << 30, total / 20);
    bool dark = IsDark();
    std::vector<MemSegment> segs(4);
    segs[0].value = used;
    segs[0].fill = dark ? RGB(47, 94, 160) : RGB(52, 112, 196);
    segs[0].longText = Fmt(T(S_MEM_DISKS), FormatSize(used).c_str());
    segs[0].shortText = FormatSize(used);
    segs[1].value = reserved;
    segs[1].hatched = true;
    if (over) {
        segs[1].fill = dark ? RGB(86, 36, 32) : RGB(244, 200, 194);
        segs[1].hatch = dark ? RGB(122, 52, 46) : RGB(230, 166, 156);
        segs[1].text = dark ? RGB(250, 226, 222) : RGB(120, 30, 20);
        segs[1].longText = Fmt(T(S_MEM_OVER), FormatSize(reserved).c_str(), FormatSize(overBy).c_str());
    } else {
        segs[1].fill = dark ? RGB(31, 52, 84) : RGB(184, 208, 240);
        segs[1].hatch = dark ? RGB(44, 76, 124) : RGB(156, 188, 232);
        segs[1].text = dark ? RGB(226, 234, 246) : RGB(20, 52, 100);
        segs[1].longText = Fmt(T(S_MEM_RESERVED), FormatSize(reserved).c_str());
    }
    segs[1].shortText = FormatSize(reserved);
    segs[2].value = freeMem;
    segs[2].fill = low ? (dark ? RGB(150, 100, 20) : RGB(176, 112, 0)) : (dark ? RGB(36, 120, 66) : RGB(46, 145, 82));
    segs[2].longText = Fmt(T(S_MEM_FREE), FormatSize(freeMem).c_str());
    segs[2].shortText = FormatSize(freeMem);
    segs[3].value = programs;
    segs[3].fill = dark ? RGB(78, 78, 78) : RGB(128, 128, 128);
    segs[3].longText = Fmt(T(S_MEM_OTHER), FormatSize(programs).c_str());
    segs[3].shortText = FormatSize(programs);

    std::wstring tip = Fmt(T(S_MEM_TIP_TOTAL), FormatSize(total).c_str());
    tip += L"\n" + Fmt(T(S_MEM_TIP_DISKS), FormatSize(used).c_str(), FormatSize(reserved).c_str()) + drives;
    tip += L"\n" + Fmt(T(S_MEM_TIP_FREE), FormatSize(freeMem).c_str(), FormatSize(avail).c_str());
    if (over) tip += L"\n" + Fmt(T(S_MEM_TIP_OVER), FormatSize(overBy).c_str());
    tip += L"\n" + Fmt(T(S_MEM_TIP_OTHER), FormatSize(programs).c_str());
    if (!g_online) tip += L"\n" + std::wstring(T(S_MEM_TIP_OFFLINE));
    tip += L"\n\n" + std::wstring(T(S_MEM_TIP_LEGEND));
    MemBarSet(g_membar, segs, tip);
}

// ----------------------------------------------------------------------- fremde RAM-Laufwerke

// Gehoert das Laufwerk zum Dateisystemtreiber driver (z. B. \FileSystem\ramfs)? Ohne Administratorrechte.
bool DriverInPath(wchar_t letter, const wchar_t* driver)
{
    using NtQVIF = LONG(NTAPI*)(HANDLE, PVOID, PVOID, ULONG, int);
    static NtQVIF fn = (NtQVIF)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryVolumeInformationFile");
    if (!fn) return false;
    wchar_t root[] = { letter, L':', L'\\', 0 };
    HANDLE h = CreateFileW(root, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    struct { BOOLEAN inPath; ULONG nameLen; WCHAR name[64]; } info{};   // FILE_FS_DRIVER_PATH_INFORMATION
    wcscpy_s(info.name, driver);
    info.nameLen = (ULONG)(wcslen(driver) * sizeof(WCHAR));
    struct { ULONG_PTR status; ULONG_PTR information; } iosb{};          // IO_STATUS_BLOCK
    LONG st = fn(h, &iosb, &info, sizeof info, 9 /* FileFsDriverPathInformation */);
    CloseHandle(h);
    return st >= 0 && info.inPath;
}

// Eigenes, eingehaengtes Laufwerk auf diesem Buchstaben? (Ein ausgehaengtes eigenes Laufwerk belegt den
// Buchstaben nicht; dort kann ein fremdes RAM-Laufwerk liegen.)
bool OwnMounted(wchar_t l)
{
    const KV* kv = FindVolume(l);
    if (!kv) return false;
    std::wstring st = Get(*kv, L"state");
    return st == L"mounted" || st == L"saving";
}

// SoftPerfect RAM Disk (Dateisystemtreiber ramfs): Kandidaten aus seiner Konfiguration, nur feste bzw.
// Wechsel-Laufwerke mit virtuellem Volume (\Device\Volume{...}), bestaetigt ueber den Treiber; belegter
// RAM = belegter Platz. Netz- und andere Laufwerke werden nie geoeffnet (koennten haengen).
void DetectForeignDrives()
{
    // Treiberpruefung nur, wenn sich die Laufwerke geaendert haben oder alle 30 s (nicht jede Sekunde
    // Handles auf fremde virtuelle Laufwerke oeffnen); die Belegung wird bei jedem Aufruf neu gelesen.
    static DWORD lastMask = 0;
    static unsigned long long lastScan = 0;
    static bool ramfs[26] = {};
    DWORD mask = GetLogicalDrives();
    unsigned long long now = GetTickCount64();
    if (mask != lastMask || now - lastScan > 30000) {
        lastMask = mask;
        lastScan = now;
        bool cand[26] = {};
        HKEY k;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\ramfs\\Parameters\\Volumes", 0, KEY_READ, &k) == ERROR_SUCCESS) {
            wchar_t name[64];
            for (DWORD i = 0;; i++) {
                DWORD n = 64;
                if (RegEnumKeyExW(k, i, name, &n, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
                wchar_t l = towupper(name[0]);
                if (n == 1 && l >= L'A' && l <= L'Z') cand[l - L'A'] = true;
            }
            RegCloseKey(k);
        }
        DWORD oldMode = 0;
        SetThreadErrorMode(SEM_FAILCRITICALERRORS, &oldMode);
        for (int i = 0; i < 26; i++) {
            wchar_t l = (wchar_t)(L'A' + i);
            ramfs[i] = false;
            if (!cand[i] || !(mask & (1u << i)) || OwnMounted(l)) continue;
            wchar_t root[] = { l, L':', L'\\', 0 }, dev[] = { l, L':', 0 }, target[512] = {};
            UINT type = GetDriveTypeW(root);
            if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_RAMDISK) continue;
            if (!QueryDosDeviceW(dev, target, 511) || wcsncmp(target, L"\\Device\\Volume{", 15) != 0) continue;
            ramfs[i] = DriverInPath(l, L"\\FileSystem\\ramfs");
        }
        SetThreadErrorMode(oldMode, nullptr);
    }
    std::vector<ForeignDrive> found;
    DWORD oldMode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &oldMode);
    for (int i = 0; i < 26; i++) {
        wchar_t l = (wchar_t)(L'A' + i);
        if (!ramfs[i] || OwnMounted(l)) continue;   // eigene Laufwerke nicht doppelt
        wchar_t root[] = { l, L':', L'\\', 0 };
        ULARGE_INTEGER totalBytes{}, freeBytes{};
        if (!GetDiskFreeSpaceExW(root, nullptr, &totalBytes, &freeBytes)) continue;
        found.push_back({ l, L"SoftPerfect", totalBytes.QuadPart,
                          totalBytes.QuadPart > freeBytes.QuadPart ? totalBytes.QuadPart - freeBytes.QuadPart : 0 });
    }
    SetThreadErrorMode(oldMode, nullptr);
    g_foreign.swap(found);
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
    DetectForeignDrives();
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
    g_ioW = 0;
    SetColumns();
    UpdateList();
    UpdateStatus();
    Layout();
}

// ----------------------------------------------------------------------- Layout

void ApplyFont()
{
    HFONT old = g_font;
    NONCLIENTMETRICSW ncm{ sizeof ncm };
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, g_dpi);
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    SendMessageW(g_list, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_status, WM_SETFONT, (WPARAM)g_font, TRUE);
    SendMessageW(g_membar, WM_SETFONT, (WPARAM)g_font, TRUE);
    if (old) DeleteObject(old);   // erst loeschen, wenn kein Steuerelement sie mehr benutzt
    g_ioW = 0;
}

int ToolbarWidth()
{
    int w = S(8);
    for (int i = 0; i < kBtnCount; i++) w += S(42) + (i < kBtnCount - 1 ? S(4) : 0) + (kSepAfter[i] ? S(14) : 0);
    return w + S(8);
}

// Aufbau: Symbolleiste | Liste | Speicherleiste | Statuszeile
void Layout()
{
    RECT rc;
    GetClientRect(g_main, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;   // minimiert
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
    if (g_font) {   // Speicherleiste mindestens Schrifthoehe plus Rand (groessere Textanzeige)
        HDC dc = GetDC(g_main);
        HGDIOBJ of = SelectObject(dc, g_font);
        TEXTMETRICW tm{};
        GetTextMetricsW(dc, &tm);
        SelectObject(dc, of);
        ReleaseDC(g_main, dc);
        barH = std::max(barH, (int)tm.tmHeight + S(12));
    }
    int barTop = (int)rc.bottom - sh - S(8) - barH;
    int listW = rc.right - 2 * m;
    // Wird die Liste schmaler, Spalten vorher anpassen, damit kein Rollbalken aufblitzt
    RECT lw{}, lc{};
    GetWindowRect(g_list, &lw);
    GetClientRect(g_list, &lc);
    int chrome = (lw.right - lw.left) - lc.right;
    if (listW < lw.right - lw.left) FitColumns(listW - chrome);
    MoveWindow(g_list, m, top, listW, std::max(0, barTop - S(8) - top), TRUE);
    MoveWindow(g_membar, m, barTop, listW, barH, TRUE);
    FitColumns();
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
    UpdateMemBar();   // Farben der Abschnitte haengen von Hell/Dunkel ab
    InvalidateRect(g_membar, nullptr, FALSE);
    FitColumns();
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
        // Textgroesse in Windows geaendert ("Text vergroessern")
        if (msg == WM_SETTINGCHANGE && wp == SPI_SETNONCLIENTMETRICS && g_list) {
            ApplyFont();
            Layout();
        }
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
        SetWindowSubclass(g_list, ListSizeProc, 2, 0);
        if (HWND hdr = ListView_GetHeader(g_list)) SetWindowSubclass(hdr, HeaderCursorProc, 3, 0);
        g_membar = MemBarCreate(g_inst, hwnd, ID_MEMBAR);
        g_status = CreateWindowExW(0, STATUSCLASSNAMEW, L"", WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP, 0, 0, 0, 0, hwnd,
            (HMENU)(INT_PTR)ID_STATUS, g_inst, nullptr);
        {   // Kurzinfo fuer gekuerzten Text im linken Teil der Statuszeile (Rechteck setzt UpdateStatus)
            TTTOOLINFOW ti{ sizeof ti };
            ti.uFlags = TTF_SUBCLASS;
            ti.hwnd = g_status;
            ti.uId = 1;
            ti.lpszText = (LPWSTR)L"";
            SendMessageW(g_tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        }
        ApplyFont();
        LoadColumnWidths();
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
        if (wp != SIZE_MINIMIZED) Layout();
        return 0;
    case WM_DPICHANGED: {
        // erst Schrift und Symbole fuer die neue DPI, dann die neue Fenstergroesse (Layout passt Spalten an)
        g_dpi = HIWORD(wp);
        ApplyFont();
        BuildIcons();
        RECT* r = (RECT*)lp;
        g_inDpiChange = true;   // WM_GETMINMAXINFO darf hier keine Pixelwerte der alten DPI nehmen
        SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        g_inDpiChange = false;
        Layout();
        return 0;
    }
    case WM_GETMINMAXINFO: {
        // Mindestgroesse aus dem Inhalt: Symbolleiste bzw. Mindestbreiten der Spalten, Kopf + drei Zeilen
        MINMAXINFO* mm = (MINMAXINFO*)lp;
        UINT dpi = GetDpiForWindow(hwnd);
        if (!g_list || !g_status || dpi != g_dpi || g_inDpiChange) {   // vor WM_CREATE bzw. waehrend eines DPI-Wechsels
            UINT d = g_list ? g_dpi : dpi;
            mm->ptMinTrackSize.x = MulDiv(480, (int)d, 96);
            mm->ptMinTrackSize.y = MulDiv(300, (int)d, 96);
            return 0;
        }
        RECT lw{}, lc{}, hr{}, sr{}, ir{};
        GetWindowRect(g_list, &lw);
        GetClientRect(g_list, &lc);
        GetWindowRect(ListView_GetHeader(g_list), &hr);
        GetWindowRect(g_status, &sr);
        int chromeX = (lw.right - lw.left) - lc.right + GetSystemMetricsForDpi(SM_CXVSCROLL, dpi);
        int rowH = ListView_GetItemCount(g_list) > 0 && ListView_GetItemRect(g_list, 0, &ir, LVIR_BOUNDS) ? ir.bottom - ir.top : S(24);
        RECT mb{};
        GetClientRect(g_membar, &mb);
        int cw = std::max(ToolbarWidth(), g_colMinSum + chromeX + 2 * S(8));
        int ch = g_toolBottom + S(6) + (hr.bottom - hr.top) + 3 * rowH + ((lw.bottom - lw.top) - lc.bottom) + S(8) +
                 std::max((int)mb.bottom, S(30)) + S(8) + (sr.bottom - sr.top);
        RECT r{ 0, 0, cw, ch };
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, TRUE, 0, dpi);
        mm->ptMinTrackSize.x = r.right - r.left;
        mm->ptMinTrackSize.y = r.bottom - r.top;
        return 0;
    }
    case WM_APP_FITCOLS:
        g_fitPosted = false;
        if (!HeaderDragging()) FitColumns();   // sonst nach dem Ziehen (WM_CAPTURECHANGED)
        return 0;
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
        // Spaltenkopf: nur Bezeichnung und Belegung lassen sich ziehen; Doppelklick = wieder automatisch
        if (g_list && nh->hwndFrom == ListView_GetHeader(g_list)) {
            NMHEADERW* hd = (NMHEADERW*)lp;
            bool ok = hd->iItem >= 0 && hd->iItem < COL_COUNT && kCols[hd->iItem].drag;
            switch (nh->code) {
            case HDN_BEGINTRACKW:
            case HDN_BEGINTRACKA:
                return ok ? FALSE : TRUE;
            case HDN_ENDTRACKW:
            case HDN_ENDTRACKA:
                if (ok && hd->pitem && (hd->pitem->mask & HDI_WIDTH)) {
                    g_userW[hd->iItem] = std::max(1, MulDiv(hd->pitem->cxy, 96, (int)g_dpi));
                    SaveColumnWidths();
                }
                PostMessageW(hwnd, WM_APP_FITCOLS, 0, 0);
                return 0;
            case HDN_DIVIDERDBLCLICKW:
            case HDN_DIVIDERDBLCLICKA:
                if (ok) {
                    g_userW[hd->iItem] = 0;
                    SaveColumnWidths();
                }
                PostMessageW(hwnd, WM_APP_FITCOLS, 0, 0);
                return TRUE;   // kein automatisches Anpassen durch die Liste
            case HDN_ITEMCHANGEDW:
            case HDN_ITEMCHANGEDA:
                // Breite von aussen geaendert (z. B. Strg+Plus passt alle Spalten an): wieder einpassen
                if (!g_inFit && !HeaderDragging() && hd->pitem && (hd->pitem->mask & HDI_WIDTH)) PostFit();
                return 0;
            }
        }
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
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, title.c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
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
