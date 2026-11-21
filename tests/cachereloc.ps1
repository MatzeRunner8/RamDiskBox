# Test "Cache verlagern" an einer nachgebauten Umgebung (RDB_CACHE_TEST_ROOT), nie an echten Browser-Caches.
# Testinstanz RDB_INSTANCE=dev mit Laufwerk Y:. Bedient den Dialog per Fensternachrichten (helles Design,
# damit Meldungen normale MessageBoxen sind). Prueft: Sperre bei laufendem Programm, Verschieben (Junctions
# in eigene Unterordner, Ordnerliste, alter Inhalt weg, gemeinsamer Fremdordner und Fallen-Junction unberuehrt),
# Wiederherstellen (normale leere Ordner, eigene Ziele entfernt).
param([string]$RunningExe = 'chrome.exe')   # ein Programm, das beim Test laeuft (fuer die Sperre)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$bin = "$root\build\bin"; $ctl = "$bin\rdbctl.exe"
$T = "$root\build\cachetest"
$env:RDB_INSTANCE = 'dev'; $env:RDB_LANG = 'de'; $env:RDB_CACHE_TEST_ROOT = $T
Add-Type @"
using System; using System.Text; using System.Runtime.InteropServices;
public class CT {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h, int id);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetDlgItemTextW(IntPtr h, int id, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowExW(IntPtr p, IntPtr after, string c, string t);
  [DllImport("user32.dll")] public static extern int GetDlgCtrlID(IntPtr h);
}
"@
$fails = 0
function Check($name, $cond) { if ($cond) { "OK     $name" } else { "FEHLER $name"; $script:fails++ } }
function LinkOf($p) { $i = Get-Item -LiteralPath $p -Force -ErrorAction SilentlyContinue; if ($i -and $i.LinkType) { "$($i.LinkType)>$($i.Target)" } elseif ($i) { 'Ordner' } else { 'fehlt' } }
function Wait-Box([int]$ms = 8000) {
    for ($k = 0; $k -lt $ms / 100; $k++) {
        $b = [CT]::FindWindowW('#32770', 'RamDiskBox')
        if ($b -ne [IntPtr]::Zero) { $sb = New-Object Text.StringBuilder 2048; [void][CT]::GetDlgItemTextW($b, 65535, $sb, 2048); return @($b, $sb.ToString()) }
        Start-Sleep -Milliseconds 100
    }
    return @([IntPtr]::Zero, '')
}
function Wait-Gone($h) { for ($k = 0; $k -lt 100 -and $h -ne [IntPtr]::Zero -and [CT]::IsWindow($h); $k++) { Start-Sleep -Milliseconds 100 } }
# OK-Knopf ueber die Beschriftung suchen: bei Meldungen nur mit "OK" hat er die Kennung 2 (IDCANCEL), sonst 1
function Press-Ok($box) {
    if ($box -eq [IntPtr]::Zero) { return }
    $btn = [CT]::FindWindowExW($box, [IntPtr]::Zero, 'Button', 'OK')
    $id = if ($btn -ne [IntPtr]::Zero) { [CT]::GetDlgCtrlID($btn) } else { 1 }
    [CT]::PostMessageW($box, 0x0111, [IntPtr]$id, $btn) | Out-Null
    Wait-Gone $box; Start-Sleep -Milliseconds 200
}
function Click($dlg, [int]$id) { [CT]::PostMessageW($dlg, 0x0111, [IntPtr]$id, [CT]::GetDlgItem($dlg, $id)) | Out-Null }
function Close-Dialog($dlg) { Click $dlg 2; Wait-Gone $dlg; Start-Sleep -Milliseconds 300 }

