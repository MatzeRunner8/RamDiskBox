#pragma once
// Bausteine der Oberflaeche: Symbole aus der Windows-Symbolschrift, Symbolknoepfe, Speicherleiste
#include <windows.h>
#include <string>

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

// Speicherleiste: belegter Speicher der RAM-Laufwerke | frei | andere Programme
HWND MemBarCreate(HINSTANCE inst, HWND parent, int id);
void MemBarSet(HWND bar, unsigned long long disks, unsigned long long freeMem, unsigned long long total,
               const std::wstring& disksText, const std::wstring& freeText, const std::wstring& otherText);

} // namespace gui
