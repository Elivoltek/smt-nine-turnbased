# SMT NINE Turn-Based Edition - patcher / installer
#
# Turns the player's own Japanese SMT NINE disc image into the English,
# turn-based PC version:
#   1. checks the build tools (offers to install missing ones with winget)
#   2. downloads the fan English patcher (negative-entrophy/smt-nine-english-patch,
#      MIT) from its official GitHub release and checks its SHA-256
#   3. makes an English copy of the player's ISO (the original stays untouched)
#   4. builds the native game from the Japanese ISO (turn-based mod included)
#   5. imports the English copy as the "English" language and selects it
#
# No game data is included in this project or downloaded by this script.
# Usage:  Install.ps1 [-Iso <path>] [-Update] [-Reimport]

param(
    [string]$Iso = '',
    [switch]$Update,
    [switch]$Reimport
)

$ErrorActionPreference = 'Stop'
$Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$Work = Join-Path $Root 'data\turnbased'
$PatcherVersion = 'v0.92'
$PatcherName = "SMT9_EN_Patcher_$PatcherVersion.exe"
$PatcherUrl = "https://github.com/negative-entrophy/smt-nine-english-patch/releases/download/$PatcherVersion/$PatcherName"
$PatcherSha256 = 'FB48E7271A5C2DBA69BA8E683AB57997374A280F293E87E125C0E1744B5344CE'
$Host.UI.RawUI.WindowTitle = 'SMT NINE Turn-Based - Installer'

function Say([string]$Text, [string]$Color = 'Gray') { Write-Host $Text -ForegroundColor $Color }
function Step([string]$Text) { Write-Host ''; Write-Host "==> $Text" -ForegroundColor Cyan }
function Fail([string]$Text) {
    Write-Host ''; Write-Host "ERROR: $Text" -ForegroundColor Red
    Write-Host 'Nothing else was changed. Fix the problem above and run the installer again.' -ForegroundColor Yellow
    Read-Host 'Press Enter to close'
    exit 1
}
function Ask([string]$Question) {
    $answer = Read-Host "$Question [Y/N]"
    return $answer -match '^(y|yes|j|ja)$'
}
function Refresh-Path {
    $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
    $user = [Environment]::GetEnvironmentVariable('Path', 'User')
    $env:Path = "$machine;$user;$env:USERPROFILE\.cargo\bin"
}
function Has-Command([string]$Name) { return [bool](Get-Command $Name -ErrorAction SilentlyContinue) }
function Winget-Install([string]$Id, [string[]]$Extra = @()) {
    if (-not (Has-Command 'winget')) { Fail "winget is not available. Install '$Id' manually (see docs\TURN_BASED.md), then run this again." }
    & winget install --id $Id -e --accept-package-agreements --accept-source-agreements @Extra
    Refresh-Path
}

