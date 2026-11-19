// RamDiskBox-Dienst: Laufwerksverwaltung, Steuer-Pipe, Statistik
#include "host.h"
#include "../common/version.h"
#include <sddl.h>
#include <cstdarg>
#include <cwctype>
#include <algorithm>

namespace rdb {

// ----------------------------------------------------------------------- Protokoll-Datei

static std::mutex g_logMu;

// Das Protokoll ist in der Sprache des Dienstes geschrieben, nicht in der des Clients.
static bool LogEn() { return ProcessLanguage() == Lang::En; }
static const wchar_t* LT(const wchar_t* de, const wchar_t* en) { return LogEn() ? en : de; }
static std::wstring LogSize(unsigned long long bytes)
{
    Lang cur = CurrentLanguage();
    SetThreadLanguage(ProcessLanguage());
    std::wstring s = FormatSize(bytes);
    SetThreadLanguage(cur);
    return s;
}

void Log(const wchar_t* de, const wchar_t* en, ...)
{
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, en);
    _vsnwprintf_s(msg, _TRUNCATE, LogEn() ? en : de, ap);
    va_end(ap);
    std::wstring line = Timestamp() + L"  " + msg + L"\r\n";
    std::lock_guard<std::mutex> g(g_logMu);
    std::wstring path = ServicePath(kLogFile);
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad) && fad.nFileSizeLow > 1024 * 1024) {
        std::wstring old = path + L".1";
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    HANDLE h = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string u(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, line.c_str(), -1, u.data(), n, nullptr, nullptr);
    DWORD w;
    WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(h);
}

std::wstring NtMessage(NTSTATUS st)
{
    switch (st) {
    case STATUS_OBJECT_NAME_COLLISION: return Tr(L"Laufwerksbuchstabe ist bereits belegt.", L"Drive letter is already in use.");
    case STATUS_ACCESS_DENIED: return Tr(L"Zugriff verweigert.", L"Access denied.");
    case STATUS_INSUFFICIENT_RESOURCES: return Tr(L"Zu wenig Arbeitsspeicher.", L"Not enough memory.");
    }
    std::wstring m = Win32Message(FspWin32FromNtStatus(st));
    wchar_t code[32];
    swprintf_s(code, L" (0x%08X)", (unsigned)st);
    return m + code;
}

static std::wstring SectionName(wchar_t letter) { return std::wstring(L"Volume.") + letter; }

// ----------------------------------------------------------------------- Start/Stopp

bool Host::Start(bool serviceMode, std::wstring& err)
{
    serviceMode_ = serviceMode;
    clientLang_ = (int)ProcessLanguage();
    startTick_ = GetTickCount64();
    stopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    // Datenordner; bei einer Installation fuer alle Benutzer in ProgramData und nur fuer SYSTEM und
    // Administratoren schreibbar, weil der Dienst INI und Image-Pfaden darin vertraut.
    CreateDirectoryW(ServiceDataDir().c_str(), nullptr);
    if (MachineInstall() && serviceMode && !ProtectDir(ServiceDataDir()))
        Log(L"Rechte des Datenordners %s konnten nicht gesetzt werden.", L"Could not set permissions of the data folder %s.",
            ServiceDataDir().c_str());
    CreateDirectoryW(ImagesDir().c_str(), nullptr);
    ini_ = std::make_unique<Ini>(ServicePath(kConfigFile));
    allowUser_ = ToBool(ini_->Get(L"Service", L"AllowUser", L"1"), true);
    lifeRead_ = ini_->GetU64(L"Stats", L"LifetimeRead", 0);
    lifeWritten_ = ini_->GetU64(L"Stats", L"LifetimeWritten", 0);
    peakRead_ = ini_->GetU64(L"Stats", L"PeakRead", 0);
    peakWrite_ = ini_->GetU64(L"Stats", L"PeakWrite", 0);
    if (ini_->Get(L"Stats", L"Since", L"").empty()) ini_->Set(L"Stats", L"Since", Timestamp());

    // Nur eine Instanz: erste Pipe-Instanz exklusiv anlegen (Probe).
    {
        std::wstring sddl = PipeSddl();
        PSECURITY_DESCRIPTOR sd = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
        SECURITY_ATTRIBUTES sa{ sizeof sa, sd, FALSE };
        HANDLE probe = CreateNamedPipeW(PipeName(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, kPipeBuffer, kPipeBuffer, 0, &sa);
        if (sd) LocalFree(sd);
        if (probe == INVALID_HANDLE_VALUE) {
            err = Tr(L"Es läuft bereits eine RamDiskBox-Instanz (Dienst oder portabel).",
                     L"A RamDiskBox instance is already running (service or portable).");
            return false;
        }
        CloseHandle(probe);
    }

    LoadConfig();
    Log(L"RamDiskBox %s (Build %s) gestartet, Modus: %s", L"RamDiskBox %s (build %s) started, mode: %s",
        RDB_VERSION_W, RDB_BUILD_W, serviceMode ? LT(L"Dienst", L"service") : LT(L"portabel", L"portable"));

    // Übrig gebliebene Defender-Ausnahmen (z. B. nach einem Absturz) entfernen; Einhängen setzt sie neu.
    for (wchar_t l = L'D'; l <= L'Z'; l++) {
        wchar_t key[2] = { l, 0 };
        if (ini_->Get(L"Exclusions", key, L"").empty()) continue;
        std::wstring e;
        if (DefenderExclusion(false, std::wstring(1, l) + L":\\", e))
            Log(L"Laufwerk %c: alte Defender-Ausnahme entfernt.", L"Drive %c: old Defender exclusion removed.", l);
        else
            Log(L"Laufwerk %c: alte Defender-Ausnahme konnte nicht entfernt werden: %s",
                L"Drive %c: could not remove old Defender exclusion: %s", l, e.c_str());
        ini_->DeleteKey(L"Exclusions", key);
    }

    std::vector<std::shared_ptr<Volume>> autos;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& [l, v] : vols_) if (v->cfg.autoMount) autos.push_back(v);
    }
    for (auto& v : autos) {
        std::lock_guard<std::mutex> g(v->op);
        std::wstring e;
        if (!MountVolume(*v, e)) {
            v->SetError(e);
            Log(L"Laufwerk %c: konnte nicht eingehängt werden: %s", L"Drive %c: could not be mounted: %s", v->cfg.letter, e.c_str());
        }
    }

    pipeThread_ = std::thread([this] { PipeLoop(); });
    timerThread_ = std::thread([this] { TimerLoop(); });
    return true;
}

