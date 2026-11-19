#pragma once
// RAM-Dateisystem von RamDiskBox auf Basis der WinFsp-API (eigene Implementierung).
#include <winfsp/winfsp.h>
#include <string>
#include <string_view>
#include <vector>
#include <map>
#include <atomic>
#include <memory>
#include <cstdio>
#include <mutex>
#include <condition_variable>
#include <deque>
#include <thread>

namespace rdb {

// Sperr-Waechter fuer SRW-Sperren
struct Shared {
    SRWLOCK* l;
    explicit Shared(SRWLOCK* x) : l(x) { AcquireSRWLockShared(l); }
    ~Shared() { ReleaseSRWLockShared(l); }
    Shared(const Shared&) = delete;
    Shared& operator=(const Shared&) = delete;
};
struct Excl {
    SRWLOCK* l;
    explicit Excl(SRWLOCK* x) : l(x) { AcquireSRWLockExclusive(l); }
    ~Excl() { ReleaseSRWLockExclusive(l); }
    Excl(const Excl&) = delete;
    Excl& operator=(const Excl&) = delete;
};

struct VolumeOptions {
    wchar_t letter = L'R';
    unsigned long long capacity = 1ull << 30;
    std::wstring label = L"RAMDisk";
    bool reportNtfs = true;
    bool useCache = true;
    bool wipe = false;
    int compress = 0;                // 0 aus, 1 schnell (LZ4), 2 normal (LZ4 HC), 3 stark (zstd)
};

struct GlobalCounters {
    std::atomic<unsigned long long> bytesRead{ 0 }, bytesWritten{ 0 }, readOps{ 0 }, writeOps{ 0 };
};

struct FsStats {
    unsigned long long capacity = 0, used = 0, memory = 0, files = 0, dirs = 0, openHandles = 0;
    unsigned long long bytesRead = 0, bytesWritten = 0, readOps = 0, writeOps = 0;
    unsigned long long compRaw = 0, compStored = 0;   // komprimierte Bloecke: Rohgroesse / gespeicherte Groesse
    int compress = 0;
};

// Datenblock einer Datei: roh (csize == 0, Kapazitaet 64 KiB bzw. cap0 bei Block 0)
// oder komprimiert (p zeigt auf csize Bytes LZ4- bzw. zstd-Daten).
struct Chunk {
    UINT8* p = nullptr;
    UINT32 csize = 0;
    UINT8 skip = 0;                  // 1 = lohnt nicht, erst nach einer Aenderung erneut versuchen
    UINT8 algo = 0;                  // Verfahren der komprimierten Daten: 1 = LZ4, 2 = zstd
    UINT8 level = 0;                 // Stufe, fuer die der Block zuletzt bearbeitet wurde (1..3)
    UINT64 cid = 0;                  // eindeutige Nummer des komprimierten Inhalts (Lese-Zwischenspeicher)
};

struct NameLess {
    using is_transparent = void;
    bool operator()(std::wstring_view a, std::wstring_view b) const noexcept;
};

struct Node;
using NodeMap = std::map<std::wstring, Node*, NameLess>;

struct Node {
    std::wstring name;               // Namensteil (Datei/Ordner) bzw. Streamname ohne ':'
    Node* parent = nullptr;          // Ordner (Hauptknoten) bzw. Hauptdatei (Stream)
    bool isStream = false;
    bool linked = false;             // im Namensraum eingehaengt
    NodeMap* children = nullptr;     // nur Ordner
    NodeMap* streams = nullptr;      // benannte Streams eines Hauptknotens
    UINT32 attributes = 0;
    UINT32 reparseTag = 0;
    UINT64 creationTime = 0, lastAccessTime = 0, lastWriteTime = 0, changeTime = 0;
    UINT64 indexNumber = 0;
    UINT64 fileSize = 0;
    UINT64 allocSize = 0;
    std::vector<UINT8> sd;           // Sicherheitsbeschreibung (selbstrelativ)
    std::vector<UINT8> reparse;      // Reparse-Daten
    std::vector<Chunk> chunks;       // Datenbloecke zu je 64 KiB (p == nullptr = Nullen)
    UINT32 cap0 = 0;                 // Kapazitaet von Block 0 (kleine Dateien < 64 KiB)
    UINT64 gen = 0;                  // Aenderungszaehler der Daten (unter exklusiver Sperre)
    bool compDirty = false;          // seit der letzten Komprimierung geaendert
    std::atomic<bool> queued{ false };  // wartet auf den Komprimierer
    SRWLOCK lock = SRWLOCK_INIT;
    std::atomic<LONG> refs{ 0 };     // offene Kontexte + 1, solange eingehaengt
    Node* allPrev = nullptr;         // Liste aller Knoten (fuer das Aufraeumen)
    Node* allNext = nullptr;