# --- Testumgebung
if (Test-Path $T) { cmd /c rmdir /s /q "$T" }
$L = "$T\Local"; $R = "$T\Roaming"
New-Item -ItemType Directory -Force "$T\SharedTemp", "$T\Precious" | Out-Null
'gemeinsam' | Set-Content "$T\SharedTemp\precious.txt"; 'nicht loeschen' | Set-Content "$T\Precious\keep.txt"
# Edge: Cache ist (wie bei Cache Relocator) eine Verknuepfung auf einen gemeinsamen Ordner
New-Item -ItemType Directory -Force "$L\Microsoft\Edge\User Data\Default" | Out-Null; '{}' | Set-Content "$L\Microsoft\Edge\User Data\Default\Preferences"
cmd /c mklink /J "$L\Microsoft\Edge\User Data\Default\Cache" "$T\SharedTemp" | Out-Null
# Brave: normaler Cache mit Dateien, schreibgeschuetzter Datei und einer Falle (Junction nach aussen)
New-Item -ItemType Directory -Force "$L\BraveSoftware\Brave-Browser\User Data\Default\Cache\Cache_Data\sub" | Out-Null
'{}' | Set-Content "$L\BraveSoftware\Brave-Browser\User Data\Default\Preferences"
1..20 | ForEach-Object { [IO.File]::WriteAllBytes("$L\BraveSoftware\Brave-Browser\User Data\Default\Cache\Cache_Data\f_$_", (New-Object byte[] 4096)) }
$ro = "$L\BraveSoftware\Brave-Browser\User Data\Default\Cache\Cache_Data\index"; 'x' | Set-Content $ro; (Get-Item $ro).IsReadOnly = $true
cmd /c mklink /J "$L\BraveSoftware\Brave-Browser\User Data\Default\Cache\Cache_Data\sub\trap" "$T\Precious" | Out-Null
New-Item -ItemType Directory -Force "$L\BraveSoftware\Brave-Browser\User Data\System Profile" | Out-Null; '{}' | Set-Content "$L\BraveSoftware\Brave-Browser\User Data\System Profile\Preferences"
New-Item -ItemType Directory -Force "$L\BraveSoftware\Brave-Browser\User Data\Profile 1" | Out-Null; '{}' | Set-Content "$L\BraveSoftware\Brave-Browser\User Data\Profile 1\Preferences"
# VS Code und Firefox
New-Item -ItemType Directory -Force "$R\Code\Cache\Cache_Data", "$L\Mozilla\Firefox\Profiles\abc.default\cache2" | Out-Null
'a' | Set-Content "$R\Code\Cache\Cache_Data\data_0"; 'b' | Set-Content "$L\Mozilla\Firefox\Profiles\abc.default\cache2\entry"
# Chrome nur fuer die Sperre (laeuft beim Test)
New-Item -ItemType Directory -Force "$L\Google\Chrome\User Data\Default\Cache" | Out-Null; '{}' | Set-Content "$L\Google\Chrome\User Data\Default\Preferences"

