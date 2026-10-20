<# Install the startup guard in a Windows jpackage BLT installation.
   Existing shortcuts and BLT's jar, settings and shared functions are preserved.
   Takes effect on BLT's next complete restart. Never closes a running BLT.
   -AppPath accepts an installation directory or its .exe for portable copies.
   -LibraryOnly exposes the pure config transformation to the regression tests. #>
param(
    [string]$AppPath = '',
    [string]$JarPath = "$PSScriptRoot\qlc-blt-guard.jar",
    [switch]$LibraryOnly
)
$ErrorActionPreference = 'Stop'

function Get-GuardedBltConfig([string]$Text, [string]$JarName) {
    if ($JarName -notmatch '^qlc-blt-guard-[a-f0-9]{16}\.jar$') { throw 'Invalid guard jar name.' }
    $newline = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = [regex]::Split($Text, '\r?\n')
    foreach ($section in @('Application', 'JavaOptions')) {
        if (@($lines | Where-Object { $_ -eq "[$section]" }).Count -ne 1) {
            throw "Unsupported BLT configuration: [$section] must occur once."
        }
    }
    $main = @($lines | Where-Object { $_ -match '^app\.mainclass=' })
    if ($main.Count -ne 1 -or @($lines | Where-Object { $_ -match '^app\.(mainmodule|mainjar)=' }).Count) {
        throw 'Unsupported BLT launcher. No files were changed.'
    }
    $original = $main[0].Substring('app.mainclass='.Length)
    if ($original -eq 'org.qlcplus.rig.BltGuardMain') {
        $saved = @($lines | Where-Object { $_ -match '^java-options=-Dqlc\.blt\.main=' })
        if ($saved.Count -ne 1) { throw 'Missing original BLT main class.' }
        $original = $saved[0].Substring('java-options=-Dqlc.blt.main='.Length)
    }
    if ($original -ne 'beat_link_trigger.BeatLinkTrigger') { throw "Unknown BLT main class: $original" }

    $result = New-Object 'System.Collections.Generic.List[string]'
    $section = ''
    foreach ($line in $lines) {
        if ($line -match '^\[(.+)\]$') { $section = $Matches[1] }
        if ($section -eq 'Application' -and $line -match '^app\.classpath=\$APPDIR[\\/]qlc-blt-guard-[a-f0-9]{16}\.jar$') { continue }
        if ($section -eq 'JavaOptions' -and $line -match '^java-options=-Dqlc\.blt\.main=') { continue }
        if ($section -eq 'Application' -and $line -match '^app\.mainclass=') {
            $result.Add('app.mainclass=org.qlcplus.rig.BltGuardMain')
        } else { $result.Add($line) }
        if ($line -eq '[Application]') { $result.Add('app.classpath=$APPDIR\' + $JarName) }
        if ($line -eq '[JavaOptions]') { $result.Add('java-options=-Dqlc.blt.main=' + $original) }
    }
    return [string]::Join($newline, $result)
}

if ($LibraryOnly) { return }
if (-not (Test-Path -LiteralPath $JarPath -PathType Leaf)) { throw "Missing BLT guard: $JarPath" }
$jarName = 'qlc-blt-guard-' + (Get-FileHash -LiteralPath $JarPath -Algorithm SHA256).Hash.Substring(0,16).ToLowerInvariant() + '.jar'

$roots = New-Object 'System.Collections.Generic.List[string]'
if ($AppPath) {
    if (Test-Path -LiteralPath $AppPath -PathType Leaf) { $roots.Add((Split-Path $AppPath)) }
    else { $roots.Add($AppPath) }
} else {
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
    $shell = New-Object -ComObject WScript.Shell
    foreach ($place in @([Environment]::GetFolderPath('Desktop'), [Environment]::GetFolderPath('CommonDesktopDirectory'), [Environment]::GetFolderPath('StartMenu'), [Environment]::GetFolderPath('CommonStartMenu'), [Environment]::GetFolderPath('Startup'), (Join-Path $env:APPDATA 'Microsoft\Internet Explorer\Quick Launch\User Pinned\TaskBar'))) {
        if (-not $place -or -not (Test-Path -LiteralPath $place)) { continue }
        foreach ($lnk in Get-ChildItem -LiteralPath $place -Filter '*.lnk' -Recurse -ErrorAction SilentlyContinue) {
            $target = $shell.CreateShortcut($lnk.FullName).TargetPath
            if ($target -and [IO.Path]::GetFileName($target) -like '*Beat Link Trigger*.exe') { $roots.Add((Split-Path $target)) }
        }
    }
}

$configs = @(
    foreach ($root in @($roots | Sort-Object -Unique)) {
        $app = Join-Path $root 'app'
        if (-not (Test-Path -LiteralPath $app -PathType Container)) { continue }
        foreach ($cfg in Get-ChildItem -LiteralPath $app -Filter '*.cfg') {
            if (Test-Path -LiteralPath (Join-Path $root ($cfg.BaseName + '.exe'))) { $cfg.FullName }
        }
    }
) | Sort-Object -Unique
if (-not $configs) { throw 'No supported BLT installation found. Use -AppPath for a portable copy.' }

$plans = @(
    foreach ($cfg in $configs) {
        $old = [IO.File]::ReadAllText($cfg)
        [pscustomobject]@{ Path = $cfg; Old = $old; New = (Get-GuardedBltConfig $old $jarName) }
    }
)

# One lock for the PC, also across Windows users/sessions. Grant only the
# access needed to open/lock it; the bootstrap never deletes the lock file.
if ($env:OS -eq 'Windows_NT') {
    if (-not $env:ProgramData) { throw 'ProgramData is missing; cannot install a machine-wide BLT guard.' }
    $lockDir = Join-Path $env:ProgramData 'qlc-rig'
    New-Item -ItemType Directory -Force -Path $lockDir | Out-Null
    $lockPath = Join-Path $lockDir 'blt-instance.lock'
    if (-not (Test-Path -LiteralPath $lockPath)) {
        # OpenOrCreate preserves the file and any active lock during reinstallation.
        $handle = [IO.File]::Open($lockPath, [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::Write, [IO.FileShare]::ReadWrite)
        $handle.Dispose()
    }
    $users = New-Object Security.Principal.SecurityIdentifier('S-1-5-32-545')
    $acl = Get-Acl -LiteralPath $lockDir
    $acl.SetAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($users, 'ReadAndExecute', 'Allow')))
    Set-Acl -LiteralPath $lockDir -AclObject $acl
    $acl = Get-Acl -LiteralPath $lockPath
    $acl.SetAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($users, 'Read, Write', 'Allow')))
    Set-Acl -LiteralPath $lockPath -AclObject $acl
}

