#!/usr/bin/env python3
"""tools/gen.py on several cores: the same output, file for file, in a fraction of the time on a handheld.

gen.py lifts every instruction of a module twice in one process - finding the functions (Module.scan, until the
boundaries stop changing) and writing their C (Module.emit_function) - and main alone is ~11 million instructions:
on the AYN Thor that is most of an hour of one core while the others wait for C to compile. Both passes are per
function, so here they run in worker processes (forked before anything is lifted: each lifts only its own share,
so memory is not multiplied), and this process only merges: the rounds of boundaries in the same order, the
functions' C in address order, cut into the same files by the same rules. Same command line as gen.py, plus
--jobs N (default: the CPU count). The Android app's game code builder runs it (runtime/gamebuild.c).

The output is gen.py's, so the game code id (GAMELIB_ID, over gen.py and the headers) is unchanged by using it.
"""
import argparse
import gc
import os
import pickle
import platform
import re
import sys
import time

_here = os.path.dirname(os.path.abspath(__file__))
sys.path[:0] = [_here, os.path.join(_here, '..')]  # (gen.py next to this one on the device, tools/ in the repo)
import gen  # noqa: E402
from gen import Module, cut_after, fname, hx, sanitize, sext26, write_if_changed  # noqa: E402

M = None    # the module, set before the workers fork (they inherit it, its lift cache still empty)
HLE = None  # address -> host function name (sdk)


def _scan(keys):
    out = []
    for a, end in keys:
        out.append(((a, end), M.scan(a, end)))
    M.cache.clear()  # (each batch lifts its own share: no copy of everything in every worker)
    return out


def _emit(batch):
    out = []
    for a, end, labels, tables in batch:
        lines = M.emit_function(a, end, labels, tables, HLE)
        out.append((a, lines, getattr(M, '_refs', set())))
        M._refs = set()
    M.cache.clear()
    return out


def batches(items, size):
    return [items[i:i + size] for i in range(0, len(items), size)]


def parallel(fn, tasks, jobs, workdir):
    """fn(task) for every task in forked workers (task i in worker i % jobs), the results in task order as they
    are ready. multiprocessing.Pool can't be used here: it needs POSIX named semaphores, which Android's libc
    lacks. Each result goes through a file (renamed into place when complete)."""
    os.makedirs(workdir, exist_ok=True)
    jobs = max(1, min(jobs, len(tasks)))
    gc.collect()
    gc.freeze()  # (the workers' garbage collector then leaves this process's objects alone: no copy of each page)
    pids = []
    for j in range(jobs):
        pid = os.fork()
        if pid == 0:
            code = 0
            try:
                for i in range(j, len(tasks), jobs):
                    r = fn(tasks[i])
                    tmp = os.path.join(workdir, f'{i}.tmp')
                    with open(tmp, 'wb') as f:
                        pickle.dump(r, f, protocol=pickle.HIGHEST_PROTOCOL)
                    os.rename(tmp, os.path.join(workdir, f'{i}.res'))
            except BaseException:
                import traceback
                traceback.print_exc()
                code = 1
            sys.stderr.flush()
            os._exit(code)
        pids.append(pid)
    gc.unfreeze()
    alive = set(pids)
    try:
        for i in range(len(tasks)):
            path = os.path.join(workdir, f'{i}.res')
            while not os.path.exists(path):
                for pid in list(alive):  # (a worker that failed: stop instead of waiting for its results)
                    done, st = os.waitpid(pid, os.WNOHANG)
                    if done:
                        alive.discard(pid)
                        if os.waitstatus_to_exitcode(st) != 0:
                            raise SystemExit(f'genpar: a worker failed (exit {os.waitstatus_to_exitcode(st)})')
                if not os.path.exists(path):
                    if not alive and not os.path.exists(path):
                        raise SystemExit(f'genpar: no result for batch {i}')
                    time.sleep(0.02)
            with open(path, 'rb') as f:
                r = pickle.load(f)
            os.remove(path)
            yield r
    finally:
        for pid in alive:
            try:
                os.kill(pid, 9)
            except OSError:
                pass
        for pid in pids:
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass


