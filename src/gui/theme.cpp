// RamDiskBox - Hell-/Dunkel-Darstellung der Oberflaeche
#include "theme.h"
#include <commctrl.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <dwmapi.h>
#include <vector>
#include <algorithm>

namespace gui {

namespace {

// Undokumentierte, seit Windows 10 1903 unveraenderte uxtheme-Funktionen (nur ueber Ordinal);
// fehlen sie, bleiben nur Kontextmenues hell, alles andere funktioniert trotzdem.
enum { AppModeDefault = 0, AppModeAllowDark = 1, AppModeForceDark = 2, AppModeForceLight = 3 };
using FnSetPreferredAppMode = int(WINAPI*)(int);
using FnAllowDarkModeForWindow = BOOL(WINAPI*)(HWND, BOOL);
using FnVoid = void(WINAPI*)();

FnSetPreferredAppMode pSetPreferredAppMode = nullptr;
FnAllowDarkModeForWindow pAllowDarkModeForWindow = nullptr;
FnVoid pFlushMenuThemes = nullptr;
FnVoid pRefreshImmersiveColorPolicyState = nullptr;

ThemeMode g_mode = ThemeMode::System;
bool g_dark = false;
constexpr wchar_t kDimProp[] = L"RamDiskBox.Dim";   // Beschriftung grau zeichnen (siehe ThemeEnableLabel)
Palette g_pal{};
HBRUSH g_brBg, g_brList, g_brEdit, g_brBorder, g_brStatus, g_brMenuHot, g_brMenuPressed, g_brFooter;
HTHEME g_menuTheme = nullptr;

// Menueleiste selbst zeichnen (undokumentierte "UAH"-Nachrichten, wie Notepad++ und der Explorer)
constexpr UINT WM_UAHDRAWMENU = 0x0091;
constexpr UINT WM_UAHDRAWMENUITEM = 0x0092;
struct UAHMENU { HMENU hmenu; HDC hdc; DWORD dwFlags; };
union UAHMENUITEMMETRICS {
    struct { DWORD cx, cy; } rgsizeBar[2];
    struct { DWORD cx, cy; } rgsizePopup[4];
};
struct UAHMENUPOPUPMETRICS { DWORD rgcx[4]; DWORD fUpdateMaxWidths : 2; };
struct UAHMENUITEM { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; };
struct UAHDRAWMENUITEM { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; };

DWORD WindowsBuild()
{
    using Fn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto f = (Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    OSVERSIONINFOW v{ sizeof v };
    return (f && f(&v) == 0) ? v.dwBuildNumber : 0;
}

bool WindowsPrefersDark()
{
    DWORD v = 1, sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
            L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

bool HighContrast()
{
    HIGHCONTRASTW hc{ sizeof hc };
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof hc, &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool Resolve()
{
    if (HighContrast()) return false;
    if (g_mode == ThemeMode::Dark) return true;
    if (g_mode == ThemeMode::Light) return false;
    return WindowsPrefersDark();
}

void ApplyAppMode()
{
    if (pSetPreferredAppMode) {
        pSetPreferredAppMode(g_dark ? AppModeForceDark : AppModeForceLight);
        if (pFlushMenuThemes) pFlushMenuThemes();
    }
    if (g_menuTheme) { CloseThemeData(g_menuTheme); g_menuTheme = nullptr; }
}

int Scale(HWND h, int px) { return MulDiv(px, (int)GetDpiForWindow(h), 96); }

bool ClassIs(HWND h, const wchar_t* name)
{
    wchar_t b[64];
    return GetClassNameW(h, b, 64) && _wcsicmp(b, name) == 0;
}

std::wstring TextOf(HWND h)
{
    int n = GetWindowTextLengthW(h);
    std::vector<wchar_t> b(n + 1);
    GetWindowTextW(h, b.data(), n + 1);
    return b.data();
}

bool IsCheckOrRadio(HWND h)
{
    if (!ClassIs(h, WC_BUTTONW)) return false;
    LONG t = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    return t == BS_CHECKBOX || t == BS_AUTOCHECKBOX || t == BS_3STATE || t == BS_AUTO3STATE ||
           t == BS_RADIOBUTTON || t == BS_AUTORADIOBUTTON;
}

void Line(HDC dc, int x, int y, int w, int h, HBRUSH br)
{
    RECT r{ x, y, x + w, y + h };
    FillRect(dc, &r, br);
}

// ----------------------------------------------------------------------- eigene Zeichenroutinen

// Kontrollkaestchen/Optionsfeld: dunkles Symbol aus dem Windows-Design, Text in heller Farbe
LRESULT DrawCheck(NMCUSTOMDRAW* cd)
{
    if (cd->dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;
    HWND h = cd->hdr.hwndFrom;
    HDC dc = cd->hdc;
    LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
    bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    LRESULT chk = SendMessageW(h, BM_GETCHECK, 0, 0);
    bool disabled = !IsWindowEnabled(h);
    int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;
    int state = radio ? (chk == BST_CHECKED ? RBS_CHECKEDNORMAL : RBS_UNCHECKEDNORMAL)
                      : (chk == BST_CHECKED ? CBS_CHECKEDNORMAL : chk == BST_INDETERMINATE ? CBS_MIXEDNORMAL : CBS_UNCHECKEDNORMAL);
    if (disabled) state += 3;
    else if (cd->uItemState & CDIS_SELECTED) state += 2;
    else if (cd->uItemState & CDIS_HOT) state += 1;

    RECT rc = cd->rc;
    FillRect(dc, &rc, g_brBg);
    HTHEME th = OpenThemeData(h, L"Button");
    SIZE gs{ Scale(h, 13), Scale(h, 13) };
    if (th) GetThemePartSize(th, dc, part, state, nullptr, TS_DRAW, &gs);
    RECT g{ rc.left, rc.top + (rc.bottom - rc.top - gs.cy) / 2, rc.left + gs.cx, 0 };
    g.bottom = g.top + gs.cy;
    if (th) DrawThemeBackground(th, dc, part, state, &g, nullptr);
    else DrawFrameControl(dc, &g, DFC_BUTTON, (radio ? DFCS_BUTTONRADIO : DFCS_BUTTONCHECK) | (chk == BST_CHECKED ? DFCS_CHECKED : 0));
    if (th) CloseThemeData(th);

    std::wstring t = TextOf(h);
    HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, disabled ? g_pal.disabled : g_pal.text);
    LRESULT ui = SendMessageW(h, WM_QUERYUISTATE, 0, 0);
    UINT dt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | ((ui & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
    RECT tr = rc;
    tr.left = g.right + Scale(h, 5);
    DrawTextW(dc, t.c_str(), -1, &tr, dt);
    if ((cd->uItemState & CDIS_FOCUS) && !(ui & UISF_HIDEFOCUS)) {
        RECT fr = tr;
        DrawTextW(dc, t.c_str(), -1, &fr, dt | DT_CALCRECT);
        int dy = ((tr.bottom - tr.top) - (fr.bottom - fr.top)) / 2;
        OffsetRect(&fr, 0, dy);
        InflateRect(&fr, 2, 1);
        SetTextColor(dc, g_pal.text);
        SetBkColor(dc, g_pal.bg);
        DrawFocusRect(dc, &fr);
    }
    SelectObject(dc, of);
    return CDRF_SKIPDEFAULT;
}

// Gruppenrahmen: Linie und Ueberschrift selbst zeichnen (das Design zeichnet die Schrift schwarz)
LRESULT CALLBACK GroupProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (g_dark && msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        TEXTMETRICW tm;
        GetTextMetricsW(dc, &tm);
        int top = rc.top + tm.tmHeight / 2;
        Line(dc, rc.left, top, rc.right - rc.left, 1, g_brBorder);
        Line(dc, rc.left, rc.bottom - 1, rc.right - rc.left, 1, g_brBorder);
        Line(dc, rc.left, top, 1, rc.bottom - top, g_brBorder);
        Line(dc, rc.right - 1, top, 1, rc.bottom - top, g_brBorder);
        std::wstring t = TextOf(h);
        if (!t.empty()) {
            SIZE sz{};
            GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
            RECT tr{ rc.left + Scale(h, 6), rc.top, rc.left + Scale(h, 6) + sz.cx + Scale(h, 6), rc.top + tm.tmHeight };
            FillRect(dc, &tr, g_brBg);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, IsWindowEnabled(h) ? g_pal.text : g_pal.disabled);
            tr.left += Scale(h, 3);
            DrawTextW(dc, t.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
        }
        SelectObject(dc, of);
        EndPaint(h, &ps);
        return 0;
    }
    if (g_dark && msg == WM_ERASEBKGND) return 1;
    return DefSubclassProc(h, msg, wp, lp);
}

// Statuszeile: Hintergrund, Teiltexte, Trennstriche und Griff selbst zeichnen
LRESULT CALLBACK StatusProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (g_dark && msg == WM_ERASEBKGND) return 1;
    if (g_dark && msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC wdc = BeginPaint(h, &ps);
        RECT rc;
        GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(wdc);
        HBITMAP bmp = CreateCompatibleBitmap(wdc, std::max(1L, rc.right), std::max(1L, rc.bottom));
        HGDIOBJ ob = SelectObject(dc, bmp);
        FillRect(dc, &rc, g_brStatus);
        Line(dc, 0, 0, rc.right, 1, g_brBorder);
        HGDIOBJ of = SelectObject(dc, (HFONT)SendMessageW(h, WM_GETFONT, 0, 0));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, g_pal.text);
        int n = (int)SendMessageW(h, SB_GETPARTS, 0, 0);
        for (int i = 0; i < n; i++) {
            RECT pr{};
            SendMessageW(h, SB_GETRECT, i, (LPARAM)&pr);
            int len = LOWORD(SendMessageW(h, SB_GETTEXTLENGTHW, i, 0));
            std::vector<wchar_t> t(len + 1);
            SendMessageW(h, SB_GETTEXTW, i, (LPARAM)t.data());
            RECT tr = pr;
            tr.left += Scale(h, 6);
            tr.right -= Scale(h, 4);
            DrawTextW(dc, t.data(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            if (i < n - 1) Line(dc, pr.right - 1, pr.top + Scale(h, 4), 1, std::max(0L, pr.bottom - pr.top - Scale(h, 8)), g_brBorder);
        }
        // Griff zum Groesseaendern (Punkte wie bei Windows 11)
        HWND parent = GetParent(h);
        if ((GetWindowLongW(h, GWL_STYLE) & SBARS_SIZEGRIP) && parent && !IsZoomed(parent)) {
            HBRUSH br = CreateSolidBrush(g_pal.textDim);
            int d = std::max(1, Scale(h, 2)), step = Scale(h, 4);
            for (int row = 0; row < 3; row++)
                for (int col = 0; col <= row; col++)
                    Line(dc, rc.right - Scale(h, 3) - d - col * step, rc.bottom - Scale(h, 3) - d - (2 - row) * step, d, d, br);
            DeleteObject(br);
        }
        SelectObject(dc, of);
        BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, ob);
        DeleteObject(bmp);
        DeleteDC(dc);
        EndPaint(h, &ps);
        return 0;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// Liste: Spaltenkopf-Schrift hell, schmaler dunkler Rahmen statt des hellen Randes
LRESULT CALLBACK ListProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    if (g_dark && msg == WM_NOTIFY) {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->code == NM_CUSTOMDRAW && nh->hwndFrom == ListView_GetHeader(h)) {
            NMCUSTOMDRAW* cd = (NMCUSTOMDRAW*)lp;
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, g_pal.text);
                return CDRF_DODEFAULT;
            }
        }
    }
    if (g_dark && msg == WM_NCPAINT) {
        LRESULT r = DefSubclassProc(h, msg, wp, lp);
        if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_CLIENTEDGE) {
            HDC dc = GetWindowDC(h);
            RECT wr;
            GetWindowRect(h, &wr);
            OffsetRect(&wr, -wr.left, -wr.top);
            FrameRect(dc, &wr, g_brBorder);
            InflateRect(&wr, -1, -1);
            FrameRect(dc, &wr, g_brList);
            ReleaseDC(h, dc);
        }
        return r;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void DrawMenuBottomLine(HWND hwnd)
{
    MENUBARINFO mbi{ sizeof mbi };
    if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return;
    RECT rc;
    GetClientRect(hwnd, &rc);
    MapWindowPoints(hwnd, nullptr, (POINT*)&rc, 2);
    RECT wr;
    GetWindowRect(hwnd, &wr);
    OffsetRect(&rc, -wr.left, -wr.top);
    RECT line = rc;
    line.bottom = line.top;
    line.top--;
    HDC dc = GetWindowDC(hwnd);
    FillRect(dc, &line, g_brBg);
    ReleaseDC(hwnd, dc);
}

// ----------------------------------------------------------------------- Steuerelemente einstellen

void ApplyControl(HWND h)
{
    bool d = g_dark;
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(h, d);
    const wchar_t* dark = d ? L"DarkMode_Explorer" : nullptr;
    if (ClassIs(h, WC_BUTTONW)) {
        LONG type = GetWindowLongW(h, GWL_STYLE) & BS_TYPEMASK;
        if (type == BS_GROUPBOX) {
            if (d) SetWindowSubclass(h, GroupProc, 1, 0);
            else RemoveWindowSubclass(h, GroupProc, 1);
        }
        SetWindowTheme(h, dark, nullptr);
    } else if (ClassIs(h, WC_EDITW)) {
        SetWindowTheme(h, d ? L"DarkMode_CFD" : nullptr, nullptr);
    } else if (ClassIs(h, WC_COMBOBOXW)) {
        SetWindowTheme(h, d ? L"DarkMode_CFD" : nullptr, nullptr);
        COMBOBOXINFO ci{ sizeof ci };
        if (GetComboBoxInfo(h, &ci) && ci.hwndList) SetWindowTheme(ci.hwndList, dark, nullptr);
    } else if (ClassIs(h, WC_LISTVIEWW)) {
        SetWindowTheme(h, d ? L"DarkMode_Explorer" : L"Explorer", nullptr);
        if (HWND hdr = ListView_GetHeader(h)) SetWindowTheme(hdr, d ? L"DarkMode_ItemsView" : nullptr, nullptr);
        if (HWND tt = ListView_GetToolTips(h)) SetWindowTheme(tt, dark, nullptr);
        COLORREF bk = d ? g_pal.listBg : GetSysColor(COLOR_WINDOW);
        ListView_SetBkColor(h, bk);
        ListView_SetTextBkColor(h, bk);
        ListView_SetTextColor(h, d ? g_pal.text : GetSysColor(COLOR_WINDOWTEXT));
        if (d) SetWindowSubclass(h, ListProc, 1, 0);
        else RemoveWindowSubclass(h, ListProc, 1);
        SetWindowPos(h, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    } else if (ClassIs(h, WC_HEADERW)) {
        SetWindowTheme(h, d ? L"DarkMode_ItemsView" : nullptr, nullptr);
    } else if (ClassIs(h, STATUSCLASSNAMEW)) {
        if (d) SetWindowSubclass(h, StatusProc, 1, 0);
        else RemoveWindowSubclass(h, StatusProc, 1);
    } else if (ClassIs(h, TOOLTIPS_CLASSW)) {
        SetWindowTheme(h, dark, nullptr);
    }
    InvalidateRect(h, nullptr, TRUE);
}

BOOL CALLBACK ApplyChild(HWND h, LPARAM)
{
    ApplyControl(h);
    return TRUE;
}

void TitleBar(HWND top)
{
    if (pAllowDarkModeForWindow) pAllowDarkModeForWindow(top, g_dark);
    BOOL v = g_dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(top, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &v, sizeof v)))
        DwmSetWindowAttribute(top, 19, &v, sizeof v);   // Windows 10 vor 20H1
}

LRESULT CtlColor(UINT msg, HDC dc, HWND ctl)
{
    switch (msg) {
    case WM_CTLCOLORDLG:
        return (LRESULT)g_brBg;
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        SetTextColor(dc, g_pal.text);
        SetBkColor(dc, g_pal.editBg);
        return (LRESULT)g_brEdit;
    case WM_CTLCOLORSTATIC: {
        bool en = IsWindowEnabled(ctl) != FALSE && !GetPropW(ctl, kDimProp);
        bool edit = ClassIs(ctl, WC_EDITW) && en;    // schreibgeschuetztes Eingabefeld
        SetTextColor(dc, en ? g_pal.text : g_pal.disabled);
        SetBkColor(dc, edit ? g_pal.editBg : g_pal.bg);
        return (LRESULT)(edit ? g_brEdit : g_brBg);
    }
    case WM_CTLCOLORBTN:
        SetBkColor(dc, g_pal.bg);
        return (LRESULT)g_brBg;
    }
    return 0;
}

bool IsCtlColor(UINT msg)
{
    return msg == WM_CTLCOLORDLG || msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX || msg == WM_CTLCOLORSTATIC ||
           msg == WM_CTLCOLORBTN;
}

// ----------------------------------------------------------------------- Meldungsfenster

struct MsgCtx {
    std::wstring text, caption;
    UINT flags = 0;
    const wchar_t *yes, *no, *ok, *cancel;
    HFONT font = nullptr;
    HICON icon = nullptr;
    int iconSize = 0;
    POINT iconPos{};
    int footerTop = 0;
};

INT_PTR CALLBACK MsgProc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    MsgCtx* c = (MsgCtx*)GetWindowLongPtrW(dlg, DWLP_USER);
    INT_PTR tr = 0;
    if (msg != WM_INITDIALOG && ThemeDialogMessage(dlg, msg, wp, lp, &tr)) return tr;
    switch (msg) {
    case WM_INITDIALOG: {
        c = (MsgCtx*)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, lp);
        SetWindowTextW(dlg, c->caption.c_str());
        HWND owner = GetWindow(dlg, GW_OWNER);
        UINT dpi = owner ? GetDpiForWindow(owner) : GetDpiForSystem();
        auto S = [dpi](int v) { return MulDiv(v, (int)dpi, 96); };
        NONCLIENTMETRICSW ncm{ sizeof ncm };
        SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof ncm, &ncm, 0, dpi);
        c->font = CreateFontIndirectW(&ncm.lfMessageFont);
        LPCWSTR ic = nullptr;
        switch (c->flags & MB_ICONMASK) {
        case MB_ICONERROR: ic = IDI_ERROR; break;
        case MB_ICONWARNING: ic = IDI_WARNING; break;
        case MB_ICONINFORMATION: ic = IDI_INFORMATION; break;
        case MB_ICONQUESTION: ic = IDI_QUESTION; break;
        }
        c->iconSize = S(32);
        if (ic) LoadIconWithScaleDown(nullptr, ic, c->iconSize, c->iconSize, &c->icon);

        struct Btn { int id; const wchar_t* text; };
        std::vector<Btn> btns;
        switch (c->flags & MB_TYPEMASK) {
        case MB_YESNO: btns = { { IDYES, c->yes }, { IDNO, c->no } }; break;
        case MB_OKCANCEL: btns = { { IDOK, c->ok }, { IDCANCEL, c->cancel } }; break;
        default: btns = { { IDOK, c->ok } }; break;
        }
        int pad = S(22), bh = S(27), bw = S(88), gap = S(8), fpad = S(12);
        HDC dc = GetDC(dlg);
        HGDIOBJ of = SelectObject(dc, c->font);
        RECT tr0{ 0, 0, S(440), 0 };
        DrawTextW(dc, c->text.c_str(), -1, &tr0, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EXPANDTABS);
        for (auto& b : btns) {
            std::wstring t = b.text;
            t.erase(std::remove(t.begin(), t.end(), L'&'), t.end());
            SIZE sz{};
            GetTextExtentPoint32W(dc, t.c_str(), (int)t.size(), &sz);
            bw = std::max(bw, (int)sz.cx + S(24));
        }
        SelectObject(dc, of);
        ReleaseDC(dlg, dc);

        int textLeft = pad + (c->icon ? c->iconSize + S(14) : 0);
        int contentH = std::max((int)tr0.bottom, c->icon ? c->iconSize : 0);
        int btnsW = (int)btns.size() * bw + ((int)btns.size() - 1) * gap;
        int cw = std::max({ textLeft + (int)tr0.right + pad, btnsW + 2 * fpad, S(300) });
        c->footerTop = pad + contentH + pad;
        int ch = c->footerTop + bh + 2 * fpad;
        c->iconPos = { pad, pad + (contentH - c->iconSize) / 2 };

        HWND st = CreateWindowExW(0, L"STATIC", c->text.c_str(), WS_CHILD | WS_VISIBLE | SS_LEFT | SS_NOPREFIX | SS_EDITCONTROL,
            textLeft, pad + (contentH - tr0.bottom) / 2, tr0.right, tr0.bottom, dlg, (HMENU)(INT_PTR)100, nullptr, nullptr);
        SendMessageW(st, WM_SETFONT, (WPARAM)c->font, FALSE);
        size_t def = ((c->flags & MB_DEFMASK) == MB_DEFBUTTON2 && btns.size() > 1) ? 1 : 0;
        int x = cw - fpad - btnsW;
        HWND defBtn = nullptr;
        for (size_t i = 0; i < btns.size(); i++) {
            HWND b = CreateWindowExW(0, L"BUTTON", btns[i].text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | (i == def ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON),
                x, c->footerTop + fpad, bw, bh, dlg, (HMENU)(INT_PTR)btns[i].id, nullptr, nullptr);
            SendMessageW(b, WM_SETFONT, (WPARAM)c->font, FALSE);
            if (i == def) defBtn = b;
            x += bw + gap;
        }
        SendMessageW(dlg, DM_SETDEFID, btns[def].id, 0);

        RECT wr{ 0, 0, cw, ch };
        AdjustWindowRectExForDpi(&wr, (DWORD)GetWindowLongW(dlg, GWL_STYLE), FALSE, (DWORD)GetWindowLongW(dlg, GWL_EXSTYLE), dpi);
        int w = wr.right - wr.left, h = wr.bottom - wr.top;
        RECT ref;
        HMONITOR mon;
        if (owner && IsWindowVisible(owner) && !IsIconic(owner)) {
            GetWindowRect(owner, &ref);
            mon = MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        } else {
            POINT pt;
            GetCursorPos(&pt);
            mon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{ sizeof mi };
            GetMonitorInfoW(mon, &mi);
            ref = mi.rcWork;
        }
        MONITORINFO mi{ sizeof mi };
        GetMonitorInfoW(mon, &mi);
        int px = ref.left + (ref.right - ref.left - w) / 2, py = ref.top + (ref.bottom - ref.top - h) / 2;
        px = std::max((int)mi.rcWork.left, std::min(px, (int)mi.rcWork.right - w));
        py = std::max((int)mi.rcWork.top, std::min(py, (int)mi.rcWork.bottom - h));
        SetWindowPos(dlg, nullptr, px, py, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
        ThemeDialogMessage(dlg, WM_INITDIALOG, 0, 0, &tr);
        MessageBeep(c->flags & MB_ICONMASK);
        SetFocus(defBtn);
        return FALSE;
    }
    case WM_ERASEBKGND: {
        if (!c) break;
        HDC dc = (HDC)wp;
        RECT rc;
        GetClientRect(dlg, &rc);
        RECT top = rc, foot = rc;
        top.bottom = foot.top = c->footerTop;
        FillRect(dc, &top, g_dark ? g_brBg : GetSysColorBrush(COLOR_WINDOW));
        FillRect(dc, &foot, g_dark ? g_brFooter : GetSysColorBrush(COLOR_BTNFACE));
        SetWindowLongPtrW(dlg, DWLP_MSGRESULT, 1);
        return TRUE;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(dlg, &ps);
        if (c && c->icon) DrawIconEx(dc, c->iconPos.x, c->iconPos.y, c->icon, c->iconSize, c->iconSize, 0, nullptr, DI_NORMAL);
        EndPaint(dlg, &ps);
        return TRUE;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDCANCEL && c && (c->flags & MB_TYPEMASK) == MB_YESNO) id = IDNO;   // Esc = Nein
        if (id == IDOK || id == IDYES || id == IDNO || id == IDCANCEL) {
            EndDialog(dlg, id);
            return TRUE;
        }
        break;
    }
    case WM_DESTROY:
        if (c) {
            if (c->font) DeleteObject(c->font);
            if (c->icon) DestroyIcon(c->icon);
            c->font = nullptr;
            c->icon = nullptr;
        }
        break;
    }
    return FALSE;
}

} // namespace

