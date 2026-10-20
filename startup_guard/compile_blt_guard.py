"""Build a dependency-free Java 8-compatible bootstrap; no BLT jar is modified."""
from pathlib import Path
import argparse
import os
import subprocess
import tempfile


def build(output, java_home=None):
    here = Path(__file__).resolve().parent
    suffix = '.exe' if os.name == 'nt' else ''
    def tool(name):
        return str(Path(java_home) / 'bin' / (name + suffix)) if java_home else name
    with tempfile.TemporaryDirectory(prefix='blt-guard-build-') as work:
        subprocess.run([tool('javac'), '--release', '8', '-Xlint:all,-options', '-Werror',
                        '-d', work, str(here / 'BltGuardMain.java')], check=True)
        target = Path(output).resolve()
        target.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run([tool('jar'), 'cf', str(target), '-C', work, 'org'], check=True)
    return target


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output')
    parser.add_argument('--java-home', default=os.environ.get('JAVA_HOME'))
    options = parser.parse_args()
    print(build(options.output, options.java_home))
