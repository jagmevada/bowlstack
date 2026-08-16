// LovyanGFX device definition for the Waveshare ESP32-S3-Touch-LCD-2.
//
// LovyanGFX rather than TFT_eSPI because TFT_eSPI configures itself through
// build-time #defines in a library-owned header, which means a second board
// cannot coexist in the same tree without editing files inside .pio. This
// project already builds two images from one source tree and is about to build
// three; a driver whose configuration lives in a normal header, in normal
// C++, is the one that survives that.
//
// Pin numbers come from board_waveshare_s3.h, which cites the schematic. They
// are not repeated as literals here -- a display that scrolls garbage because
// two headers disagree about DC is a genuinely miserable afternoon.

#pragma once

#define LGFX_USE_V1
#include <LovyanGFX.hpp>

#include "board_waveshare_s3.h"

class LGFX_WaveshareS3Touch2 : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel_;
  lgfx::Bus_SPI bus_;
  lgfx::Light_PWM light_;
  lgfx::Touch_CST816S touch_;

 public:
  LGFX_WaveshareS3Touch2() {
    {
      auto cfg = bus_.config();
      cfg.spi_host = SPI2_HOST;  // SPI3_HOST is left alone for anything later
      cfg.spi_mode = 0;
      cfg.freq_write = 40000000;
      cfg.freq_read = 16000000;
      cfg.spi_3wire = false;
      cfg.use_lock = true;
      cfg.dma_channel = SPI_DMA_CH_AUTO;

      cfg.pin_sclk = board::LCD_SCLK;
      cfg.pin_mosi = board::LCD_MOSI;
      cfg.pin_miso = board::LCD_MISO;
      cfg.pin_dc = board::LCD_DC;

      bus_.config(cfg);
      panel_.setBus(&bus_);
    }

    {
      auto cfg = panel_.config();
      cfg.pin_cs = board::LCD_CS;

      // -1, from the schematic: R16 is unpopulated, so RST is held high by
      // R14 and no GPIO reaches it. See board_waveshare_s3.h.
      cfg.pin_rst = board::LCD_RST;
      cfg.pin_busy = -1;

      cfg.panel_width = board::LCD_W;
      cfg.panel_height = board::LCD_H;
      cfg.memory_width = board::LCD_W;
      cfg.memory_height = board::LCD_H;
      cfg.offset_x = 0;
      cfg.offset_y = 0;
      cfg.offset_rotation = 0;

      cfg.dummy_read_pixel = 8;
      cfg.dummy_read_bits = 1;
      cfg.readable = false;  // no MISO to the panel; only the SD card has one

      // ST7789 on an IPS panel ships inverted. If the bring-up harness draws
      // white-on-black as black-on-white, this is the line to flip -- it is the
      // single most common wrong-looking-but-working symptom on this part.
      cfg.invert = true;
      cfg.rgb_order = false;

      // THE BUS IS SHARED WITH THE TF CARD (both on SCLK 39 / MOSI 38). Left
      // false, a card transaction and a panel flush can interleave and paint
      // the screen with filesystem bytes. Costs a mutex per transfer; buys
      // correctness the moment anything touches the card.
      cfg.bus_shared = true;

      panel_.config(cfg);
    }

    {
      // Backlight through the SS8050: active high, so invert stays false.
      auto cfg = light_.config();
      cfg.pin_bl = board::LCD_BL;
      cfg.invert = !board::LCD_BL_ACTIVE_HIGH;
      cfg.freq = 44100;
      cfg.pwm_channel = 7;
      light_.config(cfg);
      panel_.setLight(&light_);
    }

    {
      auto cfg = touch_.config();
      cfg.i2c_port = 0;
      cfg.i2c_addr = board::TP_ADDR;
      cfg.pin_sda = board::TP_SDA;
      cfg.pin_scl = board::TP_SCL;
      cfg.pin_int = board::TP_INT;

      // -1 for the same reason as the panel: TP_RESET shares the panel's
      // pulled-up net and no GPIO reaches it.
      cfg.pin_rst = board::TP_RST;

      cfg.freq = 400000;
      cfg.bus_shared = false;  // shared with the IMU, not with the display bus

      // Raw touch coordinate span. Kept equal to the panel so calibration is
      // identity by default -- the CST816D reports in panel pixels already.
      cfg.x_min = 0;
      cfg.x_max = board::LCD_W - 1;
      cfg.y_min = 0;
      cfg.y_max = board::LCD_H - 1;
      cfg.offset_rotation = 0;

      touch_.config(cfg);
      panel_.setTouch(&touch_);
    }

    setPanel(&panel_);
  }
};