void Host::Stop()
{
    if (stopping_.exchange(true)) return;
    SetEvent(stopEvent_);
    if (pipeThread_.joinable()) pipeThread_.join();
    if (timerThread_.joinable()) timerThread_.join();
    // laufende Clients kurz ausklingen lassen
    for (int i = 0; i < 50 && activeClients_.load() > 0; i++) Sleep(100);

    std::vector<std::shared_ptr<Volume>> list;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& [l, v] : vols_) list.push_back(v);
    }
    for (auto& v : list) {
        std::lock_guard<std::mutex> g(v->op);
        if (!v->fs) continue;
        std::wstring e;
        DismountVolume(*v, true, false, e);
        if (!e.empty()) Log(L"Laufwerk %c: %s", L"Drive %c: %s", v->cfg.letter, e.c_str());
    }
    PersistStats();
    Log(L"RamDiskBox beendet.", L"RamDiskBox stopped.");
}

// ----------------------------------------------------------------------- Konfiguration

void Host::LoadConfig()
{
    std::lock_guard<std::mutex> g(mu_);
    for (auto& sec : ini_->Sections()) {
        if (sec.size() != 8 || _wcsnicmp(sec.c_str(), L"Volume.", 7) != 0) continue;
        KV kv;
        const wchar_t* keys[] = { L"letter", L"size", L"label", L"folders", L"image", L"loadimage", L"saveimage",
            L"compressimage", L"autosave", L"automount", L"ntfs", L"cache", L"wipe", L"compress", L"source", L"avexclude" };
        for (auto k : keys) {
            std::wstring v = ini_->Get(sec.c_str(), k, L"\x01");
            if (v != L"\x01") kv[k] = v;
        }
        kv[L"letter"] = std::wstring(1, (wchar_t)towupper(sec[7]));
        VolumeConfig c;
        std::wstring err;
        if (!VolumeConfig::FromKV(kv, c, err)) {
            Log(L"Konfiguration %s ignoriert: %s", L"Configuration %s ignored: %s", sec.c_str(), err.c_str());
            continue;
        }
        auto v = std::make_shared<Volume>();
        v->cfg = c;
        vols_[c.letter] = v;
    }
}

void Host::WriteVolumeConfig(const VolumeConfig& c)
{
    std::wstring sec = SectionName(c.letter);
    for (auto& [k, val] : c.ToKV()) ini_->Set(sec.c_str(), k.c_str(), val);
}

void Host::PersistStats()
{
    std::lock_guard<std::mutex> g(statMu_);
    ini_->Set(L"Stats", L"LifetimeRead", U64(lifeRead_ + gc_.bytesRead.load() - offRead_));
    ini_->Set(L"Stats", L"LifetimeWritten", U64(lifeWritten_ + gc_.bytesWritten.load() - offWritten_));
    ini_->Set(L"Stats", L"PeakRead", U64(peakRead_));
    ini_->Set(L"Stats", L"PeakWrite", U64(peakWrite_));
}

std::shared_ptr<Volume> Host::Get(wchar_t letter)
{
    std::lock_guard<std::mutex> g(mu_);
    auto it = vols_.find((wchar_t)towupper(letter));
    return it == vols_.end() ? nullptr : it->second;
}

bool Host::ValidateConfig(const VolumeConfig& c, wchar_t ignoreLetter, std::wstring& err)
{
    if (!c.image.empty()) {
        const std::wstring& p = c.image;
        if (p.size() < 4 || p[1] != L':' || p[2] != L'\\' || !iswalpha(p[0])) {
            err = Tr(L"Der Image-Pfad muss vollständig sein, z. B. C:\\RAMDisk\\R.img.",
                     L"The image path must be a full path, e.g. C:\\RAMDisk\\R.img.");
            return false;
        }
        wchar_t d = (wchar_t)towupper(p[0]);
        std::lock_guard<std::mutex> g(mu_);
        if (d == c.letter) {
            err = Tr(L"Das Image darf nicht auf dem RAM-Laufwerk selbst liegen.", L"The image must not be stored on the RAM drive itself.");
            return false;
        }
        for (auto& [l, v] : vols_)
            if (l == d && l != ignoreLetter) {
                err = Tr(L"Das Image darf nicht auf einem RAM-Laufwerk liegen.", L"The image must not be stored on a RAM drive.");
                return false;
            }
    }
    for (auto& f : Split(c.folders, L';')) {
        if (f.find_first_of(L":*?\"<>|") != std::wstring::npos) {
            err = TrF(L"Ungültiger Ordnername: %s", L"Invalid folder name: %s", f.c_str());
            return false;
        }
    }
    if (!c.source.empty()) {
        const std::wstring& s = c.source;
        bool drive = s.size() >= 3 && iswalpha(s[0]) && s[1] == L':' && s[2] == L'\\';
        bool unc = s.size() > 2 && s[0] == L'\\' && s[1] == L'\\';
        if (!drive && !unc) {
            err = Tr(L"Der Quellordner muss ein vollständiger Pfad sein, z. B. D:\\Vorlage.",
                     L"The source folder must be a full path, e.g. D:\\Template.");
            return false;
        }
        if (drive && towupper(s[0]) == c.letter) {
            err = Tr(L"Der Quellordner darf nicht auf dem RAM-Laufwerk selbst liegen.", L"The source folder must not be on the RAM drive itself.");
            return false;
        }
    }
    return true;
}

// ----------------------------------------------------------------------- Laufwerke

