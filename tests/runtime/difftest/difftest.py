"""difftest.py: differential test of the lifter (tools/lifter.py, portable C lowering) against Unicorn (AArch64).

  python tests/runtime/difftest/difftest.py [--module main] [--per-shape 3] [--states 8] [--max-cases 4000]
                                            [--seed 1] [--only HEXWORD]

Real instruction words are sampled from the game's module (extracted-132/exefs/<module>), a few per "shape" (the
opcode bits that select the instruction and its form), keeping data processing and FP / SIMD instructions that the
lifter turns into straight-line C (no loads / stores, branches or system instructions in this first version). Each one
runs from the same random machine states twice: in Unicorn (reference) and in the C the lifter emits, compiled
with the runtime's headers (runtime/cpu.h, simd.h) as the game code is. X0-X30, SP, NZCV and V0-V31 must match.
FPCR is 0 (round to nearest, no flush-to-zero) in both; FPSR is not compared. Nothing of the game is written outside
build/difftest/ (ignored by git). Unicorn and Capstone are test-only Python packages (pip), never linked into the port.

Technique: OpenSmash / ssbu-recomp tools/lifter/difftest.py and mk8-recomp tests/differential_* (see
docs/engineering/recomp-project-comparison.md); this file is an independent implementation.
"""
import argparse
import os
import random
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))))
sys.path.insert(0, os.path.join(ROOT, "tools"))
from lifter import Lifter, Unsupported, is_ldst  # noqa: E402
from nso import Nso  # noqa: E402

BASE = 0x7000000000  # main's load address (runtime: module 0)
OUT = os.path.join(ROOT, "build", "difftest")
MSYS = r"C:\msys64\ucrt64\bin"
EDGE64 = [0, 1, 2, 0x7FFFFFFFFFFFFFFF, 0x8000000000000000, 0xFFFFFFFFFFFFFFFF, 0xFFFFFFFF, 0x80000000, 0x7FFFFFFF,
          0x100000000, 0xFFFFFFFF00000000]
FP32 = [0x00000000, 0x80000000, 0x3F800000, 0xBF800000, 0x7F800000, 0xFF800000, 0x7FC00000, 0x7F800001, 0x00000001,
        0x00800000, 0x4F000000, 0xCF000000, 0x5F000000, 0x3EFFFFFF, 0x4B000000]
FP64 = [0, 0x8000000000000000, 0x3FF0000000000000, 0xBFF0000000000000, 0x7FF0000000000000, 0xFFF0000000000000,
        0x7FF8000000000000, 0x7FF0000000000001, 1, 0x0010000000000000, 0x43E0000000000000, 0xC3E0000000000000]


def shape(w):
    if (w >> 25) & 0b111 == 0b111:          # FP / SIMD: opcode bits and the size / type fields
        return ("v", w & 0xFFE0FC00)
    return ("i", w >> 21, (w >> 10) & 0x3F if (w >> 24) & 0x1F in (0x0A, 0x0B, 0x1A, 0x1B) else 0)


def sample(module, per_shape, max_cases, only):
    n = Nso(os.path.join(ROOT, "extracted-132", "exefs", module))
    off, size = n.text
    lifter = Lifter(BASE, simd="c")
    seen, cases = {}, []
    words = [int(only, 16)] if only else None
    pcs = [BASE + off] if only else range(BASE + off, BASE + off + size, 4)
    for pc in pcs:
        w = words[0] if words else struct.unpack_from("<I", n.image, pc - BASE)[0]
        if w == 0 or is_ldst(w):
            continue
        k = shape(w)
        if not only and seen.get(k, 0) >= per_shape:
            continue
        try:
            insn = lifter.lift(w, pc)
        except (Unsupported, Exception):
            continue
        if insn.kind != "op" or not insn.code or "rt_" in insn.code or "u64u" in insn.code or "u32u" in insn.code:
            continue  # (straight-line, no memory, no runtime calls)
        seen[k] = seen.get(k, 0) + 1
        cases.append((w, pc, insn.code))
        if len(cases) >= max_cases:
            break
    return cases


def rand_state(rnd):
    x = [rnd.choice(EDGE64) if rnd.random() < 0.25 else rnd.getrandbits(64) for _ in range(31)]
    sp = (0x7200100000 + rnd.getrandbits(12) * 16)
    nzcv = rnd.getrandbits(4) << 28
    v = []
    for _ in range(32):
        r = rnd.random()
        if r < 0.3:
            v.append(sum(rnd.choice(FP32) << (32 * i) for i in range(4)))
        elif r < 0.5:
            v.append(rnd.choice(FP64) | (rnd.choice(FP64) << 64))
        elif r < 0.6:  # ordinary floats around 1
            v.append(sum(struct.unpack("<I", struct.pack("<f", rnd.uniform(-1e3, 1e3)))[0] << (32 * i) for i in range(4)))
        else:
            v.append(rnd.getrandbits(128))
    return x, sp, nzcv, v


