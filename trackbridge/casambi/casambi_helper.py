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

The status lines (connected / no-casambi-found / wrong-password / ...) are LAST TRACK's.
"""
import asyncio
import json
import logging
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


class Helper:
    def __init__(self, cfg_path: Path):
        self.cfg_path = cfg_path
        cfg = json.loads(cfg_path.read_text(encoding='utf-8'))
        self.network = str(cfg.get('network', 'PS'))
        self.password = str(cfg['password'])
        self.store = cfg_path.parent / 'casambi-store'
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

    # ---------------------------------------------------------------- network
    async def connect(self) -> bool:
        from CasambiBt import Casambi, discover
        from CasambiBt.errors import AuthenticationError
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
                casa = Casambi(cachePath=self.store)
                t0 = time.perf_counter()
                try:
                    await casa.connect(dev, self.password)
                except AuthenticationError:
                    self.say('wrong-password %s' % dev.address)
                    await self.safe_disconnect(casa)
                    continue
                except Exception as e:  # noqa: BLE001
                    self.say('connect-failed %s %s %s' % (dev.address, type(e).__name__, ' '.join(str(e).split())[:80]))
                    await self.safe_disconnect(casa)
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
