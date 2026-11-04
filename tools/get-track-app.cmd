<# : get-track-app.cmd - ONE file: this batch part runs PowerShell on the rest of this same file
@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command "& ([ScriptBlock]::Create([IO.File]::ReadAllText('%~f0'))) %*"
echo.
pause
exit /b
#>
<#
  get-track-app.cmd - the TRACK app test build (branch track-app, Tobias 11-04: "Lav en ny
  gren ... saa kan jeg teste i weekenden om det virker"), BESIDE the rig's QLC+.

  1. reads the release "track-app-latest" on github.com/F4MES/qlcplus (no login)
  2. downloads TRACK-app.zip to Downloads\qlc-update (only when the build is new)
  3. unzips it to %LOCALAPPDATA%\TRACK-test\<build>. Nothing of the rig's QLC+ is touched:
     not its install folder, not the registry, not its shortcuts, not rig-latest/update-qlc.cmd
  4. makes the desktop shortcut "TRACK (test)": TRACK.exe --track-app, opening the same
     PSMAIN.qxw your QLC+ shortcut opens (found the way update-qlc.cmd finds it)
  5. says whether Beat Link Trigger is installed - TRACK's beat-link helper runs on its Java
     and its beat-link (BLT itself is not started)
  Nothing is ever deleted. Back to normal: close TRACK, start QLC+ and BLT as always.
  Options (after the file name in a cmd window):
      -ShowPath X  the show file (when no QLC+ shortcut points at one)
      -Force       download and unzip again even when the build is there
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

$work = Join-Path $env:USERPROFILE "Downloads\qlc-update"
$home2 = Join-Path $env:LOCALAPPDATA "TRACK-test"
New-Item -ItemType Directory -Force -Path $work | Out-Null
New-Item -ItemType Directory -Force -Path $home2 | Out-Null

function Say($text, $colour = "Gray") { Write-Host $text -ForegroundColor $colour }

function Get-Asset($release, $name) {
    return $release.assets | Where-Object { $_.name -eq $name } | Select-Object -First 1
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

# Beat Link Trigger's install (as tools/install-blt-guard.ps1 and TRACK's trackbridge.cpp look) - only read
function Find-Blt {
    $roots = New-Object 'System.Collections.Generic.List[string]'
    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA)) {
        if ($base) {
            foreach ($relative in @('Beat Link Trigger', 'Programs\Beat Link Trigger', 'Deep Symmetry\Beat Link Trigger')) {
                $roots.Add((Join-Path $base $relative))
            }
        }
    }
    foreach ($key in @('HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*', 'HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*')) {
        foreach ($entry in @(Get-ItemProperty $key -ErrorAction SilentlyContinue)) {
            if ($entry.DisplayName -like '*Beat Link Trigger*' -and $entry.InstallLocation) { $roots.Add($entry.InstallLocation) }
        }
    }
    foreach ($root in @($roots | Sort-Object -Unique)) {
        $app = Join-Path $root 'app'
        $java = @('runtime\bin\javaw.exe', 'runtime\bin\java.exe') | ForEach-Object { Join-Path $root $_ } | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if ((Test-Path -LiteralPath $app) -and $java -and @(Get-ChildItem -LiteralPath $app -Filter '*.jar' -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike 'qlc-blt-guard*' }).Count -gt 0) {
            return [pscustomobject]@{ Root = $root; Java = $java }
        }
    }
    return $null
}