def pack(st):
    x, sp, nzcv, v = st
    return struct.pack("<31Q", *x) + struct.pack("<QQ", sp, nzcv) + b"".join(
        struct.pack("<QQ", e & (2**64 - 1), e >> 64) for e in v)


STATE_BYTES = 31 * 8 + 16 + 32 * 16


def unpack(b):
    x = list(struct.unpack_from("<31Q", b, 0))
    sp, nzcv = struct.unpack_from("<QQ", b, 248)
    v = []
    for i in range(32):
        lo, hi = struct.unpack_from("<QQ", b, 264 + 16 * i)
        v.append(lo | (hi << 64))
    return x, sp, nzcv, v


def gen_c(cases, path):
    with open(path, "w") as f:
        f.write('#include <fcntl.h>\n#include <io.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include "cpu.h"\n#include "simd.h"\n')
        f.write("void rt_dispatch(Cpu *c) { (void)c; abort(); }\nvoid rt_svc(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; abort(); }\n")
        f.write("void rt_brk(Cpu *c, uint32_t i, uint64_t p) { (void)c; (void)i; (void)p; abort(); }\nvoid rt_udf(Cpu *c, uint64_t p) { (void)c; (void)p; abort(); }\n")
        f.write("uint64_t rt_ticks(void) { return 0; }\n")
        for i, (w, pc, code) in enumerate(cases):
            f.write(f"static void case_{i}(Cpu *c) {{ /* {w:08x} @ {pc:x} */\n{code}\n}}\n")
        f.write("typedef void (*CaseFn)(Cpu *);\nstatic const CaseFn cases[] = {")
        f.write(",".join(f"case_{i}" for i in range(len(cases))))
        f.write("};\n")
        f.write(r'''
// stdin: [u32 case][state] records; stdout: the state after the case, same layout (x0-x30, sp, nzcv, v0-v31)
int main(void) {
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    unsigned char in[4 + %d];
    while (fread(in, sizeof in, 1, stdin) == 1) {
        static Cpu c;
        memset(&c, 0, sizeof c);
        uint32_t k; memcpy(&k, in, 4);
        memcpy(c.x, in + 4, 31 * 8);
        memcpy(&c.sp, in + 4 + 248, 8);
        memcpy(&c.nzcv, in + 4 + 256, 8);
        memcpy(c.v, in + 4 + 264, 32 * 16);
        cases[k](&c);
        fwrite(c.x, 31 * 8, 1, stdout);
        fwrite(&c.sp, 8, 1, stdout);
        fwrite(&c.nzcv, 8, 1, stdout);
        fwrite(c.v, 32 * 16, 1, stdout);
    }
    return 0;
}
''' % STATE_BYTES)


def unicorn_run(w, pc, st):
    from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM
    from unicorn import arm64_const as A
    x, sp, nzcv, v = st
    uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
    page = pc & ~0xFFF
    uc.mem_map(page, 0x1000)
    uc.mem_write(pc, struct.pack("<I", w))
    uc.mem_map(0x7200000000, 0x200000)  # (the stack the states' sp points into)
    # FP/SIMD on: CPACR_EL1.FPEN = 0b11
    uc.reg_write(A.UC_ARM64_REG_CPACR_EL1, 0x300000)
    for i in range(29):
        uc.reg_write(A.UC_ARM64_REG_X0 + i, x[i])
    uc.reg_write(A.UC_ARM64_REG_X29, x[29])
    uc.reg_write(A.UC_ARM64_REG_X30, x[30])
    uc.reg_write(A.UC_ARM64_REG_SP, sp)
    uc.reg_write(A.UC_ARM64_REG_NZCV, nzcv)
    uc.reg_write(A.UC_ARM64_REG_FPCR, 0)
    for i in range(32):
        uc.reg_write(A.UC_ARM64_REG_Q0 + i, v[i])
    uc.emu_start(pc, pc + 4, count=1)
    ox = [uc.reg_read(A.UC_ARM64_REG_X0 + i) for i in range(29)] + [uc.reg_read(A.UC_ARM64_REG_X29), uc.reg_read(A.UC_ARM64_REG_X30)]
    return ox, uc.reg_read(A.UC_ARM64_REG_SP), uc.reg_read(A.UC_ARM64_REG_NZCV) & 0xF0000000, \
        [uc.reg_read(A.UC_ARM64_REG_Q0 + i) for i in range(32)]


