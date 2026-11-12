#!/usr/bin/env python3
"""CI check: tools/android/genpar.py writes exactly what tools/gen.py writes. No game files in CI, so the module is
an AArch64 library made into an uncompressed NSO (the code segment, read-only data, data; MOD0 pointing at its
dynamic section and .eh_frame_hdr), lifted by both; the outputs must match file for file.
    test_genpar.py <aarch64 .so> <work dir> [jobs]"""
import filecmp
import os
import struct
import subprocess
import sys
import time

PT_LOAD, PT_DYNAMIC, PT_GNU_EH_FRAME = 1, 2, 0x6474E550


def elf_to_nso(so, out):
    d = open(so, 'rb').read()
    assert d[:4] == b'\x7fELF' and d[4] == 2, so
    phoff, = struct.unpack_from('<Q', d, 0x20)
    phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
    loads, dyn, eh = [], 0, 0
    for i in range(phnum):
        p_type, p_flags, p_off, p_vaddr, _, p_filesz, p_memsz, _ = struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize)
        if p_type == PT_LOAD:
            loads.append((p_flags, p_off, p_vaddr, p_filesz, p_memsz))
        elif p_type == PT_DYNAMIC:
            dyn = p_vaddr
        elif p_type == PT_GNU_EH_FRAME:
            eh = p_vaddr
    end = max(v + m for _, _, v, _, m in loads)
    img = bytearray((end + 0xFFF) & ~0xFFF)
    for _, off, v, fs, _ in loads:
        img[v:v + fs] = d[off:off + fs]
    text = next(l for l in loads if l[0] & 1)
    ro = next(l for l in loads if not l[0] & 1 and not l[0] & 2)
    rw = [l for l in loads if l[0] & 2]
    rw_lo, rw_hi = min(l[2] for l in rw), max(l[2] + l[3] for l in rw)
    bss_end = max(l[2] + l[4] for l in rw)
    mod0 = 0x10  # (in the ELF header, unused here)
    img[4:8] = struct.pack('<I', mod0)
    img[mod0:mod0 + 4] = b'MOD0'
    rel = lambda a: a - mod0
    img[mod0 + 4:mod0 + 28] = struct.pack('<6i', rel(dyn), rel(rw_hi), rel(bss_end), rel(eh), rel(eh), rel(rw_hi))
    segs = [(text[2], text[2] + text[3]), (ro[2], ro[2] + ro[3]), (rw_lo, rw_hi)]
    hdr = bytearray(0x100)
    hdr[0:4] = b'NSO0'
    body = bytearray()
    for i, (lo, hi) in enumerate(segs):
        struct.pack_into('<III', hdr, 0x10 + i * 0x10, 0x100 + len(body), lo, hi - lo)
        struct.pack_into('<I', hdr, 0x60 + i * 4, hi - lo)
        body += img[lo:hi]
    struct.pack_into('<I', hdr, 0x3C, bss_end - rw_hi)
    hdr[0x40:0x50] = b'genpar-test-0001'
    open(out, 'wb').write(bytes(hdr) + bytes(body))


def main():
    so, work = sys.argv[1], sys.argv[2]
    jobs = sys.argv[3] if len(sys.argv) > 3 else '4'
    os.makedirs(work, exist_ok=True)
    nso = os.path.join(work, 'test.nso')
    elf_to_nso(so, nso)
    tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
    a, b = os.path.join(work, 'gen'), os.path.join(work, 'genpar')
    t0 = time.time()
    subprocess.run([sys.executable, os.path.join(tools, 'gen.py'), 'sdk', nso, '0x1004000000', a, '--simd', 'asm'], check=True)
    t1 = time.time()
    subprocess.run([sys.executable, os.path.join(tools, 'android', 'genpar.py'), 'sdk', nso, '0x1004000000', b, '--simd', 'asm',
                    '--jobs', jobs], check=True)
    t2 = time.time()
    fa, fb = sorted(os.listdir(a)), sorted(os.listdir(b))
    bad = [f for f in fa if f not in fb or not filecmp.cmp(os.path.join(a, f), os.path.join(b, f), shallow=False)]
    bad += [f for f in fb if f not in fa]
    print(f'gen.py {t1 - t0:.0f}s, genpar.py ({jobs} jobs) {t2 - t1:.0f}s, {len(fa)} files')
    if bad:
        sys.exit(f'genpar output differs: {bad[:10]}')
    print('genpar: same output as gen.py')


if __name__ == '__main__':
    main()
