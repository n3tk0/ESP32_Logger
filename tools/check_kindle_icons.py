#!/usr/bin/env python3
"""
Hold each panel icon's own ground to the ground it is drawn on.

WHY THIS EXISTS
---------------
The browser page draws its weather glyphs as inline SVG with no background at
all: `appendWeatherIcon()` emits stroke work on a transparent canvas, so an
icon in an outlook column sits on the column's `.per` wash and an icon beside
the headline sits on the page's white. One drawing, two grounds, and nothing
to keep in step.

The panel cannot do that. FBInk blits a rectangle of pixels, every one of them
opaque, so each BMP carries a ground of its own — and that ground has to be
the ground it lands on. It was white for all of them. The three outlook icons
are blitted onto a GRAYE plate, so each one came out as a white card inside
the grey card: three bright squares in the one row of the panel that is meant
to read as three quiet ones. The big icon beside the headline lands on the
page's white and was right all along, which is why this was easy to look at
for months without seeing it.

WHAT IT CHECKS
--------------
Every fc_<code>_<size>.bmp, against the size's role:

    FC_OL_SZ    the outlook columns  -> the pen the plate is filled with
    FC_MAIN_SZ  beside the headline  -> the pen the forecast zone is cleared with

BOTH GROUNDS ARE READ OUT OF update_dash.sh, not written down here. A checker
holding its own copy of a colour is a checker that passes while the plate and
the icons on it are two different greys.

And the knockouts have to survive. The cloud body and the sun disc are FILLED
WHITE on purpose — that is what hides the rays passing behind the cloud — so
re-grounding an icon is a flood fill from its border, never a global swap of
one grey for another. An icon whose enclosed white is gone has had its art
flattened, and the check says so.

    python3 tools/check_kindle_icons.py           # report
    python3 tools/check_kindle_icons.py --fix     # re-ground in place
"""
import os
import re
import struct
import sys
from collections import deque

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DASH = os.path.join(ROOT, 'kindle/update_dash.sh')

# Icon directory and the layout that says what sizes it draws at.
PANELS = [('kindle/icons/600', 'kindle/layout/600x800.conf'),
          ('kindle/icons/1072', 'kindle/layout/1072x1448.conf')]

# fc_batt.bmp is not a weather glyph and is not in this table: it is a badge
# knocked out of its own black plate, and its ground is the black.
ICON_RE = re.compile(r'^fc_(-?\d+)_(\d+)\.bmp$')

# What each role is, in the words the panel's own comments use for it.
WHERE = {'outlook': 'outlook columns', 'main': 'headline'}


def pen_index(name):
    """An FBInk pen name to the grey it paints, 0..15.

    The palette is BLACK, GRAY1..GRAY9, GRAYA..GRAYE, WHITE — sixteen levels
    of 0x11, which is exactly the palette these BMPs carry, so the pen's index
    and the file's palette index are the same number.
    """
    if name == 'BLACK':
        return 0
    if name == 'WHITE':
        return 15
    m = re.fullmatch(r'GRAY([1-9A-E])', name)
    if m:
        return int(m.group(1), 16)
    raise SystemExit('check_kindle_icons: unknown FBInk pen %r' % name)


def dash_source():
    src = open(DASH, encoding='utf-8').read()
    return src.replace('\\\n', ' ')          # join the continuation lines


def plate_pen(src):
    """The pen the outlook plate is filled with, from the fill_rect that draws it."""
    m = re.search(r'fill_rect\b[^\n]*OL_PLATE_H[^\n]*?\s([A-Z0-9]+)\s*$',
                  src, re.M)
    if not m:
        raise SystemExit('check_kindle_icons: no outlook plate fill_rect in '
                         'update_dash.sh — has draw_forecast_body changed?')
    return m.group(1)


def zone_pen(src):
    """The pen the forecast zone is cleared with, which is what the big icon lands on.

    Not clear_screen()'s: the panel is cleared once at startup and the zone is
    cleared before every forecast tier, so the zone's pen is the one the icon
    actually meets. They are both WHITE today, and reading the wrong one would
    keep passing on the day one of them is not.
    """
    m = re.search(r'fill_rect\s+"\$Z_FC_X"[^\n]*?\s([A-Z0-9]+)\s*$', src, re.M)
    if not m:
        raise SystemExit('check_kindle_icons: no forecast-zone fill_rect in '
                         'update_dash.sh — has clear_fc_zone changed?')
    return m.group(1)


