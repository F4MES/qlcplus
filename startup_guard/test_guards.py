"""Process-level startup/crash/race tests. No lighting hardware or network used."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest

from compile_blt_guard import build

HERE = Path(__file__).resolve().parent
OPTIONS = None


def wait_for(path, process=None):
    deadline = time.monotonic() + 12
    while time.monotonic() < deadline:
        if path.exists():
            return
        if process and process.poll() is not None:
            raise AssertionError('Process exited before startup: ' + str(process.communicate()))
        time.sleep(.02)
    raise AssertionError('No startup marker: ' + str(path))


class GuardTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.work = tempfile.TemporaryDirectory(prefix='rig guard tests ')
        cls.root = Path(cls.work.name)
        cls.java_home = os.environ.get('JAVA_HOME')
        cls.jar = build(cls.root / 'guard.jar', cls.java_home)
        suffix = '.exe' if os.name == 'nt' else ''
        cls.java = str(Path(cls.java_home) / 'bin' / ('java' + suffix)) if cls.java_home else 'java'
        cls.javac = str(Path(cls.java_home) / 'bin' / ('javac' + suffix)) if cls.java_home else 'javac'
        cls.fixture = cls.root / 'BeatLinkTrigger.java'
        cls.fixture.write_text('''package beat_link_trigger;
import java.nio.file.*;
import java.nio.charset.StandardCharsets;
public class BeatLinkTrigger {
  public static void main(String[] args) throws Exception {
    Files.write(Paths.get(args[0]), String.join("|", args).getBytes(StandardCharsets.UTF_8));
    if (args.length > 2 && args[2].equals("worker")) {
      new Thread(() -> { try {
        while (!Files.exists(Paths.get(args[1]))) Thread.sleep(20);
      } catch (Exception e) { throw new RuntimeException(e); } }).start();
      return;
    }
    while (!Files.exists(Paths.get(args[1]))) Thread.sleep(20);
  }
}''', encoding='utf8')
        subprocess.run([cls.javac, '--release', '8', '-Xlint:-options', '-d', str(cls.root), str(cls.fixture)], check=True)
        cls.pwsh = OPTIONS.pwsh or shutil.which('pwsh') or (shutil.which('powershell') if os.name == 'nt' else None)
        if not cls.pwsh:
            raise RuntimeError('PowerShell required: use --pwsh PATH')

    @classmethod
    def tearDownClass(cls):
        cls.work.cleanup()

    def setUp(self):
        self.case = Path(tempfile.mkdtemp(dir=self.root))
        self.env = dict(os.environ, LOCALAPPDATA=str(self.case / 'profile'), ProgramData=str(self.case / 'shared'))
        self.processes = []

    def tearDown(self):
        for process in self.processes:
            if process.poll() is None:
                process.kill()
            process.communicate(timeout=10)

    def blt(self, marker, mode='normal', native=None):
        stop = self.case / 'stop'
        arguments = [str(marker), str(stop), mode, 'preserved argument with spaces', 'rød']
        command = [str(native)] if native else [self.java, '-Dqlc.blt.main=beat_link_trigger.BeatLinkTrigger',
                   '-cp', os.pathsep.join([str(self.jar), str(self.root)]), 'org.qlcplus.rig.BltGuardMain']
        process = subprocess.Popen(command + arguments, env=self.env, stdin=subprocess.PIPE,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.processes.append(process)
        return process

    def test_blt_duplicate_and_normal_restart_preserve_arguments(self):
        marker = self.case / 'first'
        first = self.blt(marker)
        wait_for(marker, first)
        self.assertIn('preserved argument with spaces|rød', marker.read_text(encoding='utf8'))
        for i in range(8):
            extra = self.case / ('extra' + str(i))
            process = self.blt(extra)
            _, err = process.communicate(timeout=10)
            self.assertEqual(0, process.returncode, err)
            self.assertFalse(extra.exists())
        self.assertIsNone(first.poll())
        (self.case / 'stop').touch()
        first.communicate(timeout=10)
        self.assertEqual(0, first.returncode)
        restarted = self.case / 'restart'
        process = self.blt(restarted)
        wait_for(restarted, process)

    def test_blt_keeps_lock_after_main_returns_and_recovers_after_kill(self):
        marker = self.case / 'worker'
        first = self.blt(marker, 'worker')
        wait_for(marker, first)
        extra = self.case / 'extra'
        second = self.blt(extra)
        second.communicate(timeout=10)
        self.assertFalse(extra.exists())
        first.kill()
        first.communicate(timeout=10)
        restarted = self.case / 'restart'
        process = self.blt(restarted)
        wait_for(restarted, process)

    def test_blt_simultaneous_starts_have_one_winner(self):
        markers = [self.case / ('race' + str(i)) for i in range(12)]
        processes = [self.blt(marker) for marker in markers]
        deadline = time.monotonic() + 15
        while time.monotonic() < deadline and sum(p.poll() is None for p in processes) > 1:
            time.sleep(.03)
        self.assertEqual(1, sum(p.poll() is None for p in processes))
        self.assertEqual(1, sum(marker.exists() for marker in markers))
        for process in processes:
            if process.poll() is not None:
                self.assertEqual(0, process.returncode, process.communicate())

    def test_blt_lock_failure_never_enters_application(self):
        (self.case / 'shared').write_text('not a directory')
        marker = self.case / 'must not start'
        process = self.blt(marker)
        _, err = process.communicate(timeout=10)
        self.assertEqual(1, process.returncode)
        self.assertIn(b'guard failed', err)
        self.assertFalse(marker.exists())

    def test_blt_shares_identity_between_user_profiles(self):
        first_marker = self.case / 'user-one'
        first = self.blt(first_marker)
        wait_for(first_marker, first)
        self.env['LOCALAPPDATA'] = str(self.case / 'different-profile')
        second_marker = self.case / 'user-two'
        second = self.blt(second_marker)
        second.communicate(timeout=10)
        self.assertEqual(0, second.returncode)
        self.assertFalse(second_marker.exists())
        self.assertIsNone(first.poll())

    def test_blt_missing_machine_lock_location_fails_closed(self):
        self.env = {key: value for key, value in self.env.items() if key.lower() != 'programdata'}
        marker = self.case / 'must not start'
        process = self.blt(marker)
        _, err = process.communicate(timeout=10)
        self.assertEqual(1, process.returncode)
        self.assertIn(b'ProgramData is missing', err)
        self.assertFalse(marker.exists())

    def installer(self):
        path = HERE.parent / 'tools/install-blt-guard.ps1'
        return path if path.exists() else HERE.parent / 'rig_update/install-blt-guard.ps1'

    def install(self, app):
        return subprocess.run([self.pwsh, '-NoProfile', '-File', str(self.installer()),
                               '-AppPath', str(app), '-JarPath', str(self.jar)],
                              capture_output=True, text=True, env=self.env)

    def fixture_app(self, newline='\r\n', main='beat_link_trigger.BeatLinkTrigger'):
        app = self.case / 'BLT with spaces'
        (app / 'app').mkdir(parents=True)
        (app / 'Beat Link Trigger.exe').touch()
        cfg = app / 'app/Beat Link Trigger.cfg'
        text = newline.join(['[Application]', 'app.classpath=$APPDIR\\beat-link-trigger.jar',
                             'app.mainclass=' + main, '', '[JavaOptions]',
                             'java-options=-Djava.net.preferIPv4Stack=true', '', '[ArgOptions]',
                             'arguments=--offline', ''])
        cfg.write_bytes(text.encode())
        return app, cfg, text

    def test_install_config_backup_idempotence_and_options(self):
        app, cfg, original = self.fixture_app()
        result = self.install(app)
        self.assertEqual(0, result.returncode, result.stderr)
        data = cfg.read_bytes()
        text = data.decode()
        self.assertIn('app.mainclass=org.qlcplus.rig.BltGuardMain', text)
        self.assertIn('app.classpath=$APPDIR\\beat-link-trigger.jar', text)
        self.assertIn('java-options=-Djava.net.preferIPv4Stack=true', text)
        self.assertIn('arguments=--offline', text)
        self.assertNotIn('\n', text.replace('\r\n', ''))
        backups = list(cfg.parent.glob('*.before-guard-*'))
        self.assertEqual(1, len(backups))
        self.assertEqual(original.encode(), backups[0].read_bytes())
        result = self.install(app)
        self.assertEqual(0, result.returncode, result.stderr)
        self.assertEqual(data, cfg.read_bytes())
        self.assertEqual(1, len(list(cfg.parent.glob('*.before-guard-*'))))

    def test_unknown_launcher_is_not_modified(self):
        app, cfg, original = self.fixture_app('\n', 'different.Application')
        result = self.install(app)
        self.assertNotEqual(0, result.returncode)
        self.assertEqual(original.encode(), cfg.read_bytes())
        self.assertFalse(list(cfg.parent.glob('qlc-blt-guard-*.jar')))

    def test_qlc_kernel_error_and_handle_lifetime(self):
        suffix = '.exe' if os.name == 'nt' else ''
        executables = list(Path(OPTIONS.probe_dir).resolve().rglob('guard_contract_test' + suffix))
        self.assertEqual(1, len(executables))
        subprocess.run([str(executables[0])], check=True)

    @unittest.skipUnless(os.name == 'nt', 'actual Windows kernel mutex required')
    def test_qlc_windows_race_normal_exit_and_crash(self):
        probes = list(Path(OPTIONS.probe_dir).resolve().rglob('guard_probe.exe'))
        self.assertEqual(1, len(probes))
        copied = self.case / 'other-install.exe'
        shutil.copyfile(probes[0], copied)
        def start(probe, marker):
            process = subprocess.Popen([str(probe), str(marker)], stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.processes.append(process)
            return process
        first_marker = self.case / 'qlc-first'
        first = start(probes[0], first_marker)
        wait_for(first_marker, first)
        duplicates = [start(copied, self.case / ('dup' + str(i))) for i in range(12)]
        for process in duplicates:
            process.communicate(timeout=10)
            self.assertEqual(0, process.returncode)
        self.assertFalse(list(self.case.glob('dup*')))
        self.assertIsNone(first.poll())
        first.communicate(b'quit\n', timeout=10)
        restarted = self.case / 'qlc-restart'
        second = start(copied, restarted)
        wait_for(restarted, second)
        second.kill()
        second.communicate(timeout=10)
        third_marker = self.case / 'qlc-after-crash'
        third = start(probes[0], third_marker)
        wait_for(third_marker, third)
        third.communicate(b'quit\n', timeout=10)
        race_markers = [self.case / ('qlc-race' + str(i)) for i in range(12)]
        racers = [start(copied if i % 2 else probes[0], marker) for i, marker in enumerate(race_markers)]
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and sum(p.poll() is None for p in racers) > 1:
            time.sleep(.02)
        self.assertEqual(1, sum(p.poll() is None for p in racers))
        self.assertEqual(1, sum(marker.exists() for marker in race_markers))

    @unittest.skipUnless(os.name == 'nt', 'actual Windows jpackage launcher required')
    def test_blt_real_windows_launcher(self):
        suffix = '.exe'
        bin_dir = Path(self.java).parent if Path(self.java).is_absolute() else Path(os.environ['JAVA_HOME']) / 'bin'
        fixture_input = self.case / 'input'
        fixture_input.mkdir()
        subprocess.run([str(bin_dir / ('jar' + suffix)), 'cf', str(fixture_input / 'fixture.jar'),
                        '-C', str(self.root), 'beat_link_trigger'], check=True)
        runtime = self.case / 'runtime'
        subprocess.run([str(bin_dir / ('jlink' + suffix)), '--add-modules', 'java.base',
                        '--output', str(runtime)], check=True)
        subprocess.run([str(bin_dir / ('jpackage' + suffix)), '--type', 'app-image',
                        '--name', 'Beat Link Trigger', '--input', str(fixture_input),
                        '--main-jar', 'fixture.jar', '--main-class', 'beat_link_trigger.BeatLinkTrigger',
                        '--runtime-image', str(runtime), '--dest', str(self.case)], check=True)
        app = self.case / 'Beat Link Trigger'
        result = self.install(app)
        self.assertEqual(0, result.returncode, result.stderr)
        native = app / 'Beat Link Trigger.exe'
        first_marker = self.case / 'native-first'
        first = self.blt(first_marker, native=native)
        wait_for(first_marker, first)
        second_marker = self.case / 'native-second'
        second = self.blt(second_marker, native=native)
        second.communicate(timeout=10)
        self.assertEqual(0, second.returncode)
        self.assertFalse(second_marker.exists())
        self.assertIsNone(first.poll())
        (self.case / 'stop').touch()
        first.communicate(timeout=10)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--probe-dir', default='guard-build')
    parser.add_argument('--pwsh')
    OPTIONS = parser.parse_args()
    unittest.main(argv=[sys.argv[0]], verbosity=2)
