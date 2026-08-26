# Stroke-only line art: e-ink has no fill tones worth using, and an outline
# survives dithering where a grey mass does not.
CLOUD = ('<path d="M20 45C11 45 11 32 20 31C20 19 37 17 40 27'
         'C50 25 54 39 45 45Z" fill="#fff"/>'
         '<path d="M20 45C11 45 11 32 20 31C20 19 37 17 40 27'
         'C50 25 54 39 45 45" fill="none"/>')
def sun(cx=32,cy=30,r=10,rays=True):
    s=f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="#fff"/>'
    if rays:
        import math
        for k in range(8):
            a=k*math.pi/4
            x1=cx+math.cos(a)*(r+4); y1=cy+math.sin(a)*(r+4)
            x2=cx+math.cos(a)*(r+9); y2=cy+math.sin(a)*(r+9)
            s+=f'<line x1="{x1:.0f}" y1="{y1:.0f}" x2="{x2:.0f}" y2="{y2:.0f}"/>'
    return s
def drops(n=3,slant=4,y0=50,y1=59,dx=0):
    return ''.join(f'<line x1="{22+i*11+dx}" y1="{y0}" x2="{22+i*11-slant+dx}" y2="{y1}"/>' for i in range(n))
def flakes(n=3,dx=0):
    # Six-pointed, not eight: at this size the extra pair of arms closes the
    # gaps and the flake renders as a filled blob.
    out=''
    for i in range(n):
        cx=24+i*12+dx; cy=54; r=4
        out+=(f'<line x1="{cx-r}" y1="{cy}" x2="{cx+r}" y2="{cy}"/>'
              f'<line x1="{cx-r*0.5:.0f}" y1="{cy-r*0.87:.0f}" x2="{cx+r*0.5:.0f}" y2="{cy+r*0.87:.0f}"/>'
              f'<line x1="{cx+r*0.5:.0f}" y1="{cy-r*0.87:.0f}" x2="{cx-r*0.5:.0f}" y2="{cy+r*0.87:.0f}"/>')
    return out

ICONS = {
 'clear'   : sun(32,32,11),
 'partly'  : sun(22,22,7,True)+CLOUD,
 'overcast': CLOUD,
 'fog'     : CLOUD+''.join(f'<line x1="16" y1="{51+i*5}" x2="48" y2="{51+i*5}"/>' for i in range(2)),
 'drizzle' : CLOUD+drops(3,2,50,55),
 'rain'    : CLOUD+drops(3,4,50,60),
 'snow'    : CLOUD+flakes(3),
 'showers' : CLOUD+drops(2,6,50,60,4),
 'snowshow': CLOUD+drops(1,6,50,60,-4)+flakes(1,10),
 'storm'   : CLOUD+'<path d="M34 48L26 58h7l-3 8 10-12h-7l4-6z" fill="#fff"/>',
 'unknown' : '<circle cx="32" cy="32" r="14" fill="#fff"/><text x="32" y="40" text-anchor="middle" font-size="22" font-family="Georgia,serif" stroke="none" fill="#000">?</text>',
}
