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

# R516_LEAD_MAINJAR (runde 516, Tobias 10-30: "BLT guard NOT installed: Unsupported BLT launcher"): BLT's
# Windows build is jpackage --main-jar, so its cfg says app.mainjar= (with app.mainclass= taken from the jar's
# manifest, or - older jpackage - without it). That launcher is turned into the classpath one the guard
# already handles: app.mainjar=X becomes app.classpath=X and the guard is the main class. $JarMain is the
# main class from the jar's manifest, for a cfg without app.mainclass= (the caller reads it). app.mainmodule
# (a modular launcher) is still refused, and nothing is written then.
function Get-GuardedBltConfig([string]$Text, [string]$JarName, [string]$JarMain = '') {
    if ($JarName -notmatch '^qlc-blt-guard-[a-f0-9]{16}\.jar$') { throw 'Invalid guard jar name.' }
    $newline = if ($Text.Contains("`r`n")) { "`r`n" } else { "`n" }
    $lines = [regex]::Split($Text, '\r?\n')
    foreach ($section in @('Application', 'JavaOptions')) {
        if (@($lines | Where-Object { $_ -eq "[$section]" }).Count -ne 1) {
            throw "Unsupported BLT configuration: [$section] must occur once."
        }
    }
    $main = @($lines | Where-Object { $_ -match '^app\.mainclass=' })
    $mainJar = @($lines | Where-Object { $_ -match '^app\.mainjar=' })
    if ($main.Count -gt 1 -or $mainJar.Count -gt 1 -or ($main.Count + $mainJar.Count) -eq 0 -or
        @($lines | Where-Object { $_ -match '^app\.mainmodule=' }).Count) {
        throw 'Unsupported BLT launcher. No files were changed.'
    }
    if ($main.Count -eq 1) { $original = $main[0].Substring('app.mainclass='.Length) }
    elseif ($JarMain) { $original = $JarMain }
    else { throw 'Unsupported BLT launcher (no main class in the cfg or the jar). No files were changed.' }
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
        } elseif ($section -eq 'Application' -and $line -match '^app\.mainjar=(.+)$') {
            $jar = $Matches[1]                              # R516_LEAD_MAINJAR: BLT's jar onto the classpath
            if ($jar -notmatch '^\$APPDIR[\\/]') { $jar = '$APPDIR\' + $jar }
            $result.Add('app.classpath=' + $jar)
        } else { $result.Add($line) }
        if ($line -eq '[Application]') {
            $result.Add('app.classpath=$APPDIR\' + $JarName)
            if ($main.Count -eq 0) { $result.Add('app.mainclass=org.qlcplus.rig.BltGuardMain') }
        }
        if ($line -eq '[JavaOptions]') { $result.Add('java-options=-Dqlc.blt.main=' + $original) }
    }
    return [string]::Join($newline, $result)
}

# R516_LEAD_JARMAIN: the Main-Class of the cfg's app.mainjar (only read when the cfg has no app.mainclass=);
# "" when there is none or it cannot be read - Get-GuardedBltConfig then refuses, nothing is written
function Get-JarMainClass([string]$Text, [string]$CfgPath) {
    if ($Text -match '(?m)^app\.mainclass=') { return '' }
    if ($Text -notmatch '(?m)^app\.mainjar=(.+?)\r?$') { return '' }
    $jar = $Matches[1] -replace '^\$APPDIR[\\/]', ''
    $path = Join-Path (Split-Path $CfgPath) $jar
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { return '' }
    try {
        Add-Type -AssemblyName System.IO.Compression.FileSystem
        $zip = [IO.Compression.ZipFile]::OpenRead($path)
        try {
            $entry = $zip.GetEntry('META-INF/MANIFEST.MF')
            if (-not $entry) { return '' }
            $reader = New-Object IO.StreamReader($entry.Open())
            try { $manifest = $reader.ReadToEnd() } finally { $reader.Dispose() }
        } finally { $zip.Dispose() }
        if ($manifest -match '(?m)^Main-Class:\s*(\S+)') { return $Matches[1] }
    } catch { }
    return ''
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
        [pscustomobject]@{ Path = $cfg; Old = $old; New = (Get-GuardedBltConfig $old $jarName (Get-JarMainClass $old $cfg)) }   # R516_LEAD_JARMAIN
    }
)

# R522_GUARD_SKIP (Tobias 10-31: a UAC box for Windows PowerShell on every UPDATE): a guard already in place -
# every cfg as the guard writes it, this guard's jar beside it, the PC's lock file there - changes nothing, so it
# asks for nothing. The lock's ACL write below is what needed the administrator on every run.
$lockReady = ($env:OS -ne 'Windows_NT') -or ($env:ProgramData -and (Test-Path -LiteralPath (Join-Path $env:ProgramData 'qlc-rig\blt-instance.lock')))
$pending = @($plans | Where-Object { $_.Old -cne $_.New -or -not (Test-Path -LiteralPath (Join-Path (Split-Path $_.Path) $jarName)) })
if ($lockReady -and $pending.Count -eq 0) {
    foreach ($plan in $plans) { Write-Host "BLT guard already installed: $($plan.Path)" }
    return
}

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
