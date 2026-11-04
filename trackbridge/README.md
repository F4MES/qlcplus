# TRACK app + beat-link helper (branch `track-app`)

This branch is the TRACK app as a program of its own. It lives only here: `track-layout`,
`upstream-merge`, `rig-latest` and `update-qlc.cmd` are untouched.

- **r547** (Tobias 11-04): *"Lav en ny gren, og byg de to vi lige har lagt i backloggen
  på den."*
- **r548** (Tobias 11-04): *"den skal kunne deployes på en ren pc og så bare virke fra
  start ... bygge opdateringen bedre, så den bare opdatere inde i qlc ... og genstarter sig
  selv ... og fjern de knapper vi alligevel ikke kan bruge mere."* The mockup was approved.

## r549: its own name and look, full screen, the phone

- **Its own name and logo** (Tobias 11-04: *"QLC skal fjernes fra navne, logoer osv"*): the
  program file, the launcher, the window and the shortcut are all TRACK, with its own logo
  (`branding/`: the gold waveform tile). The window title is "TRACK". The shared start
  guard says "TRACK cannot start while QLC+ (or TRACK) is running" instead of the QLC+
  text.
- **Full screen from the start.** `--track-app` always starts in full screen, whether or
  not `-f` is given.
- **The top of the page** (mockup A was approved). On the Track page, the kiosk strip
  (TRACK / VIRTUAL CONSOLE, BPM, LINK and the beat dot) is gone: TRACK / VIRTUAL CONSOLE sit
  over the clock. The Virtual Console keeps the strip as it is. The four beat dots are on
  the time's line, above PAUSED.
- **The phone** (*"husk også telefonen i track-app"*): TRACK always serves the phone
  (`http://<pc>:9999/track`), as the rig's `-w` does. On the phone, UPDATE is TRACK's own
  update, and the phone says TRACK.
- **Defender:**
  - `get-track-app.cmd` reads Windows Defender's history for TRACK's files and says what it
    finds. It only reads; nothing in Defender is changed.
  - Both programs carry version information, and TRACK still starts no PowerShell.

## r550: a tidier SETUP (TRACK app only, mockup approved)

The top row of SETUP no longer has RE-GUESS, FULL AUTO or SELF TEST:

- FULL AUTO is always on in the TRACK app.
- RE-GUESS has moved into ADVANCED as RE-GUESS ROLES, under TRACK ANALYSIS. It still takes
  two taps.

A group switches on or off with two taps: the first tap asks SURE?, and the second, within
4 s, switches it. Holding a group still makes it BASE.

## On the PC: `%LOCALAPPDATA%\TRACK`

| Path | What |
|---|---|
| `TRACK.exe` | The launcher (`launcher/track_launcher.c`). The desktop shortcut points here. |
| `current.txt` / `previous.txt` | The build it starts, and the one before. |
| `pending.txt` | A build being started for the first time. If it stops with an error within 20 s, the launcher goes back to `previous.txt`. |
| `app\<build>\` | `TRACK-engine.exe` (QLC+, `--track-app`), `trackbridge.jar`, `runtime.txt` |
| `runtime\<id>\` | `java\` (Eclipse Temurin JRE 21) and `beat-link-trigger.jar` (BLT 8.0.0, unmodified) |
| `logs\` | `track.log` (TRACK, the update, the launcher), `bridge.log` (the helper and beat-link), `bridge-console.log` |

The first install is `tools/get-track-app.cmd`. It also works on a clean PC: TRACK brings
its own Java and beat-link, so neither BLT nor QLC+ has to be installed. Where there is no
QLC+ shortcut with a show, it fetches the rig's `PSMAIN.qxw` into `Documents\TRACK` once and
never writes over it afterwards.

**What stays where it was.** Some files are deliberately not moved into the TRACK folder:
QLC+'s settings (in the registry), the labs, likes and saved looks, the analysis cache
(`~/.qlc-track-cache.edn`) and the played log. TRACK and the rig's QLC+ therefore share the
same choices.

## The update (inside TRACK)

`qmlui/trackapp.cpp` reads `track-app.json` in the release `track-app-latest`. It checks 30 s
after start, then every 5 min (r551; it was 30 min), using Windows' own `curl.exe`. A check reads
one small file in the background, so it costs nothing while the show runs. When there is a newer build:

1. It downloads the build in the background, at most 4 MB/s. If the new build needs another
   runtime, that is downloaded too.
2. It checks the size and the SHA-256.
3. It unpacks with Windows' own `tar.exe` into `app\<build>`.

Only then does the R474 chip appear, as **UPDATE TRACK**. Two taps with SHOW OFF do the
rest: TRACK writes `current.txt` (keeping `previous.txt`), starts the launcher with the same
arguments plus `--restart-after <pid>`, and quits. The new TRACK waits until the old one
is gone, then starts. The light is out for a few seconds.

Old builds and runtimes are removed after a start. The current build and the one before are
always kept. No PowerShell is used (R517: that is what Windows Defender hunted).

## beat-link helper

`trackbridge.jar` (`src/qlc/bridge.clj`) runs `qlc_shared_functions.clj` **unchanged**,
using the copy inside the jar. It calls it the way BLT's one trigger did:

- **Watch:** Master Player
- **Beat:** `(qlc-beat! beat-number)`
- **Tracked Update:** `(qlc-tracked-update! device-number beat-number playing?)`

It goes online the way BLT does. It only **reads**: it never sends status, sync, master or
load commands.

It takes the BLT startup guard's lock, `%ProgramData%\qlc-rig\blt-instance.lock`. While BLT
runs, the helper waits and BLT feeds TRACK.

It prints `STATUS cdjs=N` (the number of players, 1-6), which QLC+ reads for the page:
"2 players found" and "CDJs ok".

If TRACK's runtime folder is missing, the helper falls back to an installed BLT's Java and
jar.

## The page (TRACK_APP_UI_R548)

`feature_track_app.py` in qlcplus-pr does this. It runs after the generators, in this tree
only. Everything is gated on `trackAppCtl.active`, so this tree's QLC+ without
`--track-app` looks exactly as before. In the TRACK app:

- no START BLT and no BLT logo;
- the empty page says "Looking for the CDJs" or "N players found";
- the status line says "CDJs ok" or "no CDJs";
- the (i) guide has no taskbar steps;
- the update chip is TRACK's own.

## Newer beat-link (CDJ-3000X)

Jars in `app\<build>\trackbridge-lib\` go first on the classpath, so a newer beat-link there
wins over the one in the runtime. The other way is a new runtime id with a newer jar, which
`make_release.py` builds.

## Files in the repo

| File | What |
|---|---|
| `trackbridge/src/qlc/bridge.clj` | The helper |
| `trackbridge/qlc_shared_functions.clj` | The functions TRACK runs (a copy from qlcplus-pr; update it with each TRACK round) |
| `trackbridge/make_jar.py` / `make_release.py` | `trackbridge.jar` / the release files (app zip, runtime zip, `track-app.json`) |
| `trackbridge/launcher/track_launcher.c` | `TRACK.exe` |
| `trackbridge/smoke_test.py` | CI: the helper on BLT 8.0.0's jar |
| `qmlui/trackapp.{h,cpp}`, `qmlui/trackbridge.{h,cpp}` | TRACK's object (`trackAppCtl`): the helper, CDJ count, update, restart |
| `tools/get-track-app.cmd` | First install |

## Limits

- Not yet tested on Windows against real CDJs. That is the weekend test.
- The first start: Windows may ask whether Java may use the network. Allow it on private
  networks.
- Only the PS rig: the same show and the same `rig_room.json`.