def layout_sizes(path):
    """FC_OL_SZ and FC_MAIN_SZ out of a layout file."""
    got = {}
    for line in open(os.path.join(ROOT, path), encoding='utf-8'):
        m = re.match(r'\s*(FC_OL_SZ|FC_MAIN_SZ)=(\d+)', line)
        if m:
            got[m.group(1)] = int(m.group(2))
    for key in ('FC_OL_SZ', 'FC_MAIN_SZ'):
        if key not in got:
            raise SystemExit('check_kindle_icons: %s not in %s' % (key, path))
    return got


class Bmp4:
    """The 4-bit greyscale BMP these icons are, as a grid of palette indices."""

    def __init__(self, path):
        self.path = path
        self.raw = bytearray(open(path, 'rb').read())
        if self.raw[:2] != b'BM':
            raise SystemExit('check_kindle_icons: %s is not a BMP' % path)
        self.off = struct.unpack_from('<I', self.raw, 10)[0]
        w, h = struct.unpack_from('<ii', self.raw, 18)
        bpp = struct.unpack_from('<H', self.raw, 28)[0]
        if bpp != 4:
            raise SystemExit('check_kindle_icons: %s is %d bpp, expected 4'
                             % (path, bpp))
        self.w, self.h = w, abs(h)
        self.row = (w * 4 + 31) // 32 * 4

    def _at(self, x, y):
        return self.off + y * self.row + x // 2

    def get(self, x, y):
        b = self.raw[self._at(x, y)]
        return (b >> 4) if x % 2 == 0 else (b & 0xF)

    def set(self, x, y, v):
        i = self._at(x, y)
        if x % 2 == 0:
            self.raw[i] = (v << 4) | (self.raw[i] & 0x0F)
        else:
            self.raw[i] = (self.raw[i] & 0xF0) | v

    def save(self):
        open(self.path, 'wb').write(self.raw)


def ground(bmp):
    """The icon's ground, or None when its four corners disagree about it."""
    corners = {bmp.get(0, 0), bmp.get(bmp.w - 1, 0),
               bmp.get(0, bmp.h - 1), bmp.get(bmp.w - 1, bmp.h - 1)}
    return corners.pop() if len(corners) == 1 else None


def outside(bmp, value):
    """Every pixel of `value` reachable from the border — the ground, and only it.

    Stops at the strokes, which is what leaves the cloud body and the sun disc
    behind: they are white too, and they are not the ground.
    """
    seen = [[False] * bmp.w for _ in range(bmp.h)]
    q = deque()

    def seed(x, y):
        if not seen[y][x] and bmp.get(x, y) == value:
            seen[y][x] = True
            q.append((x, y))

    for x in range(bmp.w):
        seed(x, 0)
        seed(x, bmp.h - 1)
    for y in range(bmp.h):
        seed(0, y)
        seed(bmp.w - 1, y)
    out = []
    while q:
        x, y = q.popleft()
        out.append((x, y))
        for nx, ny in ((x + 1, y), (x - 1, y), (x, y + 1), (x, y - 1)):
            if 0 <= nx < bmp.w and 0 <= ny < bmp.h:
                seed(nx, ny)
    return out, seen


def knockout(bmp, seen):
    """White pixels the border cannot reach: the fills the art needs to keep."""
    return sum(1 for y in range(bmp.h) for x in range(bmp.w)
               if bmp.get(x, y) == 15 and not seen[y][x])


def inspect(bmp, role, rel, pens, want):
    """Everything wrong with one icon, as sentences. Empty when it is right."""
    problems = []
    have = ground(bmp)
    if have is None:
        return ['%s: its four corners are not one colour, so it has no ground '
                'to check' % rel]
    if have != want[role]:
        problems.append('%s: ground is grey %d, and it is drawn in the %s, '
                        'onto %s (grey %d) — an opaque blit has to carry the '
                        'ground it lands on'
                        % (rel, have, WHERE[role], pens[role], want[role]))
        return problems
    # The art, on a ground that is already right: the enclosed whites are what
    # stop the sun's rays showing through the cloud in front of them.
    if want[role] != 15:
        _, seen = outside(bmp, want[role])
        if knockout(bmp, seen) == 0:
            problems.append('%s: no enclosed white left — the fills that hide '
                            'what passes behind the cloud have been flattened '
                            'into the ground' % rel)
    return problems