def main():
    global M, HLE
    ap = argparse.ArgumentParser()
    ap.add_argument('name')
    ap.add_argument('nso')
    ap.add_argument('base', type=lambda s: int(s, 0))
    ap.add_argument('outdir')
    ap.add_argument('--hle')
    ap.add_argument('--chunk', type=int, default=1500)
    ap.add_argument('--chunk-bytes', type=int, default=1500000)
    ap.add_argument('--big-fn', type=int, default=100000)
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--trace-stores', action='store_true')
    ap.add_argument('--simd', choices=('asm', 'c'),
                    default='asm' if platform.machine().lower() in ('arm64', 'aarch64') else 'c')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 1)
    args = ap.parse_args()

    t0 = time.time()
    m = Module(args.name, args.nso, args.base, args.simd, args.trace_stores)
    bid = m.nso.build_id[:16]
    if args.name == 'main' and bid != gen.EEVEE_100_BUILD:  # (as gen.py: the instruction patches of other builds)
        moved = {gen.BUILD_ADDRS[bid][a]: fn for a, fn in gen.PATCHES.items()} if bid in gen.BUILD_ADDRS else {}
        print(f'{args.name}: build {bid}, ' + (f'{len(moved)} instruction patches moved' if moved else 'no instruction patches'))
        gen.PATCHES.clear()
        gen.PATCHES.update(moved)
        gen.RANDO_CODE.clear()
    starts = m.boundaries()
    hle = {}
    if args.hle and os.path.exists(args.hle):
        want = {l.strip() for l in open(args.hle) if l.strip() and not l.startswith('#')}
        for name, info, shndx, value, size in m.nso.syms:
            if shndx and name in want:
                hle[args.base + value] = name

    start_set = set(starts)
    for i, w in enumerate(m.words):
        if (w & 0xFC000000) == 0x94000000:
            t = m.lo + i * 4 + sext26(w) * 4
            if m.in_text(t):
                start_set.add(t)
    adrp_regs = {}
    for i, w in enumerate(m.words):
        pc = m.lo + i * 4
        if (w & 0x9F000000) == 0x10000000:  # ADR
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            if m.in_text(pc + imm):
                start_set.add(pc + imm)
        elif (w & 0x9F000000) == 0x90000000:  # ADRP
            imm = ((w >> 5) & 0x7FFFF) << 2 | ((w >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            adrp_regs[w & 31] = ((pc & ~0xFFF) + (imm << 12), i)
        elif (w & 0xFFC00000) == 0x91000000:  # ADD (imm, 64-bit, no shift)
            rn, rd = (w >> 5) & 31, w & 31
            if rn in adrp_regs and i - adrp_regs[rn][1] <= 8:
                t = adrp_regs[rn][0] + ((w >> 10) & 0xFFF)
                if m.in_text(t):
                    start_set.add(t)

    M, HLE = m, hle
    jobs = max(1, args.jobs)
    pardir = os.path.join(args.outdir, '.genpar')
    os.makedirs(args.outdir, exist_ok=True)
    if True:
        # 1. the functions: gen.py's rounds, each one's new (start, end) ranges scanned in parallel
        scanned = {}
        rounds = 0
        while True:
            rounds += 1
            order = sorted(start_set)
            todo = []
            for i, a in enumerate(order):
                end = order[i + 1] if i + 1 < len(order) else m.hi
                if (a, end) not in scanned:
                    todo.append((a, end))
            size = max(16, len(todo) // (jobs * 8) or 1)
            for res in parallel(_scan, batches(todo, size), jobs, pardir):
                for key, r in res:
                    scanned[key] = r
            new = set()
            for i, a in enumerate(order):
                end = order[i + 1] if i + 1 < len(order) else m.hi
                new |= scanned[(a, end)][1] - start_set
            if not new:
                break
            start_set |= new
        entries = {}
        for i, a in enumerate(order):
            end = order[i + 1] if i + 1 < len(order) else m.hi
            labels, ext, tables = scanned[(a, end)]
            entries[a] = (end, labels, tables)
        scanned = None
        print(f'{args.name}: {len(starts)} boundaries, {len(entries)} functions, {rounds} rounds, {jobs} jobs, '
              f'{time.time() - t0:.0f}s', file=sys.stderr, flush=True)

        # 2. the C: written by the workers in address order (imap keeps it), cut into files here as gen.py does
        os.makedirs(args.outdir, exist_ok=True)
        order = sorted(entries)
        if args.limit:
            order = order[:args.limit]
        work = [(a,) + entries[a] for a in order]
        emitted = (f for res in parallel(_emit, batches(work, 64), jobs, pardir) for f in res)
        nchunks = 0
        ci = 0
        pending = None
        sc = m.lifter.sc  # (simd.h in every file, as gen.py: decided by the mode, not by what was lifted)
        while ci < len(order) or pending:
            body, refs, size, fns = [], set(), 0, []
            while (pending or ci < len(order)) and len(fns) < args.chunk and size < 2 * args.chunk_bytes:
                if pending:
                    a, lines, r = pending
                    pending = None
                else:
                    a, lines, r = next(emitted)
                    ci += 1
                n = sum(len(l) + 1 for l in lines)
                if n > args.big_fn and fns:
                    pending = (a, lines, r)
                    break
                body += lines
                size += n
                fns.append(a)
                refs |= r
                if n > args.big_fn:
                    break
                if size >= args.chunk_bytes * 2 // 3 and cut_after(a - args.base):
                    break
            path = os.path.join(args.outdir, f'{args.name}_{nchunks:03d}.c')
            text = ['#include "cpu.h"\n' + ('#include "simd.h"\n' if sc else '')]
            text += [f'void {fname(r)}(Cpu *c);\n' for r in sorted(refs)]
            text += [f'void {sanitize(hle[a])}(Cpu *c);\n' for a in sorted(set(hle) & set(fns))]
            text += ['\n'.join(body), '\n']
            write_if_changed(path, ''.join(text))
            nchunks += 1
            print(f'{args.name}: progress {ci}/{len(order)} files {nchunks}', file=sys.stderr, flush=True)
        emitted.close()  # (the workers: waited for)
    try:
        os.rmdir(pardir)
    except OSError:
        pass
    for fn in os.listdir(args.outdir):
        mm = re.fullmatch(re.escape(args.name) + r'_(\d{3})\.c', fn)
        if mm and int(mm.group(1)) >= nchunks:
            os.remove(os.path.join(args.outdir, fn))
    import io
    with io.StringIO() as f:
        f.write('#include "cpu.h"\n')
        for a in order:
            f.write(f'void {fname(a)}(Cpu *c);\n')
        f.write(f'const uint64_t {args.name}_fn_addrs[] = {{\n')
        f.write(',\n'.join(hx(a) for a in order))
        f.write('};\n')
        f.write(f'const GuestFn {args.name}_fn_ptrs[] = {{\n')
        f.write(',\n'.join(fname(a) for a in order))
        f.write('};\n')
        f.write(f'const uint64_t {args.name}_fn_count = {len(order)};\n')
        write_if_changed(os.path.join(args.outdir, f'{args.name}_table.c'), f.getvalue())
    print(f'{args.name}: wrote {nchunks} chunks, {time.time() - t0:.0f}s total', file=sys.stderr)


if __name__ == '__main__':
    main()
