#!/usr/bin/env python3
"""TRACK_APP_CASAMBI_R620 - R635: built with casambi-bt-revamped (the same CasambiBt API; it also speaks the
Classic protocol the restaurant's network uses - casambi-bt 0.3.2 refused it: UnsupportedProtocolVersion).

 - the restaurant's Casambi lights follow LAST TRACK.

Tobias 11-16: "kun bruge det til last track hvor den skal skrue alle enheder langsomt op til 100%
henover last-track" - "Det skal formegentlig først checke hvilket niveau lamperne står på fra starten
og starte derfra, så den ikke først slukker alt lyset og så begynder at skrue op".

TRACK (trackengine.cpp, casambiTick) starts this when LAST TRACK is armed and talks to it on stdin,
one line at a time:

    p <0..1>   how far the last track is (LAST TRACK's own white curve; 1 = the house light)
    undo       LAST TRACK was undone before the house light: every lamp back where it stood
    quit       stop, leave the lamps as they are (stdin closing = the same)

Each lamp starts from its OWN level and only ever goes up, to 100 % at p = 1 - nothing is dimmed or
switched off first. A lamp whose level was not known joins from p = 0.5 (from 50 %). Commands go to
one lamp at a time and only when it has moved ~2 %, so the Bluetooth mesh is not flooded.
Once every lamp is at 100 % the helper lets go of the network and exits.

Everything it does is one short line on stdout (TRACK logs it as sig:casambi:<line>) and in
casambi.log beside the config. Whatever goes wrong here, TRACK and the rig are not touched.

    casambi_helper.exe <Documents/QLC+/casambi.json>      config: {"network": "PS", "password": "..."}
    casambi_helper.exe <Documents/QLC+/casambi.json> --page   SETUP > CASAMBI (below)
    casambi_helper.exe --selftest                        CI: every module is in the exe

TRACK_APP_CASAMBIPAGE_R632 (Tobias 11-21: "en casambi side under settings hvor man foerst og fremmest kan se
hvilke lys den kan se (evt. styre dem derfra?)" - "Saa vil det ogsaa fungere som en test om den faktisk kan se
lysene"). With --page the helper connects, tells every lamp and stays until told to go:

    out  unit <id>|<level 0-255 or ?>|<online 1/0>|<model>|<name>     a lamp (again whenever it changes)
    out  units <n> known <m> offline <k>                              after connecting and after "scan"
    in   set <id> <0-255>      that lamp now                   in   all <0-255>    every lamp
    in   back                  every lamp the page moved, back where it stood when the page connected
    in   scan                  tell every lamp again           in   quit           go (the lamps stay as they are)
    out  scene <id>|<name> ... scenes <n>                              the network's scenes (R637, after the lamps)
    in   scene <id> <0-255>    that scene at that relative level -> out  scene-set <id> <level> | scene-failed <id> <why>
    R639 (SETUP > CASAMBI > DELAY TEST):
    in   now <0-255> <id,id,...>         those lamps at once, one command after the other, no pacing (TRACK times it)
    in   measure <gen> <count> <id,id,...>   per lamp: <count> level changes, each timed until the lamp reports it
    out  measure <gen> <id> <i> <ms|timeout|offline> ... measure-done <gen>   (the lamp is put back where it stood)
    in   measure-stop

TRACK_APP_CASSTORE_R645 (the night of 11-21/22: every connect from 01:29 to 05:10 failed with "PermissionError
[WinError 5] Access is denied" on Documents/QLC+/casambi-store - LAST TRACK at 03:00, 04:08, 04:14 and 05:09 never
reached the lamps; at 14:14 after a fresh Windows start it worked again). Before the first connect:
 - an old casambi_helper.exe whose TRACK is gone (QLC+ closed or crashed under it) is closed:
       out  stale-helper <pid> killed | stale-helper <pid> kill-failed | other-helper <pid> (TRACK <pid> running)
 - the cache folder is tried (a file written in each folder, each file opened for writing). Locked: what holds it
   (Windows' Restart Manager) is logged, and the next folder is used - casambi-store-b beside it, then
   %LOCALAPPDATA%/TRACK/casambi-store (a copy of the cache when it can be read, otherwise a fresh one: the
   network is fetched again with the password):
       out  store-locked <folder> <file> <error> [by <exe>(<pid>) ...]    out  store <folder>
 - a connect that still fails on a locked file moves to the next folder at once and tries again.
 Errors are logged in full (300 characters, with the file) and with their traceback in casambi-debug.log.

The status lines (connected / no-casambi-found / wrong-password / ...) are LAST TRACK's.
"""
import asyncio
import json
import logging
import os
import shutil
import sys
import threading
import time
from pathlib import Path