// Fehlt eine Image-Datei, ist das nur dann "noch nicht angelegt", wenn ihr Laufwerk bzw. ihre
// Freigabe erreichbar ist und Windows "nicht gefunden" meldet (nicht etwa "Zugriff verweigert").
static bool ImageLocationReachable(const std::wstring& path, DWORD attrErr)
{
    if (attrErr != ERROR_FILE_NOT_FOUND && attrErr != ERROR_PATH_NOT_FOUND) return false;
    std::wstring root;
    if (path.size() >= 3 && path[1] == L':' && path[2] == L'\\') root = path.substr(0, 3);
    else if (path.rfind(L"\\\\", 0) == 0) {   // \\server\share\ ...
        size_t a = path.find(L'\\', 2), b = a == std::wstring::npos ? a : path.find(L'\\', a + 1);
        root = b == std::wstring::npos ? path + L"\\" : path.substr(0, b + 1);
    } else {
        return false;
    }
    return GetFileAttributesW(root.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool Host::MountVolume(Volume& v, std::wstring& err)
{
    if (v.fs) return true;
    if (IsDriveLetterUsed(v.cfg.letter)) {
        err = IsRememberedNetworkLetter(v.cfg.letter)
            ? TrF(L"Laufwerk %c: ist für ein (getrenntes) Netzlaufwerk reserviert.", L"Drive %c: is reserved for a (disconnected) network drive.", v.cfg.letter)
            : TrF(L"Laufwerk %c: ist bereits belegt.", L"Drive %c: is already in use.", v.cfg.letter);
        return false;
    }
    VolumeOptions o;
    o.letter = v.cfg.letter;
    o.capacity = v.cfg.size;
    o.label = v.cfg.label;
    o.reportNtfs = v.cfg.reportNtfs;
    o.useCache = v.cfg.useCache;
    o.wipe = v.cfg.wipe;
    o.compress = v.cfg.compress;
    std::unique_ptr<RamFs> fs;
    NTSTATUS st = RamFs::Create(o, &gc_, fs);
    if (!NT_SUCCESS(st)) { err = Tr(L"Dateisystem konnte nicht erzeugt werden: ", L"Could not create the file system: ") + NtMessage(st); return false; }
    v.SetError(L"");
    bool imageLoaded = false;
    if (!v.cfg.image.empty() && v.cfg.loadImage) {
        // Ein leer eingehaengtes Laufwerk ueberschreibt beim Aushaengen das Image. Leer starten darf es
        // deshalb nur, wenn das Image wirklich noch nicht existiert (erste Benutzung) oder beschaedigt ist
        // und beiseitegelegt wurde - nie, wenn es nur gerade nicht lesbar ist (gesperrt, keine Rechte,
        // Netz- oder USB-Laufwerk nicht da).
        DWORD attr = GetFileAttributesW(v.cfg.image.c_str());
        DWORD ae = attr == INVALID_FILE_ATTRIBUTES ? GetLastError() : 0;
        if (attr == INVALID_FILE_ATTRIBUTES && !ImageLocationReachable(v.cfg.image, ae)) {
            err = TrF(L"Der Speicherort des Images ist nicht erreichbar (%s). Das Laufwerk wird nicht eingehängt, "
                      L"damit das Image beim Aushängen nicht durch ein leeres Laufwerk ersetzt wird.",
                      L"The image location is not reachable (%s). The drive is not mounted, "
                      L"so that the image is not replaced by an empty drive when unmounting.", Win32Message(ae).c_str());
            return false;
        }
    }
    if (!v.cfg.image.empty() && v.cfg.loadImage && GetFileAttributesW(v.cfg.image.c_str()) != INVALID_FILE_ATTRIBUTES) {
        std::wstring le;
        bool notOpened = false;
        if (!fs->LoadImage(v.cfg.image, le, &notOpened)) {
            if (notOpened) {
                err = TrF(L"%s Das Laufwerk wird nicht eingehängt, damit das Image nicht überschrieben wird "
                          L"(z. B. von einem Sicherungsprogramm gesperrt oder fehlende Rechte).",
                          L"%s The drive is not mounted, so that the image is not overwritten "
                          L"(e.g. locked by a backup program or missing permissions).", le.c_str());
                return false;
            }
            // Defektes Image nicht überschreiben: beiseitelegen und leer starten.
            // Der Zusatz im Dateinamen ist sprachneutral (keine Meldung).
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t suf[64];
            swprintf_s(suf, L".damaged-%04u%02u%02u-%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            std::wstring bad = v.cfg.image + suf;
            if (!MoveFileExW(v.cfg.image.c_str(), bad.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                err = TrF(L"Das Image ist beschädigt (%s) und konnte nicht beiseitegelegt werden: %s Das Laufwerk wird nicht eingehängt.",
                          L"The image is damaged (%s) and could not be set aside: %s The drive is not mounted.",
                          le.c_str(), Win32Message(GetLastError()).c_str());
                return false;
            }
            std::wstring msg = TrF(L"Image konnte nicht geladen werden (%s). Es wurde nach %s verschoben; das Laufwerk startet leer.",
                                   L"The image could not be loaded (%s). It was moved to %s; the drive starts empty.",
                                   le.c_str(), bad.c_str());
            v.SetError(msg);
            Log(L"Laufwerk %c: %s", L"Drive %c: %s", v.cfg.letter, msg.c_str());
            fs.reset();
            st = RamFs::Create(o, &gc_, fs);
            if (!NT_SUCCESS(st)) { err = Tr(L"Dateisystem konnte nicht erzeugt werden: ", L"Could not create the file system: ") + NtMessage(st); return false; }
        } else {
            Log(L"Laufwerk %c: Image %s geladen.", L"Drive %c: image %s loaded.", v.cfg.letter, v.cfg.image.c_str());
            imageLoaded = true;
        }
    }
    for (auto& f : Split(v.cfg.folders, L';')) fs->EnsureFolder(f);
    // Quellordner als Vorlage, wenn kein Image geladen wurde
    if (!imageLoaded && !v.cfg.source.empty()) {
        RamFs::ImportStats ist;
        std::wstring ie;
        unsigned long long t0 = GetTickCount64();
        bool ok = fs->ImportFolder(v.cfg.source, ist, ie);
        Log(L"Laufwerk %c: Quellordner %s: %llu Dateien, %llu Ordner, %s, %llu übersprungen (%llu ms).",
            L"Drive %c: source folder %s: %llu files, %llu folders, %s, %llu skipped (%llu ms).", v.cfg.letter,
            v.cfg.source.c_str(), ist.files, ist.dirs, LogSize(ist.bytes).c_str(), ist.skipped, GetTickCount64() - t0);
        if (!ok || !ie.empty()) {
            v.SetError(Tr(L"Quellordner: ", L"Source folder: ") + ie);
            Log(L"Laufwerk %c: Quellordner: %s", L"Drive %c: source folder: %s", v.cfg.letter, ie.c_str());
        }
    }
    st = fs->Mount();
    if (!NT_SUCCESS(st)) {
        err = Tr(L"Einhängen fehlgeschlagen: ", L"Mounting failed: ") + NtMessage(st);
        return false;
    }
    {
        std::lock_guard<std::mutex> g(v.state);
        v.fs = std::shared_ptr<RamFs>(std::move(fs));
    }
    v.lastSaveTick = GetTickCount64();
    Log(L"Laufwerk %c: eingehängt (%s, %s).", L"Drive %c: mounted (%s, %s).", v.cfg.letter, LogSize(v.cfg.size).c_str(), v.cfg.label.c_str());
    if (v.cfg.avExclude) SetExclusion(v, true);
    return true;
}

bool Host::SaveVolume(Volume& v, const std::wstring& path, std::wstring& err)
{
    if (!v.fs) { err = Tr(L"Laufwerk ist nicht eingehängt.", L"The drive is not mounted."); return false; }
    if (path.empty()) { err = Tr(L"Für dieses Laufwerk ist keine Image-Datei eingestellt.", L"No image file is set for this drive."); return false; }
    v.saving = true;
    unsigned long long t0 = GetTickCount64();
    bool ok = v.fs->SaveImage(path, v.cfg.compressImage, err);
    v.saving = false;
    if (ok) {
        {
            std::lock_guard<std::mutex> g(v.state);
            v.lastSave = Timestamp();
        }
        v.lastSaveTick = GetTickCount64();
        Log(L"Laufwerk %c: Image gespeichert nach %s (%llu ms).", L"Drive %c: image saved to %s (%llu ms).",
            v.cfg.letter, path.c_str(), GetTickCount64() - t0);
    } else {
        Log(L"Laufwerk %c: Speichern fehlgeschlagen: %s", L"Drive %c: saving failed: %s", v.cfg.letter, err.c_str());
    }
    return ok;
}

bool Host::DismountVolume(Volume& v, bool force, bool discard, std::wstring& err)
{
    if (!v.fs) return true;
    unsigned long long open = v.fs->OpenHandles();
    if (!force && open > 0) {
        err = TrF(L"Auf dem Laufwerk sind noch %llu Datei(en) bzw. Ordner geöffnet.",
                  L"%llu file(s) or folder(s) are still open on the drive.", open);
        return false;
    }
    if (!discard && v.cfg.saveImage && !v.cfg.image.empty()) {
        std::wstring se;
        if (!SaveVolume(v, v.cfg.image, se)) {
            err = Tr(L"Image konnte nicht gespeichert werden: ", L"The image could not be saved: ") + se;
            if (!force) return false;
        }
    }
    // Bezeichnung übernehmen, falls sie im Explorer geändert wurde
    std::wstring lbl = v.fs->Label();
    if (lbl != v.cfg.label) {
        {
            std::lock_guard<std::mutex> g(v.state);
            v.cfg.label = lbl;
        }
        std::lock_guard<std::mutex> g(mu_);
        WriteVolumeConfig(v.cfg);
    }
    v.fs->Unmount();
    {
        std::lock_guard<std::mutex> g(v.state);
        v.fs.reset();   // zerstört das Dateisystem, sobald auch LIST es losgelassen hat
    }
    SetExclusion(v, false);
    Log(L"Laufwerk %c: ausgehängt%s.", L"Drive %c: dismounted%s.", v.cfg.letter,
        discard ? LT(L" (ohne Speichern)", L" (without saving)") : L"");
    return true;
}

// Defender-Ausnahme für das Laufwerk setzen bzw. entfernen (Aufrufer hält v.op)
void Host::SetExclusion(Volume& v, bool on)
{
    wchar_t key[2] = { v.cfg.letter, 0 };
    std::wstring path = std::wstring(1, v.cfg.letter) + L":\\";
    std::wstring e;
    if (on) {
        if (v.avActive) return;
        if (DefenderExclusion(true, path, e)) {
            v.avActive = true;
            std::lock_guard<std::mutex> g(mu_);
            ini_->Set(L"Exclusions", key, L"1");
            Log(L"Laufwerk %c: vom Defender-Echtzeitschutz ausgenommen.", L"Drive %c: excluded from Defender real-time protection.", v.cfg.letter);
        } else {
            v.SetError(Tr(L"Defender-Ausnahme nicht möglich: ", L"Defender exclusion not possible: ") + e);
            Log(L"Laufwerk %c: Defender-Ausnahme nicht möglich: %s", L"Drive %c: Defender exclusion not possible: %s", v.cfg.letter, e.c_str());
        }
        return;
    }
    bool recorded;
    {
        std::lock_guard<std::mutex> g(mu_);
        recorded = !ini_->Get(L"Exclusions", key, L"").empty();
    }
    if (!v.avActive && !recorded) return;
    if (DefenderExclusion(false, path, e))
        Log(L"Laufwerk %c: Defender-Ausnahme entfernt.", L"Drive %c: Defender exclusion removed.", v.cfg.letter);
    else
        Log(L"Laufwerk %c: Defender-Ausnahme konnte nicht entfernt werden: %s", L"Drive %c: could not remove Defender exclusion: %s",
            v.cfg.letter, e.c_str());
    v.avActive = false;
    std::lock_guard<std::mutex> g(mu_);
    ini_->DeleteKey(L"Exclusions", key);
}

// ----------------------------------------------------------------------- Befehle

Message Host::Handle(const Message& req, bool clientAdmin)
{
    const std::wstring& c = req.cmd;
    if (c == L"PING") {
        Message m = Message::Ok();
        m.args[L"version"] = RDB_VERSION_W;
        m.args[L"build"] = RDB_BUILD_W;
        m.args[L"mode"] = serviceMode_ ? L"service" : L"portable";
        m.args[L"exe"] = ExePath(kSvcExe);
        return m;
    }
    if (c == L"LIST") return CmdList();
    if (c == L"CREATE") return CmdCreate(req, clientAdmin);
    if (c == L"UPDATE") return CmdUpdate(req, clientAdmin);
    if (c == L"DELETE") return CmdDelete(req);
    if (c == L"MOUNT") return CmdMount(req);
    if (c == L"DISMOUNT") return CmdDismount(req);
    if (c == L"RESIZE") return CmdResize(req);
    if (c == L"SAVE") return CmdSave(req, clientAdmin);
    if (c == L"SETTINGS") return CmdSettings(req, clientAdmin);
    if (c == L"LIFETIMERESET") {
        {
            std::lock_guard<std::mutex> g(statMu_);
            lifeRead_ = lifeWritten_ = 0;
            offRead_ = gc_.bytesRead.load();
            offWritten_ = gc_.bytesWritten.load();
            peakRead_ = peakWrite_ = 0;
        }
        ini_->Set(L"Stats", L"Since", Timestamp());
        PersistStats();
        return Message::Ok();
    }
    if (c == L"QUIT") {
        if (serviceMode_) return Message::Err(Tr(L"Der Dienst wird über die Dienstverwaltung beendet.",
                                                 L"The service is stopped via the Windows service manager."));
        SetEvent(stopEvent_);
        return Message::Ok();
    }
    return Message::Err(Tr(L"Unbekannter Befehl: ", L"Unknown command: ") + c);
}

Message Host::CmdList()
{
    Message m = Message::Ok();
    m.args[L"version"] = RDB_VERSION_W;
    m.args[L"build"] = RDB_BUILD_W;
    m.args[L"mode"] = serviceMode_ ? L"service" : L"portable";
    m.args[L"allowuser"] = allowUser_ ? L"1" : L"0";
    m.args[L"exe"] = ExePath(kSvcExe);
    m.args[L"machine"] = MachineInstall() ? L"1" : L"0";
    m.args[L"imagesdir"] = ImagesDir();
    m.args[L"logfile"] = ServicePath(kLogFile);
    MEMORYSTATUSEX ms{ sizeof ms };
    GlobalMemoryStatusEx(&ms);
    m.args[L"ramtotal"] = U64(ms.ullTotalPhys);
    m.args[L"ramfree"] = U64(ms.ullAvailPhys);
    {
        std::lock_guard<std::mutex> g(statMu_);
        m.args[L"ioread"] = U64(gc_.bytesRead.load());
        m.args[L"iowritten"] = U64(gc_.bytesWritten.load());
        m.args[L"ioreadops"] = U64(gc_.readOps.load());
        m.args[L"iowriteops"] = U64(gc_.writeOps.load());
        m.args[L"liferead"] = U64(lifeRead_ + gc_.bytesRead.load() - offRead_);
        m.args[L"lifewritten"] = U64(lifeWritten_ + gc_.bytesWritten.load() - offWritten_);
        m.args[L"peakread"] = U64(peakRead_);
        m.args[L"peakwrite"] = U64(peakWrite_);
        m.args[L"speedread"] = U64(curRead_);
        m.args[L"speedwrite"] = U64(curWrite_);
        m.args[L"uptime"] = U64((GetTickCount64() - startTick_) / 1000);
    }
    m.args[L"since"] = ini_->Get(L"Stats", L"Since", L"");   // Timestamp() (ältere INIs: TT.MM.JJJJ), Anzeige mit DisplayTime
    std::vector<std::shared_ptr<Volume>> list;
    {
        std::lock_guard<std::mutex> g(mu_);
        for (auto& [l, v] : vols_) list.push_back(v);
    }
    for (auto& v : list) {
        // Kein Warten auf laufende Sicherungen: Zustand nur unter v->state kopieren.
        std::shared_ptr<RamFs> fs;
        VolumeConfig cfg;
        std::wstring lastError, lastSave;
        bool avActive = v->avActive;
        {
            std::lock_guard<std::mutex> g(v->state);
            fs = v->fs;
            cfg = v->cfg;
            lastError = v->lastError;
            lastSave = v->lastSave;
        }
        KV kv = cfg.ToKV();
        if (fs) {
            FsStats s = fs->Stats();
            kv[L"state"] = v->saving ? L"saving" : L"mounted";
            kv[L"capacity"] = U64(s.capacity);
            kv[L"used"] = U64(s.used);
            kv[L"memory"] = U64(s.memory);
            kv[L"files"] = U64(s.files);
            kv[L"dirs"] = U64(s.dirs);
            kv[L"open"] = U64(s.openHandles);
            kv[L"read"] = U64(s.bytesRead);
            kv[L"written"] = U64(s.bytesWritten);
            kv[L"livelabel"] = fs->Label();
            kv[L"avactive"] = avActive ? L"1" : L"0";
            kv[L"compraw"] = U64(s.compRaw);
            kv[L"compstored"] = U64(s.compStored);
        } else {
            kv[L"state"] = L"dismounted";
        }
        kv[L"lasterror"] = lastError;
        kv[L"lastsave"] = lastSave;   // Timestamp(), Anzeige mit DisplayTime
        m.sections.emplace_back(std::wstring(1, cfg.letter), kv);
    }
    return m;
}

// Liegt die Datei direkt im Image-Ordner des Dienstes (kein Unterordner, kein Datenstrom)?
static bool InImagesDir(const std::wstring& p)
{
    wchar_t full[MAX_PATH * 2];
    wchar_t* name = nullptr;
    DWORD n = GetFullPathNameW(p.c_str(), (DWORD)std::size(full), full, &name);
    if (!n || n >= std::size(full) || !name || !*name || wcspbrk(name, L":\\/")) return false;
    std::wstring dir(full, name - full);
    while (!dir.empty() && dir.back() == L'\\') dir.pop_back();
    return _wcsicmp(dir.c_str(), ImagesDir().c_str()) == 0;
}

// Im Dienstmodus liest und schreibt der Dienst Images und Quellordner als SYSTEM. Damit Benutzer ohne
// Adminrechte darueber keine fremden Dateien lesen oder ueberschreiben koennen, duerfen sie neue
// Image-Dateien nur im geschuetzten Image-Ordner anlegen; Quellordner und Defender-Ausnahme legen nur
// Administratoren fest. Bereits gesetzte Werte (old) duerfen bleiben.
bool Host::CheckPrivileged(const VolumeConfig& c, const VolumeConfig* old, bool admin, std::wstring& err)
{
    if (!serviceMode_ || admin) return true;
    const wchar_t* how = Tr(L"Oberfläche oder rdbctl als Administrator starten", L"run the user interface or rdbctl as administrator");
    if (!c.image.empty() && !(old && _wcsicmp(old->image.c_str(), c.image.c_str()) == 0) && !InImagesDir(c.image)) {
        err = TrF(L"Im Dienstmodus können Image-Dateien ohne Administratorrechte nur direkt im Ordner %s liegen (oder %s).",
                  L"In service mode, image files can only be placed directly in %s without administrator rights (or %s).",
                  ImagesDir().c_str(), how);
        return false;
    }
    if (!c.source.empty() && !(old && _wcsicmp(old->source.c_str(), c.source.c_str()) == 0)) {
        err = TrF(L"Einen Quellordner können im Dienstmodus nur Administratoren festlegen (%s).",
                  L"In service mode, only administrators can set a source folder (%s).", how);
        return false;
    }
    if (c.avExclude && !(old && old->avExclude)) {
        err = TrF(L"Die Defender-Ausnahme können nur Administratoren einschalten (%s).",
                  L"Only administrators can enable the Defender exclusion (%s).", how);
        return false;
    }
    return true;
}

Message Host::CmdCreate(const Message& req, bool clientAdmin)
{
    VolumeConfig c;
    std::wstring err;
    if (!VolumeConfig::FromKV(req.args, c, err)) return Message::Err(err);
    if (!ValidateConfig(c, 0, err)) return Message::Err(err);
    if (!CheckPrivileged(c, nullptr, clientAdmin, err)) return Message::Err(err);
    if (Get(c.letter)) return Message::Err(TrF(L"Für %c: gibt es bereits ein RAM-Laufwerk.", L"A RAM drive for %c: already exists.", c.letter));
    bool mount = ToBool(req.Get(L"mount", L"1"), true);
    if (mount && IsDriveLetterUsed(c.letter)) return Message::Err(TrF(L"Laufwerk %c: ist bereits belegt.", L"Drive %c: is already in use.", c.letter));
    auto v = std::make_shared<Volume>();
    v->cfg = c;
    if (mount) {
        std::lock_guard<std::mutex> g(v->op);
        if (!MountVolume(*v, err)) return Message::Err(err);
    }
    {
        std::lock_guard<std::mutex> g(mu_);
        vols_[c.letter] = v;
        WriteVolumeConfig(c);
    }
    Message m = Message::Ok();
    std::wstring warn = v->GetError();
    if (!warn.empty()) m.args[L"warning"] = warn;
    return m;
}

Message Host::CmdUpdate(const Message& req, bool clientAdmin)
{
    VolumeConfig c;
    std::wstring err;
    if (!VolumeConfig::FromKV(req.args, c, err)) return Message::Err(err);
    auto v = Get(c.letter);
    if (!v) return Message::Err(TrF(L"Kein RAM-Laufwerk %c:.", L"No RAM drive %c:.", c.letter));
    if (!ValidateConfig(c, c.letter, err)) return Message::Err(err);
    std::lock_guard<std::mutex> g(v->op);
    if (!CheckPrivileged(c, &v->cfg, clientAdmin, err)) return Message::Err(err);
    if (v->fs) {
        if (c.size != v->cfg.size && !v->fs->Resize(c.size, err)) return Message::Err(err);
        if (c.compress != v->cfg.compress) v->fs->SetCompression(c.compress);   // sofort wirksam
        if (c.avExclude != v->cfg.avExclude) {
            {
                std::lock_guard<std::mutex> gs(v->state);
                v->cfg.avExclude = c.avExclude;
            }
            SetExclusion(*v, c.avExclude);
        }
    }
    // Bezeichnung und Optionen werden beim nächsten Einhängen wirksam.
    {
        std::lock_guard<std::mutex> gs(v->state);
        v->cfg = c;
    }
    {
        std::lock_guard<std::mutex> g2(mu_);
        WriteVolumeConfig(c);
    }
    Log(L"Laufwerk %c: Einstellungen geändert.", L"Drive %c: settings changed.", c.letter);
    return Message::Ok();
}

Message Host::CmdDelete(const Message& req)
{
    std::wstring l = req.Get(L"letter");
    if (l.empty()) return Message::Err(Tr(L"Laufwerksbuchstabe fehlt.", L"Drive letter missing."));
    auto v = Get(l[0]);
    if (!v) return Message::Err(Tr(L"Kein solches RAM-Laufwerk.", L"No such RAM drive."));
    std::lock_guard<std::mutex> g(v->op);
    if (v->fs) {
        bool force = ToBool(req.Get(L"force", L"0"));
        std::wstring err;
        if (!DismountVolume(*v, force, ToBool(req.Get(L"discard", L"0")), err)) return Message::Err(err);
    }
    {
        std::lock_guard<std::mutex> g2(mu_);
        vols_.erase(v->cfg.letter);
        ini_->DeleteSection(SectionName(v->cfg.letter).c_str());
    }
    Log(L"Laufwerk %c: entfernt.", L"Drive %c: removed.", v->cfg.letter);
    return Message::Ok();
}

Message Host::CmdMount(const Message& req)
{
    std::wstring l = req.Get(L"letter");
    auto v = l.empty() ? nullptr : Get(l[0]);
    if (!v) return Message::Err(Tr(L"Kein solches RAM-Laufwerk.", L"No such RAM drive."));
    std::lock_guard<std::mutex> g(v->op);
    std::wstring err;
    if (!MountVolume(*v, err)) return Message::Err(err);
    Message m = Message::Ok();
    std::wstring warn = v->GetError();
    if (!warn.empty()) m.args[L"warning"] = warn;
    return m;
}

Message Host::CmdDismount(const Message& req)
{
    std::wstring l = req.Get(L"letter");
    auto v = l.empty() ? nullptr : Get(l[0]);
    if (!v) return Message::Err(Tr(L"Kein solches RAM-Laufwerk.", L"No such RAM drive."));
    std::lock_guard<std::mutex> g(v->op);
    std::wstring err;
    bool force = ToBool(req.Get(L"force", L"0"));
    if (!DismountVolume(*v, force, ToBool(req.Get(L"discard", L"0")), err)) {
        Message m = Message::Err(err);
        if (v->fs) m.args[L"open"] = U64(v->fs->OpenHandles());
        return m;
    }
    Message m = Message::Ok();
    if (!err.empty()) m.args[L"warning"] = err;
    return m;
}

Message Host::CmdResize(const Message& req)
{
    std::wstring l = req.Get(L"letter");
    auto v = l.empty() ? nullptr : Get(l[0]);
    if (!v) return Message::Err(Tr(L"Kein solches RAM-Laufwerk.", L"No such RAM drive."));
    unsigned long long size = ToU64(req.Get(L"size"));
    std::lock_guard<std::mutex> g(v->op);
    std::wstring err;
    if (v->fs && !v->fs->Resize(size, err)) return Message::Err(err);
    if (size < 16ull * 1024 * 1024) return Message::Err(Tr(L"Die Mindestgröße ist 16 MB.", L"The minimum size is 16 MB."));
    {
        std::lock_guard<std::mutex> gs(v->state);
        v->cfg.size = size;
    }
    {
        std::lock_guard<std::mutex> g2(mu_);
        WriteVolumeConfig(v->cfg);
    }
    Log(L"Laufwerk %c: Größe auf %s geändert.", L"Drive %c: size changed to %s.", v->cfg.letter, LogSize(size).c_str());
    return Message::Ok();
}

Message Host::CmdSave(const Message& req, bool clientAdmin)
{
    std::wstring l = req.Get(L"letter");
    auto v = l.empty() ? nullptr : Get(l[0]);
    if (!v) return Message::Err(Tr(L"Kein solches RAM-Laufwerk.", L"No such RAM drive."));
    std::wstring path = Trim(req.Get(L"path"));
    std::lock_guard<std::mutex> g(v->op);
    std::wstring err;
    if (!path.empty()) {
        VolumeConfig c = v->cfg;
        c.image = path;
        if (!ValidateConfig(c, c.letter, err)) return Message::Err(err);
        if (!CheckPrivileged(c, &v->cfg, clientAdmin, err)) return Message::Err(err);
    }
    if (!SaveVolume(*v, path.empty() ? v->cfg.image : path, err)) return Message::Err(err);
    if (!path.empty() && ToBool(req.Get(L"bind", L"1"), true)) {
        {
            std::lock_guard<std::mutex> gs(v->state);
            v->cfg.image = path;
        }
        std::lock_guard<std::mutex> g2(mu_);
        WriteVolumeConfig(v->cfg);
    }
    return Message::Ok();
}

Message Host::CmdSettings(const Message& req, bool clientAdmin)
{
    auto it = req.args.find(L"allowuser");
    if (it != req.args.end()) {
        if (serviceMode_ && !clientAdmin) return Message::Err(Tr(L"Diese Einstellung erfordert Administratorrechte.",
                                                                 L"This setting requires administrator rights."));
        allowUser_ = ToBool(it->second, true);
        ini_->Set(L"Service", L"AllowUser", allowUser_ ? L"1" : L"0");
        Log(L"Einstellung: Benutzer dürfen steuern = %s", L"Setting: users may control = %s",
            allowUser_ ? LT(L"ja", L"yes") : LT(L"nein", L"no"));
    }
    Message m = Message::Ok();
    m.args[L"allowuser"] = allowUser_ ? L"1" : L"0";
    return m;
}

// ----------------------------------------------------------------------- Pipe

std::wstring Host::PipeSddl()
{
    if (serviceMode_) {
        std::wstring s = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
        if (allowUser_) s += L"(A;;GRGW;;;IU)";
        return s;
    }
    // portabel: der startende Benutzer
    std::wstring s = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)";
    HANDLE tok;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        BYTE buf[256];
        DWORD len = 0;
        if (GetTokenInformation(tok, TokenUser, buf, sizeof buf, &len)) {
            LPWSTR sid = nullptr;
            if (ConvertSidToStringSidW(((TOKEN_USER*)buf)->User.Sid, &sid)) {
                s += L"(A;;GA;;;" + std::wstring(sid) + L")";
                LocalFree(sid);
            }
        }
        CloseHandle(tok);
    }
    return s;
}

static bool ClientIsAdmin(HANDLE pipe)
{
    if (!ImpersonateNamedPipeClient(pipe)) return false;
    bool admin = false;
    HANDLE tok;
    if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &tok)) {
        BYTE sid[SECURITY_MAX_SID_SIZE];
        DWORD sz = sizeof sid;
        BOOL member = FALSE;
        if (CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, sid, &sz) && CheckTokenMembership(tok, sid, &member) && member)
            admin = true;
        sz = sizeof sid;
        member = FALSE;
        if (!admin && CreateWellKnownSid(WinLocalSystemSid, nullptr, sid, &sz) && CheckTokenMembership(tok, sid, &member) && member)
            admin = true;
        CloseHandle(tok);
    }
    RevertToSelf();
    return admin;
}

