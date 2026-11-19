// RamDiskBoxSvc.exe - Dienst bzw. portabler Hintergrundprozess von RamDiskBox
//
//   RamDiskBoxSvc.exe service    (wird von der Dienstverwaltung gestartet)
//   RamDiskBoxSvc.exe run        (portabel, ohne Dienst; läuft bis QUIT)
//   RamDiskBoxSvc.exe install    (Dienst einrichten und starten, Admin)
//   RamDiskBoxSvc.exe uninstall  (Dienst beenden und entfernen, Admin)
#include "host.h"
#include "../common/version.h"
#include <cstdio>
#include <cstdarg>
#include <io.h>
#include <fcntl.h>
#include <sddl.h>

using namespace rdb;

static Host g_host;
static SERVICE_STATUS_HANDLE g_ssh = nullptr;
static SERVICE_STATUS g_ss{};
static HANDLE g_svcStop = nullptr;

// Konsolenausgabe: printf-Format je Sprache (Deutsch, Englisch)
static void Out(const wchar_t* de, const wchar_t* en, ...)
{
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, en);
    _vsnwprintf_s(buf, _TRUNCATE, En() ? en : de, ap);
    va_end(ap);
    DWORD w;
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(h, &w)) WriteConsoleW(h, buf, (DWORD)wcslen(buf), &w, nullptr);
    else {
        int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
        std::string u(n > 0 ? n - 1 : 0, '\0');
        if (n > 1) WideCharToMultiByte(CP_UTF8, 0, buf, -1, u.data(), n, nullptr, nullptr);
        WriteFile(h, u.data(), (DWORD)u.size(), &w, nullptr);
    }
}

static bool LoadWinFsp(std::wstring& err)
{
    if (NT_SUCCESS(FspLoad(nullptr))) return true;
    err = Tr(L"WinFsp ist nicht installiert. Bitte WinFsp von https://winfsp.dev installieren.",
             L"WinFsp is not installed. Please install WinFsp from https://winfsp.dev.");
    return false;
}

// ----------------------------------------------------------------------- Dienst

static void ReportStatus(DWORD state, DWORD exitCode = NO_ERROR, DWORD waitHint = 0)
{
    static DWORD checkpoint = 1;
    g_ss.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_ss.dwCurrentState = state;
    g_ss.dwWin32ExitCode = exitCode;
    g_ss.dwWaitHint = waitHint;
    g_ss.dwControlsAccepted = state == SERVICE_RUNNING ? (SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_PRESHUTDOWN) : 0;
    g_ss.dwCheckPoint = (state == SERVICE_RUNNING || state == SERVICE_STOPPED) ? 0 : checkpoint++;
    if (g_ssh) SetServiceStatus(g_ssh, &g_ss);
}