def disasm(w, pc):
    try:
        from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
        for i in Cs(CS_ARCH_ARM64, CS_MODE_ARM).disasm(struct.pack("<I", w), pc):
            return f"{i.mnemonic} {i.op_str}"
    except Exception:
        pass
    return "?"


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--module", default="main")
    ap.add_argument("--per-shape", type=int, default=3)
    ap.add_argument("--states", type=int, default=8)
    ap.add_argument("--max-cases", type=int, default=4000)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--only", default="")
    ap.add_argument("--inject-mismatch", action="store_true", help="negative control: corrupt case 0 (must be reported)")
    args = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    cases = sample(args.module, args.per_shape, args.max_cases, args.only)
    print(f"{len(cases)} instruction cases sampled from {args.module}", flush=True)
    if args.inject_mismatch and cases:
        w, pc, code = cases[0]
        cases[0] = (w, pc, code + "\nc->x[0] ^= 1;")
    src, exe = os.path.join(OUT, "difftest_gen.c"), os.path.join(OUT, "difftest.exe")
    gen_c(cases, src)
    env = dict(os.environ, PATH=MSYS + os.pathsep + os.environ.get("PATH", ""))
    r = subprocess.run([os.path.join(MSYS, "clang.exe"), "-O1", "-w", "-fno-strict-aliasing", "-march=x86-64-v3", "-I", os.path.join(ROOT, "runtime"),
                        src, "-o", exe], env=env, capture_output=True, text=True)
    if r.returncode:
        print(r.stderr[-4000:])
        sys.exit(2)
    rnd = random.Random(args.seed)
    states, blob = [], bytearray()
    for k in range(len(cases)):
        for _ in range(args.states):
            st = rand_state(rnd)
            states.append((k, st))
            blob += struct.pack("<I", k) + pack(st)
    out = subprocess.run([exe], input=bytes(blob), capture_output=True, env=env).stdout
    if len(out) != len(states) * STATE_BYTES:
        print(f"the compiled cases stopped early ({len(out) // STATE_BYTES} of {len(states)} states): a case aborted")
        sys.exit(2)
    bad = {}
    for i, (k, st) in enumerate(states):
        w, pc, _ = cases[k]
        got = unpack(out[i * STATE_BYTES:(i + 1) * STATE_BYTES])
        try:
            ref = unicorn_run(w, pc, st)
        except Exception as e:  # (Unicorn rejects it: not comparable)
            bad.setdefault(k, []).append(f"unicorn: {e}")
            continue
        diffs = []
        for r_ in range(31):
            if got[0][r_] != ref[0][r_]:
                diffs.append(f"x{r_} {got[0][r_]:#x} != {ref[0][r_]:#x}")
        if got[1] != ref[1]:
            diffs.append(f"sp {got[1]:#x} != {ref[1]:#x}")
        if (got[2] & 0xF0000000) != ref[2]:
            diffs.append(f"nzcv {got[2] & 0xF0000000:#x} != {ref[2]:#x}")
        for r_ in range(32):
            if got[3][r_] != ref[3][r_]:
                diffs.append(f"v{r_} {got[3][r_]:#034x} != {ref[3][r_]:#034x}")
        if diffs:
            bad.setdefault(k, []).append("; ".join(diffs[:4]))
    unicorn_only = sum(1 for k in bad if all(m.startswith("unicorn:") for m in bad[k]))
    real = {k: m for k, m in bad.items() if not all(x.startswith("unicorn:") for x in m)}
    print(f"{len(cases)} cases x {args.states} states: {len(cases) - len(bad)} match, {len(real)} DIFFER, "
          f"{unicorn_only} not runnable in Unicorn")
    with open(os.path.join(OUT, "report.txt"), "w") as f:
        for k, msgs in sorted(real.items()):
            w, pc, _ = cases[k]
            line = f"{w:08x} @{pc:x} {disasm(w, pc):<40} {len(msgs)}/{args.states}: {msgs[0]}"
            f.write(line + "\n")
    for k, msgs in sorted(real.items())[:40]:
        w, pc, _ = cases[k]
        print(f"  {w:08x} {disasm(w, pc):<38} {len(msgs)}/{args.states}: {msgs[0][:160]}")
    sys.exit(1 if real else 0)


if __name__ == "__main__":
    main()
