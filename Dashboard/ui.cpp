#include <Arduino.h>
#include "ui.h"
#include "theme.h"
#include "backdrop.h"

static void on_tick(lv_timer_t *t) {
  clock_tick();
  panel_tick();
}

void ui_init() {
  lv_obj_t *scr = lv_screen_active();
  lv_obj_set_style_bg_color(scr, C_SCHWARZ, 0);
  lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
  lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
  backdrop_create(scr);
  clock_create(scr);
  panel_create(scr);
  lv_timer_create(on_tick, 50, nullptr);
}

void ui_on_sensors() { clock_on_sensors(); }
void ui_on_loads() { panel_on_loads(); }
void ui_on_media() { panel_on_media(); }
void ui_on_cover(uint32_t id) {
  panel_on_cover(id);
  backdrop_on_cover(id);
}
