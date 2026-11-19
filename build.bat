@echo off
rem RamDiskBox bauen: build\bin\RamDiskBox.exe, RamDiskBoxSvc.exe, rdbctl.exe
rem Voraussetzung: Visual Studio 2026 (v18) mit C++ und WinFsp mit Entwicklerdateien.
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
set "WINFSP=C:\Program Files (x86)\WinFsp"
if not exist "%WINFSP%\inc\winfsp\winfsp.h" (
   echo WinFsp-Entwicklerdateien fehlen: WinFsp mit "Developer" installieren.
   exit /b 1
)
for %%d in (build build\obj build\obj\svc build\obj\ctl build\obj\gui build\obj\setup build\bin) do if not exist %%d mkdir %%d

set "CFLAGS=/nologo /std:c++20 /O2 /EHsc /W3 /MT /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /Zc:__cplusplus /Zi"

rem ---- Dienst (mit LZ4, BSD-2, src\third_party\lz4) ----
rc /nologo /fo build\obj\svc\svc.res src\svc\svc.rc || exit /b 1
cl /nologo /c /O2 /W3 /MT /Zi /Fobuild\obj\svc\ /Fdbuild\obj\svc\ src\third_party\lz4\lz4.c src\third_party\lz4\lz4hc.c || exit /b 1
rem ---- zstd 1.5.7 (BSD-3, src\third_party\zstd), ohne Assembler und Legacy-Formate ----
if not exist build\obj\svc\zstd mkdir build\obj\svc\zstd
if not exist build\obj\svc\zstd\zstd_compress.obj (
   cl /nologo /c /O2 /W1 /MT /Zi /DZSTD_DISABLE_ASM /DZSTD_LEGACY_SUPPORT=0 /Fobuild\obj\svc\zstd\ /Fdbuild\obj\svc\zstd\ ^
      src\third_party\zstd\common\*.c src\third_party\zstd\compress\*.c src\third_party\zstd\decompress\*.c || exit /b 1
)
cl %CFLAGS% /I"%WINFSP%\inc" /Fobuild\obj\svc\ /Fdbuild\obj\svc\ /Fe"build\bin\RamDiskBoxSvc.exe" ^
   src\svc\main.cpp src\svc\host.cpp src\svc\ramfs.cpp src\svc\image.cpp src\svc\import.cpp src\svc\defender.cpp src\common\common.cpp ^
   build\obj\svc\lz4.obj build\obj\svc\lz4hc.obj build\obj\svc\zstd\*.obj build\obj\svc\svc.res ^
   /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /DEBUG /OPT:REF /OPT:ICF /PDB:build\obj\svc\RamDiskBoxSvc.pdb /LIBPATH:"%WINFSP%\lib" winfsp-x64.lib /DELAYLOAD:winfsp-x64.dll delayimp.lib ^
   advapi32.lib cabinet.lib user32.lib ole32.lib oleaut32.lib wbemuuid.lib || exit /b 1

rem ---- Kommandozeile ----
rc /nologo /fo build\obj\ctl\ctl.res src\ctl\ctl.rc || exit /b 1
cl %CFLAGS% /Fobuild\obj\ctl\ /Fdbuild\obj\ctl\ /Fe"build\bin\rdbctl.exe" src\ctl\main.cpp src\common\common.cpp build\obj\ctl\ctl.res ^
   /link /SUBSYSTEM:CONSOLE /MACHINE:X64 /DEBUG /OPT:REF /OPT:ICF /PDB:build\obj\ctl\rdbctl.pdb advapi32.lib user32.lib || exit /b 1

rem ---- Oberflaeche ----
if exist src\gui\gui.cpp (
   rc /nologo /fo build\obj\gui\gui.res src\gui\gui.rc || exit /b 1
   cl %CFLAGS% /Fobuild\obj\gui\ /Fdbuild\obj\gui\ /Fe"build\bin\RamDiskBox.exe" src\gui\gui.cpp src\gui\dialogs.cpp src\gui\lang.cpp src\gui\theme.cpp src\common\common.cpp build\obj\gui\gui.res ^
      /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /MANIFEST:NO /DEBUG /OPT:REF /OPT:ICF /PDB:build\obj\gui\RamDiskBox.pdb user32.lib gdi32.lib comctl32.lib shell32.lib comdlg32.lib ^
      advapi32.lib ole32.lib uxtheme.lib dwmapi.lib shlwapi.lib uuid.lib || exit /b 1
)

copy /Y README.de.md build\bin\LIESMICH.md >nul 2>nul

rem ---- Setup: enthaelt die eben gebauten Programme als Ressourcen -> build\RamDiskBox-Setup.exe ----
rc /nologo /fo build\obj\setup\setup.res src\setup\setup.rc || exit /b 1
cl %CFLAGS% /Fobuild\obj\setup\ /Fdbuild\obj\setup\ /Fe"build\RamDiskBox-Setup.exe" src\setup\setup.cpp src\gui\theme.cpp src\common\common.cpp build\obj\setup\setup.res ^
   /link /SUBSYSTEM:WINDOWS /MACHINE:X64 /MANIFEST:NO /DEBUG /OPT:REF /OPT:ICF /PDB:build\obj\setup\RamDiskBox-Setup.pdb user32.lib gdi32.lib comctl32.lib ^
   shell32.lib ole32.lib advapi32.lib uxtheme.lib dwmapi.lib shlwapi.lib uuid.lib || exit /b 1

echo BUILD OK: %~dp0build\bin und %~dp0build\RamDiskBox-Setup.exe
