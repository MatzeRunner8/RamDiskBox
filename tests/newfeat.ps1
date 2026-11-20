# Tests fuer zstd, Stufenwechsel, Quellordner, Defender-Ausnahme ohne Adminrechte (Testinstanz, Q:/P:/O:)
param([string]$Work = "$env:TEMP\rdbtests\newfeat")
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$Ctl = "$root\build\bin\rdbctl.exe"
$env:RDB_INSTANCE = 'dev'
$env:RDB_LANG = 'de'
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$sp = $Work
New-Item -ItemType Directory $sp -Force | Out-Null
$fails = 0
function Check($name, $cond) { if ($cond) { "OK    $name" } else { "FEHLER $name"; $script:fails++ } }
function Guard($l, $label) { $v = (cmd /c "vol $($l):") -join ' '; if ($v -notmatch $label) { throw "$($l): ist nicht das Testlaufwerk ($v)" } }
function Vol($l) { $o = & $Ctl list | Out-String; $m = [regex]::Match($o, "(?ms)^$($l):.*?(?=^\w:|\z)"); $m.Value }
function CompLine($l) { ((Vol $l) -split "`n" | Where-Object { $_ -match "Komprimierung:" } | Select-Object -First 1).Trim() }
function RamOf($l) { if ((Vol $l) -match 'RAM ([\d,]+) (\w+)') { $v = [double]($matches[1] -replace ',','.'); switch ($matches[2]) { 'KB' { $v / 1024 } 'MB' { $v } 'GB' { $v * 1024 } default { $v / 1MB } } } else { -1 } }
function TextBytes([int]$n) {
    $sb = New-Object System.Text.StringBuilder; $i = 0
    while ($sb.Length -lt $n) { [void]$sb.AppendFormat("{0:00000} Messwert {1} Sensor{2} Temperatur {3},{4} Grad Status normal`r`n", $i, $i * 7, $i % 13, 20 + $i % 9, $i % 10); $i++ }
    return ,([Text.Encoding]::ASCII.GetBytes($sb.ToString(0, $n)))
}

# 1) zstd und Stufenwechsel
& $Ctl create Q 1G label=Zstd compress=max noauto nosave | Out-Null
Guard Q Zstd
$data = TextBytes (64MB)
[IO.File]::WriteAllBytes("Q:\daten.log", $data)
Start-Sleep -Seconds 6
$l1 = CompLine 'Q'
Check "zstd komprimiert ($l1)" ($l1 -match "stark \(zstd\).*gespart")
Check "zstd: Daten korrekt" ([System.Linq.Enumerable]::SequenceEqual([byte[]]$data, [byte[]][IO.File]::ReadAllBytes("Q:\daten.log")))
$ramZstd = RamOf 'Q'
& $Ctl update Q compress=fast | Out-Null
Start-Sleep -Seconds 4
$l2 = CompLine 'Q'
$ramLz4 = RamOf 'Q'
Check "Umstellung zstd -> LZ4 rechnet um ($l2, RAM $ramZstd -> $ramLz4 MB)" (($l2 -match "schnell") -and ($ramLz4 -gt $ramZstd))
Check "LZ4: Daten korrekt" ([System.Linq.Enumerable]::SequenceEqual([byte[]]$data, [byte[]][IO.File]::ReadAllBytes("Q:\daten.log")))
& $Ctl update Q compress=off | Out-Null
Start-Sleep -Seconds 4
$l3 = CompLine 'Q'
$ramOff = RamOf 'Q'
Check "Aus entpackt alles ($l3, RAM $ramOff MB)" (($l3 -notmatch "gespart") -and ($ramOff -ge 63))
Check "Aus: Daten korrekt" ([System.Linq.Enumerable]::SequenceEqual([byte[]]$data, [byte[]][IO.File]::ReadAllBytes("Q:\daten.log")))
& $Ctl update Q compress=max | Out-Null
Start-Sleep -Seconds 6
Check "Wieder zstd ($(CompLine 'Q'))" ((CompLine 'Q') -match "stark.*gespart")
$fs = [IO.File]::Open("Q:\daten.log", 'Open', 'ReadWrite')
$patch = [Text.Encoding]::ASCII.GetBytes("<<ZSTD-PATCH>>")
foreach ($pos in 100, 65530, 33554432, 67108860) { $fs.Position = $pos; $n = [Math]::Min($patch.Length, $data.Length - $pos); $fs.Write($patch, 0, $n); [Array]::Copy($patch, 0, $data, $pos, $n) }
$fs.Close()
Start-Sleep -Seconds 6
Check "Schreiben in zstd-Bloecke" ([System.Linq.Enumerable]::SequenceEqual([byte[]]$data, [byte[]][IO.File]::ReadAllBytes("Q:\daten.log")))

