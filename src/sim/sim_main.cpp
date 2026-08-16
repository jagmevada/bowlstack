// Desktop preview for the Bowlstack touch UI.
//
// Compiles the SAME src/ui/ that ships on the ESP32-S3, against the SAME
// include/lv_conf.h, at the same 240x320 and the same colour depth, driven by
// the SAME ui_demo scenarios. The only substitutions are at the very edges: SDL
// renders instead of LovyanGFX, and a mouse stands in for a finger.
//
// That sharing is the whole design. A preview that keeps its own copy of the
// screens or its own fixture data drifts from the device -- silently, and in
// the direction of whichever one is looked at more often. Here there is nothing
// to drift: this file is a window and a clock.
//
// The edit-see loop is ~7 s against ~6 min to flash, which is the difference
// between iterating on a layout and avoiding it.
//
// WHAT IT CANNOT TELL YOU. It is a preview, not an emulator:
//   - this monitor is ~96 DPI and the panel is ~200, so whether 20 px text is
//     LEGIBLE can only be settled on hardware. Pixel dimensions match exactly;
//     apparent physical size does not, and cannot.
//   - there is no tearing here, because the desktop compositor has the vsync
//     the Waveshare panel's unrouted TE pin cannot give us
//   - colour, touch accuracy, backlight and viewing angle are physical
// Layout, wording, sizing in PIXELS and state logic transfer exactly.
//
//   pio run -e sim -t exec

#include <SDL2/SDL.h>
#include <lvgl.h>

#include <cstdio>

#include "ui_demo.h"
#include "ui_gallery.h"
#include "ui_screens.h"

int main(int, char **) {
  lv_init();

  // SDL_GetTicks rather than anything of our own: LVGL 9 takes its tick source
  // at runtime, and without one every timer and animation sees zero elapsed
  // time -- the window draws once and then appears frozen.
  lv_tick_set_cb([]() -> uint32_t { return SDL_GetTicks(); });

  lv_display_t *disp = lv_sdl_window_create(ui::SCREEN_W, ui::SCREEN_H);

  // Explicit 1:1. LVGL's SDL driver can scale its window, and a zoom left at
  // anything else would make this a picture OF the UI at the wrong size rather
  // than the UI -- which is precisely the confusion this preview exists to
  // avoid. One SDL pixel is one panel pixel.
  lv_sdl_window_set_zoom(disp, 1);
  lv_sdl_window_set_title(disp, "Bowlstack 240x320 1:1");

  lv_sdl_mouse_create();
  lv_sdl_keyboard_create();

  ui::buildGallery();

  printf("Bowlstack UI preview\n");
  printf("  window   %d x %d, zoom 1 (one SDL pixel = one panel pixel)\n", ui::SCREEN_W,
         ui::SCREEN_H);
  printf("  pages    drag horizontally: 3 type, widgets, stock view, pixel-shift test\n");
  printf("  states   the stock view cycles %u scenarios every 3 s, same as the device\n",
         ui::demoCount());
  printf("  ESC      quit\n\n");
  fflush(stdout);

  bool running = true;
  while (running) {
    lv_timer_handler();

    // Pixel shift is OFF -- see todo.md. This panel is IPS-TFT, so what was
    // observed was image PERSISTENCE (temporary, self-recovering) rather than
    // OLED burn-in (permanent). Not worth engineering for. Gallery page 6 still
    // demonstrates the mechanism on demand.
    // ui::pixelShiftTick(SDL_GetTicks());

    if (ui::demoTick(SDL_GetTicks())) {
      // demoTick advances AFTER rendering the current one, so the name of what
      // is on screen is the previous index.
      const uint8_t shown = (uint8_t)((ui::demoIndex() + ui::demoCount() - 1) % ui::demoCount());
      printf("state: %s\n", ui::demoName(shown));
      fflush(stdout);
    }

    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) running = false;
      if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) running = false;
    }

    SDL_Delay(5);
  }

  return 0;
}
