"""Read a running Sky Recomp's game memory from outside (Windows): nothing in the game changes, nothing to press.

The recompiled game keeps the guest's memory at its own addresses (main at 0x7100004000 - runtime/rt.h), so
ReadProcessMemory on skyrecomp.exe reads game state directly while it runs.

  python tools/skypeek.py state                  what the VR rig reads: stage, Link, eye point, camera, masks
  python tools/skypeek.py read main+0x1829a30 12 --f32      (or --u32 / --u64 / hex bytes by default)
  python tools/skypeek.py watch main+0x1829a30 12 --f32 [--every 0.5] [--for 20]    print when it changes
  python tools/skypeek.py snap <file> main+0x1829800 0x800  save a block (diff two snapshots with `diff`)
  python tools/skypeek.py diff <file a> <file b> [--f32]
Addresses: hex, or main+<hex offset>; a leading * dereferences (e.g. *main+0x2bf9940+0x144).
"""
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

MAIN = 0x7100004000
k32 = ctypes.WinDLL('kernel32', use_last_error=True)
k32.OpenProcess.restype = wt.HANDLE
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.c_void_p, ctypes.c_size_t, ctypes.POINTER(ctypes.c_size_t)]


def find_pid(name='skyrecomp.exe'):
    class PE(ctypes.Structure):
        _fields_ = [('dwSize', wt.DWORD), ('cntUsage', wt.DWORD), ('th32ProcessID', wt.DWORD),
                    ('th32DefaultHeapID', ctypes.c_size_t), ('th32ModuleID', wt.DWORD), ('cntThreads', wt.DWORD),
                    ('th32ParentProcessID', wt.DWORD), ('pcPriClassBase', wt.LONG), ('dwFlags', wt.DWORD),
                    ('szExeFile', ctypes.c_char * 260)]
    k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
    snap = k32.CreateToolhelp32Snapshot(2, 0)
    pe = PE()
    pe.dwSize = ctypes.sizeof(PE)
    pids = []
    ok = k32.Process32First(wt.HANDLE(snap), ctypes.byref(pe))
    while ok:
        if pe.szExeFile.decode(errors='ignore').lower() == name:
            pids.append((pe.cntThreads, pe.th32ProcessID))
        ok = k32.Process32Next(wt.HANDLE(snap), ctypes.byref(pe))
    k32.CloseHandle(wt.HANDLE(snap))
    return max(pids)[1] if pids else None  # (the game re-execs itself at start: the one with the most threads)


class Game:
    def __init__(self):
        pid = find_pid()
        if not pid:
            sys.exit('skyrecomp.exe is not running')
        self.h = k32.OpenProcess(0x0410, False, pid)  # PROCESS_VM_READ | PROCESS_QUERY_INFORMATION
        if not self.h:
            sys.exit(f'cannot open process {pid} (error {ctypes.get_last_error()})')
        self.pid = pid

    def read(self, addr, n):
        buf = ctypes.create_string_buffer(n)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(wt.HANDLE(self.h), addr, buf, n, ctypes.byref(got)) or got.value != n:
            return None
        return buf.raw

    def u64(self, a):
        b = self.read(a, 8)
        return struct.unpack('<Q', b)[0] if b else 0

    def u32(self, a):
        b = self.read(a, 4)
        return struct.unpack('<I', b)[0] if b else 0

    def f32s(self, a, n):
        b = self.read(a, 4 * n)
        return struct.unpack(f'<{n}f', b) if b else None

    def addr(self, s):
        deref = s.startswith('*')
        parts = s.lstrip('*').split('+')
        a = 0
        for i, p in enumerate(parts):
            a += MAIN if p == 'main' else int(p, 16)
            if deref and i == (1 if parts[0] == 'main' else 0):
                a = self.u64(a)
        return a


