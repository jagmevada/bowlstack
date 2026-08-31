"""Generate a VLW (anti-aliased, 8 bpp) font for LovyanGFX from a TTF.

Layout taken from LovyanGFX's own parser, lgfx_fonts.cpp VLWfont::loadFont --
not from memory. All integers are big-endian uint32.

  header, 24 bytes:   gCount, version, yAdvance, unused, ascent, descent
  glyphs, 28 each:    unicode, height, width, xAdvance, dY, dX, reserved
  bitmaps:            concatenated in glyph order, width*height bytes, 8 bpp
"""
import io
import struct
import sys
from PIL import ImageFont

TTF = sys.argv[1]
PX = int(sys.argv[2])
OUT = sys.argv[3]
SYMBOL = sys.argv[4]

font = ImageFont.truetype(TTF, PX)
ascent, descent = font.getmetrics()

glyphs = []
for cp in range(0x20, 0x7F):
    ch = chr(cp)
    advance = int(round(font.getlength(ch)))
    try:
        mask, offset = font.getmask2(ch, mode='L')
    except Exception:
        mask, offset = None, (0, 0)
    w, h = (mask.size if mask is not None else (0, 0))
    if w == 0 or h == 0:
        glyphs.append((cp, 0, 0, advance, 0, 0, b''))
        continue
    # offset is (dx, dy) from the layout box's top-left; the baseline sits at
    # y = ascent, so the glyph's top above the baseline is ascent - dy.
    dx, dy = offset
    bmp = bytes(mask)  # already 8-bit, row-major, w*h
    assert len(bmp) == w * h, (cp, len(bmp), w, h)
    glyphs.append((cp, h, w, advance, ascent - dy, dx, bmp))

buf = io.BytesIO()
buf.write(struct.pack('>6I', len(glyphs), 11, PX, 0, ascent, descent))
for cp, h, w, adv, dY, dX, _ in glyphs:
    buf.write(struct.pack('>7i', cp, h, w, adv, dY, dX, 0))
for _, _, _, _, _, _, bmp in glyphs:
    buf.write(bmp)

blob = buf.getvalue()

with io.open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    f.write('// GENERATED -- do not edit. See tools/make_vlw.py.\n')
    f.write('//\n')
    f.write('// %s at %d px, ASCII 0x20-0x7E, 8 bits per pixel.\n' % (TTF.split('/')[-1], PX))
    f.write('//\n')
    f.write('// A VLW font, which is the ONLY anti-aliased format LovyanGFX can draw --\n')
    f.write('// its built-in faces and its U8g2 faces are all one bit per pixel, and on a\n')
    f.write('// ~200 DPI panel a 1 bpp glyph reads as a stack of dots however well shaped\n')
    f.write('// it is. The splash is drawn before LVGL exists, so it cannot borrow the\n')
    f.write('// Montserrat that every screen after it uses; this is that same typeface,\n')
    f.write('// converted into the one format this renderer can antialias.\n')
    f.write('//\n')
    f.write('// %d glyphs, %d bytes.\n' % (len(glyphs), len(blob)))
    f.write('#pragma once\n\n#include <stdint.h>\n\n')
    f.write('const uint8_t %s[] = {\n' % SYMBOL)
    for i in range(0, len(blob), 16):
        f.write('    ' + ' '.join('0x%02X,' % b for b in blob[i:i + 16]) + '\n')
    f.write('};\n')

print('%s: %d glyphs, %d bytes, ascent %d descent %d'
      % (OUT.split('/')[-1], len(glyphs), len(blob), ascent, descent))
