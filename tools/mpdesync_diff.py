#!/usr/bin/env python3
"""Say what two clients disagreed about, in words.

--mp-desync-dump leaves both players' copy of the guilty turn on disk as
mpdesync-s1-turnN.bin and mpdesync-s2-turnN.bin.  Those are the savegame
chunks back to back, so `cmp` says "byte 17763" and nothing else.  This
walks the chunks, finds the ones that differ and decodes the records
inside them, so the answer is "structure 1's animation is two frames
behind" rather than an offset.

    tools/mpdesync_diff.py /tmp/mpduel.XXXX/mpdesync-s{1,2}-turn298.bin
    tools/mpdesync_diff.py DIR          # every turn pair in a log directory
"""

import re
import sys
import os
import glob

TILE_FLAGS = (
    (0x08, 'unveiled'), (0x10, 'unit'), (0x20, 'structure'),
    (0x40, 'anim'), (0x80, 'explosion'),
)


def chunks(data):
    """[(name, start, end)] for one dump.  Header is '\\n== name size\\n'."""
    out = []
    for m in re.finditer(rb'\n== ([a-z]+) (\d+)\n', data):
        name = m.group(1).decode()
        size = int(m.group(2))
        start = m.end()
        out.append((name, start, start + size))
    return out


def decode_tile(b):
    ground = b[0] | ((b[1] & 1) << 8)
    flags = [n for bit, n in TILE_FLAGS if b[2] & bit]
    return ('ground %d overlay %d house %d index %d%s'
            % (ground, b[1] >> 1, b[2] & 7, b[3],
               (' [' + ' '.join(flags) + ']') if flags else ''))


def map_records(data, start, end):
    """{tile index: 4 tile bytes} -- the map chunk is index,tile pairs."""
    out = {}
    p = start
    while p + 6 <= end:
        out[int.from_bytes(data[p:p + 2], 'little')] = data[p + 2:p + 6]
        p += 6
    return out


def anim_records(data, start, end):
    """{slot: fields} -- see Animation_Save() in src/animation.c."""
    out = {}
    if end - start < 8:
        return out
    out['timer'] = int.from_bytes(data[start:start + 4], 'little')
    out['clock'] = int.from_bytes(data[start + 4:start + 8], 'little')
    p = start + 8
    while p + 16 <= end:
        u = lambda o: int.from_bytes(data[p + o:p + o + 2], 'little')
        out[u(0)] = ('tickNext %d layout %d script %d house %d step %d '
                     'icon %d tile %d,%d'
                     % (int.from_bytes(data[p + 2:p + 6], 'little'),
                        u(6), u(8), u(10) >> 8, u(10) & 0xFF,
                        u(12), u(14), int.from_bytes(data[p + 16:p + 18], 'little')
                        if p + 18 <= end else 0))
        p += 18
    return out


def report(path1, path2):
    a, b = open(path1, 'rb').read(), open(path2, 'rb').read()
    ca, cb = chunks(a), chunks(b)

    print('%s\n%s' % (os.path.basename(path1), os.path.basename(path2)))

    names = [n for n, _, _ in ca]
    if names != [n for n, _, _ in cb]:
        print('  the two dumps do not even hold the same chunks')
        return

    quiet = True
    for (name, sa, ea), (_, sb, eb) in zip(ca, cb):
        if a[sa:ea] == b[sb:eb]:
            continue
        quiet = False
        print('  == %s (%d vs %d bytes)' % (name, ea - sa, eb - sb))

        if name == 'map':
            ra, rb = map_records(a, sa, ea), map_records(b, sb, eb)
            for idx in sorted(set(ra) | set(rb)):
                if ra.get(idx) == rb.get(idx):
                    continue
                print('     tile %d (x=%d y=%d)' % (idx, idx % 64, idx // 64))
                print('       s1  %s' % (decode_tile(ra[idx]) if idx in ra else '(absent)'))
                print('       s2  %s' % (decode_tile(rb[idx]) if idx in rb else '(absent)'))
        elif name == 'anim':
            ra, rb = anim_records(a, sa, ea), anim_records(b, sb, eb)
            for key in sorted(set(ra) | set(rb), key=lambda k: (isinstance(k, str), k)):
                if ra.get(key) == rb.get(key):
                    continue
                print('     slot %s' % key)
                print('       s1  %s' % ra.get(key, '(empty)'))
                print('       s2  %s' % rb.get(key, '(empty)'))
        else:
            first = next((i for i in range(min(ea - sa, eb - sb))
                          if a[sa + i] != b[sb + i]), None)
            print('     first difference at byte %s of the chunk' % first)

    if quiet:
        print('  the two dumps agree')


def main(argv):
    if len(argv) == 3:
        report(argv[1], argv[2])
        return 0

    if len(argv) == 2 and os.path.isdir(argv[1]):
        turns = sorted({int(re.search(r'turn(\d+)', p).group(1))
                        for p in glob.glob(os.path.join(argv[1], 'mpdesync-s1-turn*.bin'))})
        for t in turns:
            p1 = os.path.join(argv[1], 'mpdesync-s1-turn%d.bin' % t)
            p2 = os.path.join(argv[1], 'mpdesync-s2-turn%d.bin' % t)
            if os.path.exists(p2):
                report(p1, p2)
                print()
        return 0

    sys.stderr.write(__doc__)
    return 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