def fmt(b, mode):
    if b is None:
        return '(unreadable)'
    if mode == 'f32':
        return ' '.join(f'{v:.3f}' for v in struct.unpack(f'<{len(b) // 4}f', b[:len(b) // 4 * 4]))
    if mode == 'u32':
        return ' '.join(f'{v:08x}' for v in struct.unpack(f'<{len(b) // 4}I', b[:len(b) // 4 * 4]))
    if mode == 'u64':
        return ' '.join(f'{v:x}' for v in struct.unpack(f'<{len(b) // 8}Q', b[:len(b) // 8 * 8]))
    return b.hex(' ')


def ptr_ok(p):
    return MAIN < p < 0x7800000000 and not p & 3


def state(g):
    name = (g.read(MAIN + 0x2bf98d8, 16) or b'').split(b'\0')[0].decode(errors='replace')
    b = g.read(MAIN + 0x2bf98f0, 16)
    print(f'stage {name!r}  bytes main+0x2bf98f0: {b.hex(" ") if b else "?"}')
    print(f'masks: execute {g.u32(MAIN + 0x1827370):#x} draw {g.u32(MAIN + 0x1827374):#x} pending draw {g.u32(MAIN + 0x1827378):#x}'
          f'   item aim {g.u32(MAIN + 0x1939760)}')
    ref = g.u64(MAIN + 0x18299d0)
    eye = g.f32s(MAIN + 0x1829a30, 3)
    print(f'block main+0x18299d0: actor ref {ref:#x}  eye point (main+0x1829a30) {eye}')
    refs = [g.u64(MAIN + 0x18299d0 + 8 * i) for i in range(-2, 8)]
    print('  pointers main+0x18299c0..: ' + ' '.join(f'{r:x}' for r in refs))
    if ptr_ok(ref):
        print(f'  ref object: vtable {g.u64(ref):#x}  +0x148.. {g.f32s(ref + 0x148, 4)}  action(+0x118) {g.u32(ref + 0x118):#x}')
    p = g.u64(MAIN + 0x2bf9940)
    print(f'player *(main+0x2bf9940) = {p:#x}', end='')
    if ptr_ok(p):
        pos = g.f32s(p + 0x144, 3)
        print(f'  vtable main+{g.u64(p) - MAIN:#x}  position {pos}  -> eye would be ({pos[0]:.1f}, {pos[1] + 150:.1f}, {pos[2]:.1f})')
        if eye and pos:
            d = ((eye[0] - pos[0]) ** 2 + (eye[2] - pos[2]) ** 2) ** 0.5
            print(f'  eye point vs player: {d:.1f} units apart (level), eye height above his floor {eye[1] - pos[1]:.1f}')
    else:
        print()
    cam = g.f32s(MAIN + 0x2bfda90, 12)
    if cam:
        c = [-(cam[i] * cam[3] + cam[4 + i] * cam[7] + cam[8 + i] * cam[11]) for i in range(3)]
        print(f'game camera (main+0x2bfda90) position ({c[0]:.1f}, {c[1]:.1f}, {c[2]:.1f})', end='')
        if ptr_ok(p):
            pos = g.f32s(p + 0x144, 3)
            print(f'  {sum((c[i] - (pos[i] + (150 if i == 1 else 0))) ** 2 for i in range(3)) ** 0.5:.0f} units from the player\'s eye', end='')
        if eye:
            print(f', {sum((c[i] - eye[i]) ** 2 for i in range(3)) ** 0.5:.0f} from the eye point', end='')
        print()


def monitor(out, every):
    """Waits for a game, then writes a line whenever what the VR rig depends on changes (stage, Link's pointers, whether
    the eye point agrees with Link's position, the pause masks) - a history to read after something went wrong."""
    f = open(out, 'a', buffering=1)
    last, g = None, None
    while True:
        if g is None:
            if not find_pid():
                time.sleep(2)
                continue
            g = Game()
            f.write(f'{time.strftime("%H:%M:%S")} attached to pid {g.pid}\n')
        name = g.read(MAIN + 0x2bf98d8, 8)
        if name is None:  # (the game closed)
            f.write(f'{time.strftime("%H:%M:%S")} game gone\n')
            g, last = None, None
            continue
        stage = name.split(b'\0')[0].decode(errors='replace')
        layer = (g.read(MAIN + 0x2bf98fb, 2) or b'??').hex()
        ref, p = g.u64(MAIN + 0x18299d0), g.u64(MAIN + 0x2bf9940)
        eye, pos = g.f32s(MAIN + 0x1829a30, 3), g.f32s(p + 0x144, 3) if ptr_ok(p) else None
        cam = g.f32s(MAIN + 0x2bfda90, 12)
        apart = ((eye[0] - pos[0]) ** 2 + (eye[2] - pos[2]) ** 2) ** 0.5 if eye and pos else -1
        camd = -1
        if cam and pos:
            c = [-(cam[i] * cam[3] + cam[4 + i] * cam[7] + cam[8 + i] * cam[11]) for i in range(3)]
            camd = sum((c[i] - (pos[i] + (150 if i == 1 else 0))) ** 2 for i in range(3)) ** 0.5
        key = (stage, layer, ref, p, g.u64(p) if ptr_ok(p) else 0, eye == (0.0, 0.0, 0.0) if eye else None,
               'agree' if 0 <= apart < 5 else 'near' if 0 <= apart < 300 else 'far', camd > 1000,
               g.u32(MAIN + 0x1827370), g.u32(MAIN + 0x1827374))
        if key != last:
            f.write(f'{time.strftime("%H:%M:%S")} stage {stage} {layer} ref {ref:x} player {p:x} vt main+{(key[4] - MAIN) if key[4] else 0:x} '
                    f'eye {tuple(round(v, 1) for v in eye) if eye else None} player pos {tuple(round(v, 1) for v in pos) if pos else None} '
                    f'eye-player {apart:.0f} ({key[6]}) camera-player {camd:.0f} masks exec {key[8]:#x} draw {key[9]:#x}\n')
            last = key
        time.sleep(every)


def main():
    a = sys.argv[1:]
    if a and a[0] == 'monitor':
        monitor(a[1], float(a[a.index('--every') + 1]) if '--every' in a else 0.5)
        return
    if not a:
        sys.exit(__doc__)
    mode = next((m for m in ('f32', 'u32', 'u64') if '--' + m in a), 'hex')
    opt = lambda k, d: float(a[a.index(k) + 1]) if k in a else d
    g = Game()
    if a[0] == 'state':
        state(g)
    elif a[0] == 'read':
        at, n = g.addr(a[1]), int(a[2], 0)
        print(f'{at:#x}: {fmt(g.read(at, n), mode)}')
    elif a[0] == 'watch':
        at, n, last, end = g.addr(a[1]), int(a[2], 0), None, time.time() + opt('--for', 30)
        while time.time() < end:
            b = g.read(g.addr(a[1]), n)
            if b != last:
                print(f'{time.strftime("%H:%M:%S")} {at:#x}: {fmt(b, mode)}', flush=True)
                last = b
            time.sleep(opt('--every', 0.25))
    elif a[0] == 'snap':
        at, n = g.addr(a[2]), int(a[3], 0)
        b = g.read(at, n)
        if b is None:
            sys.exit('unreadable')
        open(a[1], 'wb').write(struct.pack('<QQ', at, n) + b)
        print(f'{n:#x} bytes from {at:#x} -> {a[1]}')
    elif a[0] == 'diff':
        x, y = open(a[1], 'rb').read(), open(a[2], 'rb').read()
        at = struct.unpack('<Q', x[:8])[0]
        x, y = x[16:], y[16:]
        for o in range(0, min(len(x), len(y)) - 3, 4):
            if x[o:o + 4] != y[o:o + 4]:
                u, v = struct.unpack('<I', x[o:o + 4])[0], struct.unpack('<I', y[o:o + 4])[0]
                fu, fv = struct.unpack('<f', x[o:o + 4])[0], struct.unpack('<f', y[o:o + 4])[0]
                print(f'+{o:#06x} ({at + o:#x}): {u:08x} -> {v:08x}' + (f'   {fu:.3f} -> {fv:.3f}' if mode == 'f32' else ''))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main()
