# extract.ps1 - pull the Jelly Monsters cartridge out of a VIC-20 cartridge
# collection you own. No cartridge data is committed to this repo.
#
# The TOSEC "Commodore VIC20" set ships each cart as a nested .zip containing a
# raw .crt (a 2-byte $A000 load-address header + the 8K ROM). This script finds
# the Jelly Monsters [A000] entry and writes its .crt to roms/.
#
#   ./scripts/extract.ps1 -Zip "Z:\path\TOSEC...VIC20....zip"
#   ./scripts/extract.ps1 -Crt "C:\carts\Jelly Monsters.crt"   # already have one
param(
    [string]$Zip,
    [string]$Crt,
    [string]$Out = "roms/jellymonsters.crt",
    [string]$Match = "Jelly Monsters v1"
)
$ErrorActionPreference = "Stop"
New-Item -ItemType Directory -Force -Path (Split-Path $Out) | Out-Null

if ($Crt) {
    if (-not (Test-Path $Crt)) { throw "cart not found: $Crt" }
    Copy-Item $Crt $Out -Force
    Write-Host "Copied -> $Out"
    return
}
if (-not $Zip) { throw "pass -Zip <tosec.zip> or -Crt <file.crt>" }
if (-not (Test-Path $Zip)) { throw "zip not found: $Zip" }

Add-Type -AssemblyName System.IO.Compression.FileSystem
$outer = [System.IO.Compression.ZipFile]::OpenRead((Resolve-Path $Zip))
try {
    $inner = $outer.Entries | Where-Object { $_.FullName -match '\[CRT\]' -and $_.FullName -match [regex]::Escape($Match) -and $_.FullName -match '\[A000\]\.zip$' } | Select-Object -First 1
    if (-not $inner) { throw "no '$Match ... [A000]' cartridge found in $Zip" }
    $tmp = [System.IO.Path]::GetTempFileName()
    $s = $inner.Open(); $fs = [System.IO.File]::Create($tmp); $s.CopyTo($fs); $fs.Close(); $s.Close()
    $iz = [System.IO.Compression.ZipFile]::OpenRead($tmp)
    try {
        $crt = $iz.Entries | Where-Object { $_.Name -match '\.crt$' } | Select-Object -First 1
        if (-not $crt) { throw "no .crt inside $($inner.Name)" }
        $cs = $crt.Open(); $cf = [System.IO.File]::Create($Out); $cs.CopyTo($cf); $cf.Close(); $cs.Close()
    } finally { $iz.Dispose(); Remove-Item $tmp -Force }
} finally { $outer.Dispose() }
Write-Host "Extracted '$Match' -> $Out (maps at `$A000, cold-start at the word in `$A000)"
