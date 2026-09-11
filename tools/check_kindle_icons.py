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

    python3 tools/check_kindle_icons.py             # report
    python3 tools/check_kindle_icons.py --fix       # re-ground in place
    python3 tools/check_kindle_icons.py --self-test # prove the report has teeth

scripts/generate_kindle_icons.py re-grounds through this file rather than
keeping a second copy of the rule: it is what produces these BMPs, and a
generator that fills every canvas white would put the three white cards back
the next time anybody added a WMO code.
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


def resolutions():
    """{panel width: (main size, outlook size)}, out of the layout files.

    THE LAYOUTS ARE THE SOURCE, because they are what the panel itself reads
    at run time. scripts/generate_kindle_icons.py used to carry its own
    RESOLUTIONS dict and check_kindle_parity.py read the sizes back out of it,
    so one pair of numbers lived in two places: change FC_OL_SZ in a layout
    without editing the generator and the two checkers go looking at different
    filenames — one still verifying that the old size exists, the other
    reporting every file it finds as drawn at no size this panel uses, and
    neither of them naming the two copies that disagree.
    """
    out = {}
    for icon_dir, layout in PANELS:
        sizes = layout_sizes(layout)
        out[int(os.path.basename(icon_dir))] = (sizes['FC_MAIN_SZ'],
                                                sizes['FC_OL_SZ'])
    return out


class Bmp4:
    """The 4-bit greyscale BMP these icons are, as a grid of palette indices.

    THE PALETTE IS CHECKED, not assumed. Everything below compares a palette
    INDEX against an FBInk grey LEVEL, and those are the same number only while
    the palette is the ramp the generator writes — entry i is (17i, 17i, 17i).
    A file with any other palette is a legal BMP that paints something else
    entirely: reverse the ramp and a ground of index 14 is near-black, while
    every check here reports a contented GRAYE. That assumption is exactly the
    kind of copy this file's docstring refuses to keep, so it is verified once,
    here, and the rest of the file may then speak in indices.
    """

    def __init__(self, path, raw=None):
        self.path = path
        self.raw = bytearray(open(path, 'rb').read() if raw is None else raw)
        where = path or '<bytes>'
        if self.raw[:2] != b'BM':
            raise SystemExit('check_kindle_icons: %s is not a BMP' % where)
        self.off = struct.unpack_from('<I', self.raw, 10)[0]
        w, h = struct.unpack_from('<ii', self.raw, 18)
        bpp = struct.unpack_from('<H', self.raw, 28)[0]
        if bpp != 4:
            raise SystemExit('check_kindle_icons: %s is %d bpp, expected 4'
                             % (where, bpp))
        self.w, self.h = w, abs(h)
        self.row = (w * 4 + 31) // 32 * 4

        ncol = struct.unpack_from('<I', self.raw, 46)[0] or 16
        base = self.off - 4 * ncol
        for i in range(ncol):
            b, g, r, _ = struct.unpack_from('<BBBB', self.raw, base + 4 * i)
            if not (b == g == r == i * 17):
                raise SystemExit(
                    'check_kindle_icons: %s has palette entry %d = '
                    '(%d,%d,%d), not the (%d,%d,%d) ramp every grey in this '
                    'file is named by — its indices mean nothing here'
                    % (where, i, r, g, b, i * 17, i * 17, i * 17))

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
    """The icon's ground, or None when the border does not agree on one.

    EVERY PIXEL THE BORDER REACHES, not the four corners. An icon re-grounded
    in part — a hand edit, a crop, a fill that stranded a patch it could only
    have reached diagonally — keeps its corners and grows a white speck on the
    plate, and four samples cannot see it. The flood fill is being run either
    way, so asking it for the whole region costs nothing.

    Art is allowed to touch the edge: the thunderstorm's bolt does. So the
    ground is the value the BIGGEST border-reachable region has, and the test
    is that no OTHER border pixel holds a value that is a ground anywhere else
    on this icon — which is what a half-finished re-grounding looks like.
    """
    corners = {bmp.get(0, 0), bmp.get(bmp.w - 1, 0),
               bmp.get(0, bmp.h - 1), bmp.get(bmp.w - 1, bmp.h - 1)}
    if len(corners) != 1:
        return None
    return corners.pop()


