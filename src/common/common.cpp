#include "common.h"
#include <shlobj.h>
#include <sddl.h>
#include <aclapi.h>
#include <cwctype>
#include <cstdio>
#include <cstdarg>
#include <atomic>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace rdb {

// ---------------------------------------------------------------- Sprache

namespace {
std::atomic<int> g_procLang{ -1 };
thread_local int t_lang = -1;
}

bool ParseLang(const std::wstring& s, Lang& out)
{
    std::wstring t = Lower(Trim(s));
    if (t == L"de" || t == L"deutsch" || t == L"german") { out = Lang::De; return true; }
    if (t == L"en" || t == L"english" || t == L"englisch") { out = Lang::En; return true; }
    return false;
}

Lang DefaultLanguage()
{
    wchar_t buf[16];
    DWORD n = GetEnvironmentVariableW(L"RDB_LANG", buf, 16);
    Lang l;
    if (n > 0 && n < 16 && ParseLang(std::wstring(buf, n), l)) return l;
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_GERMAN ? Lang::De : Lang::En;
}

Lang ProcessLanguage()
{
    int v = g_procLang.load();
    if (v < 0) {
        v = (int)DefaultLanguage();
        g_procLang = v;
    }
    return (Lang)v;
}

void SetProcessLanguage(Lang l) { g_procLang = (int)l; }
Lang CurrentLanguage() { return t_lang >= 0 ? (Lang)t_lang : ProcessLanguage(); }
void SetThreadLanguage(Lang l) { t_lang = (int)l; }
const wchar_t* LangCode(Lang l) { return l == Lang::En ? L"en" : L"de"; }

std::wstring TrF(const wchar_t* de, const wchar_t* en, ...)
{
    wchar_t buf[4096];
    va_list ap;
    va_start(ap, en);
    _vsnwprintf_s(buf, _TRUNCATE, En() ? en : de, ap);
    va_end(ap);
    return buf;
}

std::wstring Timestamp()
{
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t b[64];
    swprintf_s(b, L"%04u-%02u-%02u %02u:%02u:%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

std::wstring DisplayTime(const std::wstring& s)
{
    unsigned y, mo, d, h, mi, se;
    wchar_t b[64];
    if (s.size() == 19 && swscanf_s(s.c_str(), L"%4u-%2u-%2u %2u:%2u:%2u", &y, &mo, &d, &h, &mi, &se) == 6) {
        if (!En()) { swprintf_s(b, L"%02u.%02u.%04u %02u:%02u:%02u", d, mo, y, h, mi, se); return b; }
        return s;
    }
    // aeltere Versionen speicherten "19.11.2026 18:04:05"
    if (s.size() == 19 && swscanf_s(s.c_str(), L"%2u.%2u.%4u %2u:%2u:%2u", &d, &mo, &y, &h, &mi, &se) == 6) {
        if (En()) { swprintf_s(b, L"%04u-%02u-%02u %02u:%02u:%02u", y, mo, d, h, mi, se); return b; }
        return s;
    }
    return s;
}

// ---------------------------------------------------------------- Protokoll

std::wstring InstanceSuffix()
{
    wchar_t buf[64];
    DWORD n = GetEnvironmentVariableW(L"RDB_INSTANCE", buf, 64);
    return (n > 0 && n < 64) ? L"." + std::wstring(buf, n) : std::wstring();
}

const wchar_t* PipeName()
{
    static std::wstring name = std::wstring(kPipeName) + InstanceSuffix();
    return name.c_str();
}

static std::wstring Escape(const std::wstring& s)
{
    std::wstring r;
    r.reserve(s.size());
    for (wchar_t c : s) {
        if (c == L'%') r += L"%25";
        else if (c == L'\n') r += L"%0A";
        else if (c == L'\r') r += L"%0D";
        else r += c;
    }
    return r;
}

static std::wstring Unescape(const std::wstring& s)
{
    std::wstring r;
    r.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == L'%' && i + 2 < s.size()) {
            std::wstring h = s.substr(i + 1, 2);
            if (h == L"25") { r += L'%'; i += 2; continue; }
            if (h == L"0A") { r += L'\n'; i += 2; continue; }
            if (h == L"0D") { r += L'\r'; i += 2; continue; }
        }
        r += s[i];
    }
    return r;
}

