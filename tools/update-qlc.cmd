<# : update-qlc.cmd - ONE file: this batch part runs PowerShell on the rest of this same file
@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command "& ([ScriptBlock]::Create([IO.File]::ReadAllText('%~f0'))) %*"
echo.
pause
exit /b
#>
<#
  update-qlc.cmd - the newest rig build of QLC+ and the show file, in one
  double-click (runde 439, N9; one file and hands-off from runde 441).

  1. reads the release "rig-latest" on github.com/F4MES/qlcplus (no login)
  2. downloads what is new: QLC-rig-setup.exe (only if the build is newer than
     the one installed last time) and PSMAIN.qxw (only if it differs)
  3. if there is something to install: closes QLC+ (5 s countdown - Ctrl+C
     stops it), installs silently, puts the new PSMAIN.qxw where your QLC+
     shortcut opens it (the old one kept beside it as PSMAIN.qxw.bak-<time>)
     and starts QLC+ again if it was running
  4. BLT's qlc_shared_functions.clj goes to Downloads\qlc-update (BLT loads it at
     start through its loader line - restart BLT if it changed); the one it
     replaces is kept as qlc_shared_functions.clj.bak-<time>
  5. rollback-qlc.cmd goes to Downloads\qlc-update too: it puts back the build,
     the show file and BLT's functions from before (runde 467)
  Nothing is ever deleted. The installers stay in Downloads\qlc-update
  (QLC-rig-setup-<build>.exe) - rollback-qlc.cmd needs them.
  A build that rollback-qlc.cmd took away is not installed again (-Force does). Options (after the file name in a cmd window):
      -Ask        ask before closing QLC+ and installing
      -Interactive  the installer with its windows (not silent)
      -NoStart    do not start QLC+ again afterwards
      -Force      install and copy even when nothing is new
      -ShowPath X the show file to replace (when no shortcut points at one)
