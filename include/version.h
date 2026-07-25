// Device identity and firmware version.

#pragma once

// Identity belongs to the INSTALLATION, not the chip. If a board fails and is
// swapped, the replacement must keep reporting the same ID so the server sees
// continuity rather than a new device -- which is exactly why this is not
// derived from the eFuse MAC. The MAC is reported alongside it for hardware
// traceability: which physical board is currently at this location.
//
// Override per unit from platformio.ini without editing this file:
//     build_flags = -DBOWLSTACK_DEVICE_ID='"BWL-007"'
#ifndef BOWLSTACK_DEVICE_ID
#define BOWLSTACK_DEVICE_ID "BWL-000"
#endif

#ifndef BOWLSTACK_FW_VERSION
#define BOWLSTACK_FW_VERSION "0.2.0"
#endif
