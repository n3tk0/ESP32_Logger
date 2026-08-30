#!/usr/bin/env python3
"""
generate_kindle_icons.py — Convert the ESP32_Logger weather SVG glyphs to
4-bit grayscale BMP files for the FBInk Kindle dashboard.

Produces 11 weather condition icons + 1 battery badge, in two sizes each,
for both 600×800 (7th Gen) and 1072×1448 (PW4) resolutions.

Requirements:
    pip install cairosvg Pillow

Usage:
    python scripts/generate_kindle_icons.py
    # Output lands in kindle/icons/600/ and kindle/icons/1072/
"""

import os
import io
import struct
from pathlib import Path
from PIL import Image

try:
    from PyQt5.QtGui import QImage, QPainter, QColor
    from PyQt5.QtSvg import QSvgRenderer
    from PyQt5.QtCore import QByteArray, QSize
    import sys
    from PyQt5.QtWidgets import QApplication
    # Need a QApplication instance to use QPainter
    if not QApplication.instance():
        app = QApplication(sys.argv)
except ImportError:
    print("Install dependencies: pip install PyQt5 Pillow")
    raise

# ---------------------------------------------------------------------------
# SVG primitives — matched exactly to KindleDashboard.cpp appendWeatherIcon()
# ---------------------------------------------------------------------------

_CLOUD = (
    '<path d="M20 45C11 45 11 32 20 31C20 19 37 17 40 27C50 25 54 39 45 45Z" '
    'fill="#fff"/>'
    '<path d="M20 45C11 45 11 32 20 31C20 19 37 17 40 27C50 25 54 39 45 45" '
    'fill="none"/>'
)

_SUN_FULL = (
    '<circle cx="32" cy="32" r="11" fill="#fff"/>'
    '<circle cx="32" cy="32" r="11" fill="none"/>'
    '<line x1="32" y1="14" x2="32" y2="8"/>'
    '<line x1="32" y1="56" x2="32" y2="50"/>'
    '<line x1="14" y1="32" x2="8" y2="32"/>'
    '<line x1="56" y1="32" x2="50" y2="32"/>'
    '<line x1="19.3" y1="19.3" x2="14.9" y2="14.9"/>'
    '<line x1="49.1" y1="49.1" x2="44.7" y2="44.7"/>'
    '<line x1="19.3" y1="44.7" x2="14.9" y2="49.1"/>'
    '<line x1="49.1" y1="14.9" x2="44.7" y2="19.3"/>'
)

_SUN_SMALL = (
    '<circle cx="22" cy="22" r="7" fill="#fff"/>'
    '<circle cx="22" cy="22" r="7" fill="none"/>'
    '<line x1="22" y1="10" x2="22" y2="6"/>'
    '<line x1="22" y1="38" x2="22" y2="34"/>'
    '<line x1="10" y1="22" x2="6" y2="22"/>'
    '<line x1="38" y1="22" x2="34" y2="22"/>'
    '<line x1="13.5" y1="13.5" x2="10.7" y2="10.7"/>'
    '<line x1="33.3" y1="33.3" x2="30.5" y2="30.5"/>'
    '<line x1="13.5" y1="30.5" x2="10.7" y2="33.3"/>'
    '<line x1="33.3" y1="10.7" x2="30.5" y2="13.5"/>'
)

def _slant(x, length):
    """Angled rain line."""
    return f'<line x1="{x}" y1="50" x2="{x - length/2}" y2="{50 + length}"/>'

def _flake(cx):
    """6-pointed snowflake."""
    cy = 54
    r = 5
    lines = ""
    for angle_deg in [0, 60, 120]:
        import math
        rad = math.radians(angle_deg)
        dx = r * math.cos(rad)
        dy = r * math.sin(rad)
        lines += f'<line x1="{cx-dx:.1f}" y1="{cy-dy:.1f}" x2="{cx+dx:.1f}" y2="{cy+dy:.1f}"/>'
    return lines

_LIGHTNING = (
    '<path d="M34 48L26 58h7l-3 8 10-12h-7l4-6z" fill="#fff" '
    'stroke="none"/>'
    '<path d="M34 48L26 58h7l-3 8 10-12h-7l4-6z" fill="none"/>'
)

