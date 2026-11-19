#pragma once
// Hell-/Dunkel-Darstellung der Oberflaeche (Win32)
//
// Hell = unveraenderte Windows-Steuerelemente. Dunkel = dunkle Titelleiste (DWM), dunkle Varianten
// der Windows-Designs ("DarkMode_Explorer", "DarkMode_CFD", "DarkMode_ItemsView") und eigenes Zeichnen
// dort, wo Windows keine dunkle Variante hat (Menueleiste, Statuszeile, Gruppenrahmen, Text von
// Kontrollkaestchen, Meldungsfenster). Bei aktivem Kontrastdesign bleibt es immer bei den Systemfarben.
#include <windows.h>
#include <string>

namespace gui {

enum class ThemeMode { System = 0, Light = 1, Dark = 2 };

ThemeMode ParseThemeMode(const std::wstring& s);   // "system" | "light" | "dark"
const wchar_t* ThemeModeName(ThemeMode m);

void ThemeInit(ThemeMode m);      // einmal beim Start, vor dem ersten Fenster
bool ThemeSetMode(ThemeMode m);   // true = Hell/Dunkel hat gewechselt
ThemeMode ThemeGetMode();
bool ThemeUpdate();               // Windows-Einstellung neu lesen; true = Hell/Dunkel hat gewechselt
bool IsDark();

struct Palette {
    COLORREF bg, listBg, editBg, text, textDim, disabled, border, statusBg, menuHot, menuPressed, footerBg;
};
const Palette& Colors();

// Fenster und alle Kindfenster an die aktuelle Darstellung anpassen (auch zum Umschalten zur Laufzeit).
void ThemeApply(HWND top);

// Beschriftung (Static) aus-/einschalten. Im Dunkelmodus bleibt sie technisch aktiv und wird nur grau
// gezeichnet, weil Windows deaktivierte Beschriftungen immer hell "eingepraegt" darstellt.
void ThemeEnableLabel(HWND label, bool enable);

// Zuerst in Dialogprozeduren aufrufen; true = Nachricht erledigt, *result zurueckgeben.
bool ThemeDialogMessage(HWND dlg, UINT msg, WPARAM wp, LPARAM lp, INT_PTR* result);
// Zuerst in der Prozedur des Hauptfensters aufrufen; true = Nachricht erledigt.
bool ThemeWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result);

// Meldungsfenster im dunklen Design (MB_OK, MB_OKCANCEL, MB_YESNO; Symbole; MB_DEFBUTTON2).
int DarkMessageBox(HWND parent, const std::wstring& text, const wchar_t* caption, UINT flags,
                   const wchar_t* yes, const wchar_t* no, const wchar_t* ok, const wchar_t* cancel);

} // namespace gui
