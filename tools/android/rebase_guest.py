#!/usr/bin/env python3
"""Android build: a copy of the sources with the guest address window moved down.

The runtime maps the game at fixed addresses (guest addresses are host addresses, runtime/rt.h): a 12.5 GB window
at 0x70_0000_0000. On a desktop a collision with the host's own mappings is settled by re-executing for a fresh
layout; an Android app is forked from the zygote, whose mappings land at random in the top 64 GB of the 39-bit
address space (0x70_0000_0000 and up) and can't be re-rolled. So the Android build uses 0x10_0000_0000, far below
anything the system maps there (and above the 4 GB that ART keeps for itself).

The window's addresses are written out all over the runtime and the generator (module bases, the hook tables of
runtime/fnhook.c, the patch sites of tools/patches.py...), always as 10-digit hex literals 0x7Xxxxxxxxx. Instead of
touching every one of them in the tree - and every one a later upstream change adds - this writes a rebased copy of
runtime/ and tools/ for the Android build to compile and to generate the game code with:

    rebase_guest.py <repo root> <out dir>      ->  <out dir>/runtime, <out dir>/tools
    rebase_guest.py --base <hex literal>       ->  that address rebased (the `make gen` bases)
    rebase_guest.py --check <repo root>        ->  lists what would change, and literals that look off

Both sides must come from the same copy: the game code library records what it was built from (GAMELIB_ID).
"""
import os
import re
import shutil
import sys

SRC_TOP = 0x7   # 0x7X_xxxx_xxxx: the desktop window (and the range checks around it, up to 0x80_0000_0000)
DST_TOP = 0x1   # 0x1X_xxxx_xxxx
LITERAL = re.compile(r'0x%X([0-7][0-9a-fA-F]{8})(?![0-9a-fA-F])' % SRC_TOP)
TEXT = ('.c', '.h', '.cpp', '.mm', '.py', '.txt', '.md', '.sh')
SKIP_DIRS = {'__pycache__', 'venv', '.venv', 'android'}


def rebase_text(s):
    return LITERAL.subn(lambda m: '0x%X%s' % (DST_TOP, m.group(1)), s)


def rebase_tree(root, out):
    total = 0
    for top in ('runtime', 'tools'):
        for dirpath, dirs, files in os.walk(os.path.join(root, top)):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
            rel = os.path.relpath(dirpath, root)
            os.makedirs(os.path.join(out, rel), exist_ok=True)
            for name in sorted(files):
                src, dst = os.path.join(dirpath, name), os.path.join(out, rel, name)
                if not name.endswith(TEXT):
                    shutil.copy2(src, dst)
                    continue
                with open(src, encoding='utf-8', errors='surrogateescape', newline='') as f:
                    text = f.read()
                new, n = rebase_text(text)
                total += n
                # (written only when it changed: keeps the timestamps, and so the incremental builds, of the rest)
                old = None
                if os.path.exists(dst):
                    with open(dst, encoding='utf-8', errors='surrogateescape', newline='') as f:
                        old = f.read()
                if old != new:
                    with open(dst, 'w', encoding='utf-8', errors='surrogateescape', newline='') as f:
                        f.write(new)
    return total


def check(root):
    total = 0
    odd = re.compile(r'0x[0-9a-fA-F]*7[0-3][0-9a-fA-F]{8}(?![0-9a-fA-F])')
    for top in ('runtime', 'tools'):
        for dirpath, dirs, files in os.walk(os.path.join(root, top)):
            dirs[:] = sorted(d for d in dirs if d not in SKIP_DIRS)
            for name in sorted(files):
                if not name.endswith(TEXT):
                    continue
                path = os.path.join(dirpath, name)
                with open(path, encoding='utf-8', errors='surrogateescape') as f:
                    for i, line in enumerate(f, 1):
                        n = len(LITERAL.findall(line))
                        total += n
                        if n:
                            print('%s:%d: %s' % (os.path.relpath(path, root), i, line.strip()[:150]))
                        for m in odd.finditer(line):  # longer literals that end like a window address
                            if not LITERAL.fullmatch(m.group(0)):
                                print('?? %s:%d: %s (left as is)' % (os.path.relpath(path, root), i, m.group(0)))
    print('%d literals' % total)


def main(argv):
    if len(argv) == 3 and argv[1] == '--base':
        new, n = rebase_text(argv[2])
        if n != 1:
            sys.exit('not a guest address: %s' % argv[2])
        print(new)
    elif len(argv) == 3 and argv[1] == '--check':
        check(argv[2])
    elif len(argv) == 3:
        n = rebase_tree(argv[1], argv[2])
        if n < 100:  # (the tree has ~150: fewer means the layout changed upstream and this needs another look)
            sys.exit('only %d guest addresses found under %s: has runtime/rt.h changed?' % (n, argv[1]))
        print('rebased %d guest addresses: %s' % (n, argv[2]))
    else:
        sys.exit(__doc__)


if __name__ == '__main__':
    main(sys.argv)