#>
param(
    [switch]$Ask,
    [switch]$Interactive,
    [switch]$NoStart,
    [switch]$Force,
    [string]$ShowPath = "",
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

# R475_FLAGS (runde 475): beside state.json - "busy" while this runs (QLC+'s UPDATE starts no
# second one), "closing" just before QLC+ is closed (it skips its "save?" box), and "show-on",
# which QLC+ writes with SHOW ON: then QLC+ is left running
$busyFlag    = Join-Path (Split-Path $state) "busy"
$closingFlag = Join-Path (Split-Path $state) "closing"
$showOnFlag  = Join-Path (Split-Path $state) "show-on"

function Read-State {
    # runde 467: + history (every build installed, newest last) and rolledBack (the
    # builds rollback-qlc.cmd took away - not installed again by themselves)
    $s = [pscustomobject]@{ exeRev = ""; showSha = ""; history = @(); rolledBack = @() }
    if (Test-Path $state) {
        try {
            $j = Get-Content $state -Raw | ConvertFrom-Json
            if ($j.exeRev) { $s.exeRev = "$($j.exeRev)" }
            if ($j.showSha) { $s.showSha = "$($j.showSha)" }
            if ($j.history) { $s.history = @($j.history | ForEach-Object { "$_" }) }
            if ($j.rolledBack) { $s.rolledBack = @($j.rolledBack | ForEach-Object { "$_" }) }
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
    # R511_UPDATER_PART (runde 511): into <name>.part first, in its place only when it is whole. A cut-off
    # download wrote straight over the target: the kept rollback-qlc.cmd half written, or half an
    # installer under its build's name that rollback-qlc.cmd then took for "the build before"
    $part = "$target.part"
    Invoke-WebRequest -UseBasicParsing -Uri $asset.browser_download_url -OutFile $part
    $got = (Get-Item -LiteralPath $part).Length
    if ($got -ne $asset.size) { throw ("{0}: got {1} bytes, expected {2}" -f $asset.name, $got, $asset.size) }
    Move-Item -LiteralPath $part -Destination $target -Force
}

function Get-QlcProcesses { return @(Get-Process -Name "qlcplus-qml", "qlcplus" -ErrorAction SilentlyContinue) }

# R514_UPDATER_EXE_CHECK (Tobias 10-29: after the e98b1a5 install his shortcuts said QLC+ was gone):
# the installer's code 0 is not proof - qlcplus-qml.exe has to be in the install folder afterwards.
# Only reads (registry, shortcut, Defender's history); changes nothing
function Get-QlcExe {
    $dir = (Get-ItemProperty -Path "HKCU:\Software\qlcplus" -Name Install_Dir -ErrorAction SilentlyContinue).Install_Dir
    if (-not $dir) { $dir = "C:\QLC+5" }
    return ("{0}\qlcplus-qml.exe" -f "$dir".TrimEnd("\"))     # not Join-Path: it throws for a drive that is not there
}

function Get-ShortcutTarget($sc) {
    if (-not $sc) { return "" }
    try { return "$((New-Object -ComObject WScript.Shell).CreateShortcut($sc.Link).TargetPath)" } catch { return "" }
}

function Show-Defender {
    try {
        $hits = @(Get-MpThreatDetection -ErrorAction Stop | Where-Object { "$($_.Resources)" -match "qlc" } |
                  Sort-Object InitialDetectionTime -Descending | Select-Object -First 3)
        if ($hits.Count -eq 0) { Say "  Windows Defender has no QLC+ detection on record." ; return }
        foreach ($h in $hits) {
            $name = (Get-MpThreat -ErrorAction SilentlyContinue | Where-Object { $_.ThreatID -eq $h.ThreatID } | Select-Object -First 1).ThreatName
            Say ("  Windows Defender {0}: {1} -> {2}" -f $h.InitialDetectionTime, $name, ("$($h.Resources)")) "Red"
        }
        Say "  Windows Security > Virus & threat protection > Protection history: Restore / Allow it, then update-qlc.cmd -Force." "Yellow"
    } catch { Say "  (Windows Defender's history not readable here: $($_.Exception.Message))" "Yellow" }
}

# the build installed before $rev, put back silently; "" when there is none or it did not come back
function Restore-Previous($s, $rev) {
    $hist = @($s.history | Where-Object { $_ -and $_ -ne $rev })
    for ($i = $hist.Count - 1; $i -ge 0; $i--) {
        $f = Join-Path $work ("QLC-rig-setup-{0}.exe" -f $hist[$i])
        if (-not (Test-Path -LiteralPath $f)) { continue }
        Say ("  putting QLC+ {0} back on (silent) ..." -f $hist[$i]) "Yellow"
        $q = Start-Process -FilePath $f -ArgumentList "/S" -Wait -PassThru
        if ($q.ExitCode -eq 0 -and (Test-Path -LiteralPath (Get-QlcExe))) { return $hist[$i] }
        Say ("  QLC+ {0} did not come back either (code {1})." -f $hist[$i], $q.ExitCode) "Red"
        return ""
    }
    return ""
}

# the QLC+ shortcut that opens the show (-o / --open "<file>.qxw"): where the
# show file lives, and how QLC+ is started again afterwards
function Find-Shortcut {
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
                return [pscustomobject]@{ Link = $lnk.FullName; Show = $p }
            }
        }
    }
    return $null
}

function Close-Qlc {
    $procs = Get-QlcProcesses
    if ($procs.Count -eq 0) { return $false }
    # R475_SHOW_ON: never with the show on - before the countdown and after it
    if (Test-Path $showOnFlag) {
        Say "SHOW is ON in QLC+ - QLC+ left running. SHOW OFF, then UPDATE again." "Red"
        exit 3
    }
    if ($Ask) {
        if ((Read-Host "QLC+ is running - close it and update? (y/n)") -notmatch '^[yYjJ]') { throw "stopped - QLC+ left running" }
    } else {
        for ($i = 5; $i -ge 1; $i--) {
            Write-Host ("`r  QLC+ closes in {0} s - Ctrl+C stops it " -f $i) -NoNewline -ForegroundColor Yellow
            Start-Sleep -Seconds 1
        }
        Write-Host ""
    }
    if (Test-Path $showOnFlag) {
        Say "SHOW is ON in QLC+ - QLC+ left running. SHOW OFF, then UPDATE again." "Red"
        exit 3
    }
    # R475_CLOSING: QLC+ skips its "save?" box (the show file is replaced below)
    Set-Content -Path $closingFlag -Value $PID
    Start-Sleep -Seconds 2
    # politely first (its own window close), then for good after 15 s
    foreach ($p in $procs) { try { [void]$p.CloseMainWindow() } catch { } }
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-QlcProcesses).Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if ((Get-QlcProcesses).Count -gt 0) {
        Say "  QLC+ did not close by itself - stopping it" "Yellow"
        Get-QlcProcesses | Stop-Process -Force
        Start-Sleep -Seconds 2
    }
    Say "  QLC+ closed."
    return $true
}

Set-Content -Path $busyFlag -Value $PID      # R475_BUSY: gone in the finally at the end
try {
Say "QLC+ rig update ($Repo, $Tag)" "Cyan"
try {
    $release = Invoke-RestMethod -UseBasicParsing -Uri "https://api.github.com/repos/$Repo/releases/tags/$Tag" `
        -Headers @{ "User-Agent" = "qlc-rig-update"; "Accept" = "application/vnd.github+json" }
} catch {
    Say "Could not reach GitHub: $($_.Exception.Message)" "Red"
    exit 2
}
Say ("  release: {0}" -f $release.body)
$s = Read-State
$wasRunning = (Get-QlcProcesses).Count -gt 0
$sc = Find-Shortcut
if ($sc) { Say ("  shortcut {0} opens {1}" -f (Split-Path $sc.Link -Leaf), $sc.Show) }

# ---- 1. what is new (downloaded while QLC+ still runs) ----
$exeFile = $null; $rev = ""
$exe = Get-Asset $release "QLC-rig-setup.exe"
$revA = Get-Asset $release "rig-build.txt"
if ($revA) {
    $rev = (Invoke-WebRequest -UseBasicParsing -Uri $revA.browser_download_url).Content
    if ($rev -is [byte[]]) { $rev = [Text.Encoding]::UTF8.GetString($rev) }
    $rev = "$rev".Trim()
}
# R516B_UPDATER_GONE (Tobias 10-30: the shortcut said QLC+ was gone again, hours after an install that
# checked out - UPDATE then said "already installed"): every run looks for qlcplus-qml.exe, not only the
# run that installs. Gone = the build is installed again (and Defender's history shown), whatever the state
$qlcGone = -not (Test-Path -LiteralPath (Get-QlcExe))
if (-not $exe) {
    Say "No QLC-rig-setup.exe in the release yet (the build may still be running)." "Yellow"
} elseif ($rev -and $rev -eq $s.exeRev -and -not $Force -and -not $qlcGone) {
    Say "QLC+ $rev is already installed." "Green"
} elseif ($rev -and $s.rolledBack -contains $rev -and -not $Force -and -not $qlcGone) {
    # runde 467: rollback-qlc.cmd took this build away - not again by itself
    Say "QLC+ $rev was rolled back with rollback-qlc.cmd - not installed again (a newer build will be; -Force installs this one)." "Yellow"
} else {
    if ($qlcGone) {                                  # R516B_UPDATER_GONE: say why it installs again
        Say ("QLC+ is NOT there any more: {0} is gone - installing QLC+ {1} again." -f (Get-QlcExe), $rev) "Red"
        Show-Defender
    }
    $exeFile = Join-Path $work ("QLC-rig-setup-{0}.exe" -f ($(if ($rev) { $rev } else { "new" })))
    Fetch $exe $exeFile
}

$showFile = $null; $newSha = ""
$show = Get-Asset $release "PSMAIN.qxw"
if ($show) {
    if (-not $ShowPath -and $sc) { $ShowPath = $sc.Show }
    if (-not $ShowPath) { $ShowPath = Read-Host "Where is your PSMAIN.qxw? (full path, empty = skip)" }
    if ($ShowPath) {
        $tmp = Join-Path $work "PSMAIN.qxw.new"
        Fetch $show $tmp
        $head = Get-Content $tmp -TotalCount 3 -Encoding UTF8 | Out-String
        if ($head -notmatch "<\?xml" -or (Select-String -Path $tmp -Pattern "<Workspace" -SimpleMatch -Quiet) -eq $false) {
            Say "The downloaded show file does not look like a QLC+ workspace - left alone." "Red"
        } else {
            $newSha = (Get-FileHash $tmp -Algorithm SHA256).Hash
            $oldSha = if (Test-Path $ShowPath) { (Get-FileHash $ShowPath -Algorithm SHA256).Hash } else { "" }
            if ($newSha -eq $oldSha -and -not $Force) { Say "PSMAIN.qxw is already the newest." "Green" }
            else { $showFile = $tmp }
        }
    }
}

# ---- 2. close, install, copy - only when there is something to do ----
if ($exeFile -or $showFile) {
    $closed = Close-Qlc
    if ($exeFile) {
        Say "  installing QLC+ $rev $(if ($Interactive) { '' } else { '(silent)' }) ..."
        if ($Interactive) { $p = Start-Process -FilePath $exeFile -Wait -PassThru }
        else              { $p = Start-Process -FilePath $exeFile -ArgumentList "/S" -Wait -PassThru }
        $qlcExe = Get-QlcExe                                   # R514_UPDATER_EXE_CHECK
        if ($p.ExitCode -eq 0 -and -not (Test-Path -LiteralPath $qlcExe)) {
            # code 0, but no program: not marked installed, the build before goes back on by itself
            Say "The installer said OK, but $qlcExe is NOT there - QLC+ $rev is not marked installed." "Red"
            Show-Defender
            $back = Restore-Previous $s $rev
            if ($back) {
                $s.exeRev = $back
                $s.rolledBack = @($s.rolledBack | Where-Object { $_ -ne $rev }) + @($rev)
                Save-State $s
                Say "QLC+ $back is back on. $rev is not installed again by itself (update-qlc.cmd -Force tries it again)." "Green"
            } else {
                Say "No build before to put back - run the installer by hand: $exeFile (or rollback-qlc.cmd)." "Red"
            }
        } elseif ($p.ExitCode -eq 0) {
            $s.exeRev = $rev
            if ($rev) { $s.history = @($s.history | Where-Object { $_ -ne $rev }) + @($rev) }   # runde 467
            $s.rolledBack = @($s.rolledBack | Where-Object { $_ -ne $rev })
            Save-State $s
            Say "QLC+ $rev installed." "Green"
            $tgt = Get-ShortcutTarget $sc
            if ($tgt -and -not (Test-Path -LiteralPath $tgt)) {
                Say ("  but your shortcut {0} points at {1}, which does not exist - QLC+ is {2}" -f (Split-Path $sc.Link -Leaf), $tgt, $qlcExe) "Red"
            }
        } else {
            Say ("The installer ended with code {0} - nothing marked as installed. It is in {1}." -f $p.ExitCode, $exeFile) "Yellow"
        }
    }
    if ($showFile) {
        if (Test-Path $ShowPath) {
            # R511_UPDATER_BAK_SHOW (runde 511): to the second - an update and an update -Force in the same
            # minute wrote the second backup over the first, and the show from before was gone
            $bak = "{0}.bak-{1}" -f $ShowPath, (Get-Date -Format "yyyyMMdd-HHmmss")
            Copy-Item -Path $ShowPath -Destination $bak
            Say "  the old show file is kept as $bak"
        }
        Copy-Item -Path $showFile -Destination $ShowPath -Force
        $s.showSha = $newSha; Save-State $s
        Say "PSMAIN.qxw updated: $ShowPath" "Green"
    }
    # ---- 3. QLC+ back on, if it was on ----
    if (($wasRunning -or $closed) -and -not $NoStart) {
        if ($sc) { Start-Process -FilePath $sc.Link }
        else {
            $dir = (Get-ItemProperty -Path "HKCU:\Software\qlcplus" -Name Install_Dir -ErrorAction SilentlyContinue).Install_Dir
            if ($dir -and (Test-Path (Join-Path $dir "qlcplus-qml.exe"))) { Start-Process -FilePath (Join-Path $dir "qlcplus-qml.exe") }
        }
        Say "  QLC+ started again."
    }
} else {
    Say "Nothing new - QLC+ was not touched." "Green"
}

# ---- 4. BLT's shared functions ----
$clj = Get-Asset $release "qlc_shared_functions.clj"
if ($clj) {
    $cljPath = Join-Path $work "qlc_shared_functions.clj"
    $cljNew = Join-Path $work "qlc_shared_functions.clj.new"
    Fetch $clj $cljNew
    # runde 467: the one it replaces is kept (rollback-qlc.cmd puts it back)
    if ((Test-Path $cljPath) -and (Get-FileHash $cljPath).Hash -ne (Get-FileHash $cljNew).Hash) {
        $cljBak = "{0}.bak-{1}" -f $cljPath, (Get-Date -Format "yyyyMMdd-HHmmss")   # R511_UPDATER_BAK_CLJ: to the second
        Copy-Item -Path $cljPath -Destination $cljBak
        Say "  the old BLT functions are kept as $cljBak"
    }
    Copy-Item -Path $cljNew -Destination $cljPath -Force
    Remove-Item -Path $cljNew -ErrorAction SilentlyContinue
    Say "BLT's shared functions are in $cljPath (restart BLT to load them if they changed)."
}

# ---- 5. rollback-qlc.cmd beside the installers (runde 467) ----
$rb = Get-Asset $release "rollback-qlc.cmd"
if ($rb) {
    $rbPath = Join-Path $work "rollback-qlc.cmd"
    try { Fetch $rb $rbPath; Say "If this build misbehaves: double-click $rbPath" }
    catch { Say "  rollback-qlc.cmd could not be fetched: $($_.Exception.Message)" "Yellow" }
}
# ---- BLT startup guard: installed in its native launcher, never its shared functions ----
$guardJar = Get-Asset $release "qlc-blt-guard.jar"
$guardInstaller = Get-Asset $release "install-blt-guard.ps1"
if ($guardJar -and $guardInstaller) {
    $guardJarPath = Join-Path $work "qlc-blt-guard.jar"
    $guardScriptPath = Join-Path $work "install-blt-guard.ps1"
    try {
        Fetch $guardJar $guardJarPath
        Fetch $guardInstaller $guardScriptPath
        try { & $guardScriptPath -JarPath $guardJarPath }
        catch [System.UnauthorizedAccessException] {
            # Program Files installations need Windows' normal UAC approval.
            $quotedScript = $guardScriptPath.Replace("'", "''")
            $quotedJar = $guardJarPath.Replace("'", "''")
            $command = "try { & '$quotedScript' -JarPath '$quotedJar'; exit 0 } catch { Write-Error `$_; exit 1 }"
            $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($command))
            $installer = Start-Process -FilePath "powershell.exe" -Verb RunAs -Wait -PassThru -ArgumentList @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-EncodedCommand', $encoded)
            if ($installer.ExitCode -ne 0) { throw "BLT guard installation failed ($($installer.ExitCode))." }
        }
        Say "BLT guard ready. Fully close and restart BLT once to activate it." "Green"
    } catch {
        Say "BLT guard NOT installed: $($_.Exception.Message)" "Yellow"
        Say "QLC+ was updated. Close BLT and run UPDATE again, or use install-blt-guard.ps1 -AppPath for a portable copy." "Yellow"
    }
}
Say "Done." "Cyan"
} finally {
    # R475_END_BUSY: also after an exit, an error or Ctrl+C
    Remove-Item -Path $busyFlag, $closingFlag -ErrorAction SilentlyContinue
}