// ======================================================================= oeffentlich

ThemeMode ParseThemeMode(const std::wstring& s)
{
    if (_wcsicmp(s.c_str(), L"light") == 0) return ThemeMode::Light;
    if (_wcsicmp(s.c_str(), L"dark") == 0) return ThemeMode::Dark;
    return ThemeMode::System;
}

const wchar_t* ThemeModeName(ThemeMode m)
{
    return m == ThemeMode::Light ? L"light" : m == ThemeMode::Dark ? L"dark" : L"system";
}

void ThemeInit(ThemeMode m)
{
    if (WindowsBuild() >= 18362) {   // Windows 10 1903
        HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (ux) {
            pRefreshImmersiveColorPolicyState = (FnVoid)GetProcAddress(ux, MAKEINTRESOURCEA(104));
            pAllowDarkModeForWindow = (FnAllowDarkModeForWindow)GetProcAddress(ux, MAKEINTRESOURCEA(133));
            pSetPreferredAppMode = (FnSetPreferredAppMode)GetProcAddress(ux, MAKEINTRESOURCEA(135));
            pFlushMenuThemes = (FnVoid)GetProcAddress(ux, MAKEINTRESOURCEA(136));
        }
    }
    g_pal.bg = RGB(32, 32, 32);
    g_pal.listBg = RGB(25, 25, 25);
    g_pal.editBg = RGB(45, 45, 45);
    g_pal.text = RGB(235, 235, 235);
    g_pal.textDim = RGB(165, 165, 165);
    g_pal.disabled = RGB(118, 118, 118);
    g_pal.border = RGB(72, 72, 72);
    g_pal.statusBg = RGB(40, 40, 40);
    g_pal.menuHot = RGB(62, 62, 62);
    g_pal.menuPressed = RGB(78, 78, 78);
    g_pal.footerBg = RGB(44, 44, 44);
    g_brBg = CreateSolidBrush(g_pal.bg);
    g_brList = CreateSolidBrush(g_pal.listBg);
    g_brEdit = CreateSolidBrush(g_pal.editBg);
    g_brBorder = CreateSolidBrush(g_pal.border);
    g_brStatus = CreateSolidBrush(g_pal.statusBg);
    g_brMenuHot = CreateSolidBrush(g_pal.menuHot);
    g_brMenuPressed = CreateSolidBrush(g_pal.menuPressed);
    g_brFooter = CreateSolidBrush(g_pal.footerBg);
    g_mode = m;
    g_dark = Resolve();
    ApplyAppMode();
}

