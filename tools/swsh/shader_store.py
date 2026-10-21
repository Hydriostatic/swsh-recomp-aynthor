"""Find every shader stage in the game's files and measure how many of a real session's programs they cover.

Usage:
  shader_store.py inventory <romfs dir> [<more dirs>...] --out <store dir>
  shader_store.py coverage <store dir> <pipeline_recipes_v*.bin>

`inventory` walks the dirs (the romfs and, optionally, the DLC romfs), opens each .gfpak (every entry is LZ4) and
scans every file for BNSH containers (loose .bnsh_vsh / .bnsh_fsh, and the ones inside .gfpak, .ptcl, ...). Each
container is parsed as nn::gfx ResShaderFile (the layout from SuperMarioWonderRecomp tools/maxwell/containers.py, MIT):
variations -> program -> per-stage code block + NVN control section. A stage's id is the runtime's own: FNV-1a 64 of
the code block, `control + 0x704` bytes from its start (runtime/nvn/objects.c set_shaders, exec.c stage_hash_rt), so a
stage found here is the one the game hands to nvnProgramSetShaders. Written to the store:
  stages/<hash>.code, stages/<hash>.ctrl   each distinct stage (code block, control section)
  stages.tsv                               hash, stage, code bytes, first file seen in, times seen
  programs.tsv                             the vertex + fragment pairs of every variation (by hash)
The store holds the game's own code: keep it out of git (build/ or the cache dir).

`coverage` reads the pipeline recipes of a real session (every pipeline a draw created; records hold the two stages'
content hashes) and reports how many stages and programs the store has.
"""
import collections
import os
import struct
import sys

import lz4.block

SIG = b'BNSH\0\0\0\0'
STAGES = ('vertex', 'hull', 'domain', 'geometry', 'fragment', 'compute')  # ShaderStage order of ResShaderProgramData
CODE_MAGIC = {'compute': 0x12345679}  # (0x12345678 in front of the graphics stages)
CONTROL_MAGIC = 0x98761234


def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def u64(d, o): return struct.unpack_from('<Q', d, o)[0]