std::wstring Message::Get(const std::wstring& k, const std::wstring& def) const
{
    auto it = args.find(k);
    return it == args.end() ? def : it->second;
}

std::wstring Message::Serialize() const
{
    std::wstring t = cmd + L"\n";
    for (auto& [k, v] : args) t += k + L"=" + Escape(v) + L"\n";
    for (auto& [name, kv] : sections) {
        t += L"[" + name + L"]\n";
        for (auto& [k, v] : kv) t += k + L"=" + Escape(v) + L"\n";
    }
    return t;
}

Message Message::Parse(const std::wstring& text)
{
    Message m;
    size_t pos = 0;
    bool first = true;
    KV* target = &m.args;
    while (pos <= text.size()) {
        size_t e = text.find(L'\n', pos);
        if (e == std::wstring::npos) e = text.size();
        std::wstring line = text.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (first) { m.cmd = Trim(line); first = false; continue; }
        if (line.empty()) { if (e >= text.size()) break; continue; }
        if (line.front() == L'[' && line.back() == L']') {
            m.sections.emplace_back(line.substr(1, line.size() - 2), KV{});
            target = &m.sections.back().second;
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        (*target)[line.substr(0, eq)] = Unescape(line.substr(eq + 1));
        if (e >= text.size()) break;
    }
    return m;
}

bool PipeCall(const Message& req, Message& resp, std::wstring* err, DWORD timeoutMs)
{
    Message r = req;
    if (r.args.find(L"lang") == r.args.end()) r.args[L"lang"] = LangCode(CurrentLanguage());
    std::wstring out = r.Serialize();
    std::vector<wchar_t> buf(kPipeBuffer / sizeof(wchar_t));
    DWORD read = 0;
    BOOL ok = CallNamedPipeW(PipeName(), (LPVOID)out.data(), (DWORD)(out.size() * sizeof(wchar_t)),
        buf.data(), (DWORD)(buf.size() * sizeof(wchar_t)), &read, timeoutMs);
    if (!ok) {
        DWORD e = GetLastError();
        if (err) {
            if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PIPE_BUSY || e == ERROR_SEM_TIMEOUT)
                *err = Tr(L"Der RamDiskBox-Hintergrundprozess läuft nicht.", L"The RamDiskBox background process is not running.");
            else if (e == ERROR_ACCESS_DENIED)
                *err = Tr(L"Zugriff auf den RamDiskBox-Dienst verweigert (Administratorrechte nötig oder Einstellung „Benutzer dürfen steuern“ aus).",
                          L"Access to the RamDiskBox service denied (administrator rights required or setting \"users may control\" off).");
            else
                *err = Tr(L"Verbindung zum Hintergrundprozess fehlgeschlagen: ", L"Connection to the background process failed: ") + Win32Message(e);
        }
        return false;
    }
    resp = Message::Parse(std::wstring(buf.data(), read / sizeof(wchar_t)));
    if (!resp.IsOk()) {
        if (err) *err = resp.Get(L"message", Tr(L"Unbekannter Fehler", L"Unknown error"));
        return false;
    }
    return true;
}

bool ServiceReachable()
{
    Message req, resp;
    req.cmd = L"PING";
    return PipeCall(req, resp, nullptr, 1500);
}

// ---------------------------------------------------------------- VolumeConfig

