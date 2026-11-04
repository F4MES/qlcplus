#!/usr/bin/env python3
"""Builds trackbridge.jar: TRACK's beat-link helper (track-app branch).

The jar holds Clojure SOURCE only - qlc/bridge.clj and a fallback copy of
qlc_shared_functions.clj. Nothing is compiled: it runs on Beat Link Trigger's
installed jar (Clojure, beat-link, cheshire, timbre) and Java:

    javaw -cp "<BLT jar>;trackbridge.jar" clojure.main -m qlc.bridge --parent-pipe

    python3 trackbridge/make_jar.py out/trackbridge.jar
"""
import sys
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
FILES = [
    (HERE / 'src' / 'qlc' / 'bridge.clj', 'qlc/bridge.clj'),
    (HERE / 'qlc_shared_functions.clj', 'qlc_shared_functions.clj'),
]
MANIFEST = 'Manifest-Version: 1.0\r\nCreated-By: trackbridge/make_jar.py\r\n\r\n'


def build(target):
    target = Path(target).resolve()
    target.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(target, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('META-INF/MANIFEST.MF', MANIFEST)
        for src, name in FILES:
            z.write(src, name)
    return target


if __name__ == '__main__':
    print(build(sys.argv[1] if len(sys.argv) > 1 else 'trackbridge.jar'))
