#pragma once
#include "../common/common.h"
#include "lang.h"
#include "theme.h"
#include "resource.h"
#include <commctrl.h>
#include <string>
#include <vector>

namespace gui {

struct GuiSettings {
    std::wstring lang = L"auto";      // auto | de | en
    std::wstring theme = L"system";   // system | light | dark
    bool trayClose = true;
    bool startMin = false;
};

extern HINSTANCE g_inst;
extern HWND g_main;
extern GuiSettings g_set;
extern rdb::Message g_last;   // letzte LIST-Antwort
extern bool g_online;

rdb::Ini& GuiIni();
std::wstring ImagesFolder();   // Image-Ordner des Hintergrundprozesses
void SaveGuiSettings();

std::wstring Fmt(const wchar_t* fmt, ...);
int MsgBox(HWND parent, const std::wstring& text, UINT flags);
void OpenUrl(const wchar_t* url);
bool IsAutostart();
void SetAutostart(bool on);
bool ServiceInstalled();
bool WinFspInstalled();
int RunElevated(HWND parent, const std::wstring& exe, const std::wstring& args); // Exit-Code, -1 = abgebrochen
bool EnsureHost(bool startPortable, std::wstring* err);
std::vector<wchar_t> FreeLetters(wchar_t include);
const rdb::KV* FindVolume(wchar_t letter);
std::wstring CurrentTempDir();   // TEMP des Benutzerkontos (Registry, unexpandiert)
void RequestRefresh();

bool DlgVolume(HWND parent, rdb::VolumeConfig& cfg, bool isNew, bool mounted);
bool DlgSettings(HWND parent);   // true = Sprache oder Darstellung geaendert
void DlgTemp(HWND parent);
void DlgCache(HWND parent);   // cache.cpp: Cache von Browsern/Programmen verlagern
void DlgAbout(HWND parent);

} // namespace gui
