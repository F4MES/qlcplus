#!/usr/bin/env python3
"""CI smoke test for TRACK's beat-link helper (track-app branch).

    python3 trackbridge/smoke_test.py <beat-link-trigger.jar> <trackbridge.jar>

Without any CDJ on the network it checks what can be checked anywhere:
  1. the helper starts on BLT's jar, loads qlc_shared_functions.clj (the copy in the
     jar) and connects to QLC+'s port 9998 - a stand-in server here - with "hello";
  2. a second helper sees the PC's beat-link lock and exits with code 3 (as it does
     while Beat Link Trigger runs);
  3. with --parent-pipe, closing its stdin (QLC+ quitting) stops it with code 0.
"""
import os
import socket
import subprocess
import sys
import tempfile
import threading
import time

BLT, HELPER = sys.argv[1], sys.argv[2]
HOME = tempfile.mkdtemp(prefix='trackbridge-home-')
JAVA = os.path.join(os.environ['JAVA_HOME'], 'bin', 'java') if os.environ.get('JAVA_HOME') else 'java'
CMD = [JAVA, '-Duser.home=' + HOME, '-cp', os.pathsep.join([BLT, HELPER]),
       'clojure.main', '-m', 'qlc.bridge']
received = []


def serve():
    srv = socket.socket()
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(('127.0.0.1', 9998))
    srv.listen(4)
    while True:
        conn, _ = srv.accept()
        threading.Thread(target=lambda c: [received.append(line) for line in c.makefile('r')],
                         args=(conn,), daemon=True).start()


threading.Thread(target=serve, daemon=True).start()
first = subprocess.Popen(CMD + ['--parent-pipe'], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
deadline = time.time() + 120
while time.time() < deadline and not any('"hello"' in r for r in received):
    time.sleep(0.5)
assert any('"hello"' in r for r in received), 'no hello from the helper on port 9998'
print('1. helper connected to QLC+ with hello')

second = subprocess.run(CMD, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=120)
assert second.returncode == 3, 'second helper: code %d, not 3' % second.returncode
print('2. second helper saw the lock: code 3')

first.stdin.close()
code = first.wait(timeout=60)
assert code == 0, 'helper after its pipe closed: code %d, not 0' % code
print('3. pipe closed: helper stopped with code 0')

log = open(os.path.join(HOME, '.qlc-rig-update', 'bridge.log'), encoding='utf-8', errors='replace').read()
for needle in ('Shared functions loaded', 'Looking for DJ Link devices', 'QLC+ closed our pipe'):
    assert needle in log, 'bridge.log lacks: ' + needle
print('bridge.log: shared functions loaded, looked for DJ Link devices, stopped by the pipe')
print('OK')
