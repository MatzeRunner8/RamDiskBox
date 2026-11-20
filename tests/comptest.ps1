# Komprimierung im Arbeitsspeicher: Teil-Lesen, Schreiben in komprimierte Bloecke, Kuerzen, Stufenwechsel,
# Image-Rundlauf (Testinstanz, legt T: mit Image an)
param([string]$Work = "$env:TEMP\rdbtests\comptest")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$Ctl = "$root\build\bin\rdbctl.exe"
$env:RDB_INSTANCE = 'dev'
$env:RDB_LANG = 'de'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
New-Item -ItemType Directory $Work -Force | Out-Null
$D = "T:"
$img = "$Work\T.img"
if (Test-Path -LiteralPath $img) { Remove-Item -LiteralPath $img -Force }
& $Ctl create T 1G label=CompTest compress=fast noauto "image=$img" | Out-Null
if (((cmd /c vol T:) -join ' ') -notmatch 'CompTest') { throw "T: ist nicht das Testlaufwerk" }
$fails = 0
function Check($name, $cond) { if ($cond) { "OK    $name" } else { "FEHLER $name"; $script:fails++ } }
function CompInfo { $l = & $Ctl list | Select-String "Komprimierung:" | Select-Object -First 1; if ($l) { $l.Line.Trim() } else { "" } }
$rnd = New-Object System.Random 4711
function RandBytes([int]$n) { $b = New-Object byte[] $n; $rnd.NextBytes($b); return ,$b }
function TextBytes([int]$n) {
    $sb = New-Object System.Text.StringBuilder
    $i = 0
    while ($sb.Length -lt $n) { [void]$sb.AppendFormat("2026-11-19 17:{0:00}:{1:00} INFO  Modul{2} Verarbeitung von Datensatz {3} abgeschlossen, Status OK`r`n", ($i / 60) % 60, $i % 60, $i % 7, $i); $i++ }
    return ,([Text.Encoding]::ASCII.GetBytes($sb.ToString(0, $n)))
}

$t = "$D\ct"
New-Item -ItemType Directory $t | Out-Null
$exp = @{}
foreach ($s in 100, 4095, 65536, 65537, 300001, 5000000, 40000000) { $b = TextBytes $s; [IO.File]::WriteAllBytes("$t\text$s.log", $b); $exp["text$s.log"] = $b }
$b = RandBytes 20000000; [IO.File]::WriteAllBytes("$t\random.bin", $b); $exp["random.bin"] = $b
$m = New-Object byte[] 3000000; [Array]::Copy((TextBytes 1500000), 0, $m, 0, 1500000); [Array]::Copy((RandBytes 1500000), 0, $m, 1500000, 1500000)
[IO.File]::WriteAllBytes("$t\mixed.bin", $m); $exp["mixed.bin"] = $m
New-Item -ItemType Directory "$t\small" | Out-Null
for ($i = 0; $i -lt 500; $i++) { $sb = TextBytes (2000 + $i * 7); [IO.File]::WriteAllBytes("$t\small\s$i.txt", $sb); $exp["small\s$i.txt"] = $sb }

Start-Sleep -Seconds 5
$info = CompInfo
Check "Komprimierung hat gearbeitet ($info)" ($info -match "gespart")
function VerifyAll($tag) {
    $bad = 0
    foreach ($k in $exp.Keys) {
        $act = [IO.File]::ReadAllBytes("$t\$k")
        if (-not [System.Linq.Enumerable]::SequenceEqual([byte[]]$exp[$k], [byte[]]$act)) { $bad++; if ($bad -le 3) { "  Abweichung: $k" } }
    }
    Check "$tag : alle $($exp.Count) Dateien unveraendert" ($bad -eq 0)
}
VerifyAll "Lesen nach Komprimierung"

$f = "$t\text40000000.log"; $e = $exp["text40000000.log"]
$fs = New-Object IO.FileStream($f, 'Open', 'Read', 'Read', 1)
$okr = $true
for ($i = 0; $i -lt 300; $i++) {
    $pos = $rnd.Next(0, $e.Length - 5000); $len = $rnd.Next(1, 5000)
    $fs.Position = $pos; $buf = New-Object byte[] $len; $n = $fs.Read($buf, 0, $len)
    for ($j = 0; $j -lt $n; $j += 97) { if ($buf[$j] -ne $e[$pos + $j]) { $okr = $false; break } }
    if (-not $okr) { break }
}
$fs.Close()
Check "300 zufaellige Teil-Lesevorgaenge" $okr

Start-Sleep -Seconds 3
$f = "$t\text5000000.log"; $e = $exp["text5000000.log"]
$fs = [IO.File]::Open($f, 'Open', 'ReadWrite')
foreach ($pos in 12345, 65530, 1000000, 4999990) {
    $patch = [Text.Encoding]::ASCII.GetBytes("#GEAENDERT#")
    $fs.Position = $pos; $fs.Write($patch, 0, [Math]::Min($patch.Length, $e.Length - $pos))
    [Array]::Copy($patch, 0, $e, $pos, [Math]::Min($patch.Length, $e.Length - $pos))
}
$fs.Close()
Start-Sleep -Seconds 3
Check "Schreiben in komprimierte Bloecke" ([System.Linq.Enumerable]::SequenceEqual([byte[]]$e, [byte[]][IO.File]::ReadAllBytes($f)))

$f = "$t\text300001.log"
$fs = [IO.File]::Open($f, 'Open', 'ReadWrite'); $fs.SetLength(100000); $fs.SetLength(250000); $fs.Close()
$act = [IO.File]::ReadAllBytes($f); $e = $exp["text300001.log"]
$okh = $true; for ($i = 0; $i -lt 100000; $i += 13) { if ($act[$i] -ne $e[$i]) { $okh = $false; break } }
$okz = $true; for ($i = 100000; $i -lt 250000; $i++) { if ($act[$i] -ne 0) { $okz = $false; break } }
Check "Kuerzen erhaelt Anfang" $okh
Check "Verlaengern nach Kuerzen liefert Nullen" $okz
$ne = New-Object byte[] 250000; [Array]::Copy($e, 0, $ne, 0, 100000); $exp["text300001.log"] = $ne

Start-Sleep -Seconds 3
$f = "$t\text65536.log"; $add = TextBytes 70000
$fs = [IO.File]::Open($f, 'Append', 'Write'); $fs.Write($add, 0, $add.Length); $fs.Close()
$ne = New-Object byte[] (65536 + 70000); [Array]::Copy($exp["text65536.log"], 0, $ne, 0, 65536); [Array]::Copy($add, 0, $ne, 65536, 70000); $exp["text65536.log"] = $ne
Start-Sleep -Seconds 3
VerifyAll "Nach Aendern/Kuerzen/Anhaengen"

& $Ctl update T compress=max | Out-Null
Start-Sleep -Seconds 8
$info2 = CompInfo
Check "Wechsel auf stark ($info2)" ($info2 -match "stark")
VerifyAll "Nach Wechsel auf stark"
& $Ctl update T compress=off | Out-Null
VerifyAll "Nach Abschalten"
& $Ctl update T compress=fast | Out-Null
Start-Sleep -Seconds 4

$o = & $Ctl dismount T 2>&1
Check "Aushaengen mit Image ($o)" ($LASTEXITCODE -eq 0)
$o = & $Ctl mount T 2>&1
Check "Einhaengen mit Image ($o)" ($LASTEXITCODE -eq 0)
Start-Sleep -Seconds 4
VerifyAll "Nach Image-Rundlauf"
& $Ctl delete T force discard | Out-Null
cmd /c rmdir /s /q "$Work"
"Fehler gesamt: $fails"
