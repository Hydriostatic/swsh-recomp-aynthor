"""LZ4 block decompression in pure Python: what tools/nso.py needs of the lz4 package, for the Android build's
on-device generator (no C extension there). Same results as lz4.block.decompress(data, uncompressed_size=n)."""


def decompress(src, uncompressed_size=-1, **_):
    src = memoryview(bytes(src))
    n = len(src)
    out = bytearray()
    i = 0
    while i < n:
        token = src[i]
        i += 1
        lit = token >> 4
        if lit == 15:
            while True:
                b = src[i]
                i += 1
                lit += b
                if b != 255:
                    break
        out += src[i:i + lit]
        i += lit
        if i >= n:  # (the last sequence has literals only)
            break
        off = src[i] | src[i + 1] << 8
        i += 2
        if not off or off > len(out):
            raise ValueError('lz4: bad offset %d at %d' % (off, i))
        ml = token & 15
        if ml == 15:
            while True:
                b = src[i]
                i += 1
                ml += b
                if b != 255:
                    break
        ml += 4
        start = len(out) - off
        if ml <= off:
            out += out[start:start + ml]
        else:  # overlapping: the pattern repeats
            chunk = out[start:]
            reps, rest = divmod(ml, off)
            out += chunk * reps + chunk[:rest]
    if uncompressed_size >= 0 and len(out) != uncompressed_size:
        raise ValueError('lz4: %d bytes, expected %d' % (len(out), uncompressed_size))
    return bytes(out)