void Host::ClientThread(HANDLE pipe)
{
    std::vector<wchar_t> buf(kPipeBuffer / sizeof(wchar_t));
    std::wstring text;
    for (;;) {
        DWORD r = 0;
        BOOL ok = ReadFile(pipe, buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)), &r, nullptr);
        text.append(buf.data(), r / sizeof(wchar_t));
        if (ok) break;
        if (GetLastError() != ERROR_MORE_DATA) { text.clear(); break; }
    }
    if (!text.empty()) {
        Message req = Message::Parse(text);
        // Antworten in der Sprache des Aufrufers; der Zeitgeber übernimmt die zuletzt verwendete.
        Lang l;
        if (ParseLang(req.Get(L"lang"), l)) {
            SetThreadLanguage(l);
            clientLang_ = (int)l;
        }
        Message resp;
        try {
            resp = Handle(req, ClientIsAdmin(pipe));
        } catch (const std::exception&) {
            resp = Message::Err(Tr(L"Interner Fehler im Dienst.", L"Internal error in the service."));
        }
        std::wstring out = resp.Serialize();
        if (out.size() * sizeof(wchar_t) > kPipeBuffer) out = Message::Err(Tr(L"Antwort zu groß.", L"Response too large.")).Serialize();
        DWORD w;
        WriteFile(pipe, out.data(), (DWORD)(out.size() * sizeof(wchar_t)), &w, nullptr);
        FlushFileBuffers(pipe);
    }
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    activeClients_--;
}

