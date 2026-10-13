<#
  update-qlc.ps1 - fetches the newest rig build of QLC+ and the show file
  (runde 439, N9). Run it on the lys-pc with QLC+ closed:

      double-click update-qlc.cmd        (or: powershell -ExecutionPolicy Bypass -File update-qlc.ps1)

  What it does, in order - and nothing else:
    1. reads the release "rig-latest" on github.com/F4MES/qlcplus (no login:
       the repository is public)
    2. QLC+: downloads QLC-rig-setup.exe and starts the installer, if the
       build is newer than the one this script installed last time
    3. PSMAIN.qxw: finds the show file your QLC+ shortcut opens, keeps the old
       one beside it as PSMAIN.qxw.bak-<date-time>, and puts the new one in -
       only if it differs
    4. qlc_shared_functions.clj (BLT): saved to Downloads\qlc-update - BLT
       reads it from its own window, so you paste it there yourself
  It never stops QLC+ for you, never deletes anything, and asks before it
  installs. Options:
      -Silent        install without the installer's windows
      -Force         install and copy even when nothing is new
      -ShowPath X    the show file to replace (when no shortcut points at one)
      -SkipExe / -SkipShow
#>
param(
    [switch]$Silent,
    [switch]$Force,
    [string]$ShowPath = "",
    [switch]$SkipExe,
    [switch]$SkipShow,
    [string]$Repo = "F4MES/qlcplus",
    [string]$Tag = "rig-latest"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"      # Invoke-WebRequest is 10x slower with the bar
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$work  = Join-Path $env:USERPROFILE "Downloads\qlc-update"
$state = Join-Path $env:LOCALAPPDATA "qlc-rig-update\state.json"
New-Item -ItemType Directory -Force -Path $work | Out-Null
New-Item -ItemType Directory -Force -Path (Split-Path $state) | Out-Null

function Say($text, $colour = "Gray") { Write-Host $text -ForegroundColor $colour }

function Read-State {
    $s = [pscustomobject]@{ exeRev = ""; showSha = "" }
    if (Test-Path $state) {
        try {
            $j = Get-Content $state -Raw | ConvertFrom-Json
            if ($j.exeRev) { $s.exeRev = "$($j.exeRev)" }
            if ($j.showSha) { $s.showSha = "$($j.showSha)" }
        } catch { }
    }
    return $s
}

function Save-State($s) { $s | ConvertTo-Json | Set-Content -Path $state -Encoding UTF8 }

function Get-Asset($release, $name) {
    return $release.assets | Where-Object { $_.name -eq $name } | Select-Object -First 1
}

function Fetch($asset, $target) {
    Say ("  downloading {0} ({1:N1} MB) ..." -f $asset.name, ($asset.size / 1MB))
    Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $target
    $got = (Get-Item $target).Length
    if ($got -ne $asset.size) { throw ("{0}: got {1} bytes, expected {2}" -f $asset.name, $got, $asset.size) }
}

# the show file the QLC+ shortcut opens (-o / --open "<file>.qxw")
function Find-ShowFile {
    $places = @(
        [Environment]::GetFolderPath("Desktop"),
        [Environment]::GetFolderPath("CommonDesktopDirectory"),
        [Environment]::GetFolderPath("StartMenu"),
        [Environment]::GetFolderPath("CommonStartMenu"),
        [Environment]::GetFolderPath("Startup"),
        (Join-Path $env:APPDATA "Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar")
    ) | Where-Object { $_ -and (Test-Path $_) }
    $shell = New-Object -ComObject WScript.Shell
    foreach ($place in $places) {
        foreach ($lnk in Get-ChildItem -Path $place -Filter *.lnk -Recurse -ErrorAction SilentlyContinue) {
            $sc = $shell.CreateShortcut($lnk.FullName)
            if ($sc.TargetPath -notmatch "qlcplus") { continue }
            $m = [regex]::Match($sc.Arguments, '"([^"]+\.qxw)"|(\S+\.qxw)')
            if ($m.Success) {
                $p = if ($m.Groups[1].Success) { $m.Groups[1].Value } else { $m.Groups[2].Value }
                if (-not [IO.Path]::IsPathRooted($p) -and $sc.WorkingDirectory) { $p = Join-Path $sc.WorkingDirectory $p }
                Say ("  shortcut {0} opens {1}" -f $lnk.Name, $p)
                return $p
            }
        }
    }
    return ""
}

Say "QLC+ rig update ($Repo, $Tag)" "Cyan"

if (Get-Process -Name "qlcplus-qml", "qlcplus" -ErrorAction SilentlyContinue) {
    Say "QLC+ is running. Close it (save nothing) and run this again." "Yellow"
    exit 1
}

try {
    $release = Invoke-RestMethod -UseBasicParsing -Uri "https://api.github.com/repos/$Repo/releases/tags/$Tag" `
        -Headers @{ "User-Agent" = "qlc-rig-update"; "Accept" = "application/vnd.github+json" }
} catch {
    Say "Could not reach GitHub: $($_.Exception.Message)" "Red"
    exit 2
}
Say ("  release: {0}" -f $release.body)
$s = Read-State

# ---- 1. QLC+ itself ----
if (-not $SkipExe) {
    $exe = Get-Asset $release "QLC-rig-setup.exe"
    $revA = Get-Asset $release "rig-build.txt"
    $rev = ""
    if ($revA) {
        $rev = (Invoke-WebRequest -UseBasicParsing -Uri $revA.browser_download_url).Content
        if ($rev -is [byte[]]) { $rev = [Text.Encoding]::UTF8.GetString($rev) }
        $rev = "$rev".Trim()
    }
    if (-not $exe) {
        Say "No QLC-rig-setup.exe in the release yet (the build may still be running)." "Yellow"
    } elseif ($rev -and $rev -eq $s.exeRev -and -not $Force) {
        Say "QLC+ $rev is already installed - nothing to do." "Green"
    } else {
        $target = Join-Path $work ("QLC-rig-setup-{0}.exe" -f ($(if ($rev) { $rev } else { "new" })))
        Fetch $exe $target
        $answer = Read-Host "Install QLC+ $rev now? (y/n)"
        if ($answer -match '^[yYjJ]') {
            if ($Silent) { $p = Start-Process -FilePath $target -ArgumentList "/S" -Wait -PassThru }
            else         { $p = Start-Process -FilePath $target -Wait -PassThru }
            if ($p.ExitCode -eq 0) {
                $s.exeRev = $rev; Save-State $s
                Say "QLC+ $rev installed." "Green"
            } else {
                Say ("The installer ended with code {0} - nothing marked as installed." -f $p.ExitCode) "Yellow"
            }
        } else {
            Say "Not installed. The installer is in $target." "Yellow"
        }
    }
}

# ---- 2. the show file ----
if (-not $SkipShow) {
    $show = Get-Asset $release "PSMAIN.qxw"
    if (-not $show) {
        Say "No PSMAIN.qxw in the release." "Yellow"
    } else {
        if (-not $ShowPath) { $ShowPath = Find-ShowFile }
        if (-not $ShowPath) {
            $ShowPath = Read-Host "Where is your PSMAIN.qxw? (full path, empty = skip)"
        }
        if ($ShowPath) {
            $tmp = Join-Path $work "PSMAIN.qxw.new"
            Fetch $show $tmp
            $head = Get-Content $tmp -TotalCount 3 -Encoding UTF8 | Out-String
            if ($head -notmatch "<\?xml" -or (Select-String -Path $tmp -Pattern "<Workspace" -SimpleMatch -Quiet) -eq $false) {
                Say "The downloaded show file does not look like a QLC+ workspace - left alone." "Red"
            } else {
                $newSha = (Get-FileHash $tmp -Algorithm SHA256).Hash
                $oldSha = if (Test-Path $ShowPath) { (Get-FileHash $ShowPath -Algorithm SHA256).Hash } else { "" }
                if ($newSha -eq $oldSha -and -not $Force) {
                    Say "PSMAIN.qxw is already the newest." "Green"
                } else {
                    if (Test-Path $ShowPath) {
                        $bak = "{0}.bak-{1}" -f $ShowPath, (Get-Date -Format "yyyyMMdd-HHmm")
                        Copy-Item -Path $ShowPath -Destination $bak
                        Say "  the old one is kept as $bak"
                    }
                    Copy-Item -Path $tmp -Destination $ShowPath -Force
                    $s.showSha = $newSha; Save-State $s
                    Say "PSMAIN.qxw updated: $ShowPath" "Green"
                }
            }
        }
    }
}

# ---- 3. BLT's shared functions ----
$clj = Get-Asset $release "qlc_shared_functions.clj"
if ($clj) {
    $cljPath = Join-Path $work "qlc_shared_functions.clj"
    Fetch $clj $cljPath
    Say "BLT's shared functions are in $cljPath (paste them into BLT if they changed)."
}
Say "Done." "Cyan"
