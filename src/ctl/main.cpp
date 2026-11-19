// rdbctl.exe - Kommandozeile fuer RamDiskBox
// Sprache: Windows-Anzeigesprache (Deutsch -> Deutsch, sonst Englisch); RDB_LANG=de|en ueberschreibt sie.
#include "../common/common.h"
#include "../common/version.h"
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>

using namespace rdb;

static void Out(const std::wstring& s)
{
    DWORD w;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(h, &w)) WriteConsoleW(h, s.data(), (DWORD)s.size(), &w, nullptr);
    else {
        int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
        std::string u(n, '\0');
        WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), u.data(), n, nullptr, nullptr);
        WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
    }
}

static const wchar_t* YesNo(bool b) { return b ? Tr(L"ja", L"yes") : Tr(L"nein", L"no"); }

static int Fail(const std::wstring& msg)
{
    Out(Tr(L"Fehler: ", L"Error: ") + msg + L"\n");
    return 1;
}

static void Usage()
{
    struct Line { const wchar_t* synDe; const wchar_t* synEn; const wchar_t* de; const wchar_t* en; };
    static const Line lines[] = {
        { L"list", L"list",
          L"RAM-Laufwerke mit aktuellem Zustand", L"RAM drives and their current state" },
        { L"create <L> <Größe> [Optionen]", L"create <L> <size> [options]",
          L"anlegen und einhängen", L"create and mount" },
        { L"update <L> [Größe] [Optionen]", L"update <L> [size] [options]",
          L"Einstellungen ändern", L"change settings" },
        { L"mount <L>", L"mount <L>",
          L"einhängen (lädt das Image)", L"mount (loads the image)" },
        { L"dismount <L> [force] [discard]", L"dismount <L> [force] [discard]",
          L"aushängen (speichert das Image)", L"dismount (saves the image)" },
        { L"resize <L> <Größe>", L"resize <L> <size>",
          L"Größe im laufenden Betrieb ändern", L"change the size while mounted" },
        { L"save <L> [Pfad]", L"save <L> [path]",
          L"Image jetzt speichern (Pfad = neues Image)", L"save the image now (path = new image)" },
        { L"delete <L> [force] [discard]", L"delete <L> [force] [discard]",
          L"Laufwerk entfernen", L"remove the drive" },
        { L"iostats", L"iostats",
          L"E/A-Zähler seit Dienststart", L"I/O counters since service start" },
        { L"lifetime [reset]", L"lifetime [reset]",
          L"Gesamtzähler und Spitzenwerte", L"lifetime totals and peak rates" },
        { L"settings [allowuser on|off]", L"settings [allowuser on|off]",
          L"Dienst-Einstellungen", L"service settings" },
        { L"status", L"status",
          L"läuft der Dienst?", L"is the service running?" },
        { L"quit", L"quit",
          L"portable Instanz beenden (speichert Images)", L"stop the portable instance (saves images)" },
    };
    size_t w = 0;
    for (auto& l : lines) w = std::max(w, wcslen(Tr(l.synDe, l.synEn)));

    std::wstring s = std::wstring(L"rdbctl ") + RDB_VERSION_W + L" (Build " + RDB_BUILD_W + L") - RamDiskBox " +
        Tr(L"Kommandozeile", L"command line") + L"\n\n";
    for (auto& l : lines) {
        std::wstring syn = Tr(l.synDe, l.synEn);
        s += L"  rdbctl " + syn + std::wstring(w - syn.size() + 2, L' ') + Tr(l.de, l.en) + L"\n";
    }
    s += L"\n";
    s += Tr(
        L"Größe: z. B. 512M, 4G, 1.5G (ohne Einheit = MB)\n"
        L"Optionen: label=Name  folders=Temp;Cache  image=C:\\RAMDisk\\R.img  autosave=Minuten\n"
        L"          compress=off|fast|normal|max   (Komprimierung im RAM: LZ4, LZ4 HC, zstd)\n"
        L"          source=D:\\Vorlage   (Ordner beim Einhängen kopieren, wenn kein Image geladen wird)\n"
        L"          avexclude   (vom Defender-Echtzeitschutz ausnehmen, nur Dienstmodus; weniger Schutz)\n"
        L"          noload  nosave  nocompress  noauto  nontfs  nocache  wipe  nomount\n"
        L"          (für update auch: load save compress auto ntfs cache nowipe noavexclude)\n"
        L"Sprache: RDB_LANG=de|en (Standard: Windows-Anzeigesprache)\n",
        L"Size: e.g. 512M, 4G, 1.5G (no unit = MB)\n"
        L"Options: label=Name  folders=Temp;Cache  image=C:\\RAMDisk\\R.img  autosave=minutes\n"
        L"         compress=off|fast|normal|max   (compression in RAM: LZ4, LZ4 HC, zstd)\n"
        L"         source=D:\\Template   (folder copied on mount when no image is loaded)\n"
        L"         avexclude   (exclude from Defender real-time protection, service mode only; less protection)\n"
        L"         noload  nosave  nocompress  noauto  nontfs  nocache  wipe  nomount\n"
        L"         (update also accepts: load save compress auto ntfs cache nowipe noavexclude)\n"
        L"Language: RDB_LANG=de|en (default: Windows display language)\n");
    Out(s);
}