static DWORD WINAPI CtrlHandler(DWORD ctrl, DWORD, LPVOID, LPVOID)
{
    switch (ctrl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_PRESHUTDOWN:
        ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
        SetEvent(g_svcStop);
        return NO_ERROR;
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI ServiceMain(DWORD, LPWSTR*)
{
    g_ssh = RegisterServiceCtrlHandlerExW(kServiceName, CtrlHandler, nullptr);
    if (!g_ssh) return;
    ReportStatus(SERVICE_START_PENDING, NO_ERROR, 30000);
    g_svcStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::wstring err;
    if (!LoadWinFsp(err) || !g_host.Start(true, err)) {
        Log(L"Dienststart fehlgeschlagen: %s", L"Service start failed: %s", err.c_str());
        ReportStatus(SERVICE_STOPPED, ERROR_SERVICE_SPECIFIC_ERROR);
        return;
    }
    ReportStatus(SERVICE_RUNNING);
    WaitForSingleObject(g_svcStop, INFINITE);
    // Während des Speicherns regelmäßig Fortschritt melden.
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread pinger([done] {
        while (WaitForSingleObject(done, 2000) == WAIT_TIMEOUT) ReportStatus(SERVICE_STOP_PENDING, NO_ERROR, 30000);
    });
    g_host.Stop();
    SetEvent(done);
    pinger.join();
    ReportStatus(SERVICE_STOPPED);
}

// ----------------------------------------------------------------------- portabel

static LRESULT CALLBACK HiddenWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_QUERYENDSESSION:
        ShutdownBlockReasonCreate(hwnd, Tr(L"RamDiskBox speichert die RAM-Laufwerke ...", L"RamDiskBox is saving the RAM drives ..."));
        return TRUE;
    case WM_ENDSESSION:
        if (wp) {
            g_host.Stop();
            ShutdownBlockReasonDestroy(hwnd);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static BOOL WINAPI ConsoleCtrl(DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        SetEvent(g_host.StopEvent());
        return TRUE;
    }
    return FALSE;
}

static int RunPortable()
{
    std::wstring err;
    if (!LoadWinFsp(err) || !g_host.Start(false, err)) {
        Log(L"Start (portabel) fehlgeschlagen: %s", L"Start (portable) failed: %s", err.c_str());
        Out(L"Fehler: %s\n", L"Error: %s\n", err.c_str());
        return 1;
    }
    SetConsoleCtrlHandler(ConsoleCtrl, TRUE);
    SetProcessShutdownParameters(0x3FF, 0);  // möglichst früh benachrichtigt werden
    WNDCLASSW wc{};
    wc.lpfnWndProc = HiddenWndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RamDiskBoxHostWindow";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, L"RamDiskBox Host", 0, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
    (void)hwnd;
    Out(L"RamDiskBox %s läuft (portabel). Beenden mit Strg+C.\n",
        L"RamDiskBox %s is running (portable). Press Ctrl+C to quit.\n", RDB_VERSION_W);
    HANDLE stop = g_host.StopEvent();
    for (;;) {
        DWORD w = MsgWaitForMultipleObjects(1, &stop, FALSE, INFINITE, QS_ALLINPUT);
        if (w == WAIT_OBJECT_0) break;
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    g_host.Stop();
    return 0;
}

// ----------------------------------------------------------------------- Einrichtung

static bool WaitServiceState(SC_HANDLE svc, DWORD want, DWORD timeoutMs)
{
    SERVICE_STATUS ss;
    unsigned long long end = GetTickCount64() + timeoutMs;
    while (QueryServiceStatus(svc, &ss)) {
        if (ss.dwCurrentState == want) return true;
        if (GetTickCount64() > end) return false;
        Sleep(250);
    }
    return false;
}

static int Install()
{
    std::wstring err;
    if (!LoadWinFsp(err)) { Out(L"%s\n", L"%s\n", err.c_str()); return 2; }
    // Läuft eine portable Instanz, diese zuerst geordnet beenden (sie speichert ihre Images).
    Message ping, resp;
    ping.cmd = L"PING";
    if (PipeCall(ping, resp, nullptr, 1500) && resp.Get(L"mode") == L"portable") {
        Out(L"Beende die portable Instanz ...\n", L"Stopping the portable instance ...\n");
        Message q;
        q.cmd = L"QUIT";
        PipeCall(q, resp, nullptr, 5000);
        for (int i = 0; i < 600 && ServiceReachable(); i++) Sleep(500);
    }
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CREATE_SERVICE | SC_MANAGER_CONNECT);
    if (!scm) { Out(L"Dienstverwaltung: %s\n", L"Service manager: %s\n", Win32Message(GetLastError()).c_str()); return 3; }
    std::wstring bin = L"\"" + ExePath(kSvcExe) + L"\" service";
    SC_HANDLE svc = CreateServiceW(scm, kServiceName, kServiceDisplay, SERVICE_ALL_ACCESS, SERVICE_WIN32_OWN_PROCESS,
        SERVICE_AUTO_START, SERVICE_ERROR_NORMAL, bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!svc && GetLastError() == ERROR_SERVICE_EXISTS) {
        svc = OpenServiceW(scm, kServiceName, SERVICE_ALL_ACCESS);
        if (svc) ChangeServiceConfigW(svc, SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
            bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, kServiceDisplay);
    }
    if (!svc) {
        Out(L"Dienst konnte nicht angelegt werden: %s\n", L"Could not create the service: %s\n", Win32Message(GetLastError()).c_str());
        CloseServiceHandle(scm);
        return 4;
    }
    // Angemeldete Benutzer dürfen den Dienst starten (Oberfläche ohne Adminrechte).
    {
        PSECURITY_DESCRIPTOR sd = nullptr;
        if (ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)",
                SDDL_REVISION_1, &sd, nullptr)) {
            SetServiceObjectSecurity(svc, DACL_SECURITY_INFORMATION, sd);
            LocalFree(sd);
        }
    }
    // Beschreibung in der Sprache dessen, der den Dienst einrichtet
    SERVICE_DESCRIPTIONW desc{ (LPWSTR)Tr(L"Stellt die RAM-Laufwerke von RamDiskBox bereit und sichert sie beim Herunterfahren.",
                                          L"Provides the RamDiskBox RAM drives and saves them at shutdown.") };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_DESCRIPTION, &desc);
    SERVICE_PRESHUTDOWN_INFO pre{ 300000 };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_PRESHUTDOWN_INFO, &pre);
    SC_ACTION acts[2] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 30000 } };
    SERVICE_FAILURE_ACTIONSW fa{ 86400, nullptr, nullptr, 2, acts };
    ChangeServiceConfig2W(svc, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);
    SERVICE_STATUS ss;
    if (QueryServiceStatus(svc, &ss) && ss.dwCurrentState != SERVICE_RUNNING) {
        if (!StartServiceW(svc, 0, nullptr) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) {
            Out(L"Dienst eingerichtet, Start fehlgeschlagen: %s\n", L"Service installed, but it could not be started: %s\n",
                Win32Message(GetLastError()).c_str());
            CloseServiceHandle(svc);
            CloseServiceHandle(scm);
            return 5;
        }
        WaitServiceState(svc, SERVICE_RUNNING, 60000);
    }
    Out(L"Dienst \"%s\" ist eingerichtet und läuft.\n", L"Service \"%s\" is installed and running.\n", kServiceName);
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return 0;
}

