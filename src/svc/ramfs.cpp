// RamDiskBox - RAM-Dateisystem (eigene Implementierung auf der WinFsp-API)
#include "ramfs.h"
#include "../common/common.h"
#include "../third_party/lz4/lz4.h"
#include "../third_party/lz4/lz4hc.h"
#include "../third_party/zstd/zstd.h"
#include <sddl.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <new>

namespace rdb {

namespace {

constexpr UINT64 kAllocUnit = 4096;          // Clustergroesse nach aussen
constexpr size_t kChunk = 64 * 1024;         // Datenblock
constexpr size_t kSmallGran = 4096;          // Wachstumsschritt kleiner Dateien
constexpr unsigned long long kCompressDelayMs = 1500;  // Komprimieren erst kurz nach dem Schliessen

// Wurzel: System/Administratoren Vollzugriff, angemeldete Benutzer Aendern, Benutzer Lesen,
// Ersteller-Besitzer Vollzugriff auf eigene Dateien.
constexpr wchar_t kRootSddl[] =
    L"O:BAG:BAD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1301bf;;;AU)(A;OICIIO;FA;;;CO)(A;OICI;0x1200a9;;;BU)";

inline UINT64 RoundUp(UINT64 v, UINT64 a) { return (v + a - 1) / a * a; }

UINT64 NowFt()
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

#ifndef FILE_SEQUENTIAL_ONLY
#define FILE_SEQUENTIAL_ONLY 0x00000004
#endif

// Kopierprogramme (Explorer, CopyFile, robocopy) oeffnen das Ziel mit FILE_SEQUENTIAL_ONLY zum
// Schreiben. Solche Zugriffe gehen am Windows-Cache vorbei: Sonst fuellt eine grosse Kopie den
// Cache bis zur Dirty-Page-Grenze, und Windows haelt den Kopiervorgang immer wieder sekundenlang
// an. Gilt nur fuer diesen einen Zugriff (WinFsp: FileObject ohne FO_CACHE_SUPPORTED); andere
// Handles derselben Datei bleiben im Cache, WinFsp schreibt ihn vor einem direkten Zugriff zurueck.
// RDB_DIRECTWRITE=0 schaltet das ab (fuer Vergleichsmessungen).
bool DirectWriteEnabled()
{
    static const bool on = [] {
        wchar_t v[4] = {};
        DWORD n = GetEnvironmentVariableW(L"RDB_DIRECTWRITE", v, 4);
        return !(n == 1 && v[0] == L'0');
    }();
    return on;
}

void BypassCacheForCopyTarget(bool useCache, bool isDir, UINT32 createOptions, UINT32 grantedAccess)
{
    if (!useCache || isDir || !(createOptions & FILE_SEQUENTIAL_ONLY)) return;
    if (!(grantedAccess & (FILE_WRITE_DATA | FILE_APPEND_DATA))) return;
    if (!DirectWriteEnabled()) return;
    if (FSP_FILE_SYSTEM_OPERATION_CONTEXT* oc = FspFileSystemGetOperationContext())
        oc->Response->Rsp.Create.Opened.DisableCache = 1;
}

} // namespace

bool NameLess::operator()(std::wstring_view a, std::wstring_view b) const noexcept
{
    return CompareStringOrdinal(a.data(), (int)a.size(), b.data(), (int)b.size(), TRUE) == CSTR_LESS_THAN;
}

bool ParsePath(PCWSTR path, ParsedPath& out)
{
    std::wstring_view p(path ? path : L"");
    size_t i = 0;
    while (i < p.size()) {
        while (i < p.size() && p[i] == L'\\') i++;
        if (i >= p.size()) break;
        size_t j = p.find(L'\\', i);
        if (j == std::wstring_view::npos) j = p.size();
        out.comps.push_back(p.substr(i, j - i));
        i = j;
    }
    if (!out.comps.empty()) {
        std::wstring_view& last = out.comps.back();
        size_t c = last.find(L':');
        if (c != std::wstring_view::npos) {
            std::wstring_view s = last.substr(c + 1);
            last = last.substr(0, c);
            size_t c2 = s.find(L':');
            if (c2 != std::wstring_view::npos) {
                std::wstring_view type = s.substr(c2 + 1);
                if (CompareStringOrdinal(type.data(), (int)type.size(), L"$DATA", 5, TRUE) != CSTR_EQUAL)
                    return false;
                s = s.substr(0, c2);
            }
            out.stream = s;
            if (last.empty()) return false;
        }
    }
    return true;
}

const FSP_FILE_SYSTEM_INTERFACE RamFs::kInterface = MakeInterface();

// ======================================================================= Lebenszyklus

NTSTATUS RamFs::Create(const VolumeOptions& opt, GlobalCounters* gc, std::unique_ptr<RamFs>& out)
{
    std::unique_ptr<RamFs> self(new (std::nothrow) RamFs());
    if (!self) return STATUS_INSUFFICIENT_RESOURCES;
    self->opt_ = opt;
    self->gc_ = gc;
    self->capacity_ = opt.capacity;
    self->label_ = opt.label.substr(0, 32);
    self->heap_ = HeapCreate(0, 0, 0);
    if (!self->heap_) return STATUS_INSUFFICIENT_RESOURCES;

    PSECURITY_DESCRIPTOR sd = nullptr;
    ULONG sdLen = 0;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(kRootSddl, SDDL_REVISION_1, &sd, &sdLen))
        return FspNtStatusFromWin32(GetLastError());
    self->rootSd_.assign((UINT8*)sd, (UINT8*)sd + sdLen);
    LocalFree(sd);

    Node* root = self->NewNode();
    root->attributes = FILE_ATTRIBUTE_DIRECTORY;
    root->children = new NodeMap;
    root->creationTime = root->lastAccessTime = root->lastWriteTime = root->changeTime = NowFt();
    root->indexNumber = self->nextIndex_++;
    root->sd = self->rootSd_;
    root->linked = true;
    root->refs = 1;
    self->root_ = root;

    FSP_FSCTL_VOLUME_PARAMS vp;
    memset(&vp, 0, sizeof vp);
    vp.Version = sizeof(FSP_FSCTL_VOLUME_PARAMS);
    vp.SectorSize = 512;
    vp.SectorsPerAllocationUnit = (UINT16)(kAllocUnit / 512);
    vp.MaxComponentLength = 255;
    vp.VolumeCreationTime = NowFt();
    vp.VolumeSerialNumber = (UINT32)(vp.VolumeCreationTime / 10000000ull) ^ (UINT32)GetTickCount64() ^ ((UINT32)opt.letter << 24);
    vp.FileInfoTimeout = opt.useCache ? INFINITE : 0;
    vp.VolumeInfoTimeoutValid = 1;
    vp.VolumeInfoTimeout = 1000;   // freier Platz soll im Explorer aktuell bleiben
    vp.CaseSensitiveSearch = 0;
    vp.CasePreservedNames = 1;
    vp.UnicodeOnDisk = 1;
    vp.PersistentAcls = 1;
    vp.ReparsePoints = 1;
    vp.ReparsePointsAccessCheck = 0;
    vp.NamedStreams = 1;
    // Jedes Cleanup melden lassen, damit offene Handles gezaehlt werden koennen.
    vp.PostCleanupWhenModifiedOnly = 0;
    vp.PostDispositionWhenNecessaryOnly = 1;
    vp.PassQueryDirectoryFileName = 1;
    // Windows-Cache beim Schliessen: Behalten macht erneutes Oeffnen und Lesen rund 30-mal schneller
    // (Treffer bleiben im Kernel). Das geht nur, wenn vor jedem Image-Speichern das ganze Laufwerk
    // zurueckgeschrieben werden kann (Volume-Flush, braucht Dienstmodus bzw. Administratorrechte).
    // Sonst beim Schliessen zurueckschreiben und verwerfen, damit Images immer vollstaendig sind.
    vp.FlushAndPurgeOnCleanup = (opt.useCache && opt.keepCache) ? 0 : 1;
    vp.AllowOpenInKernelMode = 1;
    wcscpy_s(vp.FileSystemName, sizeof vp.FileSystemName / sizeof(WCHAR), opt.reportNtfs ? L"NTFS" : kAppName);

