#!/usr/bin/env python3
"""
Render docs/images/kindle-dashboard.png without a device or a running board.

    python3 tools/kindle_preview/preview.py [hourly|daily] [en|bg] [calm|cold] [width]
    node    tools/kindle_preview/shot.mjs            # -> kindle.png, prints the height

Writes preview.html beside itself. See docs/KINDLE_DASHBOARD.md for what the
picture is and is not evidence of: the stylesheet is read out of the firmware,
the markup is this script's own.
"""
import os, re, math, random, sys
random.seed(7)

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
exec(open(os.path.join(HERE, 'icons.py')).read())
mode = sys.argv[1] if len(sys.argv)>1 else 'hourly'
lang = sys.argv[2] if len(sys.argv)>2 else 'en'
scen = sys.argv[3] if len(sys.argv)>3 else 'calm'

PAGE_W = int(sys.argv[4]) if len(sys.argv) > 4 else 600
# 5th argument: draw the low-battery badge. Off by default, because the page
# it is meant to represent usually has no warning on it and a preview that
# always shows one would misrepresent the common case.
WARN = len(sys.argv) > 5 and sys.argv[5] in ("warn", "1", "true")
def kdpx(n):
    return (n * PAGE_W + 300) // 600 if n >= 0 else -((-n * PAGE_W + 300) // 600)

# The stylesheet is reconstructed from the KD_S/KD_N calls in the firmware, not
# copied. That is the whole point of this script: a preview built from its own
# copy of the CSS is a preview that can be right while the device is wrong,
# which has already happened twice on this branch.
src = open(os.path.join(ROOT, 'src/web/KindleDashboard.cpp')).read()
i0 = src.index('#define KD_N(n)   p += kdPx(n)')
i1 = src.index('#undef KD_S')
blk = src[i0:i1]
blk = re.sub(r'/\*.*?\*/', '', blk, flags=re.S)
blk = re.sub(r'//[^\n]*', '', blk)

style = ''
for m in re.finditer(r'KD_S\(\s*((?:"(?:[^"\\]|\\.)*"\s*)+)\)|KD_N\(\s*(-?\d+)\s*\)', blk):
    if m.group(1):
        for lit in re.findall(r'"((?:[^"\\]|\\.)*)"', m.group(1)):
            style += lit.replace('\\"', '"').replace('\\\\', '\\')
    else:
        style += str(kdpx(int(m.group(2))))
if len(style) < 1500:
    raise SystemExit('stylesheet extraction produced %d chars — the emitter shape changed' % len(style))

# ── The clock style, also extracted rather than copied ──────────────────────
# config.kindle.clockStyle is a runtime setting, and its CSS is emitted by
# kdSkinCss() in src/web/KindleSkin.h as overrides appended after the sheet
# above. Each style claims to keep the block's total height at the 139 px the
# design fixed — which is what keeps the hairline under the clock level with
# the outdoor column — and that claim is only worth anything if it is measured.
# So the arm is pulled out of the firmware and appended here the same way the
# device would append it, and shot.mjs prints the resulting page height.
CLOCK = sys.argv[6] if len(sys.argv) > 6 else 'plain'
if CLOCK != 'plain':
    arm = {'boxed': 'KCLOCK_BOXED', 'ruled': 'KCLOCK_RULED', 'dated': 'KCLOCK_DATED'}
    if CLOCK not in arm:
        raise SystemExit('unknown clock style %r (plain|boxed|ruled|dated)' % CLOCK)
    skin_src = open(os.path.join(ROOT, 'src/web/KindleSkin.h')).read()
    j0 = skin_src.index('case ' + arm[CLOCK] + ':')
    j1 = skin_src.index('break;', j0)
    blk = re.sub(r'//[^\n]*', '', skin_src[j0:j1])
    for m in re.finditer(r'out \+= "((?:[^"\\]|\\.)*)"|out \+= kdPx\(\s*(-?\d+)\s*\)', blk):
        style += m.group(1).replace('\\"', '"') if m.group(1) else str(kdpx(int(m.group(2))))


