<#
.SYNOPSIS
  Prepares the Windows build toolchain (gbrecomp.exe, libgbrt.a, SDL2, w64devkit).
.DESCRIPTION
  Reads pins.lock and ensures .tools/ contains:
    - gbrecomp.exe and libgbrt.a (from bundled bin/lib, release download, or source build)
    - portable w64devkit (gcc, g++, make, sh) verified by SHA-256
    - SDL2 MinGW development headers/libs and SDL2.dll (bundled or downloaded & verified)
#>
[CmdletBinding()]
param(
    [switch]$FromSource
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$RepoDir  = Split-Path -Parent $PSScriptRoot
$ToolsDir = Join-Path $RepoDir ".tools"
$PinsPath = Join-Path $RepoDir "pins.lock"
$LogFile  = Join-Path $RepoDir ".bootstrap.log"

if (-not (Test-Path $PinsPath)) {
    throw "ERROR: pins.lock not found in $RepoDir"
}

New-Item -ItemType Directory -Force -Path $ToolsDir | Out-Null
"" | Set-Content -Path $LogFile -Encoding UTF8

function Write-Step([string]$msg) {
    Write-Host ""
    Write-Host "==> $msg" -ForegroundColor Cyan
    Add-Content -Path $LogFile -Value "[STEP] $msg"
}

function Write-Ok([string]$msg) {
    Write-Host "    ok   $msg" -ForegroundColor Green
    Add-Content -Path $LogFile -Value "[OK] $msg"
}

function Write-Info([string]$msg) {
    Write-Host "    -    $msg"
    Add-Content -Path $LogFile -Value "[INFO] $msg"
}

function Read-Pins([string]$path) {
    $map = @{}
    $section = ""
    foreach ($raw in Get-Content -Path $path -Encoding UTF8) {
        $line = $raw.Trim()
        if (-not $line -or $line.StartsWith("#")) { continue }
        if ($line -match '^\[([^\]]+)\]$') {
            $section = $matches[1].Trim()
            continue
        }
        $eq = $line.IndexOf('=')
        if ($eq -gt 0 -and $section) {
            $key = $line.Substring(0, $eq).Trim()
            $val = $line.Substring($eq + 1).Trim()
            $map["$section.$key"] = $val
        }
    }
    return $map
}

function Get-FileSha256Lower([string]$path) {
    return (Get-FileHash -Algorithm SHA256 -Path $path).Hash.ToLowerInvariant()
}

function Invoke-VerifiedDownload([string]$url, [string]$dest, [string]$expectedSha256, [string]$label) {
    Write-Info "downloading $label from $url"
    if (Test-Path $dest) { Remove-Item -Force $dest }
    Invoke-WebRequest -Uri $url -OutFile $dest -UseBasicParsing
    if ($expectedSha256) {
        $got = Get-FileSha256Lower $dest
        if ($got -ne $expectedSha256.ToLowerInvariant()) {
            Remove-Item -Force $dest -ErrorAction SilentlyContinue
            throw "SHA-256 mismatch for $label (expected $expectedSha256, got $got)"
        }
        Write-Ok "$label checksum verified"
    }
}

function Invoke-Logged([string]$exe, [string[]]$argList, [string]$workDir = $RepoDir) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $exe
    $psi.Arguments = (($argList | ForEach-Object {
        if ($_ -match '\s|"') { '"' + ($_ -replace '"', '\"') + '"' } else { $_ }
    }) -join ' ')
    $psi.WorkingDirectory = $workDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true

    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    $p.WaitForExit()
    $out = $outTask.Result
    $err = $errTask.Result
    if ($out) { Add-Content -Path $LogFile -Value $out }
    if ($err) { Add-Content -Path $LogFile -Value $err }
    return $p.ExitCode
}

function Throw-WithLogTail([string]$prefix) {
    $tail = ""
    if (Test-Path $LogFile) {
        $lines = Get-Content -Path $LogFile -Tail 40 -ErrorAction SilentlyContinue
        if ($lines) {
            $tail = "`n" + ($lines -join "`n")
        }
    }
    throw "$prefix (see $LogFile)$tail"
}

