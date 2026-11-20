<#
.SYNOPSIS
  Native Windows Forms GUI for Metal Gear: Ghost Babel Recomp (zero Python required).
#>
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

$ScriptRoot = $PSScriptRoot
$RepoDir    = Split-Path -Parent $ScriptRoot
$BuildPs1   = Join-Path $ScriptRoot "build.ps1"
$BootPs1    = Join-Path $ScriptRoot "bootstrap.ps1"
$IconPng    = Join-Path $RepoDir "tools\gui\icon.png"

$RemasterExe = Join-Path $RepoDir "build\ghost-babel-remaster.exe"
$AuthExe     = Join-Path $RepoDir "build\ghost-babel-authentic.exe"
$SingleExe   = Join-Path $RepoDir "build\ghost-babel.exe"

$form = New-Object System.Windows.Forms.Form
$form.Text = "Metal Gear: Ghost Babel - Recomp"
$form.ClientSize = New-Object System.Drawing.Size(740, 680)
$form.MinimumSize = New-Object System.Drawing.Size(640, 560)
$form.StartPosition = "CenterScreen"
$form.BackColor = [System.Drawing.Color]::FromArgb(244, 244, 245)
$form.Font = New-Object System.Drawing.Font("Segoe UI", 9.5)
$form.AllowDrop = $true

if (Test-Path $IconPng) {
    try {
        $bmp = [System.Drawing.Bitmap]::FromFile($IconPng)
        $form.Icon = [System.Drawing.Icon]::FromHandle($bmp.GetHicon())
    } catch {}
}

# Header
$lblTitle = New-Object System.Windows.Forms.Label
$lblTitle.Text = "Metal Gear: Ghost Babel - Recomp"
$lblTitle.Font = New-Object System.Drawing.Font("Segoe UI", 13, [System.Drawing.FontStyle]::Bold)
$lblTitle.Location = New-Object System.Drawing.Point(20, 16)
$lblTitle.AutoSize = $true
$form.Controls.Add($lblTitle)

$lblSub = New-Object System.Windows.Forms.Label
$lblSub.Text = "Select or drop your .gbc ROM to build Widescreen Remaster and 160px Authentic native executables."
$lblSub.ForeColor = [System.Drawing.Color]::FromArgb(85, 85, 85)
$lblSub.Location = New-Object System.Drawing.Point(20, 44)
$lblSub.AutoSize = $true
$form.Controls.Add($lblSub)

# Step 1: ROM Picker
$lblStep1 = New-Object System.Windows.Forms.Label
$lblStep1.Text = "1. Choose your ROM (or drag & drop a .gbc/.gb file here)"
$lblStep1.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
$lblStep1.Location = New-Object System.Drawing.Point(20, 78)
$lblStep1.AutoSize = $true
$form.Controls.Add($lblStep1)

$txtRom = New-Object System.Windows.Forms.TextBox
$txtRom.Location = New-Object System.Drawing.Point(20, 102)
$txtRom.Size = New-Object System.Drawing.Size(520, 26)
$txtRom.Anchor = "Top,Left,Right"
$form.Controls.Add($txtRom)

$btnBrowse = New-Object System.Windows.Forms.Button
$btnBrowse.Text = "Browse..."
$btnBrowse.Location = New-Object System.Drawing.Point(548, 100)
$btnBrowse.Size = New-Object System.Drawing.Size(84, 28)
$btnBrowse.Anchor = "Top,Right"
$form.Controls.Add($btnBrowse)

$btnCheck = New-Object System.Windows.Forms.Button
$btnCheck.Text = "Check"
$btnCheck.Location = New-Object System.Drawing.Point(638, 100)
$btnCheck.Size = New-Object System.Drawing.Size(82, 28)
$btnCheck.Anchor = "Top,Right"
$form.Controls.Add($btnCheck)

$lblStatus = New-Object System.Windows.Forms.Label
$lblStatus.Text = "Pick your Metal Gear: Ghost Babel .gbc ROM to begin."
$lblStatus.ForeColor = [System.Drawing.Color]::FromArgb(85, 85, 85)
$lblStatus.Location = New-Object System.Drawing.Point(20, 134)
$lblStatus.Size = New-Object System.Drawing.Size(700, 34)
$lblStatus.Anchor = "Top,Left,Right"
$form.Controls.Add($lblStatus)

# Step 2: Build & Play
$lblStep2 = New-Object System.Windows.Forms.Label
$lblStep2.Text = "2. Build & Play"
$lblStep2.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
$lblStep2.Location = New-Object System.Drawing.Point(20, 174)
$lblStep2.AutoSize = $true
$form.Controls.Add($lblStep2)