static bool ApplyOptions(VolumeConfig& c, int argc, wchar_t** argv, int start, std::wstring& err, bool* noMount)
{
    for (int i = start; i < argc; i++) {
        std::wstring a = argv[i];
        std::wstring l = Lower(a);
        size_t eq = a.find(L'=');
        if (eq != std::wstring::npos) {
            std::wstring k = Lower(a.substr(0, eq)), v = a.substr(eq + 1);
            if (k == L"label") c.label = v.substr(0, 32);
            else if (k == L"folders") c.folders = v;
            else if (k == L"image") c.image = v;
            else if (k == L"autosave") c.autoSaveMin = (unsigned)ToU64(v);
            else if (k == L"source") c.source = v;
            else if (k == L"compress") {
                c.compress = ParseCompress(v, -1);
                if (c.compress < 0) { err = Tr(L"compress= erwartet off, fast, normal oder max.", L"compress= expects off, fast, normal or max."); return false; }
            }
            else if (k == L"size") { if (!ParseSize(v, c.size)) { err = Tr(L"Ungültige Größe: ", L"Invalid size: ") + v; return false; } }
            else { err = Tr(L"Unbekannte Option: ", L"Unknown option: ") + a; return false; }
            continue;
        }
        if (l == L"noload") c.loadImage = false;
        else if (l == L"load") c.loadImage = true;
        else if (l == L"nosave") c.saveImage = false;
        else if (l == L"save") c.saveImage = true;
        else if (l == L"nocompress") c.compressImage = false;
        else if (l == L"compress") c.compressImage = true;
        else if (l == L"noauto") c.autoMount = false;
        else if (l == L"auto") c.autoMount = true;
        else if (l == L"nontfs") c.reportNtfs = false;
        else if (l == L"ntfs") c.reportNtfs = true;
        else if (l == L"nocache") c.useCache = false;
        else if (l == L"cache") c.useCache = true;
        else if (l == L"avexclude") c.avExclude = true;
        else if (l == L"noavexclude") c.avExclude = false;
        else if (l == L"wipe") c.wipe = true;
        else if (l == L"nowipe") c.wipe = false;
        else if (l == L"nomount" && noMount) *noMount = true;
        else {
            unsigned long long s;
            if (ParseSize(a, s)) { c.size = s; continue; }
            err = Tr(L"Unbekannte Option: ", L"Unknown option: ") + a;
            return false;
        }
    }
    return true;
}

static bool Call(const Message& req, Message& resp)
{
    std::wstring err;
    if (!PipeCall(req, resp, &err, 600000)) {
        Out(Tr(L"Fehler: ", L"Error: ") + err + L"\n");
        return false;
    }
    if (!resp.Get(L"warning").empty()) Out(Tr(L"Hinweis: ", L"Note: ") + resp.Get(L"warning") + L"\n");
    return true;
}