LEVEL_STEP = 5          # ~2 %: a lamp gets a new level only when it has moved this much (0-255)
SEND_GAP = 0.08         # s between two commands - spread over the mesh
CONNECT_TRIES = 3
FULL_HOLD = 5.0         # s at 100 % before letting go (a late undo still finds us)


def selftest() -> int:
    import bleak  # noqa: F401
    import CasambiBt  # noqa: F401
    from CasambiBt import Casambi, discover  # noqa: F401
    if sys.platform == 'win32':
        import bleak.backends.winrt.client  # noqa: F401
        import bleak.backends.winrt.scanner  # noqa: F401
    import httpx  # noqa: F401
    import cryptography  # noqa: F401
    import bleak_retry_connector  # noqa: F401   R635: casambi-bt-revamped (Classic networks) needs it
    import anyio  # noqa: F401
    print('selftest ok', flush=True)
    return 0


# ---------------------------------------------------------------- TRACK_APP_CASSTORE_R645
STORE_NAMES = ('casambi-store', 'casambi-store-b', 'local')   # local: %LOCALAPPDATA%/TRACK/casambi-store


def store_path(base: Path, name: str) -> Path:
    if name == 'local':
        root = os.environ.get('LOCALAPPDATA') or os.environ.get('TEMP') or str(base)
        return Path(root) / 'TRACK' / 'casambi-store'
    return base / name


def err_text(e: BaseException, n: int = 300) -> str:
    return '%s %s' % (type(e).__name__, ' '.join(str(e).split())[:n])


def is_lock_error(e: BaseException) -> bool:
    return isinstance(e, PermissionError) or getattr(e, 'winerror', None) in (5, 32, 33)


def probe_store(path: Path):
    """(ok, the file or folder that failed, the error): a file written and removed in every folder, every file
    opened for writing - what the cache does when it connects"""
    if not path.exists():
        try:
            path.mkdir(parents=True)                       # the cache makes it again (it wants its own version file)
        except OSError as e:
            return False, path, e
    try:
        items = list(path.rglob('*'))[:400]
    except OSError as e:
        return False, path, e
    for d in [path] + [q for q in items if q.is_dir()]:
        t = d / ('.track-probe-%d' % os.getpid())
        try:
            t.write_bytes(b'1')
            t.unlink()
        except OSError as e:
            return False, d, e
    for q in items:
        if q.is_file():
            try:
                with open(q, 'r+b'):
                    pass
            except OSError as e:
                return False, q, e
    return True, None, None


def win_processes() -> dict:
    """pid -> (exe, parent pid), Windows only (a ToolHelp snapshot)"""
    if sys.platform != 'win32':
        return {}
    import ctypes
    from ctypes import wintypes

    class PE(ctypes.Structure):
        _fields_ = [('dwSize', wintypes.DWORD), ('cntUsage', wintypes.DWORD), ('th32ProcessID', wintypes.DWORD),
                    ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wintypes.DWORD),
                    ('cntThreads', wintypes.DWORD), ('th32ParentProcessID', wintypes.DWORD),
                    ('pcPriClassBase', ctypes.c_long), ('dwFlags', wintypes.DWORD), ('szExeFile', wintypes.WCHAR * 260)]
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
    k.CreateToolhelp32Snapshot.argtypes = [wintypes.DWORD, wintypes.DWORD]
    k.Process32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PE)]
    k.Process32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(PE)]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    snap = k.CreateToolhelp32Snapshot(0x2, 0)
    if not snap or snap == ctypes.c_void_p(-1).value:
        return {}
    out = {}
    try:
        e = PE()
        e.dwSize = ctypes.sizeof(PE)
        ok = k.Process32FirstW(snap, ctypes.byref(e))
        while ok:
            out[int(e.th32ProcessID)] = (e.szExeFile, int(e.th32ParentProcessID))
            ok = k.Process32NextW(snap, ctypes.byref(e))
    finally:
        k.CloseHandle(snap)
    return out


def win_terminate(pid: int) -> bool:
    import ctypes
    from ctypes import wintypes
    k = ctypes.WinDLL('kernel32', use_last_error=True)
    k.OpenProcess.restype = wintypes.HANDLE
    k.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    k.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    k.CloseHandle.argtypes = [wintypes.HANDLE]
    h = k.OpenProcess(0x0001, False, pid)          # PROCESS_TERMINATE
    if not h:
        return False
    try:
        return bool(k.TerminateProcess(h, 9))
    finally:
        k.CloseHandle(h)