def grounds():
    """The two pens, and the greys they paint, read out of update_dash.sh."""
    src = dash_source()
    pens = {'outlook': plate_pen(src), 'main': zone_pen(src)}
    return pens, {role: pen_index(name) for role, name in pens.items()}


def each_icon():
    """Every weather icon on both panels, with the role its size gives it."""
    for icon_dir, layout in PANELS:
        sizes = layout_sizes(layout)
        role_of = {sizes['FC_OL_SZ']: 'outlook', sizes['FC_MAIN_SZ']: 'main'}
        full = os.path.join(ROOT, icon_dir)
        for name in sorted(os.listdir(full)):
            m = ICON_RE.match(name)
            if not m:
                continue
            size = int(m.group(2))
            yield ('%s/%s' % (icon_dir, name), os.path.join(full, name),
                   role_of.get(size), sizes)


def self_test():
    """Bend each icon in memory and require this file to notice.

    THE MUTATION LIVES HERE rather than in the workflow, for the reason the
    parity checker's does: a CI step that rewrites a file and restores it needs
    a copy of what that file looks like, and the copy goes stale first. These
    are BMPs — a step bending one would be a byte offset in a YAML file, which
    is worse again. Nothing is written to disk.
    """
    pens, want = grounds()
    bad = []
    bends = 0
    n = 0
    for rel, path, role, _ in each_icon():
        if role is None:
            continue
        n += 1
        bends += 3 if want[role] != 15 else 2

        # 1. The ground is the ground it lands on.
        bmp = Bmp4(path)
        wrong = 15 if want[role] != 15 else 14
        for x, y in outside(bmp, want[role])[0]:
            bmp.set(x, y, wrong)
        if not inspect(bmp, role, rel, pens, want):
            bad.append('%s: a ground of grey %d went unnoticed' % (rel, wrong))

        # 2. One corner off is a ground this file cannot speak for.
        bmp = Bmp4(path)
        bmp.set(0, 0, 0)
        if not inspect(bmp, role, rel, pens, want):
            bad.append('%s: a corner that does not match the rest went '
                       'unnoticed' % rel)

        if want[role] == 15:
            continue

        # 3. The knockouts, flattened — what a global swap of one grey for
        # another does to an icon, as against the flood fill --fix uses.
        bmp = Bmp4(path)
        for y in range(bmp.h):
            for x in range(bmp.w):
                if bmp.get(x, y) == 15:
                    bmp.set(x, y, want[role])
        if not inspect(bmp, role, rel, pens, want):
            bad.append('%s: its cloud fills were flattened into the plate and '
                       'this file passed it' % rel)

    for line in bad:
        print('check_kindle_icons: %s' % line, file=sys.stderr)
    if bad:
        return 1
    print('check_kindle_icons: self-test — %d bends across %d icons, every one '
          'of them caught' % (bends, n))
    return 0


def main(argv):
    args = argv[1:]
    for arg in args:
        if arg not in ('--fix', '--self-test'):
            raise SystemExit('usage: check_kindle_icons.py [--fix | --self-test]')
    if '--self-test' in args:
        return self_test()
    fix = '--fix' in args

    pens, want = grounds()
    problems = []
    fixed = []
    checked = 0

    for rel, path, role, sizes in each_icon():
        if role is None:
            problems.append('%s: drawn at no size this panel uses '
                            '(FC_OL_SZ=%d, FC_MAIN_SZ=%d)'
                            % (rel, sizes['FC_OL_SZ'], sizes['FC_MAIN_SZ']))
            continue

        checked += 1
        bmp = Bmp4(path)
        have = ground(bmp)
        if fix and have is not None and have != want[role]:
            for x, y in outside(bmp, have)[0]:
                bmp.set(x, y, want[role])
            bmp.save()
            bmp = Bmp4(path)
            fixed.append(rel)
        problems += inspect(bmp, role, rel, pens, want)

    for rel in fixed:
        print('re-grounded %s' % rel)
    if problems:
        for p in problems:
            print('check_kindle_icons: %s' % p, file=sys.stderr)
        return 1
    print('check_kindle_icons: %d icons, each on the ground it is drawn on '
          '(outlook %s, headline %s)' % (checked, pens['outlook'], pens['main']))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