def stray_ground(bmp, want):
    """Border pixels of a ground this icon should no longer be carrying.

    The one shape this catches that `ground()` cannot: a partial re-grounding,
    where the corners are the new grey and a patch of the old one is still
    sitting on the edge somewhere between them.
    """
    edge = [(x, y) for x in range(bmp.w) for y in (0, bmp.h - 1)]
    edge += [(x, y) for y in range(bmp.h) for x in (0, bmp.w - 1)]
    return sum(1 for x, y in edge if bmp.get(x, y) == 15 and want != 15)


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


def knockout(bmp):
    """White the border cannot reach THROUGH WHITE: the fills the art must keep.

    The reachability has to be over white, not over the ground. Asking which
    white pixels the GROUND's region failed to reach answers nothing — the
    ground's region is all ground, so no white pixel is ever in it and the
    count comes back as every white pixel in the file. That is not a check on
    the knockouts; it is a check that the file contains the colour white.
    """
    _, seen = outside(bmp, 15)
    return sum(1 for y in range(bmp.h) for x in range(bmp.w)
               if bmp.get(x, y) == 15 and not seen[y][x])


# Every one of these icons is built round a white fill — the cloud body, the
# sun's disc, the circle behind the question mark — and the smallest of them
# covers a seventeenth of its file. A fiftieth is comfortably under that and
# comfortably over the speck a leaked fill or a stray edit leaves behind, and
# it is a fraction rather than a pixel count so it means the same at 34 px and
# at 61 px.
KNOCKOUT_SHARE = 50


def inspect(bmp, role, rel, pens, want):
    """Everything wrong with one icon, as sentences. Empty when it is right."""
    problems = []
    have = ground(bmp)
    if have is None:
        return ['%s: its four corners are not one colour, so it has no ground '
                'to check' % rel]
    if have != want[role]:
        return ['%s: ground is grey %d, and it is drawn in the %s, onto %s '
                '(grey %d) — an opaque blit has to carry the ground it lands '
                'on' % (rel, have, WHERE[role], pens[role], want[role])]
    if stray_ground(bmp, want[role]):
        problems.append('%s: %d pixel(s) of white still on its border, with '
                        'the corners already grey %d — it has been re-grounded '
                        'in part' % (rel, stray_ground(bmp, want[role]),
                                     want[role]))
    # The art, on a ground that is already right: the enclosed whites are what
    # stop the sun's rays showing through the cloud in front of them.
    if want[role] != 15:
        least = bmp.w * bmp.h // KNOCKOUT_SHARE
        kept = knockout(bmp)
        if kept < least:
            problems.append('%s: %d px of enclosed white, and this icon should '
                            'carry at least %d — the fills that hide what '
                            'passes behind the cloud have been flattened into '
                            'the ground' % (rel, kept, least))
    return problems


