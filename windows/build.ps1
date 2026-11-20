<#
.SYNOPSIS
  Validates and compiles a user's Game Boy / Game Boy Color ROM on Windows.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$RomPath,
    [switch]$ValidateOnly,
    [switch]$NoPrompt,
    [switch]$ForceRemaster,
    [switch]$SyntheticSmoke
)

$ErrorActionPreference = "Stop"
$RepoDir  = Split-Path -Parent $PSScriptRoot
$ToolsDir = Join-Path $RepoDir ".tools"
$BuildDir = Join-Path $RepoDir "build"
$LogFile  = Join-Path $RepoDir ".build.log"
$PinsPath = Join-Path $RepoDir "pins.lock"

function Test-GameBoyRom([string]$path) {
    if (-not (Test-Path $path)) {
        return @{ Ok = $false; Errors = @("the file '$path' does not exist"); Infos = @() }
    }
    if ((Get-Item $path).PSIsContainer) {
        return @{ Ok = $false; Errors = @("'$path' is a folder, not a ROM file"); Infos = @() }
    }

    $bytes = [System.IO.File]::ReadAllBytes($path)
    $size  = $bytes.Length
    $errors = New-Object System.Collections.Generic.List[string]
    $infos  = New-Object System.Collections.Generic.List[string]

    if ($size -lt 0x150) {
        $errors.Add("the file is $size bytes; every Game Boy ROM is at least 336 (0x150).")
        return @{ Ok = $false; Errors = $errors; Infos = $infos }
    }

    $nintendoLogo = [byte[]]@(0xCE,0xED,0x66,0x66,0xCC,0x0D,0x00,0x0B,0x03,0x73,0x00,0x83,0x00,0x0C,0x00,0x0D)
    $logoSlice = $bytes[0x104..0x113]
    $isNintendo = $true
    $isUnsigned = $true
    for ($i = 0; $i -lt 16; $i++) {
        if ($logoSlice[$i] -ne $nintendoLogo[$i]) { $isNintendo = $false }
        if ($logoSlice[$i] -ne 0xFF) { $isUnsigned = $false }
    }
    if ($isNintendo) {
        $infos.Add("signature: Nintendo")
    } elseif ($isUnsigned) {
        $infos.Add("signature: unsigned (homebrew/copy)")
    } else {
        $errors.Add("the manufacturer signature (0x104-0x113) matches no Game Boy ROM.")
    }

    $sizeCode = [int]$bytes[0x148]
    if ($sizeCode -gt 0x08) {
        $errors.Add("ROM-size code in header is invalid or >8 MiB.")
    } else {
        $declared = ([int64]32 -shl $sizeCode) * 1024
        if ($size -eq $declared) {
            $infos.Add(("size in header: {0:N0} bytes" -f $declared))
        } else {
            $errors.Add(("header declares {0:N0} bytes, but file has {1:N0} bytes." -f $declared, $size))
        }
    }
    $infos.Add(("actual size: {0:N0} bytes" -f $size))

    if (($size % 0x4000) -ne 0) {
        $errors.Add("size ($size bytes) is not a multiple of 16 KiB.")
    }

    $rawTitle = New-Object System.Text.StringBuilder
    for ($i = 0x134; $i -lt 0x144; $i++) {
        $b = $bytes[$i]
        if ($b -eq 0) { break }
        if ($b -ge 32 -and $b -lt 127) { [void]$rawTitle.Append([char]$b) }
    }
    $title = ($rawTitle.ToString() -replace '\s+', ' ').Trim()
    if (-not $title) { $title = "(no title)" }
    $infos.Add("title: $title")

    $cgb = $bytes[0x143]
    $modeStr = switch ($cgb) {
        0x80 { "Game Boy Color (CGB)" }
        0xC0 { "Game Boy Color + Game Boy Advance" }
        default { "classic Game Boy (DMG)" }
    }
    $infos.Add("mode: $modeStr")

    $mbcMap = @{
        0x00="ROM"; 0x01="MBC1"; 0x02="MBC1+RAM"; 0x03="MBC1+RAM+BATTERY";
        0x05="MBC2"; 0x06="MBC2+BATTERY"; 0x08="ROM+RAM"; 0x09="ROM+RAM+BATTERY";
        0x10="MBC3"; 0x11="MBC3+RAM"; 0x12="MBC3+RAM+BATTERY"; 0x13="MBC3+BATTERY";
        0x19="MBC5"; 0x1A="MBC5+RAM"; 0x1B="MBC5+RAM+BATTERY";
        0x1C="MBC5+RUMBLE"; 0x1D="MBC5+RUMBLE+RAM"; 0x1E="MBC5+RUMBLE+RAM+BATTERY"
    }
    $cartCode = [int]$bytes[0x147]
    $cartName = if ($mbcMap.ContainsKey($cartCode)) { $mbcMap[$cartCode] } else { ("code 0x{0:X2}" -f $cartCode) }
    $infos.Add("cartridge: $cartName")

    $ramMap = @{ 0="none"; 1="2 KiB"; 2="8 KiB"; 3="32 KiB"; 4="128 KiB"; 5="64 KiB" }
    $ramCode = [int]$bytes[0x149]
    $ramName = if ($ramMap.ContainsKey($ramCode)) { $ramMap[$ramCode] } else { "code $ramCode" }
    $infos.Add("RAM: $ramName")

    return @{
        Ok     = ($errors.Count -eq 0)
        Errors = $errors
        Infos  = $infos
        Title  = $title
        Size   = $size
    }
}

