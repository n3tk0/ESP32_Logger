#!/usr/bin/env python3
"""
Hold the Kindle panel's layout to the browser page's design.

WHY THIS EXISTS
---------------
GET /kindle and kindle/update_dash.sh are ONE design rendered twice, from one
set of settings, by two pieces of code that cannot see each other: the page
gets its type sizes from a stylesheet emitted in C++, the panel gets its from
a .conf file of pixel coordinates. Nothing connects them, so they drift — and
every drift is invisible from both sides. The browser page looks right. The
panel looks right. Only somebody holding a photograph of one next to a
photograph of the other can see that the clock is eight pixels smaller, the
footer one, the hero four, and the second headline value a whole point of grey
too dark.

That is how the last set was found. This is so the next set is not.

WHAT IT CHECKS, AND WHAT IT DELIBERATELY DOES NOT
-------------------------------------------------
TYPE SIZES and the few widths the design fixes — the things that are the
design. Not positions: the page is a flow layout and the panel is absolute
coordinates, so "where the forecast starts" is legitimately a different number
on each and comparing them would be noise that trains people to ignore this.

The stylesheet is READ OUT OF THE FIRMWARE, the same way
tools/kindle_preview/preview.py reads it, rather than copied here. A checker
holding its own copy of the numbers is a checker that can pass while both the
page and the panel are wrong.

    python3 tools/check_kindle_parity.py
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CPP = os.path.join(ROOT, 'src/web/KindleDashboard.cpp')
SKIN = os.path.join(ROOT, 'src/web/KindleSkin.h')

# The two panels this firmware ships layouts for, and the KINDLE_PAGE_W each
# one's page is built at. kdPx() rounds the CSS pixel to the panel's scale.
PANELS = [('kindle/layout/600x800.conf', 600),
          ('kindle/layout/1072x1448.conf', 1072)]


def kdpx(n, page_w):
    """kdPx() from src/web/KindleDashboard.h, to the pixel."""
    return (n * page_w + 300) // 600 if n >= 0 else -((-n * page_w + 300) // 600)


def extract_css(path, start, end):
    """Replay the KD_S / KD_N calls between two markers into a string."""
    src = open(path, encoding='utf-8').read()
    blk = src[src.index(start):src.index(end)]
    blk = re.sub(r'/\*.*?\*/', '', blk, flags=re.S)
    blk = re.sub(r'//[^\n]*', '', blk)
    out = ''
    for m in re.finditer(r'KD_S\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)'
                         r'|KD_N\(\s*(-?\d+)\s*\)', blk):
        if m.group(1):
            for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
                out += lit.replace('\\"', '"').replace('\\\\', '\\')
        else:
            # Kept as the CSS pixel, not the scaled one: this checker compares
            # against a layout at a known panel width and scales it itself.
            out += '@%s@' % m.group(2)
    return out


def css_decl(sheet, selector, prop):
    """The value of one declaration, as the CSS pixel the firmware wrote.

    Numbers are carried as @N@ rather than as literal digits so that a
    declaration can be told apart from a length that happens to share its
    value, and so that a rule body still contains no braces to confuse this.
    """
    m = re.search(r'(?:^|[;}])' + re.escape(selector) + r'\{([^{}]*)\}', sheet)
    if not m:
        raise SystemExit('parity: no rule for %s — the stylesheet changed shape'
                         % selector)
    body = m.group(1)
    m = re.search(re.escape(prop) + r':@(-?\d+)@', body)
    if not m:
        raise SystemExit('parity: %s has no %s — the stylesheet changed shape'
                         % (selector, prop))
    return int(m.group(1))


def kskin_px(name):
    """One of kdSkinCss()'s clock-style sizes, out of KindleSkin.h."""
    src = open(SKIN, encoding='utf-8').read()
    arm = src[src.index('case %s:' % name):]
    arm = arm[:arm.index('break;')]
    m = re.search(r'"font-size:";\s*\n?\s*out \+= kdPx\((\d+)\)', arm) or \
        re.search(r'font-size:";\s*out \+= kdPx\((\d+)\)', arm)
    if not m:
        m = re.search(r'kdPx\((\d+)\);\s*\n\s*out \+= "px;line-height:', arm)
    if not m:
        raise SystemExit('parity: no font-size in %s — kdSkinCss() changed' % name)
    return int(m.group(1))