static bool FindVolume(wchar_t letter, VolumeConfig& c, std::wstring& err)
{
    Message req, resp;
    req.cmd = L"LIST";
    if (!PipeCall(req, resp, &err)) return false;
    for (auto& [name, kv] : resp.sections) {
        if (!name.empty() && towupper(name[0]) == towupper(letter))
            return VolumeConfig::FromKV(kv, c, err);
    }
    err = std::wstring(Tr(L"Kein RAM-Laufwerk ", L"No RAM drive ")) + (wchar_t)towupper(letter) + L":.";
    return false;
}

static const wchar_t* StateText(const std::wstring& s)
{
    if (s == L"mounted") return Tr(L"eingehängt", L"mounted");
    if (s == L"saving") return Tr(L"speichert", L"saving");
    return Tr(L"ausgehängt", L"dismounted");
}

// Anzeigename der RAM-Komprimierung (die Schluesselwoerter off/fast/normal/max bleiben fuer Optionen)
static const wchar_t* CompressDisplay(int level)
{
    switch (level) {
    case 1: return Tr(L"schnell (LZ4)", L"fast (LZ4)");
    case 2: return L"normal (LZ4 HC)";
    case 3: return Tr(L"stark (zstd)", L"strong (zstd)");
    }
    return Tr(L"aus", L"off");
}

