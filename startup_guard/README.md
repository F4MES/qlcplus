# Lighting-PC startup guards

QLC+ (Windows): both `qlcplus.exe` and `qlcplus-qml.exe` acquire the same global
Windows mutex **before QApplication**. Its handle stays alive until process exit.
Simultaneous starts cannot both proceed, even from different installed copies or
Windows sessions. The extra process exits; it never closes the existing one.
Guard errors fail closed. Other operating systems retain their existing behavior.

BLT (Windows installer / jpackage app): `tools/install-blt-guard.ps1` inserts a
small Java bootstrap in the launcher's `app/*.cfg`. The original BLT jar, options,
arguments, shortcuts, preferences and shared functions are preserved. No Java agent
or extra runtime module is needed. A versioned guard jar goes beside BLT's jar;
the configuration is replaced atomically and its previous version is kept.

The bootstrap takes an exclusive OS file lock before loading BLT's own main class.
It covers every shortcut/direct executable start of that patched installation,
and shares its identity between patched installations and Windows users/sessions
through `%ProgramData%\qlc-rig\blt-instance.lock`. The installer grants Windows
users read/write access to that lock; it does not grant deletion access.
Lock handles live until JVM exit, including after BLT's main returns to Swing.
Crash/forced termination releases the lock; the lock file is deliberately retained.
QLC+ and BLT have independent locks and can run together.

The rig release builds `qlc-blt-guard.jar`. The normal QLC updater downloads it
and the installer, discovers BLT through its shortcuts, install registry and usual
locations, and installs the guard (Windows may ask for administrator access).
**BLT must be fully restarted once before this protects it.** A BLT already running
without the guard is never terminated or claimed to be protected.

Portable installations can be patched with `-AppPath 'C:\path\Beat Link Trigger'`.
Direct `java -jar` starts and unpatched BLT installations are
outside the BLT guarantee. A later BLT MSI upgrade can replace its configuration;
run the QLC updater again afterwards. No watcher kills existing processes.

Build locally: `python startup_guard/compile_blt_guard.py qlc-blt-guard.jar` (JDK 9+).
Tests: configure/build this directory with CMake, then run
`python startup_guard/test_guards.py --probe-dir guard-build` with a JDK and PowerShell.
The Windows CI job exercises real kernel locks and the real Windows launcher cfg;
local macOS checks cannot replace a lighting-PC acceptance test. The rig release
waits for the startup-guard tests on both Windows and Linux before publishing.

Source of truth in the collaboration workspace: `feature_single_instance.py`,
`startup_guard/*`, `rig_update/install-blt-guard.ps1`, `rig_update/update-qlc.cmd`
and `fix_ci_rig_release.py`. Run the first patch twice on both project trees.