def win_lockers(path: Path) -> list:
    """[(pid, name)] of the processes holding that file open (Windows' Restart Manager)"""
    if sys.platform != 'win32' or not path.is_file():
        return []
    import ctypes
    from ctypes import wintypes

    class RM_UNIQUE_PROCESS(ctypes.Structure):
        _fields_ = [('dwProcessId', wintypes.DWORD), ('ProcessStartTime', wintypes.FILETIME)]

    class RM_PROCESS_INFO(ctypes.Structure):
        _fields_ = [('Process', RM_UNIQUE_PROCESS), ('strAppName', wintypes.WCHAR * 256),
                    ('strServiceShortName', wintypes.WCHAR * 64), ('ApplicationType', ctypes.c_int),
                    ('AppStatus', wintypes.ULONG), ('TSSessionId', wintypes.DWORD), ('bRestartable', wintypes.BOOL)]
    rm = ctypes.WinDLL('rstrtmgr')
    session = wintypes.DWORD(0)
    key = ctypes.create_unicode_buffer(64)
    if rm.RmStartSession(ctypes.byref(session), 0, key) != 0:
        return []
    try:
        names = (wintypes.LPCWSTR * 1)(str(path))
        if rm.RmRegisterResources(session, 1, names, 0, None, 0, None) != 0:
            return []
        needed = wintypes.UINT(0)
        count = wintypes.UINT(0)
        reasons = wintypes.DWORD(0)
        res = rm.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), None, ctypes.byref(reasons))
        if res not in (0, 234) or needed.value == 0:      # 234 = ERROR_MORE_DATA: there are some
            return []
        arr = (RM_PROCESS_INFO * needed.value)()
        count = wintypes.UINT(needed.value)
        if rm.RmGetList(session, ctypes.byref(needed), ctypes.byref(count), arr, ctypes.byref(reasons)) != 0:
            return []
        return [(int(arr[i].Process.dwProcessId), arr[i].strAppName) for i in range(count.value)]
    finally:
        rm.RmEndSession(session)


def stale_helpers(procs: dict, me: int, parent: int):
    """[(pid, kill, owner)] for every other casambi_helper.exe: a one-file exe is two processes (the boot loader and
    Python under it) - the pair's owner is the boot loader's parent (TRACK). Owner gone = stale (kill)"""
    out = []
    seen = set()
    for pid, (exe, pp) in sorted(procs.items()):
        if exe.lower() != 'casambi_helper.exe' or pid in (me, parent) or pid in seen:
            continue
        top = pid
        if pp in procs and procs[pp][0].lower() == 'casambi_helper.exe':
            top = pp
        if top in (me, parent):
            continue
        group = {top} | {p for p, (x, q) in procs.items() if q == top and x.lower() == 'casambi_helper.exe'}
        seen |= group
        owner = procs.get(top, ('', 0))[1]
        out.append((top, owner not in procs, owner, sorted(group)))
    return out