# ---------------------------------------------------------------------------
# WMO condition code → SVG elements
# ---------------------------------------------------------------------------

ICONS = {
    0:   ("clear",        _SUN_FULL),
    1:   ("partly",       _SUN_SMALL + _CLOUD),
    3:   ("overcast",     _CLOUD),
    45:  ("fog",          _CLOUD +
          '<line x1="14" y1="51" x2="50" y2="51"/>'
          '<line x1="14" y1="56" x2="50" y2="56"/>'),
    51:  ("drizzle",      _CLOUD + _slant(22, 5) + _slant(33, 5) + _slant(44, 5)),
    61:  ("rain",         _CLOUD + _slant(22, 10) + _slant(33, 10) + _slant(44, 10)),
    71:  ("snow",         _CLOUD + _flake(22) + _flake(35) + _flake(48)),
    80:  ("showers",      _CLOUD + _slant(26, 10) + _slant(37, 10)),
    85:  ("snowshowers",  _CLOUD + _slant(24, 10) + _flake(40)),
    95:  ("thunder",      _CLOUD + _LIGHTNING),
    -1:  ("unknown",
          '<circle cx="32" cy="32" r="14" fill="#fff"/>'
          '<circle cx="32" cy="32" r="14" fill="none"/>'
          '<text x="32" y="39" text-anchor="middle" font-family="Georgia" '
          'font-size="22" fill="#000" stroke="none">?</text>'),
}

# Battery warning badge (from appendBatteryBadge)
BATTERY_SVG_INNER = (
    '<g stroke="none">'
    '<rect x="0" y="0" width="46" height="22" rx="3" fill="#000"/>'
    '<rect x="7" y="6" width="24" height="10" rx="1" fill="#fff"/>'
    '<rect x="9" y="8" width="20" height="6" fill="#000"/>'
    '<rect x="31" y="9" width="3" height="4" fill="#fff"/>'
    '<rect x="38" y="5" width="3" height="8" fill="#fff"/>'
    '<rect x="38" y="15" width="3" height="3" fill="#fff"/>'
    '</g>'
)

# ---------------------------------------------------------------------------
# Resolution configs: { res_w: [(main_icon_size, outlook_icon_size)] }
# ---------------------------------------------------------------------------
RESOLUTIONS = {
    600:  (52, 34),
    1072: (93, 61),
}


def wrap_svg(inner: str, viewbox: str = "0 0 64 64",
             width: int = 64, height: int = 64) -> str:
    """Wrap SVG elements in a full SVG document."""
    return (
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{width}" height="{height}" viewBox="{viewbox}" '
        f'stroke="#000" stroke-width="2.4" stroke-linecap="round" '
        f'stroke-linejoin="round" fill="none">'
        f'{inner}</svg>'
    )


def render_svg_to_png_bytes(svg_str: str, target_width: int, target_height: int) -> bytes:
    renderer = QSvgRenderer(QByteArray(svg_str.encode("utf-8")))
    image = QImage(target_width, target_height, QImage.Format_ARGB32)
    image.fill(QColor("white"))
    painter = QPainter(image)
    renderer.render(painter)
    painter.end()

    # Convert to PNG bytes
    byte_array = QByteArray()
    import PyQt5.QtCore as QtCore
    buffer = QtCore.QBuffer(byte_array)
    buffer.open(QtCore.QIODevice.WriteOnly)
    image.save(buffer, "PNG")
    return byte_array.data()

