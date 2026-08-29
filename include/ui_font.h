// Fonts generated for this project, as opposed to LVGL's built-ins.
//
// LVGL SHIPS MONTSERRAT UP TO 48 PX AND NO FURTHER. The dashboard's mass value
// was already at 48, so "make the number bigger" could not be answered by
// switching a #define in lv_conf.h -- there is nothing above it to switch to.
//
// Generated with lv_font_conv from the Montserrat-Medium.ttf that LVGL itself
// ships for regenerating its built-ins, so this is the same typeface at a size
// LVGL does not provide rather than a different one:
//
//     npx lv_font_conv --no-compress --format lvgl --bpp 4 --size 56 \
//       --lv-include lvgl.h \
//       --font .pio/libdeps/<env>/lvgl/scripts/built_in_font/Montserrat-Medium.ttf \
//       --symbols "0123456789.- " -o src/ui/font_mass_56.c
//
// SUBSET TO TWELVE GLYPHS on purpose. A full 56 px face is most of a hundred
// kilobytes; digits, a decimal point, a minus and a space are everything a mass
// reading can contain, and they come to about 7 KB. The cost of that decision is
// that this font must never be used for anything but the number -- a label that
// picks it up for a word renders as blanks. The unit, the caption and every
// other string on the page stay on the built-in Montserrat.
//
// WIDTHS, because they are what bounds the choice. At 56 px the widest digit
// advances 37.4 px and the minus 21.4, so the worst reading three decimals can
// produce -- "-19.999" -- is about 193 px. The label it lives in is 194 wide.
// That is deliberate rather than lucky: 64 px was the first size tried and the
// same string comes to 221 px there, which clips inside a 240 px panel. If the
// precision setting ever grows a fourth decimal, this font has to come back
// down a size, not the label go up in width.
//
// THE SWAP COST NO WIDTH AT ALL, WHICH IS NOT LUCK EITHER BUT IS SURPRISING.
// LVGL's shipped lv_font_montserrat_48 is NOT this TTF at 48 px -- generating
// the same subset at --size 48 from the file above gives a digit advance of
// 32.0 px against the built-in's 37.1. The shipped font is a wider cut.
//
// Which means going from the built-in 48 to this 56 changes the digits from
// 34 px tall / 37.1 px wide to 39 px tall / 37.4 px wide: FIFTEEN PER CENT
// TALLER AT THE SAME WIDTH. The number reads bigger without needing a wider
// label, which is the whole reason 56 fits at all.
//
// Do not "verify" a generated size by comparing its metrics against an LVGL
// built-in of a nearby size -- they are different cuts and the comparison says
// nothing. Compare against the same subset generated at a known size, which is
// how the scaling above was actually checked.

#pragma once

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

// Montserrat Medium, 56 px, 4 bpp. Digits, '.', '-' and space ONLY.
extern const lv_font_t font_mass_56;

#ifdef __cplusplus
}
#endif
