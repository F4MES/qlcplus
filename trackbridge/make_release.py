#!/usr/bin/env python3
"""Builds the TRACK app's release files (track-app branch, runde 548).

    python3 trackbridge/make_release.py --base TRACK-app-base.zip --rev abc1234 \\
        --jre OpenJDK21U-jre_x64_windows.zip --blt beat-link-trigger-8.0.0.jar --out out/

out/ gets:
  TRACK-app.zip               the build: TRACK-engine.exe (QLC+, run with --track-app),
                              TRACK-launcher.exe, trackbridge.jar, track-app-build.txt,
                              runtime.txt (the runtime it needs), TRACK-README.md,
                              TRACK-fixtures/ (the rig's own fixture definitions)
  TRACK-runtime-<id>.zip      Java (java/) and beat-link (beat-link-trigger.jar) - the same
                              bytes for the same JRE and BLT jar, so a PC downloads it once
  track-app.json              rev, and size + SHA-256 of both zips (TRACK's update and
                              get-track-app.cmd read it)
  track-app-build.txt         the rev (one line)

Tobias 11-04: "den skal kunne deployes paa en ren pc og saa bare virke fra start".
"""
import argparse
import hashlib
import json
import re
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FIXED = (2020, 1, 1, 0, 0, 0)      # fixed times: the same input gives the same zip
NOTICE = """TRACK runtime - what is in this folder

java/                    Eclipse Temurin JRE (Eclipse Adoptium), GPLv2 with the Classpath
                         Exception - https://adoptium.net
beat-link-trigger.jar    Beat Link Trigger by Deep Symmetry (James Elliott), unmodified,
                         Eclipse Public License 2.0 - https://github.com/Deep-Symmetry/beat-link-trigger
                         (it carries beat-link, Clojure and the libraries TRACK's helper uses)

TRACK runs them as a separate process (trackbridge.jar); nothing of them is built into QLC+.
"""


def safe(text):
    return re.sub(r'[^A-Za-z0-9._-]', '_', text)


def put(z, name, data, mode=0o644):
    info = zipfile.ZipInfo(name, FIXED)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (mode & 0xFFFF) << 16
    z.writestr(info, data)


def sha_size(path):
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest(), Path(path).stat().st_size


def runtime_id(jre, blt):
    with zipfile.ZipFile(jre) as z:
        rel = [n for n in z.namelist() if n.count('/') == 1 and n.endswith('/release')]
        text = z.read(rel[0]).decode('utf-8', 'replace') if rel else ''
    m = re.search(r'^JAVA_RUNTIME_VERSION="([^"]+)"', text, re.M) or re.search(r'^JAVA_VERSION="([^"]+)"', text, re.M)
    java = m.group(1) if m else 'unknown'
    version = 'unknown'
    with zipfile.ZipFile(blt) as z:
        for n in z.namelist():
            if n.startswith('META-INF/maven/beat-link-trigger/') and n.endswith('pom.properties'):
                for line in z.read(n).decode('utf-8', 'replace').splitlines():
                    if line.startswith('version='):
                        version = line[8:].strip()
    return safe('jre%s-blt%s' % (java, version))


def build_runtime(jre, blt, target):
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as out, zipfile.ZipFile(jre) as src:
        for info in sorted(src.infolist(), key=lambda i: i.filename):
            parts = info.filename.split('/', 1)
            if len(parts) < 2 or not parts[1] or info.is_dir():
                continue                                    # the JRE's own top folder becomes java/
            put(out, 'java/' + parts[1], src.read(info), 0o755 if '/bin/' in '/' + parts[1] else 0o644)
        put(out, 'beat-link-trigger.jar', Path(blt).read_bytes())
        put(out, 'RUNTIME-NOTICE.txt', NOTICE)


def round_of(label):
    """TRACK_APP_BUILDNOTE_R601: "TRACK app R601: ..." -> "R601" (the first R-number in the commit's subject)"""
    m = re.search(r'\bR(\d{3,5})\b', label or '')
    return 'R' + m.group(1) if m else ''


def build_app(base, rev, rid, target, rnd=''):
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as out, zipfile.ZipFile(base) as src:
        names = set(src.namelist())
        assert 'qlcplus-qml.exe' in names, 'no qlcplus-qml.exe in the portable folder'
        assert 'TRACK-launcher.exe' in names, 'no TRACK-launcher.exe in the portable folder'
        for info in src.infolist():
            if info.is_dir():
                continue
            name = 'TRACK-engine.exe' if info.filename == 'qlcplus-qml.exe' else info.filename
            put(out, name, src.read(info))
        jar = Path(target).parent / 'trackbridge.jar'
        import importlib.util
        spec = importlib.util.spec_from_file_location('make_jar', HERE / 'make_jar.py')
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        mod.build(jar)
        put(out, 'trackbridge.jar', jar.read_bytes())
        jar.unlink()
        put(out, 'track-app-build.txt', rev + '\n')
        if rnd:
            put(out, 'track-app-round.txt', rnd + '\n')     # TRACK_APP_BUILDNOTE_R601: the page's foot line
        put(out, 'runtime.txt', rid + '\n')
        put(out, 'TRACK-README.md', (HERE / 'README.md').read_bytes())
        # R549 (Tobias 11-04: "QLC skal fjernes fra navne, logoer osv"): TRACK's logo for its window and shortcut
        put(out, 'track.png', (HERE / 'branding' / 'track.png').read_bytes())
        put(out, 'track.ico', (HERE / 'branding' / 'track.ico').read_bytes())
        # R548_FIXTURES: the rig's own definitions (not in QLC+'s library) - TRACK copies each one
        # into the user's QLC+ fixture folder at start, only when no definition of that model is there
        for qxf in sorted((HERE / 'fixtures').glob('*.qxf')):
            put(out, 'TRACK-fixtures/' + qxf.name, qxf.read_bytes())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', required=True)
    ap.add_argument('--rev', required=True)
    ap.add_argument('--jre', required=True)
    ap.add_argument('--blt', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--label', default='', help="the commit's subject - its R-number goes with the build")
    a = ap.parse_args()
    if not re.fullmatch(r'[0-9a-f]{6,40}', a.rev):
        raise SystemExit('odd rev: %r' % a.rev)
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    rid = runtime_id(a.jre, a.blt)
    rt = out / ('TRACK-runtime-%s.zip' % rid)
    build_runtime(a.jre, a.blt, rt)
    app = out / 'TRACK-app.zip'
    rnd = round_of(a.label)
    print('round: %s (from %r)' % (rnd or '-', a.label))     # TRACK_APP_BUILDNOTE_R601: in the CI log
    build_app(a.base, a.rev, rid, app, rnd)
    rsha, rsize = sha_size(rt)
    asha, asize = sha_size(app)
    manifest = {'rev': a.rev,
                'app': {'asset': app.name, 'size': asize, 'sha256': asha},
                'runtime': {'id': rid, 'asset': rt.name, 'size': rsize, 'sha256': rsha}}
    (out / 'track-app.json').write_text(json.dumps(manifest, indent=1) + '\n')
    (out / 'track-app-build.txt').write_text(a.rev + '\n')
    print(json.dumps(manifest, indent=1))


if __name__ == '__main__':
    main()
