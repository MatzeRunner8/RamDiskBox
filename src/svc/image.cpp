// RamDiskBox - Image-Dateien (Inhalt eines Laufwerks sichern und wiederherstellen)
//
// Aufbau: FileHeader, danach eine Folge von Bloecken (BlockHeader + Daten, XPRESS-komprimiert
// oder roh, CRC32 ueber die Rohdaten), abgeschlossen durch einen Block mit rawLen 0.
// Der Blockstrom enthaelt Datensaetze in Vorordnung (Eltern vor Kindern).
#include "ramfs.h"
#include "../common/common.h"
#include <compressapi.h>
#include <algorithm>
#include <cstring>
#include <new>

namespace rdb {

namespace {

constexpr char kMagic[8] = { 'R', 'D', 'B', 'I', 'M', 'G', '0', '1' };
constexpr UINT32 kVersion = 1;
constexpr size_t kBlock = 4 * 1024 * 1024;
constexpr size_t kChunk = 64 * 1024;
constexpr UINT64 kAllocUnit = 4096;

enum : UINT8 { RecEnd = 0, RecDir = 1, RecFile = 2, RecStream = 3 };

#pragma pack(push, 1)
struct FileHeader {
    char magic[8];
    UINT32 version;
    UINT32 flags;          // Bit 0: komprimiert
    UINT64 capacity;
    UINT64 created;
    UINT32 labelLen;
    WCHAR label[32];
};
struct BlockHeader {
    UINT32 rawLen;
    UINT32 storedLen;
    UINT32 crc;
    UINT8 method;          // 0 = roh, 1 = XPRESS
};
#pragma pack(pop)

typedef ULONG(NTAPI* RtlComputeCrc32Fn)(ULONG, const BYTE*, INT);

ULONG Crc(const BYTE* p, size_t n)
{
    static RtlComputeCrc32Fn f = (RtlComputeCrc32Fn)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlComputeCrc32");
    return f ? f(0, p, (INT)n) : 0;
}

UINT64 NowFt()
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    return ((UINT64)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

bool WriteAll(HANDLE h, const void* p, size_t n)
{
    const BYTE* b = (const BYTE*)p;
    while (n) {
        DWORD k = (DWORD)std::min<size_t>(n, 1u << 30), w = 0;
        if (!WriteFile(h, b, k, &w, nullptr) || w != k) return false;
        b += k; n -= k;
    }
    return true;
}

bool ReadAll(HANDLE h, void* p, size_t n)
{
    BYTE* b = (BYTE*)p;
    while (n) {
        DWORD k = (DWORD)std::min<size_t>(n, 1u << 30), r = 0;
        if (!ReadFile(h, b, k, &r, nullptr) || r != k) return false;
        b += k; n -= k;
    }
    return true;
}

bool AllZero(const BYTE* p, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        if (*(const UINT64*)(p + i)) return false;
    for (; i < n; i++)
        if (p[i]) return false;
    return true;
}

bool CreateParentDirs(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return true;
    std::wstring dir = path.substr(0, pos);
    if (dir.empty() || (dir.size() == 2 && dir[1] == L':')) return true;
    DWORD a = GetFileAttributesW(dir.c_str());
    if (a != INVALID_FILE_ATTRIBUTES) return (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!CreateParentDirs(dir)) return false;
    return CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

} // namespace

// ----------------------------------------------------------------------- Schreiber

struct RamFs::ImageWriter {
    HANDLE h = INVALID_HANDLE_VALUE;
    bool compress = false;
    COMPRESSOR_HANDLE comp = nullptr;
    std::vector<BYTE> buf, out;
    size_t fill = 0;

    bool Init(HANDLE file, bool c)
    {
        h = file;
        compress = c;
        buf.resize(kBlock);
        if (compress) {
            if (!CreateCompressor(COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW, nullptr, &comp)) compress = false;
            else out.resize(kBlock + kBlock / 2);
        }
        return true;
    }
    ~ImageWriter() { if (comp) CloseCompressor(comp); }

    bool FlushBlock()
    {
        if (!fill) return true;
        BlockHeader bh{};
        bh.rawLen = (UINT32)fill;
        bh.crc = Crc(buf.data(), fill);
        const BYTE* data = buf.data();
        bh.storedLen = (UINT32)fill;
        bh.method = 0;
        if (compress) {
            SIZE_T cs = 0;
            if (Compress(comp, buf.data(), fill, out.data(), out.size(), &cs) && cs < fill) {
                data = out.data();
                bh.storedLen = (UINT32)cs;
                bh.method = 1;
            }
        }
        if (!WriteAll(h, &bh, sizeof bh) || !WriteAll(h, data, bh.storedLen)) return false;
        fill = 0;
        return true;
    }

    bool Put(const void* p, size_t n)
    {
        const BYTE* b = (const BYTE*)p;
        while (n) {
            size_t k = std::min(n, kBlock - fill);
            memcpy(buf.data() + fill, b, k);
            fill += k; b += k; n -= k;
            if (fill == kBlock && !FlushBlock()) return false;
        }
        return true;
    }
    template <class T> bool PutV(const T& v) { return Put(&v, sizeof v); }
    bool PutStr(const std::wstring& s)
    {
        UINT16 l = (UINT16)std::min<size_t>(s.size(), 32767);
        return PutV(l) && Put(s.data(), l * sizeof(WCHAR));
    }
    bool PutBlob(const std::vector<UINT8>& v)
    {
        UINT32 l = (UINT32)v.size();
        return PutV(l) && (l == 0 || Put(v.data(), l));
    }
    bool Finish()
    {
        if (!FlushBlock()) return false;
        BlockHeader end{};
        return WriteAll(h, &end, sizeof end);
    }
};

// ----------------------------------------------------------------------- Leser

struct RamFs::ImageReader {
    HANDLE h = INVALID_HANDLE_VALUE;
    DECOMPRESSOR_HANDLE dec = nullptr;
    std::vector<BYTE> buf, in;
    size_t pos = 0, len = 0;
    bool eof = false;
    std::wstring err;

    bool Init(HANDLE file)
    {
        h = file;
        buf.resize(kBlock);
        in.resize(kBlock + kBlock / 2);
        if (!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS | COMPRESS_RAW, nullptr, &dec)) {
            err = Tr(L"Dekompressor nicht verfügbar.", L"Decompressor not available.");
            return false;
        }
        return true;
    }
    ~ImageReader() { if (dec) CloseDecompressor(dec); }

    bool NextBlock()
    {
        BlockHeader bh;
        if (!ReadAll(h, &bh, sizeof bh)) { err = Tr(L"Image ist abgeschnitten.", L"Image is truncated."); return false; }
        if (bh.rawLen == 0) { eof = true; err = Tr(L"Unerwartetes Ende des Images.", L"Unexpected end of image."); return false; }
        if (bh.rawLen > kBlock || bh.storedLen > in.size() || (bh.method == 0 && bh.storedLen != bh.rawLen)) {
            err = Tr(L"Image ist beschädigt (Blockkopf).", L"Image is damaged (block header).");
            return false;
        }
        if (bh.method == 0) {
            if (!ReadAll(h, buf.data(), bh.rawLen)) { err = Tr(L"Image ist abgeschnitten.", L"Image is truncated."); return false; }
        } else if (bh.method == 1) {
            if (!ReadAll(h, in.data(), bh.storedLen)) { err = Tr(L"Image ist abgeschnitten.", L"Image is truncated."); return false; }
            SIZE_T outLen = 0;
            if (!Decompress(dec, in.data(), bh.storedLen, buf.data(), bh.rawLen, &outLen) || outLen != bh.rawLen) {
                err = Tr(L"Image ist beschädigt (Entpacken).", L"Image is damaged (decompression).");
                return false;
            }
        } else {
            err = Tr(L"Image ist beschädigt (Verfahren).", L"Image is damaged (method).");
            return false;
        }
        if (Crc(buf.data(), bh.rawLen) != bh.crc) { err = Tr(L"Image ist beschädigt (Prüfsumme).", L"Image is damaged (checksum)."); return false; }
        pos = 0;
        len = bh.rawLen;
        return true;
    }

    bool Get(void* p, size_t n)
    {
        BYTE* b = (BYTE*)p;
        while (n) {
            if (pos == len && !NextBlock()) return false;
            size_t k = std::min(n, len - pos);
            memcpy(b, buf.data() + pos, k);
            pos += k; b += k; n -= k;
        }
        return true;
    }
    template <class T> bool GetV(T& v) { return Get(&v, sizeof v); }
    bool GetStr(std::wstring& s)
    {
        UINT16 l;
        if (!GetV(l)) return false;
        s.resize(l);
        return l == 0 || Get(s.data(), l * sizeof(WCHAR));
    }
    bool GetBlob(std::vector<UINT8>& v, size_t maxLen)
    {
        UINT32 l;
        if (!GetV(l)) return false;
        if (l > maxLen) { err = Tr(L"Image ist beschädigt (Feldlänge).", L"Image is damaged (field length)."); return false; }
        v.resize(l);
        return l == 0 || Get(v.data(), l);
    }
};

// ----------------------------------------------------------------------- Speichern

bool RamFs::WriteNodeRec(ImageWriter& w, Node* n, const std::wstring& rel, std::wstring& err)
{
    Shared nl(&n->lock);
    UINT8 type = n->isStream ? RecStream : (n->IsDir() ? RecDir : RecFile);
    bool ok = w.PutV(type) && w.PutStr(rel) &&
        w.PutV(n->attributes) && w.PutV(n->reparseTag) &&
        w.PutV(n->creationTime) && w.PutV(n->lastAccessTime) && w.PutV(n->lastWriteTime) && w.PutV(n->changeTime) &&
        w.PutBlob(n->sd) && w.PutBlob(n->reparse) &&
        w.PutV(n->fileSize) && w.PutV(n->allocSize);
    if (!ok) { err = TrF(L"Schreibfehler: %lu", L"Write error: %lu", GetLastError()); return false; }
    if (type != RecDir && n->fileSize) {
        std::vector<UINT8> tmp(kChunk);
        UINT64 chunks = (n->fileSize + kChunk - 1) / kChunk;
        for (UINT64 c = 0; c < chunks; c++) {
            size_t k = (size_t)std::min<UINT64>(kChunk, n->fileSize - c * kChunk);
            bool present = c < n->chunks.size() && n->chunks[(size_t)c].p != nullptr;
            if (present) {
                ReadRange(n, c * kChunk, tmp.data(), k);
                if (AllZero(tmp.data(), k)) present = false;
            }
            UINT8 flag = present ? 1 : 0;
            if (!w.PutV(flag) || (present && !w.Put(tmp.data(), k))) {
                err = TrF(L"Schreibfehler: %lu", L"Write error: %lu", GetLastError());
                return false;
            }
        }
    }
    return true;
}

bool RamFs::SaveTree(ImageWriter& w, Node* dir, const std::wstring& rel, std::wstring& err)
{
    for (auto& kv : *dir->children) {
        Node* c = kv.second;
        std::wstring cr = rel.empty() ? c->name : rel + L"\\" + c->name;
        if (!WriteNodeRec(w, c, cr, err)) return false;
        if (c->streams)
            for (auto& s : *c->streams)
                if (!WriteNodeRec(w, s.second, cr + L":" + s.second->name, err)) return false;
        if (c->IsDir() && !SaveTree(w, c, cr, err)) return false;
    }
    return true;
}

bool RamFs::SaveImage(const std::wstring& path, bool compress, std::wstring& err)
{
    try {
        if (path.size() >= 2 && path[1] == L':' && towupper(path[0]) == towupper(opt_.letter)) {
            err = Tr(L"Das Image darf nicht auf dem RAM-Laufwerk selbst liegen.", L"The image must not be stored on the RAM drive itself.");
            return false;
        }
        if (!CreateParentDirs(path)) { err = Tr(L"Zielordner kann nicht angelegt werden.", L"Cannot create the target folder."); return false; }
        // ohne gehaltene Sperren: der Treiber schreibt dabei ueber uns zurueck. Bleibt der Windows-Cache
        // beim Schliessen erhalten, ist der Flush Pflicht - sonst fehlten juengste Aenderungen im Image.
        if (!FlushVolume() && opt_.useCache && opt_.keepCache) {
            err = Tr(L"Der Windows-Cache des Laufwerks konnte nicht zurückgeschrieben werden; das Image wurde nicht gespeichert.",
                     L"The drive's Windows cache could not be written back; the image was not saved.");
            return false;
        }
        std::wstring tmp = path + L".tmp";
        HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            err = TrF(L"Image-Datei kann nicht angelegt werden (Fehler %lu).", L"Cannot create the image file (error %lu).", GetLastError());
            return false;
        }
        bool ok = true;
        {
            FileHeader fh{};
            memcpy(fh.magic, kMagic, 8);
            fh.version = kVersion;
            fh.flags = compress ? 1 : 0;
            fh.capacity = capacity_;
            fh.created = NowFt();
            std::wstring lbl = Label();
            fh.labelLen = (UINT32)std::min<size_t>(lbl.size(), 32);
            memcpy(fh.label, lbl.data(), fh.labelLen * sizeof(WCHAR));
            ok = WriteAll(h, &fh, sizeof fh);
            if (!ok) err = Tr(L"Schreibfehler beim Dateikopf.", L"Write error in the file header.");
        }
        if (ok) {
            ImageWriter w;
            w.Init(h, compress);
            {
                Shared tl(&tree_);
                ok = WriteNodeRec(w, root_, L"", err) && SaveTree(w, root_, L"", err);
            }
            if (ok) {
                UINT8 end = RecEnd;
                ok = w.PutV(end) && w.Finish();
                if (!ok) err = Tr(L"Schreibfehler am Ende des Images.", L"Write error at the end of the image.");
            }
        }
        if (ok && !FlushFileBuffers(h)) { ok = false; err = Tr(L"Image konnte nicht auf den Datenträger geschrieben werden.", L"The image could not be written to disk."); }
        CloseHandle(h);
        if (!ok) { DeleteFileW(tmp.c_str()); return false; }
        if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            err = TrF(L"Image konnte nicht ersetzt werden (Fehler %lu).", L"The image could not be replaced (error %lu).", GetLastError());
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    } catch (const std::bad_alloc&) {
        err = Tr(L"Zu wenig Arbeitsspeicher zum Speichern.", L"Not enough memory to save.");
        return false;
    }
}

// ----------------------------------------------------------------------- Laden

bool RamFs::LoadImage(const std::wstring& path, std::wstring& err, bool* notOpened)
{
    if (notOpened) *notOpened = false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e = GetLastError();
        err = TrF(L"Image-Datei kann nicht geöffnet werden: %s", L"Cannot open the image file: %s", Win32Message(e).c_str());
        if (notOpened) *notOpened = true;
        return false;
    }
    struct Closer { HANDLE h; ~Closer() { CloseHandle(h); } } closer{ h };
    try {
        FileHeader fh;
        if (!ReadAll(h, &fh, sizeof fh) || memcmp(fh.magic, kMagic, 8) != 0) {
            err = Tr(L"Keine RamDiskBox-Image-Datei.", L"Not a RamDiskBox image file.");
            return false;
        }
        if (fh.version != kVersion) { err = Tr(L"Unbekannte Image-Version.", L"Unknown image version."); return false; }
        ImageReader r;
        if (!r.Init(h)) { err = r.err; return false; }
        Excl tl(&tree_);
        std::vector<UINT8> tmp(kChunk);
        for (;;) {
            UINT8 type;
            if (!r.GetV(type)) { err = r.err; return false; }
            if (type == RecEnd) break;
            if (type != RecDir && type != RecFile && type != RecStream) { err = Tr(L"Image ist beschädigt (Satzart).", L"Image is damaged (record type)."); return false; }
            std::wstring rel;
            UINT32 attrs, tag;
            UINT64 ct, at, wt, cht, fsize, asize;
            std::vector<UINT8> sd, rp;
            if (!r.GetStr(rel) || !r.GetV(attrs) || !r.GetV(tag) || !r.GetV(ct) || !r.GetV(at) || !r.GetV(wt) ||
                !r.GetV(cht) || !r.GetBlob(sd, 65536) || !r.GetBlob(rp, 16 * 1024 + 8) || !r.GetV(fsize) || !r.GetV(asize)) {
                err = r.err.empty() ? std::wstring(Tr(L"Image ist beschädigt.", L"Image is damaged.")) : r.err;
                return false;
            }
            Node* n = nullptr;
            if (rel.empty()) {
                if (type != RecDir) { err = Tr(L"Image ist beschädigt (Wurzel).", L"Image is damaged (root)."); return false; }
                n = root_;
                n->attributes = attrs | FILE_ATTRIBUTE_DIRECTORY;
                n->creationTime = ct; n->lastAccessTime = at; n->lastWriteTime = wt; n->changeTime = cht;
                if (!sd.empty() && IsValidSecurityDescriptor(sd.data())) n->sd = sd;
                continue;
            }
            std::wstring full = L"\\" + rel;
            ParsedPath pp;
            if (!ParsePath(full.c_str(), pp) || pp.comps.empty() || ((type == RecStream) != !pp.stream.empty())) {
                err = Tr(L"Image ist beschädigt (Pfad).", L"Image is damaged (path).");
                return false;
            }
            Node* existing = nullptr;
            Node* parent = nullptr;
            NTSTATUS st = Find(pp, &existing, &parent);
            if (NT_SUCCESS(st) || st != STATUS_OBJECT_NAME_NOT_FOUND || !parent) {
                err = TrF(L"Image ist beschädigt (Struktur: %s).", L"Image is damaged (structure: %s).", rel.c_str());
                return false;
            }
            n = NewNode();
            if (type == RecStream) {
                n->isStream = true;
                n->name = pp.stream;
            } else {
                n->name = pp.comps.back();
                n->attributes = attrs;
                n->reparseTag = tag;
                n->creationTime = ct; n->lastAccessTime = at; n->lastWriteTime = wt; n->changeTime = cht;
                n->indexNumber = nextIndex_++;
                n->sd = (!sd.empty() && IsValidSecurityDescriptor(sd.data())) ? sd : parent->sd;
                if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) n->reparse = rp;
                if (type == RecDir) {
                    n->attributes |= FILE_ATTRIBUTE_DIRECTORY;
                    n->children = new NodeMap;
                } else {
                    n->attributes &= ~FILE_ATTRIBUTE_DIRECTORY;
                }
            }
            Link(parent, n);
            if (type != RecDir) {
                UINT64 a = std::max<UINT64>((fsize + kAllocUnit - 1) / kAllocUnit * kAllocUnit,
                    asize / kAllocUnit * kAllocUnit);
                if (!Charge(n, a)) {
                    err = Tr(L"Das Image ist größer als das Laufwerk. Bitte die Größe erhöhen.", L"The image is larger than the drive. Please increase the size.");
                    return false;
                }
                n->fileSize = fsize;
                UINT64 chunks = (fsize + kChunk - 1) / kChunk;
                for (UINT64 c = 0; c < chunks; c++) {
                    UINT8 flag;
                    if (!r.GetV(flag)) { err = r.err; return false; }
                    if (!flag) continue;
                    size_t k = (size_t)std::min<UINT64>(kChunk, fsize - c * kChunk);
                    if (!r.Get(tmp.data(), k)) { err = r.err; return false; }
                    if (!NT_SUCCESS(WriteRange(n, c * kChunk, tmp.data(), k))) {
                        err = Tr(L"Zu wenig Arbeitsspeicher zum Laden des Images.", L"Not enough memory to load the image.");
                        return false;
                    }
                }
                n->compDirty = false;
            }
        }
        if (compLevel_.load() > 0) {
            ReleaseSRWLockExclusive(&tree_);   // EnqueueAll sperrt selbst (geteilt)
            EnqueueAll();
            AcquireSRWLockExclusive(&tree_);
        }
        return true;
    } catch (const std::bad_alloc&) {
        err = Tr(L"Zu wenig Arbeitsspeicher zum Laden des Images.", L"Not enough memory to load the image.");
        return false;
    }
}

} // namespace rdb