function Find-MatchingProfile([string]$title, [int64]$sizeBytes) {
    $profDir = Join-Path $RepoDir "profiles"
    if (-not (Test-Path $profDir)) { return $null }
    foreach ($file in (Get-ChildItem -Path $profDir -Filter "*.json" | Sort-Object Name)) {
        try {
            $prof = Get-Content -Path $file.FullName -Raw -Encoding UTF8 | ConvertFrom-Json
            $wantTitle = $prof.title
            $wantSize  = $prof.size_bytes
            if ($null -eq $wantTitle -and $null -eq $wantSize) { continue }
            if ($null -ne $wantTitle -and $wantTitle -ne $title) { continue }
            if ($null -ne $wantSize -and [int64]$wantSize -ne $sizeBytes) { continue }
            return $prof
        } catch {
            continue
        }
    }
    return $null
}

function Invoke-TimedExe([string]$exePath, [string[]]$argList, [int]$timeoutSec, [string]$label) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exePath
    $psi.Arguments = (($argList | ForEach-Object {
        if ($_ -match '\s|"') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' ')
    $psi.WorkingDirectory = $RepoDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    [void]$proc.Start()
    $outTask = $proc.StandardOutput.ReadToEndAsync()
    $errTask = $proc.StandardError.ReadToEndAsync()
    $finished = $proc.WaitForExit($timeoutSec * 1000)
    if (-not $finished) {
        try { & taskkill /PID $proc.Id /T /F 2>$null | Out-Null } catch {}
        throw "$label timed out after ${timeoutSec}s (see $LogFile)"
    }
    $stdout = $outTask.Result
    $stderr = $errTask.Result
    if ($stdout) { Add-Content -Path $LogFile -Value $stdout }
    if ($stderr) { Add-Content -Path $LogFile -Value $stderr }
    if ($proc.ExitCode -ne 0) {
        $tailLines = @()
        foreach ($chunk in @($stdout, $stderr)) {
            if ($chunk) {
                $tailLines += ($chunk -split "`r?`n" | Where-Object { $_ -ne "" })
            }
        }
        if ($tailLines.Count -gt 20) {
            $tailLines = $tailLines[($tailLines.Count - 20)..($tailLines.Count - 1)]
        }
        $tailText = if ($tailLines.Count -gt 0) { "`n" + ($tailLines -join "`n") } else { "" }
        throw "$label failed with exit code $($proc.ExitCode) (see $LogFile)$tailText"
    }
}

# --------------------------------------------------------------- 1. Validate
$val = Test-GameBoyRom $RomPath
foreach ($line in $val.Infos) { Write-Host "    $line" }
if (-not $val.Ok) {
    Write-Host "`n  This ROM did not pass validation:" -ForegroundColor Red
    foreach ($e in $val.Errors) { Write-Host "    - $e" -ForegroundColor Red }
    exit 1
}
if ($ValidateOnly) {
    Write-Host "`n  Valid ROM. Ready to build." -ForegroundColor Green
    exit 0
}

$romAbs = (Resolve-Path $RomPath).Path
"" | Set-Content -Path $LogFile -Encoding UTF8

# --------------------------------------------------------------- 2. Auto-setup
$wantToolsVer = ""
if (Test-Path $PinsPath) {
    $inTools = $false
    foreach ($raw in Get-Content $PinsPath) {
        $t = $raw.Trim()
        if ($t -match '^\[') { $inTools = ($t -eq '[tools]') }
        elseif ($inTools -and $t -match '^version\s*=\s*(.+)$') { $wantToolsVer = $matches[1].Trim() }
    }
}
$haveVerPath = Join-Path $ToolsDir "VERSION"
$haveToolsVer = if (Test-Path $haveVerPath) { (Get-Content -Raw $haveVerPath).Trim() } else { "" }
$w64Bin = Join-Path $ToolsDir "w64devkit\bin"

if (-not (Test-Path (Join-Path $ToolsDir "gbrecomp.exe")) -or `
    -not (Test-Path (Join-Path $ToolsDir "libgbrt.a")) -or `
    -not (Test-Path (Join-Path $w64Bin "gcc.exe")) -or `
    ($wantToolsVer -and $haveToolsVer -ne $wantToolsVer)) {
    & (Join-Path $PSScriptRoot "bootstrap.ps1")
}

$env:PATH = "$w64Bin;$env:PATH"

$sdlDir = ""
foreach ($cand in @((Join-Path $RepoDir "sdl2"), (Join-Path $ToolsDir "sdl2"))) {
    if ((Test-Path (Join-Path $cand "include\SDL2\SDL.h")) -and (Test-Path (Join-Path $cand "bin\SDL2.dll"))) {
        $sdlDir = $cand
        break
    }
}
if (-not $sdlDir) {
    & (Join-Path $PSScriptRoot "bootstrap.ps1")
    $sdlDir = Join-Path $ToolsDir "sdl2"
}

# --------------------------------------------------------------- 3. Generate
Write-Host ""
Write-Host "==> Translating the ROM" -ForegroundColor Cyan
Write-Host "    -    this takes a minute or two; the ROM becomes ~280 C files"

$projDir = Join-Path $BuildDir "project"
$workDir = Join-Path $BuildDir "work"
if (Test-Path $projDir) { Remove-Item -Recurse -Force $projDir }
if (Test-Path $workDir) { Remove-Item -Recurse -Force $workDir }
New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null

$prof = Find-MatchingProfile $val.Title $val.Size
$profArgs = @()
$profName = ""
if ($null -ne $prof) {
    $profName = [string]$prof.name
    Write-Host "    -    analysis profile: $profName"
    if ($prof.gbrecomp_args) { $profArgs = @($prof.gbrecomp_args) }
}

$buildRemaster = ($ForceRemaster.IsPresent -or ($profName -eq "ghost-babel")) -and (Test-Path (Join-Path $RepoDir "remaster"))

$rtArgs = @()
foreach ($cand in @((Join-Path $RepoDir "runtime"), (Join-Path $ToolsDir "engine\runtime"))) {
    if (Test-Path (Join-Path $cand "include\gbrt.h")) {
        $rtArgs = @("--runtime-dir", $cand)
        break
    }
}

Write-Host "GBPHASE translate"
$gbExe = Join-Path $ToolsDir "gbrecomp.exe"
$gbAllArgs = @($romAbs, "-o", $projDir, "--output-prefix", "baserom") + $rtArgs + $profArgs
try {
    Invoke-TimedExe $gbExe $gbAllArgs 600 "gbrecomp.exe"
} catch {
    Write-Host "`nCode generation failed: $($_.Exception.Message)" -ForegroundColor Red
    if (Test-Path $LogFile) { Get-Content $LogFile -Tail 20 }
    exit 1
}
$nFiles = (Get-ChildItem -Path $projDir -Filter "*.c").Count
Write-Host "    ok   $nFiles C files generated" -ForegroundColor Green

# --------------------------------------------------------------- 4. Compile
Write-Host ""
Write-Host "==> Compiling (a few minutes)" -ForegroundColor Cyan
Write-Host "GBPHASE compile"

$jobs = [Math]::Min(8, [Math]::Max(2, [int]$env:NUMBER_OF_PROCESSORS))
$makeExe  = Join-Path $w64Bin "make.exe"
$sdlMake  = if ($sdlDir -eq (Join-Path $RepoDir "sdl2")) { "sdl2" } else { ".tools/sdl2" }
$makeArgs = @(
    "-f", "windows/Makefile.win",
    "--no-print-directory",
    "-j$jobs",
    ("BUILD_REMASTER=" + $(if ($buildRemaster) { "1" } else { "0" })),
    "ROM_SRC=build/project",
    "REMASTER_DIR=remaster",
    "TOOLS_DIR=.tools",
    "SDL2_DIR=$sdlMake"
)

$psiMake = New-Object System.Diagnostics.ProcessStartInfo
$psiMake.FileName = $makeExe
$psiMake.Arguments = (($makeArgs | ForEach-Object {
    if ($_ -match '\s|"') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
}) -join ' ')
$psiMake.WorkingDirectory = $RepoDir
$psiMake.UseShellExecute = $false
$psiMake.CreateNoWindow = $true
$psiMake.RedirectStandardOutput = $true
$psiMake.RedirectStandardError  = $true

$procMake = New-Object System.Diagnostics.Process
$procMake.StartInfo = $psiMake
[void]$procMake.Start()
$errTask = $procMake.StandardError.ReadToEndAsync()
while (-not $procMake.StandardOutput.EndOfStream) {
    $line = $procMake.StandardOutput.ReadLine()
    Write-Host $line
    Add-Content -Path $LogFile -Value $line
}
$procMake.WaitForExit()
$makeErr = $errTask.Result
if ($makeErr) {
    Add-Content -Path $LogFile -Value $makeErr
    Write-Host $makeErr
}
if ($procMake.ExitCode -ne 0) {
    Write-Host "`nCompilation failed (exit $($procMake.ExitCode)). See $LogFile" -ForegroundColor Red
    exit 1
}

Copy-Item -Force (Join-Path $sdlDir "bin\SDL2.dll") (Join-Path $BuildDir "SDL2.dll")

$authExe   = Join-Path $BuildDir "ghost-babel-authentic.exe"
$remastExe = Join-Path $BuildDir "ghost-babel-remaster.exe"
$singleExe = Join-Path $BuildDir "ghost-babel.exe"

if ($buildRemaster) {
    Write-Host ("    ok   remaster (256px): build\ghost-babel-remaster.exe ({0:N1} MiB)" -f ((Get-Item $remastExe).Length / 1MB)) -ForegroundColor Green
    Write-Host ("    ok   authentic (160px): build\ghost-babel-authentic.exe ({0:N1} MiB)" -f ((Get-Item $authExe).Length / 1MB)) -ForegroundColor Green
} else {
    Write-Host ("    ok   executable: build\ghost-babel.exe ({0:N1} MiB)" -f ((Get-Item $singleExe).Length / 1MB)) -ForegroundColor Green
}

# --------------------------------------------------------------- 5. Smoke test
Write-Host ""
Write-Host "==> Testing the result" -ForegroundColor Cyan

if ($SyntheticSmoke) {
    if ($buildRemaster) {
        Invoke-TimedExe $authExe @("--headless", "--limit", "200000") 60 "ghost-babel-authentic.exe"
        Write-Host "    ok   ghost-babel-authentic.exe executed synthetic guest instructions" -ForegroundColor Green
        Invoke-TimedExe $remastExe @("--help") 30 "ghost-babel-remaster.exe"
        Write-Host "    ok   ghost-babel-remaster.exe responded to --help" -ForegroundColor Green
    } else {
        Invoke-TimedExe $singleExe @("--headless", "--limit", "200000") 60 "ghost-babel.exe"
        Write-Host "    ok   ghost-babel.exe executed synthetic guest instructions" -ForegroundColor Green
    }
} else {
    if ($buildRemaster) {
        Invoke-TimedExe $authExe @("--headless", "--limit-frames", "120") 120 "ghost-babel-authentic.exe"
        Write-Host "    ok   ghost-babel-authentic.exe ran 120 frames without errors" -ForegroundColor Green
        Invoke-TimedExe $remastExe @("--headless", "--widescreen", "--limit-frames", "120") 120 "ghost-babel-remaster.exe"
        Write-Host "    ok   ghost-babel-remaster.exe ran 120 frames without errors" -ForegroundColor Green
    } else {
        Invoke-TimedExe $singleExe @("--headless", "--limit-frames", "120") 120 "ghost-babel.exe"
        Write-Host "    ok   ghost-babel.exe ran 120 frames without errors" -ForegroundColor Green
    }
}

Write-Host "GBPHASE done"

if (-not $NoPrompt) {
    $ans = Read-Host "`n  Delete intermediate build/project and build/work folders to free disk space? [Y/n]"
    if ($ans -notmatch '^(n|no)$') {
        if (Test-Path $projDir) { Remove-Item -Recurse -Force $projDir }
        if (Test-Path $workDir) { Remove-Item -Recurse -Force $workDir }
        Write-Host "  Removed intermediate build files. Your executables and saves are preserved."
    }
}

Write-Host ""
Write-Host "Done!" -ForegroundColor Green
if ($buildRemaster) {
    Write-Host "  Play Widescreen Remaster: run-remaster.bat   (or build\ghost-babel-remaster.exe)"
    Write-Host "  Play 160px Authentic:     run-authentic.bat  (or build\ghost-babel-authentic.exe)"
} else {
    Write-Host "  Play: build\ghost-babel.exe"
}