$lblProgress = New-Object System.Windows.Forms.Label
$lblProgress.Text = "Idle."
$lblProgress.ForeColor = [System.Drawing.Color]::FromArgb(85, 85, 85)
$lblProgress.Location = New-Object System.Drawing.Point(20, 198)
$lblProgress.Size = New-Object System.Drawing.Size(700, 20)
$form.Controls.Add($lblProgress)

$progressBar = New-Object System.Windows.Forms.ProgressBar
$progressBar.Location = New-Object System.Drawing.Point(20, 222)
$progressBar.Size = New-Object System.Drawing.Size(700, 20)
$progressBar.Anchor = "Top,Left,Right"
$progressBar.Minimum = 0
$progressBar.Maximum = 1000
$form.Controls.Add($progressBar)

$btnSetup = New-Object System.Windows.Forms.Button
$btnSetup.Text = "Setup Tools"
$btnSetup.Location = New-Object System.Drawing.Point(20, 252)
$btnSetup.Size = New-Object System.Drawing.Size(98, 30)
$form.Controls.Add($btnSetup)

$btnCompile = New-Object System.Windows.Forms.Button
$btnCompile.Text = "Compile"
$btnCompile.Location = New-Object System.Drawing.Point(124, 252)
$btnCompile.Size = New-Object System.Drawing.Size(92, 30)
$form.Controls.Add($btnCompile)

$btnCancel = New-Object System.Windows.Forms.Button
$btnCancel.Text = "Cancel"
$btnCancel.Location = New-Object System.Drawing.Point(222, 252)
$btnCancel.Size = New-Object System.Drawing.Size(76, 30)
$btnCancel.Enabled = $false
$form.Controls.Add($btnCancel)

$btnRemaster = New-Object System.Windows.Forms.Button
$btnRemaster.Text = "Play Remaster (Widescreen)"
$btnRemaster.Location = New-Object System.Drawing.Point(312, 252)
$btnRemaster.Size = New-Object System.Drawing.Size(172, 30)
$form.Controls.Add($btnRemaster)

$btnAuth = New-Object System.Windows.Forms.Button
$btnAuth.Text = "Play Authentic"
$btnAuth.Location = New-Object System.Drawing.Point(490, 252)
$btnAuth.Size = New-Object System.Drawing.Size(112, 30)
$form.Controls.Add($btnAuth)

$btnClean = New-Object System.Windows.Forms.Button
$btnClean.Text = "Free space"
$btnClean.Location = New-Object System.Drawing.Point(608, 252)
$btnClean.Size = New-Object System.Drawing.Size(112, 30)
$btnClean.Anchor = "Top,Right"
$form.Controls.Add($btnClean)

# Step 3: Output Log
$lblStep3 = New-Object System.Windows.Forms.Label
$lblStep3.Text = "3. Build Log"
$lblStep3.Font = New-Object System.Drawing.Font("Segoe UI", 9.5, [System.Drawing.FontStyle]::Bold)
$lblStep3.Location = New-Object System.Drawing.Point(20, 294)
$lblStep3.AutoSize = $true
$form.Controls.Add($lblStep3)

$txtLog = New-Object System.Windows.Forms.TextBox
$txtLog.Location = New-Object System.Drawing.Point(20, 318)
$txtLog.Size = New-Object System.Drawing.Size(700, 342)
$txtLog.Anchor = "Top,Bottom,Left,Right"
$txtLog.Multiline = $true
$txtLog.ReadOnly = $true
$txtLog.ScrollBars = "Both"
$txtLog.WordWrap = $false
$txtLog.BackColor = [System.Drawing.Color]::FromArgb(17, 17, 17)
$txtLog.ForeColor = [System.Drawing.Color]::FromArgb(221, 221, 221)
$txtLog.Font = New-Object System.Drawing.Font("Consolas", 9)
$form.Controls.Add($txtLog)

$script:activeProc = $null
$script:activeTask = ""

function Append-Log([string]$line) {
    if ($form.IsDisposed) { return }
    $txtLog.AppendText($line + [Environment]::NewLine)
}