def svg_to_greyscale_bmp(svg_str: str, size: int) -> bytes:
    """Render SVG to a 4-bit greyscale BMP at the given pixel size."""
    png_data = render_svg_to_png_bytes(svg_str, size, size)

    # Open with Pillow and convert to greyscale
    img = Image.open(io.BytesIO(png_data)).convert("L")  # 8-bit grey

    # Build 4-bit BMP manually
    w, h = img.size
    row_bytes = (w + 1) // 2  # 4-bit packed, padded to byte boundary
    # BMP rows must be padded to 4-byte boundary
    row_stride = (row_bytes + 3) & ~3

    # Palette: 16 greyscale entries (BGRA)
    palette = b""
    for i in range(16):
        v = i * 17  # 0, 17, 34, ..., 255
        palette += struct.pack("BBBB", v, v, v, 0)

    pixel_size = row_stride * h
    header_size = 14 + 40 + len(palette)
    file_size = header_size + pixel_size

    # BITMAPFILEHEADER
    bfh = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, header_size)

    # BITMAPINFOHEADER
    bih = struct.pack("<IiiHHIIiiII",
                      40,       # header size
                      w, h,     # dimensions (positive = bottom-up)
                      1,        # planes
                      4,        # bits per pixel
                      0,        # compression (none)
                      pixel_size,
                      0, 0,     # pixels per meter
                      16,       # colors used
                      16)       # colors important

    # Pixel data (bottom-up)
    pixels = bytearray(pixel_size)
    for y in range(h):
        src_y = h - 1 - y  # BMP is bottom-up
        row_offset = y * row_stride
        for x in range(w):
            grey = img.getpixel((x, src_y))
            idx = grey >> 4  # 8-bit → 4-bit
            byte_pos = row_offset + x // 2
            if x % 2 == 0:
                pixels[byte_pos] = (idx << 4) | (pixels[byte_pos] & 0x0F)
            else:
                pixels[byte_pos] = (pixels[byte_pos] & 0xF0) | idx

    return bfh + bih + palette + bytes(pixels)


def generate_all():
    """Generate all icon BMP files."""
    project_root = Path(__file__).parent.parent
    kindle_dir = project_root / "kindle" / "icons"

    for res_w, (main_sz, outlook_sz) in RESOLUTIONS.items():
        out_dir = kindle_dir / str(res_w)
        out_dir.mkdir(parents=True, exist_ok=True)

        for code, (name, svg_inner) in ICONS.items():
            svg_doc = wrap_svg(svg_inner)

            for sz in (main_sz, outlook_sz):
                bmp = svg_to_greyscale_bmp(svg_doc, sz)
                fname = f"fc_{code}_{sz}.bmp"
                path = out_dir / fname
                path.write_bytes(bmp)
                print(f"  {path.relative_to(project_root)} ({len(bmp)} bytes)")

        # Battery badge (different viewbox: 46×22)
        batt_svg = wrap_svg(BATTERY_SVG_INNER, viewbox="0 0 46 22",
                            width=46, height=22)
        # Use kdPx logic: (px * res_w / 600)
        batt_w = int(46 * res_w / 600)
        batt_h = int(22 * res_w / 600)
        batt_png = render_svg_to_png_bytes(batt_svg, batt_w, batt_h)
        # Battery is not square, handle separately
        img = Image.open(io.BytesIO(batt_png)).convert("L")
        w, h = img.size
        row_bytes = (w + 1) // 2
        row_stride = (row_bytes + 3) & ~3
        palette = b""
        for i in range(16):
            v = i * 17
            palette += struct.pack("BBBB", v, v, v, 0)
        pixel_size = row_stride * h
        header_size = 14 + 40 + len(palette)
        file_size = header_size + pixel_size
        bfh = struct.pack("<2sIHHI", b"BM", file_size, 0, 0, header_size)
        bih = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 4, 0, pixel_size,
                          0, 0, 16, 16)
        pixels = bytearray(pixel_size)
        for y_bmp in range(h):
            src_y = h - 1 - y_bmp
            row_offset = y_bmp * row_stride
            for x in range(w):
                grey = img.getpixel((x, src_y))
                idx = grey >> 4
                byte_pos = row_offset + x // 2
                if x % 2 == 0:
                    pixels[byte_pos] = (idx << 4) | (pixels[byte_pos] & 0x0F)
                else:
                    pixels[byte_pos] = (pixels[byte_pos] & 0xF0) | idx
        batt_path = out_dir / "fc_batt.bmp"
        batt_path.write_bytes(bfh + bih + palette + bytes(pixels))
        print(f"  {batt_path.relative_to(project_root)} ({os.path.getsize(batt_path)} bytes)")

    print(f"\nDone. Icons in {kindle_dir.relative_to(project_root)}/")


if __name__ == "__main__":
    generate_all()
