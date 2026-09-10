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


def css_layout_pairs():
    """The design, in CSS pixels, and where the panel keeps its copy.

    Left: what the browser page is told. Right: the layout key the FBInk
    renderer draws with. Same number, two files, and until this existed nothing
    said so.
    """
    return [
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


def compare(sheet, confs=None):
    """How many type sizes disagree, for a given sheet and set of layouts.

    `confs` maps a layout path to an already-loaded dict, so the self-test can
    hand in a bent copy; anything not named is read from disk.
    """
    bad = 0
    for rel, page_w in PANELS:
        conf = (confs or {}).get(rel)
        if conf is None:
            conf = load_conf(os.path.join(ROOT, rel))
        for sel, prop, keys in css_layout_pairs():
            want = kdpx(css_decl(sheet, sel, prop), page_w)
            for key in keys:
                if conf.get(key) != want:
                    bad += 1
    return bad


def bend(sheet, sel, prop, by=3):
    """The stylesheet with one declaration moved, or the original if it could
    not be found — which is itself something the self-test reports."""
    pat = (r'((?:^|[;}])' + re.escape(sel) + r'\{[^{}]*?'
           + re.escape(prop) + r':)@(-?\d+)@')
    return re.sub(pat, lambda m: '%s@%d@' % (m.group(1), int(m.group(2)) + by),
                  sheet, count=1)


def self_test():
    """Prove this checker fails when the two ends drift, without editing them.

    CI USED TO DO THIS WITH A sed. It rewrote one font-size in
    KindleDashboard.cpp, ran main(), and expected a failure — which meant a
    regex in a YAML file that had to match the C++ emitter's exact layout, down
    to the run of spaces between `KD_S(".wd-d{font-size:");` and its `KD_N`.
    Two copies of the emitter's shape, one of them in a file nothing compiles.
    It went stale the first time a size moved and reported the drift as
    "check_kindle_parity.py passed a stylesheet the layout does not follow",
    which is a lie about the checker and sends the reader to the wrong file.

    The mutation belongs here, where the numbers are already parsed: bend one
    of them in memory and assert the comparison notices. Nothing on disk is
    touched, so there is no restore to get wrong and no shape to match.
    """
    sheet = extract_css(CPP, '#define KD_N(n)   p += kdPx(n)', '#undef KD_S')
    fails = []

    # Both directions, because the drift can start at either end: a layout
    # number edited without the stylesheet, or a stylesheet edited without the
    # layout. Every pair is exercised, not one hand-picked rule.
    for sel, prop, keys in css_layout_pairs():
        css = css_decl(sheet, sel, prop)
        for rel, page_w in PANELS:
            conf = load_conf(os.path.join(ROOT, rel))
            want = kdpx(css, page_w)
            for key in keys:
                if conf.get(key) != want:
                    fails.append('%s %s{%s} and %s already disagree'
                                 % (rel, sel, prop, key))
                # The layout drifting away from the page...
                if compare(sheet, {rel: dict(conf, **{key: want + 3})}) == 0:
                    fails.append('a %s three px off %s{%s} was not noticed'
                                 % (key, sel, prop))
        # ...and the page drifting away from the layout.
        bent = bend(sheet, sel, prop)
        if bent == sheet:
            fails.append('could not bend %s{%s} — the sheet changed shape'
                         % (sel, prop))
        elif compare(bent, None) == 0:
            fails.append('a stylesheet three px off every layout was not noticed'
                         ' at %s{%s}' % (sel, prop))

    if fails:
        print('SELF-TEST FAIL: %d case(s) this checker would have let through.\n'
              % len(fails))
        for f in fails:
            print('  ' + f)
        return 1
    print('OK: self-test — the checker notices a drift at either end of all %d '
          'type sizes.' % len(css_layout_pairs()))
    return 0


def main():
    sheet = extract_css(CPP, '#define KD_N(n)   p += kdPx(n)', '#undef KD_S')
    if len(sheet) < 1500:
        raise SystemExit('parity: stylesheet extraction produced %d chars — '
                         'the emitter changed shape' % len(sheet))

    pairs = css_layout_pairs()

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
        mw = re.search(r'CHART_W = kdPx\((\d+)\);', open(CPP, encoding='utf-8').read())
        loW_design = int(mw.group(1)) if mw else None
        for rel, (w, h) in (('kindle/layout/600x800.conf', (loW, loH)),
                            ('kindle/layout/1072x1448.conf', (hiW, hiH))):
            conf = load_conf(os.path.join(ROOT, rel))
            if conf.get('GR_W') != w or conf.get('GR_H') != h:
                problems.append('%s: GR_W/GR_H = %s/%s but the collector serves '
                                'a %dx%d image'
                                % (rel, conf.get('GR_W'), conf.get('GR_H'), w, h))

    # ── And the plot area inside it, which is the same design twice again ────
    #
    # The page draws its chart as an SVG whose margins are flat kdPx() values;
    # the panel blits a BMP whose margins ChartBmp scales from the design's own
    # dimensions. Same picture, so the same margins — and the only thing making
    # that true is that ChartBmp divides by the number CHART_H is built from.
    #
    # IT STOPPED BEING TRUE THE DAY THE CHART GREW. imageH went 200 to 220 and
    # the vertical pair kept dividing by 200, so the panel plotted a 24 h curve
    # into 181 px where the page used 184, one pixel lower and a percent and a
    # half shorter. Nothing said so: the size check above compares GR_W/GR_H,
    # which were both right.
    m = re.search(r'CHART_H = kdPx\((\d+)\);', open(CPP, encoding='utf-8').read())
    design_h = int(m.group(1)) if m else None
    if design_h is None:
        problems.append('parity: no CHART_H = kdPx(N) in KindleDashboard.cpp — '
                        'the page no longer states its chart height')

    svg = re.search(r'const int L = kdPx\((\d+)\), R = CHART_W - kdPx\((\d+)\), '
                    r'T = kdPx\((\d+)\), B = CHART_H - kdPx\((\d+)\);',
                    open(CPP, encoding='utf-8').read())
    mar = re.findall(r'inline int margin([LRTB])\(uint16_t \w\) '
                     r'\{ return (?:\w - )?\w \* (\d+) / (\d+); \}', bmp)
    if not svg or len(mar) != 4:
        problems.append('parity: the chart margins are no longer stated as '
                        'kdPx() on the page and marginL/R/T/B on the panel')
    else:
        want = dict(zip('LRTB', (int(g) for g in svg.groups())))
        # The horizontal pair scales by the design's width, the vertical by its
        # height. Both are the number the page builds its own box from.
        design = {'L': loW_design, 'R': loW_design, 'T': design_h, 'B': design_h}
        for side, num, den in mar:
            if int(num) != want[side]:
                problems.append('parity: margin%s insets %s px but the page '
                                'insets its SVG by %d' % (side, num, want[side]))
            if design[side] is not None and int(den) != design[side]:
                problems.append('parity: margin%s divides by %s but the design '
                                'it scales from is %d' % (side, den, design[side]))

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
    if '--self-test' in sys.argv:
        sys.exit(self_test())
    sys.exit(main())