    NTSTATUS st = FspFileSystemCreate((PWSTR)L"" FSP_FSCTL_DISK_DEVICE_NAME, &vp, &kInterface, &self->fs_);
    if (!NT_SUCCESS(st)) { self->fs_ = nullptr; return st; }
    self->fs_->UserContext = self.get();
    self->compLevel_ = std::clamp(opt.compress, 0, 3);
    RamFs* raw = self.get();
    self->compThread_ = std::thread([raw] { raw->CompressorLoop(); });
    out = std::move(self);
    return STATUS_SUCCESS;
}

RamFs::~RamFs()
{
    if (mounted_) Unmount();
    {
        std::lock_guard<std::mutex> g(qMu_);
        qStop_ = true;
    }
    qCv_.notify_all();
    if (compThread_.joinable()) compThread_.join();
    queue_.clear();   // Knoten werden unten ohnehin alle freigegeben
    if (fs_) FspFileSystemDelete(fs_);
    Node* n = allHead_;
    while (n) {
        Node* next = n->allNext;
        for (size_t i = 0; i < n->chunks.size(); i++) FreeChunk(n, i);
        delete n->children;
        delete n->streams;
        delete n;
        n = next;
    }
    allHead_ = nullptr;
    if (heap_) HeapDestroy(heap_);
}

NTSTATUS RamFs::Mount()
{
    wchar_t mp[3] = { opt_.letter, L':', 0 };
    NTSTATUS st = FspFileSystemSetMountPoint(fs_, mp);
    if (!NT_SUCCESS(st)) return st;
    st = FspFileSystemStartDispatcher(fs_, 0);
    if (!NT_SUCCESS(st)) {
        FspFileSystemRemoveMountPoint(fs_);
        return st;
    }
    mounted_ = true;
    if (opt_.useCache) {
        nStop_ = false;
        notifyThread_ = std::thread([this] { NotifierLoop(); });
    }
    return STATUS_SUCCESS;
}

void RamFs::Unmount()
{
    if (!mounted_) return;
    if (notifyThread_.joinable()) {
        {
            std::lock_guard<std::mutex> g(nMu_);
            nStop_ = true;
        }
        nCv_.notify_all();
        notifyThread_.join();
    }
    FspFileSystemStopDispatcher(fs_);
    FspFileSystemRemoveMountPoint(fs_);
    mounted_ = false;
}

bool RamFs::FlushVolume()
{
    if (!mounted_) return true;
    // Volume-Flush: der WinFsp-Treiber schreibt dabei die Caches aller Dateien zurueck, die er noch
    // kennt (offen oder vom Windows-Cache gehalten). Braucht Administrator- bzw. SYSTEM-Rechte; ohne
    // diese Rechte verwirft der Treiber den Cache beim Schliessen (keepCache = false), dann sind
    // geschlossene Dateien ohnehin schon zurueckgeschrieben.
    wchar_t path[] = L"\\\\.\\X:";
    path[4] = opt_.letter;
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = FlushFileBuffers(h);
    CloseHandle(h);
    return ok != FALSE;
}

FsStats RamFs::Stats()
{
    FsStats s;
    s.capacity = capacity_;
    s.used = used_;
    s.memory = memory_;
    s.files = files_;
    s.dirs = dirs_;
    s.openHandles = openHandles_;
    s.bytesRead = bytesRead_;
    s.bytesWritten = bytesWritten_;
    s.readOps = readOps_;
    s.writeOps = writeOps_;
    s.compRaw = compRaw_;
    s.compStored = compStored_;
    s.compress = compLevel_;
    return s;
}

bool RamFs::Resize(unsigned long long newCapacity, std::wstring& err)
{
    if (newCapacity < 16ull * 1024 * 1024) { err = Tr(L"Die Mindestgröße ist 16 MB.", L"The minimum size is 16 MB."); return false; }
    if (newCapacity < used_.load()) { err = Tr(L"Die neue Größe ist kleiner als der belegte Platz.", L"The new size is smaller than the space in use."); return false; }
    capacity_ = newCapacity;
    opt_.capacity = newCapacity;
    return true;
}

std::wstring RamFs::Label()
{
    Shared l(&labelLock_);
    return label_;
}

// ======================================================================= Knoten

Node* RamFs::NewNode()
{
    Node* n = new Node();
    Excl l(&allLock_);
    n->allNext = allHead_;
    if (allHead_) allHead_->allPrev = n;
    allHead_ = n;
    return n;
}