$pins = Read-Pins $PinsPath
$toolsVersion   = $pins["tools.version"]
$releaseBase    = $pins["tools.release_base"]
$w64Ver         = $pins["windows.w64devkit_version"]
$w64Url         = $pins["windows.w64devkit_url"]
$w64Sha         = $pins["windows.w64devkit_sha256"]
$sdlVer         = $pins["windows.sdl2_version"]
$sdlUrl         = $pins["windows.sdl2_url"]
$sdlSha         = $pins["windows.sdl2_sha256"]
$engineUrl      = $pins["engine.url"]
$engineCommit   = $pins["engine.commit"]
$enginePatchRel = $pins["engine.patch"]
$enginePatchSha = $pins["engine.patch_sha256"]
$engineTreeWant = $pins["engine.tree_after"]

$buildFromSource = $FromSource.IsPresent -or ($env:GB_BUILD_FROM_SOURCE -eq "1")
if ($buildFromSource) {
    $releaseBase = ""
}

$versionFile = Join-Path $ToolsDir "VERSION"
$installedVer = if (Test-Path $versionFile) { (Get-Content -Raw $versionFile).Trim() } else { "" }

# --------------------------------------------------------------- 1. w64devkit
Write-Step "Checking portable C/C++ compiler (w64devkit)"
$w64Dir = Join-Path $ToolsDir "w64devkit"
$w64Bin = Join-Path $w64Dir "bin"
$w64VerFile = Join-Path $ToolsDir "w64devkit.version"
$w64Installed = if (Test-Path $w64VerFile) { (Get-Content -Raw $w64VerFile).Trim() } else { "" }

$haveCompiler = (Test-Path (Join-Path $w64Bin "gcc.exe")) -and `
                (Test-Path (Join-Path $w64Bin "g++.exe")) -and `
                (Test-Path (Join-Path $w64Bin "make.exe")) -and `
                (Test-Path (Join-Path $w64Bin "sh.exe")) -and `
                ($w64Installed -eq $w64Ver)

if (-not $haveCompiler) {
    $installer = Join-Path $ToolsDir "w64devkit-sfx.exe"
    $tmpExtract = Join-Path $ToolsDir "w64devkit-tmp"
    if (Test-Path $tmpExtract) { Remove-Item -Recurse -Force $tmpExtract }
    if (Test-Path $w64Dir) { Remove-Item -Recurse -Force $w64Dir }
    New-Item -ItemType Directory -Force -Path $tmpExtract | Out-Null

    Invoke-VerifiedDownload $w64Url $installer $w64Sha "w64devkit $w64Ver"
    Write-Info "extracting portable compiler toolchain (one-time)..."
    $proc = Start-Process -FilePath $installer -ArgumentList @("-y", "-o$tmpExtract") -Wait -PassThru -NoNewWindow
    if ($proc.ExitCode -ne 0) {
        throw "Failed to extract w64devkit (exit code $($proc.ExitCode))"
    }
    Remove-Item -Force $installer -ErrorAction SilentlyContinue

    $makeMatch = Get-ChildItem -Path $tmpExtract -Filter "make.exe" -Recurse -File |
                 Where-Object {
                     (Test-Path (Join-Path $_.Directory.FullName "gcc.exe")) -and
                     (Test-Path (Join-Path $_.Directory.FullName "sh.exe"))
                 } |
                 Select-Object -First 1
    if (-not $makeMatch) {
        throw "w64devkit extracted, but bin\{make,gcc,sh}.exe were not found under $tmpExtract"
    }
    $foundBin = $makeMatch.Directory.FullName
    $foundRoot = Split-Path -Parent $foundBin
    Move-Item -Path $foundRoot -Destination $w64Dir
    if (Test-Path $tmpExtract) { Remove-Item -Recurse -Force $tmpExtract -ErrorAction SilentlyContinue }

    foreach ($req in @("gcc.exe", "g++.exe", "make.exe", "sh.exe", "ar.exe", "strip.exe")) {
        if (-not (Test-Path (Join-Path $w64Bin $req))) {
            throw "Missing required tool '$req' in $w64Bin"
        }
    }
    Set-Content -Path $w64VerFile -Value $w64Ver -Encoding UTF8
    Write-Ok "w64devkit $w64Ver ready in .tools/w64devkit"
} else {
    Write-Ok "w64devkit $w64Ver already ready"
}

$env:PATH = "$w64Bin;$env:PATH"

# --------------------------------------------------------------- 2. SDL2 MinGW
Write-Step "Checking SDL2 MinGW libraries"
$bundledSdl = Join-Path $RepoDir "sdl2"
$cachedSdl  = Join-Path $ToolsDir "sdl2"
$sdlDir = ""

