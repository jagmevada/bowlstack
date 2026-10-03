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
//     ETA6098 STAT (LED1 cathode) --[1k]-- free GPIO (INPUT_PULLUP)
//
// 1k, NOT the 10k this file used to say. That value fails silently, in the
// direction that matters. STAT pulled low sits at ~0.15 V; through 10k against
// the internal pull-up the pin reads
//
//     0.15 + 3.15 x 10/55 = 0.72 V     vs  V_IL(max) = 0.25 x VDD = 0.825 V
//
// which is 105 mV of margin -- and the margin cannot be bounded, because
// Espressif specifies R_PU as "-- 45 --" kOhm: a typical with NO minimum and no
// maximum. A part whose pull-up comes in below ~36.7k reads HIGH instead, and
// HIGH means "not charging". A unit would report not-charging while charging,
// on some boards and not others, with nothing anywhere indicating a fault.
//
// 1k gives 0.22 V and breaks only below 3.67k, which nothing plausible
// violates. It also caps fault current at 3.3 mA if the pin is ever driven
// high by mistake.
//
// Note this inverts the sense used on the discrete board, where the pin is
// HIGH while charging.
//
// WHY THAT TAP IS SAFE, which was not obvious and had to be got from the
// netlist rather than assumed. The worry with tapping an indicator LED is what
// the node floats to when the open-drain output releases -- on a 5 V rail it
// would sit above this chip's 3.6 V absolute maximum. It does not, because
// LED1's anode is not on 5 V:
//
//     anode   net = { LED1-A , R13 }  -> R13 is 3K to VBAT
//     cathode net = { LED1-K , U6 pin 9 }        <- exactly two nodes; tap here
//
// So the highest the cathode can be pulled is VBAT - Vf ~= 4.2 - 1.9 = 2.3 V,
// which is BELOW the 3.3 V an INPUT_PULLUP holds the pin at. The LED therefore
// carries no current, the node follows the pin rather than the other way
// round, and the pin reads a clean 3.3 V. Charging pulls it hard to GND. No
// divider, no clamping, no series resistor needed for levels -- the 10k above
// is strain relief for a fragile pad joint and short protection for a slipped
// probe, nothing more.
//
// FINDING THE CATHODE WITH A METER AND NO SCHEMATIC: board unpowered, measure
// each LED1 pad to P2-14 (VBAT). The pad reading ~3k is the ANODE, through
// R13. The pad reading open is the CATHODE. Solder to the LED pad and never to
// U6 pin 9 -- the ETA6098 is a 0.5 mm-pitch DFN-10.
//
// Set -DBOWLSTACK_CHARGE_SENSE=1 once the wire is on. Until then this stays
// false and `charging` is published as unknown, because an unfitted mod that
// reported `false` would be a claim rather than an absence.
#ifdef BOWLSTACK_CHARGE_SENSE
static const bool CHARGER_STATUS_READABLE = true;
#else
static const bool CHARGER_STATUS_READABLE = false;
#endif

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
//   P2:  3V3  GND  IO43 IO44 IO47 IO48 IO15 IO13 IO11 IO12 IO14 IO9  GND VBAT
//
// P2 POSITIONS 7-10 WERE WRONG HERE until the netlist was re-read. This file
// said `IO13 IO12 IO15 IO11`; the board is `IO15 IO13 IO11 IO12`. The SET of
// free pins was right and the firmware never cared, which is exactly why it
// survived -- nothing built from these numbers fails, because nothing in the
// image counts header positions. It is only wrong for the person holding the
// wire, and it is wrong in the worst way: a lead intended for IO13 lands on
// IO15, one intended for IO12 lands on IO13, and both are free pins, so the
// board comes up and the signal is simply somewhere else.
//
// Netlist evidence, ESP32-S3-Touch-LCD-2-SchDoc.pdf p1:
//     PIP207 -> CAM_D2/IO15    PIP208  -> CAM_D1/IO13
//     PIP209 -> CAM_D3/IO11    PIP2010 -> CAM_D0/IO12
// cross-checked against the drawing's own pinout table.
//
// docs/waveshare_port.md carried the same permutation in a wiring table.
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
// SO: one hardware bus, one switch, one channel per cell. GPIO12/11 is broken
// out on header P2 -- SDA on P2-10, SCL on P2-9 -- and reaches hardware I2C
// port 1; port 0 stays LovyanGFX's, and GPIO47/48 now carry nothing but the
// touch chip and the IMU again.
//
// NO ON-BOARD PULL-UPS ON THIS PAIR. The trunk gets them from the mux breakout.
// This paragraph used to describe GPIO12/11 as the camera's SCCB pair "with
// 4.7k already fitted (R4, R5)" -- that is GPIO21/16, which is a different pair
// and is now the free one. Sections 8 and 9 below always said so; this
// paragraph contradicted them, and a wrong pull-up claim is the kind of thing
// somebody debugs with a scope rather than doubts.
//
// THE MUX IS A SWITCH, NOT A BUFFER. Each downstream stub needs its own 4.7k
// pair to 3V3 -- fitted on the converter breakout, or fitted by hand. See
// i2cmux.h for the rest of what follows from that, including why exactly one
// channel is ever enabled.
//
// A0/A1/A2 to GND gives 0x70. They have no internal pull-downs in the silicon;
// most breakouts fit their own, but a floating address pin is an address that
// moves, so strap them.
// MOVED TO IO12/IO11, AND NOTE THE ORDER -- SDA is 12 and SCL is 11, which is
// the reverse of how this pair reads left to right on the header. It is a
// wiring fact, confirmed on the bench: with the two swapped the mux does not
// answer at all, and with them this way round all three converters ACK at
// 400 kHz on the first try.
//
// PULL-UPS COME FROM THE MUX BREAKOUT HERE, and that is luck rather than
// design. R4 and R5 -- the 4.7k pair the camera connector carries on IO21/IO16
// -- are the reason that was the better bus, and IO11/IO12 have nothing. The
// board fitted happens to pull its own trunk up, verified by both lines idling
// high at boot and by three clean ACKs at full speed.
//
// So do NOT add a second pair. Two 4.7k in parallel is 2.4k, which is more
// current than a converter should have to sink. But a DIFFERENT mux board may
// not have them, and without pull-ups this bus does not fail cleanly -- short
// wires get away with the pins' own leakage and the symptom is a converter that
// NAKs intermittently and reads like a flaky part. That is this pair's actual
// history: the retired arrangement above ran cell B bit-banged here at 100 kHz
// precisely because 400 kHz produced NAKs.
//
// WHAT IS BETTER ABOUT IT, and it is not nothing: the whole load-cell loom now
// leaves on P2 -- signals at P2-9/P2-10, power at P2-1/P2-2 -- where the old
// pair split it across both headers, signals on P1 and 3V3 necessarily on P2
// because P1 has none. The panel loom is entirely on P1 and the cell loom
// entirely on P2, which is the separation the pin audit wanted and could not
// have while the bus was on the camera pair.
//
// IO21 and IO16 were free again here, and they are the two most valuable free pins
// on the board: a complete I2C bus with pull-ups already fitted. THEY ARE NOW
// SPENT: section 10 puts the 200 kg buffer module on them.
static const int8_t CELL_SDA = 12;  // P2-10  -- NO on-board pull-up, fit 4.7k
static const int8_t CELL_SCL = 11;  // P2-9   -- NO on-board pull-up, fit 4.7k

