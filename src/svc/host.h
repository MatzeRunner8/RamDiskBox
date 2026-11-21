#pragma once
#include <winfsp/winfsp.h>
#include "../common/common.h"
#include "ramfs.h"
#include <mutex>
#include <memory>
#include <map>
#include <atomic>
#include <thread>

namespace rdb {

// Protokoll-Datei: printf-Format je Sprache; es gilt die Sprache des Dienstes (ProcessLanguage),
// nicht die der Anfrage.
void Log(const wchar_t* de, const wchar_t* en, ...);
std::wstring NtMessage(NTSTATUS st);
// defender.cpp; permanent = ein neuer Versuch hilft nicht (keine Rechte, Defender nicht vorhanden)
bool DefenderExclusion(bool add, const std::wstring& path, std::wstring& err, bool* permanent = nullptr);
bool DefenderHasExclusion(const std::wstring& path, bool& has, bool& hidden, std::wstring& err, bool* permanent = nullptr);

// Sperren: 'op' serialisiert Einhaengen/Speichern/Aushaengen/Aendern. Wer cfg, fs, lastError
// oder lastSave SCHREIBT, haelt zusaetzlich 'state'; LIST liest nur unter 'state'.
struct Volume {
    VolumeConfig cfg;
    std::shared_ptr<RamFs> fs;
    std::mutex op;
    std::mutex state;
    unsigned long long lastSaveTick = 0;
    std::wstring lastError;        // letzte Warnung/Fehlermeldung
    std::wstring lastSave;         // Zeitpunkt der letzten Sicherung (Timestamp(); Clients zeigen ihn mit DisplayTime)
    std::atomic<bool> saving{ false };
    bool avActive = false;         // Defender-Ausnahme gesetzt (unter op)
    bool avOwned = false;          // ... und von RamDiskBox selbst eingetragen (nur dann wieder entfernen)
    unsigned long long avRetryTick = 0;  // naechster Versuch, falls das Setzen fehlschlug (0 = keiner)
    unsigned avRetries = 0;
    std::wstring avError;          // zuletzt gesetzte Defender-Meldung (wird bei Erfolg wieder geloescht)

    void SetError(const std::wstring& e) { std::lock_guard<std::mutex> g(state); lastError = e; }
    std::wstring GetError() { std::lock_guard<std::mutex> g(state); return lastError; }
};

class Host {
public:
    bool Start(bool serviceMode, std::wstring& err);
    void Stop();                   // sichert und haengt alles aus
    HANDLE StopEvent() const { return stopEvent_; }
    bool ServiceMode() const { return serviceMode_; }

private:
    Message Handle(const Message& req, bool clientAdmin);
    Message CmdList();
    Message CmdCreate(const Message& req, bool clientAdmin);
    Message CmdUpdate(const Message& req, bool clientAdmin);
    Message CmdDelete(const Message& req);
    Message CmdMount(const Message& req);
    Message CmdDismount(const Message& req);
    Message CmdResize(const Message& req);
    Message CmdSave(const Message& req, bool clientAdmin);
    bool CheckPrivileged(const VolumeConfig& c, const VolumeConfig* old, bool admin, std::wstring& err);
    Message CmdSettings(const Message& req, bool clientAdmin);

    bool MountVolume(Volume& v, std::wstring& err);
    bool DismountVolume(Volume& v, bool force, bool discard, std::wstring& err);
    bool SaveVolume(Volume& v, const std::wstring& path, std::wstring& err);
    void SetExclusion(Volume& v, bool on);
    void RetryStaleExclusions();
    std::shared_ptr<Volume> Get(wchar_t letter);
    bool ValidateConfig(const VolumeConfig& c, wchar_t ignoreLetter, std::wstring& err);
    void LoadConfig();
    void WriteVolumeConfig(const VolumeConfig& c);
    void PersistStats();

    void PipeLoop();
    void ClientThread(HANDLE pipe);
    void TimerLoop();
    std::wstring PipeSddl();

    bool serviceMode_ = false;
    std::mutex mu_;
    std::map<wchar_t, std::shared_ptr<Volume>> vols_;
    GlobalCounters gc_;
    std::unique_ptr<Ini> ini_;
    HANDLE stopEvent_ = nullptr;
    std::thread pipeThread_, timerThread_;
    std::atomic<bool> allowUser_{ true };
    std::atomic<bool> stopping_{ false };
    std::atomic<int> activeClients_{ 0 };
    std::atomic<int> clientLang_{ 0 };   // Sprache der letzten Anfrage (Lang), fuer Meldungen des Zeitgebers
    std::vector<wchar_t> staleExcl_;     // eigene Defender-Ausnahmen, deren Entfernen fehlschlug (unter mu_)
    unsigned long long staleRetryTick_ = 0;

    // Statistik
    std::mutex statMu_;
    unsigned long long lifeRead_ = 0, lifeWritten_ = 0;     // gespeicherte Basis
    unsigned long long offRead_ = 0, offWritten_ = 0;       // Zaehlerstand beim letzten Zuruecksetzen
    unsigned long long peakRead_ = 0, peakWrite_ = 0;       // Bytes/s
    unsigned long long curRead_ = 0, curWrite_ = 0;         // aktuelle Bytes/s
    unsigned long long startTick_ = 0;
};

} // namespace rdb
