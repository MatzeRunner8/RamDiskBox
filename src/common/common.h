#pragma once
// Gemeinsame Hilfen fuer Dienst (RamDiskBoxSvc), Kommandozeile (rdbctl) und Oberflaeche (RamDiskBox).
// Im Dienst muss <winfsp/winfsp.h> VOR dieser Datei eingebunden werden.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <string>
#include <vector>
#include <map>
#include <utility>

namespace rdb {

// Windows vergleicht Pipe-, Dienst-, Datei- und Registry-Namen ohne Gross-/Kleinschreibung:
// Installationen und Einstellungen von "RAMDiskBox" (bis 1.2) werden weiter gefunden.
inline constexpr wchar_t kAppName[] = L"RamDiskBox";
inline constexpr wchar_t kPipeName[] = L"\\\\.\\pipe\\RamDiskBox.Control";
// Pipe-Name; die Umgebungsvariable RDB_INSTANCE haengt einen Zusatz an (getrennte Testinstanzen).
const wchar_t* PipeName();
std::wstring InstanceSuffix();   // "" oder ".<RDB_INSTANCE>"
inline constexpr wchar_t kServiceName[] = L"RamDiskBox";
inline constexpr wchar_t kServiceDisplay[] = L"RamDiskBox";
inline constexpr wchar_t kSvcExe[] = L"RamDiskBoxSvc.exe";
inline constexpr wchar_t kGuiExe[] = L"RamDiskBox.exe";
inline constexpr wchar_t kConfigFile[] = L"RamDiskBox.ini";      // gehoert dem Dienst
inline constexpr wchar_t kGuiConfigFile[] = L"RamDiskBoxGui.ini"; // gehoert der Oberflaeche
inline constexpr wchar_t kLogFile[] = L"RamDiskBox.log";
inline constexpr DWORD kPipeBuffer = 256 * 1024;

// ---------------------------------------------------------------- Ablageorte
// Portabel (Standard): alles neben der EXE. Installation fuer alle Benutzer (Setup legt die
// Markierungsdatei kMachineMarker neben die Programme): Dienstdaten (INI, Protokoll, Images) in
// %ProgramData%\RamDiskBox, schreibbar nur fuer SYSTEM/Administratoren; Einstellungen der
// Oberflaeche je Benutzer in %APPDATA%\RamDiskBox.
inline constexpr wchar_t kMachineMarker[] = L"RamDiskBox.machine";
bool MachineInstall();
std::wstring ServiceDataDir();
std::wstring ServicePath(const wchar_t* fileName);   // Datei im Dienst-Datenordner
std::wstring GuiDataDir();
std::wstring ImagesDir();                            // Standardordner fuer Image-Dateien
bool ProtectDir(const std::wstring& dir);            // SYSTEM/Administratoren voll, Benutzer nur lesen

// ---------------------------------------------------------------- Sprache (Deutsch/Englisch)
// Standard ist die Anzeigesprache von Windows (Deutsch -> Deutsch, sonst Englisch); die Umgebungs-
// variable RDB_LANG=de|en ueberschreibt sie. PipeCall schickt die Sprache des Aufrufers mit
// (lang=de|en), der Dienst antwortet in dieser Sprache.
enum class Lang { De = 0, En = 1 };
Lang DefaultLanguage();                     // RDB_LANG, sonst Windows-Anzeigesprache
Lang ProcessLanguage();                     // Standard dieses Prozesses (anfangs DefaultLanguage)
void SetProcessLanguage(Lang l);
Lang CurrentLanguage();                     // Sprache des aufrufenden Threads, sonst ProcessLanguage
void SetThreadLanguage(Lang l);             // nur fuer den aufrufenden Thread
const wchar_t* LangCode(Lang l);            // "de" / "en"
bool ParseLang(const std::wstring& s, Lang& out);
inline bool En() { return CurrentLanguage() == Lang::En; }
// Text in der aktuellen Sprache: Tr(L"Deutsch", L"English")
inline const wchar_t* Tr(const wchar_t* de, const wchar_t* en) { return En() ? en : de; }
// printf-Format in der aktuellen Sprache: TrF(L"Laufwerk %c:", L"Drive %c:", letter)
std::wstring TrF(const wchar_t* de, const wchar_t* en, ...);
// Zeitstempel: gespeichert/uebertragen als "2026-11-19 18:04:05", angezeigt je Sprache
std::wstring Timestamp();
std::wstring DisplayTime(const std::wstring& stamp);   // De: "19.11.2026 18:04:05", En: "2026-11-19 18:04:05"

using KV = std::map<std::wstring, std::wstring>;

// Einfaches Textprotokoll ueber die Named Pipe (UTF-16, eine Nachricht je Anfrage):
//   Zeile 1: Befehl bzw. "OK"/"ERR"
//   key=value-Zeilen gehoeren zu args, bis eine "[name]"-Zeile einen Abschnitt beginnt.
struct Message {
    std::wstring cmd;
    KV args;
    std::vector<std::pair<std::wstring, KV>> sections;