static const uint8_t MUX_ADDR = 0x70;  // A0/A1/A2 grounded

// Cell index -> mux channel. Identity today, and stated as a table anyway: the
// mapping is a WIRING fact, and the day one stub moves to channel 5 to dodge a
// damaged pin, this is the only line that should have to change.
static const uint8_t CELL_COUNT = 3;
static const int8_t CELL_MUX_CH[CELL_COUNT] = {0, 1, 2};

// Fixed and unchangeable -- see above.
static const uint8_t NAU7802_ADDR = 0x2A;

// ---------------------------------------------------------------------------
// 9. Panel controls -- rotary encoder, charge sense, status LED
// ---------------------------------------------------------------------------
// Six signals added by hand to a development board. Every one is on a header
// pin, so this is jumper wire rather than rework, and the physical positions
// are given because that is what the person holding the wire needs -- the GPIO
// number is what the firmware needs and they are not interchangeable. See the
// P2 correction in section 7 for why that distinction is written down twice.
//
// ON P1, BECAUSE THAT IS WHERE THE ENCODER PHYSICALLY IS. P2 is the better
// electrical answer -- its free run ends on a ground pin where P1's abuts
// CELL_SCL -- and it lost to the loom. The encoder is a panel part on a short
// harness, and routing it to the far header to protect ADC channels nobody is
// using is how a prototype acquires a wire that gets snagged.
//
//     P1-3   IO6    status LED               power / charge indicator
//     P1-6   IO18   encoder SW
//     P1-8   IO8    encoder CLK
//     P1-9   IO7    encoder DT
//     P1-10  IO10   VBUS sense               5k/10k divider from P1-14
//     P1-13  GND    encoder common, switch return, LED cathode
//
//     P2-8   IO13   charge sense (STAT)      DEFINED, NOT WIRED -- see below
//
// THE HAZARD THAT USED TO BE HERE HAS MOVED. While the cell bus was on the
// camera pair, P1-4 and P1-7 were CELL_SCL and CELL_SDA -- sitting between the
// LED and the encoder, one slipped wire from taking every load cell down. The
// bus is on P2-9/P2-10 now, so P1 carries nothing but panel controls and a
// misplaced lead here costs at worst one input.
//
// IO21 was originally asked for as the switch pin, back when it was CELL_SDA;
// it is free again today. IO18 stays the switch because nothing is gained by
// moving a working wire.
//
// IO18 IS THE ONE PIN ON THIS BOARD WITH A TWO-NODE NET -- {P1-6, U2-24}, and
// no stub on J1, the camera FPC. Every other free pin carries a third node
// there. The one mark against it is that it is also the only sub-19 GPIO with a
// HIGH-level power-up glitch (~60 us), so at reset the pad may briefly drive
// into a closed switch contact. Survivable and accepted: 60 us into a
// mechanical contact is nothing, and the alternative was splitting the encoder
// across both ends of the header.
//
// WHAT THIS SPENDS: IO6, IO7 and IO8 are ADC1_CH5/CH6/CH7, and ADC1 is the only
// ADC usable while WiFi is up. Three of its channels now carry a contact input
// pair and an LED. Accepted knowingly -- the load cells have their own
// converters and the battery has GPIO5, so nothing here has ever wanted another
// analog input. IO2, IO4 and IO9 remain if that changes.
//
// A NOTE ON IO17, which is two positions from the LED at P1-5 and would have
// been free. R6 makes it the one pin that can NEVER be an input, so spending it
// on the one output would have kept IO6's ADC channel. Not done, because the
// LED is a panel part and P1-3 is where it lands; the swap is a one-line change
// here if the analog channel is ever wanted back.
static const int8_t PIN_ENC_CLK    = 8;   // P1-8
static const int8_t PIN_ENC_DT     = 7;   // P1-9
static const int8_t PIN_ENC_SW     = 18;  // P1-6

