# SMT NINE Turn-Based Edition - launcher window
$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$RepoUrl = 'https://github.com/Elivoltek/smt-nine-turnbased'
Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing
[System.Windows.Forms.Application]::EnableVisualStyles()

function Language-Ready([string]$Id) {
    $ini = Join-Path $Root "runtime\languages\$Id.ini"
    if (-not (Test-Path $ini)) { return $false }
    return ([IO.File]::ReadAllText($ini) -match '(?m)^\s*State\s*=\s*Ready\s*$')
}
function Installed { return (Test-Path (Join-Path $Root 'smt9_recomp.exe')) -and (Language-Ready 'English') }
function Run-Installer([string]$Arguments = '') {
    $script = Join-Path $PSScriptRoot 'Install.ps1'
    Start-Process powershell.exe -ArgumentList "-NoProfile -ExecutionPolicy Bypass -File `"$script`" $Arguments" -WorkingDirectory $Root
}

$navy = [System.Drawing.Color]::FromArgb(18, 28, 62)
$edge = [System.Drawing.Color]::FromArgb(110, 150, 205)
$gold = [System.Drawing.Color]::FromArgb(150, 130, 40)

$form = New-Object System.Windows.Forms.Form
$form.Text = 'SMT NINE - Turn-Based Edition'
$form.ClientSize = New-Object System.Drawing.Size(420, 470)
$form.StartPosition = 'CenterScreen'
$form.FormBorderStyle = 'FixedSingle'
$form.MaximizeBox = $false
$form.BackColor = [System.Drawing.Color]::FromArgb(8, 12, 28)
if (Test-Path (Join-Path $Root 'smt9_recomp.exe')) {
    try { $form.Icon = [System.Drawing.Icon]::ExtractAssociatedIcon((Join-Path $Root 'smt9_recomp.exe')) } catch {}
}

$title = New-Object System.Windows.Forms.Label
$title.Text = "SHIN MEGAMI TENSEI NINE"
$title.Font = New-Object System.Drawing.Font('Segoe UI', 17, [System.Drawing.FontStyle]::Bold)
$title.ForeColor = [System.Drawing.Color]::FromArgb(235, 240, 255)
$title.TextAlign = 'MiddleCenter'
$title.SetBounds(10, 14, 400, 36)
$form.Controls.Add($title)

$subtitle = New-Object System.Windows.Forms.Label
$subtitle.Text = 'Turn-Based Edition  -  English'
$subtitle.Font = New-Object System.Drawing.Font('Segoe UI', 11)
$subtitle.ForeColor = [System.Drawing.Color]::FromArgb(200, 170, 255)
$subtitle.TextAlign = 'MiddleCenter'
$subtitle.SetBounds(10, 50, 400, 24)
$form.Controls.Add($subtitle)

$status = New-Object System.Windows.Forms.Label
$status.Font = New-Object System.Drawing.Font('Segoe UI', 9)
$status.TextAlign = 'MiddleCenter'
$status.SetBounds(10, 80, 400, 36)
$form.Controls.Add($status)

$script:buttons = @{}
$y = 124
function Add-Button([string]$Key, [string]$Text, [scriptblock]$Action) {
    $button = New-Object System.Windows.Forms.Button
    $button.Text = $Text
    $button.Font = New-Object System.Drawing.Font('Segoe UI', 11, [System.Drawing.FontStyle]::Bold)
    $button.ForeColor = [System.Drawing.Color]::White
    $button.BackColor = $navy
    $button.FlatStyle = 'Flat'
    $button.FlatAppearance.BorderColor = $edge
    $button.FlatAppearance.BorderSize = 2
    $button.SetBounds(60, $script:y, 300, 40)
    $button.Add_Click($Action)
    $form.Controls.Add($button)
    $script:buttons[$Key] = $button
    $script:y += 48
}

Add-Button 'play' 'Play' {
    Start-Process (Join-Path $Root 'smt9_recomp.exe') -WorkingDirectory $Root
    $form.Close()
}
Add-Button 'install' 'Install / Patch my game...' { Run-Installer }
Add-Button 'settings' 'Settings (graphics, controls)' { Start-Process (Join-Path $Root 'Config.exe') -WorkingDirectory $Root }
Add-Button 'update' 'Update Turn-Based Edition' {
    if (Test-Path (Join-Path $Root '.git')) { Run-Installer '-Update' } else { Start-Process "$RepoUrl/releases" }
}
Add-Button 'saves' 'Open save folder' {
    $saves = Join-Path $Root 'save'
    New-Item -ItemType Directory -Force -Path $saves | Out-Null
    Start-Process explorer.exe $saves
}
Add-Button 'guide' 'How to play (battle controls)' {
    $guide = Join-Path $Root 'docs\TURN_BASED.md'
    if (Test-Path $guide) { Start-Process notepad.exe $guide } else { Start-Process $RepoUrl }
}

$credit = New-Object System.Windows.Forms.Label
$credit.Text = 'Fan project. Needs your own Japanese disc image. Not affiliated with Atlus/SEGA.'
$credit.Font = New-Object System.Drawing.Font('Segoe UI', 7.5)
$credit.ForeColor = [System.Drawing.Color]::FromArgb(120, 130, 160)
$credit.TextAlign = 'MiddleCenter'
$credit.SetBounds(10, 438, 400, 24)
$form.Controls.Add($credit)

function Refresh-Status {
    $ready = Installed
    $built = Test-Path (Join-Path $Root 'smt9_recomp.exe')
    if ($ready) {
        $status.Text = 'Ready to play.'
        $status.ForeColor = [System.Drawing.Color]::FromArgb(120, 240, 120)
        $script:buttons['install'].Text = 'Repair / Re-patch...'
    } elseif ($built) {
        $status.Text = "The game is built, but English is not installed yet.`nClick 'Install / Patch my game' to finish."
        $status.ForeColor = [System.Drawing.Color]::FromArgb(245, 225, 120)
    } else {
        $status.Text = "Not installed yet. Click 'Install / Patch my game'`nand select your Japanese SMT NINE ISO."
        $status.ForeColor = [System.Drawing.Color]::FromArgb(245, 225, 120)
    }
    $script:buttons['play'].Enabled = $built
    $script:buttons['play'].BackColor = if ($ready) { $gold } else { $navy }
    $script:buttons['settings'].Enabled = Test-Path (Join-Path $Root 'Config.exe')
}
Refresh-Status
$timer = New-Object System.Windows.Forms.Timer
$timer.Interval = 3000
$timer.Add_Tick({ Refresh-Status })
$timer.Start()
[void]$form.ShowDialog()
