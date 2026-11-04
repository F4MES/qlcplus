# TRACK app + beat-link helper (branch `track-app`)

Tobias 11-04: *"Lav en ny gren, og byg de to vi lige har lagt i backloggen på den, så kan
jeg teste i weekenden om det virker."* This is the first testable version of both backlog
items. It lives only on this branch. `track-layout` and `upstream-merge` are untouched, and so
are `rig-latest` and `update-qlc.cmd`.

## What it is

**TRACK app.** `TRACK.exe --track-app` is QLC+'s engine and Track page in kiosk mode. It is
unzipped into its own folder (`%LOCALAPPDATA%\TRACK-test\<build>`), so nothing is installed
over the rig's QLC+. It opens the same `PSMAIN.qxw`, reads the same QLC+ settings (labs,
looks, likes) and drives the same DMX. QLC+'s single-instance guard still means only one of
TRACK and QLC+ can run at a time.

**beat-link helper.** In `--track-app` mode, QLC+ starts `trackbridge.jar` itself
(`qmlui/trackbridge.cpp`), watches it, and starts it again if it stops. When QLC+ quits, it
closes the helper's pipe and the helper stops. The helper is a hidden Java process. It runs
on Beat Link Trigger's own installed Java and jar, so BLT has to be installed, but BLT
itself is never started.

The helper runs `qlc_shared_functions.clj` **unchanged**. It loads the copy from
`Downloads\qlc-update` (where UPDATE puts it), or the copy inside the jar if that file is
missing. It calls the file the way BLT's one trigger did:

- **Watch:** Master Player
- **Beat:** `(qlc-beat! beat-number)`
- **Tracked Update:** `(qlc-tracked-update! device-number beat-number playing?)`

It goes online the way BLT does:

- the same finders, with RGB waveforms;
- no real player number;
- metadata passive when a player allows only three database clients.

It talks to the Track page over the same TCP port (9998) and protocol.

**Read only.** Like BLT, the helper announces itself as a virtual player so it can get
metadata. It never sends status, sync, master or load commands.

**One beat-link program at a time.** The helper takes the BLT startup guard's lock,
`%ProgramData%\qlc-rig\blt-instance.lock`:

- If BLT is already running, the helper exits with code 3. QLC+ tries again every 15 s, and
  BLT keeps feeding TRACK in the meantime.
- If the helper is running and BLT is started, the guarded BLT says it is already running.

## Files

| File | What |
|---|---|
| `trackbridge/src/qlc/bridge.clj` | The helper: lock, log, BLT's go-online and trigger, loads the shared functions |
| `trackbridge/qlc_shared_functions.clj` | Fallback copy, used when `Downloads\qlc-update\qlc_shared_functions.clj` is missing |
| `trackbridge/make_jar.py` | Builds `trackbridge.jar`. It holds sources only, so there is nothing to compile |
| `trackbridge/smoke_test.py` | CI test against BLT 8.0.0's jar: hello on 9998, lock code 3, pipe stop |
| `qmlui/trackbridge.{h,cpp}` | Finds BLT's Java and jar, starts and watches the helper, logs to `startup.log` |
| `qmlui/main.cpp` | `--track-app` = kiosk + the helper |
| `tools/get-track-app.cmd` | On the lys-PC: downloads `track-app-latest`, unzips it beside QLC+, makes the "TRACK (test)" shortcut |
| `.github/workflows/build.yml` | `trackbridge-smoke`, the portable folder, and `release-track-app` (this branch only) |

## Logs on the lys-PC

All logs are in `%LOCALAPPDATA%\qlc-rig-update\`:

- `bridge.log`: the helper and beat-link, rotated at 2 MB with 5 backups;
- `bridge-console.log`: the JVM's own output;
- `startup.log`: the `TRACK` lines from QLC+.

## Newer beat-link (CDJ-3000X)

Jars in `trackbridge-lib\` beside `TRACK.exe` go first on the classpath. A newer beat-link
placed there therefore wins over the one inside BLT's jar. This is the route to the
CDJ-3000X, once we have built, tested and released a beat-link that reads it.

## Limits of this first version

- BLT must be installed (8.0.0 or later, with its bundled Java).
- It has not been tested against real CDJs. In the cloud it ran end to end against a
  simulated CDJ-2000NXS2 (keep-alives, status and beats on a separate network): it went
  online, sent a bare track after 4 s without metadata, sent positions, and handled
  stop/play.
- There is no new UI. START BLT on the empty Track page still starts BLT. While the helper
  runs, the guarded BLT just says it is already running.
- PS rig only: the same show and the same rig_room.json.