def reground(bmp, want):
    """Move an icon's ground to `want`, or say why it cannot be moved.

    Returns the reason as a sentence, or None when it worked. NOTHING IS
    WRITTEN by this: the fill is a flood from the border, and a flood fill is
    one anti-aliased break in a 1.3 px stroke away from escaping into the
    cloud body and flattening it. The caller checks the return before it
    touches the file — a repair tool that destroys the artwork and then
    reports that the artwork is missing is worse than no repair tool.
    """
    have = ground(bmp)
    if have is None:
        return 'its four corners are not one colour, so there is no ground to move'
    if have == want:
        return None
    for x, y in outside(bmp, have)[0]:
        bmp.set(x, y, want)
    if want != 15:
        least = bmp.w * bmp.h // KNOCKOUT_SHARE
        kept = knockout(bmp)
        if kept < least:
            return ('the fill escaped into the artwork — %d px of enclosed '
                    'white left where there should be at least %d, so nothing '
                    'was written' % (kept, least))
    return None


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

    THE PRISTINE ICON IS CHECKED FIRST, and each bend has to change a pixel.
    Without both, this proves nothing on a corpus that is already wrong: the
    first draft seeded its flood fill from pixels already equal to the ground
    it wanted, so against white-grounded icons it filled nothing, inspect()
    fired on the defect that was already there, and the run reported 110 bends
    all caught while the plain check was failing every one of the 22 files.
    A self-test that cannot tell "my bend was caught" from "this file was
    already broken" would go on passing after a refactor that broke ground()
    outright — which is the one thing it exists to prevent.

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

    def bend(rel, bmp, before, what):
        """Require the bend to have changed something, and to be caught."""
        nonlocal bends
        bends += 1
        if bmp.raw == before:
            bad.append('%s: the %s bend changed no pixel, so whatever it '
                       'reported was already true of the file' % (rel, what))
        elif not inspect(bmp, role, rel, pens, want):
            bad.append('%s: %s went unnoticed' % (rel, what))

    for rel, path, role, _ in each_icon():
        if role is None:
            continue
        n += 1

        # Nothing below means anything unless the file starts out right.
        pristine = Bmp4(path)
        was = inspect(pristine, role, rel, pens, want)
        if was:
            bad.append('%s: already fails the plain check, so no bend below '
                       'can be said to have been caught by it (%s)'
                       % (rel, was[0]))
            continue
        before = bytes(pristine.raw)

        # 1. The ground, moved to the one it is not drawn on — and then
        # moved back with reground(), which has to return the file it started
        # from, to the byte. THE ONLY PATH IN THIS FILE THAT WRITES, so it is
        # the one that most needs proving: a fill that escaped into the cloud
        # would come back a different file, and one that stopped short would
        # come back with the old ground still on it.
        bmp = Bmp4(path)
        wrong = 15 if want[role] != 15 else 14
        for x, y in outside(bmp, want[role])[0]:
            bmp.set(x, y, wrong)
        bent = bytes(bmp.raw)
        bend(rel, bmp, before, 'a ground of grey %d' % wrong)

        bends += 1
        rt = Bmp4(path, bent)
        was = Bmp4(path, bent)
        why = reground(rt, want[role])
        if why:
            bad.append('%s: reground() would not put the ground back — %s'
                       % (rel, why))
        else:
            # NOT a byte-for-byte round trip, and it cannot be: the bend above
            # paints the ground in a value some of the anti-aliased fringe
            # already holds, so flooding back over it legitimately swallows
            # those pixels too. What has to hold is the contract — every pixel
            # it rewrites was the ground it was moving, and became the ground
            # it was moving to. Anything else is a fill that reached into the
            # artwork, and this is the only path in the file that writes.
            wrote = 0
            for y in range(rt.h):
                for x in range(rt.w):
                    if rt.get(x, y) == was.get(x, y):
                        continue
                    wrote += 1
                    if was.get(x, y) != wrong or rt.get(x, y) != want[role]:
                        bad.append('%s: reground() turned a pixel of grey %d '
                                   'into grey %d, and it was moving grey %d to '
                                   'grey %d' % (rel, was.get(x, y),
                                                rt.get(x, y), wrong,
                                                want[role]))
                        break
                else:
                    continue
                break
            if wrote == 0:
                bad.append('%s: reground() wrote nothing over a ground it was '
                           'asked to move' % rel)

        # 2. One corner off is a ground this file cannot speak for.
        bmp = Bmp4(path)
        bmp.set(0, 0, 0 if bmp.get(0, 0) != 0 else 15)
        bend(rel, bmp, before, 'a corner that does not match the rest')

        if want[role] == 15:
            continue

        # 3. The knockouts, flattened — what a global swap of one grey for
        # another does to an icon, as against the flood fill --fix uses.
        bmp = Bmp4(path)
        for y in range(bmp.h):
            for x in range(bmp.w):
                if bmp.get(x, y) == 15:
                    bmp.set(x, y, want[role])
        bend(rel, bmp, before, 'its cloud fills flattened into the plate')

        # 4. And the same flattening with one white pixel left on the edge,
        # which is what passed the first draft: it counted every white pixel
        # in the file as a knockout, so one was enough to stand in for all of
        # them.
        bmp = Bmp4(path)
        for y in range(bmp.h):
            for x in range(bmp.w):
                if bmp.get(x, y) == 15:
                    bmp.set(x, y, want[role])
        bmp.set(bmp.w // 2, 0, 15)
        bend(rel, bmp, before, 'its fills flattened but for one edge pixel')

    for line in bad:
        print('check_kindle_icons: %s' % line, file=sys.stderr)
    if bad:
        return 1
    print('check_kindle_icons: self-test — %d bends across %d icons, every one '
          'of them a real change and every one of them caught' % (bends, n))
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
        if fix and ground(bmp) != want[role]:
            why = reground(bmp, want[role])
            if why:
                problems.append('%s: %s' % (rel, why))
                continue
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
