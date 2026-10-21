<# : rollback-qlc.cmd - ONE file: this batch part runs PowerShell on the rest of this same file
@echo off
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -Command "& ([ScriptBlock]::Create([IO.File]::ReadAllText('%~f0'))) %*"
echo.
pause
exit /b
#>
<#
  rollback-qlc.cmd - back to what ran before the last update-qlc.cmd, in one
  double-click (runde 467, Tobias 10-15: "Rollback i update-qlc.cmd").

  It shows what it will do and asks once:
    QLC+        the build installed before this one (its installer is still in
                Downloads\qlc-update as QLC-rig-setup-<build>.exe)
    PSMAIN.qxw  the newest PSMAIN.qxw.bak-<time> beside the show file
    BLT         the newest qlc_shared_functions.clj.bak-<time> in Downloads\qlc-update
  Answer  y = all of it,  q = only QLC+,  n = nothing.
  What it replaces is kept (<name>.rolledback-<time>) - nothing is ever deleted.
  update-qlc.cmd will not install a build you rolled back from again; a newer
  build it will (update-qlc.cmd -Force installs it anyway).
  Options (after the file name in a cmd window):
      -Yes        no question - all of it
      -NoStart    do not start QLC+ again afterwards
      -ShowPath X the show file (when no shortcut points at one)
#>
param(
    [switch]$Yes,
    [switch]$NoStart,
    [string]$ShowPath = ""
)

$ErrorActionPreference = "Stop"

$work  = Join-Path $env:USERPROFILE "Downloads\qlc-update"
$state = Join-Path $env:LOCALAPPDATA "qlc-rig-update\state.json"

function Say($text, $colour = "Gray") { Write-Host $text -ForegroundColor $colour }