void RamFs::Unref(Node* n)
{
    if (n->refs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        FreeNode(n);
}

void RamFs::FreeNode(Node* n)
{
    FreeData(n);
    Charge(n, 0);
    {
        Excl l(&allLock_);
        if (n->allPrev) n->allPrev->allNext = n->allNext; else allHead_ = n->allNext;
        if (n->allNext) n->allNext->allPrev = n->allPrev;
    }
    if (n->isStream) {
        Node* main = n->parent;
        delete n;
        if (main) Unref(main);
    } else {
        delete n->children;
        delete n->streams;
        delete n;
    }
}

void RamFs::Link(Node* parent, Node* n)
{
    n->parent = parent;
    if (n->isStream) {
        if (!parent->streams) parent->streams = new NodeMap;
        parent->streams->emplace(n->name, n);
        Ref(parent);  // Stream haelt seine Hauptdatei
    } else {
        parent->children->emplace(n->name, n);
        if (n->IsDir()) dirs_++; else files_++;
        if (n->attributes & FILE_ATTRIBUTE_REPARSE_POINT) reparseCount_++;
    }
    n->linked = true;
    Ref(n);
}

// Gibt Daten und Belegung eines aus dem Namensraum entfernten Knotens sofort frei
// (wie NTFS beim Loeschen). Der Knoten selbst lebt weiter, bis der letzte Kontext
// geschlossen ist (der Windows-Cache kann ihn noch eine Weile halten).
void RamFs::ReleaseData(Node* n)
{
    Excl nl(&n->lock);
    FreeData(n);
    n->fileSize = 0;
    Charge(n, 0);
}

void RamFs::Unlink(Node* n)
{
    if (!n->linked) return;
    if (n->isStream) {
        if (n->parent->streams) n->parent->streams->erase(n->name);
        ReleaseData(n);
    } else {
        if (n->streams) {
            std::vector<Node*> list;
            for (auto& kv : *n->streams) list.push_back(kv.second);
            n->streams->clear();
            for (Node* s : list) { s->linked = false; ReleaseData(s); Unref(s); }
        }
        n->parent->children->erase(n->name);
        if (n->IsDir()) dirs_--; else { files_--; ReleaseData(n); }
        if (n->attributes & FILE_ATTRIBUTE_REPARSE_POINT) reparseCount_--;
    }
    n->linked = false;
    Unref(n);
}

NTSTATUS RamFs::Find(const ParsedPath& pp, Node** pnode, Node** pparent)
{
    Node* cur = root_;
    size_t cnt = pp.comps.size();
    *pnode = nullptr;
    if (pparent) *pparent = nullptr;
    if (cnt == 0) { *pnode = root_; return STATUS_SUCCESS; }
    for (size_t i = 0; i + 1 < cnt; i++) {
        if (!cur->IsDir()) return STATUS_OBJECT_PATH_NOT_FOUND;
        auto it = cur->children->find(pp.comps[i]);
        if (it == cur->children->end()) return STATUS_OBJECT_PATH_NOT_FOUND;
        cur = it->second;
    }
    if (!cur->IsDir()) return STATUS_OBJECT_PATH_NOT_FOUND;
    if (pparent) *pparent = cur;
    auto it = cur->children->find(pp.comps[cnt - 1]);
    if (it == cur->children->end()) return STATUS_OBJECT_NAME_NOT_FOUND;
    Node* n = it->second;
    if (!pp.stream.empty()) {
        if (pparent) *pparent = n;
        if (!n->streams) return STATUS_OBJECT_NAME_NOT_FOUND;
        auto si = n->streams->find(pp.stream);
        if (si == n->streams->end()) return STATUS_OBJECT_NAME_NOT_FOUND;
        n = si->second;
    }
    *pnode = n;
    return STATUS_SUCCESS;
}

void RamFs::FillInfo(Node* n, FSP_FSCTL_FILE_INFO* fi)
{
    Node* m = n->Main();
    fi->FileAttributes = m->attributes;
    fi->ReparseTag = m->reparseTag;
    fi->AllocationSize = n->allocSize;
    fi->FileSize = n->fileSize;
    fi->CreationTime = m->creationTime;
    fi->LastAccessTime = m->lastAccessTime;
    fi->LastWriteTime = m->lastWriteTime;
    fi->ChangeTime = m->changeTime;
    fi->IndexNumber = m->indexNumber;
    fi->HardLinks = 0;
    fi->EaSize = 0;
}

void RamFs::TouchDir(Node* dir)
{
    if (!dir) return;
    UINT64 now = NowFt();
    dir->lastWriteTime = now;
    dir->changeTime = now;
}

std::vector<UINT8> RamFs::InheritedSd(Node* parent, bool isDir)
{
    PSECURITY_DESCRIPTOR newSd = nullptr;
    GENERIC_MAPPING gm = { FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS };
    const std::vector<UINT8>& psd = parent->sd.empty() ? rootSd_ : parent->sd;
    if (CreatePrivateObjectSecurityEx((PSECURITY_DESCRIPTOR)psd.data(), nullptr, &newSd, nullptr, isDir ? TRUE : FALSE,
            SEF_DACL_AUTO_INHERIT | SEF_SACL_AUTO_INHERIT | SEF_DEFAULT_OWNER_FROM_PARENT |
            SEF_DEFAULT_GROUP_FROM_PARENT | SEF_AVOID_PRIVILEGE_CHECK | SEF_AVOID_OWNER_CHECK,
            nullptr, &gm)) {
        DWORD len = GetSecurityDescriptorLength(newSd);
        std::vector<UINT8> v((UINT8*)newSd, (UINT8*)newSd + len);
        DestroyPrivateObjectSecurity(&newSd);
        return v;
    }
    return psd;
}

bool RamFs::EnsureFolder(const std::wstring& relPath)
{
    try {
        std::wstring p = L"\\" + relPath;
        for (auto& c : p) if (c == L'/') c = L'\\';
        ParsedPath pp;
        if (!ParsePath(p.c_str(), pp) || !pp.stream.empty()) return false;
        Excl tl(&tree_);
        Node* cur = root_;
        for (auto comp : pp.comps) {
            if (comp.empty() || comp.size() > 255 || comp == L"." || comp == L"..") return false;
            auto it = cur->children->find(comp);
            if (it != cur->children->end()) {
                if (!it->second->IsDir()) return false;
                cur = it->second;
                continue;
            }
            Node* n = NewNode();
            n->name = comp;
            n->attributes = FILE_ATTRIBUTE_DIRECTORY;
            n->children = new NodeMap;
            n->creationTime = n->lastAccessTime = n->lastWriteTime = n->changeTime = NowFt();
            n->indexNumber = nextIndex_++;
            n->sd = InheritedFolderSd(cur);
            Link(cur, n);
            TouchDir(cur);
            cur = n;
        }
        return true;
    } catch (const std::bad_alloc&) {
        return false;
    }
}

// ======================================================================= Daten

UINT8* RamFs::AllocBlock(size_t cap)
{
    UINT8* p = cap == kChunk
        ? (UINT8*)VirtualAlloc(nullptr, kChunk, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)
        : (UINT8*)HeapAlloc(heap_, HEAP_ZERO_MEMORY, cap);
    if (p) memory_ += cap;
    return p;
}

void RamFs::FreeBlock(UINT8* p, size_t cap)
{
    if (opt_.wipe) SecureZeroMemory(p, cap);
    if (cap == kChunk) VirtualFree(p, 0, MEM_RELEASE);
    else HeapFree(heap_, 0, p);
    memory_ -= cap;
}

size_t RamFs::RawCap(const Node* n, size_t i)
{
    return i == 0 ? n->cap0 : kChunk;
}

void RamFs::FreeChunk(Node* n, size_t i)
{
    Chunk& c = n->chunks[i];
    if (!c.p) { c = Chunk(); return; }
    size_t cap = RawCap(n, i);
    if (c.csize) {
        DropCompressed(c, cap);
    } else {
        FreeBlock(c.p, cap);
    }
    c = Chunk();
}

// Komprimierten Block in einen Rohblock verwandeln (vor dem Schreiben). Exklusive Knotensperre.
NTSTATUS RamFs::Promote(Node* n, size_t i)
{
    Chunk& c = n->chunks[i];
    c.skip = 0;
    if (!c.p || !c.csize) return STATUS_SUCCESS;
    size_t cap = RawCap(n, i);
    UINT8* raw = AllocBlock(cap);
    if (!raw) return STATUS_INSUFFICIENT_RESOURCES;
    if (!Unpack(c, raw, cap)) memset(raw, 0, cap);   // darf nicht vorkommen
    DropCompressed(c, cap);
    c.p = raw;
    c.csize = 0;
    c.algo = 0;
    c.level = 0;
    c.cid = 0;
    return STATUS_SUCCESS;
}

void RamFs::FreeData(Node* n)
{
    n->gen++;
    for (size_t i = 0; i < n->chunks.size(); i++) FreeChunk(n, i);
    n->chunks.clear();
    n->chunks.shrink_to_fit();
    n->cap0 = 0;
}

bool RamFs::Charge(Node* n, UINT64 newAlloc)
{
    if (newAlloc > n->allocSize) {
        UINT64 delta = newAlloc - n->allocSize;
        UINT64 cur = used_.load();
        do {
            if (cur + delta > capacity_.load()) return false;
        } while (!used_.compare_exchange_weak(cur, cur + delta));
    } else {
        used_ -= n->allocSize - newAlloc;
    }
    n->allocSize = newAlloc;
    return true;
}

void RamFs::Truncate(Node* n, UINT64 newSize)
{
    n->gen++;
    n->compDirty = true;
    size_t keep = (size_t)((newSize + kChunk - 1) / kChunk);
    for (size_t i = keep; i < n->chunks.size(); i++) FreeChunk(n, i);
    if (n->chunks.size() > keep) n->chunks.resize(keep);
    if (keep == 0) { n->cap0 = 0; n->chunks.shrink_to_fit(); return; }
    size_t last = keep - 1;
    size_t in = (size_t)(newSize - (UINT64)last * kChunk);
    // Der Blockvektor kann kuerzer als die Dateigroesse sein (Daten noch im Windows-Cache).
    if (last < n->chunks.size() && n->chunks[last].p) {
        size_t cap = RawCap(n, last);
        if (in < cap) {
            // Hinter dem neuen Dateiende muessen Nullen stehen (spaeteres Verlaengern)
            if (n->chunks[last].csize && !NT_SUCCESS(Promote(n, last))) return;
            memset(n->chunks[last].p + in, 0, cap - in);
        }
    }
}

NTSTATUS RamFs::SetSize(Node* n, UINT64 newSize, bool setAlloc)
{
    if (setAlloc) {
        UINT64 a = RoundUp(newSize, kAllocUnit);
        if (!Charge(n, a)) return STATUS_DISK_FULL;
        if (n->fileSize > a) { Truncate(n, a); n->fileSize = a; }
    } else {
        if (newSize > n->allocSize && !Charge(n, RoundUp(newSize, kAllocUnit))) return STATUS_DISK_FULL;
        if (newSize < n->fileSize) Truncate(n, newSize);
        n->fileSize = newSize;
    }
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::EnsureRange(Node* n, UINT64 off, UINT64 end)
{
    n->gen++;
    n->compDirty = true;
    size_t need = (size_t)((end + kChunk - 1) / kChunk);
    if (n->chunks.size() < need) n->chunks.resize(need);
    size_t first = (size_t)(off / kChunk), last = (size_t)((end - 1) / kChunk);
    for (size_t i = first; i <= last; i++) {
        Chunk& c = n->chunks[i];
        if (c.csize) {
            NTSTATUS st = Promote(n, i);
            if (!NT_SUCCESS(st)) return st;
        }
        c.skip = 0;
        if (i == 0) {
            size_t inEnd = last == 0 ? (size_t)end : kChunk;
            size_t want = (size_t)RoundUp(inEnd, kSmallGran);
            if (want > kChunk || n->fileSize > kChunk) want = kChunk;
            if (!c.p) {
                UINT8* p = AllocBlock(want);
                if (!p) return STATUS_INSUFFICIENT_RESOURCES;
                c.p = p;
                n->cap0 = (UINT32)want;
            } else if (n->cap0 < want) {
                UINT8* p = AllocBlock(want);
                if (!p) return STATUS_INSUFFICIENT_RESOURCES;
                memcpy(p, c.p, n->cap0);
                FreeBlock(c.p, n->cap0);
                c.p = p;
                n->cap0 = (UINT32)want;
            }
        } else if (!c.p) {
            UINT8* p = AllocBlock(kChunk);
            if (!p) return STATUS_INSUFFICIENT_RESOURCES;
            c.p = p;
        }
    }
    return STATUS_SUCCESS;
}

void RamFs::ReadRange(Node* n, UINT64 off, UINT8* dst, size_t len)
{
    // Je Thread der zuletzt entpackte Block (kleine Lesevorgaenge im selben Block)
    thread_local UINT64 lastCid = 0;
    thread_local std::vector<UINT8> unpacked;
    while (len) {
        size_t i = (size_t)(off / kChunk), in = (size_t)(off % kChunk);
        size_t k = std::min(len, kChunk - in);
        const Chunk* c = i < n->chunks.size() ? &n->chunks[i] : nullptr;
        if (!c || !c->p) {
            memset(dst, 0, k);
        } else {
            size_t cap = RawCap(n, i);
            const UINT8* src = c->p;
            if (c->csize) {
                if (in == 0 && k == cap) {
                    // ganzer Block: direkt ins Ziel entpacken
                    if (!Unpack(*c, dst, cap)) memset(dst, 0, k);
                    dst += k; off += k; len -= k;
                    continue;
                }
                if (lastCid != c->cid || unpacked.size() < kChunk) {
                    if (unpacked.size() < kChunk) unpacked.resize(kChunk);
                    if (!Unpack(*c, unpacked.data(), cap)) {
                        memset(unpacked.data(), 0, cap);
                        lastCid = 0;
                    } else {
                        lastCid = c->cid;
                    }
                }
                src = unpacked.data();
            }
            if (in < cap) {
                size_t cc = std::min(k, cap - in);
                memcpy(dst, src + in, cc);
                if (cc < k) memset(dst + cc, 0, k - cc);
            } else {
                memset(dst, 0, k);
            }
        }
        dst += k; off += k; len -= k;
    }
}

NTSTATUS RamFs::WriteRange(Node* n, UINT64 off, const UINT8* src, size_t len)
{
    if (!len) return STATUS_SUCCESS;
    NTSTATUS st = EnsureRange(n, off, off + len);
    if (!NT_SUCCESS(st)) return st;
    while (len) {
        size_t i = (size_t)(off / kChunk), in = (size_t)(off % kChunk);
        size_t k = std::min(len, kChunk - in);
        memcpy(n->chunks[i].p + in, src, k);
        src += k; off += k; len -= k;
    }
    return STATUS_SUCCESS;
}

// ======================================================================= Komprimierung

namespace {
constexpr int kLz4HcLevel = 6;    // Stufe "normal"
constexpr int kZstdLevel = 6;     // Stufe "stark"
}

bool RamFs::Unpack(const Chunk& c, UINT8* dst, size_t cap)
{
    if (c.algo == 2) {
        struct Dctx {
            ZSTD_DCtx* p = nullptr;
            ~Dctx() { if (p) ZSTD_freeDCtx(p); }
        };
        thread_local Dctx d;
        if (!d.p) d.p = ZSTD_createDCtx();
        if (!d.p) return false;
        size_t r = ZSTD_decompressDCtx(d.p, dst, cap, c.p, c.csize);
        return !ZSTD_isError(r) && r == cap;
    }
    return LZ4_decompress_safe((const char*)c.p, (char*)dst, (int)c.csize, (int)cap) == (int)cap;
}

void RamFs::DropCompressed(Chunk& c, size_t cap)
{
    if (opt_.wipe) SecureZeroMemory(c.p, c.csize);
    HeapFree(heap_, 0, c.p);
    memory_ -= c.csize;
    compStored_ -= c.csize;
    compRaw_ -= cap;
}

void RamFs::SetCompression(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    int old = compLevel_.exchange(level);
    opt_.compress = level;
    // Vorhandene Bloecke im Hintergrund auf die neue Stufe bringen (bei "aus": entpacken)
    if (level != old) EnqueueAll();
}

void RamFs::EnqueueCompress(Node* n, bool immediate)
{
    if (n->IsDir() || qStop_.load()) return;
    if (n->queued.exchange(true)) return;
    Ref(n);
    {
        std::lock_guard<std::mutex> g(qMu_);
        // sofort faellige vorne, verzoegerte hinten (Temp-Dateien werden oft gleich wieder geloescht)
        if (immediate) queue_.emplace_front(n, 0ull);
        else queue_.emplace_back(n, GetTickCount64() + kCompressDelayMs);
    }
    qCv_.notify_one();
}

void RamFs::EnqueueAll()
{
    Shared tl(&tree_);
    std::vector<Node*> stack{ root_ };
    while (!stack.empty()) {
        Node* d = stack.back();
        stack.pop_back();
        for (auto& kv : *d->children) {
            Node* c = kv.second;
            if (c->IsDir()) stack.push_back(c);
            std::vector<Node*> targets;
            if (!c->IsDir()) targets.push_back(c);
            if (c->streams) for (auto& s : *c->streams) targets.push_back(s.second);
            for (Node* t : targets) {
                {
                    Excl nl(&t->lock);   // Stufe geaendert: "lohnt nicht" neu bewerten
                    for (auto& ch : t->chunks) ch.skip = 0;
                }
                EnqueueCompress(t, true);
            }
        }
    }
}

void RamFs::CompressNode(Node* n, void* fastState, void* hcState, void* zctx, std::vector<char>& out)
{
    thread_local std::vector<char> copy(kChunk);
    for (size_t i = 0;; i++) {
        int level = compLevel_.load();
        if (qStop_.load()) return;
        UINT8* src = nullptr;
        size_t cap = 0;
        UINT64 gen = 0;
        bool wasComp = false;
        {
            // Nur kurz sperren: Block kopieren bzw. entpacken; komprimiert wird ohne Sperre.
            Shared nl(&n->lock);
            if (!n->linked || i >= n->chunks.size()) return;
            const Chunk& c = n->chunks[i];
            if (!c.p) continue;
            cap = RawCap(n, i);
            if (c.csize) {
                if (c.level == level) continue;          // schon in der gewuenschten Stufe
                if (!Unpack(c, (UINT8*)copy.data(), cap)) continue;
                wasComp = true;
            } else {
                if (level == 0 || c.skip || cap < 1024) continue;
                memcpy(copy.data(), c.p, cap);
            }
            src = c.p;
            gen = n->gen;
        }
        int cs = 0;
        if (level == 1) {
            cs = LZ4_compress_fast_extState(fastState, copy.data(), out.data(), (int)cap, (int)out.size(), 1);
        } else if (level == 2) {
            cs = LZ4_compress_HC_extStateHC(hcState, copy.data(), out.data(), (int)cap, (int)out.size(), kLz4HcLevel);
        } else if (level == 3) {
            size_t r = ZSTD_compressCCtx((ZSTD_CCtx*)zctx, out.data(), out.size(), copy.data(), cap, kZstdLevel);
            cs = ZSTD_isError(r) ? 0 : (int)r;
        }
        Excl nl(&n->lock);
        if (i >= n->chunks.size()) return;
        Chunk& c = n->chunks[i];
        if (n->gen != gen || c.p != src || (c.csize != 0) != wasComp) continue;   // inzwischen geaendert
        if (level == 0) {
            // Komprimierung abgeschaltet: zurueck in einen Rohblock
            UINT8* raw = AllocBlock(cap);
            if (!raw) continue;
            memcpy(raw, copy.data(), cap);
            DropCompressed(c, cap);
            c.p = raw;
            c.csize = 0;
            c.algo = 0;
            c.level = 0;
            c.cid = 0;
            continue;
        }
        if (cs <= 0 || (size_t)cs > cap - cap / 8) {   // spart unter 12,5 %
            if (wasComp) c.level = (UINT8)level;       // bisherige Fassung behalten
            else c.skip = 1;
            continue;
        }
        UINT8* blk = (UINT8*)HeapAlloc(heap_, 0, (size_t)cs);
        if (!blk) continue;
        memcpy(blk, out.data(), (size_t)cs);
        if (wasComp) DropCompressed(c, cap);
        else FreeBlock(c.p, cap);
        c.p = blk;
        c.csize = (UINT32)cs;
        c.algo = level == 3 ? 2 : 1;
        c.level = (UINT8)level;
        c.cid = ++compSerial_;
        memory_ += (size_t)cs;
        compRaw_ += cap;
        compStored_ += (size_t)cs;
    }
}

void RamFs::CompressorLoop()
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    void* fast = malloc((size_t)LZ4_sizeofState());
    void* hc = malloc((size_t)LZ4_sizeofStateHC());
    ZSTD_CCtx* zctx = ZSTD_createCCtx();
    std::vector<char> out(std::max((size_t)LZ4_compressBound((int)kChunk), ZSTD_compressBound(kChunk)));
    for (;;) {
        Node* n = nullptr;
        {
            std::unique_lock<std::mutex> l(qMu_);
            while (!n) {
                if (qStop_.load()) break;
                if (queue_.empty()) { qCv_.wait(l); continue; }
                unsigned long long now = GetTickCount64(), due = queue_.front().second;
                if (due <= now) {
                    n = queue_.front().first;
                    queue_.pop_front();
                } else {
                    qCv_.wait_for(l, std::chrono::milliseconds(due - now));
                }
            }
        }
        if (!n) break;
        n->queued = false;   // neue Aenderungen duerfen wieder einreihen
        if (fast && hc && zctx) CompressNode(n, fast, hc, zctx, out);
        Unref(n);
    }
    free(fast);
    free(hc);
    if (zctx) ZSTD_freeCCtx(zctx);
}
// ======================================================================= WinFsp-Rueckrufe

NTSTATUS RamFs::GetVolumeInfo(FSP_FILE_SYSTEM* fs, FSP_FSCTL_VOLUME_INFO* vi)
{
    RamFs* s = Self(fs);
    UINT64 cap = s->capacity_, used = s->used_;
    vi->TotalSize = cap;
    vi->FreeSize = used < cap ? cap - used : 0;
    Shared l(&s->labelLock_);
    size_t len = std::min<size_t>(s->label_.size(), 32);
    vi->VolumeLabelLength = (UINT16)(len * sizeof(WCHAR));
    memcpy(vi->VolumeLabel, s->label_.data(), len * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::SetVolumeLabel(FSP_FILE_SYSTEM* fs, PWSTR label, FSP_FSCTL_VOLUME_INFO* vi)
{
    RamFs* s = Self(fs);
    {
        Excl l(&s->labelLock_);
        s->label_ = std::wstring(label ? label : L"").substr(0, 32);
    }
    return GetVolumeInfo(fs, vi);
}

NTSTATUS RamFs::GetSecurityByName(FSP_FILE_SYSTEM* fs, PWSTR name, PUINT32 pattr,
    PSECURITY_DESCRIPTOR sd, SIZE_T* psdSize)
{
    RamFs* s = Self(fs);
    try {
        ParsedPath pp;
        if (!ParsePath(name, pp)) return STATUS_OBJECT_NAME_INVALID;
        Shared tl(&s->tree_);
        if (s->reparseCount_.load() > 0 &&
            FspFileSystemFindReparsePoint(fs, GetReparsePointByName, s, name, pattr))
            return STATUS_REPARSE;
        Node* n;
        NTSTATUS st = s->Find(pp, &n, nullptr);
        if (!NT_SUCCESS(st)) return st;
        Node* m = n->Main();
        Shared nl(&m->lock);
        if (pattr) *pattr = m->attributes;
        if (psdSize) {
            if (m->sd.size() > *psdSize) {
                *psdSize = m->sd.size();
                return STATUS_BUFFER_OVERFLOW;
            }
            *psdSize = m->sd.size();
            if (sd && !m->sd.empty()) memcpy(sd, m->sd.data(), m->sd.size());
        }
        return STATUS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS RamFs::CreateCb(FSP_FILE_SYSTEM* fs, PWSTR name, UINT32 createOptions, UINT32 grantedAccess,
    UINT32 attrs, PSECURITY_DESCRIPTOR sd, UINT64 allocSize, PVOID* pctx, FSP_FSCTL_FILE_INFO* fi)
{
    RamFs* s = Self(fs);
    try {
        ParsedPath pp;
        if (!ParsePath(name, pp) || pp.comps.empty()) return STATUS_OBJECT_NAME_INVALID;
        bool isDir = (createOptions & FILE_DIRECTORY_FILE) != 0;
        Excl tl(&s->tree_);
        Node* existing = nullptr;
        Node* parent = nullptr;
        NTSTATUS st = s->Find(pp, &existing, &parent);
        if (NT_SUCCESS(st)) return STATUS_OBJECT_NAME_COLLISION;
        if (st != STATUS_OBJECT_NAME_NOT_FOUND || !parent) return st;

        Node* n;
        UINT64 now = NowFt();
        if (!pp.stream.empty()) {
            if (isDir) return STATUS_NOT_A_DIRECTORY;
            if (pp.stream.size() > 255) return STATUS_OBJECT_NAME_INVALID;
            n = s->NewNode();
            n->isStream = true;
            n->name = pp.stream;
            Node* m = parent;
            m->lastWriteTime = m->changeTime = now;
        } else {
            if (pp.comps.back().size() > 255) return STATUS_OBJECT_NAME_INVALID;
            n = s->NewNode();
            n->name = pp.comps.back();
            if (isDir) {
                n->attributes = (attrs | FILE_ATTRIBUTE_DIRECTORY) & ~FILE_ATTRIBUTE_NORMAL;
                n->children = new NodeMap;
            } else {
                n->attributes = (attrs & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL)) | FILE_ATTRIBUTE_ARCHIVE;
            }
            if (sd) {
                DWORD len = GetSecurityDescriptorLength(sd);
                n->sd.assign((UINT8*)sd, (UINT8*)sd + len);
            } else {
                n->sd = isDir ? s->InheritedFolderSd(parent) : parent->sd;
            }
            n->creationTime = n->lastAccessTime = n->lastWriteTime = n->changeTime = now;
            n->indexNumber = s->nextIndex_++;
        }
        s->Link(parent, n);
        if (!isDir && allocSize && !s->Charge(n, RoundUp(allocSize, kAllocUnit))) {
            s->Unlink(n);
            return STATUS_DISK_FULL;
        }
        if (!n->isStream) s->TouchDir(parent);
        s->Ref(n);
        s->openHandles_++;
        s->userHandles_++;
        *pctx = n;
        FillInfo(n, fi);
        BypassCacheForCopyTarget(s->opt_.useCache, isDir, createOptions, grantedAccess);
        return STATUS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS RamFs::Open(FSP_FILE_SYSTEM* fs, PWSTR name, UINT32 createOptions, UINT32 grantedAccess,
    PVOID* pctx, FSP_FSCTL_FILE_INFO* fi)
{
    RamFs* s = Self(fs);
    try {
        ParsedPath pp;
        if (!ParsePath(name, pp)) return STATUS_OBJECT_NAME_INVALID;
        Shared tl(&s->tree_);
        Node* n;
        NTSTATUS st = s->Find(pp, &n, nullptr);
        if (!NT_SUCCESS(st)) return st;
        s->Ref(n);
        s->openHandles_++;
        s->userHandles_++;
        *pctx = n;
        Shared nl(&n->lock);
        FillInfo(n, fi);
        BypassCacheForCopyTarget(s->opt_.useCache, n->IsDir(), createOptions, grantedAccess);
        return STATUS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS RamFs::Overwrite(FSP_FILE_SYSTEM* fs, PVOID ctx, UINT32 attrs, BOOLEAN replace,
    UINT64 allocSize, FSP_FSCTL_FILE_INFO* fi)
{
    RamFs* s = Self(fs);
    Node* n = (Node*)ctx;
    Node* m = n->Main();
    Excl tl(&s->tree_);
    // Ueberschreiben der Hauptdatei entfernt benannte Streams ohne offene Handles (wie NTFS).
    if (!n->isStream && n->streams) {
        std::vector<Node*> drop;
        for (auto& kv : *n->streams)
            if (kv.second->refs.load() == 1) drop.push_back(kv.second);
        for (Node* x : drop) s->Unlink(x);
    }
    NTSTATUS st;
    {
        Excl nl(&n->lock);
        s->Truncate(n, 0);
        n->fileSize = 0;
        st = s->Charge(n, RoundUp(allocSize, kAllocUnit)) ? STATUS_SUCCESS : STATUS_DISK_FULL;
    }
    {
        Excl ml(&m->lock);
        if (!n->isStream) {
            UINT32 keep = m->attributes & FILE_ATTRIBUTE_REPARSE_POINT;
            UINT32 a = (attrs & ~(FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_DIRECTORY)) | FILE_ATTRIBUTE_ARCHIVE;
            m->attributes = replace ? (a | keep) : (m->attributes | a);
        }
        UINT64 now = NowFt();
        m->lastAccessTime = m->lastWriteTime = m->changeTime = now;
    }
    FillInfo(n, fi);
    return st;
}

VOID RamFs::Cleanup(FSP_FILE_SYSTEM* fs, PVOID ctx, PWSTR name, ULONG flags)
{
    (void)name;
    RamFs* s = Self(fs);
    Node* n = (Node*)ctx;
    Node* m = n->Main();
    if (s->userHandles_.load() > 0) s->userHandles_--;
    // Aenderung (nicht nur Lesezugriff): danach den Zwischenspeicher des Treibers verwerfen lassen
    const bool modified = (flags & (FspCleanupSetArchiveBit | FspCleanupSetLastWriteTime | FspCleanupSetChangeTime)) != 0;
    if (flags & (FspCleanupSetArchiveBit | FspCleanupSetLastAccessTime | FspCleanupSetLastWriteTime | FspCleanupSetChangeTime)) {
        Excl ml(&m->lock);
        if ((flags & FspCleanupSetArchiveBit) && !m->IsDir()) m->attributes |= FILE_ATTRIBUTE_ARCHIVE;
        UINT64 now = NowFt();
        if (flags & FspCleanupSetLastAccessTime) m->lastAccessTime = now;
        if (flags & FspCleanupSetLastWriteTime) m->lastWriteTime = now;
        if (flags & FspCleanupSetChangeTime) m->changeTime = now;
    }
    if (flags & FspCleanupSetAllocationSize) {
        Excl nl(&n->lock);
        s->SetSize(n, RoundUp(n->fileSize, kAllocUnit), true);
    }
    if (flags & FspCleanupDelete) {
        Excl tl(&s->tree_);
        if (n->linked && !(n->IsDir() && n->children && !n->children->empty())) {
            bool stream = n->isStream;
            Node* parent = n->parent;
            s->Unlink(n);
            if (!stream) s->TouchDir(parent);
            return;
        }
    }
    if (modified && s->opt_.useCache) s->QueueNotify(n);
    // Geaenderte Datei nach kurzer Pause im Hintergrund komprimieren
    if (n->compDirty && s->compLevel_.load() > 0 && n->linked) {
        n->compDirty = false;
        s->EnqueueCompress(n, false);
    }
}

std::wstring RamFs::NotifyPath(Node* n)
{
    std::vector<const std::wstring*> parts;
    for (Node* p = n->Main(); p && p != root_; p = p->parent) parts.push_back(&p->name);
    std::wstring path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        path += L'\\';
        path += **it;
    }
    if (path.empty()) path = L"\\";
    // Ohne Namensnormalisierung erwartet WinFsp die Grossschreibung des Namens
    std::wstring up(path.size(), L'\0');
    int len = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_UPPERCASE, path.data(), (int)path.size(), up.data(), (int)up.size(),
        nullptr, nullptr, 0);
    return len == (int)path.size() ? up : path;
}

void RamFs::QueueNotify(Node* n)
{
    std::wstring p;
    {
        Shared tl(&tree_);
        if (!n->Main()->linked) return;
        p = NotifyPath(n);
    }
    {
        std::lock_guard<std::mutex> g(nMu_);
        nQ_.push_back(std::move(p));
    }
    nCv_.notify_one();
}

void RamFs::NotifierLoop()
{
    std::vector<UINT8> buf(64 * 1024);
    std::vector<UINT8> one(sizeof(FSP_FSCTL_NOTIFY_INFO) + 65536);
    for (;;) {
        std::vector<std::wstring> batch;
        {
            std::unique_lock<std::mutex> g(nMu_);
            nCv_.wait(g, [&] { return nStop_ || !nQ_.empty(); });
            if (nStop_) return;
            batch.swap(nQ_);
        }
        std::sort(batch.begin(), batch.end());
        batch.erase(std::unique(batch.begin(), batch.end()), batch.end());
        for (int tries = 0; tries < 50; tries++) {
            NTSTATUS st = FspFileSystemNotifyBegin(fs_, 500);
            if (st == STATUS_CANT_WAIT) { Sleep(10); continue; }   // gerade laeuft eine Umbenennung
            if (!NT_SUCCESS(st)) break;
            ULONG used = 0;
            for (auto& p : batch) {
                size_t bytes = p.size() * sizeof(WCHAR);
                if (bytes > 65000) continue;
                auto* ni = (FSP_FSCTL_NOTIFY_INFO*)one.data();
                ni->Size = (UINT16)(sizeof(FSP_FSCTL_NOTIFY_INFO) + bytes);
                ni->Filter = FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_ATTRIBUTES;
                ni->Action = FILE_ACTION_MODIFIED;
                memcpy(ni->FileNameBuf, p.data(), bytes);
                if (!FspFileSystemAddNotifyInfo(ni, buf.data(), (ULONG)buf.size(), &used)) {
                    FspFileSystemNotify(fs_, (FSP_FSCTL_NOTIFY_INFO*)buf.data(), used);
                    used = 0;
                    FspFileSystemAddNotifyInfo(ni, buf.data(), (ULONG)buf.size(), &used);
                }
            }
            if (used) FspFileSystemNotify(fs_, (FSP_FSCTL_NOTIFY_INFO*)buf.data(), used);
            FspFileSystemNotifyEnd(fs_);
            break;
        }
    }
}

VOID RamFs::Close(FSP_FILE_SYSTEM* fs, PVOID ctx)
{
    RamFs* s = Self(fs);
    s->openHandles_--;
    s->Unref((Node*)ctx);
}

NTSTATUS RamFs::Read(FSP_FILE_SYSTEM* fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len, PULONG pbt)
{
    RamFs* s = Self(fs);
    Node* n = (Node*)ctx;
    Shared nl(&n->lock);
    if (off >= n->fileSize) { *pbt = 0; return STATUS_END_OF_FILE; }
    UINT64 end = std::min<UINT64>(off + len, n->fileSize);
    size_t cnt = (size_t)(end - off);
    s->ReadRange(n, off, (UINT8*)buf, cnt);
    *pbt = (ULONG)cnt;
    s->bytesRead_ += cnt; s->readOps_++;
    if (s->gc_) { s->gc_->bytesRead += cnt; s->gc_->readOps++; }
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::Write(FSP_FILE_SYSTEM* fs, PVOID ctx, PVOID buf, UINT64 off, ULONG len,
    BOOLEAN writeToEnd, BOOLEAN constrained, PULONG pbt, FSP_FSCTL_FILE_INFO* fi)
{
    RamFs* s = Self(fs);
    Node* n = (Node*)ctx;
    try {
        Excl nl(&n->lock);
        if (constrained) {
            if (off >= n->fileSize) { *pbt = 0; FillInfo(n, fi); return STATUS_SUCCESS; }
            if (off + len > n->fileSize) len = (ULONG)(n->fileSize - off);
        } else {
            if (writeToEnd) off = n->fileSize;
            UINT64 end = off + len;
            if (end > n->fileSize) {
                NTSTATUS st = s->SetSize(n, end, false);
                if (!NT_SUCCESS(st)) return st;
            }
        }
        NTSTATUS st = s->WriteRange(n, off, (const UINT8*)buf, len);
        if (!NT_SUCCESS(st)) return st;
        *pbt = len;
        s->bytesWritten_ += len; s->writeOps_++;
        if (s->gc_) { s->gc_->bytesWritten += len; s->gc_->writeOps++; }
        FillInfo(n, fi);
        return STATUS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS RamFs::Flush(FSP_FILE_SYSTEM* fs, PVOID ctx, FSP_FSCTL_FILE_INFO* fi)
{
    (void)fs;
    if (ctx) {
        Node* n = (Node*)ctx;
        Shared nl(&n->lock);
        FillInfo(n, fi);
    }
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::GetFileInfo(FSP_FILE_SYSTEM* fs, PVOID ctx, FSP_FSCTL_FILE_INFO* fi)
{
    (void)fs;
    Node* n = (Node*)ctx;
    Shared nl(&n->lock);
    FillInfo(n, fi);
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::SetBasicInfo(FSP_FILE_SYSTEM* fs, PVOID ctx, UINT32 attrs, UINT64 ct, UINT64 at,
    UINT64 wt, UINT64 cht, FSP_FSCTL_FILE_INFO* fi)
{
    (void)fs;
    Node* n = (Node*)ctx;
    Node* m = n->Main();
    {
        Excl ml(&m->lock);
        if (attrs != INVALID_FILE_ATTRIBUTES) {
            UINT32 keep = m->attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT);
            m->attributes = (attrs & ~(FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NORMAL | FILE_ATTRIBUTE_REPARSE_POINT)) | keep;
        }
        if (ct) m->creationTime = ct;
        if (at) m->lastAccessTime = at;
        if (wt) m->lastWriteTime = wt;
        if (cht) m->changeTime = cht;
    }
    FillInfo(n, fi);
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::SetFileSize(FSP_FILE_SYSTEM* fs, PVOID ctx, UINT64 newSize, BOOLEAN setAlloc,
    FSP_FSCTL_FILE_INFO* fi)
{
    RamFs* s = Self(fs);
    Node* n = (Node*)ctx;
    Excl nl(&n->lock);
    NTSTATUS st = s->SetSize(n, newSize, setAlloc != FALSE);
    FillInfo(n, fi);
    return st;
}

NTSTATUS RamFs::CanDelete(FSP_FILE_SYSTEM* fs, PVOID ctx, PWSTR name)
{
    (void)name;
    RamFs* s = Self(fs);
    Node* n = (Node*)ctx;
    Shared tl(&s->tree_);
    if (n->IsDir() && n->children && !n->children->empty()) return STATUS_DIRECTORY_NOT_EMPTY;
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::Rename(FSP_FILE_SYSTEM* fs, PVOID ctx, PWSTR name, PWSTR newName, BOOLEAN replace)
{
    (void)name;
    RamFs* s = Self(fs);
    Node* n = (Node*)ctx;
    try {
        if (n->isStream) return STATUS_INVALID_PARAMETER;
        ParsedPath np;
        if (!ParsePath(newName, np) || np.comps.empty() || !np.stream.empty()) return STATUS_OBJECT_NAME_INVALID;
        if (np.comps.back().size() > 255) return STATUS_OBJECT_NAME_INVALID;
        Excl tl(&s->tree_);
        if (!n->linked) return STATUS_OBJECT_NAME_NOT_FOUND;
        Node* cur = s->root_;
        for (size_t i = 0; i + 1 < np.comps.size(); i++) {
            auto it = cur->children->find(np.comps[i]);
            if (it == cur->children->end() || !it->second->IsDir()) return STATUS_OBJECT_PATH_NOT_FOUND;
            cur = it->second;
        }
        Node* newParent = cur;
        for (Node* p = newParent; p; p = p->parent)
            if (p == n) return STATUS_INVALID_PARAMETER;
        std::wstring leaf(np.comps.back());
        auto it = newParent->children->find(std::wstring_view(leaf));
        if (it != newParent->children->end() && it->second != n) {
            if (!replace) return STATUS_OBJECT_NAME_COLLISION;
            Node* t = it->second;
            if (t->IsDir()) return STATUS_ACCESS_DENIED;
            s->Unlink(t);   // offene Handles prueft bereits der WinFsp-Treiber
        }
        Node* oldParent = n->parent;
        auto self = oldParent->children->find(std::wstring_view(n->name));
        if (self != oldParent->children->end() && self->second == n) oldParent->children->erase(self);
        n->name = leaf;
        n->parent = newParent;
        newParent->children->emplace(n->name, n);
        s->TouchDir(oldParent);
        if (newParent != oldParent) s->TouchDir(newParent);
        return STATUS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS RamFs::GetSecurity(FSP_FILE_SYSTEM* fs, PVOID ctx, PSECURITY_DESCRIPTOR sd, SIZE_T* psdSize)
{
    RamFs* s = Self(fs);
    Node* m = ((Node*)ctx)->Main();
    Shared ml(&m->lock);
    const std::vector<UINT8>& v = m->sd.empty() ? s->rootSd_ : m->sd;
    if (v.size() > *psdSize) {
        *psdSize = v.size();
        return STATUS_BUFFER_OVERFLOW;
    }
    *psdSize = v.size();
    if (sd) memcpy(sd, v.data(), v.size());
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::SetSecurity(FSP_FILE_SYSTEM* fs, PVOID ctx, SECURITY_INFORMATION si, PSECURITY_DESCRIPTOR mod)
{
    RamFs* s = Self(fs);
    Node* m = ((Node*)ctx)->Main();
    try {
        Excl ml(&m->lock);
        if (m->sd.empty()) m->sd = s->rootSd_;
        PSECURITY_DESCRIPTOR newSd = nullptr;
        NTSTATUS st = FspSetSecurityDescriptor(m->sd.data(), si, mod, &newSd);
        if (!NT_SUCCESS(st)) return st;
        DWORD len = GetSecurityDescriptorLength(newSd);
        m->sd.assign((UINT8*)newSd, (UINT8*)newSd + len);
        FspDeleteSecurityDescriptor(newSd, (NTSTATUS(*)())FspSetSecurityDescriptor);
        m->changeTime = NowFt();
        return STATUS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

bool RamFs::AddDirEntry(Node* n, const wchar_t* name, size_t nameLen, PVOID buf, ULONG len, PULONG bt)
{
    union {
        UINT8 raw[sizeof(FSP_FSCTL_DIR_INFO) + 256 * sizeof(WCHAR)];
        FSP_FSCTL_DIR_INFO d;
    } u;
    if (nameLen > 255) nameLen = 255;
    memset(&u.d, 0, sizeof(FSP_FSCTL_DIR_INFO));
    u.d.Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + nameLen * sizeof(WCHAR));
    FillInfo(n, &u.d.FileInfo);
    memcpy(u.d.FileNameBuf, name, nameLen * sizeof(WCHAR));
    return FspFileSystemAddDirInfo(&u.d, buf, len, bt) != FALSE;
}

NTSTATUS RamFs::ReadDirectory(FSP_FILE_SYSTEM* fs, PVOID ctx, PWSTR pattern, PWSTR marker,
    PVOID buf, ULONG len, PULONG pbt)
{
    (void)pattern;
    RamFs* s = Self(fs);
    Node* d = (Node*)ctx;
    if (!d->IsDir() || !d->children) return STATUS_NOT_A_DIRECTORY;
    Shared tl(&s->tree_);
    if (d != s->root_ && d->parent) {
        if (!marker) {
            if (!AddDirEntry(d, L".", 1, buf, len, pbt)) return STATUS_SUCCESS;
        }
        if (!marker || wcscmp(marker, L".") == 0) {
            if (!AddDirEntry(d->parent, L"..", 2, buf, len, pbt)) return STATUS_SUCCESS;
        }
        if (marker && (wcscmp(marker, L".") == 0 || wcscmp(marker, L"..") == 0)) marker = nullptr;
    }
    auto it = marker ? d->children->upper_bound(std::wstring_view(marker)) : d->children->begin();
    for (; it != d->children->end(); ++it) {
        if (!AddDirEntry(it->second, it->first.data(), it->first.size(), buf, len, pbt))
            return STATUS_SUCCESS;
    }
    FspFileSystemAddDirInfo(nullptr, buf, len, pbt);
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::GetDirInfoByName(FSP_FILE_SYSTEM* fs, PVOID ctx, PWSTR fileName, FSP_FSCTL_DIR_INFO* di)
{
    RamFs* s = Self(fs);
    Node* d = (Node*)ctx;
    if (!d->IsDir() || !d->children) return STATUS_NOT_A_DIRECTORY;
    Shared tl(&s->tree_);
    auto it = d->children->find(std::wstring_view(fileName));
    if (it == d->children->end()) return STATUS_OBJECT_NAME_NOT_FOUND;
    Node* n = it->second;
    size_t nl = std::min<size_t>(n->name.size(), 255);
    memset(di, 0, sizeof(FSP_FSCTL_DIR_INFO));
    di->Size = (UINT16)(sizeof(FSP_FSCTL_DIR_INFO) + nl * sizeof(WCHAR));
    FillInfo(n, &di->FileInfo);
    memcpy(di->FileNameBuf, n->name.data(), nl * sizeof(WCHAR));
    return STATUS_SUCCESS;
}

// Wird nur mit gehaltener Namensraumsperre aufgerufen.
NTSTATUS RamFs::GetReparsePointByName(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR name, BOOLEAN isDir,
    PVOID buf, PSIZE_T psize)
{
    (void)context; (void)isDir;
    RamFs* s = Self(fs);
    ParsedPath pp;
    if (!ParsePath(name, pp)) return STATUS_OBJECT_NAME_INVALID;
    Node* n;
    NTSTATUS st = s->Find(pp, &n, nullptr);
    if (!NT_SUCCESS(st)) return st;
    Node* m = n->Main();
    if (!(m->attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return STATUS_NOT_A_REPARSE_POINT;
    if (buf) {
        if (m->reparse.size() > *psize) return STATUS_BUFFER_TOO_SMALL;
        *psize = m->reparse.size();
        memcpy(buf, m->reparse.data(), m->reparse.size());
    }
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::ResolveReparsePoints(FSP_FILE_SYSTEM* fs, PWSTR name, UINT32 idx, BOOLEAN resolveLast,
    PIO_STATUS_BLOCK pio, PVOID buf, PSIZE_T psize)
{
    RamFs* s = Self(fs);
    try {
        Shared tl(&s->tree_);
        return FspFileSystemResolveReparsePoints(fs, GetReparsePointByName, s, name, idx, resolveLast, pio, buf, psize);
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS RamFs::GetReparsePoint(FSP_FILE_SYSTEM* fs, PVOID ctx, PWSTR name, PVOID buf, PSIZE_T psize)
{
    (void)fs; (void)name;
    Node* m = ((Node*)ctx)->Main();
    Shared ml(&m->lock);
    if (!(m->attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return STATUS_NOT_A_REPARSE_POINT;
    if (m->reparse.size() > *psize) return STATUS_BUFFER_TOO_SMALL;
    *psize = m->reparse.size();
    memcpy(buf, m->reparse.data(), m->reparse.size());
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::SetReparsePoint(FSP_FILE_SYSTEM* fs, PVOID ctx, PWSTR name, PVOID buf, SIZE_T size)
{
    (void)name;
    RamFs* s = Self(fs);
    Node* m = ((Node*)ctx)->Main();
    try {
        if (size < 8) return STATUS_IO_REPARSE_DATA_INVALID;
        Excl tl(&s->tree_);
        if (m->IsDir() && m->children && !m->children->empty()) return STATUS_DIRECTORY_NOT_EMPTY;
        bool had = (m->attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (had) {
            NTSTATUS st = FspFileSystemCanReplaceReparsePoint(m->reparse.data(), m->reparse.size(), buf, size);
            if (!NT_SUCCESS(st)) return st;
        }
        Excl ml(&m->lock);
        m->reparse.assign((UINT8*)buf, (UINT8*)buf + size);
        m->attributes |= FILE_ATTRIBUTE_REPARSE_POINT;
        m->reparseTag = *(ULONG*)buf;
        m->changeTime = NowFt();
        if (!had && m->linked) s->reparseCount_++;
        return STATUS_SUCCESS;
    } catch (const std::bad_alloc&) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }
}

NTSTATUS RamFs::DeleteReparsePoint(FSP_FILE_SYSTEM* fs, PVOID ctx, PWSTR name, PVOID buf, SIZE_T size)
{
    (void)name;
    RamFs* s = Self(fs);
    Node* m = ((Node*)ctx)->Main();
    Excl tl(&s->tree_);
    if (!(m->attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return STATUS_NOT_A_REPARSE_POINT;
    NTSTATUS st = FspFileSystemCanReplaceReparsePoint(m->reparse.data(), m->reparse.size(), buf, size);
    if (!NT_SUCCESS(st)) return st;
    Excl ml(&m->lock);
    m->reparse.clear();
    m->reparse.shrink_to_fit();
    m->attributes &= ~FILE_ATTRIBUTE_REPARSE_POINT;
    m->reparseTag = 0;
    m->changeTime = NowFt();
    if (m->linked) s->reparseCount_--;
    return STATUS_SUCCESS;
}

NTSTATUS RamFs::GetStreamInfo(FSP_FILE_SYSTEM* fs, PVOID ctx, PVOID buf, ULONG len, PULONG pbt)
{
    RamFs* s = Self(fs);
    Node* m = ((Node*)ctx)->Main();
    Shared tl(&s->tree_);
    auto add = [&](const std::wstring& nm, Node* x) -> bool {
        union {
            UINT8 raw[sizeof(FSP_FSCTL_STREAM_INFO) + 256 * sizeof(WCHAR)];
            FSP_FSCTL_STREAM_INFO si;
        } u;
        size_t l = std::min<size_t>(nm.size(), 255);
        memset(&u.si, 0, sizeof(FSP_FSCTL_STREAM_INFO));
        u.si.Size = (UINT16)(sizeof(FSP_FSCTL_STREAM_INFO) + l * sizeof(WCHAR));
        u.si.StreamSize = x->fileSize;
        u.si.StreamAllocationSize = x->allocSize;
        memcpy(u.si.StreamNameBuf, nm.data(), l * sizeof(WCHAR));
        return FspFileSystemAddStreamInfo(&u.si, buf, len, pbt) != FALSE;
    };
    if (!m->IsDir() && !add(std::wstring(), m)) return STATUS_SUCCESS;
    if (m->streams)
        for (auto& kv : *m->streams)
            if (!add(kv.second->name, kv.second)) return STATUS_SUCCESS;
    FspFileSystemAddStreamInfo(nullptr, buf, len, pbt);
    return STATUS_SUCCESS;
}

FSP_FILE_SYSTEM_INTERFACE RamFs::MakeInterface()
{
    FSP_FILE_SYSTEM_INTERFACE i;
    memset(&i, 0, sizeof i);
    i.GetVolumeInfo = GetVolumeInfo;
    i.SetVolumeLabel = SetVolumeLabel;
    i.GetSecurityByName = GetSecurityByName;
    i.Create = CreateCb;
    i.Open = Open;
    i.Overwrite = Overwrite;
    i.Cleanup = Cleanup;
    i.Close = Close;
    i.Read = Read;
    i.Write = Write;
    i.Flush = Flush;
    i.GetFileInfo = GetFileInfo;
    i.SetBasicInfo = SetBasicInfo;
    i.SetFileSize = SetFileSize;
    i.CanDelete = CanDelete;
    i.Rename = Rename;
    i.GetSecurity = GetSecurity;
    i.SetSecurity = SetSecurity;
    i.ReadDirectory = ReadDirectory;
    i.ResolveReparsePoints = ResolveReparsePoints;
    i.GetReparsePoint = GetReparsePoint;
    i.SetReparsePoint = SetReparsePoint;
    i.DeleteReparsePoint = DeleteReparsePoint;
    i.GetStreamInfo = GetStreamInfo;
    i.GetDirInfoByName = GetDirInfoByName;
    return i;
}

} // namespace rdb
