#!/usr/bin/env python3
"""TRACK_APP_LOADGUARD_R600: can Windows start what is in this folder?

    python3 trackbridge/check_imports.py C:/qlcplus

Windows refuses to start a program when one of the DLLs beside it does not have a function the
program asks for (exit code 0xC0000139, "entry point not found") - and it only shows on a PC
that starts the program, never while building. That is how every TRACK build from R588 to R599
stopped at its first start on the lys-pc: the build was compiled with one C++ runtime and
shipped with Qt's older libstdc++-6.dll, and one new line of code asked for a name only the
newer runtime has.

This reads every .exe and .dll in the folder and checks each function they import from a DLL
that is also in the folder against what that DLL really exports. Any name that is missing is
listed and the exit code is 1. DLLs that are not in the folder (Windows' own) are not checked.
Plain Python, nothing to install.
"""
import struct
import sys
from pathlib import Path


class PE:
    def __init__(self, path):
        self.path = path
        self.d = Path(path).read_bytes()
        d = self.d
        if d[:2] != b'MZ':
            raise ValueError('not a PE file')
        pe = struct.unpack_from('<I', d, 0x3C)[0]
        if d[pe:pe + 4] != b'PE\0\0':
            raise ValueError('not a PE file')
        nsec, optsize = struct.unpack_from('<H', d, pe + 6)[0], struct.unpack_from('<H', d, pe + 20)[0]
        opt = pe + 24
        magic = struct.unpack_from('<H', d, opt)[0]
        if magic not in (0x10b, 0x20b):
            raise ValueError('unknown optional header')
        self.wide = magic == 0x20b
        ndir_at = opt + (108 if self.wide else 92)
        ndir = struct.unpack_from('<I', d, ndir_at)[0]
        self.dirs = [struct.unpack_from('<II', d, ndir_at + 4 + 8 * i) for i in range(min(ndir, 16))]
        self.secs = []
        at = opt + optsize
        for i in range(nsec):
            vsize, va, rsize, rptr = struct.unpack_from('<IIII', d, at + 40 * i + 8)
            self.secs.append((va, max(vsize, rsize), rptr))

    def off(self, rva):
        for va, size, rptr in self.secs:
            if va <= rva < va + size:
                return rva - va + rptr
        raise ValueError('address %#x is in no section' % rva)

    def text(self, rva):
        a = self.off(rva)
        return self.d[a:self.d.index(b'\0', a)].decode('latin-1')

    def exports(self):
        if len(self.dirs) < 1 or self.dirs[0][0] == 0:
            return set()
        a = self.off(self.dirs[0][0])
        count, names = struct.unpack_from('<I', self.d, a + 24)[0], struct.unpack_from('<I', self.d, a + 32)[0]
        if count == 0:
            return set()
        n = self.off(names)
        return {self.text(struct.unpack_from('<I', self.d, n + 4 * i)[0]) for i in range(count)}

    def imports(self):
        """{dll name: [function names]} - imports by number are left out"""
        out = {}
        if len(self.dirs) < 2 or self.dirs[1][0] == 0:
            return out
        a = self.off(self.dirs[1][0])
        size, fmt, high = (8, '<Q', 1 << 63) if self.wide else (4, '<I', 1 << 31)
        while True:
            lookup, _, _, name, first = struct.unpack_from('<IIIII', self.d, a)
            if name == 0 and first == 0:
                break
            a += 20
            funcs = out.setdefault(self.text(name), [])
            t = self.off(lookup or first)
            while True:
                v = struct.unpack_from(fmt, self.d, t)[0]
                t += size
                if v == 0:
                    break
                if v & high:
                    continue
                funcs.append(self.text((v & 0x7fffffff) + 2))
        return out


def main(folder):
    root = Path(folder)
    files = sorted(p for p in root.rglob('*') if p.is_file() and p.suffix.lower() in ('.exe', '.dll'))
    if not files:
        print('check_imports: no .exe or .dll in %s' % root)
        return 1
    by_name = {}
    for p in files:
        if p.suffix.lower() == '.dll':
            by_name.setdefault(p.name.lower(), []).append(p)
    exports = {}
    missing, checked = [], 0
    for p in files:
        try:
            imports = PE(p).imports()
            for dll in imports:
                for q in by_name.get(dll.lower(), []):
                    if q not in exports:
                        exports[q] = PE(q).exports()
        except (ValueError, struct.error, IndexError) as e:
            print('check_imports: %s or a DLL it uses cannot be read (%s)' % (p.relative_to(root), e))
            return 1
        for dll, funcs in sorted(imports.items()):
            found = by_name.get(dll.lower())
            if not found:
                continue                                    # Windows' own - not ours to check
            # the one Windows takes: beside the program (the folder itself) first, then beside the importer
            pick = sorted(found, key=lambda q: (q.parent != root, q.parent != p.parent, str(q)))[0]
            for f in funcs:
                checked += 1
                if f not in exports[pick]:
                    missing.append((str(p.relative_to(root)), str(pick.relative_to(root)), f))
    print('check_imports: %d files, %d imports checked against the DLLs in the folder' % (len(files), checked))
    if missing:
        print('check_imports: %d imports that the shipped DLL does NOT have - Windows will not start this:' % len(missing))
        for who, dll, f in missing:
            print('  %s wants %s from %s' % (who, f, dll))
        return 1
    print('check_imports: every one is there')
    return 0


if __name__ == '__main__':
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