    bool IsDir() const { return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0; }
    Node* Main() { return isStream ? parent : this; }
};

struct ParsedPath {
    std::vector<std::wstring_view> comps;
    std::wstring_view stream;
};

class RamFs {
public:
    static NTSTATUS Create(const VolumeOptions& opt, GlobalCounters* gc, std::unique_ptr<RamFs>& out);
    ~RamFs();

    NTSTATUS Mount();
    void Unmount();
    bool Mounted() const { return mounted_; }

    FsStats Stats();
    bool Resize(unsigned long long newCapacity, std::wstring& err);
    std::wstring Label();
    // Von Programmen offen gehaltene Handles (ohne Datei-Objekte, die nur der Windows-Cache haelt)
    unsigned long long OpenHandles() const { return userHandles_.load(); }
    // Windows-Cache des Laufwerks zurueckschreiben (vor dem Speichern eines Images)
    void FlushVolume();
    const VolumeOptions& Options() const { return opt_; }
    // Komprimierung im Arbeitsspeicher (0..3), auch im laufenden Betrieb
    void SetCompression(int level);

    // Ordner anlegen (relativer Pfad, z. B. "Temp" oder "Temp\Sub"), vor oder nach dem Einhaengen.
    bool EnsureFolder(const std::wstring& relPath);

    // Image (image.cpp). LoadImage: *notOpened = true, wenn die Datei gar nicht geoeffnet werden konnte
    // (gesperrt, keine Rechte) - dann ist sie nicht beschaedigt und darf weder beiseitegelegt noch
    // spaeter ueberschrieben werden.
    bool SaveImage(const std::wstring& path, bool compress, std::wstring& err);
    bool LoadImage(const std::wstring& path, std::wstring& err, bool* notOpened = nullptr);

    // Quellordner kopieren (import.cpp), vor dem Einhaengen. err enthaelt ggf. eine Warnung.
    struct ImportStats { unsigned long long files = 0, dirs = 0, bytes = 0, skipped = 0; };
    bool ImportFolder(const std::wstring& src, ImportStats& st, std::wstring& err);

private:
    RamFs() = default;

    // Interne Hilfen
    Node* NewNode();
    void Ref(Node* n) { n->refs.fetch_add(1, std::memory_order_relaxed); }
    void Unref(Node* n);
    void FreeNode(Node* n);
    void Link(Node* parent, Node* n);          // in Ordner bzw. Streamliste einhaengen
    void Unlink(Node* n);                      // aushaengen (mit Streams)
    void ReleaseData(Node* n);
    NTSTATUS Find(const ParsedPath& pp, Node** node, Node** parent);
    static void FillInfo(Node* n, FSP_FSCTL_FILE_INFO* fi);
    bool Charge(Node* n, UINT64 newAlloc);
    NTSTATUS SetSize(Node* n, UINT64 newSize, bool setAlloc);
    void Truncate(Node* n, UINT64 newSize);
    void FreeData(Node* n);
    NTSTATUS EnsureRange(Node* n, UINT64 off, UINT64 end);
    void ReadRange(Node* n, UINT64 off, UINT8* dst, size_t len);
    NTSTATUS WriteRange(Node* n, UINT64 off, const UINT8* src, size_t len);
    UINT8* AllocBlock(size_t cap);
    void FreeBlock(UINT8* p, size_t cap);
    static size_t RawCap(const Node* n, size_t i);
    void FreeChunk(Node* n, size_t i);
    NTSTATUS Promote(Node* n, size_t i);       // komprimierten Block zum Schreiben entpacken
    void EnqueueCompress(Node* n, bool immediate);
    void EnqueueAll();
    void CompressorLoop();
    void CompressNode(Node* n, void* fastState, void* hcState, void* zctx, std::vector<char>& out);
    static bool Unpack(const Chunk& c, UINT8* dst, size_t cap);
    void DropCompressed(Chunk& c, size_t cap);
    std::vector<UINT8> InheritedFolderSd(Node* parent) { return InheritedSd(parent, true); }
    std::vector<UINT8> InheritedSd(Node* parent, bool isDir);
    void TouchDir(Node* dir);

