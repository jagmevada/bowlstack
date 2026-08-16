// LVGL configuration for the Waveshare touch build.
//
// DELIBERATELY MINIMAL. LVGL's lv_conf_internal.h supplies a default for every
// option it has, each wrapped in `#ifndef`, so anything absent here is not
// missing -- it is the upstream default, and it stays the upstream default
// across a library upgrade instead of being pinned to whatever the template
// said on the day it was copied. Only settings this board actually forces are
// listed, which also makes the file readable as a statement of intent.
//
// Found via -DLV_CONF_INCLUDE_SIMPLE, since PlatformIO already puts include/
// on the compiler's search path.

#ifndef LV_CONF_H
#define LV_CONF_H

// NOTHING AN ASSEMBLER CANNOT PARSE MAY GO IN THIS FILE. No #include, no
// typedef, no declaration -- macros only.
//
// LVGL ships hand-written assembly for some targets (lv_blend_helium.S and
// friends). Those are .S files, so the C PREPROCESSOR runs over them, and the
// first thing they do is pull in lv_conf_internal.h -- which pulls in this
// file. Whatever is here is therefore fed to the assembler.
//
// LVGL's own lv_conf_template.h opens with `#include <stdint.h>`, so copying
// the template verbatim reproduces this. The failure is not obviously about
// this file at all; it is dozens of lines of:
//
//     .../sys-include/stdint.h:22: Error: unknown opcode or format name 'typedef'
//     *** [.../lv_blend_helium.S.o] Error 1
//
// i.e. the assembler dutifully trying to assemble C type declarations. The
// assembly bodies themselves are guarded by LV_USE_DRAW_SW_ASM and compile to
// nothing on Xtensa; it is only the preamble that has to stay clean.
//
// Nothing below needs stdint types -- sizes are written as plain integer
// literals for exactly this reason.

// --- colour ----------------------------------------------------------------
// ST7789T3 driven as RGB565. 16 bpp is also what makes the flush callback a
// straight memcpy into LovyanGFX's native pixel type rather than a per-pixel
// conversion.
#define LV_COLOR_DEPTH 16

// --- memory ----------------------------------------------------------------
// LVGL's own pool rather than the C library's, and sized generously because
// this build has 512 KB of internal SRAM and spends almost none of it: the
// discrete firmware's five tasks together reserve ~31 KB of stack.
//
// The pool stays in INTERNAL RAM even though the board has 8 MB of PSRAM.
// PSRAM is the right home for large image assets and would be the wrong home
// for widget metadata -- every allocation LVGL walks during a redraw would go
// out over the octal bus, and cache pressure would show up as a UI that is
// mysteriously slower under load. Framebuffers are the thing worth moving out,
// and those are allocated explicitly in ui/display code, not from this pool.
#define LV_USE_STDLIB_MALLOC LV_STDLIB_BUILTIN
#define LV_MEM_SIZE (64U * 1024U)

// --- OS integration --------------------------------------------------------
// LV_OS_NONE, and that is a design decision rather than a default left alone.
//
// LVGL is not thread-safe. This project's answer to shared mutable state is
// already established and works: sensorTask owns SensorArray exclusively, and
// state crosses task boundaries only as immutable snapshots taken under a
// mutex. The UI follows the same rule -- one task makes every lv_* call, and
// reads device state through tasks::snapshot() like the LED task does.
//
// LV_OS_FREERTOS would add lv_lock()/lv_unlock() so several tasks could call
// into LVGL safely. That is a strictly larger thing to get right, and it buys
// nothing here, because there is no second task that wants to draw.
#define LV_USE_OS LV_OS_NONE

// --- timing ----------------------------------------------------------------
// LVGL's default refresh period is 33 ms, i.e. a HARD 30 FPS CEILING that has
// nothing to do with what the hardware can do. Measuring the scope page against
// that default would have produced a confident "30 FPS" that was really just
// this constant being read back.
//
// 10 ms puts the ceiling at 100 FPS, well above anything a 40 MHz SPI panel can
// sustain, so the number on screen is the renderer's and the bus's limit rather
// than the timer's. Nothing redraws when nothing is invalidated, so a static
// screen still costs nothing.
#define LV_DEF_REFR_PERIOD 10
// LVGL 9 takes the tick source at RUNTIME via lv_tick_set_cb(millis), so there
// is no LV_TICK_CUSTOM block here the way v8 needed one. Mentioned because its
// absence looks like an omission when porting a v8 config.