if ((Test-Path (Join-Path $bundledSdl "include\SDL2\SDL.h")) -and (Test-Path (Join-Path $bundledSdl "bin\SDL2.dll"))) {
    $sdlDir = $bundledSdl
    Write-Ok "using bundled SDL2 ($sdlVer)"
} elseif ((Test-Path (Join-Path $cachedSdl "include\SDL2\SDL.h")) -and (Test-Path (Join-Path $cachedSdl "bin\SDL2.dll"))) {
    $sdlDir = $cachedSdl
    Write-Ok "using cached SDL2 ($sdlVer)"
} else {
    $sdlZip = Join-Path $ToolsDir "sdl2-mingw.zip"
    $sdlTmp = Join-Path $ToolsDir "sdl2-tmp"
    if (Test-Path $sdlTmp) { Remove-Item -Recurse -Force $sdlTmp }
    if (Test-Path $cachedSdl) { Remove-Item -Recurse -Force $cachedSdl }

    Invoke-VerifiedDownload $sdlUrl $sdlZip $sdlSha "SDL2 MinGW $sdlVer"
    Expand-Archive -Path $sdlZip -DestinationPath $sdlTmp -Force
    Remove-Item -Force $sdlZip -ErrorAction SilentlyContinue

    $hdr = Get-ChildItem -Path $sdlTmp -Filter "SDL.h" -Recurse -File |
           Where-Object { $_.FullName -match 'x86_64-w64-mingw32' } |
           Select-Object -First 1
    if (-not $hdr) {
        $hdr = Get-ChildItem -Path $sdlTmp -Filter "SDL.h" -Recurse -File | Select-Object -First 1
    }
    if (-not $hdr) {
        throw "Could not find SDL.h inside $sdlTmp"
    }
    $mingwRoot = Split-Path -Parent (Split-Path -Parent $hdr.Directory.FullName)
    Move-Item -Path $mingwRoot -Destination $cachedSdl
    $licenseFile = Get-ChildItem -Path $sdlTmp -Filter "LICENSE*" -Recurse -File | Select-Object -First 1
    if ($licenseFile) {
        Copy-Item -Force $licenseFile.FullName (Join-Path $cachedSdl "LICENSE.txt")
    }
    Remove-Item -Recurse -Force $sdlTmp -ErrorAction SilentlyContinue
    $sdlDir = $cachedSdl
    Write-Ok "SDL2 $sdlVer installed into .tools/sdl2"
}

# --------------------------------------------------------------- 3. gbrecomp + libgbrt.a
Write-Step "Preparing gbrecomp and runtime library"
$gbrecompDst = Join-Path $ToolsDir "gbrecomp.exe"
$libDst      = Join-Path $ToolsDir "libgbrt.a"
$bundledExe  = Join-Path $RepoDir "bin\gbrecomp.exe"
$bundledLib  = Join-Path $RepoDir "lib\libgbrt.a"

$haveTools = (-not $buildFromSource) -and (Test-Path $gbrecompDst) -and (Test-Path $libDst) -and ($installedVer -eq $toolsVersion)

if ($haveTools) {
    Write-Ok "tools v$toolsVersion already ready"
    exit 0
}

if ((-not $buildFromSource) -and (Test-Path $bundledExe) -and (Test-Path $bundledLib)) {
    Copy-Item -Force $bundledExe $gbrecompDst
    Copy-Item -Force $bundledLib $libDst
    Set-Content -Path $versionFile -Value $toolsVersion -Encoding UTF8
    Write-Ok "release tools v$toolsVersion installed into .tools/"
    exit 0
}