    std::wstring Get(const std::wstring& k, const std::wstring& def = L"") const;
    std::wstring Serialize() const;
    static Message Parse(const std::wstring& text);
    static Message Ok() { Message m; m.cmd = L"OK"; return m; }
    static Message Err(const std::wstring& text) { Message m; m.cmd = L"ERR"; m.args[L"message"] = text; return m; }
    bool IsOk() const { return cmd == L"OK"; }
};

// Ruft den Dienst auf. false + err bei Verbindungsfehler oder ERR-Antwort.
bool PipeCall(const Message& req, Message& resp, std::wstring* err, DWORD timeoutMs = 5000);
bool ServiceReachable();

// Einstellungen eines Laufwerks (Dienst-Konfiguration + Protokoll).
struct VolumeConfig {
    wchar_t letter = 0;
    unsigned long long size = 0;      // Bytes
    std::wstring label = L"RAMDisk";
    std::wstring folders;             // durch ';' getrennt, z. B. "Temp"
    std::wstring image;               // Image-Datei (leer = keine)
    bool loadImage = true;            // beim Einhaengen laden
    bool saveImage = true;            // beim Aushaengen/Herunterfahren speichern
    bool compressImage = true;
    unsigned autoSaveMin = 0;         // 0 = aus
    bool autoMount = true;            // beim Start des Dienstes einhaengen
    bool reportNtfs = true;           // Dateisystemname NTFS melden (Kompatibilitaet)
    bool useCache = true;             // Windows-Dateicache nutzen
    bool wipe = false;                // freigegebenen Speicher ueberschreiben
    int compress = 0;                 // Komprimierung im RAM: 0 aus, 1 schnell, 2 normal, 3 stark
    std::wstring source;              // Quellordner, wird beim Einhaengen kopiert (wenn kein Image geladen wird)
    bool avExclude = false;           // vom Defender-Echtzeitschutz ausnehmen (solange eingehaengt)

    KV ToKV() const;
    static bool FromKV(const KV& kv, VolumeConfig& out, std::wstring& err);
};

// Hilfsfunktionen
std::wstring ExeDir();
std::wstring ExePath(const wchar_t* fileName);
std::wstring Trim(const std::wstring& s);
std::wstring Lower(std::wstring s);
std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep);
std::wstring FormatSize(unsigned long long bytes);           // "1,50 GB" (De) bzw. "1.50 GB" (En)
bool ParseSize(const std::wstring& text, unsigned long long& out); // "512M", "4G", "1,5 GB", reine Zahl = MB
std::wstring U64(unsigned long long v);
unsigned long long ToU64(const std::wstring& s, unsigned long long def = 0);
bool ToBool(const std::wstring& s, bool def = false);
std::wstring Win32Message(DWORD code);                       // Systemtext in der aktuellen Sprache, falls vorhanden
const wchar_t* CompressName(int level);            // off, fast, normal, max
int ParseCompress(const std::wstring& s, int def); // Name oder Ziffer, -1 = ungueltig
bool IsDriveLetterUsed(wchar_t letter);           // belegt, verbunden oder fuer ein getrenntes Netzlaufwerk gemerkt
bool IsRememberedNetworkLetter(wchar_t letter);   // HKCU\Network\<Buchstabe> (des aufrufenden Benutzers)

// INI (UTF-16, liegt neben der EXE)
struct Ini {
    std::wstring path;
    explicit Ini(std::wstring p);
    std::wstring Get(const wchar_t* sec, const wchar_t* key, const wchar_t* def = L"") const;
    unsigned long long GetU64(const wchar_t* sec, const wchar_t* key, unsigned long long def) const;
    void Set(const wchar_t* sec, const wchar_t* key, const std::wstring& val) const;
    void DeleteSection(const wchar_t* sec) const;
    void DeleteKey(const wchar_t* sec, const wchar_t* key) const;
    std::vector<std::wstring> Sections() const;
};

} // namespace rdb