void Host::PipeLoop()
{
    HANDLE ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    while (WaitForSingleObject(stopEvent_, 0) != WAIT_OBJECT_0) {
        std::wstring sddl = PipeSddl();
        PSECURITY_DESCRIPTOR sd = nullptr;
        ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr);
        SECURITY_ATTRIBUTES sa{ sizeof sa, sd, FALSE };
        HANDLE pipe = CreateNamedPipeW(PipeName(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            PIPE_UNLIMITED_INSTANCES, kPipeBuffer, kPipeBuffer, 0, &sa);
        if (sd) LocalFree(sd);
        if (pipe == INVALID_HANDLE_VALUE) {
            Log(L"Pipe konnte nicht angelegt werden: %s", L"Could not create the pipe: %s", Win32Message(GetLastError()).c_str());
            if (WaitForSingleObject(stopEvent_, 1000) == WAIT_OBJECT_0) break;
            continue;
        }
        OVERLAPPED ov{};
        ResetEvent(ev);
        ov.hEvent = ev;
        bool connected = false;
        if (ConnectNamedPipe(pipe, &ov)) connected = true;
        else {
            DWORD e = GetLastError();
            if (e == ERROR_PIPE_CONNECTED) connected = true;
            else if (e == ERROR_IO_PENDING) {
                HANDLE hs[2] = { ev, stopEvent_ };
                DWORD w = WaitForMultipleObjects(2, hs, FALSE, INFINITE);
                if (w == WAIT_OBJECT_0) {
                    DWORD dummy;
                    connected = GetOverlappedResult(pipe, &ov, &dummy, FALSE) != FALSE;
                } else {
                    CancelIo(pipe);
                    CloseHandle(pipe);
                    break;
                }
            }
        }
        if (!connected) { CloseHandle(pipe); continue; }
        // Für synchrones Lesen/Schreiben im Client-Thread
        activeClients_++;
        std::thread([this, pipe] { ClientThread(pipe); }).detach();
    }
    CloseHandle(ev);
}