    // Der WinFsp-Treiber haelt Dateiinfos (bei Windows-Cache unbegrenzt) zwischen. Zeitstempel, die erst
    // im Cleanup gesetzt werden, kaemen sonst nie bei Programmen an: nach dem Cleanup meldet ein eigener
    // Thread die Datei als geaendert, das verwirft den Zwischenspeicher (FspFileSystemNotify).
    void QueueNotify(Node* n);
    void NotifierLoop();
    std::wstring NotifyPath(Node* n);          // unter tree_; ohne Namensnormalisierung in Grossbuchstaben

    // Image-Hilfen (image.cpp)
    struct ImageWriter;
    struct ImageReader;
    bool WriteNodeRec(ImageWriter& w, Node* n, const std::wstring& rel, std::wstring& err);
    bool SaveTree(ImageWriter& w, Node* dir, const std::wstring& rel, std::wstring& err);

    // WinFsp-Rueckrufe
    static RamFs* Self(FSP_FILE_SYSTEM* fs) { return (RamFs*)fs->UserContext; }
    static NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM*, FSP_FSCTL_VOLUME_INFO*);
    static NTSTATUS SetVolumeLabel(FSP_FILE_SYSTEM*, PWSTR, FSP_FSCTL_VOLUME_INFO*);
    static NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM*, PWSTR, PUINT32, PSECURITY_DESCRIPTOR, SIZE_T*);
    static NTSTATUS CreateCb(FSP_FILE_SYSTEM*, PWSTR, UINT32, UINT32, UINT32, PSECURITY_DESCRIPTOR, UINT64, PVOID*, FSP_FSCTL_FILE_INFO*);
    static NTSTATUS Open(FSP_FILE_SYSTEM*, PWSTR, UINT32, UINT32, PVOID*, FSP_FSCTL_FILE_INFO*);
    static NTSTATUS Overwrite(FSP_FILE_SYSTEM*, PVOID, UINT32, BOOLEAN, UINT64, FSP_FSCTL_FILE_INFO*);
    static VOID Cleanup(FSP_FILE_SYSTEM*, PVOID, PWSTR, ULONG);
    static VOID Close(FSP_FILE_SYSTEM*, PVOID);
    static NTSTATUS Read(FSP_FILE_SYSTEM*, PVOID, PVOID, UINT64, ULONG, PULONG);
    static NTSTATUS Write(FSP_FILE_SYSTEM*, PVOID, PVOID, UINT64, ULONG, BOOLEAN, BOOLEAN, PULONG, FSP_FSCTL_FILE_INFO*);
    static NTSTATUS Flush(FSP_FILE_SYSTEM*, PVOID, FSP_FSCTL_FILE_INFO*);
    static NTSTATUS GetFileInfo(FSP_FILE_SYSTEM*, PVOID, FSP_FSCTL_FILE_INFO*);
    static NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM*, PVOID, UINT32, UINT64, UINT64, UINT64, UINT64, FSP_FSCTL_FILE_INFO*);
    static NTSTATUS SetFileSize(FSP_FILE_SYSTEM*, PVOID, UINT64, BOOLEAN, FSP_FSCTL_FILE_INFO*);
    static NTSTATUS CanDelete(FSP_FILE_SYSTEM*, PVOID, PWSTR);
    static NTSTATUS Rename(FSP_FILE_SYSTEM*, PVOID, PWSTR, PWSTR, BOOLEAN);
    static NTSTATUS GetSecurity(FSP_FILE_SYSTEM*, PVOID, PSECURITY_DESCRIPTOR, SIZE_T*);
    static NTSTATUS SetSecurity(FSP_FILE_SYSTEM*, PVOID, SECURITY_INFORMATION, PSECURITY_DESCRIPTOR);
    static NTSTATUS ReadDirectory(FSP_FILE_SYSTEM*, PVOID, PWSTR, PWSTR, PVOID, ULONG, PULONG);
    static NTSTATUS GetDirInfoByName(FSP_FILE_SYSTEM*, PVOID, PWSTR, FSP_FSCTL_DIR_INFO*);
    static NTSTATUS ResolveReparsePoints(FSP_FILE_SYSTEM*, PWSTR, UINT32, BOOLEAN, PIO_STATUS_BLOCK, PVOID, PSIZE_T);
    static NTSTATUS GetReparsePoint(FSP_FILE_SYSTEM*, PVOID, PWSTR, PVOID, PSIZE_T);
    static NTSTATUS SetReparsePoint(FSP_FILE_SYSTEM*, PVOID, PWSTR, PVOID, SIZE_T);
    static NTSTATUS DeleteReparsePoint(FSP_FILE_SYSTEM*, PVOID, PWSTR, PVOID, SIZE_T);
    static NTSTATUS GetStreamInfo(FSP_FILE_SYSTEM*, PVOID, PVOID, ULONG, PULONG);
    static NTSTATUS GetReparsePointByName(FSP_FILE_SYSTEM*, PVOID, PWSTR, BOOLEAN, PVOID, PSIZE_T);
    static bool AddDirEntry(Node* n, const wchar_t* name, size_t nameLen, PVOID buf, ULONG len, PULONG bt);

    static FSP_FILE_SYSTEM_INTERFACE MakeInterface();
    static const FSP_FILE_SYSTEM_INTERFACE kInterface;

    VolumeOptions opt_;
    GlobalCounters* gc_ = nullptr;
    FSP_FILE_SYSTEM* fs_ = nullptr;
    Node* root_ = nullptr;
    HANDLE heap_ = nullptr;
    bool mounted_ = false;
    SRWLOCK tree_ = SRWLOCK_INIT;      // Namensraum (vor Knotensperren erwerben)
    SRWLOCK allLock_ = SRWLOCK_INIT;   // Liste aller Knoten
    Node* allHead_ = nullptr;
    SRWLOCK labelLock_ = SRWLOCK_INIT;
    std::wstring label_;
    std::vector<UINT8> rootSd_;
    std::atomic<unsigned long long> capacity_{ 0 };
    std::atomic<unsigned long long> used_{ 0 };     // Summe der Belegungsgroessen
    std::atomic<unsigned long long> memory_{ 0 };   // tatsaechlich belegter Speicher
    std::atomic<unsigned long long> files_{ 0 }, dirs_{ 0 };
    std::atomic<unsigned long long> openHandles_{ 0 };   // offene Kontexte (bis Close)
    std::atomic<unsigned long long> userHandles_{ 0 };   // offene Handles (bis Cleanup)
    std::atomic<unsigned long long> nextIndex_{ 1 };
    std::atomic<long> reparseCount_{ 0 };
    std::atomic<unsigned long long> bytesRead_{ 0 }, bytesWritten_{ 0 }, readOps_{ 0 }, writeOps_{ 0 };

    // Komprimierer (eigener Thread je Laufwerk)
    std::atomic<int> compLevel_{ 0 };
    std::atomic<unsigned long long> compRaw_{ 0 }, compStored_{ 0 };
    std::atomic<unsigned long long> compSerial_{ 0 };
    std::thread compThread_;
    std::mutex qMu_;
    std::condition_variable qCv_;
    std::deque<std::pair<Node*, unsigned long long>> queue_;   // Knoten, faellig ab (Tick)
    std::atomic<bool> qStop_{ false };

    // Aenderungsmeldungen an den Treiber (nur mit Windows-Cache, solange eingehaengt)
    std::thread notifyThread_;
    std::mutex nMu_;
    std::condition_variable nCv_;
    std::vector<std::wstring> nQ_;
    bool nStop_ = false;
};

bool ParsePath(PCWSTR path, ParsedPath& out);

} // namespace rdb