static int Uninstall()
{
    SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { Out(L"Dienstverwaltung: %s\n", L"Service manager: %s\n", Win32Message(GetLastError()).c_str()); return 3; }
    SC_HANDLE svc = OpenServiceW(scm, kServiceName, SERVICE_STOP | SERVICE_QUERY_STATUS | DELETE);
    if (!svc) {
        DWORD e = GetLastError();
        CloseServiceHandle(scm);
        if (e == ERROR_SERVICE_DOES_NOT_EXIST) { Out(L"Der Dienst ist nicht eingerichtet.\n", L"The service is not installed.\n"); return 0; }
        Out(L"Dienst: %s\n", L"Service: %s\n", Win32Message(e).c_str());
        return 4;
    }
    SERVICE_STATUS ss;
    if (ControlService(svc, SERVICE_CONTROL_STOP, &ss) || GetLastError() == ERROR_SERVICE_NOT_ACTIVE) {
        Out(L"Dienst wird beendet (Images werden gespeichert) ...\n", L"Stopping the service (saving images) ...\n");
        WaitServiceState(svc, SERVICE_STOPPED, 600000);
    }
    BOOL ok = DeleteService(svc);
    DWORD e = GetLastError();
    CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    if (!ok && e != ERROR_SERVICE_MARKED_FOR_DELETE) {
        Out(L"Entfernen fehlgeschlagen: %s\n", L"Removal failed: %s\n", Win32Message(e).c_str());
        return 5;
    }
    Out(L"Dienst \"%s\" wurde entfernt.\n", L"Service \"%s\" has been removed.\n", kServiceName);
    return 0;
}

int wmain(int argc, wchar_t** argv)
{
    std::wstring cmd = argc > 1 ? Lower(argv[1]) : L"";
    if (cmd == L"service") {
        SERVICE_TABLE_ENTRYW table[] = { { (LPWSTR)kServiceName, ServiceMain }, { nullptr, nullptr } };
        return StartServiceCtrlDispatcherW(table) ? 0 : 1;
    }
    if (cmd == L"run") return RunPortable();
    if (cmd == L"install") return Install();
    if (cmd == L"uninstall") return Uninstall();
    Out(L"RamDiskBoxSvc %s (Build %s)\n\n"
        L"  RamDiskBoxSvc install    Dienst einrichten und starten (Administrator)\n"
        L"  RamDiskBoxSvc uninstall  Dienst beenden und entfernen (Administrator)\n"
        L"  RamDiskBoxSvc run        ohne Dienst im Hintergrund laufen (portabel)\n\n"
        L"Bedienung: %s (Oberfläche) oder rdbctl.exe (Kommandozeile).\n",
        L"RamDiskBoxSvc %s (build %s)\n\n"
        L"  RamDiskBoxSvc install    install and start the service (administrator)\n"
        L"  RamDiskBoxSvc uninstall  stop and remove the service (administrator)\n"
        L"  RamDiskBoxSvc run        run in the background without a service (portable)\n\n"
        L"Use %s (user interface) or rdbctl.exe (command line).\n",
        RDB_VERSION_W, RDB_BUILD_W, kGuiExe);
    return argc > 1 ? 1 : 0;
}