def load_conf(path):
    vals = {}
    for line in open(path, encoding='utf-8'):
        line = line.strip()
        if not line or line.startswith('#') or '=' not in line:
            continue
        k, v = line.split('=', 1)
        if v.strip().lstrip('-').isdigit():
            vals[k.strip()] = int(v.strip())
    return vals


def main():
    sheet = extract_css(CPP, '#define KD_N(n)   p += kdPx(n)', '#undef KD_S')
    if len(sheet) < 1500:
        raise SystemExit('parity: stylesheet extraction produced %d chars — '
                         'the emitter changed shape' % len(sheet))

    # ── The design, in CSS pixels, and where the panel keeps its copy ────────
    #
    # Left: what the browser page is told. Right: the layout key the FBInk
    # renderer draws with. Same number, two files, and until this existed
    # nothing said so.
    pairs = [
        ('.lab',        'font-size', ['GROUP_LAB_SZ', 'GRID_LAB_SZ']),
        ('.v1',         'font-size', ['HERO_SZ']),
        ('.v2',         'font-size', ['BIG_SZ']),
        ('.sub',        'font-size', ['SUB_SZ']),
        ('.gv',         'font-size', ['GRID_VAL_SZ']),
        ('.grid-3 .gv', 'font-size', ['GRID_VAL_SZ_3']),
        ('.iv',         'font-size', ['IN_VAL_SZ']),
        ('.iv-1',       'font-size', ['IN_VAL_SZ_1']),
        ('.clock',      'font-size', ['CL_SIZE']),
        ('.sec',        'font-size', ['LAB_SZ']),
        ('.fc',         'font-size', ['FC_TEXT_SZ']),
        ('.fc-t',       'font-size', ['FC_TEMP_SZ']),
        ('.per',        'width',     ['OL_PLATE_W']),
        ('.per-l',      'font-size', ['OL_LABEL_SZ']),
        ('.per-t',      'font-size', ['OL_TEMP_SZ']),
        ('.wd-n',       'font-size', ['WK_NAME_SZ']),
        ('.wd-d',       'font-size', ['WK_DAY_SZ']),
        ('.foot',       'font-size', ['FOOT_SZ']),
        ('.key',        'font-size', ['KEY_SZ']),
        ('.ax',         'font-size', ['AX_SZ']),
    ]

    # The runtime clock styles, whose CSS lives in kdSkinCss() rather than in
    # the sheet. Each claims to keep the clock block at the height the design
    # fixed, and the panel has to be set at the same size for that to hold.
    clock_pairs = [('KCLOCK_BOXED', 'CL_SZ_BOXED'),
                   ('KCLOCK_RULED', 'CL_SZ_RULED'),
                   ('KCLOCK_DATED', 'CL_SZ_DATED')]

    problems = []
    for rel, page_w in PANELS:
        conf = load_conf(os.path.join(ROOT, rel))
        for sel, prop, keys in pairs:
            want = kdpx(css_decl(sheet, sel, prop), page_w)
            for key in keys:
                if key not in conf:
                    problems.append('%s: %s is missing (the page sets %s{%s:%dpx})'
                                    % (rel, key, sel, prop, want))
                elif conf[key] != want:
                    problems.append('%s: %s=%d but the page sets %s{%s} to %d'
                                    % (rel, key, conf[key], sel, prop, want))
        for case, key in clock_pairs:
            want = kdpx(kskin_px(case), page_w)
            if conf.get(key) != want:
                problems.append('%s: %s=%s but kdSkinCss() sets %s to %d'
                                % (rel, key, conf.get(key), case, want))

        # The week strip spans the page's content width, seven cells across.
        pad = kdpx(18, page_w)
        cell = (page_w - 2 * pad + 6) // 7      # .wd is width:14.28%
        if conf.get('WK_X') != pad:
            problems.append('%s: WK_X=%s but the page indents its body %d'
                            % (rel, conf.get('WK_X'), pad))
        if abs(conf.get('WK_CELL_W', 0) - cell) > 1:
            problems.append('%s: WK_CELL_W=%s but seven cells across the '
                            'content width is %d'
                            % (rel, conf.get('WK_CELL_W'), cell))

    # The chart image's own size, which the layout has to reserve exactly:
    # short and the image is clipped, long and the axis labels land in space.
    # Read from ChartBmp::imageW/imageH in KindleChartBmp.h, which is where
    # both the renderer and /kindle/data now ask.
    bmp = open(os.path.join(ROOT, 'src/web/KindleChartBmp.h'), encoding='utf-8').read()
    m = re.search(r'imageW\(uint16_t panelW\)\s*\{\s*return \(panelW > 600\) '
                  r'\? (\d+) : (\d+); \}\s*\n'
                  r'\s*inline uint16_t imageH\(uint16_t panelW\)\s*\{\s*'
                  r'return \(panelW > 600\) \? (\d+)\s*: (\d+); \}', bmp)
    if not m:
        problems.append('parity: ChartBmp::imageW/imageH no longer state the BMP size')
    else:
        hiW, loW, hiH, loH = (int(g) for g in m.groups())
        for rel, (w, h) in (('kindle/layout/600x800.conf', (loW, loH)),
                            ('kindle/layout/1072x1448.conf', (hiW, hiH))):
            conf = load_conf(os.path.join(ROOT, rel))
            if conf.get('GR_W') != w or conf.get('GR_H') != h:
                problems.append('%s: GR_W/GR_H = %s/%s but the collector serves '
                                'a %dx%d image'
                                % (rel, conf.get('GR_W'), conf.get('GR_H'), w, h))

    # ── Every icon the collector can ask for exists on the panel ─────────────
    #
    # The page draws an SVG chosen by a range of WMO codes; the panel blits a
    # BMP named after a code. weatherIconCode() reduces the one to the other,
    # and the panel does no mapping at all — so every value it can return must
    # be a file that is there. It was not: nothing reduced the code before, so
    # a partly-cloudy afternoon (WMO 2) asked for fc_2_52.bmp, and the panel
    # drew the circled question mark that means "no forecast".
    # REPORTED, NEVER RAISED. This tool's contract is to print "FAIL: N
    # place(s)…" and exit 1; a reformatted signature or a moved generator must
    # come out as one of those lines, not as a traceback from str.index() or a
    # missing file — the guard immediately below shows the intent was always
    # to report shape changes rather than abort on them.
    def read(rel):
        try:
            with open(os.path.join(ROOT, rel), encoding='utf-8') as fh:
                return fh.read()
        except OSError as exc:
            problems.append('parity: cannot read %s (%s)' % (rel, exc))
            return ''

    fc = read('src/modules/ForecastModule.cpp')
    m = re.search(r'int\s+weatherIconCode\s*\(\s*int\s+\w+\s*\)\s*\{'
                  r'(.*?)\n\}', fc, re.S)
    want = sorted({int(g) for g in re.findall(r'return\s+(-?\d+);', m.group(1))}) \
        if m else []
    if not m:
        problems.append('parity: weatherIconCode() not found in '
                        'ForecastModule.cpp — the function changed shape')
    elif len(want) < 5:
        problems.append('parity: weatherIconCode() returned %d codes — the '
                        'function changed shape' % len(want))

    gen = read('scripts/generate_kindle_icons.py')
    m = re.search(r'RESOLUTIONS\s*=\s*\{(.*?)\}', gen, re.S)
    if not m:
        problems.append('parity: generate_kindle_icons.py no longer states its '
                        'RESOLUTIONS')
    else:
        for res, main_sz, ol_sz in re.findall(
                r'(\d+)\s*:\s*\((\d+),\s*(\d+)\)', m.group(1)):
            for code in want:
                for sz in (main_sz, ol_sz):
                    f = os.path.join(ROOT, 'kindle/icons', res,
                                     'fc_%d_%s.bmp' % (code, sz))
                    if not os.path.isfile(f):
                        problems.append(
                            'kindle/icons/%s/fc_%d_%s.bmp is missing, and '
                            'weatherIconCode() can return %d — the panel would '
                            'draw the question mark'
                            % (res, code, sz, code))

    if problems:
        print('FAIL: %d place(s) where the panel and the page disagree.\n'
              % len(problems))
        for p in problems:
            print('  ' + p)
        print('\nThey are one design rendered twice. A number that differs here '
              'is a\npanel that quietly stops matching the browser page.')
        return 1

    print('OK: %d type sizes and %d clock styles agree across %d panels.'
          % (len(pairs), len(clock_pairs), len(PANELS)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