function Refresh-Buttons {
    $running = ($null -ne $script:activeProc)
    $btnSetup.Enabled    = -not $running
    $btnCompile.Enabled  = -not $running
    $btnCancel.Enabled   = $running
    $btnBrowse.Enabled   = -not $running
    $btnCheck.Enabled    = -not $running
    $btnRemaster.Enabled = (-not $running) -and (Test-Path $RemasterExe)
    $btnAuth.Enabled     = (-not $running) -and ((Test-Path $AuthExe) -or (Test-Path $SingleExe))
    $hasWork             = (Test-Path (Join-Path $RepoDir "build\project")) -or (Test-Path (Join-Path $RepoDir "build\work"))
    $btnClean.Enabled    = (-not $running) -and $hasWork
}

function Set-Status([string]$text, [System.Drawing.Color]$color) {
    $lblStatus.Text = $text
    $lblStatus.ForeColor = $color
}

function Invoke-RomValidation {
    $rom = $txtRom.Text.Trim()
    if (-not $rom) {
        Set-Status "Choose a ROM file first." ([System.Drawing.Color]::DarkOrange)
        return $false
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$BuildPs1`" -ValidateOnly -RomPath `"$rom`""
    $psi.WorkingDirectory = $RepoDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true
    $p = [System.Diagnostics.Process]::Start($psi)
    $out = $p.StandardOutput.ReadToEnd() + $p.StandardError.ReadToEnd()
    $p.WaitForExit()
    Append-Log $out.Trim()
    if ($p.ExitCode -eq 0) {
        Set-Status "Valid Game Boy ROM. Ready to compile." ([System.Drawing.Color]::ForestGreen)
        return $true
    }
    Set-Status "Invalid ROM file. See log below." ([System.Drawing.Color]::Firebrick)
    return $false
}

function Handle-ProcessLine([string]$line) {
    if ($null -eq $line) { return }
    if ($line.StartsWith("GBPHASE ")) {
        $phase = $line.Substring(8).Trim()
        switch ($phase) {
            "translate" {
                $progressBar.Style = [System.Windows.Forms.ProgressBarStyle]::Marquee
                $lblProgress.Text = "Translating the ROM into C..."
            }
            "compile" {
                $progressBar.Style = [System.Windows.Forms.ProgressBarStyle]::Continuous
                $progressBar.Value = 0
                $lblProgress.Text = "Compiling generated files..."
            }
            "link" {
                $progressBar.Style = [System.Windows.Forms.ProgressBarStyle]::Continuous
                $progressBar.Value = 1000
                $lblProgress.Text = "Linking executable(s)..."
            }
            "done" {
                $progressBar.Style = [System.Windows.Forms.ProgressBarStyle]::Continuous
                $progressBar.Value = 1000
            }
        }
        return
    }
    if ($line.StartsWith("GBPROGRESS ")) {
        $parts = $line.Split(' ', [System.StringSplitOptions]::RemoveEmptyEntries)
        if ($parts.Length -eq 3) {
            $done  = [int]$parts[1]
            $total = [int]$parts[2]
            if ($total -gt 0) {
                $progressBar.Style = [System.Windows.Forms.ProgressBarStyle]::Continuous
                $progressBar.Value = [Math]::Min(1000, [int](($done * 1000) / $total))
                $pct = [int](($done * 100) / $total)
                $lblProgress.Text = "Compiling $done/$total files ($pct%)"
            }
        }
        return
    }
    if ($line.Trim()) {
        Append-Log $line
    }
}

function Start-BackgroundScript([string]$taskKind, [string]$scriptPath, [string]$argString, [string]$statusText) {
    if ($null -ne $script:activeProc) { return }
    $script:activeTask = $taskKind
    $txtLog.Clear()
    $progressBar.Style = [System.Windows.Forms.ProgressBarStyle]::Marquee
    $lblProgress.Text = "Starting..."
    Set-Status $statusText ([System.Drawing.Color]::RoyalBlue)

    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = "powershell.exe"
    $psi.Arguments = "-NoProfile -ExecutionPolicy Bypass -File `"$scriptPath`" $argString"
    $psi.WorkingDirectory = $RepoDir
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError  = $true

    $proc = New-Object System.Diagnostics.Process
    $proc.StartInfo = $psi
    $proc.EnableRaisingEvents = $true

    $outHandler = [System.Diagnostics.DataReceivedEventHandler]{
        param($sender, $e)
        if ($null -ne $e.Data -and -not $form.IsDisposed) {
            $form.BeginInvoke([Action[string]]{ param($ln) Handle-ProcessLine $ln }, $e.Data) | Out-Null
        }
    }
    $exitHandler = [System.EventHandler]{
        param($sender, $e)
        if (-not $form.IsDisposed) {
            $code = $sender.ExitCode
            $form.BeginInvoke([Action[int]]{
                param($rc)
                $progressBar.Style = [System.Windows.Forms.ProgressBarStyle]::Continuous
                $task = $script:activeTask
                $script:activeProc = $null
                $script:activeTask = ""
                Refresh-Buttons
                if ($rc -eq 0) {
                    $progressBar.Value = 1000
                    if ($task -eq "setup") {
                        $lblProgress.Text = "Tools ready."
                        Set-Status "Tools prepared. Choose a ROM and click Compile." ([System.Drawing.Color]::ForestGreen)
                    } else {
                        $lblProgress.Text = "Ready."
                        Set-Status "Done. Choose Play Remaster (Widescreen) or Play Authentic." ([System.Drawing.Color]::ForestGreen)
                    }
                } else {
                    $progressBar.Value = 0
                    $lblProgress.Text = "Failed."
                    Set-Status "Task stopped (exit $rc). Check log below." ([System.Drawing.Color]::Firebrick)
                }
            }, $code) | Out-Null
        }
    }

    $proc.add_OutputDataReceived($outHandler)
    $proc.add_ErrorDataReceived($outHandler)
    $proc.add_Exited($exitHandler)

    $script:activeProc = $proc
    Refresh-Buttons
    [void]$proc.Start()
    $proc.BeginOutputReadLine()
    $proc.BeginErrorReadLine()
}

# Events
$form.Add_DragEnter({
    param($s, $e)
    if ($e.Data.GetDataPresent([System.Windows.Forms.DataFormats]::FileDrop)) {
        $e.Effect = [System.Windows.Forms.DragDropEffects]::Copy
    }
})

$form.Add_DragDrop({
    param($s, $e)
    $files = $e.Data.GetData([System.Windows.Forms.DataFormats]::FileDrop)
    if ($files -and $files.Length -gt 0) {
        $txtRom.Text = $files[0]
        [void](Invoke-RomValidation)
    }
})

$btnBrowse.Add_Click({
    $dlg = New-Object System.Windows.Forms.OpenFileDialog
    $dlg.Title = "Select Game Boy Color ROM"
    $dlg.Filter = "Game Boy ROMs (*.gbc;*.gb)|*.gbc;*.gb|All Files (*.*)|*.*"
    if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
        $txtRom.Text = $dlg.FileName
        [void](Invoke-RomValidation)
    }
})

$btnCheck.Add_Click({ [void](Invoke-RomValidation) })

$btnSetup.Add_Click({
    Start-BackgroundScript "setup" $BootPs1 "" "Downloading / verifying w64devkit, SDL2, and compiler tools..."
})

$btnCompile.Add_Click({
    $rom = $txtRom.Text.Trim()
    if (-not $rom) {
        Set-Status "Choose a ROM file first." ([System.Drawing.Color]::DarkOrange)
        return
    }
    Start-BackgroundScript "build" $BuildPs1 "-NoPrompt -RomPath `"$rom`"" "Building native executables from your ROM (may download portable compiler on first run)..."
})

