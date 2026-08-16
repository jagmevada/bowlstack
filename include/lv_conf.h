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
// LVGL 9 takes the tick source at RUNTIME via lv_tick_set_cb(millis), so there
// is no LV_TICK_CUSTOM block here the way v8 needed one. Mentioned because its
// absence looks like an omission when porting a v8 config.

// --- fonts -----------------------------------------------------------------
// The bowl count is the number a person reads from across a kitchen, so it
// gets a genuinely large face; everything else is labels and chips.
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_48 1
#define LV_FONT_DEFAULT &lv_font_montserrat_14

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