bool ThemeSetMode(ThemeMode m)
{
    g_mode = m;
    return ThemeUpdate();
}

ThemeMode ThemeGetMode() { return g_mode; }

bool ThemeUpdate()
{
    if (pRefreshImmersiveColorPolicyState) pRefreshImmersiveColorPolicyState();
    bool d = Resolve();
    if (d == g_dark) return false;
    g_dark = d;
    ApplyAppMode();
    return true;
}

bool IsDark() { return g_dark; }
const Palette& Colors() { return g_pal; }

void ThemeApply(HWND top)
{
    TitleBar(top);
    EnumChildWindows(top, ApplyChild, 0);
    if (GetMenu(top)) DrawMenuBar(top);
    SetWindowPos(top, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(top, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
}

void ThemeEnableLabel(HWND label, bool enable)
{
    if (g_dark) {
        EnableWindow(label, TRUE);
        if (enable) RemovePropW(label, kDimProp);
        else SetPropW(label, kDimProp, (HANDLE)1);
    } else {
        RemovePropW(label, kDimProp);
        EnableWindow(label, enable);
    }
    InvalidateRect(label, nullptr, TRUE);
}

bool ThemeDialogMessage(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, INT_PTR* result)
{
    if (msg == WM_DESTROY) {   // Markierungen der Beschriftungen entfernen
        EnumChildWindows(dlg, [](HWND h, LPARAM) -> BOOL { RemovePropW(h, kDimProp); return TRUE; }, 0);
        return false;
    }
    if (msg == WM_INITDIALOG) {
        TitleBar(dlg);   // auch hell: bei dunklem Windows und Einstellung "Hell" ausdruecklich helle Titelleiste
        if (g_dark) EnumChildWindows(dlg, ApplyChild, 0);
        return false;    // die Dialogprozedur macht weiter
    }
    if (!g_dark) return false;
    if (IsCtlColor(msg)) {
        *result = (INT_PTR)CtlColor(msg, (HDC)wp, (HWND)lp);
        return true;
    }
    if (msg == WM_NOTIFY) {
        NMHDR* nh = (NMHDR*)lp;
        if (nh->code == NM_CUSTOMDRAW && IsCheckOrRadio(nh->hwndFrom)) {
            SetWindowLongPtrW(dlg, DWLP_MSGRESULT, DrawCheck((NMCUSTOMDRAW*)lp));
            *result = TRUE;
            return true;
        }
    }
    return false;
}

bool ThemeWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result)
{
    if (msg == WM_THEMECHANGED && g_menuTheme) {
        CloseThemeData(g_menuTheme);
        g_menuTheme = nullptr;
        return false;
    }
    if (!g_dark) return false;
    switch (msg) {
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(hwnd, &rc);
        FillRect((HDC)wp, &rc, g_brBg);
        *result = 1;
        return true;
    }
    case WM_UAHDRAWMENU: {
        UAHMENU* um = (UAHMENU*)lp;
        MENUBARINFO mbi{ sizeof mbi };
        if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return false;
        RECT wr;
        GetWindowRect(hwnd, &wr);
        RECT rc = mbi.rcBar;
        OffsetRect(&rc, -wr.left, -wr.top);
        FillRect(um->hdc, &rc, g_brBg);
        *result = TRUE;
        return true;
    }
    case WM_UAHDRAWMENUITEM: {
        UAHDRAWMENUITEM* di = (UAHDRAWMENUITEM*)lp;
        wchar_t s[256] = {};
        MENUITEMINFOW mii{ sizeof mii };
        mii.fMask = MIIM_STRING;
        mii.dwTypeData = s;
        mii.cch = 255;
        GetMenuItemInfoW(di->um.hmenu, di->umi.iPosition, TRUE, &mii);
        UINT st = di->dis.itemState;
        HBRUSH bg = (st & ODS_SELECTED) ? g_brMenuPressed : (st & ODS_HOTLIGHT) ? g_brMenuHot : g_brBg;
        FillRect(di->um.hdc, &di->dis.rcItem, bg);
        DWORD flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER | ((st & ODS_NOACCEL) ? DT_HIDEPREFIX : 0);
        if (!g_menuTheme) g_menuTheme = OpenThemeData(hwnd, L"Menu");
        COLORREF fg = (st & (ODS_GRAYED | ODS_DISABLED)) ? g_pal.disabled : (st & ODS_INACTIVE) ? g_pal.textDim : g_pal.text;
        if (g_menuTheme) {
            DTTOPTS o{ sizeof o };
            o.dwFlags = DTT_TEXTCOLOR;
            o.crText = fg;
            DrawThemeTextEx(g_menuTheme, di->um.hdc, MENU_BARITEM, MBI_NORMAL, s, (int)mii.cch, flags, &di->dis.rcItem, &o);
        } else {
            SetBkMode(di->um.hdc, TRANSPARENT);
            SetTextColor(di->um.hdc, fg);
            DrawTextW(di->um.hdc, s, (int)mii.cch, &di->dis.rcItem, flags);
        }
        *result = TRUE;
        return true;
    }
    case WM_NCPAINT:
    case WM_NCACTIVATE:
        *result = DefWindowProcW(hwnd, msg, wp, lp);
        DrawMenuBottomLine(hwnd);
        return true;
    }
    if (IsCtlColor(msg)) {
        *result = CtlColor(msg, (HDC)wp, (HWND)lp);
        return true;
    }
    return false;
}

int DarkMessageBox(HWND parent, const std::wstring& text, const wchar_t* caption, UINT flags,
                   const wchar_t* yes, const wchar_t* no, const wchar_t* ok, const wchar_t* cancel)
{
    struct alignas(4) Tpl {
        DLGTEMPLATE t;
        WORD menu, cls, title;
    } tpl{};
    tpl.t.style = WS_POPUP | WS_CAPTION | WS_SYSMENU | DS_MODALFRAME;
    tpl.t.dwExtendedStyle = WS_EX_DLGMODALFRAME;
    MsgCtx c;
    c.text = text;
    c.caption = caption ? caption : L"";
    c.flags = flags;
    c.yes = yes; c.no = no; c.ok = ok; c.cancel = cancel;
    INT_PTR r = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), &tpl.t, parent, MsgProc, (LPARAM)&c);
    return r <= 0 ? IDCANCEL : (int)r;
}

} // namespace gui