// DEFINED BUT NOT WIRED. The VBUS tap on IO10 is what drives the indicator, and
// it answers "on mains", not "charging" -- the ETA6098 terminates when the cell
// is full and 5 V stays present, so a VBUS-driven indicator keeps signalling
// charge after charging has stopped. That is a deliberate simplification for
// one indicator LED and it is fine there.
//
// It is NOT fine for the `charging` column, which is why nothing publishes it:
// CHARGER_STATUS_READABLE stays false while this pin has no wire on it, and
// device_status carries `unknown` rather than a guess. Fitting the 1k tap in
// section 5 and defining BOWLSTACK_CHARGE_SENSE is what changes that.
static const int8_t PIN_CHARGE_STAT = 13; // P2-8, ETA6098 STAT, LOW = charging

// THE POWER-ON REMINDER, and its real job is to be seen when nothing else on
// the unit is. The display blanks on an inactivity timeout and the board then
// looks dead while still drawing from the cell -- so this LED says "the battery
// switch is still on", which is the thing somebody needs to know at the end of
// a service.
//
//     on mains    blinking at 1 Hz     charging, or at least connected
//     on battery  steady               running down the cell -- switch me off
//
// STEADY IS THE BATTERY STATE, WHICH IS THE MORE URGENT ONE, and that is the
// right way round even though it costs more current. A blink is easy to miss
// across a room and easy to mistake for a status indicator doing something
// harmless; a steady light on a device that appears to be off reads as a
// mistake, which is exactly what it is. At ~2 mA that is ~32 mAh over a
// sixteen-hour night -- a few per cent of a cell, and self-limiting, because
// the whole point is that somebody sees it and flips the switch.
//
// It also survives deep sleep, if the power-management work goes that far: IO6
// is RTC-capable so gpio_hold_en() can hold the level through sleep. Only the
// steady state can be held -- blinking needs a CPU -- and that works out,
// because deep sleep only ever happens on battery, which is the steady case.
static const int8_t PIN_STATUS_LED = 6;   // P1-3

