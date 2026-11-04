<# : get-track-app.cmd - ONE file: this batch part runs PowerShell on the rest of this same file
@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command "& ([ScriptBlock]::Create([IO.File]::ReadAllText('%~f0'))) %*"
echo.
pause
exit /b
#>
<#
  get-track-app.cmd - installs the TRACK app (branch track-app) - the FIRST time. After that
  TRACK updates itself: the UPDATE TRACK chip on its page (runde 548, Tobias 11-04: "bygge
  opdateringen bedre, saa den bare opdatere inde i qlc ... og genstarter sig selv, fremfor
  alt det cmd stuff (fint til foerste installering)").

  Works on a clean PC (Tobias 11-04: "den skal kunne deployes paa en ren pc og saa bare
  virke fra start"): TRACK brings its own Java and beat-link - nothing else to install.

  1. reads track-app.json in the release "track-app-latest" on github.com/F4MES/qlcplus
  2. downloads and checks (size + SHA-256) TRACK and its runtime (Java + beat-link, once)
  3. puts everything in %LOCALAPPDATA%\TRACK:
        TRACK.exe (the launcher), app\<build>\, runtime\<id>\, logs\, current.txt
     Nothing of the rig's QLC+ is touched: not its folder, registry or shortcuts.
  4. the show: the PSMAIN.qxw your QLC+ shortcut opens; on a PC without one, Documents\TRACK\
     PSMAIN.qxw (downloaded from the rig release once, never written over)
  5. the desktop shortcut "TRACK" (it replaces this script's earlier "TRACK (test)")
  Options (after the file name in a cmd window):
      -ShowPath X  the show file to open
      -Force       download and unpack again even when the build is there
#>
param(
    [string]$ShowPath = "",
    [switch]$Force,
    [string]$Repo = "F4MES/qlcplus",
    [string]$Tag = "track-app-latest"
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$root = Join-Path $env:LOCALAPPDATA "TRACK"
foreach ($d in @($root, (Join-Path $root "app"), (Join-Path $root "runtime"), (Join-Path $root "downloads"), (Join-Path $root "logs"))) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}
$base = "https://github.com/$Repo/releases/download/$Tag/"

function Say($text, $colour = "Gray") { Write-Host $text -ForegroundColor $colour }

function Log($text) {
    $line = "{0}  TRACK  get-track-app: {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $text
    Add-Content -LiteralPath (Join-Path $root "logs\track.log") -Value $line -Encoding UTF8
}

# one file from the release: downloaded as .part, size and SHA-256 checked, then unpacked
# into <target>.unpacking and moved in place only when <marker> is in it
function Fetch-Unpack($item, $target, $marker) {
    $zip = Join-Path $root ("downloads\" + $item.asset)
    $part = "$zip.part"
    Say ("  downloading {0} ({1:N1} MB) ..." -f $item.asset, ($item.size / 1MB))
    if (Test-Path -LiteralPath $part) { Remove-Item -LiteralPath $part -Force }
    Invoke-WebRequest -UseBasicParsing -Uri ($base + $item.asset) -OutFile $part
    if ((Get-Item -LiteralPath $part).Length -ne $item.size) { throw ("{0}: the download is not whole" -f $item.asset) }
    $sha = (Get-FileHash -LiteralPath $part -Algorithm SHA256).Hash.ToLowerInvariant()
    if ($sha -ne $item.sha256) { Remove-Item -LiteralPath $part -Force; throw ("{0}: SHA-256 does not match the release" -f $item.asset) }
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }
    Move-Item -LiteralPath $part -Destination $zip
    Unblock-File -LiteralPath $zip
    $tmp = "$target.unpacking"
    if (Test-Path -LiteralPath $tmp) { Remove-Item -LiteralPath $tmp -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $tmp | Out-Null
    Say "  unpacking ..."
    $tar = Join-Path $env:SystemRoot "System32\tar.exe"
    if (Test-Path -LiteralPath $tar) {
        & $tar -xf $zip -C $tmp
        if ($LASTEXITCODE -ne 0) { throw ("{0}: tar.exe could not unpack it" -f $item.asset) }
    } else {
        Expand-Archive -LiteralPath $zip -DestinationPath $tmp
    }
    if (-not (Test-Path -LiteralPath (Join-Path $tmp $marker))) { throw ("{0}: {1} missing after unpacking - Windows Defender? (Protection history)" -f $item.asset, $marker) }
    if (Test-Path -LiteralPath $target) { Remove-Item -LiteralPath $target -Recurse -Force }
    Move-Item -LiteralPath $tmp -Destination $target
    Remove-Item -LiteralPath $zip -Force
}

# the QLC+ shortcut that opens the show (as update-qlc.cmd's Find-Shortcut) - only read
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
                return [pscustomobject]@{ Link = $lnk.FullName; Show = $p; Arguments = $sc.Arguments }
            }
        }
    }
    return $null
}