KV VolumeConfig::ToKV() const
{
    KV kv;
    kv[L"letter"] = std::wstring(1, letter);
    kv[L"size"] = U64(size);
    kv[L"label"] = label;
    kv[L"folders"] = folders;
    kv[L"image"] = image;
    kv[L"loadimage"] = loadImage ? L"1" : L"0";
    kv[L"saveimage"] = saveImage ? L"1" : L"0";
    kv[L"compressimage"] = compressImage ? L"1" : L"0";
    kv[L"autosave"] = U64(autoSaveMin);
    kv[L"automount"] = autoMount ? L"1" : L"0";
    kv[L"ntfs"] = reportNtfs ? L"1" : L"0";
    kv[L"cache"] = useCache ? L"1" : L"0";
    kv[L"wipe"] = wipe ? L"1" : L"0";
    kv[L"compress"] = CompressName(compress);
    kv[L"source"] = source;
    kv[L"avexclude"] = avExclude ? L"1" : L"0";
    return kv;
}

bool VolumeConfig::FromKV(const KV& kv, VolumeConfig& c, std::wstring& err)
{
    auto get = [&](const wchar_t* k, const std::wstring& def) {
        auto it = kv.find(k);
        return it == kv.end() ? def : it->second;
    };
    std::wstring l = Trim(get(L"letter", L""));
    if (l.empty() || !iswalpha(l[0])) { err = Tr(L"Ungültiger Laufwerksbuchstabe.", L"Invalid drive letter."); return false; }
    c.letter = (wchar_t)towupper(l[0]);
    if (c.letter < L'D' || c.letter > L'Z') { err = Tr(L"Der Laufwerksbuchstabe muss zwischen D und Z liegen.", L"The drive letter must be between D and Z."); return false; }
    c.size = ToU64(get(L"size", L"0"));
    if (c.size < 16ull * 1024 * 1024) { err = Tr(L"Die Größe muss mindestens 16 MB betragen.", L"The size must be at least 16 MB."); return false; }
    c.label = get(L"label", L"RAMDisk").substr(0, 32);
    c.folders = get(L"folders", L"");
    c.image = Trim(get(L"image", L""));
    c.loadImage = ToBool(get(L"loadimage", L"1"), true);
    c.saveImage = ToBool(get(L"saveimage", L"1"), true);
    c.compressImage = ToBool(get(L"compressimage", L"1"), true);
    c.autoSaveMin = (unsigned)ToU64(get(L"autosave", L"0"));
    c.autoMount = ToBool(get(L"automount", L"1"), true);
    c.reportNtfs = ToBool(get(L"ntfs", L"1"), true);
    c.useCache = ToBool(get(L"cache", L"1"), true);
    c.wipe = ToBool(get(L"wipe", L"0"), false);
    c.compress = ParseCompress(get(L"compress", L"off"), 0);
    if (c.compress < 0) { err = Tr(L"Ungültige Komprimierung (off, fast, normal, max).", L"Invalid compression (off, fast, normal, max)."); return false; }
    c.source = Trim(get(L"source", L""));
    while (c.source.size() > 3 && (c.source.back() == L'\\' || c.source.back() == L'/')) c.source.pop_back();
    c.avExclude = ToBool(get(L"avexclude", L"0"), false);
    return true;
}

// ---------------------------------------------------------------- Hilfen

std::wstring ExeDir()
{
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring p(buf, n);
    size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? L"." : p.substr(0, s);
}

std::wstring ExePath(const wchar_t* fileName) { return ExeDir() + L"\\" + fileName; }

// ---------------------------------------------------------------- Ablageorte

static std::wstring KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

bool MachineInstall()
{
    static const bool machine = GetFileAttributesW(ExePath(kMachineMarker).c_str()) != INVALID_FILE_ATTRIBUTES;
    return machine;
}

std::wstring ServiceDataDir()
{
    if (!MachineInstall()) return ExeDir();
    std::wstring pd = KnownFolder(FOLDERID_ProgramData);
    return (pd.empty() ? std::wstring(L"C:\\ProgramData") : pd) + L"\\" + kAppName;
}