// VBUS presence, and it answers a DIFFERENT QUESTION from PIN_CHARGE_STAT.
// STAT says "current is going into the cell"; this says "the unit is on mains".
// They diverge exactly when the battery is full: the charger terminates, STAT
// releases, and 5 V is still there. Publishing VBUS as `charging` would
// therefore claim a charge that finished hours ago.
//
//     P1-14 (5V) --[5k]--+-- P1-10 (IO10), INPUT_PULLDOWN
//                        |
//                     [10k]
//                        |
//                       GND
//
// A REAL DIVIDER, both legs external. That is what makes this sound rather than
// merely survivable, and the difference is worth stating because the tempting
// version -- one series resistor into the internal pull-down -- is neither.
//
//     5 x 10/15                    = 3.33 V   divider alone
//     5 x (10||45)/(5 + 10||45)    = 3.10 V   with the internal pull-down too
//     5 x (10||20)/(5 + 10||20)    = 2.86 V   even at a 20k internal pull-down
//
// Every case clears V_IH (2.48 V) and sits under the 3.6 V absolute maximum, so
// the ESD clamp is never called on and no current is injected into the 3V3 rail.
//
// WHY NOT ONE RESISTOR INTO THE INTERNAL PULL-DOWN. Because that resistance is
// published as "-- 45 --" kOhm: a typical with no minimum and no maximum. Sized
// for 3.0 V against 45k, the same resistor gives 2.0 V at 20k (reads LOW, misses
// mains entirely) and 4.1 V at 100k (over the maximum, clamping into the rail).
// There is no single value that is correct across the tolerance, which is the
// whole reason the lower leg has to be a resistor somebody chose.
//
// INPUT_PULLDOWN is kept rather than plain INPUT. It is redundant against the
// external resistor by design -- if that one is ever knocked off, the pin still
// reads a definite LOW instead of floating and reporting mains power that is
// not there.
//
// 5k above the divider rather than 10k: with the 10k lower leg fitted, 5k puts
// the node at 3.33 V where 10k would put it at 2.50 V -- and V_IH is 2.48 V,
// which is not a margin, it is a coin flip.
static const int8_t PIN_VBUS_SENSE = 10;  // P1-10

// Active HIGH: GPIO -> 220R -> LED anode, cathode to GND at P1-13.
//
// DRIVEN BY LEDC, NOT digitalWrite, so it can fade. Channel 0, which lands on
// timer 0 -- LovyanGFX has the backlight on channel 7 and arduino-esp32 2.0.17
// maps channels to timers as (chan >> 1) & 3, so 7 AND 6 both sit on timer 3.
// Touching either would re-time or steal the display's brightness.
//
// Against this project's usual common-anode convention, and for a physical
// reason rather than a preference: 3V3 exists at exactly ONE header position
// (P2-1) and the load cells already need it, because P1 carries no 3V3 at all.
// GND is available at three. Sourcing from the GPIO is what the connector
// allows. The board's own backlight is already active-high through an NPN, so
// the polarity is not foreign here.
static const bool STATUS_LED_ACTIVE_HIGH = true;