Add-Type -Namespace Nine -Name Ini -MemberDefinition @'
[DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool WritePrivateProfileString(string section, string key, string value, string file);
'@

function Language-Ready([string]$Id) {
    $ini = Join-Path $Root "runtime\languages\$Id.ini"
    if (-not (Test-Path $ini)) { return $false }
    return ([IO.File]::ReadAllText($ini) -match '(?m)^\s*State\s*=\s*Ready\s*$')
}
function Base-Built { return (Test-Path (Join-Path $Root 'smt9_recomp.exe')) -and (Test-Path (Join-Path $Root 'runtime\native_game_jp.xbe')) }

Say ''
Say '  SHIN MEGAMI TENSEI NINE  -  Turn-Based Edition' 'Magenta'
Say '  English (fan translation) + traditional SMT turn-based battles' 'Magenta'
Say '  You need your own Japanese SMT NINE disc image. No game data is included.' 'DarkGray'

New-Item -ItemType Directory -Force -Path $Work | Out-Null
Start-Transcript -Path (Join-Path $Work 'install.log') -Append | Out-Null
Refresh-Path

# --- Update mode: pull the newest edition and rebuild ---------------------------
if ($Update) {
    Step 'Updating the Turn-Based Edition'
    if (-not (Test-Path (Join-Path $Root '.git'))) { Fail 'This folder was not downloaded with git. Download the newest version from GitHub instead.' }
    if (-not (Base-Built)) { Fail 'The game is not installed yet. Run "Install / Patch game" first.' }
    & git -C $Root pull --ff-only
    if ($LASTEXITCODE -ne 0) { Fail 'git pull failed.' }
    & (Join-Path $Root 'update.cmd')
    if ($LASTEXITCODE -ne 0) { Fail 'The update build failed. See the messages above.' }
    Say 'Update complete. Start the game from the launcher.' 'Green'
    Read-Host 'Press Enter to close'; exit 0
}

# --- 1. Free space and tools -----------------------------------------------------
Step 'Checking disk space and build tools'
$drive = Get-PSDrive -Name ($Root.Substring(0, 1))
$freeGb = [math]::Floor($drive.Free / 1GB)
Say "Free space on drive $($drive.Name): $freeGb GB"
if (-not (Base-Built) -and $freeGb -lt 25) {
    if (-not (Ask "Less than 25 GB free. The build needs about 15-25 GB. Continue anyway?")) { Fail 'Not enough free disk space.' }
}

# Python 3.14
$pythonOk = $false
if (Has-Command 'py') { cmd /c 'py -3.14 --version >nul 2>&1'; $pythonOk = ($LASTEXITCODE -eq 0) }
if (-not $pythonOk) {
    Say 'Python 3.14 is missing.' 'Yellow'
    if (Ask 'Install Python 3.14 now with winget?') { Winget-Install 'Python.Python.3.14' }
    cmd /c 'py -3.14 --version >nul 2>&1'
    if ($LASTEXITCODE -ne 0) { Fail 'Python 3.14 is still not available (the "py -3.14" launcher). Install it from python.org, then run this again.' }
}
Say 'Python 3.14 ........ OK' 'Green'

# Rustup
if (-not (Has-Command 'rustup')) {
    Say 'Rustup (Rust toolchain installer) is missing.' 'Yellow'
    if (Ask 'Install Rustup now with winget?') { Winget-Install 'Rustlang.Rustup' }
    if (-not (Has-Command 'rustup')) { Fail 'Rustup is still not available. Install it from https://rustup.rs, close this window and run again.' }
}
Say 'Rustup ............. OK' 'Green'

# Visual Studio 2022 Build Tools (C++)
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
function Has-VS {
    if (-not (Test-Path $vswhere)) { return $false }
    $path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    return [bool]$path
}
if (-not (Has-VS)) {
    Say 'Visual Studio 2022 Build Tools (C++) are missing. This is a large download (several GB).' 'Yellow'
    if (Ask 'Install them now with winget?') {
        Winget-Install 'Microsoft.VisualStudio.2022.BuildTools' @('--override', '--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --add Microsoft.VisualStudio.Component.VC.CMake.Project')
    }
    if (-not (Has-VS)) { Fail 'Visual Studio 2022 Build Tools with "Desktop development with C++" are still missing.' }
}
Say 'C++ Build Tools .... OK' 'Green'

# --- 2. The player's Japanese ISO ------------------------------------------------
$needBase = -not (Base-Built)
$needEnglish = $Reimport -or -not (Language-Ready 'English')
if (-not $needBase -and -not $needEnglish) {
    Step 'Already installed'
    Say 'The English turn-based game is already installed. Use "Play" in the launcher.' 'Green'
    Say 'To redo the English part, run: Install.ps1 -Reimport' 'DarkGray'
    Read-Host 'Press Enter to close'; exit 0
}

Step 'Select your Japanese SMT NINE disc image'
if (-not $Iso) {
    Add-Type -AssemblyName System.Windows.Forms
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select your Japanese Shin Megami Tensei NINE ISO (unmodified)'
    $dialog.Filter = 'Xbox disc image (*.iso)|*.iso|All files (*.*)|*.*'
    if ($dialog.ShowDialog() -ne [System.Windows.Forms.DialogResult]::OK) { Fail 'No ISO selected.' }
    $Iso = $dialog.FileName
}
if (-not (Test-Path $Iso)) { Fail "File not found: $Iso" }
$Iso = (Resolve-Path $Iso).Path
Say "Japanese ISO: $Iso"
Say 'Your ISO is only read, never changed.' 'DarkGray'

# --- 3. English copy with the fan patcher ----------------------------------------
$English = Join-Path $Work 'SMT9_English.iso'
if ($needEnglish) {
    Step "Getting the English patcher $PatcherVersion (by negative-entrophy, MIT license)"
    $patcher = Join-Path $Work $PatcherName
    if (-not (Test-Path $patcher) -or (Get-FileHash $patcher -Algorithm SHA256).Hash -ne $PatcherSha256) {
        [Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
        Say "Downloading $PatcherUrl"
        Invoke-WebRequest -UseBasicParsing -Uri $PatcherUrl -OutFile $patcher
    }
    $hash = (Get-FileHash $patcher -Algorithm SHA256).Hash
    if ($hash -ne $PatcherSha256) { Remove-Item $patcher -Force; Fail "The downloaded patcher does not match the expected SHA-256 ($hash)." }
    Say 'Patcher verified.' 'Green'

    Step 'Making an English copy of your ISO'
    if (Test-Path $English) { Remove-Item $English -Force }
    $process = Start-Process -FilePath $patcher -ArgumentList @("`"$Iso`"", "`"$English`"", '--format', 'iso') -Wait -PassThru -NoNewWindow
    if ($process.ExitCode -ne 0 -or -not (Test-Path $English)) {
        Fail 'The English patcher rejected the ISO. It needs an unmodified Japanese Redump dump of SMT NINE (AT-002).'
    }
    Say 'English copy created.' 'Green'
}

# --- 4. Native build from the Japanese ISO ---------------------------------------
if (-not (Test-Path (Join-Path $Root '.venv\Scripts\python.exe'))) {
    Step 'Preparing the build environment (setup.cmd)'
    & (Join-Path $Root 'setup.cmd')
    if ($LASTEXITCODE -ne 0) { Fail 'setup.cmd failed. See the messages above.' }
}
if ($needBase) {
    Step 'Building the game from your Japanese ISO (this takes a long time - often 1-2 hours)'
    $buildArgs = @('--iso', $Iso)
    if (Test-Path (Join-Path $Root 'build\progress.json')) { $buildArgs += '--resume'; Say 'Continuing an earlier, unfinished build.' 'DarkGray' }
    & (Join-Path $Root 'build.cmd') @buildArgs
    if ($LASTEXITCODE -ne 0) { Fail 'The build failed. See the messages above (and docs\BUILDING.md).' }
    Say 'Game built.' 'Green'
}

# --- 5. Import the English copy ---------------------------------------------------
if ($needEnglish) {
    Step 'Installing English (recompiling the translated game code - often 30-90 minutes)'
    $languages = Join-Path $Root 'runtime\languages'
    New-Item -ItemType Directory -Force -Path $languages | Out-Null
    $lock = Join-Path $languages 'English.lock'
    if (Test-Path $lock) { Remove-Item $lock -Force }
    $log = Join-Path $languages 'English.import.log'
    $python = Join-Path $Root '.venv\Scripts\python.exe'
    $import = Start-Process -FilePath $python -ArgumentList @("`"$(Join-Path $Root 'import_language.py')`"", '--iso', "`"$English`"", '--id', 'English', '--name', 'English') -PassThru -NoNewWindow
    $shown = 0
    while (-not $import.HasExited) {
        Start-Sleep -Seconds 5
        if (Test-Path $log) {
            $lines = @(Get-Content $log -ErrorAction SilentlyContinue)
            if ($lines.Count -gt $shown) { $lines[$shown..($lines.Count - 1)] | ForEach-Object { Say "  $_" 'DarkGray' }; $shown = $lines.Count }
        }
    }
    $import.WaitForExit()
    if ($import.ExitCode -ne 0 -or -not (Language-Ready 'English')) {
        Fail "Installing English failed. Details: $log"
    }
    Say 'English installed.' 'Green'
    Remove-Item $English -Force -ErrorAction SilentlyContinue   # the copy is no longer needed
}

# --- 6. Select English ------------------------------------------------------------
Step 'Selecting English'
$ini = Join-Path $Root 'runtime\NINE.ini'
[Nine.Ini]::WritePrivateProfileString('Display', 'Language', 'English', $ini) | Out-Null
[Nine.Ini]::WritePrivateProfileString('Interface', 'Language', 'English', $ini) | Out-Null

# --- Done -------------------------------------------------------------------------
Step 'Done!'
Say 'SMT NINE Turn-Based Edition is installed. Start it with "SMT NINE Turn-Based.cmd" (Play).' 'Green'
$shortcut = Join-Path ([Environment]::GetFolderPath('Desktop')) 'SMT NINE Turn-Based.lnk'
if (-not (Test-Path $shortcut) -and (Ask 'Create a desktop shortcut to the launcher?')) {
    $shell = New-Object -ComObject WScript.Shell
    $link = $shell.CreateShortcut($shortcut)
    $link.TargetPath = Join-Path $Root 'SMT NINE Turn-Based.cmd'
    $link.WorkingDirectory = $Root
    if (Test-Path (Join-Path $Root 'smt9_recomp.exe')) { $link.IconLocation = (Join-Path $Root 'smt9_recomp.exe') + ',0' }
    $link.Save()
}
Stop-Transcript | Out-Null
Read-Host 'Press Enter to close'