std::wstring ServicePath(const wchar_t* fileName) { return ServiceDataDir() + L"\\" + fileName; }

std::wstring GuiDataDir()
{
    if (!MachineInstall()) return ExeDir();
    std::wstring ad = KnownFolder(FOLDERID_RoamingAppData);
    if (ad.empty()) return ExeDir();
    std::wstring d = ad + L"\\" + kAppName;
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

std::wstring ImagesDir() { return ServiceDataDir() + L"\\Images"; }

bool ProtectDir(const std::wstring& dir)
{
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FRFX;;;BU)",
            SDDL_REVISION_1, &sd, nullptr))
        return false;
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted);
    DWORD r = SetNamedSecurityInfoW((LPWSTR)dir.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
        nullptr, nullptr, dacl, nullptr);
    LocalFree(sd);
    return r == ERROR_SUCCESS;
}

std::wstring Trim(const std::wstring& s)
{
    size_t a = 0, b = s.size();
    while (a < b && iswspace(s[a])) a++;
    while (b > a && iswspace(s[b - 1])) b--;
    return s.substr(a, b - a);
}

std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep)
{
    std::vector<std::wstring> r;
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t e = s.find(sep, pos);
        if (e == std::wstring::npos) e = s.size();
        std::wstring part = Trim(s.substr(pos, e - pos));
        if (!part.empty()) r.push_back(part);
        pos = e + 1;
    }
    return r;
}

std::wstring FormatSize(unsigned long long b)
{
    wchar_t buf[64];
    const double k = 1024.0;
    if (b < 1024) swprintf_s(buf, L"%llu B", b);
    else if (b < 1024ull * 1024) swprintf_s(buf, L"%.1f KB", b / k);
    else if (b < 1024ull * 1024 * 1024) swprintf_s(buf, L"%.1f MB", b / (k * k));
    else if (b < 1024ull * 1024 * 1024 * 1024) swprintf_s(buf, L"%.2f GB", b / (k * k * k));
    else swprintf_s(buf, L"%.2f TB", b / (k * k * k * k));
    std::wstring s = buf;
    if (!En()) for (auto& c : s) if (c == L'.') c = L',';
    return s;
}

bool ParseSize(const std::wstring& text, unsigned long long& out)
{
    std::wstring t = Lower(Trim(text));
    if (t.empty()) return false;
    for (auto& c : t) if (c == L',') c = L'.';
    wchar_t* end = nullptr;
    double v = wcstod(t.c_str(), &end);
    if (end == t.c_str() || v <= 0) return false;
    std::wstring unit = Trim(end);
    double mul;
    if (unit.empty() || unit == L"m" || unit == L"mb" || unit == L"mib") mul = 1024.0 * 1024;
    else if (unit == L"g" || unit == L"gb" || unit == L"gib") mul = 1024.0 * 1024 * 1024;
    else if (unit == L"t" || unit == L"tb" || unit == L"tib") mul = 1024.0 * 1024 * 1024 * 1024;
    else if (unit == L"k" || unit == L"kb" || unit == L"kib") mul = 1024.0;
    else if (unit == L"b") mul = 1.0;
    else return false;
    out = (unsigned long long)(v * mul);
    return true;
}

std::wstring U64(unsigned long long v) { return std::to_wstring(v); }

unsigned long long ToU64(const std::wstring& s, unsigned long long def)
{
    std::wstring t = Trim(s);
    if (t.empty()) return def;
    wchar_t* end = nullptr;
    unsigned long long v = wcstoull(t.c_str(), &end, 10);
    return end == t.c_str() ? def : v;
}

bool ToBool(const std::wstring& s, bool def)
{
    std::wstring t = Lower(Trim(s));
    if (t == L"1" || t == L"on" || t == L"true" || t == L"ja" || t == L"yes") return true;
    if (t == L"0" || t == L"off" || t == L"false" || t == L"nein" || t == L"no") return false;
    return def;
}