def fnv1a(b):
    h = 0xcbf29ce484222325
    for x in b:
        h = ((h ^ x) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h or 1


def bnsh_programs(d, base):
    """[(variation, [(stage name, code bytes, control bytes)])] of the BNSH at d[base:]; [] when it isn't one."""
    if base + 0x20 > len(d):
        return []
    size = u32(d, base + 0x1C)
    first = u16(d, base + 0x16)
    if not 0x100 < size <= len(d) - base or d[base + first:base + first + 4] != b'grsc':
        return []
    g = base + first
    nvar, var_array = u32(d, g + 0x1C), u64(d, g + 0x20)
    out = []
    for k in range(nvar):
        binary = u64(d, base + var_array + k * 0x40 + 0x10)
        if not binary:
            continue
        p = base + binary
        stages = []
        for s, kind in enumerate(STAGES):
            sc = u64(d, p + 8 + s * 8)
            if not sc:
                continue
            sc += base
            ctrl, code = base + u64(d, sc + 8), base + u64(d, sc + 0x10)
            code_size, ctrl_size = u32(d, sc + 0x18), u32(d, sc + 0x1C)
            if code + code_size > len(d) or ctrl + ctrl_size > len(d) or ctrl_size < 0x708:
                continue
            if u32(d, ctrl) != CONTROL_MAGIC or u32(d, code) != CODE_MAGIC.get(kind, 0x12345678):
                continue
            n = u32(d, ctrl + 0x704)  # (the size the runtime hashes; equal to the descriptor's in every file seen)
            if not n or n > code_size:
                n = code_size
            stages.append((kind, d[code:code + n], d[ctrl:ctrl + ctrl_size]))
        out.append((k, stages))
    return out


def expand(path, d):
    """(label, bytes) of a file's data, or of each entry of a .gfpak."""
    if d[:8] != b'GFLXPACK':
        yield path, d
        return
    nf, fi = u32(d, 0x10), u64(d, 0x18)
    for i in range(nf):
        _, ct, dsz, csz, _, off = struct.unpack_from('<HHIIIQ', d, fi + i * 24)
        raw = d[off:off + csz]
        if ct == 2:
            raw = lz4.block.decompress(raw, uncompressed_size=dsz)
        elif ct != 0:
            continue  # (not seen in this game: every entry is LZ4)
        yield f'{path}#{i}', raw


def buffers(path):
    with open(path, 'rb') as f:
        yield from expand(path, f.read())


def romfs_image(path):
    """The files of a RomFS image (the setup's romfs.bin, the DLC's aoc/<data id>.bin), by name; [] if it isn't one."""
    with open(path, 'rb') as f:
        h = struct.unpack('<10Q', f.read(0x50))
        if h[0] != 0x50:
            return
        f.seek(h[7])
        meta = f.read(h[8])
        o = 0
        while o + 0x20 <= len(meta):
            off, size = u64(meta, o + 8), u64(meta, o + 0x10)
            nl = u32(meta, o + 0x1C)
            name = meta[o + 0x20:o + 0x20 + nl].decode('utf-8', 'replace')
            o += 0x20 + ((nl + 3) & ~3)
            if size > 512 * 1024 * 1024:
                continue
            f.seek(h[9] + off)
            yield from expand(f'{os.path.basename(path)}:{name}', f.read(size))


def nso(path):
    """The executable's segments (text, rodata, data), decompressed: the game keeps some shaders in it."""
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'NSO0':
        return d
    flags, segs = u32(d, 0xC), []
    for k in range(3):
        foff, moff, size = struct.unpack_from('<III', d, 0x10 + k * 0x10)
        raw = d[foff:foff + u32(d, 0x60 + k * 4)]
        segs.append((moff, lz4.block.decompress(raw, uncompressed_size=size) if flags & (1 << k) else raw))
    out = bytearray(max(m + len(b) for m, b in segs))  # (as loaded: each segment at its offset in memory)
    for m, b in segs:
        out[m:m + len(b)] = b
    return bytes(out)


def sources(roots):
    """Every buffer to scan: files under a directory (an exefs directory: its executables), or a RomFS image."""
    for root in roots:
        if os.path.isfile(root):
            yield from romfs_image(root)
            continue
        exefs = os.path.basename(os.path.normpath(root)) == 'exefs'
        for dp, _, names in os.walk(root):
            for name in names:
                path = os.path.join(dp, name)
                if exefs:
                    yield os.path.relpath(path, os.path.dirname(root)), nso(path)
                elif os.path.getsize(path) <= 512 * 1024 * 1024:
                    yield from (( os.path.relpath(lab, root), d) for lab, d in buffers(path))


def inventory(dirs, out):
    os.makedirs(os.path.join(out, 'stages'), exist_ok=True)
    seen = {}  # fnv -> [stage, bytes, first label, count]
    by_bytes = {}  # code bytes -> fnv (hash each distinct block once)
    progs = set()
    nfiles = ncont = 0
    if True:
        if True:
            if True:
                for label, d in sources(dirs):
                    nfiles += 1
                    i = d.find(SIG)
                    while i != -1:
                        ps = bnsh_programs(d, i)
                        ncont += bool(ps)
                        for _, stages in ps:
                            hs = {}
                            for kind, code, ctrl in stages:
                                h = by_bytes.get(code)
                                if h is None:
                                    h = by_bytes[code] = fnv1a(code)
                                if h in seen:
                                    seen[h][3] += 1
                                else:
                                    seen[h] = [kind, len(code), label, 1]
                                    with open(os.path.join(out, 'stages', f'{h:016x}.code'), 'wb') as f:
                                        f.write(code)
                                    with open(os.path.join(out, 'stages', f'{h:016x}.ctrl'), 'wb') as f:
                                        f.write(ctrl)
                                hs[kind] = h
                            progs.add((hs.get('vertex', 0), hs.get('fragment', 0), hs.get('compute', 0)))
                        i = d.find(SIG, i + 8)
    with open(os.path.join(out, 'stages.tsv'), 'w', encoding='utf-8') as f:
        f.write('hash\tstage\tbytes\tfirst_seen\tcount\n')
        for h, (kind, n, src, cnt) in sorted(seen.items()):
            f.write(f'{h:016x}\t{kind}\t{n}\t{src}\t{cnt}\n')
    with open(os.path.join(out, 'programs.tsv'), 'w') as f:
        f.write('vertex\tfragment\tcompute\n')
        for v, fr, c in sorted(progs):
            f.write(f'{v:016x}\t{fr:016x}\t{c:016x}\n')
    kinds = collections.Counter(v[0] for v in seen.values())
    print(f'{nfiles} files, {ncont} BNSH containers, {len(seen)} distinct stages '
          f'({sum(v[1] for v in seen.values()) / 1e6:.1f} MB), {len(progs)} distinct programs')
    for k, n in kinds.most_common():
        print(f'  {k:9s} {n:6d}')


def coverage(store, recipes):
    have = set()
    with open(os.path.join(store, 'stages.tsv')) as f:
        next(f)
        for line in f:
            have.add(int(line.split('\t')[0], 16))
    pairs = set()
    with open(os.path.join(store, 'programs.tsv')) as f:
        next(f)
        for line in f:
            v, fr, _ = (int(x, 16) for x in line.split('\t'))
            pairs.add((v, fr))
    d = open(recipes, 'rb').read()
    # RecipeRec: magic, version, keysize, pipesize (u32 each), chash[2], ShaderKey vkey, fkey, PipeKey, check
    keysize, pipesize = u32(d, 8), u32(d, 12)
    rec = 32 + 2 * keysize + pipesize + 8
    stages, progs = set(), set()
    for o in range(0, len(d) - rec + 1, rec):
        if u32(d, o) != 0x50434552:
            continue
        v, fr = u64(d, o + 16), u64(d, o + 24)
        progs.add((v, fr))
        stages.update(x for x in (v, fr) if x)
    miss = [h for h in stages if h not in have]
    pmiss = [p for p in progs if not all(x in have for x in p if x)]
    print(f'{len(d) // rec} recipes: {len(progs)} programs, {len(stages)} stages')
    print(f'  stages in the store   {len(stages) - len(miss):6d} / {len(stages)} ({100 * (1 - len(miss) / max(1, len(stages))):.1f}%)')
    print(f'  programs, both stages {len(progs) - len(pmiss):6d} / {len(progs)}')
    print(f'  programs as a BNSH variation pairs them {sum(p in pairs for p in progs):6d} / {len(progs)}')
    if miss:
        print('  missing stages (first 20):', ' '.join(f'{h:016x}' for h in miss[:20]))


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == 'inventory' and '--out' in a:
        out = a[a.index('--out') + 1]
        inventory([x for x in a[1:a.index('--out')]], out)
    elif len(a) == 3 and a[0] == 'coverage':
        coverage(a[1], a[2])
    else:
        sys.exit(__doc__)