// ----------------------------------------------------------------------- Zeitgeber

void Host::TimerLoop()
{
    unsigned long long lastR = gc_.bytesRead.load(), lastW = gc_.bytesWritten.load();
    unsigned long long lastTick = GetTickCount64();
    unsigned tickCount = 0;
    while (WaitForSingleObject(stopEvent_, 1000) == WAIT_TIMEOUT) {
        // Hintergrundmeldungen (z. B. automatisches Speichern) in der Sprache des letzten Clients
        SetThreadLanguage((Lang)clientLang_.load());
        unsigned long long now = GetTickCount64();
        unsigned long long r = gc_.bytesRead.load(), w = gc_.bytesWritten.load();
        unsigned long long dt = now - lastTick;
        if (dt > 0) {
            std::lock_guard<std::mutex> g(statMu_);
            curRead_ = (r - lastR) * 1000 / dt;
            curWrite_ = (w - lastW) * 1000 / dt;
            if (curRead_ > peakRead_) peakRead_ = curRead_;
            if (curWrite_ > peakWrite_) peakWrite_ = curWrite_;
        }
        lastR = r; lastW = w; lastTick = now;

        // Automatisches Speichern
        std::vector<std::shared_ptr<Volume>> list;
        {
            std::lock_guard<std::mutex> g(mu_);
            for (auto& [l, v] : vols_) list.push_back(v);
        }
        for (auto& v : list) {
            // Erst die Sperre, dann lesen (fs/cfg werden nur unter 'op' geändert)
            std::unique_lock<std::mutex> g(v->op, std::try_to_lock);
            if (!g.owns_lock()) continue;
            if (!v->fs || !v->cfg.autoSaveMin || v->cfg.image.empty()) continue;
            if (now - v->lastSaveTick < (unsigned long long)v->cfg.autoSaveMin * 60000ull) continue;
            std::wstring err;
            if (!SaveVolume(*v, v->cfg.image, err)) {
                v->SetError(Tr(L"Automatisches Speichern fehlgeschlagen: ", L"Automatic save failed: ") + err);
                v->lastSaveTick = now;  // nicht jede Sekunde neu versuchen
            }
        }
        if (++tickCount % 60 == 0) PersistStats();
    }
}

} // namespace rdb
