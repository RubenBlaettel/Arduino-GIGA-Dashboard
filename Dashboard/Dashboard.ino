// GIGA Dashboard: chronograph clock with the PC's CPU, GPU and RAM temperatures and loads,
// plus the song Spotify plays on the PC.
// Board: Arduino GIGA R1 WiFi + GIGA Display Shield (landscape 800x480, touch not used).
// Needs the PC bridge (bridge/bridge.py) on the USB port. Build and upload: tools/build.ps1 -Upload
#include "Arduino_H7_Video.h"  // first: provides lv_conf.h to the library discovery
#include "lvgl.h"
#include "config.h"
#include "display.h"
#include "ui.h"
#include "link.h"
#include "diag.h"
#include "cover.h"

void setup() {
  link_begin();
  display_begin();
  cover_begin();
  ui_init();
  diag_begin();
}

void loop() {
  diag_phase(PH_LINK);
  link_poll();
  diag_phase(PH_LVGL);
  lv_timer_handler();
  diag_phase(PH_IDLE);
  delay(2);
}
