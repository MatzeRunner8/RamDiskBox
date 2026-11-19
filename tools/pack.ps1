# Stellt das portable Paket zusammen: dist\RamDiskBox-<Version>\ und dist\RamDiskBox-<Version>-portable.zip
$root = Split-Path $PSScriptRoot -Parent
$ver = (Select-String -Path "$root\src\common\version.h" -Pattern '#define RDB_VER_(MAJOR|MINOR|PATCH)\s+(\d+)' |
    ForEach-Object { $_.Matches[0].Groups[2].Value }) -join '.'
$name = "RamDiskBox-$ver"
$dist = Join-Path $root "dist"
$dir = Join-Path $dist $name
if (Test-Path $dir) { Remove-Item -Recurse -Force $dir }
New-Item -ItemType Directory -Force $dir | Out-Null
foreach ($f in 'RamDiskBox.exe', 'RamDiskBoxSvc.exe', 'rdbctl.exe') {
    Copy-Item (Join-Path $root "build\bin\$f") $dir
}
Copy-Item (Join-Path $root 'README.de.md') (Join-Path $dir 'LIESMICH.md')
Copy-Item (Join-Path $root 'README.md') $dir
Copy-Item (Join-Path $root 'CHANGELOG.md') $dir
Copy-Item (Join-Path $root 'LICENSE') $dir
Copy-Item (Join-Path $root 'src\third_party\lz4\LICENSE') (Join-Path $dir 'LICENSE-lz4.txt')
Copy-Item (Join-Path $root 'src\third_party\zstd\LICENSE') (Join-Path $dir 'LICENSE-zstd.txt')
$zip = Join-Path $dist "$name-portable.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$dir\*" -DestinationPath $zip
Get-ChildItem $dir | ForEach-Object { "{0,-20} {1,10:N0}  {2}" -f $_.Name, $_.Length, (Get-FileHash $_.FullName -Algorithm SHA256).Hash }
"ZIP: $zip ({0:N0} Bytes)" -f (Get-Item $zip).Length
# Setup (build.bat baut es zuletzt, mit genau diesen Programmen)
$setup = Join-Path $dist "$name-Setup.exe"
Copy-Item (Join-Path $root 'build\RamDiskBox-Setup.exe') $setup -Force
"Setup: $setup ({0:N0} Bytes, SHA256 {1})" -f (Get-Item $setup).Length, (Get-FileHash $setup -Algorithm SHA256).Hash