if ((-not $buildFromSource) -and $releaseBase) {
    $zipUrl = "$releaseBase/ghost-babel-tools-windows-x86_64-$toolsVersion.zip"
    $zipPath = Join-Path $ToolsDir "tools-win.zip"
    $zipTmp  = Join-Path $ToolsDir "tools-tmp"
    if (Test-Path $zipTmp) { Remove-Item -Recurse -Force $zipTmp }

    Invoke-VerifiedDownload $zipUrl $zipPath "" "Windows toolchain v$toolsVersion"
    Expand-Archive -Path $zipPath -DestinationPath $zipTmp -Force
    Remove-Item -Force $zipPath -ErrorAction SilentlyContinue

    $pkgRoot = Join-Path $zipTmp "ghost-babel-tools"
    if (-not (Test-Path $pkgRoot)) { $pkgRoot = $zipTmp }

    $pkgExe = Join-Path $pkgRoot "bin\gbrecomp.exe"
    $pkgLib = Join-Path $pkgRoot "lib\libgbrt.a"
    if (-not (Test-Path $pkgExe) -or -not (Test-Path $pkgLib)) {
        throw "Downloaded release zip did not contain bin/gbrecomp.exe and lib/libgbrt.a"
    }
    Copy-Item -Force $pkgExe $gbrecompDst
    Copy-Item -Force $pkgLib $libDst

    foreach ($dirName in @("runtime", "sdl2")) {
        $srcSub = Join-Path $pkgRoot $dirName
        $dstSub = Join-Path $RepoDir $dirName
        if (Test-Path $srcSub) {
            if (Test-Path $dstSub) { Remove-Item -Recurse -Force $dstSub }
            Move-Item -Path $srcSub -Destination $dstSub
        }
    }
    Remove-Item -Recurse -Force $zipTmp -ErrorAction SilentlyContinue
    Set-Content -Path $versionFile -Value $toolsVersion -Encoding UTF8
    Write-Ok "downloaded prebuilt tools v$toolsVersion"
    exit 0
}

# Build from source (CI or offline developer with cmake + git)
Write-Info "building gbrecomp.exe and libgbrt.a from pinned upstream + patch"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    throw "git is required to build tools from source"
}
if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake is required to build tools from source"
}

$engineDir = Join-Path $ToolsDir "engine"
if (Test-Path (Join-Path $engineDir ".git")) {
    Write-Ok "engine checkout already present"
} else {
    if (Test-Path $engineDir) { Remove-Item -Recurse -Force $engineDir }
    Write-Info "cloning $engineUrl"
    if ((Invoke-Logged "git" @("clone", "-q", "--filter=blob:none", $engineUrl, $engineDir)) -ne 0) {
        throw "git clone failed (see $LogFile)"
    }
    if ((Invoke-Logged "git" @("-C", $engineDir, "checkout", "-q", $engineCommit)) -ne 0) {
        throw "git checkout $engineCommit failed (see $LogFile)"
    }
}

$patchFile = Join-Path $RepoDir $enginePatchRel
if (-not (Test-Path $patchFile)) { throw "missing patch: $patchFile" }
$gotPatchSha = Get-FileSha256Lower $patchFile
if ($gotPatchSha -ne $enginePatchSha.ToLowerInvariant()) {
    throw "engine patch sha256 mismatch ($gotPatchSha vs $enginePatchSha)"
}

$prevEap = $ErrorActionPreference
$ErrorActionPreference = "Continue"
$treeNow = (& git -C $engineDir rev-parse "HEAD^{tree}" 2>$null)
$headNow = (& git -C $engineDir rev-parse HEAD 2>$null)
$ErrorActionPreference = $prevEap
if ($treeNow -ne $engineTreeWant) {
    if ($headNow -ne $engineCommit) {
        [void](Invoke-Logged "git" @("-C", $engineDir, "am", "--abort"))
        [void](Invoke-Logged "git" @("-C", $engineDir, "reset", "-q", "--hard", $engineCommit))
    }
    if ((Invoke-Logged "git" @("-C", $engineDir, "-c", "user.name=Ghost Babel Recomp", "-c", "user.email=build@localhost", "am", "-q", $patchFile)) -ne 0) {
        throw "git am failed on $patchFile (see $LogFile)"
    }
    $treeNow = (& git -C $engineDir rev-parse "HEAD^{tree}")
    if ($treeNow -ne $engineTreeWant) {
        throw "engine tree mismatch after patch (expected $engineTreeWant, got $treeNow)"
    }
}
Write-Ok "engine patch applied and tree hash verified ($treeNow)"