class Helper:
    def __init__(self, cfg_path: Path):
        self.cfg_path = cfg_path
        cfg = json.loads(cfg_path.read_text(encoding='utf-8'))
        self.network = str(cfg.get('network', 'PS'))
        self.password = str(cfg['password'])
        self.store = cfg_path.parent / 'casambi-store'
        self.stores_tried: list[Path] = []                 # TRACK_APP_CASSTORE_R645
        self.store_ready = False
        self.logf = (cfg_path.parent / 'casambi.log').open('a', encoding='utf-8')
        self.casa = None
        self.units = []
        self.start: dict[int, int | None] = {}
        self.sent: dict[int, int | None] = {}
        self.changed: set[int] = set()
        self.p = 0.0
        self.cmds: asyncio.Queue[str] = asyncio.Queue()
        self.held: str | None = None     # a command read while draining p's - next in line

    def say(self, msg: str) -> None:
        print(msg, flush=True)
        self.logf.write('%s %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), msg))
        self.logf.flush()

    # ---------------------------------------------------------------- stdin
    def read_stdin(self, loop: asyncio.AbstractEventLoop) -> None:
        for line in sys.stdin:
            loop.call_soon_threadsafe(self.cmds.put_nowait, line.strip())
        loop.call_soon_threadsafe(self.cmds.put_nowait, 'quit')

    # ---------------------------------------------------------------- TRACK_APP_CASSTORE_R645
    def clear_stale(self) -> None:
        try:
            procs = win_processes()
            killed = 0
            for top, stale, owner, group in stale_helpers(procs, os.getpid(), os.getppid()):
                if not stale:
                    self.say('other-helper %d (TRACK %d running)' % (top, owner))
                    continue
                ok = all([win_terminate(p) for p in sorted(group, reverse=True)])
                killed += 1
                self.say('stale-helper %d %s' % (top, 'killed' if ok else 'kill-failed'))
            if killed:
                time.sleep(1.0)                            # Windows lets go of its files and Bluetooth
        except Exception as e:  # noqa: BLE001
            logging.exception('clear_stale')
            self.say('stale-check-failed %s' % err_text(e, 120))

    def next_store(self, why: str = '') -> bool:
        """the next cache folder that is not locked; False when none is left"""
        base = self.cfg_path.parent
        first = store_path(base, STORE_NAMES[0])
        for name in STORE_NAMES:
            path = store_path(base, name)
            if path in self.stores_tried:
                continue
            self.stores_tried.append(path)
            n = -1
            if path != first and not path.exists():
                n = 0
                if first.exists():
                    for f in first.rglob('*'):            # what can be read of the cache: no need to fetch it again
                        try:
                            t = path / f.relative_to(first)
                            if f.is_dir():
                                t.mkdir(parents=True, exist_ok=True)
                            elif not f.name.startswith('.track-probe'):
                                t.parent.mkdir(parents=True, exist_ok=True)
                                shutil.copyfile(f, t)
                                n += 1
                        except OSError:
                            pass
            ok, bad, e = probe_store(path)
            if not ok:
                by = ''
                try:
                    lk = win_lockers(bad) if bad is not None else []
                    names = win_processes() if lk else {}
                    by = ''.join(' by %s(%d)' % (names.get(pid, (n, 0))[0] or n, pid) for pid, n in lk)
                except Exception:  # noqa: BLE001
                    logging.exception('win_lockers')
                self.say('store-locked %s %s %s%s' % (name, bad, err_text(e, 160), by))
                continue
            if path != first:
                self.say('store %s%s%s' % (name, (' (copied %d files)' % n) if n >= 0 else '', (' - ' + why) if why else ''))
            self.store = path
            return True
        return False

    # ---------------------------------------------------------------- network
    async def connect(self) -> bool:
        from CasambiBt import Casambi, discover
        from CasambiBt.errors import AuthenticationError
        if not self.store_ready:                           # TRACK_APP_CASSTORE_R645: once, before the first connect
            self.store_ready = True
            self.clear_stale()
            if not self.next_store():
                self.say('no-store every cache folder is locked')
        for attempt in range(1, CONNECT_TRIES + 1):
            try:
                devices = await discover()
            except Exception as e:  # noqa: BLE001
                self.say('bluetooth-error %s' % type(e).__name__)
                await asyncio.sleep(5)
                continue
            if not devices:
                self.say('no-casambi-found try %d' % attempt)
                await asyncio.sleep(3)
                continue
            devices.sort(key=lambda d: -(getattr(d, 'rssi', None) or -999))
            tried = set()
            for dev in devices:
                if dev.address in tried:
                    continue
                tried.add(dev.address)
                failed = False
                for _store_try in range(len(STORE_NAMES)):   # TRACK_APP_CASSTORE_R645: a locked cache -> the next
                    casa = Casambi(cachePath=self.store)
                    t0 = time.perf_counter()
                    try:
                        await casa.connect(dev, self.password)
                    except AuthenticationError:
                        self.say('wrong-password %s' % dev.address)
                        await self.safe_disconnect(casa)
                        failed = True
                        break
                    except Exception as e:  # noqa: BLE001
                        logging.exception('connect-failed %s', dev.address)
                        self.say('connect-failed %s %s' % (dev.address, err_text(e)))
                        await self.safe_disconnect(casa)
                        if is_lock_error(e) and self.next_store('the cache was locked'):
                            continue
                        failed = True
                        break
                    break
                if failed:
                    continue
                if casa.networkName.strip().lower() != self.network.strip().lower():
                    self.say('other-network %s' % casa.networkName)
                    await self.safe_disconnect(casa)
                    continue
                self.casa = casa
                self.say('connected %s %.1fs' % (casa.networkName, time.perf_counter() - t0))
                return True
            await asyncio.sleep(3)
        self.say('gave-up')
        return False

    @staticmethod
    async def safe_disconnect(casa) -> None:
        try:
            await casa.disconnect()
        except Exception:  # noqa: BLE001
            pass

    def read_levels(self) -> None:
        self.units = list(self.casa.units)
        known = 0
        for u in self.units:
            d = u.state.dimmer if u.state is not None else None
            self.start[u.deviceId] = d
            self.sent[u.deviceId] = d
            known += d is not None
        self.say('units %d known %d' % (len(self.units), known))

    def target(self, dev_id: int) -> int | None:
        s = self.start.get(dev_id)
        p = self.p
        if s is None:
            if p < 0.5:
                return None
            return round(128 + 127 * (p - 0.5) / 0.5)
        return round(s + (255 - s) * p)

    async def send(self, unit, level: int) -> bool:
        try:
            await self.casa.setLevel(unit, level)
            return True
        except Exception as e:  # noqa: BLE001
            self.say('send-failed #%d %s' % (unit.deviceId, type(e).__name__))
            return False

    async def follow(self) -> bool:
        """one pass over the lamps; True when every lamp is at 100 %"""
        all_full = True
        for u in self.units:
            t = self.target(u.deviceId)
            last = self.sent.get(u.deviceId)
            if t is None:
                all_full = False
                continue
            if last is not None and t <= last:
                if last < 255:
                    all_full = False
                continue                                  # only ever up
            if last is not None and t - last < LEVEL_STEP and not (self.p >= 1.0 and t == 255):
                all_full = False
                continue
            if await self.send(u, t):
                self.sent[u.deviceId] = t
                self.changed.add(u.deviceId)
            if t < 255:
                all_full = False
            await asyncio.sleep(SEND_GAP)
        return all_full

    async def restore(self) -> None:
        by_id = {u.deviceId: u for u in self.units}
        n = 0
        for dev_id in sorted(self.changed):
            s = self.start.get(dev_id)
            if s is None or dev_id not in by_id:
                continue
            if await self.send(by_id[dev_id], s):
                n += 1
            await asyncio.sleep(SEND_GAP)
        self.say('restored %d' % n)

    # ---------------------------------------------------------------- main
    async def run(self) -> int:
        loop = asyncio.get_running_loop()
        threading.Thread(target=self.read_stdin, args=(loop,), daemon=True).start()
        self.say('start network=%s' % self.network)
        if not await self.connect():
            return 2
        await asyncio.sleep(2.0)                          # the lamps report their levels
        self.read_levels()
        full_since = None
        try:
            while True:
                if self.held is not None:
                    cmd, self.held = self.held, None
                else:
                    try:
                        cmd = await asyncio.wait_for(self.cmds.get(), timeout=0.5)
                    except asyncio.TimeoutError:
                        cmd = ''
                # only the newest p counts - drain what queued up while we were sending
                while cmd.startswith('p ') and not self.cmds.empty():
                    nxt = self.cmds.get_nowait()
                    if not nxt.startswith('p '):
                        self.held = nxt
                        break
                    cmd = nxt
                if cmd == 'quit':
                    self.say('quit')
                    return 0
                if cmd == 'undo':
                    await self.restore()
                    return 0
                if cmd.startswith('p '):
                    try:
                        self.p = max(self.p, min(1.0, max(0.0, float(cmd[2:]))))
                    except ValueError:
                        pass
                if self.casa is not None and not self.casa.connected:
                    self.say('lost - reconnecting')
                    await self.safe_disconnect(self.casa)
                    self.casa = None
                    if not await self.connect():
                        return 3
                full = await self.follow()
                if full and self.p >= 1.0:
                    if full_since is None:
                        full_since = time.monotonic()
                        self.say('full')
                    elif time.monotonic() - full_since >= FULL_HOLD:
                        self.say('done')
                        return 0
        finally:
            if self.casa is not None:
                await self.safe_disconnect(self.casa)


def write_network(helper, casa) -> None:
    """TRACK_APP_CASAMBINET_R638 (Tobias 11-21: "skal gerne skrive navnene/id'erne eller hvad du skal bruge i loggen
    til dig selv, saa vi senere maaske kan bruge nogle af lamperne/grupperne til mere"): the network as TRACK sees
    it, in Documents/QLC+/casambi-network.txt (SEND TO CLAUDE takes it) - lamps, groups, scenes, and the scenes /
    groups exactly as the cached network data has them (is a scene's lamp level in there?). No key, no password:
    the raw part is the "scenes" and "groups" lists only."""
    try:
        import glob
        units = list(getattr(casa, 'units', None) or [])
        groups = list(getattr(casa, 'groups', None) or [])
        scenes = list(getattr(casa, 'scenes', None) or [])
        net = getattr(casa, '_casaNetwork', None)
        out = ['# Casambi network as TRACK sees it - %s - casambi_helper (no keys, no password)' % time.strftime('%Y-%m-%d %H:%M:%S'),
               'network %s | protocol %s | revision %s' % (getattr(casa, 'networkName', '?'),
                                                         getattr(net, 'protocolVersion', '?'), getattr(net, '_networkRevision', '?')),
               '', 'UNITS (%d)' % len(units), 'id | name | model | manufacturer | mode | controls | online | level']
        for u in sorted(units, key=lambda x: x.deviceId):
            ut = getattr(u, 'unitType', None)
            ctl = ','.join(str(getattr(getattr(c, 'type', None), 'name', '?')) for c in (getattr(ut, 'controls', None) or []))
            d = u.state.dimmer if u.state is not None else None
            out.append('%d | %s | %s | %s | %s | %s | %d | %s' % (u.deviceId, u.name, getattr(ut, 'model', ''), getattr(ut, 'manufacturer', ''),
                                                              getattr(ut, 'mode', ''), ctl, 1 if u.online else 0, '?' if d is None else d))
        out += ['', 'GROUPS (%d)' % len(groups), 'id | name | unit ids']
        for g in groups:
            out.append('%s | %s | %s' % (getattr(g, 'groudId', getattr(g, 'groupId', '?')), g.name,
                                         ','.join(str(u.deviceId) for u in (getattr(g, 'units', None) or []))))
        out += ['', 'SCENES (%d)' % len(scenes), 'id | name']
        for s in scenes:
            out.append('%d | %s' % (s.sceneId, s.name))
        # the cached network data: its scenes and groups as they are
        raw = None
        for f in sorted(glob.glob(str(helper.store / '**' / '*.json'), recursive=True), key=lambda x: -Path(x).stat().st_mtime):
            try:
                d = json.loads(Path(f).read_text(encoding='utf-8'))
            except Exception:  # noqa: BLE001
                continue
            if isinstance(d, dict) and isinstance(d.get('network'), dict) and 'scenes' in d['network']:
                raw = d['network']
                break
        out += ['', 'RAW (cached network data: "scenes" and "groups" only)']
        if raw is None:
            out.append('no cached network file found')
        else:
            out.append('network keys: ' + ', '.join(sorted(k for k in raw.keys())))
            for key in ('scenes', 'groups'):
                for item in raw.get(key) or []:
                    out.append('%s: %s' % (key[:-1], json.dumps(item, ensure_ascii=False)[:2000]))
        (helper.cfg_path.parent / 'casambi-network.txt').write_text('\n'.join(out) + '\n', encoding='utf-8')
        print('network-file %d units %d groups %d scenes' % (len(units), len(groups), len(scenes)), flush=True)
    except Exception as e:  # noqa: BLE001
        print('network-file-failed %s' % type(e).__name__, flush=True)


class Page(Helper):
    """TRACK_APP_CASAMBIPAGE_R632: SETUP > CASAMBI - every lamp, and a hand on them"""

    def __init__(self, cfg_path: Path):
        super().__init__(cfg_path)
        self.pending: dict[int, int] = {}
        self.pend_scene: tuple[int, int] | None = None      # R637: the newest scene asked for
        self.now_cmds: list[tuple[int, list[int]]] = []      # R639: test commands, sent at once
        self.waiting = None                                  # R639: (id, level, future) - MEASURE waits for the lamp
        self.measure_task = None
        self.measure_stop = False

    def say(self, msg: str) -> None:
        super().say('page ' + msg if msg.startswith('unit ') is False and msg.startswith('units ') is False
                    else msg)

    @staticmethod
    def clean(s) -> str:
        return ' '.join(str(s or '').replace('|', '/').split())[:60]

    def tell(self, u) -> None:
        d = u.state.dimmer if u.state is not None else None
        model = getattr(getattr(u, 'unitType', None), 'model', '') or ''
        print('unit %d|%s|%d|%s|%s' % (u.deviceId, '?' if d is None else int(d), 1 if u.online else 0,
                                       self.clean(model), self.clean(u.name) or ('#%d' % u.deviceId)), flush=True)

    def tell_all(self) -> None:
        self.units = list(self.casa.units)
        known = sum(1 for u in self.units if u.state is not None and u.state.dimmer is not None)
        off = sum(1 for u in self.units if not u.online)
        for u in self.units:
            self.tell(u)
        print('units %d known %d offline %d' % (len(self.units), known, off), flush=True)
        # TRACK_APP_ROOMLIGHT_R637: the scenes made in the Casambi app (ROOM LIGHT's list)
        scenes = list(getattr(self.casa, 'scenes', None) or [])
        for s in scenes:
            print('scene %d|%s' % (s.sceneId, self.clean(s.name) or ('#%d' % s.sceneId)), flush=True)
        print('scenes %d' % len(scenes), flush=True)
        self.logf.write('%s page units %d known %d offline %d\n' % (time.strftime('%Y-%m-%d %H:%M:%S'),
                                                                   len(self.units), known, off))
        self.logf.flush()

    def on_unit(self, u) -> None:
        try:
            w = self.waiting                              # R639: MEASURE - the lamp's own word that it is there
            if w is not None and u.deviceId == w[0] and not w[2].done():
                d = u.state.dimmer if u.state is not None else None
                if d is not None and abs(d - w[1]) <= 6:
                    w[2].set_result(True)
            self.tell(u)
        except Exception:  # noqa: BLE001
            pass

    async def run_measure(self, gen: int, count: int, ids: list[int]) -> None:
        """R639: per lamp, count level changes; each timed from the command until the lamp reports the level"""
        loop = asyncio.get_running_loop()
        by_id = {u.deviceId: u for u in self.units}
        try:
            for dev_id in ids:
                u = by_id.get(dev_id)
                if u is None or not u.online:
                    print('measure %d %d -1 offline' % (gen, dev_id), flush=True)
                    continue
                orig = u.state.dimmer if u.state is not None else None
                hi = (orig is None) or orig < 128              # the first change goes the other way
                for i in range(count):
                    if self.measure_stop:
                        break
                    target = 204 if hi else 51
                    hi = not hi
                    fut = loop.create_future()
                    self.waiting = (dev_id, target, fut)
                    t0 = time.perf_counter()
                    await self.send(u, target)
                    try:
                        await asyncio.wait_for(fut, 6.0)      # R641: lamps up to a few seconds late
                        print('measure %d %d %d %d' % (gen, dev_id, i, int((time.perf_counter() - t0) * 1000)), flush=True)
                    except asyncio.TimeoutError:
                        print('measure %d %d %d timeout' % (gen, dev_id, i), flush=True)
                    self.waiting = None
                    await asyncio.sleep(0.3)
                if orig is not None:
                    await self.send(u, orig)
                    await asyncio.sleep(SEND_GAP)
        finally:
            self.waiting = None
            print('measure-done %d' % gen, flush=True)

    def take(self, cmd: str) -> bool:
        """False: quit"""
        parts = cmd.split()
        if not parts:
            return True
        if parts[0] == 'quit':
            return False
        try:
            if parts[0] == 'set' and len(parts) == 3:
                self.pending[int(parts[1])] = max(0, min(255, int(parts[2])))
            elif parts[0] == 'all' and len(parts) == 2:
                lvl = max(0, min(255, int(parts[1])))
                for u in self.units:
                    if u.online:                          # a lamp that does not answer is left out
                        self.pending[u.deviceId] = lvl
            elif parts[0] == 'back':
                for dev_id in sorted(self.changed):
                    s = self.start.get(dev_id)
                    if s is not None:
                        self.pending[dev_id] = s
            elif parts[0] == 'scan':
                self.tell_all()
            elif parts[0] == 'scene' and len(parts) == 3:        # R637
                self.pend_scene = (int(parts[1]), max(0, min(255, int(parts[2]))))
            elif parts[0] == 'now' and len(parts) == 3:          # R639
                self.now_cmds.append((max(0, min(255, int(parts[1]))), [int(x) for x in parts[2].split(',') if x]))
            elif parts[0] == 'measure' and len(parts) == 4:      # R639
                if self.measure_task is not None and not self.measure_task.done():
                    self.measure_task.cancel()                # a new one: the old one's lines carry its own number
                self.measure_stop = False
                self.measure_task = asyncio.ensure_future(
                    self.run_measure(int(parts[1]), max(1, min(40, int(parts[2]))), [int(x) for x in parts[3].split(',') if x]))
            elif parts[0] == 'measure-stop':
                self.measure_stop = True
        except ValueError:
            pass
        return True

    async def run(self) -> int:
        loop = asyncio.get_running_loop()
        threading.Thread(target=self.read_stdin, args=(loop,), daemon=True).start()
        self.say('start network=%s' % self.network)
        if not await self.connect():
            return 2
        await asyncio.sleep(2.0)                          # the lamps report their levels
        self.units = list(self.casa.units)
        for u in self.units:                              # BACK's levels: as they stood when the page connected
            self.start[u.deviceId] = u.state.dimmer if u.state is not None else None
        self.tell_all()
        write_network(self, self.casa)                    # TRACK_APP_CASAMBINET_R638
        try:
            self.casa.registerUnitChangedHandler(self.on_unit)
        except Exception:  # noqa: BLE001
            pass
        by_id = {u.deviceId: u for u in self.units}
        try:
            while True:
                try:
                    cmd = await asyncio.wait_for(self.cmds.get(), timeout=0.5)
                except asyncio.TimeoutError:
                    cmd = ''
                cmds = [cmd]
                while not self.cmds.empty():
                    cmds.append(self.cmds.get_nowait())
                for c in cmds:
                    if not self.take(c):
                        self.say('quit')
                        return 0
                if self.casa is not None and not self.casa.connected:
                    self.say('lost - reconnecting')
                    await self.safe_disconnect(self.casa)
                    self.casa = None
                    if not await self.connect():
                        return 3
                    await asyncio.sleep(2.0)
                    self.tell_all()
                    try:
                        self.casa.registerUnitChangedHandler(self.on_unit)
                    except Exception:  # noqa: BLE001
                        pass
                    by_id = {u.deviceId: u for u in self.units}
                # R639: a test's commands first, at once - TRACK keeps the time
                while self.now_cmds:
                    lvl, ids = self.now_cmds.pop(0)
                    for dev_id in ids:
                        u = by_id.get(dev_id)
                        if u is not None:
                            await self.send(u, lvl)
                # the newest level per lamp, one lamp at a time; new commands are read between two lamps
                while self.pending:
                    dev_id, lvl = next(iter(self.pending.items()))
                    del self.pending[dev_id]
                    u = by_id.get(dev_id)
                    if u is None:
                        continue
                    if await self.send(u, lvl):
                        self.changed.add(dev_id)
                        print('unit %d|%d|%d|%s|%s' % (dev_id, lvl, 1 if u.online else 0,
                                                       self.clean(getattr(getattr(u, 'unitType', None), 'model', '')),
                                                       self.clean(u.name) or ('#%d' % dev_id)), flush=True)
                    await asyncio.sleep(SEND_GAP)
                    while not self.cmds.empty():
                        if not self.take(self.cmds.get_nowait()):
                            self.say('quit')
                            return 0
                if self.pend_scene is not None:              # R637: the newest scene, after the lamps
                    sid, lvl = self.pend_scene
                    self.pend_scene = None
                    sc = next((s for s in (getattr(self.casa, 'scenes', None) or []) if s.sceneId == sid), None)
                    if sc is None:
                        print('scene-failed %d gone' % sid, flush=True)
                    else:
                        try:
                            await self.casa.switchToScene(sc, lvl)
                            print('scene-set %d %d' % (sid, lvl), flush=True)
                            self.logf.write('%s page scene-set %d %s %d\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), sid, self.clean(sc.name), lvl))
                            self.logf.flush()
                        except Exception as e:  # noqa: BLE001
                            print('scene-failed %d %s' % (sid, type(e).__name__), flush=True)
                    await asyncio.sleep(SEND_GAP)
        finally:
            if self.casa is not None:
                await self.safe_disconnect(self.casa)


def main() -> int:
    # R636: TRACK reads every line as UTF-8. On Windows a piped stdout is the ANSI code page (cp1252), and the
    # lamps' Danish names came out as "Emh?tte" / "Glasv?g" / "s?jler"
    try:
        sys.stdout.reconfigure(encoding='utf-8', errors='replace')
    except Exception:  # noqa: BLE001
        pass
    if len(sys.argv) > 1 and sys.argv[1] == '--selftest':
        return selftest()
    if len(sys.argv) < 2:
        print('usage: casambi_helper <casambi.json>', flush=True)
        return 1
    cfg = Path(sys.argv[1])
    if not cfg.exists():
        print('no-config', flush=True)
        return 1
    logging.basicConfig(level=logging.WARNING,
                        handlers=[logging.FileHandler(cfg.parent / 'casambi-debug.log', encoding='utf-8')])
    try:
        if len(sys.argv) > 2 and sys.argv[2] == '--page':     # TRACK_APP_CASAMBIPAGE_R632
            return asyncio.run(Page(cfg).run())
        return asyncio.run(Helper(cfg).run())
    except Exception as e:  # noqa: BLE001
        print('crash %s: %s' % (type(e).__name__, str(e)[:120]), flush=True)
        logging.exception('crash')
        return 1


if __name__ == '__main__':
    sys.exit(main())