# 2) Quellordner
$src = Join-Path $sp "quelle"
if (Test-Path $src) { cmd /c rmdir /s /q "$src" }
New-Item -ItemType Directory "$src\Unter\Tief" -Force | Out-Null
$files = @{}
$rnd = New-Object System.Random 99
foreach ($pair in @(@("a.txt", 100), @("Unter\b.bin", 70000), @("Unter\Tief\c.bin", 3000000), @("leer.txt", 0))) {
    $b = New-Object byte[] $pair[1]; $rnd.NextBytes($b); [IO.File]::WriteAllBytes("$src\$($pair[0])", $b); $files[$pair[0]] = $b
}
$z = New-Object byte[] (8MB); [Array]::Copy(([byte[]](1..200)), 0, $z, 4MB, 200); [IO.File]::WriteAllBytes("$src\duenn.bin", $z); $files["duenn.bin"] = $z
(Get-Item "$src\a.txt").LastWriteTime = [datetime]"2019-05-06 07:08:09"
(Get-Item "$src\a.txt").Attributes = 'ReadOnly, Hidden'
cmd /c mklink /J "$src\Verknuepfung" "$src\Unter" | Out-Null
$img = Join-Path $sp "P.img"
if (Test-Path $img) { Remove-Item -LiteralPath $img -Force }
& $Ctl create P 64M label=Vorlage "source=$src" "image=$img" noauto | Out-Null
Guard P Vorlage
$bad = 0; foreach ($k in $files.Keys) { if (-not [System.Linq.Enumerable]::SequenceEqual([byte[]]$files[$k], [byte[]][IO.File]::ReadAllBytes("P:\$k"))) { $bad++; "  Abweichung $k" } }
Check "Quellordner kopiert ($($files.Count) Dateien, Unterordner)" ($bad -eq 0)
$fa = Get-Item -Force "P:\a.txt"
Check "Zeitstempel uebernommen" ($fa.LastWriteTime -eq [datetime]"2019-05-06 07:08:09")
Check "Attribute uebernommen" (($fa.Attributes -band [IO.FileAttributes]::ReadOnly) -and ($fa.Attributes -band [IO.FileAttributes]::Hidden))
Check "Verknuepfung nicht verfolgt" (-not (Test-Path "P:\Verknuepfung"))
$ramP = RamOf 'P'
Check "Duenne Datei belegt kaum RAM ($ramP MB fuer 11 MB Dateien)" ($ramP -lt 5)
[IO.File]::WriteAllText("P:\neu.txt", "nach dem Kopieren angelegt")
& $Ctl dismount P | Out-Null
[IO.File]::WriteAllText("$src\spaeter.txt", "erst nach dem ersten Einhaengen in der Quelle")
& $Ctl mount P | Out-Null
Check "Image hat Vorrang vor Quellordner" ((Test-Path "P:\neu.txt") -and -not (Test-Path "P:\spaeter.txt"))
$big = New-Object byte[] (40MB); $rnd.NextBytes($big); [IO.File]::WriteAllBytes("$src\gross1.bin", $big); [IO.File]::WriteAllBytes("$src\gross2.bin", $big)
& $Ctl create O 48M label=Klein "source=$src" noauto nosave | Out-Null
$vo = Vol 'O'
Check "Zu klein: Warnung statt Abbruch" ($vo -match "Hinweis: Quellordner: Das Laufwerk ist zu klein")

# 3) Defender-Ausnahme ohne Adminrechte (darf nichts aendern, nur melden)
& $Ctl update Q avexclude | Out-Null
$vq = Vol 'Q'
Check "Defender-Ausnahme ohne Admin: sauber abgelehnt" (($vq -match "Defender-Ausnahme: ja \(nicht aktiv\)") -and ($vq -match "Defender-Ausnahme nicht m.glich"))
& $Ctl update Q noavexclude | Out-Null

foreach ($l in 'Q', 'P', 'O') { & $Ctl delete $l force discard | Out-Null }
cmd /c rmdir "$src\Verknuepfung" | Out-Null
cmd /c rmdir /s /q "$sp"
"Fehler gesamt: $fails"