static int CmdList()
{
    Message req, resp;
    req.cmd = L"LIST";
    if (!Call(req, resp)) return 1;
    Out(L"RamDiskBox " + resp.Get(L"version") + L" (" +
        (resp.Get(L"mode") == L"service" ? Tr(L"Dienst", L"service") : Tr(L"portabel", L"portable")) + L")\n");
    if (resp.sections.empty()) { Out(Tr(L"Keine RAM-Laufwerke eingerichtet.\n", L"No RAM drives configured.\n")); return 0; }
    for (auto& [name, kv] : resp.sections) {
        auto g = [&](const wchar_t* k) { auto it = kv.find(k); return it == kv.end() ? std::wstring() : it->second; };
        std::wstring line = name + L":  " + StateText(g(L"state")) + L"  " + FormatSize(ToU64(g(L"size")));
        if (g(L"state") != L"dismounted") {
            line += Tr(L"  belegt ", L"  used ") + FormatSize(ToU64(g(L"used")));
            line += L"  RAM " + FormatSize(ToU64(g(L"memory")));
            line += L"  " + g(L"files") + Tr(L" Dateien, ", g(L"files") == L"1" ? L" file, " : L" files, ") +
                g(L"dirs") + Tr(L" Ordner, ", g(L"dirs") == L"1" ? L" folder, " : L" folders, ") +
                g(L"open") + Tr(L" offen", L" open");
        }
        line += Tr(L"\n    Bezeichnung: ", L"\n    Label: ") + g(L"label");
        if (!g(L"folders").empty()) line += Tr(L"   Ordner: ", L"   Folders: ") + g(L"folders");
        line += L"\n    Image: " + (g(L"image").empty() ? std::wstring(Tr(L"(keins)", L"(none)")) : g(L"image"));
        if (!g(L"image").empty()) {
            line += Tr(L"  [laden ", L"  [load ") + std::wstring(YesNo(ToBool(g(L"loadimage"))));
            line += Tr(L", speichern ", L", save ") + std::wstring(YesNo(ToBool(g(L"saveimage"))));
            if (ToU64(g(L"autosave"))) line += Tr(L", alle ", L", every ") + g(L"autosave") + L" min";
            line += L"]";
        }
        line += Tr(L"\n    Autostart: ", L"\n    Auto-mount: ") + std::wstring(YesNo(ToBool(g(L"automount"))));
        line += Tr(L"   NTFS melden: ", L"   Report NTFS: ") + std::wstring(YesNo(ToBool(g(L"ntfs"))));
        line += L"   Cache: " + std::wstring(YesNo(ToBool(g(L"cache"))));
        line += Tr(L"   Überschreiben: ", L"   Wipe: ") + std::wstring(YesNo(ToBool(g(L"wipe"))));
        {
            int lvl = ParseCompress(g(L"compress"), 0);
            line += Tr(L"\n    Komprimierung: ", L"\n    Compression: ") + std::wstring(CompressDisplay(lvl));
            unsigned long long raw = ToU64(g(L"compraw")), st = ToU64(g(L"compstored"));
            if (raw) {
                line += L", " + FormatSize(raw) + L" -> " + FormatSize(st) + L" (" + U64((raw > st ? raw - st : 0) * 100 / raw) +
                    Tr(L" % gespart)", L"% saved)");
            }
        }
        if (!g(L"source").empty()) line += Tr(L"\n    Quellordner: ", L"\n    Source folder: ") + g(L"source");
        if (ToBool(g(L"avexclude"))) {
            line += Tr(L"\n    Defender-Ausnahme: ja", L"\n    Defender exclusion: yes");
            line += ToBool(g(L"avactive")) ? Tr(L" (aktiv)", L" (active)") : Tr(L" (nicht aktiv)", L" (not active)");
        }
        if (!g(L"lastsave").empty()) line += Tr(L"\n    Zuletzt gespeichert: ", L"\n    Last saved: ") + DisplayTime(g(L"lastsave"));
        if (!g(L"lasterror").empty()) line += Tr(L"\n    Hinweis: ", L"\n    Note: ") + g(L"lasterror");
        Out(line + L"\n");
    }
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    // Sprache: RDB_LANG=de|en, sonst Windows-Anzeigesprache; PipeCall gibt sie an den Dienst weiter.
    SetProcessLanguage(DefaultLanguage());
    if (argc < 2) { Usage(); return 0; }
    std::wstring cmd = Lower(argv[1]);
    auto letterArg = [&](int i, wchar_t& l) -> bool {
        if (argc <= i || !iswalpha(argv[i][0])) return false;
        l = (wchar_t)towupper(argv[i][0]);
        return true;
    };
    wchar_t L = 0;
    Message req, resp;

    if (cmd == L"help" || cmd == L"/?" || cmd == L"-h" || cmd == L"--help") { Usage(); return 0; }
    if (cmd == L"list") return CmdList();
    if (cmd == L"status") {
        req.cmd = L"PING";
        std::wstring err;
        if (!PipeCall(req, resp, &err, 2000)) return Fail(err);
        Out(L"RamDiskBox " + resp.Get(L"version") + L" (Build " + resp.Get(L"build") + L") " +
            (resp.Get(L"mode") == L"service" ? Tr(L"läuft als Dienst.", L"is running as a service.")
                                             : Tr(L"läuft als portabler Prozess.", L"is running as a portable process.")) + L"\n");
        return 0;
    }
    if (cmd == L"create") {
        if (!letterArg(2, L) || argc < 4) return Fail(Tr(L"Aufruf: rdbctl create <L> <Größe> [Optionen]", L"Usage: rdbctl create <L> <size> [options]"));
        VolumeConfig c;
        c.letter = L;
        if (!ParseSize(argv[3], c.size)) return Fail(Tr(L"Ungültige Größe: ", L"Invalid size: ") + std::wstring(argv[3]));
        std::wstring err;
        bool noMount = false;
        if (!ApplyOptions(c, argc, argv, 4, err, &noMount)) return Fail(err);
        req.cmd = L"CREATE";
        req.args = c.ToKV();
        req.args[L"mount"] = noMount ? L"0" : L"1";
        if (!Call(req, resp)) return 1;
        Out(std::wstring(Tr(L"RAM-Laufwerk ", L"RAM drive ")) + L + L": " +
            (noMount ? Tr(L"angelegt.", L"created.") : Tr(L"angelegt und eingehängt.", L"created and mounted.")) + L"\n");
        return 0;
    }
    if (cmd == L"update") {
        if (!letterArg(2, L)) return Fail(Tr(L"Aufruf: rdbctl update <L> [Größe] [Optionen]", L"Usage: rdbctl update <L> [size] [options]"));
        VolumeConfig c;
        std::wstring err;
        if (!FindVolume(L, c, err)) return Fail(err);
        if (!ApplyOptions(c, argc, argv, 3, err, nullptr)) return Fail(err);
        req.cmd = L"UPDATE";
        req.args = c.ToKV();
        if (!Call(req, resp)) return 1;
        Out(Tr(L"Einstellungen gespeichert.\n", L"Settings saved.\n"));
        return 0;
    }
    if (cmd == L"mount" || cmd == L"dismount" || cmd == L"delete" || cmd == L"save" || cmd == L"resize") {
        if (!letterArg(2, L)) return Fail(Tr(L"Laufwerksbuchstabe fehlt.", L"Drive letter missing."));
        req.args[L"letter"] = std::wstring(1, L);
        if (cmd == L"mount") req.cmd = L"MOUNT";
        else if (cmd == L"save") {
            req.cmd = L"SAVE";
            if (argc > 3) req.args[L"path"] = argv[3];
        } else if (cmd == L"resize") {
            unsigned long long s;
            if (argc < 4 || !ParseSize(argv[3], s)) return Fail(Tr(L"Aufruf: rdbctl resize <L> <Größe>", L"Usage: rdbctl resize <L> <size>"));
            req.cmd = L"RESIZE";
            req.args[L"size"] = U64(s);
        } else {
            req.cmd = cmd == L"delete" ? L"DELETE" : L"DISMOUNT";
            for (int i = 3; i < argc; i++) {
                std::wstring o = Lower(argv[i]);
                if (o == L"force") req.args[L"force"] = L"1";
                else if (o == L"discard") req.args[L"discard"] = L"1";
                else return Fail(Tr(L"Unbekannte Option: ", L"Unknown option: ") + o);
            }
        }
        if (!Call(req, resp)) return 1;
        Out(L"OK\n");
        return 0;
    }
    if (cmd == L"iostats" || cmd == L"lifetime") {
        if (cmd == L"lifetime" && argc > 2 && Lower(argv[2]) == L"reset") {
            req.cmd = L"LIFETIMERESET";
            if (!Call(req, resp)) return 1;
            Out(Tr(L"Gesamtzähler zurückgesetzt.\n", L"Lifetime counters reset.\n"));
            return 0;
        }
        req.cmd = L"LIST";
        if (!Call(req, resp)) return 1;
        if (cmd == L"iostats") {
            Out(Tr(L"Seit Dienststart: gelesen ", L"Since service start: read ") + FormatSize(ToU64(resp.Get(L"ioread"))) +
                L" (" + resp.Get(L"ioreadops") + Tr(L" Vorgänge), geschrieben ", L" operations), written ") +
                FormatSize(ToU64(resp.Get(L"iowritten"))) + L" (" + resp.Get(L"iowriteops") + Tr(L" Vorgänge)\n", L" operations)\n"));
        } else {
            Out(Tr(L"Seit ", L"Since ") + DisplayTime(resp.Get(L"since")) +
                Tr(L": gelesen ", L": read ") + FormatSize(ToU64(resp.Get(L"liferead"))) +
                Tr(L", geschrieben ", L", written ") + FormatSize(ToU64(resp.Get(L"lifewritten"))) +
                Tr(L"\nSpitze: lesen ", L"\nPeak: read ") + FormatSize(ToU64(resp.Get(L"peakread"))) +
                Tr(L"/s, schreiben ", L"/s, write ") + FormatSize(ToU64(resp.Get(L"peakwrite"))) + L"/s\n");
        }
        return 0;
    }
    if (cmd == L"quit") {
        req.cmd = L"QUIT";
        if (!Call(req, resp)) return 1;
        Out(Tr(L"Portable Instanz wird beendet (Images werden gespeichert).\n",
               L"Portable instance is shutting down (images are being saved).\n"));
        return 0;
    }
    if (cmd == L"settings") {
        req.cmd = L"SETTINGS";
        if (argc > 3 && Lower(argv[2]) == L"allowuser") req.args[L"allowuser"] = ToBool(argv[3]) ? L"1" : L"0";
        else if (argc > 2) return Fail(Tr(L"Aufruf: rdbctl settings [allowuser on|off]", L"Usage: rdbctl settings [allowuser on|off]"));
        if (!Call(req, resp)) return 1;
        Out(std::wstring(Tr(L"Benutzer dürfen steuern: ", L"Users may control: ")) + YesNo(resp.Get(L"allowuser") == L"1") + L"\n");
        return 0;
    }
    Usage();
    return 1;
}
