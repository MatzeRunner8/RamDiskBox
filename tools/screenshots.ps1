# Bildschirmfotos fuer die READMEs: Testinstanz (RDB_INSTANCE=dev) mit Beispiel-Laufwerken X:, Y:, Z:
# (nur freie Buchstaben; ohne persoenliche Pfade). Aufruf: tools\screenshots.ps1 [-Out Ordner] [-Setup]
param([string]$Out = "", [switch]$Setup, [string[]]$Langs = @('de', 'en'))
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $Out) { $Out = "$root\docs\screenshots" }
$bin = "$root\build\bin"
$ctl = "$bin\rdbctl.exe"
$env:RDB_INSTANCE = 'dev'
$env:RDB_LANG = 'de'
New-Item -ItemType Directory -Force $Out | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class RS {
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindowW(string c, string t);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int a, out RECT r, int size);
  [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
}
"@
[RS]::SetProcessDPIAware() | Out-Null

function Shot([IntPtr]$h, [string]$file) {
    [RS]::SetWindowPos($h, [IntPtr](-1), 0, 0, 0, 0, 0x0003) | Out-Null
    [RS]::SetForegroundWindow($h) | Out-Null
    Start-Sleep -Milliseconds 800
    $r = New-Object RS+RECT
    [RS]::DwmGetWindowAttribute($h, 9, [ref]$r, 16) | Out-Null
    $w = $r.R - $r.L; $hh = $r.B - $r.T
    $bmp = New-Object System.Drawing.Bitmap $w, $hh
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.CopyFromScreen($r.L, $r.T, 0, 0, (New-Object System.Drawing.Size $w, $hh))
    $g.Dispose()
    [RS]::SetWindowPos($h, [IntPtr](-2), 0, 0, 0, 0, 0x0003) | Out-Null
    $bmp.Save($file, [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
    "$file ($w x $hh)"
}
function MainWnd { [RS]::FindWindowW("RamDiskBoxMainWnd.dev", [NullString]::Value) }
function WaitWnd($cls, $title) { for ($i = 0; $i -lt 40; $i++) { $h = [RS]::FindWindowW($cls, $title); if ($h -ne [IntPtr]::Zero) { return $h }; Start-Sleep -Milliseconds 150 }; [IntPtr]::Zero }
function StartGui($lang, $theme) {
    $m = MainWnd
    if ($m -ne [IntPtr]::Zero) { [RS]::PostMessageW($m, 0x0111, [IntPtr]40017, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 900 }
    $text = "[GUI]`r`nLanguage=$lang`r`nTheme=$theme`r`nTrayClose=0`r`nStartMinimized=0`r`n[Window]`r`nPos=80,80,1725,885,0`r`n"
    [IO.File]::WriteAllText((Join-Path $bin "RamDiskBoxGui.ini"), $text, [Text.Encoding]::Unicode)
    $env:RDB_LANG = $lang
    Start-Process (Join-Path $bin "RamDiskBox.exe")
    for ($i = 0; $i -lt 50 -and (MainWnd) -eq [IntPtr]::Zero; $i++) { Start-Sleep -Milliseconds 200 }
    Start-Sleep -Seconds 3
}

# Hintergrundprozess und Beispiel-Laufwerke
foreach ($l in 'X', 'Y', 'Z') { if ((Get-PSDrive -PSProvider FileSystem).Name -contains $l) { throw "$($l): ist belegt" } }
& $ctl quit 2>$null | Out-Null; Start-Sleep -Seconds 2
Start-Process "$bin\RamDiskBoxSvc.exe" -ArgumentList run -WindowStyle Hidden; Start-Sleep -Seconds 2
& $ctl create X 4G label=RAMDisk folders=Temp compress=max noload nosave noauto "image=C:\RAMDisk\X.img" | Out-Null
& $ctl update X load save auto | Out-Null
& $ctl create Y 2G label=Cache compress=fast noauto nosave | Out-Null
& $ctl create Z 1G label=Build noauto nosave nomount | Out-Null
if (((cmd /c vol X:) -join ' ') -notmatch 'RAMDisk' -or ((cmd /c vol Y:) -join ' ') -notmatch 'Cache') { throw "Beispiel-Laufwerke nicht eindeutig" }
$sb = New-Object Text.StringBuilder; $i = 0
while ($sb.Length -lt 1MB) { [void]$sb.AppendFormat("2026-11-19 10:{0:00}:{1:00} INFO Build step {2} finished in {3} ms`r`n", ($i / 60) % 60, $i % 60, $i % 97, 100 + $i % 900); $i++ }
$chunk = [Text.Encoding]::ASCII.GetBytes($sb.ToString(0, 1MB))
New-Item -ItemType Directory -Force X:\Temp\logs | Out-Null
$fs = [IO.File]::Create("X:\Temp\logs\build.log"); for ($k = 0; $k -lt 180; $k++) { $fs.Write($chunk, 0, $chunk.Length) }; $fs.Close()
$rnd = New-Object Random 7; $b = New-Object byte[] (1MB)
$fs = [IO.File]::Create("Y:\cache.bin"); for ($k = 0; $k -lt 60; $k++) { $rnd.NextBytes($b); $fs.Write($b, 0, $b.Length) }; $fs.Close()
$fs = [IO.File]::Create("Y:\index.log"); for ($k = 0; $k -lt 90; $k++) { $fs.Write($chunk, 0, $chunk.Length) }; $fs.Close()
Start-Sleep -Seconds 8

foreach ($lang in $Langs) {
    StartGui $lang 'dark'
    Shot (MainWnd) "$Out\main-dark-$lang.png"
    [RS]::PostMessageW((MainWnd), 0x0111, [IntPtr]40002, [IntPtr]0) | Out-Null
    $t = if ($lang -eq 'de') { "RAM-Laufwerk X: bearbeiten" } else { "Edit RAM drive X:" }
    $d = WaitWnd "#32770" $t
    if ($d -ne [IntPtr]::Zero) { Shot $d "$Out\drive-dark-$lang.png"; [RS]::PostMessageW($d, 0x0111, [IntPtr]2, [IntPtr]0) | Out-Null; Start-Sleep -Milliseconds 500 }
    StartGui $lang 'light'
    Shot (MainWnd) "$Out\main-light-$lang.png"
    if ($Setup) {
        $p = Start-Process "$root\build\RamDiskBox-Setup.exe" -PassThru
        $ver = (Get-Item "$root\build\RamDiskBox-Setup.exe").VersionInfo
        $v = "$($ver.FileMajorPart).$($ver.FileMinorPart).$($ver.FileBuildPart)"
        $t = if ($lang -eq 'de') { "RamDiskBox $v $([char]0x2013) Setup" } else { "RamDiskBox $v Setup" }
        $d = WaitWnd "#32770" $t
        Start-Sleep -Milliseconds 800
        if ($d -ne [IntPtr]::Zero) { Shot $d "$Out\setup-$lang.png"; [RS]::PostMessageW($d, 0x0111, [IntPtr]2, [IntPtr]0) | Out-Null }
        $p.WaitForExit(5000) | Out-Null
    }
}
$m = MainWnd
if ($m -ne [IntPtr]::Zero) { [RS]::PostMessageW($m, 0x0111, [IntPtr]40017, [IntPtr]0) | Out-Null }
foreach ($l in 'X', 'Y', 'Z') { & $ctl delete $l force discard | Out-Null }
& $ctl quit | Out-Null
foreach ($f in 'RamDiskBox.ini', 'RamDiskBoxGui.ini', 'RamDiskBox.log') { $p = Join-Path $bin $f; if (Test-Path -LiteralPath $p) { Remove-Item -LiteralPath $p -Force } }