// The encoder's switch is the ONLY deep-sleep wake source this board has, and
// that is worth knowing before the power-management work starts rather than
// after. The touch controller cannot do it: TP_INT is GPIO46, which is outside
// the S3's RTC_GPIO range (0-21) AND whose net reaches no header, so it can
// neither be used for EXT0 nor re-routed to a pin that could. IO6 is RTC-
// capable, so SW can wake the chip; IO2 and IO4 are too, if turning the knob
// should also wake it.
static const bool ENC_SW_IS_WAKE_CAPABLE = true;

// ---------------------------------------------------------------------------
// 10. The 200 kg buffer-stock module -- a SECOND, separate bus
// ---------------------------------------------------------------------------
// One NAU7802 behind its own TCA9548A, built exactly like the counter's three and
// joined to the board by its own four-wire lead. It does NOT share the cell trunk
// of section 8, for three reasons that are each sufficient:
//
//   * IT IS THE SAME MODULE, so its mux is ALSO at 0x70. Two 0x70s on one trunk
//     both answer and both drive the bus. Sharing would mean re-strapping one to
//     0x71 and teaching the (deliberately one-mux) driver about two.
//   * A cable pulled, or a stub latched low, on the 200 kg lead must not be able
//     to take the counter's cells down -- or the other way round. The counter's
//     trunk has an undiagnosed dropout (see todo.md); a second measurement that
//     must not be lost should not be hanging on it.
//   * A long lead on a shared 400 kHz trunk eats the rise-time budget of every
//     cell on it.
//
// IO21/IO16 is the camera's SCCB pair, with R4/R5 (4.7k to 3V3) already fitted and
// nothing on J1 -- section 7 calls them the two most valuable free pins on the
// board, and this is what they were saved for. They are broken out on P1: SDA on
// P1-7, SCL on P1-4.
//
// IT IS BIT-BANGED, and that is forced rather than chosen. The S3 has two I2C
// controllers: port 0 is LovyanGFX's (touch + IMU), port 1 is the cell trunk.
// Port -1 is lgfx's first software slot -- the one the original two-cell build
// ran cell B on, at 100 kHz, so the mechanism is proven on this very board. One
// converter at 10 SPS asks very little of it.
//
// WIRING, CONFIRMED ON THE BENCH 2026-10-03 with a scan that tried both
// orientations: SDA=IO21 / SCL=IO16 answers (mux at 0x70, a NAU7802 at 0x2A on
// channel 0, revision register 0x0F); the swapped pair shows nothing. As with the
// trunk, a swap is silent rather than noisy, so that check is worth repeating
// after any rewiring.
//
// P1 HAS NO 3V3. The lead's VCC has to come from P2-1 (and GND from P2-2 or
// P1-13), so this lead splits across both headers -- the very thing section 8's
// move onto P2 was meant to end for the counter's. Accepted: the alternative was
// the trunk.
//
// The pull-ups are the board's R4/R5 PLUS the mux breakout's own, ~2.35k in
// parallel. That is ~1.4 mA of sink current, inside the 3 mA I2C allows, and a
// non-issue at 100 kHz.
static const int8_t BUF_SDA = CAM_SCCB_SDA;  // IO21, P1-7
static const int8_t BUF_SCL = CAM_SCCB_SCL;  // IO16, P1-4
static const int BUF_PORT = -1;              // lgfx bit-banged slot 0
static const uint8_t BUF_MUX_ADDR = 0x70;    // A0/A1/A2 grounded -- own bus, so no clash
static const int8_t BUF_MUX_CH = 0;          // the one cell is on channel 0

}  // namespace board