std::wstring Win32Message(DWORD code)
{
    wchar_t* msg = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    // zuerst in der eingestellten Sprache (falls das Sprachpaket fehlt: Windows-Standard)
    DWORD lang = En() ? MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US) : MAKELANGID(LANG_GERMAN, SUBLANG_GERMAN);
    DWORD n = FormatMessageW(flags, nullptr, code, lang, (LPWSTR)&msg, 0, nullptr);
    if (!n) n = FormatMessageW(flags, nullptr, code, 0, (LPWSTR)&msg, 0, nullptr);
    std::wstring s = n ? std::wstring(msg, n) : TrF(L"Fehler %lu", L"Error %lu", code);
    if (msg) LocalFree(msg);
    return Trim(s);
}

const wchar_t* CompressName(int level)
{
    switch (level) {
    case 1: return L"fast";
    case 2: return L"normal";
    case 3: return L"max";
    }
    return L"off";
}

int ParseCompress(const std::wstring& s, int def)
{
    std::wstring t = Lower(Trim(s));
    if (t.empty()) return def;
    if (t == L"off" || t == L"0" || t == L"aus" || t == L"none") return 0;
    if (t == L"fast" || t == L"1" || t == L"schnell" || t == L"lz4") return 1;
    if (t == L"normal" || t == L"2" || t == L"hc") return 2;
    if (t == L"max" || t == L"3" || t == L"stark" || t == L"strong" || t == L"high" || t == L"zstd") return 3;
    return -1;
}

bool IsDriveLetterUsed(wchar_t letter)
{
    DWORD mask = GetLogicalDrives();
    int idx = towupper(letter) - L'A';
    if (idx < 0 || idx > 25) return true;
    if (mask & (1u << idx)) return true;
    wchar_t dev[3] = { (wchar_t)towupper(letter), L':', 0 };
    wchar_t target[512];
    return QueryDosDeviceW(dev, target, (DWORD)std::size(target)) != 0;
}

// ---------------------------------------------------------------- INI

Ini::Ini(std::wstring p) : path(std::move(p))
{
    // UTF-16LE mit BOM anlegen, damit die Profile-API Unicode schreibt.
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const unsigned char bom[2] = { 0xFF, 0xFE };
            DWORD w;
            WriteFile(h, bom, 2, &w, nullptr);
            CloseHandle(h);
        }
    }
}

std::wstring Ini::Get(const wchar_t* sec, const wchar_t* key, const wchar_t* def) const
{
    std::vector<wchar_t> buf(4096);
    DWORD n = GetPrivateProfileStringW(sec, key, def, buf.data(), (DWORD)buf.size(), path.c_str());
    return std::wstring(buf.data(), n);
}

unsigned long long Ini::GetU64(const wchar_t* sec, const wchar_t* key, unsigned long long def) const
{
    return ToU64(Get(sec, key, L""), def);
}

void Ini::Set(const wchar_t* sec, const wchar_t* key, const std::wstring& val) const
{
    WritePrivateProfileStringW(sec, key, val.c_str(), path.c_str());
}

void Ini::DeleteSection(const wchar_t* sec) const
{
    WritePrivateProfileStringW(sec, nullptr, nullptr, path.c_str());
}

void Ini::DeleteKey(const wchar_t* sec, const wchar_t* key) const
{
    WritePrivateProfileStringW(sec, key, nullptr, path.c_str());
}

std::vector<std::wstring> Ini::Sections() const
{
    std::vector<wchar_t> buf(32768);
    DWORD n = GetPrivateProfileSectionNamesW(buf.data(), (DWORD)buf.size(), path.c_str());
    std::vector<std::wstring> r;
    for (DWORD i = 0; i < n;) {
        std::wstring s(&buf[i]);
        if (s.empty()) break;
        r.push_back(s);
        i += (DWORD)s.size() + 1;
    }
    return r;
}

} // namespace rdb