Say "TRACK app ($Repo, $Tag) - in $root, beside the rig's QLC+ (which is not touched)" "Cyan"
if (@(Get-Process -Name "TRACK-engine" -ErrorAction SilentlyContinue).Count -gt 0) {
    Say "TRACK is running - close it first (TRACK updates itself from now on: UPDATE TRACK on its page)." "Yellow"
    exit 3
}
try {
    $m = Invoke-RestMethod -UseBasicParsing -Uri ($base + "track-app.json") -Headers @{ "User-Agent" = "track-app-install" }
} catch {
    Say "Could not read the release (or it is not built yet): $($_.Exception.Message)" "Red"
    exit 2
}
foreach ($v in @($m.rev, $m.runtime.id, $m.app.asset, $m.runtime.asset)) {
    if ("$v" -notmatch '^[A-Za-z0-9._-]{1,64}$') { Say "The release's track-app.json looks odd ('$v') - stopping." "Red"; exit 2 }
}
$rev = "$($m.rev)"; $rid = "$($m.runtime.id)"

try {
    $rt = Join-Path $root "runtime\$rid"
    if ((Test-Path -LiteralPath (Join-Path $rt "beat-link-trigger.jar")) -and -not $Force) {
        Say "Runtime $rid (Java + beat-link) is already there." "Green"
    } else {
        Say "Runtime $rid (Java + beat-link) - once:"
        Fetch-Unpack $m.runtime $rt "beat-link-trigger.jar"
    }
    $app = Join-Path $root "app\$rev"
    if ((Test-Path -LiteralPath (Join-Path $app "TRACK-engine.exe")) -and -not $Force) {
        Say "TRACK $rev is already there." "Green"
    } else {
        Say "TRACK ${rev}:"
        Fetch-Unpack $m.app $app "TRACK-engine.exe"
    }
} catch {
    Say $_.Exception.Message "Red"
    Log ("install failed: " + $_.Exception.Message)
    exit 2
}

# the launcher and which build it starts
Copy-Item -LiteralPath (Join-Path $app "TRACK-launcher.exe") -Destination (Join-Path $root "TRACK.exe") -Force
$curFile = Join-Path $root "current.txt"
$old = if (Test-Path -LiteralPath $curFile) { (Get-Content -LiteralPath $curFile -TotalCount 1).Trim() } else { "" }
if ($old -and $old -ne $rev) { Set-Content -LiteralPath (Join-Path $root "previous.txt") -Value $old -Encoding ASCII }
Set-Content -LiteralPath $curFile -Value $rev -Encoding ASCII
Log "installed $rev (runtime $rid)"

# the show
$sc = Find-Shortcut
if (-not $ShowPath -and $sc) { $ShowPath = $sc.Show; Say ("  your QLC+ shortcut {0} opens {1}" -f (Split-Path $sc.Link -Leaf), $ShowPath) }
if (-not $ShowPath) {
    $docs = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "TRACK"
    New-Item -ItemType Directory -Force -Path $docs | Out-Null
    $ShowPath = Join-Path $docs "PSMAIN.qxw"
    if (Test-Path -LiteralPath $ShowPath) {
        Say "  show: $ShowPath (yours, kept)"
    } else {
        Say "  no QLC+ on this PC - fetching the rig's show (PSMAIN.qxw) to $ShowPath"
        try {
            Invoke-WebRequest -UseBasicParsing -Uri "https://github.com/$Repo/releases/download/rig-latest/PSMAIN.qxw" -OutFile "$ShowPath.part"
            Move-Item -LiteralPath "$ShowPath.part" -Destination $ShowPath
        } catch {
            Say "  could not fetch PSMAIN.qxw: $($_.Exception.Message) - run again with -ShowPath" "Red"
            exit 2
        }
    }
}
if (-not (Test-Path -LiteralPath $ShowPath)) { Say "No show file at '$ShowPath' - run again with -ShowPath" "Red"; exit 2 }
$args2 = '-o "{0}"' -f $ShowPath
if (-not $sc -or $sc.Arguments -match '(^|\s)(-f|--fullscreen)(\s|$)') { $args2 += ' -f' }

# the shortcut (and away with this script's own earlier one, which pointed at TRACK-test)
$desktop = [Environment]::GetFolderPath("Desktop")
$shell = New-Object -ComObject WScript.Shell
$oldLink = Join-Path $desktop "TRACK (test).lnk"
if (Test-Path -LiteralPath $oldLink) {
    if ($shell.CreateShortcut($oldLink).TargetPath -match '\\TRACK-test\\') { Remove-Item -LiteralPath $oldLink -Force; Say "  the old 'TRACK (test)' shortcut is replaced" }
}
$s = $shell.CreateShortcut((Join-Path $desktop "TRACK.lnk"))
$s.TargetPath = Join-Path $root "TRACK.exe"
$s.Arguments = $args2
$s.WorkingDirectory = $root
$s.IconLocation = (Join-Path $app "TRACK-engine.exe") + ",0"
$s.Description = "TRACK - QLC+'s engine with its own beat-link (updates itself)"
$s.Save()
Say "Desktop shortcut: TRACK -> TRACK.exe $args2" "Green"

Say ""
Say "Next:" "Cyan"
Say "  1. Close QLC+ and Beat Link Trigger (one light program and one beat-link program at a time)."
Say "  2. Double-click 'TRACK'. The first time, Windows may ask whether Java (OpenJDK Platform binary)"
Say "     may use the network: allow it on private networks - that is how TRACK hears the CDJs."
Say "  3. TRACK finds the CDJs by itself ('2 players found'). New builds: UPDATE TRACK on the page."
Say "  Logs: $root\logs (track.log, bridge.log). Back to normal: close TRACK, start QLC+ and BLT."