function Read-State {
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

function Save-State($s) {
    New-Item -ItemType Directory -Force -Path (Split-Path $state) | Out-Null
    $s | ConvertTo-Json | Set-Content -Path $state -Encoding UTF8
}

function Get-QlcProcesses { return @(Get-Process -Name "qlcplus-qml", "qlcplus" -ErrorAction SilentlyContinue) }

# the build an installer file is for: QLC-rig-setup-<build>.exe
function Rev-Of($file) {
    $m = [regex]::Match([IO.Path]::GetFileNameWithoutExtension($file), '^QLC-rig-setup-(.+)$')
    if ($m.Success) { return $m.Groups[1].Value } else { return "" }
}

# the installer to go back to: the build installed before this one (the history
# update-qlc.cmd keeps from runde 467), else the newest other installer kept
function Find-Previous($s) {
    if (-not (Test-Path $work)) { return $null }
    $all = @(Get-ChildItem -Path $work -Filter "QLC-rig-setup-*.exe" -ErrorAction SilentlyContinue |
             Where-Object { (Rev-Of $_.Name) -and (Rev-Of $_.Name) -ne "new" } |
             Sort-Object LastWriteTime -Descending)
    $hist = @($s.history)
    # from the build running now backwards - a second rollback goes one further back
    $start = [Array]::LastIndexOf([string[]]$hist, [string]$s.exeRev)
    if ($start -lt 0) { $start = $hist.Count }
    for ($i = $start - 1; $i -ge 0; $i--) {
        if ($hist[$i] -eq $s.exeRev -or $s.rolledBack -contains $hist[$i]) { continue }
        $f = $all | Where-Object { (Rev-Of $_.Name) -eq $hist[$i] } | Select-Object -First 1
        if ($f) { return $f }
    }
    return $all | Where-Object { (Rev-Of $_.Name) -ne $s.exeRev -and $s.rolledBack -notcontains (Rev-Of $_.Name) } | Select-Object -First 1
}

# the QLC+ shortcut that opens the show - as update-qlc.cmd finds it
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

# the newest <file>.bak-<time> beside a file
function Find-Backup($path) {
    if (-not $path) { return $null }
    $dir = Split-Path $path
    if (-not $dir -or -not (Test-Path $dir)) { return $null }
    $leaf = Split-Path $path -Leaf
    return Get-ChildItem -Path $dir -Filter ($leaf + ".bak-*") -ErrorAction SilentlyContinue |
           Sort-Object LastWriteTime -Descending | Select-Object -First 1
}

# put a backup back; what was there is kept as <file>.rolledback-<time>
function Put-Back($backup, $path) {
    if (Test-Path $path) {
        $keep = "{0}.rolledback-{1}" -f $path, (Get-Date -Format "yyyyMMdd-HHmm")
        Copy-Item -Path $path -Destination $keep -Force
        Say "  kept what was there as $keep"
    }
    Copy-Item -Path $backup.FullName -Destination $path -Force
}

function Close-Qlc {
    $procs = Get-QlcProcesses
    if ($procs.Count -eq 0) { return $false }
    foreach ($p in $procs) { try { [void]$p.CloseMainWindow() } catch { } }
    $deadline = (Get-Date).AddSeconds(15)
    while ((Get-QlcProcesses).Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
    if ((Get-QlcProcesses).Count -gt 0) {
        Say "  QLC+ did not close by itself - stopping it" "Yellow"
        Get-QlcProcesses | Stop-Process -Force
        # R487_B5_KILLWAIT: until it is gone - a dying one still holds the single-instance guard
        $deadline = (Get-Date).AddSeconds(15)
        while ((Get-QlcProcesses).Count -gt 0 -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
        if ((Get-QlcProcesses).Count -gt 0) { Say "  QLC+ is still closing - if it does not come back, start it by hand" "Red" }
    }
    Say "  QLC+ closed."
    return $true
}

# ---------------------------------------------------------------- the plan
Say "QLC+ rollback" "Cyan"
$s = Read-State
$prev = Find-Previous $s
$sc = $null
try { $sc = Find-Shortcut } catch { }
if (-not $ShowPath -and $sc) { $ShowPath = $sc.Show }
$showBak = Find-Backup $ShowPath
$cljPath = Join-Path $work "qlc_shared_functions.clj"
$cljBak = Find-Backup $cljPath

$now = if ($s.exeRev) { $s.exeRev } else { "(unknown)" }
if ($prev) { Say ("  QLC+        {0}  ->  {1}   ({2:yyyy-MM-dd HH:mm})" -f $now, (Rev-Of $prev.Name), $prev.LastWriteTime) }
else       { Say  "  QLC+        no older installer in $work - QLC+ stays as it is" "Yellow" }
if ($showBak) { Say ("  PSMAIN.qxw  back to {0}" -f $showBak.Name) }
else          { Say  "  PSMAIN.qxw  no backup beside $(if ($ShowPath) { $ShowPath } else { 'the show file (no shortcut found)' }) - stays" "Yellow" }
if ($cljBak)  { Say ("  BLT         back to {0}" -f $cljBak.Name) }
else          { Say  "  BLT         no backup of qlc_shared_functions.clj - stays" "Yellow" }

if (-not $prev -and -not $showBak -and -not $cljBak) {
    Say "Nothing to roll back to." "Yellow"
    exit 1
}

$answer = "y"
if (-not $Yes) {
    $answer = Read-Host "Roll back?  y = all of it,  q = only QLC+,  n = nothing"
}
if ($answer -notmatch '^[yYjJqQ]') {
    Say "Nothing changed." "Green"
    exit 0
}
$onlyQlc = $answer -match '^[qQ]'

# ---------------------------------------------------------------- do it
$wasRunning = (Get-QlcProcesses).Count -gt 0
$closed = $false
if ($prev -or ($showBak -and -not $onlyQlc)) { $closed = Close-Qlc }

if ($prev) {
    $rev = Rev-Of $prev.Name
    Say "  installing QLC+ $rev (silent) ..."
    # an installer that cannot run must not leave QLC+ closed: QLC+ starts again below
    $p = $null
    try { $p = Start-Process -FilePath $prev.FullName -ArgumentList "/S" -Wait -PassThru }
    catch { Say "  the installer could not run: $($_.Exception.Message)" "Red" }
    if ($p -and $p.ExitCode -eq 0) {
        if ($s.exeRev) { $s.rolledBack = @($s.rolledBack | Where-Object { $_ -ne $s.exeRev }) + @($s.exeRev) }
        $s.exeRev = $rev
        Save-State $s
        Say "QLC+ $rev is back." "Green"
    } else {
        Say ("The installer ended with code {0} - QLC+ may still be the newer build." -f $(if ($p) { $p.ExitCode } else { "-" })) "Red"
    }
}
if ($showBak -and -not $onlyQlc) {
    Put-Back $showBak $ShowPath
    $s.showSha = (Get-FileHash $ShowPath -Algorithm SHA256).Hash
    Save-State $s
    Say "PSMAIN.qxw is back: $ShowPath" "Green"
}
if ($cljBak -and -not $onlyQlc) {
    Put-Back $cljBak $cljPath
    Say "BLT's functions are back: $cljPath  -  in BLT: File > Update (or restart BLT) to load them." "Green"
}

if (($wasRunning -or $closed) -and -not $NoStart) {
    if ($sc) { Start-Process -FilePath $sc.Link }
    else {
        $dir = (Get-ItemProperty -Path "HKCU:\Software\qlcplus" -Name Install_Dir -ErrorAction SilentlyContinue).Install_Dir
        if ($dir -and (Test-Path (Join-Path $dir "qlcplus-qml.exe"))) { Start-Process -FilePath (Join-Path $dir "qlcplus-qml.exe") }
    }
    Say "  QLC+ started again."
}
Say ("Done. update-qlc.cmd will not put {0} back by itself." -f ($s.rolledBack -join ", ")) "Cyan"