Say "TRACK app test build ($Repo, $Tag) - beside the rig's QLC+, which is not touched" "Cyan"
try {
    $release = Invoke-RestMethod -UseBasicParsing -Uri "https://api.github.com/repos/$Repo/releases/tags/$Tag" `
        -Headers @{ "User-Agent" = "track-app-test"; "Accept" = "application/vnd.github+json" }
} catch {
    Say "Could not reach GitHub (or the release is not built yet): $($_.Exception.Message)" "Red"
    exit 2
}
Say ("  release: {0}" -f $release.body)

$zip = Get-Asset $release "TRACK-app.zip"
$revA = Get-Asset $release "track-app-build.txt"
if (-not $zip -or -not $revA) { Say "The release has no TRACK-app.zip yet - the build may still be running." "Yellow"; exit 2 }
$rev = (Invoke-WebRequest -UseBasicParsing -Uri $revA.browser_download_url).Content
if ($rev -is [byte[]]) { $rev = [Text.Encoding]::UTF8.GetString($rev) }
$rev = "$rev".Trim()
if ($rev -notmatch '^[0-9a-f]{6,40}$') { Say "Odd build id '$rev' - stopping." "Red"; exit 2 }

$dest = Join-Path $home2 $rev
if ((Test-Path -LiteralPath (Join-Path $dest "TRACK.exe")) -and -not $Force) {
    Say "TRACK $rev is already unpacked: $dest" "Green"
} else {
    $file = Join-Path $work ("TRACK-app-{0}.zip" -f $rev)
    if (-not (Test-Path -LiteralPath $file) -or (Get-Item -LiteralPath $file).Length -ne $zip.size) {
        Say ("  downloading TRACK-app.zip ({0:N1} MB) ..." -f ($zip.size / 1MB))
        $part = "$file.part"
        Invoke-WebRequest -UseBasicParsing -Uri $zip.browser_download_url -OutFile $part
        if ((Get-Item -LiteralPath $part).Length -ne $zip.size) { throw "TRACK-app.zip: the download is not whole" }
        Move-Item -LiteralPath $part -Destination $file -Force
    }
    Unblock-File -LiteralPath $file
    $tmp = "$dest.unpacking-" + [guid]::NewGuid().ToString('N').Substring(0, 8)
    Say "  unpacking to $dest ..."
    Expand-Archive -LiteralPath $file -DestinationPath $tmp
    if (Test-Path -LiteralPath $dest) { $dest = "$dest-" + (Get-Date -Format 'yyyyMMdd-HHmmss') }   # never over a folder
    Move-Item -LiteralPath $tmp -Destination $dest
    if (-not (Test-Path -LiteralPath (Join-Path $dest "TRACK.exe"))) { Say "TRACK.exe is missing after unpacking - Windows Defender? (Protection history)" "Red"; exit 2 }
    Say "TRACK $rev unpacked: $dest" "Green"
}

# the show file and the rig shortcut's own options (fullscreen) - nothing of it changed
$sc = Find-Shortcut
if (-not $ShowPath -and $sc) { $ShowPath = $sc.Show; Say ("  your QLC+ shortcut {0} opens {1}" -f (Split-Path $sc.Link -Leaf), $ShowPath) }
if (-not $ShowPath) { $ShowPath = Read-Host "Where is your PSMAIN.qxw? (full path)" }
if (-not (Test-Path -LiteralPath $ShowPath)) { Say "No show file at '$ShowPath' - run again with -ShowPath" "Red"; exit 2 }
$args2 = '--track-app -o "{0}"' -f $ShowPath
if ($sc -and $sc.Arguments -match '(^|\s)(-f|--fullscreen)(\s|$)') { $args2 += ' -f' }

$link = Join-Path ([Environment]::GetFolderPath("Desktop")) "TRACK (test).lnk"
$shell = New-Object -ComObject WScript.Shell
$s = $shell.CreateShortcut($link)
$s.TargetPath = Join-Path $dest "TRACK.exe"
$s.Arguments = $args2
$s.WorkingDirectory = $dest
$s.IconLocation = (Join-Path $dest "TRACK.exe") + ",0"
$s.Description = "TRACK app test (track-app $rev) - QLC+'s engine with its own beat-link helper"
$s.Save()
Say "Desktop shortcut: TRACK (test) -> TRACK.exe $args2" "Green"

$blt = Find-Blt
if ($blt) {
    Say ("Beat Link Trigger found: {0} - TRACK's beat-link helper runs on its Java ({1})" -f $blt.Root, $blt.Java) "Green"
} else {
    Say "Beat Link Trigger was NOT found - TRACK's beat-link helper needs its Java and beat-link. Install BLT 8 as before." "Red"
}

Say ""
Say "To test:" "Cyan"
Say "  1. Close QLC+ AND Beat Link Trigger (only one QLC+ and one beat-link program can run at a time)."
Say "  2. Double-click 'TRACK (test)' on the desktop. The Track page opens; after a few seconds the"
Say "     helper finds the CDJs and the first track arrives - as with BLT, but no BLT window."
Say "  3. The helper's log: %LOCALAPPDATA%\qlc-rig-update\bridge.log (and TRACK lines in startup.log)."
Say "  Back to normal: close TRACK, then start QLC+ and BLT as always."