$gccExe  = (Join-Path $w64Bin "gcc.exe").Replace('\', '/')
$gxxExe  = (Join-Path $w64Bin "g++.exe").Replace('\', '/')
$makeExe = (Join-Path $w64Bin "make.exe").Replace('\', '/')
$engBuild = Join-Path $engineDir "build-win"
if (Test-Path $engBuild) { Remove-Item -Recurse -Force $engBuild }

$gen = if (Get-Command ninja -ErrorAction SilentlyContinue) { "Ninja" } else { "MSYS Makefiles" }
$genArgs = @("-G", $gen, "-S", $engineDir, "-B", $engBuild,
    "-DCMAKE_BUILD_TYPE=Release",
    "-DBUILD_TESTS=OFF",
    "-DCMAKE_C_COMPILER=$gccExe",
    "-DCMAKE_CXX_COMPILER=$gxxExe",
    "-DCMAKE_MAKE_PROGRAM=$makeExe",
    "-DCMAKE_EXE_LINKER_FLAGS=-static -static-libgcc -static-libstdc++")
if ((Invoke-Logged "cmake" $genArgs) -ne 0) { Throw-WithLogTail "cmake configure failed" }
if ((Invoke-Logged "cmake" @("--build", $engBuild, "--target", "gbrecomp", "-j", $env:NUMBER_OF_PROCESSORS)) -ne 0) {
    Throw-WithLogTail "cmake build gbrecomp failed"
}

$builtExe = Get-ChildItem -Path $engBuild -Filter "gbrecomp.exe" -Recurse -File | Select-Object -First 1
if (-not $builtExe) { throw "gbrecomp.exe not found under $engBuild" }
Copy-Item -Force $builtExe.FullName $gbrecompDst
[void](Invoke-Logged (Join-Path $w64Bin "strip.exe") @($gbrecompDst))
Write-Ok "compiled static gbrecomp.exe"

# Compile libgbrt.a with MinGW + SDL2
$rtDir  = Join-Path $engineDir "runtime"
$objDir = Join-Path $ToolsDir ".obj-win"
if (Test-Path $objDir) { Remove-Item -Recurse -Force $objDir }
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

$sdlInc1 = Join-Path $sdlDir "include\SDL2"
$sdlInc2 = Join-Path $sdlDir "include"
$rtInc   = Join-Path $rtDir "include"
$imguiD  = Join-Path $rtDir "vendor\imgui"
$imguiB  = Join-Path $imguiD "backends"

foreach ($f in Get-ChildItem -Path (Join-Path $rtDir "src") -Filter "*.c") {
    $base = [System.IO.Path]::GetFileNameWithoutExtension($f.Name)
    if ($base -eq "gbrt_gameplay_mutation" -or $base -eq "gbrt_native_patch") { continue }
    $obj = Join-Path $objDir "$base.o"
    $rc = Invoke-Logged $gccExe @("-std=gnu11", "-O3", "-ffunction-sections", "-fdata-sections",
        "-DGB_HAS_SDL2", "-DGB_PLATFORM_WINDOWS", "-I$rtInc", "-I$sdlInc1", "-I$sdlInc2",
        "-c", $f.FullName, "-o", $obj)
    if ($rc -ne 0) { Throw-WithLogTail "gcc failed on $($f.Name)" }
}

$cppFiles = @(
    (Join-Path $rtDir "src\platform_sdl.cpp"),
    (Join-Path $imguiD "imgui.cpp"),
    (Join-Path $imguiD "imgui_draw.cpp"),
    (Join-Path $imguiD "imgui_tables.cpp"),
    (Join-Path $imguiD "imgui_widgets.cpp"),
    (Join-Path $imguiB "imgui_impl_sdl2.cpp"),
    (Join-Path $imguiB "imgui_impl_sdlrenderer2.cpp")
)
foreach ($cf in $cppFiles) {
    $base = [System.IO.Path]::GetFileNameWithoutExtension($cf)
    $obj = Join-Path $objDir "$base.o"
    $rc = Invoke-Logged $gxxExe @("-std=c++20", "-O3", "-ffunction-sections", "-fdata-sections",
        "-DGB_HAS_SDL2", "-DGB_PLATFORM_WINDOWS", "-I$rtInc", "-I$imguiD", "-I$imguiB",
        "-I$sdlInc1", "-I$sdlInc2", "-c", $cf, "-o", $obj)
    if ($rc -ne 0) { Throw-WithLogTail "g++ failed on $cf" }
}

if (Test-Path $libDst) { Remove-Item -Force $libDst }
$allObjs = @("rcs", $libDst) + (Get-ChildItem -Path $objDir -Filter "*.o").FullName
if ((Invoke-Logged (Join-Path $w64Bin "ar.exe") $allObjs) -ne 0) {
    throw "ar failed building libgbrt.a (see $LogFile)"
}
Remove-Item -Recurse -Force $objDir -ErrorAction SilentlyContinue

Set-Content -Path $versionFile -Value $toolsVersion -Encoding UTF8
Write-Ok "built libgbrt.a from source (v$toolsVersion)"