foreach ($plan in $plans) {
    $cfg = $plan.Path
    $old = $plan.Old
    $new = $plan.New
    if ([IO.File]::ReadAllText($cfg) -cne $old) { throw "BLT configuration changed during installation: $cfg" }
    $destination = Join-Path (Split-Path $cfg) $jarName
    # Versioned jar: never overwrite a jar held open by a running Java VM.
    if (-not (Test-Path -LiteralPath $destination)) { Copy-Item -LiteralPath $JarPath -Destination $destination }
    if ((Get-FileHash -LiteralPath $destination).Hash -ne (Get-FileHash -LiteralPath $JarPath).Hash) { throw 'BLT guard copy failed verification.' }
    if ($old -cne $new) {
        $temporary = $cfg + '.guard-' + [guid]::NewGuid().ToString('N')
        $backup = $cfg + '.before-guard-' + (Get-Date -Format 'yyyyMMdd-HHmmss') + '-' + [guid]::NewGuid().ToString('N')
        try {
            [IO.File]::WriteAllText($temporary, $new, (New-Object Text.UTF8Encoding($false)))
            [IO.File]::Replace($temporary, $cfg, $backup)
        } finally {
            if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary }
        }
    }
    Write-Host "BLT guard installed: $cfg. Active after BLT is fully closed and started again."
}