S = {
 'en': dict(place='Balcony', when='Tuesday 25 November &middot; 17:40', out='Outside', ins='Inside',
   to=' to ', today='&deg; today', hum='% humidity', fall='falling', h24='Last 24 hours',
   mean='outside mean', band=', shaded band = hourly low to high', inl='inside', now='now',
   fc='Forecast', cond='Showers', wind='wind 23 km/h', age='18 min old',
   foot2='Measured on site', refresh='refresh', clear='clear',
   mon='November', wd=['Mon','Tue','Wed','Thu','Fri','Sat','Sun'], cols3=['Wed','Thu','Fri']),
 'bg': dict(place='Балкон', when='вторник 25 ноември &middot; 17:40', out='Навън', ins='Вътре',
   to=' до ', today='&deg; днес', hum='% влажност', fall='пада', h24='Последните 24 часа',
   mean='средно навън', band=', сивото е час. мин–макс', inl='вътре', now='сега',
   fc='Прогноза', cond='Превалявания', wind='вятър 23 км/ч', age='18 мин',
   foot2='Измерено на място', refresh='обнови', clear='изчисти',
   mon='ноември', wd=['пн','вт','ср','чт','пт','сб','нд'], cols3=['ср','чт','пт']),
}[lang]

W,H = kdpx(560), kdpx(200)
L,R = kdpx(40), W - kdpx(4)
T,B = kdpx(10), H - kdpx(26)
HOURS=24
if scen=='cold':
    HERO=dict(cls='big big4', now='-12.4', lo='-14.2', hi='11.4', hum='100', hpa='1027',
              arrow='&#8599;', trend=('rising' if lang=='en' else 'расте'),
              d='+3.6 hPa/3h', cond=('Snow showers' if lang=='en' else 'Снеговалеж'),
              wind=('wind 47 km/h' if lang=='en' else 'вятър 47 км/ч'),
              ft='-3&deg; / -9&deg;', ic='snowshow')
else:
    HERO=dict(cls='big', now='8.4', lo='-2.4', hi='15.3', hum='71', hpa='1008',
              arrow='&#8600;', trend=S['fall'], d='-1.2 hPa/3h',
              cond=S['cond'], wind=S['wind'], ft='14&deg; / 3&deg;', ic='showers')
out=[]
if scen=='cold':
    # -23h..-11h calm autumn evening/day, then a cold front crosses at -10h:
    # the mean falls ~15 K in six hours and the hourly min-max spread widens
    # from ~1 K to ~5 K because gusts keep swapping the air over the sensor.
    for i in range(HOURS):
        if i < 13:
            base = 9.5 + 1.6*math.sin(i/3.0); spread = 0.55
        elif i < 19:
            f = (i-12)/6.0
            base = 9.5 - 15.0*f; spread = 0.55 + 1.9*f
        else:
            base = -5.4 + 0.8*math.sin(i/1.7); spread = 2.4
        j = random.random()*0.4
        out.append((base-spread-j, base+spread+j, base))
else:
    for i in range(HOURS):
        hod=(i+11)%24; base=6.5-7.5*math.cos((hod-3)/24*2*math.pi)
        out.append((base-1.1-random.random()*0.5, base+1.1+random.random()*0.5, base))
inn=[(20.6,21.4,21.0+0.25*math.sin(i/3)) for i in range(HOURS)]
if scen!='cold':
    for k in (9,10): out[k]=None
lo=min(min(o[0] for o in out if o), min(i[0] for i in inn)); hi=max(max(o[1] for o in out if o), max(i[1] for i in inn))
pad=max((hi-lo)*0.06,0.4); lo-=pad; hi+=pad; span=hi-lo
X=lambda i:L+int((R-L)/(HOURS-1)*i); Y=lambda v:T+int((hi-v)/span*(B-T))
g=['<svg class="chart" width="%d" height="%d" viewBox="0 0 %d %d">'%(W,H,W,H)]
for i in list(range(0,HOURS,3))+[HOURS-1]:
    g.append('<line class="vgrid" x1="%d" y1="%d" x2="%d" y2="%d"/>'%(X(i),T,X(i),B))
for k in range(5):
    v=hi-span*k/4; y=T+int((B-T)*k/4)
    g.append('<line class="%s" x1="%d" y1="%d" x2="%d" y2="%d"/>'%('base' if k==4 else 'grid',L,y,R,y))
    g.append('<text class="ax" x="%d" y="%d" text-anchor="end">%d</text>'%(L-kdpx(7),y+kdpx(4),round(v)))
i=0
while i<HOURS:
    if out[i] is None: i+=1; continue
    j=i
    while j+1<HOURS and out[j+1]: j+=1
    if j>i:
        d=''.join(('M' if k==i else 'L')+'%d %d '%(X(k),Y(out[k][1])) for k in range(i,j+1))
        d+=''.join('L%d %d '%(X(k),Y(out[k][0])) for k in range(j,i-1,-1))+'Z'
        g.append('<path class="band" d="%s"/>'%d)
    i=j+1