// --- fonts -----------------------------------------------------------------
// A range, not a minimal set, because the specimen page compares them side by
// side on the real panel. Each costs roughly 2-9 KB of flash and this image
// uses 12% of 6.5 MB, so the trade is not close.
//
// ON "THE FONT LOOKS LIKE DOTS". These ARE antialiased -- LVGL renders the
// built-in Montserrat at 4 bpp, giving 16 levels of coverage per pixel. The
// roughness at small sizes is not a missing feature; it is that this panel is
// ~200 DPI and a 14 px glyph stem is barely one pixel wide, so there is almost
// nothing for antialiasing to grade. Going UP in size is what fixes it, which
// is why the range now reaches well past what the layout currently uses.
//
// A different TYPEFACE is the other lever, and LVGL does not ship one: only
// Montserrat is built in for Latin. Any other face -- and any Devanagari or
// Gujarati for foundation branding -- has to be converted from a TTF to a C
// array with LVGL's font converter and committed as source.
#define LV_FONT_MONTSERRAT_12 1
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_18 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_22 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_32 1
#define LV_FONT_MONTSERRAT_36 1
#define LV_FONT_MONTSERRAT_40 1
#define LV_FONT_MONTSERRAT_48 1

// Kept as the deliberate counter-example on the specimen page: unscii is a
// genuine 1 bpp pixel font with no antialiasing at all. Seeing it beside
// Montserrat is the quickest way to confirm that what the UI is doing is not
// "no antialiasing" but "not enough pixels".
#define LV_FONT_UNSCII_8 1
#define LV_FONT_UNSCII_16 1

// 18, not 14, and the jump is evidence-based rather than taste. On the real
// panel the 20 px title read as clean while the 14 px body read as "dots" --
// same typeface, same 4 bpp antialiasing, only the size differing. That is the
// signature of too few pixels per glyph, not of a bad font.
//
// THE TYPE SCALE, and it is a rule rather than a preference:
//
//   14 / 16   messages, information, errors -- read deliberately, up close
//   18 min    labels, states, major widgets -- read at a glance
//   24 / 48   the level tag and the bowl count -- read across the room
//
// The default is 18 because that is the floor for anything glanceable, which is
// most of this UI. Anything wanting 14 or 16 asks for it explicitly, so the
// smaller sizes are always a deliberate choice and never something a widget
// inherited by accident.
//
// The evidence behind the floor: on the real panel the 20 px title read as
// clean and the 14 px body read as "dots" -- same typeface, same 4 bpp
// antialiasing, only the size differing. That is too few pixels per glyph, not
// a bad font.
//
// It is a layout constraint as much as a style one. At these sizes there is no
// room for dense text, so density comes down instead -- which docs/frontend.md
// argues for on its own terms: an operational display watched during a busy
// service, where glanceability beats completeness.
#define LV_FONT_DEFAULT &lv_font_montserrat_18

// --- logging ---------------------------------------------------------------
// On, at WARN. An embedded GUI that fails silently fails invisibly: a bad
// flush callback or an exhausted pool shows up as a frozen screen with no
// console trace at all unless this is enabled. WARN keeps it quiet in normal
// operation. lv_log_register_print_cb() routes it to Serial at init.
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 0

// --- diagnostics -----------------------------------------------------------
// Both off. Turn LV_USE_PERF_MONITOR on while tuning the flush path -- it
// overlays FPS and CPU in a corner, which is the fastest way to find out
// whether a sluggish screen is the SPI transfer or the redraw.
#define LV_USE_PERF_MONITOR 0
#define LV_USE_MEM_MONITOR 0

// Catches use-after-free and pool corruption early, at a small cost per
// allocation. Worth keeping on until the UI stops changing shape.
#define LV_USE_ASSERT_MALLOC 1
#define LV_USE_ASSERT_OBJ 1

#endif  // LV_CONF_H
