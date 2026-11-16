#!/usr/bin/env python3
"""TRACK_APP_CASAMBI_R620 - the restaurant's Casambi lights follow LAST TRACK.

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
    casambi_helper.exe --selftest                        CI: every module is in the exe
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
                    self.say('connect-failed %s %s' % (dev.address, type(e).__name__))
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


def main() -> int:
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
        return asyncio.run(Helper(cfg).run())
    except Exception as e:  # noqa: BLE001
        print('crash %s: %s' % (type(e).__name__, str(e)[:120]), flush=True)
        logging.exception('crash')
        return 1


if __name__ == '__main__':
    sys.exit(main())
