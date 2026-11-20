// RamDiskBox - Symbole, Symbolknoepfe und Speicherleiste
#include "widgets.h"
#include "theme.h"
#include <commctrl.h>
#include <map>
#include <tuple>
#include <vector>
#include <algorithm>

#pragma comment(lib, "msimg32.lib")

namespace gui {

namespace {

constexpr wchar_t kHotProp[] = L"RamDiskBox.Hot";
constexpr wchar_t kMemBarClass[] = L"RamDiskBoxMemBar";

int Scale(HWND h, int v) { return MulDiv(v, (int)GetDpiForWindow(h), 96); }

const wchar_t* IconFontName()
{
    static const wchar_t* name = [] {
        bool fluent = false;
        HDC dc = GetDC(nullptr);
        LOGFONTW lf{};
        lf.lfCharSet = DEFAULT_CHARSET;
        wcscpy_s(lf.lfFaceName, L"Segoe Fluent Icons");
        EnumFontFamiliesExW(dc, &lf, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM p) -> int {
            *(bool*)p = true;
            return 0;
        }, (LPARAM)&fluent, 0);
        ReleaseDC(nullptr, dc);
        return fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
    }();
    return name;
}

std::map<std::tuple<wchar_t, int, COLORREF>, HBITMAP> g_cache;

void RoundFill(HDC dc, const RECT& r, int radius, COLORREF c)
{
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

LRESULT CALLBACK ToolButtonProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR)
{
    switch (msg) {
    case WM_MOUSEMOVE:
        if (!GetPropW(h, kHotProp)) {
            SetPropW(h, kHotProp, (HANDLE)1);
            TRACKMOUSEEVENT t{ sizeof t, TME_LEAVE, h, 0 };
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, kHotProp);
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_ENABLE:
        RemovePropW(h, kHotProp);
        break;
    case WM_NCDESTROY:
        RemovePropW(h, kHotProp);
        RemoveWindowSubclass(h, ToolButtonProc, 1);
        break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

// ----------------------------------------------------------------------- Speicherleiste

struct MemBarData {
    unsigned long long vals[3] = {};
    std::wstring texts[3];
    HFONT font = nullptr;
};

void PaintMemBar(HWND h, HDC wdc)
{
    MemBarData* d = (MemBarData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    RECT rc;
    GetClientRect(h, &rc);
    HDC dc = CreateCompatibleDC(wdc);
    HBITMAP bmp = CreateCompatibleBitmap(wdc, std::max(1L, rc.right), std::max(1L, rc.bottom));
    HGDIOBJ ob = SelectObject(dc, bmp);
    bool dark = IsDark();
    HBRUSH bg = CreateSolidBrush(dark ? Colors().bg : GetSysColor(COLOR_BTNFACE));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    if (d) {
        HGDIOBJ of = SelectObject(dc, d->font ? d->font : GetStockObject(DEFAULT_GUI_FONT));
        const COLORREF fills[3] = {
            dark ? RGB(47, 94, 160) : RGB(52, 112, 196),   // RAM-Laufwerke
            dark ? RGB(36, 120, 66) : RGB(46, 145, 82),    // frei
            dark ? RGB(78, 78, 78) : RGB(128, 128, 128),   // andere Programme
        };
        int gap = Scale(h, 6), pad = Scale(h, 12), radius = Scale(h, 8);
        int avail = std::max(0, (int)rc.right - 2 * gap);
        int minw[3], w[3];
        unsigned long long sum = d->vals[0] + d->vals[1] + d->vals[2];
        for (int i = 0; i < 3; i++) {
            SIZE sz{};
            GetTextExtentPoint32W(dc, d->texts[i].c_str(), (int)d->texts[i].size(), &sz);
            minw[i] = sz.cx + 2 * pad;
            w[i] = sum ? (int)((double)avail * d->vals[i] / sum) : avail / 3;
            w[i] = std::max(w[i], minw[i]);
        }
        // Mindestbreiten fuer die Beschriftung: Ueberhang vom Teil mit dem meisten Spielraum nehmen
        int total = w[0] + w[1] + w[2];
        while (total > avail) {
            int best = 0;
            for (int i = 1; i < 3; i++) if (w[i] - minw[i] > w[best] - minw[best]) best = i;
            int slack = w[best] - minw[best];
            if (slack <= 0) { w[best] -= total - avail; break; }   // zu schmal: Text wird gekuerzt
            int take = std::min(slack, total - avail);
            w[best] -= take;
            total -= take;
        }
        w[2] += avail - (w[0] + w[1] + w[2]);   // Rundungsrest
        int x = 0;
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));
        for (int i = 0; i < 3; i++) {
            RECT r{ x, 0, x + std::max(0, w[i]), rc.bottom };
            if (r.right > r.left) {
                RoundFill(dc, r, radius, fills[i]);
                RECT tr = r;
                InflateRect(&tr, -Scale(h, 4), 0);
                DrawTextW(dc, d->texts[i].c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
            }
            x = r.right + gap;
        }
        SelectObject(dc, of);
    }
    BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, ob);
    DeleteObject(bmp);
    DeleteDC(dc);
}

LRESULT CALLBACK MemBarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    MemBarData* d = (MemBarData*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE:
        SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)new MemBarData);
        break;
    case WM_NCDESTROY:
        delete d;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case WM_SETFONT:
        if (d) d->font = (HFONT)wp;
        if (LOWORD(lp)) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintMemBar(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

} // namespace

COLORREF TintColor(Tint t)
{
    bool d = IsDark();
    switch (t) {
    case Tint::Green: return d ? RGB(76, 194, 108) : RGB(30, 145, 66);
    case Tint::Red: return d ? RGB(236, 112, 102) : RGB(196, 52, 44);
    case Tint::Blue: return d ? RGB(100, 168, 250) : RGB(28, 106, 206);
    case Tint::Amber: return d ? RGB(232, 182, 72) : RGB(196, 134, 14);
    default: return d ? RGB(172, 177, 186) : RGB(96, 101, 110);
    }
}

HBITMAP GlyphBitmap(wchar_t glyph, int px, COLORREF color)
{
    auto key = std::make_tuple(glyph, px, color);
    auto it = g_cache.find(key);
    if (it != g_cache.end()) return it->second;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = px;
    bi.bmiHeader.biHeight = -px;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bmp) { DeleteDC(dc); return nullptr; }
    HGDIOBJ ob = SelectObject(dc, bmp);
    memset(bits, 0, (size_t)px * px * 4);
    // Weiss auf Schwarz mit Graustufen-Kantenglaettung; die Helligkeit wird zum Alphawert
    HFONT f = CreateFontW(-px, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
        CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH, IconFontName());
    HGDIOBJ of = SelectObject(dc, f);
    SetTextColor(dc, RGB(255, 255, 255));
    SetBkMode(dc, TRANSPARENT);
    RECT r{ 0, 0, px, px };
    DrawTextW(dc, &glyph, 1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    UINT32* p = (UINT32*)bits;
    UINT32 cr = GetRValue(color), cg = GetGValue(color), cb = GetBValue(color);
    for (int i = 0; i < px * px; i++) {
        UINT32 v = p[i];
        UINT32 a = std::max({ v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF });
        p[i] = (a << 24) | ((cr * a / 255) << 16) | ((cg * a / 255) << 8) | (cb * a / 255);
    }
    SelectObject(dc, of);
    DeleteObject(f);
    SelectObject(dc, ob);
    DeleteDC(dc);
    g_cache[key] = bmp;
    return bmp;
}

void DrawGlyph(HDC dc, wchar_t glyph, const RECT& box, int px, COLORREF color, BYTE alpha)
{
    HBITMAP bmp = GlyphBitmap(glyph, px, color);
    if (!bmp) return;
    HDC src = CreateCompatibleDC(dc);
    HGDIOBJ ob = SelectObject(src, bmp);
    int x = box.left + (box.right - box.left - px) / 2, y = box.top + (box.bottom - box.top - px) / 2;
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, alpha, AC_SRC_ALPHA };
    AlphaBlend(dc, x, y, px, px, src, 0, 0, px, px, bf);
    SelectObject(src, ob);
    DeleteDC(src);
}