# --- Testinstanz und Oberflaeche
if (Test-Path Y:\) { throw "Y: belegt" }
if ([CT]::FindWindowW("RamDiskBoxMainWnd.dev", [NullString]::Value) -ne [IntPtr]::Zero) { throw "Test-Oberflaeche laeuft schon" }
$h = Start-Process "$bin\RamDiskBoxSvc.exe" -ArgumentList run -WindowStyle Hidden -PassThru; Start-Sleep -Seconds 2
& $ctl create Y 1G label=RDBCacheTest noauto nosave folders=Temp | Out-Null
Start-Sleep -Seconds 1
if (((cmd /c vol Y:) -join ' ') -notmatch 'RDBCacheTest') { throw "Y: ist nicht das Testlaufwerk" }
"[GUI]`r`nLanguage=de`r`nTheme=light`r`nTrayClose=0`r`nStartMinimized=0`r`n" | Set-Content -Encoding Unicode "$bin\RamDiskBoxGui.ini"
$g = Start-Process "$bin\RamDiskBox.exe" -PassThru
for ($k = 0; $k -lt 50 -and ([CT]::FindWindowW("RamDiskBoxMainWnd.dev", [NullString]::Value)) -eq [IntPtr]::Zero; $k++) { Start-Sleep -Milliseconds 200 }
$main = [CT]::FindWindowW("RamDiskBoxMainWnd.dev", [NullString]::Value)
Start-Sleep -Seconds 3
function Open-Dialog {
    [CT]::PostMessageW($main, 0x0111, [IntPtr]40021, [IntPtr]::Zero) | Out-Null
    for ($k = 0; $k -lt 50; $k++) { $d = [CT]::FindWindowW('#32770', 'Cache verlagern'); if ($d -ne [IntPtr]::Zero) { Start-Sleep -Milliseconds 800; return $d }; Start-Sleep -Milliseconds 200 }
    throw "Dialog nicht gefunden"
}
try {
    # 1) Sperre: Chrome laeuft -> nichts darf passieren
    $running = @(Get-Process -Name ($RunningExe -replace '\.exe$', '') -ErrorAction SilentlyContinue).Count -gt 0
    $dlg = Open-Dialog
    Click $dlg 3107
    $box = Wait-Box
    if ($running) {
        "INFO   Box: [$($box[0])] $($box[1] -replace "`r?`n", ' | ')"
        Check "Sperre meldet laufendes Programm" ($box[1] -match 'schlie' -and $box[1] -match 'Chrome')
        Press-Ok $box[0]
        Check "Sperre: Edge unveraendert" ((LinkOf "$L\Microsoft\Edge\User Data\Default\Cache") -match 'SharedTemp')
        Check "Sperre: Brave unveraendert" ((LinkOf "$L\BraveSoftware\Brave-Browser\User Data\Default\Cache") -eq 'Ordner')
    } else { "INFO   $RunningExe laeuft nicht, Sperre nicht geprueft"; if ($box[0] -ne [IntPtr]::Zero) { [CT]::PostMessageW($box[0], 0x0111, [IntPtr]2, [IntPtr]::Zero) | Out-Null } }
    $dlgOld = $dlg
    Close-Dialog $dlg

    # 2) Verschieben ohne Chrome
    cmd /c rmdir /s /q "$L\Google"
    if (Test-Path "$L\Google") { "INFO   Chrome-Testordner blieb: " + ((Get-ChildItem "$L\Google" -Recurse -Force | ForEach-Object { $_.FullName.Substring($L.Length) }) -join ', ') }
    $dlg = Open-Dialog
    "INFO   Dialog alt/neu: $dlgOld / $dlg"
    Click $dlg 3107
    $box = Wait-Box; Check "Rueckfrage vor dem Verschieben" ($box[1] -match 'gel.scht'); Press-Ok $box[0]
    $box = Wait-Box 15000; "INFO   Ergebnis: $($box[1] -replace "`r?`n", ' | ')"; Press-Ok $box[0]
    Check "Edge -> eigener Ordner" ((LinkOf "$L\Microsoft\Edge\User Data\Default\Cache") -eq 'Junction>Y:\Cache\Edge-Default')
    Check "Brave -> eigener Ordner" ((LinkOf "$L\BraveSoftware\Brave-Browser\User Data\Default\Cache") -eq 'Junction>Y:\Cache\Brave-Default')
    Check "Brave Profil 1 (noch ohne Cache) -> eigener Ordner" ((LinkOf "$L\BraveSoftware\Brave-Browser\User Data\Profile 1\Cache") -eq 'Junction>Y:\Cache\Brave-Profile 1')
    Check "System Profile nicht angefasst" ((LinkOf "$L\BraveSoftware\Brave-Browser\User Data\System Profile\Cache") -eq 'fehlt')
    Check "VS Code -> eigener Ordner" ((LinkOf "$R\Code\Cache") -eq 'Junction>Y:\Cache\VSCode')
    Check "Firefox -> eigener Ordner" ((LinkOf "$L\Mozilla\Firefox\Profiles") -eq 'Junction>Y:\Cache\Firefox')
    Check "Gemeinsamer Fremdordner unberuehrt" (Test-Path "$T\SharedTemp\precious.txt")
    Check "Falle (Junction im Cache) nicht verfolgt" (Test-Path "$T\Precious\keep.txt")
    Check "Alter Brave-Cache geloescht" (-not (Test-Path "Y:\Cache\Brave-Default\Cache_Data"))
    'probe' | Set-Content "$R\Code\Cache\probe.txt"
    Check "Schreiben ueber die Verbindung landet auf Y:" (Test-Path "Y:\Cache\VSCode\probe.txt")
    $folders = ((& $ctl list) | Select-String 'Ordner:' | Select-Object -First 1).Line
    Check "Ordnerliste enthaelt die Ziele ($folders)" ($folders -match 'Cache\\Edge-Default' -and $folders -match 'Cache\\VSCode' -and $folders -match 'Temp')
    & $ctl dismount Y force | Out-Null; & $ctl mount Y | Out-Null; Start-Sleep -Seconds 1
    Check "Nach erneutem Einhaengen sind die Ziele wieder da" ((Test-Path 'Y:\Cache\Edge-Default') -and (Test-Path 'Y:\Cache\Firefox'))

    # 3) Wiederherstellen
    Click $dlg 3101; Start-Sleep -Milliseconds 200
    Click $dlg 3109; Start-Sleep -Seconds 1   # Aktualisieren: alles neu, im Test wieder alles ausgewaehlt
    Click $dlg 3108
    $box = Wait-Box; Check "Rueckfrage vor dem Wiederherstellen" ($box[1] -match 'Umleitung'); Press-Ok $box[0]
    $box = Wait-Box 15000; "INFO   Ergebnis: $($box[1] -replace "`r?`n", ' | ')"; Press-Ok $box[0]
    foreach ($p in "$L\Microsoft\Edge\User Data\Default\Cache", "$L\BraveSoftware\Brave-Browser\User Data\Default\Cache", "$R\Code\Cache", "$L\Mozilla\Firefox\Profiles") {
        Check "Wieder normaler Ordner: $($p.Substring($T.Length))" ((LinkOf $p) -eq 'Ordner')
    }
    Check "Eigene Ziele entfernt" (-not (Test-Path 'Y:\Cache\Edge-Default') -and -not (Test-Path 'Y:\Cache\VSCode'))
    $folders = ((& $ctl list) | Select-String 'Ordner:' | Select-Object -First 1).Line
    Check "Ordnerliste wieder nur Temp ($folders)" ($folders -match 'Temp' -and $folders -notmatch 'Cache\\')
    Check "Gemeinsamer Fremdordner weiter unberuehrt" (Test-Path "$T\SharedTemp\precious.txt")
    Check "Falle weiter unberuehrt" (Test-Path "$T\Precious\keep.txt")
    Click $dlg 2
} finally {
    Start-Sleep -Milliseconds 500
    $b = [CT]::FindWindowW('#32770', 'RamDiskBox'); if ($b -ne [IntPtr]::Zero) { [CT]::PostMessageW($b, 0x0111, [IntPtr]2, [IntPtr]::Zero) | Out-Null }
    $d = [CT]::FindWindowW('#32770', 'Cache verlagern'); if ($d -ne [IntPtr]::Zero) { [CT]::PostMessageW($d, 0x0111, [IntPtr]2, [IntPtr]::Zero) | Out-Null }
    Start-Sleep -Milliseconds 500
    [CT]::PostMessageW($main, 0x0111, [IntPtr]40017, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Seconds 1
    if (-not $g.HasExited) { Stop-Process -Id $g.Id -Force }
    & $ctl delete Y force discard | Out-Null
    & $ctl quit 2>$null | Out-Null
    Start-Sleep -Seconds 1
    if (-not $h.HasExited) { Stop-Process -Id $h.Id -Force }
    # Junctions zuerst einzeln entfernen, damit rmdir /s nie in Ziele laeuft
    Get-ChildItem $T -Recurse -Force -Attributes ReparsePoint -ErrorAction SilentlyContinue | ForEach-Object { cmd /c rmdir "$($_.FullName)" }
    cmd /c rmdir /s /q "$T"
    foreach ($f in 'RamDiskBox.ini', 'RamDiskBoxGui.ini', 'RamDiskBox.log') { $p = Join-Path $bin $f; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force } }
}
"Fehler gesamt: $fails"
