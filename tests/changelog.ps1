# Prueft Dateisystem-Verhalten (u. a. die Fehler aus dem SoftPerfect-Changelog) an einer Testinstanz.
# Aufruf: tests\changelog.ps1   (Testinstanz RDB_INSTANCE=dev muss laufen; nutzt nur X: und Y:)
param([string]$Work = "$env:TEMP\rdbtests\changelog")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$env:RDB_INSTANCE = 'dev'
$env:RDB_LANG = 'de'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$ctl = "$root\build\bin\rdbctl.exe"
$sp = $Work
$fails = 0
function Check($name, $cond) { if ($cond) { "OK    $name" } else { "FEHLER $name"; $script:fails++ } }
function Guard($l, $label) { $v = (cmd /c "vol $($l):") -join ' '; if ($v -notmatch $label) { throw "$($l): ist nicht das Testlaufwerk ($v)" } }
Add-Type @"
using System; using System.Runtime.InteropServices;
public class SL {
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern IntPtr CreateFileW(string n, uint a, uint s, IntPtr sa, uint c, uint f, IntPtr t);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern IntPtr ReOpenFile(IntPtr h, uint a, uint s, uint f);
  [DllImport("kernel32.dll", SetLastError=true)] public static extern bool ReadFile(IntPtr h, byte[] b, uint n, out uint r, IntPtr o);
  [DllImport("kernel32.dll")] public static extern bool CloseHandle(IntPtr h);
  [DllImport("kernel32.dll", SetLastError=true, CharSet=CharSet.Unicode)] public static extern bool CreateDirectoryExW(string tpl, string n, IntPtr sa);
}
"@

if (Test-Path -LiteralPath $sp) { cmd /c rmdir /s /q "$sp" }
New-Item -ItemType Directory "$sp\src\leer1", "$sp\src\a\leer2", "$sp\src\b" -Force | Out-Null
[IO.File]::WriteAllText("$sp\src\b\file.txt", "inhalt")

& $ctl create X 512M label=SLTest noauto "image=$sp\X.img" | Out-Null
Guard X SLTest
& $ctl create Y 512M label=SLNoCache noauto nosave nocache | Out-Null
Guard Y SLNoCache