for cls,ser in (("l-out",out),("l-in",inn)):
    d='';pen=False
    for i in range(HOURS):
        if ser[i] is None: pen=False; continue
        d+=('L' if pen else 'M')+'%d %d '%(X(i),Y(ser[i][2])); pen=True
    g.append('<path class="%s" d="%s"/>'%(cls,d))
for i in range(0,HOURS,6):
    g.append('<text class="ax" x="%d" y="%d" text-anchor="middle">-%dh</text>'%(X(i),H-kdpx(8),HOURS-1-i))
g.append(('<text class="ax" x="%d" y="%d" text-anchor="end">'%(X(HOURS-1),H-kdpx(8)))+S['now']+'</text>')
g.append('</svg>')

def ico(name,px):
    return ('<svg viewBox="0 0 64 64" width="%d" height="%d" stroke="#000" stroke-width="2.4" '
            'stroke-linecap="round" stroke-linejoin="round" fill="none">%s</svg>'%(px,px,ICONS[name]))

if mode=='daily':
    cols=[(S['cols3'][0],'rain',14,4),(S['cols3'][1],'partly',16,6),(S['cols3'][2],'clear',18,7)]
else:
    cols=([('21:00','snowshow',-6,None),('00:00','snow',-8,None),('03:00','clear',-9,None)]
          if scen=='cold' else
          [('21:00','showers',6,None),('00:00','overcast',4,None),('03:00','clear',2,None)])
per=''
for lab,ic,t,lo2 in cols:
    extra='<span class="dim">/%d&deg;</span>'%lo2 if lo2 is not None else ''
    per+=('<td class="per"><div class="per-l">%s</div>%s<div class="per-t">%d&deg;%s</div></td>'
          %(lab,ico(ic,kdpx(34)),t,extra))

# The battery badge, replayed from appendBatteryBadge() in
# src/web/KindleDashboard.cpp. A COPY, like the rest of the markup here and
# unlike the stylesheet, which is extracted — see this directory's README for
# which half of this preview can drift and which cannot. The arithmetic is the
# same kdpx() the firmware uses, so at least the geometry cannot.
def battery_badge():
    w, h, r = kdpx(46), kdpx(22), kdpx(3)
    bx, by, bw, bh, t2 = kdpx(7), kdpx(6), kdpx(24), kdpx(10), kdpx(2)
    ex = kdpx(38)
    return (
        '<svg class="bw" width="%d" height="%d" viewBox="0 0 %d %d">' % (w, h, w, h) +
        '<rect x="0" y="0" width="%d" height="%d" rx="%d" fill="#000"/>' % (w, h, r) +
        '<rect x="%d" y="%d" width="%d" height="%d" fill="#fff"/>' % (bx, by, bw, bh) +
        '<rect x="%d" y="%d" width="%d" height="%d" fill="#000"/>'
            % (bx + t2, by + t2, bw - 2 * t2, bh - 2 * t2) +
        '<rect x="%d" y="%d" width="%d" height="%d" fill="#fff"/>'
            % (bx + bw, by + kdpx(3), kdpx(3), bh - kdpx(6)) +
        '<rect x="%d" y="%d" width="%d" height="%d" fill="#fff"/>'
            % (ex, kdpx(5), kdpx(3), kdpx(8)) +
        '<rect x="%d" y="%d" width="%d" height="%d" fill="#fff"/>'
            % (ex, kdpx(15), kdpx(3), kdpx(3)) +
        '</svg>')

wk=''
for i,(n,dnum) in enumerate(list(zip(S['wd'],[24,25,26,27,28,29,30]))):
    cls='wd wd-now' if i==1 else ('wd wd-we' if i>=5 else 'wd')
    wk+='<td class="%s"><div class="wd-n">%s</div><div class="wd-d">%d</div></td>'%(cls,n,dnum)

# ── The slot flow ───────────────────────────────────────────────────────────
# The page is a list of readings now, so the preview is too. These are the six
# the firmware defaults to, packed the way kdSlotsPack() packs them: a hero
# beside the clock on row 0, then the rest across full-width rows.
#
# The sizes and the twelfths are duplicated from src/web/KindleSlots.h rather
# than parsed out of it, and that is a deliberate limit of this tool: it draws
# what the layout SHOULD look like, and the host tests are what prove the
# firmware packs it that way.
# The widths are the ones kdSlotsPack() ends up with after a row shares itself
# equally among its slots: two readings on a full-width row are six twelfths
# each whatever their sizes, so the rows end flush and the columns line up.
SLOTS = [
    # label,           value,       unit,  size, units, row, bold, age
    (S['out'],         HERO['now'], '°',   'h',  6,     0,   True,  '3 ' + ('min old' if lang == 'en' else 'мин')),
    (('HUM' if lang == 'en' else 'ВЛАГА'), HERO['hum'], '%', 'm', 6, 1, False, ''),   # noqa: E501
    (('PRESS' if lang == 'en' else 'НАЛЯГ'), HERO['hpa'], 'hPa', 'm', 6, 1, False, ''),
    (S['ins'],         '21.0',      '°',   'l',  6,     2,   False, '1 ' + ('min old' if lang == 'en' else 'мин')),
    (('HUM' if lang == 'en' else 'ВЛАГА'), '44',  '%',   'm',  6,     2,   False, ''),
    ('AQI',            '42',        '',    'm',  12,    3,   False, ''),
]

