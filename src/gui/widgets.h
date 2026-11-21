#pragma once
// Bausteine der Oberflaeche: Symbole aus der Windows-Symbolschrift, Symbolknoepfe, Speicherleiste
#include <windows.h>
#include <string>
#include <vector>

namespace gui {

// Zeichen der Symbolschrift (Segoe Fluent Icons, unter Windows 10 Segoe MDL2 Assets)
namespace Glyph {
constexpr wchar_t Add = 0xE710;
constexpr wchar_t Cancel = 0xE711;
constexpr wchar_t Edit = 0xE70F;
constexpr wchar_t Settings = 0xE713;
constexpr wchar_t CheckMark = 0xE73E;
constexpr wchar_t Stop = 0xE71A;
constexpr wchar_t Save = 0xE74E;
constexpr wchar_t FolderOpen = 0xE838;
constexpr wchar_t CompletedSolid = 0xEC61;
constexpr wchar_t Warning = 0xE7BA;
constexpr wchar_t RadioOff = 0xECCA;
}

// Farben fuer Symbole, passend zu Hell/Dunkel
enum class Tint { Green, Red, Blue, Gray, Amber };
COLORREF TintColor(Tint t);

// Symbol als 32-Bit-Bitmap mit vormultipliziertem Alpha (zwischengespeichert, nicht freigeben)
HBITMAP GlyphBitmap(wchar_t glyph, int px, COLORREF color);
// Symbol mittig in box zeichnen; alpha 0..255 (z. B. halbtransparent fuer deaktiviert)
void DrawGlyph(HDC dc, wchar_t glyph, const RECT& box, int px, COLORREF color, BYTE alpha = 255);

// Symbolknopf: BUTTON mit BS_OWNERDRAW. Attach verfolgt das Ueberfahren mit der Maus,
// Draw zeichnet ihn in WM_DRAWITEM (Hintergrund je nach Zustand, Symbol, Fokusrahmen).
void ToolButtonAttach(HWND button);
void ToolButtonDraw(const DRAWITEMSTRUCT* di, wchar_t glyph, COLORREF color);

// Speicherleiste: Abschnitte proportional zu ihrem Wert (Abschnitte mit Wert 0 entfallen). Jeder
// Abschnitt hat eine lange und eine kurze Beschriftung; passt keine, bleibt er ohne Text, die genauen
// Werte stehen in der Kurzinfo (tooltip, mehrzeilig).
struct MemSegment {
    unsigned long long value = 0;
    COLORREF fill = 0, text = RGB(255, 255, 255);
    bool hatched = false;      // schraffiert (z. B. reservierte, noch nicht belegte Kapazitaet)
    COLORREF hatch = 0;        // Farbe der Schraffurlinien
    std::wstring longText, shortText;
};
HWND MemBarCreate(HINSTANCE inst, HWND parent, int id);
void MemBarSet(HWND bar, const std::vector<MemSegment>& segments, const std::wstring& tooltip);

} // namespace gui
