// RamDiskBox - Symbole, Symbolknoepfe und Speicherleiste
#include "widgets.h"
#include "theme.h"
#include <commctrl.h>
#include <uxtheme.h>
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
    std::vector<MemSegment> segs;
    std::wstring tip;
    HFONT font = nullptr;
    HWND tipWnd = nullptr;
    int tipDark = -1;
};

int TextWidth(HDC dc, const std::wstring& s)
{
    if (s.empty()) return 0;
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), (int)s.size(), &sz);
    return sz.cx;
}

// Abgerundeter Abschnitt mit diagonaler Schraffur (Linienbreite und Abstand skalieren mit der DPI)
void HatchFill(HWND h, HDC dc, const RECT& r, int radius, COLORREF base, COLORREF lines)
{
    RoundFill(dc, r, radius, base);
    HRGN rgn = CreateRoundRectRgn(r.left, r.top, r.right + 1, r.bottom + 1, radius, radius);
    SelectClipRgn(dc, rgn);
    HPEN pen = CreatePen(PS_SOLID, std::max(1, Scale(h, 2)), lines);
    HGDIOBJ op = SelectObject(dc, pen);
    int step = std::max(4, Scale(h, 8)), height = r.bottom - r.top;
    for (int x = r.left - height; x < r.right; x += step) {
        MoveToEx(dc, x, r.bottom, nullptr);
        LineTo(dc, x + height, r.top);
    }
    SelectObject(dc, op);
    DeleteObject(pen);
    SelectClipRgn(dc, nullptr);
    DeleteObject(rgn);
}

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
    std::vector<const MemSegment*> vis;
    unsigned long long sum = 0;
    if (d) for (auto& s : d->segs) if (s.value) { vis.push_back(&s); sum += s.value; }
    const int n = (int)vis.size();
    if (n > 0 && rc.right > 0) {
        HGDIOBJ of = SelectObject(dc, d->font ? d->font : GetStockObject(DEFAULT_GUI_FONT));
        const int gap = Scale(h, 6), pad = Scale(h, 12), inner = Scale(h, 4), radius = Scale(h, 8), sliver = Scale(h, 4);
        const int avail = std::max(0, (int)rc.right - gap * (n - 1));
        std::vector<int> share(n), lw(n), sw(n), minw(n), w(n);
        std::vector<bool> tiny(n);
        for (int i = 0; i < n; i++) {
            share[i] = (int)((double)avail * vis[i]->value / sum);
            lw[i] = TextWidth(dc, vis[i]->longText) + 2 * pad;
            sw[i] = TextWidth(dc, vis[i]->shortText) + 2 * pad;
            // Sehr kleine Anteile (unter 3 %) bekommen keine Beschriftung, die sie breiter machen wuerde
            tiny[i] = (double)vis[i]->value / sum < 0.03 && share[i] < sw[i];
        }
        // Eine Stufe fuer alle: lange Texte, kurze Texte oder ohne Text, je nachdem, was hineinpasst
        int level = 2;
        for (int lv = 0; lv < 2 && level == 2; lv++) {
            long long need = 0;
            for (int i = 0; i < n; i++) need += tiny[i] ? sliver : (lv == 0 ? lw[i] : sw[i]);
            if (need <= avail) level = lv;
        }
        long long total = 0;
        for (int i = 0; i < n; i++) {
            minw[i] = (tiny[i] || level == 2) ? sliver : (level == 0 ? lw[i] : sw[i]);
            w[i] = std::max(share[i], minw[i]);
            total += w[i];
        }
        // Ueberhang vom Abschnitt mit dem meisten Spielraum nehmen, nie unter seine Mindestbreite
        while (total > avail) {
            int best = -1;
            for (int i = 0; i < n; i++)
                if (w[i] > minw[i] && (best < 0 || w[i] - minw[i] > w[best] - minw[best])) best = i;
            if (best < 0) break;   // zu schmal: Abschnitte werden rechts abgeschnitten
            int take = (int)std::min<long long>(w[best] - minw[best], total - avail);
            w[best] -= take;
            total -= take;
        }
        if (total < avail) {   // Rundungsrest an den breitesten Abschnitt
            int big = 0;
            for (int i = 1; i < n; i++) if (w[i] > w[big]) big = i;
            w[big] += (int)(avail - total);
        }
        SetBkMode(dc, TRANSPARENT);
        int x = 0;
        for (int i = 0; i < n; i++) {
            const MemSegment& s = *vis[i];
            RECT r{ x, 0, std::min<LONG>(rc.right, x + w[i]), rc.bottom };
            if (r.right > r.left) {
                if (s.hatched) HatchFill(h, dc, r, radius, s.fill, s.hatch);
                else RoundFill(dc, r, radius, s.fill);
                // laengste Beschriftung, die in den Abschnitt passt
                int room = (int)(r.right - r.left) - 2 * inner;
                const std::wstring* t = nullptr;
                if (!tiny[i]) {
                    if (!s.longText.empty() && TextWidth(dc, s.longText) <= room) t = &s.longText;
                    else if (!s.shortText.empty() && TextWidth(dc, s.shortText) <= room) t = &s.shortText;
                }
                if (t) {
                    RECT tr = r;
                    InflateRect(&tr, -inner, 0);
                    SetTextColor(dc, s.text);
                    DrawTextW(dc, t->c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                }
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
        if (d && d->tipWnd) DestroyWindow(d->tipWnd);
        delete d;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case WM_SETFONT:
        if (d) d->font = (HFONT)wp;
        if (LOWORD(lp)) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_SIZE:   // Abschnitte und Beschriftungen haengen von der Breite ab
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_DPICHANGED_AFTERPARENT:
        if (d && d->tipWnd) SendMessageW(d->tipWnd, TTM_SETMAXTIPWIDTH, 0, Scale(h, 640));
        break;
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
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = MemBarProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kMemBarClass;
        RegisterClassW(&wc);
        registered = true;
    }
    return CreateWindowExW(0, kMemBarClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, parent, (HMENU)(INT_PTR)id, inst, nullptr);
}

void MemBarSet(HWND bar, const std::vector<MemSegment>& segments, const std::wstring& tooltip)
{
    MemBarData* d = (MemBarData*)GetWindowLongPtrW(bar, GWLP_USERDATA);
    if (!d) return;
    bool changed = segments.size() != d->segs.size();
    for (size_t i = 0; !changed && i < segments.size(); i++) {
        const MemSegment &a = segments[i], &b = d->segs[i];
        changed = a.value != b.value || a.fill != b.fill || a.text != b.text || a.hatched != b.hatched ||
                  a.hatch != b.hatch || a.longText != b.longText || a.shortText != b.shortText;
    }
    if (changed) {
        d->segs = segments;
        InvalidateRect(bar, nullptr, FALSE);
    }
    // Kurzinfo mit den genauen Werten (eigenes Fenster, mehrzeilig, passend zu Hell/Dunkel)
    if (!d->tipWnd) {
        d->tipWnd = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
            CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, bar, nullptr, nullptr, nullptr);
        TTTOOLINFOW ti{ sizeof ti };
        ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        ti.hwnd = bar;
        ti.uId = (UINT_PTR)bar;
        ti.lpszText = (LPWSTR)L"";
        SendMessageW(d->tipWnd, TTM_ADDTOOLW, 0, (LPARAM)&ti);
        SendMessageW(d->tipWnd, TTM_SETMAXTIPWIDTH, 0, Scale(bar, 640));
        SendMessageW(d->tipWnd, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000);
    }
    int dark = IsDark() ? 1 : 0;
    if (dark != d->tipDark) {
        SetWindowTheme(d->tipWnd, dark ? L"DarkMode_Explorer" : nullptr, nullptr);
        d->tipDark = dark;
    }
    if (tooltip != d->tip) {
        d->tip = tooltip;
        TTTOOLINFOW ti{ sizeof ti };
        ti.hwnd = bar;
        ti.uId = (UINT_PTR)bar;
        ti.lpszText = (LPWSTR)d->tip.c_str();
        SendMessageW(d->tipWnd, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
    }
}

} // namespace gui
