#pragma once
// Crash and hang diagnostics that survive a reset (a NOLOAD RAM section in SRAM4).
// - A fatal mbed error (HardFault, stack overflow, ...) stores status/PC/LR and reboots.
// - A soft watchdog thread reboots when loop() stops running for 8 s (e.g. an LVGL assert
//   spinning in while(1), or a USB write the PC never reads).
// The reason is reported to the bridge in HELLO and ends up in bridge/logs/bridge.log.
#include <stdint.h>

enum DiagPhase : uint16_t {
  PH_SETUP = 1,
  PH_LINK = 2,    // link_poll(): serial parsing and UI updates from PC data
  PH_LVGL = 3,    // lv_timer_handler(): rendering, input, animations
  PH_IDLE = 4,
  PH_SHOT = 5,    // streaming a screenshot
};

void diag_begin();                 // call at the end of setup()
void diag_phase(uint16_t phase);   // breadcrumb + heartbeat
const char *diag_last_reset();     // "" after a clean start
