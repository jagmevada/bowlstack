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
#include <cstdlib>

#include "ui_demo.h"
#include "ui_pages.h"
#include "ui_perf.h"
#include "ui_scope.h"
#include "ui_screens.h"
#include "ui_wifi.h"

// --- screenshots -----------------------------------------------------------------
// BOWLSTACK_SIM_SHOT=<file.bmp> writes the window to a BMP once
// BOWLSTACK_SIM_SHOT_MS (default 1500) have passed, then exits. It exists so a layout
// can be CHECKED by looking at it -- by a reviewer, or by an agent with no window to
// look at -- rather than argued about from pixel arithmetic. The demo cycles a
// scenario every 3 s, so the delay picks the scenario: 1500 is the first, 4500 the
// second, and so on.
//
// THE SOFTWARE RENDERER, ONLY WHEN SHOOTING. Reading pixels back after a present is
// undefined on an accelerated renderer (the back buffer may already be discarded);
// the software one keeps them. An ordinary run is left on whatever SDL picks.
static void saveShot(lv_display_t *disp, const char *path) {
  SDL_Renderer *r = (SDL_Renderer *)lv_sdl_window_get_renderer(disp);
  SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ui::SCREEN_W, ui::SCREEN_H, 32,
                                                  SDL_PIXELFORMAT_ARGB8888);
  if (!r || !s) {
    printf("shot: no renderer or surface\n");
    return;
  }
  if (SDL_RenderReadPixels(r, nullptr, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) != 0 ||
      SDL_SaveBMP(s, path) != 0)
    printf("shot: FAILED: %s\n", SDL_GetError());
  else
    printf("shot: wrote %s\n", path);
  SDL_FreeSurface(s);
}

int main(int, char **) {
  const char *shotPath = getenv("BOWLSTACK_SIM_SHOT");
  const char *shotMsEnv = getenv("BOWLSTACK_SIM_SHOT_MS");
  const uint32_t shotAt = shotMsEnv ? (uint32_t)atoi(shotMsEnv) : 1500u;
  if (shotPath) SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software");

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

  ui::wifiSetMac("28:84:85:47:ab:bc");
  // BUILT FROM THE SAME EXPRESSION THE DEVICE USES, not typed. This read
  // "Bowlstack-BWL-001" while the panel showed "Bowlstack-LDC-001", so the
  // preview's QR encoded a different network from the product's -- exactly the
  // silent divergence CLAUDE.md's "one fixture, two targets" rule exists to
  // stop, and invisible because a QR does not look wrong when it is wrong.
  ui::wifiSetSetupAp("Bowlstack-" BOWLSTACK_DEVICE_ID, "bowlstack");
  ui::demoInstallWifiMocks();

  ui::perfBegin();
  // Plain heap: the desktop has no PSRAM, which is a difference in the machine
  // rather than in the fixtures.
  ui::scopeSetBuffer(malloc(ui::SCOPE_BUF_BYTES));
  ui::buildPages();
  // BOWLSTACK_SIM_PAGE=buffers|buf1|bufcal|settings|scale|diagnose opens that overlay
  // at start, so a screenshot (above) can be taken of a page that is several taps
  // deep without a mouse.
  if (const char *page = getenv("BOWLSTACK_SIM_PAGE")) ui::pagesPreviewOpen(page);

  printf("Bowlstack UI preview\n");
  printf("  window   %d x %d, zoom 1 (one SDL pixel = one panel pixel)\n", ui::SCREEN_W,
         ui::SCREEN_H);
  printf("  pages    drag horizontally: stock view | live scope + FPS\n");
  printf("  states   the stock view cycles %u scenarios every 3 s, same as the device\n",
         ui::demoCount());
  printf("  ESC      quit\n\n");
  fflush(stdout);

  bool running = true;
  while (running) {
    ui::perfFrameStart(SDL_GetTicks());
    lv_timer_handler();
    ui::perfFrameEnd(SDL_GetTicks());

    // Pixel shift is OFF -- see todo.md. This panel is IPS-TFT, so what was
    // observed was image PERSISTENCE (temporary, self-recovering) rather than
    // OLED burn-in (permanent). Not worth engineering for.
    // ui::pixelShiftTick(SDL_GetTicks());

    ui::pagesTick(SDL_GetTicks());

    // THE DESKTOP FIGURE IS A CEILING, NOT A PREDICTION. This renders with SDL
    // into system memory on a desktop CPU; the device renders on a 240 MHz core
    // and then pushes every dirty pixel over a 40 MHz SPI bus. Read the two
    // numbers side by side and the gap between them is what the panel costs.
    static uint32_t nextFps = 0;
    if (SDL_GetTicks() > nextFps) {
      nextFps = SDL_GetTicks() + 2000;
      char perf[128];
      ui::perfFormat(perf, sizeof(perf));
      printf("%s\n", perf);
      fflush(stdout);
    }

    if (shotPath && SDL_GetTicks() >= shotAt) {
      lv_refr_now(disp);  // the frame on screen is the frame written
      saveShot(disp, shotPath);
      running = false;
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
