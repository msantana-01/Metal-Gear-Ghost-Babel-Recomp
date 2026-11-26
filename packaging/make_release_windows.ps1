<#
.SYNOPSIS
  Builds the Windows release archive (ghost-babel-tools-windows-x86_64-<version>.zip)
  from a strict whitelist (ADR-005: toolchain only, never the game).
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Version
)

$ErrorActionPreference = "Stop"
$RepoDir = Split-Path -Parent $PSScriptRoot
Set-Location $RepoDir

$outRoot = Join-Path $RepoDir "packaging\dist"
$pkgRoot = Join-Path $outRoot "ghost-babel-tools"
if (Test-Path $outRoot) { Remove-Item -Recurse -Force $outRoot }
New-Item -ItemType Directory -Force -Path `
    (Join-Path $pkgRoot "bin"), `
    (Join-Path $pkgRoot "lib"), `
    (Join-Path $pkgRoot "sdl2\include\SDL2"), `
    (Join-Path $pkgRoot "sdl2\lib"), `
    (Join-Path $pkgRoot "sdl2\bin") | Out-Null

$whitelist = @(
    "README.md",
    "LICENSE",
    "pins.lock",
    "patches\gb-recompiled-ghost-babel.patch",
    "remaster\remastmain.c",
    "remaster\native_presenter.c",
    "remaster\native_presenter.h",
    "remaster\camera_extent.c",
    "remaster\camera_extent.h",
    "remaster\room_record.c",
    "remaster\room_record.h",
    "remaster\room_layers.c",
    "remaster\room_layers.h",
    "remaster\room_map.c",
    "remaster\room_map.h",
    "remaster\room_links.c",
    "remaster\room_links.h",
    "remaster\room_compositor.c",
    "remaster\room_compositor.h",
    "remaster\learned_links.c",
    "remaster\learned_links.h",
    "remaster\room_actors.c",
    "remaster\room_actors.h",
    "remaster\room_controls.c",
    "remaster\room_controls.h",
    "remaster\room_rumble.c",
    "remaster\room_rumble.h",
    "remaster\codec_lipsync.c",
    "remaster\codec_lipsync.h",
    "remaster\sprite_shadow.c",
    "remaster\sprite_shadow.h",
    "profiles\ghost-babel.json",
    "windows\Makefile.win",
    "windows\bootstrap.ps1",
    "windows\build.ps1",
    "windows\gui.ps1",
    "windows\GhostBabelRecomp.bat",
    "windows\build.bat",
    "windows\run-remaster.bat",
    "windows\run-authentic.bat",
    "tools\validate_rom.py",
    "tools\pick_profile.py",
    "tools\guard_copyright.sh",
    "tools\gui\icon.png",
    "tests\make_test_rom.py"
)

foreach ($rel in $whitelist) {
    $src = Join-Path $RepoDir $rel
    if (-not (Test-Path $src)) { throw "MISSING from repo: $rel" }
    $dst = Join-Path $pkgRoot $rel
    New-Item -ItemType Directory -Force -Path (Split-Path -Parent $dst) | Out-Null
    Copy-Item -Force $src $dst
}

# Copy user-facing .bat launchers to archive root for one-click ergonomics.
foreach ($bat in @("GhostBabelRecomp.bat", "build.bat", "run-remaster.bat", "run-authentic.bat")) {
    Copy-Item -Force (Join-Path $RepoDir "windows\$bat") (Join-Path $pkgRoot $bat)
}

# Precompiled toolchain binaries (never game executables)
$exeSrc = Join-Path $RepoDir ".tools\gbrecomp.exe"
$libSrc = Join-Path $RepoDir ".tools\libgbrt.a"
if (-not (Test-Path $exeSrc)) { throw "MISSING .tools\gbrecomp.exe (run windows/bootstrap.ps1)" }
if (-not (Test-Path $libSrc)) { throw "MISSING .tools\libgbrt.a (run windows/bootstrap.ps1)" }
Copy-Item -Force $exeSrc (Join-Path $pkgRoot "bin\gbrecomp.exe")
Copy-Item -Force $libSrc (Join-Path $pkgRoot "lib\libgbrt.a")

# Runtime C/C++ snapshot embedded by gbrecomp
$rtSrc = $null
foreach ($cand in @((Join-Path $RepoDir ".tools\engine\runtime"), (Join-Path $RepoDir "runtime"))) {
    if (Test-Path (Join-Path $cand "include\gbrt.h")) { $rtSrc = $cand; break }
}
if (-not $rtSrc) { throw "MISSING runtime snapshot (run windows/bootstrap.ps1)" }
Copy-Item -Recurse -Force $rtSrc (Join-Path $pkgRoot "runtime")

# SDL2 MinGW headers, import libs, and SDL2.dll (zlib-licensed runtime dependency)
$sdlSrc = $null
foreach ($cand in @((Join-Path $RepoDir ".tools\sdl2"), (Join-Path $RepoDir "sdl2"))) {
    if ((Test-Path (Join-Path $cand "include\SDL2\SDL.h")) -and (Test-Path (Join-Path $cand "bin\SDL2.dll"))) {
        $sdlSrc = $cand; break
    }
}
if (-not $sdlSrc) { throw "MISSING SDL2 MinGW directory (run windows/bootstrap.ps1)" }
Copy-Item -Recurse -Force (Join-Path $sdlSrc "include\*") (Join-Path $pkgRoot "sdl2\include")
Copy-Item -Recurse -Force (Join-Path $sdlSrc "lib\*")     (Join-Path $pkgRoot "sdl2\lib")
Copy-Item -Force (Join-Path $sdlSrc "bin\SDL2.dll")       (Join-Path $pkgRoot "sdl2\bin\SDL2.dll")
$sdlLic = Join-Path $sdlSrc "LICENSE.txt"
if (Test-Path $sdlLic) { Copy-Item -Force $sdlLic (Join-Path $pkgRoot "sdl2\LICENSE.txt") }

# Zip with top-level ghost-babel-tools/ directory
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zipName = "ghost-babel-tools-windows-x86_64-$Version.zip"
$zipPath = Join-Path $outRoot $zipName
if (Test-Path $zipPath) { Remove-Item -Force $zipPath }
[System.IO.Compression.ZipFile]::CreateFromDirectory(
    $pkgRoot,
    $zipPath,
    [System.IO.Compression.CompressionLevel]::Optimal,
    $true
)

# Verify archive contents against ADR-005 (zero ROM/game binary artifacts)
$zipReader = [System.IO.Compression.ZipFile]::OpenRead($zipPath)
try {
    foreach ($entry in $zipReader.Entries) {
        $norm = $entry.FullName.Replace('\', '/')
        if ($norm.EndsWith('/')) { continue }
        if ($norm -match '\.(gbc|gb|sav|rtc)$' -or
            $norm -match '(^|/)baserom' -or
            $norm -match '^ghost-babel-tools/build/' -or
            $norm -match 'ghost-babel-(remaster|authentic)') {
            throw "ABORTED: forbidden ROM-derived entry in Windows archive: $norm"
        }
        if ($norm -match '\.(exe|dll)$' -and
            $norm -ne 'ghost-babel-tools/bin/gbrecomp.exe' -and
            $norm -ne 'ghost-babel-tools/sdl2/bin/SDL2.dll') {
            throw "ABORTED: unexpected binary in Windows archive: $norm"
        }
    }
} finally {
    $zipReader.Dispose()
}

$hash = (Get-FileHash -Algorithm SHA256 -Path $zipPath).Hash.ToLowerInvariant()
[System.IO.File]::WriteAllText("$zipPath.sha256", "$hash  $zipName`n", (New-Object System.Text.UTF8Encoding($false)))
Write-Host "artifact:  $zipPath"
Write-Host "sha256:    $hash"
Write-Host "ok: tools only (ADR-005 verified)." -ForegroundColor Green
