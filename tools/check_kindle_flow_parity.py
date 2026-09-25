#!/usr/bin/env python3
"""The settings page's copy of the layout rules, held to the collector's.

src/web/KindleFlow.h works out where everything on the e-ink page goes. The
settings page draws a preview of that page before anything is saved, so
www/js/kindle.js carries the same rules again (kdFlowCompute and the functions
it calls). Two copies of a calculation drift the first time one of them is
edited alone, and a preview that disagrees with the page is worse than none,
because it is believed.

This builds tools/kindle_preview/flow_dump from the header, runs the JS copy
under node, and compares them field by field over every combination of
sections, every count of places, and a spread of widths — and the worst-case
width of a list of readings, which is where the two could disagree on text.

  python3 tools/check_kindle_flow_parity.py              # the check
  python3 tools/check_kindle_flow_parity.py --self-test  # proves it can fail
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
JS = os.path.join(ROOT, 'www', 'js', 'kindle.js')
SRC = os.path.join(ROOT, 'tools', 'kindle_preview', 'flow_dump.cpp')
BEGIN = '// ── Where everything goes: src/web/KindleFlow.h, again'
END = '// Whether the page the preview draws has a forecast band.'

FIELDS = ['topBot', 'grow', 'labSz', 'heroSz', 'bigSz', 'headGap', 'slashW', 'subSz',
          'heroY', 'subY', 'gridRows', 'gridY', 'gridRowH', 'gridValSz', 'clSize',
          'clBoxed', 'clRuled', 'clRuledPad', 'clDated', 'clDateSz', 'clDateGap',
          'clGrow', 'clH', 'inRuleY', 'inLabY', 'inValY', 'inVal2Y', 'inValSz1',
          'inValSz', 'inW1Pm', 'inStack', 'sepH', 'chart', 'forecast', 'week',
          'rule2Y', 'grY', 'grH', 'rule3Y', 'wkRuleY']

PLACES = [
    ('temperature', '8.4', '°', 0), ('temperature', '-12.4', '°', 0),
    ('temperature', '21', '', 0), ('dew_point', '3.1', '°', 0),
    ('humidity', '71', '%', 0), ('humidity', '100', '%', 0),
    ('pressure', '1008', 'hPa', 1), ('pressure', '756', 'mmHg', 0),
    ('pressure', '29.77', 'inHg', 1), ('co2', '640', 'ppm', 0),
    ('co2', '12040', 'ppm', 0), ('aqi', '42', '', 0), ('lux', '1250', 'lx', 0),
    ('wind_speed', '4.2', 'm/s', 0), ('wind_direction', '270', '°', 0),
    ('uv_index', '3', '', 0), ('pm2_5', '12.5', 'µg/m³', 0),
    ('battery_percent', '9', '%', 0), ('rain', '0.0', 'mm', 0),
    ('temperature', '+5.0', '°', 0), ('voltage', '3.71', 'V', 0),
    ('temperature', '21.5', '°C', 0), ('temperature', '70', '°F', 0),
]

WIDTHS = [
    [3135, 2202, 2376, 1836, 1500, 2600],   # an ordinary page
    [3600, 3400, 3200, 3000, 2800, 2600],   # long units, big numbers
    [900, 1100, 1300, 1000, 1200, 1400],    # short ones
    [0, 5200, 700, 4400, 1000, 3000],       # mixed, with an unmeasured one
]


def build(tmp):
    exe = os.path.join(tmp, 'flow_dump')
    subprocess.check_call(['g++', '-std=gnu++17', '-O1', '-I', ROOT, SRC, '-o', exe])
    return exe


def js_engine(js_text):
    a = js_text.find(BEGIN)
    b = js_text.find(END)
    if a < 0 or b < 0 or b < a:
        sys.exit('check_kindle_flow_parity: cannot find the flow engine in www/js/kindle.js')
    return js_text[a:b]


def cases():
    for mask in range(16):
        for w in WIDTHS:
            for ng in range(7):
                for ni in range(4):
                    yield {
                        'chart': mask & 1, 'fc': (mask >> 1) & 1, 'week': (mask >> 2) & 1,
                        'sub': (mask >> 3) & 1, 'grid': w[:ng], 'in': w[:ni][::-1],
                    }


def run(js_text):
    with tempfile.TemporaryDirectory() as tmp:
        exe = build(tmp)
        all_cases = list(cases())
        spec = ';'.join('%s:%s:%s:%d' % p for p in PLACES)
        c_adv = [int(x) for x in subprocess.check_output([exe, 'adv', spec], text=True).split()]
        c_out = []
        for c in all_cases:
            args = ['json', 'chart=%d' % c['chart'], 'fc=%d' % c['fc'],
                    'week=%d' % c['week'], 'sub=%d' % c['sub'],
                    'grid=' + ','.join(map(str, c['grid'])),
                    'in=' + ','.join(map(str, c['in']))]
            c_out.append(json.loads(subprocess.check_output([exe] + args, text=True)))

        driver = js_engine(js_text) + '''
var input = JSON.parse(require("fs").readFileSync(0, "utf8"));
var out = { adv: input.places.map(function (p) {
              return kdFlowWorstAdvance(p[0], p[1], p[2], !!p[3]); }),
            flows: input.cases.map(function (c) {
              return kdFlowCompute({ chart: !!c.chart, forecast: !!c.fc, week: !!c.week,
                                     sub: !!c.sub, nGrid: c.grid.length, gridAdv: c.grid,
                                     nIn: c.in.length, inAdv: c.in }); }) };
process.stdout.write(JSON.stringify(out));
'''
        js_file = os.path.join(tmp, 'flow.js')
        with open(js_file, 'w', encoding='utf-8') as f:
            f.write(driver)
        js = json.loads(subprocess.check_output(
            ['node', js_file], text=True,
            input=json.dumps({'places': PLACES, 'cases': all_cases})))

    bad = []
    for p, a, b in zip(PLACES, c_adv, js['adv']):
        if a != b:
            bad.append('worst width of %s %r %r: C %d, JS %d' % (p[0], p[1], p[2], a, b))
    for c, a, b in zip(all_cases, c_out, js['flows']):
        for k in FIELDS:
            if a[k] != b.get(k):
                bad.append('%s: %s is %r in C, %r in JS' % (
                    json.dumps(c, separators=(',', ':')), k, a[k], b.get(k)))
                break
    return len(all_cases), bad


def main():
    js_text = open(JS, encoding='utf-8').read()
    if '--self-test' in sys.argv:
        # A rule edited on one side only has to be caught: the headline's
        # growth cap, and the caption width the indoor row keeps.
        for old, new in (('GROW_MAX:1180', 'GROW_MAX:1190'), ('IN_CAP_W:66', 'IN_CAP_W:60'),
                         ('w = c === 0xB0 ? 330', 'w = c === 0xB0 ? 340')):
            if old not in js_text:
                sys.exit('self-test: %r is not in kindle.js any more' % old)
            _, bad = run(js_text.replace(old, new, 1))
            if not bad:
                sys.exit('self-test: FAILED to notice %s -> %s' % (old, new))
            print('self-test: noticed %s -> %s (%s)' % (old, new, bad[0]))
        print('self-test: OK')
        return
    n, bad = run(js_text)
    if bad:
        print('check_kindle_flow_parity: the preview and the collector disagree:')
        for b in bad[:20]:
            print('  ' + b)
        if len(bad) > 20:
            print('  ... and %d more' % (len(bad) - 20))
        sys.exit(1)
    print('check_kindle_flow_parity: OK (%d layouts, %d readings agree)' % (n, len(PLACES)))


if __name__ == '__main__':
    main()