void ToolButtonAttach(HWND button)
{
    SetWindowSubclass(button, ToolButtonProc, 1, 0);
}

void ToolButtonDraw(const DRAWITEMSTRUCT* di, wchar_t glyph, COLORREF color)
{
    HDC dc = di->hDC;
    RECT rc = di->rcItem;
    HWND h = di->hwndItem;
    bool dark = IsDark();
    bool disabled = (di->itemState & ODS_DISABLED) != 0;
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool hot = GetPropW(h, kHotProp) != nullptr;
    HBRUSH bg = CreateSolidBrush(dark ? Colors().bg : GetSysColor(COLOR_BTNFACE));
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    if (!disabled && (pressed || hot)) {
        COLORREF c = dark ? (pressed ? RGB(74, 74, 74) : RGB(56, 56, 56)) : (pressed ? RGB(200, 200, 200) : RGB(225, 225, 225));
        RoundFill(dc, rc, Scale(h, 8), c);
    }
    int px = (rc.bottom - rc.top) * 3 / 5;
    DrawGlyph(dc, glyph, rc, px, color, disabled ? 64 : 255);
    if ((di->itemState & ODS_FOCUS) && !(di->itemState & ODS_NOFOCUSRECT)) {
        RECT f = rc;
        InflateRect(&f, -Scale(h, 2), -Scale(h, 2));
        SetTextColor(dc, dark ? Colors().text : GetSysColor(COLOR_BTNTEXT));
        SetBkColor(dc, dark ? Colors().bg : GetSysColor(COLOR_BTNFACE));
        DrawFocusRect(dc, &f);
    }
}

HWND MemBarCreate(HINSTANCE inst, HWND parent, int id)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = MemBarProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kMemBarClass;
        RegisterClassW(&wc);
        registered = true;
    }
    return CreateWindowExW(0, kMemBarClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, inst, nullptr);
}

void MemBarSet(HWND bar, unsigned long long disks, unsigned long long freeMem, unsigned long long total,
               const std::wstring& disksText, const std::wstring& freeText, const std::wstring& otherText)
{
    MemBarData* d = (MemBarData*)GetWindowLongPtrW(bar, GWLP_USERDATA);
    if (!d) return;
    unsigned long long other = total > freeMem + disks ? total - freeMem - disks : 0;
    unsigned long long v[3] = { disks, freeMem, other };
    std::wstring t[3] = { disksText, freeText, otherText };
    bool changed = false;
    for (int i = 0; i < 3; i++) {
        if (d->vals[i] != v[i] || d->texts[i] != t[i]) changed = true;
        d->vals[i] = v[i];
        d->texts[i] = t[i];
    }
    if (changed) InvalidateRect(bar, nullptr, FALSE);
}

} // namespace gui