foreach ($d in 'X', 'Y') {
    "--- Laufwerk $($d): " + $(if ($d -eq 'X') { "mit Windows-Cache" } else { "ohne Windows-Cache" })
    robocopy "$sp\src" "$($d):\rc" /E /COPY:DAT /DCOPY:DAT /R:0 /W:0 /NP /NJH /NJS | Out-Null
    Check "robocopy kopiert leere Ordner" ((Test-Path "$($d):\rc\leer1") -and (Test-Path "$($d):\rc\a\leer2"))
    cmd /c xcopy "$sp\src" "$($d):\xc\" /E /I /Q /Y | Out-Null
    Check "xcopy kopiert leere Ordner" ((Test-Path "$($d):\xc\leer1") -and (Test-Path "$($d):\xc\a\leer2"))
    Copy-Item "$sp\src" "$($d):\ci" -Recurse
    Check "Copy-Item kopiert leere Ordner" ((Test-Path "$($d):\ci\leer1") -and (Test-Path "$($d):\ci\a\leer2"))
    Check "CreateDirectoryEx mit Vorlage" ([SL]::CreateDirectoryExW("$sp\src\leer1", "$($d):\tpl", [IntPtr]::Zero))
    $sh = New-Object -ComObject Shell.Application
    $sh.NameSpace("$($d):\").CopyHere("$sp\src\a", 4 + 16 + 1024)
    for ($i = 0; $i -lt 40 -and -not (Test-Path "$($d):\a\leer2"); $i++) { Start-Sleep -Milliseconds 250 }
    Check "Explorer-Kopie (Shell) kopiert leere Ordner" (Test-Path "$($d):\a\leer2")

    [IO.File]::WriteAllText("$($d):\case.txt", "x")
    Rename-Item "$($d):\case.txt" "CASE.txt"
    Check "Datei nur Gross-/Kleinschreibung umbenennen" ((Get-ChildItem "$($d):\" -Filter 'case.txt').Name -ceq 'CASE.txt')
    New-Item -ItemType Directory "$($d):\ordner" | Out-Null
    cmd /c ren "$($d):\ordner" "Ordner"
    Check "Ordner nur Gross-/Kleinschreibung umbenennen" ((Get-ChildItem "$($d):\" -Directory -Filter 'ordner').Name -ceq 'Ordner')

    $f = "$($d):\t.txt"; $old = [datetime]'2020-01-01'
    [IO.File]::WriteAllText($f, "1"); (Get-Item $f).LastWriteTime = $old
    [IO.File]::WriteAllText($f, "2")
    Check "Zeit nach Ueberschreiben (CREATE_ALWAYS)" ((Get-Item $f).LastWriteTime -gt [datetime]'2025-01-01')
    (Get-Item $f).LastWriteTime = $old
    $fs = [IO.File]::Open($f, 'Open', 'ReadWrite'); $fs.WriteByte(65); $fs.Close()
    Check "Zeit nach Schreiben in vorhandene Datei" ((Get-Item $f).LastWriteTime -gt [datetime]'2025-01-01')
    (Get-Item $f).LastWriteTime = $old
    $fs = [IO.File]::Open($f, 'Open', 'ReadWrite'); $fs.SetLength(0); $fs.Close()
    Check "Zeit nach Kuerzen (SetLength 0)" ((Get-Item $f).LastWriteTime -gt [datetime]'2025-01-01')
    (Get-Item $f).LastWriteTime = $old
    $fs = [IO.File]::Open($f, 'Truncate', 'Write'); $fs.Close()
    Check "Zeit nach Oeffnen mit TRUNCATE_EXISTING" ((Get-Item $f).LastWriteTime -gt [datetime]'2025-01-01')
    (Get-Item $f).LastWriteTime = $old
    $fs = [IO.File]::Open($f, 'Open', 'ReadWrite'); $fs.SetLength(5000); $fs.Close()
    Check "Zeit nach Verlaengern (SetLength)" ((Get-Item $f).LastWriteTime -gt [datetime]'2025-01-01')

    [IO.File]::WriteAllText("$($d):\re.txt", "ReOpenFile-Test")
    $h = [SL]::CreateFileW("$($d):\re.txt", [uint32]2147483648, 3, [IntPtr]::Zero, 3, 0, [IntPtr]::Zero)
    $h2 = [SL]::ReOpenFile($h, [uint32]2147483648, 3, 0)
    $ok = $h2 -ne [IntPtr](-1) -and $h2 -ne [IntPtr]::Zero
    $txt = ""
    if ($ok) { $b = New-Object byte[] 64; $n = 0; [SL]::ReadFile($h2, $b, 64, [ref]$n, [IntPtr]::Zero) | Out-Null; $txt = [Text.Encoding]::ASCII.GetString($b, 0, $n); [SL]::CloseHandle($h2) | Out-Null }
    [SL]::CloseHandle($h) | Out-Null
    Check "ReOpenFile auf vorhandenem Handle ($txt)" ($ok -and $txt -eq "ReOpenFile-Test")

    $pat = New-Object byte[] (1MB); (New-Object Random 5).NextBytes($pat)
    $mmf = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile("$($d):\mm.bin", 'Create', [NullString]::Value, 24MB)
    $acc = $mmf.CreateViewAccessor()
    for ($k = 0; $k -lt 24; $k++) { $acc.WriteArray([long]$k * 1MB, $pat, 0, $pat.Length) }
    $acc.Dispose(); $mmf.Dispose()
    $got = [IO.File]::ReadAllBytes("$($d):\mm.bin")
    $okm = $got.Length -eq 24MB
    if ($okm) { foreach ($k in 0, 11, 23) { if (-not [Linq.Enumerable]::SequenceEqual([byte[]]$pat, [byte[]]($got[($k * 1MB)..(($k + 1) * 1MB - 1)]))) { $okm = $false } } }
    Check "Speicherabbild: neue Datei vollstaendig ($($got.Length) Bytes)" $okm
    [IO.File]::WriteAllBytes("$($d):\mm2.bin", (New-Object byte[] (8MB)))
    $mmf = [IO.MemoryMappedFiles.MemoryMappedFile]::CreateFromFile("$($d):\mm2.bin", 'Open', [NullString]::Value, 0)
    $acc = $mmf.CreateViewAccessor(); $acc.WriteArray(3MB, $pat, 0, $pat.Length); $acc.Dispose(); $mmf.Dispose()
    $got = [IO.File]::ReadAllBytes("$($d):\mm2.bin")
    Check "Speicherabbild: vorhandene Datei geaendert" ([Linq.Enumerable]::SequenceEqual([byte[]]$pat, [byte[]]($got[(3MB)..(4MB - 1)])))

    if ($d -eq 'X') {
        New-Item -ItemType Directory "$sp\arch" -Force | Out-Null
        $rnd = New-Object Random 9
        foreach ($n in 1..6) { $b = New-Object byte[] ($n * 300000 + 17); $rnd.NextBytes($b); [IO.File]::WriteAllBytes("$sp\arch\f$n.bin", $b) }
        [IO.File]::WriteAllBytes("$sp\arch\leer.bin", (New-Object byte[] 0))
        Compress-Archive -Path "$sp\arch\*" -DestinationPath "$sp\arch.zip" -Force
        cmd /c tar -cf "$sp\arch.tar" -C "$sp\arch" . 2>$null
    }
    Expand-Archive "$sp\arch.zip" "$($d):\zip" -Force
    New-Item -ItemType Directory "$($d):\tar" | Out-Null
    cmd /c tar -xf "$sp\arch.tar" -C "$($d):\tar" 2>$null
    $out = "$sp\out$d"
    robocopy "$($d):\zip" "$out\zip" /E /R:0 /W:0 /NP /NJH /NJS | Out-Null
    Copy-Item "$($d):\tar" "$out\tar" -Recurse
    $bad = 0
    foreach ($fi in Get-ChildItem "$sp\arch" -File) {
        $h0 = (Get-FileHash $fi.FullName).Hash
        foreach ($p in "$($d):\zip\$($fi.Name)", "$($d):\tar\$($fi.Name)", "$out\zip\$($fi.Name)", "$out\tar\$($fi.Name)") { if ((Get-FileHash $p).Hash -ne $h0) { $bad++; "  Abweichung $p" } }
    }
    Check "Entpacken (zip, tar) und wieder herunterkopieren" ($bad -eq 0)

    New-Item -ItemType Directory "$($d):\target" | Out-Null
    cmd /c mklink /J "$($d):\j" "$($d):\target" | Out-Null
    [IO.File]::WriteAllText("$($d):\mv.txt", "mv")
    Move-Item "$($d):\mv.txt" "$($d):\j\mv.txt"
    Check "Verschieben in Verbindung auf dem Laufwerk" (Test-Path "$($d):\target\mv.txt")
    Rename-Item "$($d):\j\mv.txt" "mv2.txt"
    Check "Umbenennen innerhalb der Verbindung" (Test-Path "$($d):\target\mv2.txt")
    cmd /c mklink /J "$sp\cj$d" "$($d):\target" | Out-Null
    [IO.File]::WriteAllText("$sp\von-c.txt", "c")
    Move-Item "$sp\von-c.txt" "$sp\cj$d\von-c.txt"
    Check "Verschieben von aussen ueber Verbindung auf das Laufwerk" (Test-Path "$($d):\target\von-c.txt")
    cmd /c rmdir "$sp\cj$d" | Out-Null
}

$b = New-Object byte[] (50MB); (New-Object Random 3).NextBytes($b)
$s0 = & $ctl iostats | Out-String
[IO.File]::WriteAllBytes("X:\traffic.bin", $b)
Start-Sleep -Seconds 2
$s1 = & $ctl iostats | Out-String
& $ctl save X | Out-Null
$s2 = & $ctl iostats | Out-String
function Written($t) { $m = [regex]::Match($t, 'geschrieben ([\d,]+) (\w+)'); $v = [double]($m.Groups[1].Value -replace ',', '.'); if ($m.Groups[2].Value -eq 'GB') { $v * 1024 } elseif ($m.Groups[2].Value -eq 'KB') { $v / 1024 } else { $v } }
$dw = (Written $s1) - (Written $s0)
Check ("Datenverkehr einfach gezaehlt (+{0:N1} MB fuer 50 MB)" -f $dw) ([Math]::Abs($dw - 50) -lt 1)
Check "Image-Sicherung zaehlt nicht als Datenverkehr" ((Written $s2) -eq (Written $s1))

[IO.File]::WriteAllText("X:\wichtig.txt", "darf nicht verloren gehen")
& $ctl dismount X force | Out-Null
$lock = [IO.File]::Open("$sp\X.img", 'Open', 'Read', 'None')
$o = & $ctl mount X 2>&1 | Out-String
$mountedWhileLocked = Test-Path "X:\"
$lock.Close()
if ($mountedWhileLocked) { & $ctl dismount X force | Out-Null }
Check "Bei gesperrtem Image nicht eingehaengt" (-not $mountedWhileLocked)
& $ctl mount X | Out-Null
Check "Gesperrtes Image nicht ueberschrieben (Inhalt noch da)" ((Test-Path "X:\wichtig.txt") -and ((Get-Content "X:\wichtig.txt") -eq "darf nicht verloren gehen"))

& $ctl delete X force discard | Out-Null
& $ctl delete Y force discard | Out-Null
cmd /c rmdir /s /q "$sp"
"Fehler gesamt: $fails"
