#!/usr/bin/env python3
"""A sampling profiler for a running engine on Windows (x64), from the
standard library and dbghelp alone (docs/performance.md).

sample_profile.py <pid> [--hz 500] [--seconds 60] [--after 0] [--top 60]
                  [--out report.txt] [--lines NAME ...]

Suspends the process's busiest thread `hz` times a second, walks its stack
(StackWalk64) and resolves the frames from the PDB (SymFromAddr). Prints each
function's self samples (the frame it was in) and total samples (anywhere on
the stack), as % of all samples; with --lines, the samples in functions
naming NAME by source line. --out also writes each distinct stack
(<out>.stacks), for other questions later.

Needs a build with symbols: a Release build configured with /Zi and linked
with /DEBUG (docs/performance.md).
"""

import argparse
import collections
import ctypes
import ctypes.wintypes as wt
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
dbg = ctypes.WinDLL("dbghelp", use_last_error=True)

PROCESS_ALL = 0x1F0FFF
THREAD_ALL = 0x1F03FF
TH32CS_SNAPTHREAD = 0x4
CONTEXT_FULL = 0x10000B


class THREADENTRY32(ctypes.Structure):
    _fields_ = [
        ("dwSize", wt.DWORD),
        ("cntUsage", wt.DWORD),
        ("th32ThreadID", wt.DWORD),
        ("th32OwnerProcessID", wt.DWORD),
        ("tpBasePri", ctypes.c_long),
        ("tpDeltaPri", ctypes.c_long),
        ("dwFlags", wt.DWORD),
    ]


class IMAGEHLP_LINE64(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.DWORD),
        ("Key", ctypes.c_void_p),
        ("LineNumber", wt.DWORD),
        ("FileName", ctypes.c_char_p),
        ("Address", ctypes.c_uint64),
    ]


class SYMBOL_INFO(ctypes.Structure):
    _fields_ = [
        ("SizeOfStruct", wt.ULONG),
        ("TypeIndex", wt.ULONG),
        ("Reserved", ctypes.c_uint64 * 2),
        ("Index", wt.ULONG),
        ("Size", wt.ULONG),
        ("ModBase", ctypes.c_uint64),
        ("Flags", wt.ULONG),
        ("Value", ctypes.c_uint64),
        ("Address", ctypes.c_uint64),
        ("Register", wt.ULONG),
        ("Scope", wt.ULONG),
        ("Tag", wt.ULONG),
        ("NameLen", wt.ULONG),
        ("MaxNameLen", wt.ULONG),
        ("Name", ctypes.c_char * 1024),
    ]


k32.OpenProcess.restype = wt.HANDLE
k32.OpenThread.restype = wt.HANDLE
k32.CreateToolhelp32Snapshot.restype = wt.HANDLE
dbg.SymFunctionTableAccess64.restype = ctypes.c_void_p
dbg.SymGetModuleBase64.restype = ctypes.c_uint64
dbg.StackWalk64.argtypes = [
    wt.DWORD,
    wt.HANDLE,
    wt.HANDLE,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_void_p,
    ctypes.c_void_p,
]
dbg.SymFromAddr.argtypes = [
    wt.HANDLE,
    ctypes.c_uint64,
    ctypes.POINTER(ctypes.c_uint64),
    ctypes.POINTER(SYMBOL_INFO),
]
dbg.SymInitialize.argtypes = [wt.HANDLE, ctypes.c_char_p, wt.BOOL]
k32.GetThreadTimes.argtypes = [wt.HANDLE] + [ctypes.POINTER(wt.FILETIME)] * 4
k32.GetThreadContext.argtypes = [wt.HANDLE, ctypes.c_void_p]
k32.SuspendThread.argtypes = [wt.HANDLE]
k32.ResumeThread.argtypes = [wt.HANDLE]


def threads_of(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    te = THREADENTRY32()
    te.dwSize = ctypes.sizeof(te)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(te))
    while ok:
        if te.th32OwnerProcessID == pid:
            out.append(te.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(te))
    k32.CloseHandle(snap)
    return out


def cpu_time(h):
    c, e, k, u = (wt.FILETIME() for _ in range(4))
    k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k), ctypes.byref(u))
    return (k.dwHighDateTime << 32 | k.dwLowDateTime) + (u.dwHighDateTime << 32 | u.dwLowDateTime)