def slot_cells(row):
    out = ''
    for i, (lab, val, unit, sz, units, r, bold, age) in enumerate(SLOTS):
        if r != row:
            continue
        # Only the hero captions itself on a line of its own; the rest set the
        # label inline with the value, which is what appendSlotRows() does and
        # is what keeps a row one line tall. The badge is the exception — it is
        # floated, so it needs a line to float within.
        badge = battery_badge() if (WARN and i == 0) else ''
        own = (sz == 'h') or bool(badge)
        out += ('<td class="slot sl-%s" width="%d%%">' % (sz, (units * 100 + 6) // 12) +
                ('<div class="lab">' + lab + badge + '</div>' if own else '') +
                '<div class="val' + (' val-b' if bold else '') + '">' +
                ('' if own else '<span class="lab-i">' + lab + '</span> ') + val +
                (unit_span(unit) if unit else '') +
                ('<span class="age"> &middot; ' + age + '</span>' if age else '') +
                '</div></td>')
    return out

def unit_span(unit):
    """Degrees and per-cent set tight against the number; everything else after
    a space. The same rule appendSlotRows() applies."""
    if unit == '°':
        return '<span class="unit unit-d">°</span>'
    if unit == '%':
        return '<span class="unit">%</span>'
    return '<span class="unit"> ' + unit + '</span>'

def slot_row(row):
    cells = slot_cells(row)
    return ('<table class="slots"><tr>' + cells + '</tr></table>') if cells else ''

body=('<table class="hero"><tr><td width="50%">'
 + slot_row(0) +
 '</td><td width="50%" class="sep">'
 '<div class="clock">17:40</div>'
 +('<div class="clock-d">25 %s</div>'%S['mon'] if CLOCK=='dated' else '')+
 '</td></tr></table>'
 + slot_row(1) + slot_row(2) + slot_row(3)
 + '<div class="rule"></div><div class="sec">'+S['h24']+'</div>'
 + '\n'.join(g) +
 '<table class="key"><tr><td>'
 +('<svg width="%d" height="%d"><line x1="0" y1="%d" x2="%d" y2="%d" stroke="#000" stroke-width="%d"/></svg> '%(kdpx(26),kdpx(9),kdpx(5),kdpx(26),kdpx(5),kdpx(3)))
 +S['mean']+'<span class="dim">'+S['band']+'</span>'
 '</td><td style="text-align:right">'
 +('<svg width="%d" height="%d"><line x1="0" y1="%d" x2="%d" y2="%d" stroke="#777" stroke-width="%d" stroke-dasharray="%d %d"/></svg> '%(kdpx(26),kdpx(9),kdpx(5),kdpx(26),kdpx(5),kdpx(2),kdpx(7),kdpx(5)))
 +S['inl']+'</td></tr></table>'
 '<div class="rule"></div><div class="sec">'+S['fc']+'</div>'
 '<table><tr><td width="%d" class="ico">'%kdpx(56)+ico(HERO['ic'],kdpx(52))+'</td>'
 '<td class="fc">'+HERO['cond']+'<div class="fc-t">'+HERO['ft']+'</div>'
 '<div class="sub">'+HERO['wind']+' &middot; <span class="dim">'+S['age']+'</span></div></td>'
 +per+'</tr></table>'
 '<div class="rule"></div><div class="sec sec-wk">'+S['mon']+'</div>'
 '<table class="wk"><tr>'+wk+'</tr></table>'
 +'<table class="foot"><tr><td>'+S['foot2']+'</td>'
  '<td class="act"><a href="/kindle">'+S['refresh']+'</a>'
  '<a href="/kindle/clear">'+S['clear']+'</a></td></tr></table>')
open(os.path.join(HERE, 'preview.html'),'w').write('<!DOCTYPE html><html><head><meta charset="UTF-8"><title>W</title><style>'+style+'</style></head><body>'+body+'</body></html>')
print('preview:',mode)
