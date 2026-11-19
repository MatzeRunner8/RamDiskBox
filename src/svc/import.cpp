// RamDiskBox - Quellordner beim Einhaengen ins RAM-Laufwerk kopieren
#include "ramfs.h"
#include "../common/common.h"
#include <algorithm>
#include <cstring>
#include <new>

namespace rdb {

namespace {

constexpr size_t kChunk = 64 * 1024;
constexpr UINT64 kAllocUnit = 4096;
constexpr DWORD kKeepAttrs = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
    FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;

// Pfade mit \\?\ fuer mehr als MAX_PATH Zeichen
std::wstring LongPath(const std::wstring& p)
{
    if (p.rfind(L"\\\\?\\", 0) == 0) return p;
    if (p.size() > 2 && p[1] == L':') return L"\\\\?\\" + p;
    if (p.rfind(L"\\\\", 0) == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return p;
}

UINT64 Ft(const FILETIME& f) { return ((UINT64)f.dwHighDateTime << 32) | f.dwLowDateTime; }

bool AllZero(const UINT8* p, size_t n)
{
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
        if (*(const UINT64*)(p + i)) return false;
    for (; i < n; i++)
        if (p[i]) return false;
    return true;
}

} // namespace

bool RamFs::ImportFolder(const std::wstring& srcIn, ImportStats& st, std::wstring& err)
{
    std::wstring src = srcIn;
    while (src.size() > 3 && (src.back() == L'\\' || src.back() == L'/')) src.pop_back();
    DWORD a = GetFileAttributesW(LongPath(src).c_str());
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY)) {
        err = TrF(L"Quellordner nicht gefunden: %s", L"Source folder not found: %s", src.c_str());
        return false;
    }
    try {
        std::vector<UINT8> buf(1024 * 1024);   // Vielfaches von 64 KiB: Teilstuecke liegen in je einem Block
        bool full = false;
        {
            Excl tl(&tree_);
            std::vector<std::pair<std::wstring, Node*>> stack{ { src, root_ } };
            while (!stack.empty() && !full) {
                auto [dir, parent] = stack.back();
                stack.pop_back();
                WIN32_FIND_DATAW fd;
                std::wstring pattern = LongPath(dir) + L"\\*";
                HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (h == INVALID_HANDLE_VALUE) { st.skipped++; continue; }
                do {
                    std::wstring name = fd.cFileName;
                    if (name == L"." || name == L"..") continue;
                    // Verknuepfungen (Symlinks, Junctions) nicht verfolgen
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || name.size() > 255) { st.skipped++; continue; }
                    bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
                    std::wstring path = dir + L"\\" + name;
                    auto it = parent->children->find(std::wstring_view(name));
                    if (it != parent->children->end()) {
                        // Schon vorhanden (z. B. per "Ordner anlegen"): Ordner zusammenfuehren, Dateien nicht ersetzen
                        if (isDir && it->second->IsDir()) stack.emplace_back(path, it->second);
                        else st.skipped++;
                        continue;
                    }
                    Node* n = NewNode();
                    n->name = name;
                    n->attributes = fd.dwFileAttributes & kKeepAttrs;
                    n->creationTime = Ft(fd.ftCreationTime);
                    n->lastAccessTime = Ft(fd.ftLastAccessTime);
                    n->lastWriteTime = Ft(fd.ftLastWriteTime);
                    n->changeTime = n->lastWriteTime;
                    n->indexNumber = nextIndex_++;
                    n->sd = InheritedSd(parent, isDir);
                    if (isDir) {
                        n->children = new NodeMap;
                        Link(parent, n);
                        st.dirs++;
                        stack.emplace_back(path, n);
                        continue;
                    }
                    if (!(n->attributes & ~FILE_ATTRIBUTE_ARCHIVE)) n->attributes |= FILE_ATTRIBUTE_ARCHIVE;
                    Link(parent, n);
                    UINT64 size = ((UINT64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
                    if (!Charge(n, (size + kAllocUnit - 1) / kAllocUnit * kAllocUnit)) {
                        Unlink(n);
                        full = true;
                        err = Tr(L"Das Laufwerk ist zu klein für den Quellordner; es wurde nur ein Teil übernommen.", L"The drive is too small for the source folder; only part of it was copied.");
                        break;
                    }
                    HANDLE f = CreateFileW(LongPath(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_BACKUP_SEMANTICS, nullptr);
                    if (f == INVALID_HANDLE_VALUE) { Unlink(n); st.skipped++; continue; }
                    bool ok = true;
                    {
                        Excl nl(&n->lock);
                        n->fileSize = size;
                        UINT64 off = 0;
                        while (ok && off < size) {
                            DWORD want = (DWORD)std::min<UINT64>(buf.size(), size - off), got = 0;
                            if (!ReadFile(f, buf.data(), want, &got, nullptr) || got == 0) { ok = false; break; }
                            for (DWORD p = 0; p < got; p += (DWORD)kChunk) {
                                DWORD k = std::min<DWORD>((DWORD)kChunk, got - p);
                                if (AllZero(buf.data() + p, k)) continue;   // Nullen belegen keinen Speicher
                                if (!NT_SUCCESS(WriteRange(n, off + p, buf.data() + p, k))) { ok = false; break; }
                            }
                            off += got;
                        }
                    }
                    CloseHandle(f);
                    if (!ok) { Unlink(n); st.skipped++; continue; }
                    st.files++;
                    st.bytes += size;
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        }
        if (compLevel_.load() > 0) EnqueueAll();
        return true;
    } catch (const std::bad_alloc&) {
        err = Tr(L"Zu wenig Arbeitsspeicher beim Kopieren des Quellordners.", L"Not enough memory to copy the source folder.");
        return false;
    }
}

} // namespace rdb
