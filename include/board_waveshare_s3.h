// Waveshare ESP32-S3-Touch-LCD-2 -- what the board already spends, and what is
// left for Bowlstack.
//
// Every number here was read off the manufacturer's schematic netlist
// (ESP32-S3-Touch-LCD-2-SchDoc.pdf), NOT from a pinout picture or a forum post.
// The netlist names each net and lists the component pins on it, so an entry
// like
//
//     PIU2010  PIR1902  PIR2001  PIC3101   ->  net BAT_ADC / IO5
//
// is direct evidence that module pin 10 (= GPIO5) sits on the same node as
// R19, R20 and C31. Where a value below could not be established that way it
// says so explicitly rather than guessing.
//
// This header describes the BOARD. It deliberately contains no Bowlstack
// tuning -- config.h stays the place for that -- so it can be included by the
// bring-up harness, which knows nothing about bowls.

#pragma once

#include <Arduino.h>

namespace board {

// ---------------------------------------------------------------------------
// 1. Silicon
// ---------------------------------------------------------------------------
// ESP32-S3R8: 16 MB quad flash (W25Q128JVSI) + 8 MB OCTAL PSRAM in package.
//
// The octal PSRAM is the fact with consequences. It consumes GPIO33-37
// permanently -- in the netlist those five nets carry exactly one node each,
// the module pin itself, going nowhere on the board -- so they are not "free
// but unrouted", they are spoken for inside the package. Together with
// GPIO26-32 (quad flash) that is twelve GPIOs gone before anything is wired.
//
// PlatformIO needs to be told, or the image boots without PSRAM and the UI
// framebuffer allocation fails at runtime rather than at build time:
//
//     board_build.arduino.memory_type = qio_opi
//
static const uint8_t PSRAM_RESERVED[] = {33, 34, 35, 36, 37};
static const uint8_t FLASH_RESERVED[] = {26, 27, 28, 29, 30, 31, 32};

// ---------------------------------------------------------------------------
// 2. Display -- ST7789T3, 240x320 IPS, SPI
// ---------------------------------------------------------------------------
// The LCD and the TF card SHARE one SPI bus. Only the chip selects differ, so
// any driver touching both must be told the bus is shared or a card read will
// corrupt the panel mid-transfer (LovyanGFX: cfg.bus_shared = true).
static const int8_t LCD_SCLK = 39;   // shared with SD_SCLK
static const int8_t LCD_MOSI = 38;   // shared with SD_MOSI
static const int8_t LCD_MISO = 40;   // the SD card's; the panel is write-only
static const int8_t LCD_DC   = 42;
static const int8_t LCD_CS   = 45;

// NOT A TYPO, AND NOT AN OVERSIGHT. The panel's RESET and the touch chip's
// RESET are tied to one node held up by R14 (10k to 3V3). R16 -- the 0R link
// that would connect that node to GPIO0 -- is marked "NC/0R" and is left
// unpopulated. So there is no software reset for either device on a stock
// board: they reset only at power-on.
//
// Two things follow. A driver must be told -1 rather than being handed GPIO0,
// which is the BOOT strap and would be actively harmful to toggle. And a touch
// controller that has wedged cannot be recovered without cutting power -- if
// that ever shows up in the field, populating R16 is the fix, at the cost of
// GPIO0.
static const int8_t LCD_RST = -1;
static const int8_t TP_RST  = -1;

// Backlight is ACTIVE HIGH: GPIO1 -> R12 (1k) -> base of T1 (SS8050, NPN),
// emitter to GND, collector sinking LEDK. Driving the pin high turns the
// backlight ON -- the opposite polarity to this project's common-anode status
// LEDs, which is worth stating once rather than rediscovering.
//
// It is a plain GPIO with a transistor, so it takes PWM (LEDC) for dimming.
static const int8_t LCD_BL = 1;
static const bool   LCD_BL_ACTIVE_HIGH = true;

static const uint16_t LCD_W = 240;
static const uint16_t LCD_H = 320;

// ---------------------------------------------------------------------------
// 3. Touch -- CST816D, I2C 0x15
// ---------------------------------------------------------------------------
// The touch controller does NOT get a private bus. GPIO47/48 carry the touch
// chip AND the QMI8658 IMU, and are also broken out to header P2 pins 5 and 6.
// Pull-ups are already fitted (R29, R30, 4.7k to 3V3) -- do not add more.
//
// This is the single most important constraint for the sensor port: hanging
// four VL53L0X modules on this bus would put the bowl sensors, the touch
// screen and the IMU in one failure domain. A clone ToF module that latches
// SDA low -- the exact failure this project already splits two buses to
// contain -- would take the touchscreen down with it, i.e. destroy the UI at
// precisely the moment someone needs it to diagnose the fault. Keep this bus
// for on-board peripherals only.
static const int8_t TP_SDA = 48;
static const int8_t TP_SCL = 47;
static const int8_t TP_INT = 46;
static const uint8_t TP_ADDR = 0x15;

static const int8_t IMU_SDA  = 48;  // same bus as touch
static const int8_t IMU_SCL  = 47;
static const int8_t IMU_INT1 = 3;

// ---------------------------------------------------------------------------
// 4. Battery -- ALREADY DIVIDED. Do not fit your own.
// ---------------------------------------------------------------------------
// The board carries a resistive divider from VBAT to GPIO5:
//
//     VBAT --[R19 200k]--+-- GPIO5 (ADC1_CH4)
//                        |
//                     [R20 100k]   [C31 100nF]
//                        |            |
//                       GND          GND
//
// So the plan to "add a voltage divider later" is already done in hardware,
// and adding a second one in parallel would load this one and skew every
// reading. What changes instead is the CALIBRATION CONSTANT: the ratio here is
// 3.0, not the 2.0 of the discrete build's 10k+10k.
//
// GPIO5 is ADC1, which is mandatory rather than convenient -- ADC2 is unusable
// while WiFi is running, and this device is a WiFi device by definition.
//
// Range check against the existing SoC curve: a 4.2 V cell presents 1.40 V at
// the pin and a 2.75 V cell presents 0.92 V. At 12 dB attenuation the S3's
// usable span is roughly 150-2450 mV, so the entire battery range sits inside
// it with margin at both ends. Nothing about the curve in battery_soc.h needs
// to change; only BATTERY_DIVIDER does.
//
// THE CAVEAT THAT MATTERS. Source impedance is 200k||100k = 66.7k, against the
// ~10k the ESP32 SAR ADC wants to see. C31 is what makes this work at all: it
// is a local charge reservoir some four orders of magnitude larger than the
// sampling capacitor, so an individual conversion barely droops. But the
// static error from ADC input leakage across 66.7k does not average away, and
// it differs per chip. Read this as: the 16-sample mean is still worth taking,
// and per-unit calibration is MORE important on this board than on the
// discrete one, not less. Calibrate against a multimeter before trusting a
// band boundary.
static const int8_t PIN_BATTERY_ADC = 5;
static const float  BATTERY_DIVIDER_NOMINAL = 3.0f;  // (200k + 100k) / 100k

// ---------------------------------------------------------------------------
// 5. Charger -- ETA6098, and its status pin goes nowhere useful
// ---------------------------------------------------------------------------
// The charger's STAT output (U6 pin 9) drives the cathode of LED1 directly.
// That net has exactly two nodes -- the charger and the LED -- and no module
// pin on it. There is therefore NO WAY to read charge state in firmware on an
// unmodified board.
//
// This costs the `charging` field that the discrete build publishes. The
// honest firmware behaviour is to report it as unknown rather than false: the
// whole codebase already refuses to state what it cannot measure, and a hard
// `false` would be a claim, not an absence.
//
// If charge state turns out to be worth having, the mod is small and the
// existing config already anticipates its polarity. STAT is open-drain and
// pulls LOW while charging, which is the TP4056-style sense that
// CHARGING_ACTIVE_LOW = true was written for:
//
//     ETA6098 STAT (LED1 cathode) --[10k]-- free GPIO (INPUT_PULLUP)
//
// Note this inverts the sense used on the discrete board, where the pin is
// HIGH while charging. Flipping that flag is the entire software change.
static const bool CHARGER_STATUS_READABLE = false;

// ---------------------------------------------------------------------------
// 6. TF card (shares the display's SPI bus)
// ---------------------------------------------------------------------------
static const int8_t SD_SCLK = 39;
static const int8_t SD_MOSI = 38;
static const int8_t SD_MISO = 40;
static const int8_t SD_CS   = 41;

// ---------------------------------------------------------------------------
// 7. What is actually left
// ---------------------------------------------------------------------------
// Broken out to the two 14-pin headers. Everything else on the module is
// flash, PSRAM, the panel, the touch/IMU bus, USB or the console.
//
//   P1:  IO2  IO4  IO6  IO16 IO17 IO18 IO21 IO8  IO7  IO10 IO20 IO19 GND 5V
//   P2:  3V3  GND  IO43 IO44 IO47 IO48 IO13 IO12 IO15 IO11 IO14 IO9  GND VBAT
//
// Three of those are already claimed by something you probably want to keep:
//
//   IO19, IO20   native USB D-/D+. Usable as GPIO only by giving up USB-CDC
//                and USB-JTAG, which on this board is how you get a console
//                at all once GPIO43/44 are repurposed. Treat as unavailable.
//   IO43, IO44   UART0 console. Free ONLY if the build uses USB-CDC instead
//                (-DARDUINO_USB_CDC_ON_BOOT=1). The serial plotter harness
//                works fine over CDC, so this is a real option -- but it is a
//                choice, not a freebie.
//   IO17         has R6, a 10k pull-down (it is the camera's PWDN line). Fine
//                as an output; a poor choice for an I2C line, which would
//                have to fight it.
//
// The camera connector's SCCB bus is the find worth using. GPIO16 and GPIO21
// are a full I2C pair, already fitted with 4.7k pull-ups (R4, R5), broken out
// on P1, and idle unless a camera is plugged into J1 -- which this project has
// no use for. That is one of Bowlstack's two sensor buses for free.
//
// The second bus has no pull-ups anywhere and needs external 4.7k resistors.
static const uint8_t FREE_GPIOS[] = {
    2, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 21,
};

// The camera SCCB pair: a real I2C bus with pull-ups already fitted.
static const int8_t CAM_SCCB_SDA = 21;
static const int8_t CAM_SCCB_SCL = 16;

// ---------------------------------------------------------------------------
// 8. Load cells -- three NAU7802 behind a TCA9548A, on ONE bus
// ---------------------------------------------------------------------------
// THE NAU7802 HAS NO ADDRESS PINS. Every part answers at 0x2A and there is no
// strap, no OTP field and no software way to move it. That fact has not
// changed; what changed is the answer to it.
//
// WHAT THIS REPLACED, because the reasoning is worth keeping. Two cells were
// two BUSES: cell A on GPIO47/48 sharing the touch controller's hardware port,
// cell B bit-banged on GPIO11/12 with no pull-ups fitted. Both halves of that
// were bad in a way the third cell made unarguable:
//
//   * Cell A sat on the TOUCH AND IMU bus, which section 3 above argues should
//     stay clear of off-board parts. A cell that latched SDA low took the
//     screen down with it -- see the timed-touch backoff in loadcell_main.cpp,
//     written for exactly that.
//   * Cell B was software on the render loop, at 100 kHz because GPIO11/12 have
//     no pull-ups and 400 kHz produced NAKs that read as a flaky converter.
//   * A third cell would have been a third bus, and there was no third pair
//     worth having.
//
// SO: one hardware bus, one switch, one channel per cell. GPIO21/16 is the
// camera's SCCB pair -- a complete I2C bus with 4.7k pull-ups already fitted
// (R4, R5), broken out on header P1, and idle because this project will never
// fit a camera. It reaches hardware I2C port 1; port 0 stays LovyanGFX's, and
// GPIO47/48 now carry nothing but the touch chip and the IMU again.
//
// THE MUX IS A SWITCH, NOT A BUFFER. Each downstream stub needs its own 4.7k
// pair to 3V3 -- fitted on the converter breakout, or fitted by hand. See
// i2cmux.h for the rest of what follows from that, including why exactly one
// channel is ever enabled.
//
// A0/A1/A2 to GND gives 0x70. They have no internal pull-downs in the silicon;
// most breakouts fit their own, but a floating address pin is an address that
// moves, so strap them.
static const int8_t CELL_SDA = 21;  // == CAM_SCCB_SDA, R4 4.7k fitted
static const int8_t CELL_SCL = 16;  // == CAM_SCCB_SCL, R5 4.7k fitted

static const uint8_t MUX_ADDR = 0x70;  // A0/A1/A2 grounded

// Cell index -> mux channel. Identity today, and stated as a table anyway: the
// mapping is a WIRING fact, and the day one stub moves to channel 5 to dodge a
// damaged pin, this is the only line that should have to change.
static const uint8_t CELL_COUNT = 3;
static const int8_t CELL_MUX_CH[CELL_COUNT] = {0, 1, 2};

// Fixed and unchangeable -- see above.
static const uint8_t NAU7802_ADDR = 0x2A;

}  // namespace board