$btnCancel.Add_Click({
    if ($null -ne $script:activeProc) {
        Append-Log "Cancelling build..."
        try { & taskkill /PID $script:activeProc.Id /T /F 2>$null | Out-Null } catch {}
    }
})

$btnRemaster.Add_Click({
    if (Test-Path $RemasterExe) {
        Start-Process -FilePath $RemasterExe -ArgumentList @("--widescreen") -WorkingDirectory (Split-Path -Parent $RemasterExe)
    }
})

$btnAuth.Add_Click({
    $target = if (Test-Path $AuthExe) { $AuthExe } else { $SingleExe }
    if (Test-Path $target) {
        Start-Process -FilePath $target -WorkingDirectory (Split-Path -Parent $target)
    }
})

$btnClean.Add_Click({
    $freed = 0
    foreach ($sub in @("project", "work")) {
        $p = Join-Path $RepoDir "build\$sub"
        if (Test-Path $p) {
            $freed += (Get-ChildItem -Path $p -Recurse -File | Measure-Object -Property Length -Sum).Sum
            Remove-Item -Recurse -Force $p -ErrorAction SilentlyContinue
        }
    }
    Refresh-Buttons
    $mb = [Math]::Round($freed / 1MB, 1)
    $lblProgress.Text = "Build space freed."
    Append-Log "Removed intermediate build files (~$mb MB freed). Executables and saves are intact."
})

$form.Add_FormClosing({
    if ($null -ne $script:activeProc) {
        try { & taskkill /PID $script:activeProc.Id /T /F 2>$null | Out-Null } catch {}
    }
})

Refresh-Buttons
[void]$form.ShowDialog()