def busiest_thread(pid, window=1.0):
    handles = {t: k32.OpenThread(THREAD_ALL, False, t) for t in threads_of(pid)}
    before = {t: cpu_time(h) for t, h in handles.items() if h}
    time.sleep(window)
    after = {t: cpu_time(handles[t]) for t in before}
    tid = max(before, key=lambda t: after[t] - before[t])
    for t, h in handles.items():
        if h and t != tid:
            k32.CloseHandle(h)
    return tid, handles[tid]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pid", type=int)
    ap.add_argument("--hz", type=float, default=500)
    ap.add_argument("--seconds", type=float, default=60)
    ap.add_argument("--after", type=float, default=0, help="wait this long first")
    ap.add_argument("--top", type=int, default=60)
    ap.add_argument("--depth", type=int, default=64)
    ap.add_argument("--out")
    ap.add_argument(
        "--lines",
        action="append",
        default=[],
        help="also count self samples by source line in functions naming this",
    )
    a = ap.parse_args()

    time.sleep(a.after)
    hp = k32.OpenProcess(PROCESS_ALL, False, a.pid)
    if not hp:
        sys.exit(f"cannot open process {a.pid}: {ctypes.get_last_error()}")
    dbg.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES off
    if not dbg.SymInitialize(hp, None, True):
        sys.exit(f"SymInitialize failed: {ctypes.get_last_error()}")
    tid, ht = busiest_thread(a.pid)
    print(f"sampling thread {tid}", file=sys.stderr)

    ctx_buf = ctypes.create_string_buffer(1232 + 16)
    ctx_addr = (ctypes.addressof(ctx_buf) + 15) & ~15
    frame = ctypes.create_string_buffer(1024)
    fta = ctypes.cast(dbg.SymFunctionTableAccess64, ctypes.c_void_p)
    gmb = ctypes.cast(dbg.SymGetModuleBase64, ctypes.c_void_p)

    stacks = collections.Counter()
    period = 1.0 / a.hz
    end = time.perf_counter() + a.seconds
    taken = 0
    while time.perf_counter() < end:
        t0 = time.perf_counter()
        if k32.WaitForSingleObject(hp, 0) == 0:
            break  # the process has ended
        if k32.SuspendThread(ht) == 0xFFFFFFFF:
            break  # the thread (or process) has gone
        try:
            ctypes.memset(ctx_addr, 0, 1232)
            ctypes.c_uint32.from_address(ctx_addr + 0x30).value = CONTEXT_FULL
            if not k32.GetThreadContext(ht, ctx_addr):
                continue
            rip = ctypes.c_uint64.from_address(ctx_addr + 0xF8).value
            rsp = ctypes.c_uint64.from_address(ctx_addr + 0x98).value
            rbp = ctypes.c_uint64.from_address(ctx_addr + 0xA0).value
            ctypes.memset(frame, 0, 1024)
            for off, val in ((0, rip), (32, rbp), (48, rsp)):
                ctypes.c_uint64.from_buffer(frame, off).value = val
                ctypes.c_uint32.from_buffer(frame, off + 12).value = 3  # AddrModeFlat
            pcs = []
            while len(pcs) < a.depth:
                if not dbg.StackWalk64(
                    0x8664, hp, ht, ctypes.addressof(frame), ctx_addr, None, fta, gmb, None
                ):
                    break
                pc = ctypes.c_uint64.from_buffer(frame, 0).value
                if pc == 0:
                    break
                pcs.append(pc)
        finally:
            k32.ResumeThread(ht)
        if pcs:
            stacks[tuple(pcs)] += 1
            taken += 1
        rest = period - (time.perf_counter() - t0)
        if rest > 0:
            time.sleep(rest)

    names = {}
    sym = SYMBOL_INFO()
    sym.SizeOfStruct = 88
    sym.MaxNameLen = 1000
    disp = ctypes.c_uint64()

    def name(pc):
        if pc not in names:
            if dbg.SymFromAddr(hp, pc, ctypes.byref(disp), ctypes.byref(sym)):
                names[pc] = sym.Name.decode(errors="replace")
            else:
                names[pc] = f"0x{pc:x}"
        return names[pc]

    self_n = collections.Counter()
    total_n = collections.Counter()
    callers = collections.defaultdict(collections.Counter)
    for st, n in stacks.items():
        fns = [name(pc) for pc in st]
        self_n[fns[0]] += n
        for f in set(fns):
            total_n[f] += n
        for i in range(len(fns) - 1):
            callers[fns[i]][fns[i + 1]] += n
    lines = [
        f"{taken} samples of thread {tid} at {a.hz:g} Hz over {a.seconds:g} s",
        "",
        "== self ==",
        f"{'%':>6} {'n':>7}  function",
    ]
    for f, n in self_n.most_common(a.top):
        lines.append(f"{100 * n / taken:6.2f} {n:7}  {f}")
    lines += ["", "== total (anywhere on the stack) ==", f"{'%':>6} {'n':>7}  function"]
    for f, n in total_n.most_common(a.top * 2):
        lines.append(f"{100 * n / taken:6.2f} {n:7}  {f}")
    lines += ["", "== top self functions' callers =="]
    for f, _ in self_n.most_common(25):
        cs = ", ".join(f"{c} {100 * m / taken:.1f}%" for c, m in callers[f].most_common(4))
        lines.append(f"{f}: {cs}")
    if a.lines:
        dbg.SymGetLineFromAddr64.argtypes = [
            wt.HANDLE,
            ctypes.c_uint64,
            ctypes.POINTER(wt.DWORD),
            ctypes.POINTER(IMAGEHLP_LINE64),
        ]
        ln = IMAGEHLP_LINE64()
        ln.SizeOfStruct = ctypes.sizeof(IMAGEHLP_LINE64)
        d32 = wt.DWORD()
        by_line = collections.Counter()
        for st, n in stacks.items():
            # the first frame (from the leaf) inside a wanted function
            for i, pc in enumerate(st):
                if any(w in name(pc) for w in a.lines):
                    # a caller's frame holds its return address: the call is just before
                    at = pc if i == 0 else pc - 1
                    if dbg.SymGetLineFromAddr64(hp, at, ctypes.byref(d32), ctypes.byref(ln)):
                        f = ln.FileName.decode(errors="replace").replace("\\", "/").split("/")[-1]
                        by_line[f"{f}:{ln.LineNumber}  {name(pc)[:70]}"] += n
                    break
        lines += ["", "== by line (inclusive, first frame in " + ", ".join(a.lines) + ") =="]
        for k, n in by_line.most_common(80):
            lines.append(f"{100 * n / taken:6.2f} {n:7}  {k}")
    text = "\n".join(lines)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as fh:
            fh.write(text)
        # the raw stacks too, for asking other questions later
        with open(a.out + ".stacks", "w", encoding="utf-8") as fh:
            for st, n in stacks.most_common():
                fh.write(f"{n}\t" + ";".join(name(pc) for pc in reversed(st)) + "\n")
    print(text)


if __name__ == "__main__":
    main()
