#include <Arduino.h>
#include "Arduino_H7_Video.h"
#include "dsi.h"
#include "lvgl.h"
#include "config.h"
#include "display.h"

static Arduino_H7_Video Display(SCREEN_W, SCREEN_H, GigaDisplayShield);

// The core's lv_conf gives LVGL a 64 KB heap and halts (while(1)) when it runs out. Add a second
// pool. LVGL's TLSF is sized for LV_MEM_SIZE, so lv_mem_add_pool() silently rejects pools > 64 KB.
static uint8_t lvPoolFast[64 * 1024] __attribute__((aligned(8)));

// Arduino_H7_Video's own flush realloc()s a 4-byte-per-pixel buffer on every flush; a failed or
// stale buffer made lv_draw_sw_rotate HardFault. Same rotation here, into one fixed buffer sized to
// LVGL's partial draw buffer (1/10 screen, RGB565).
static uint8_t rotBuf[SCREEN_W * SCREEN_H / 10] __attribute__((aligned(32)));

static void flush_rotated(lv_display_t *disp, const lv_area_t *area, uint8_t *px) {
  int32_t w = lv_area_get_width(area), h = lv_area_get_height(area);
  if ((size_t)(w * h * 2) <= sizeof(rotBuf)) {
    // Logical landscape area -> physical portrait framebuffer (480 wide, 800 high).
    lv_display_rotation_t rot = lv_display_get_rotation(disp);
    lv_draw_sw_rotate(px, rotBuf, w, h, w * 2, h * 2, rot, LV_COLOR_FORMAT_RGB565);
    int32_t x, y;  // top-left of the rotated block in the framebuffer
    if (rot == LV_DISPLAY_ROTATION_90) {   // logical (lx, ly) -> physical (ly, 799 - lx)
      x = area->y1;
      y = lv_display_get_horizontal_resolution(disp) - area->x2 - 1;
    } else {                               // 270: logical (lx, ly) -> physical (479 - ly, lx)
      x = lv_display_get_vertical_resolution(disp) - area->y2 - 1;
      y = area->x1;
    }
    uint32_t offset = (x + dsi_getDisplayXSize() * y) * 2;
    dsi_lcdDrawImage(rotBuf, (void *)(dsi_getActiveFrameBuffer() + offset), h, w, DMA2D_INPUT_RGB565);
  }
  lv_display_flush_ready(disp);
}

int display_rotation_deg() {
  return lv_display_get_rotation(lv_display_get_default()) == LV_DISPLAY_ROTATION_90 ? 90 : 270;
}

void display_begin() {
  Display.begin();  // lv_init(), SDRAM.begin(), LVGL display with 270 degree rotation
  lv_mem_add_pool(lvPoolFast, sizeof(lvPoolFast));
  lv_display_t *disp = lv_display_get_default();
  if (DISPLAY_FLIPPED) lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_90);  // still 800x480 logical
  lv_display_set_flush_cb(disp, flush_rotated);
}

size_t heap_largest_block() {
  size_t lo = 0, hi = 512 * 1024;
  while (hi - lo > 1024) {
    size_t mid = (lo + hi) / 2;
    void *p = malloc(mid);
    if (p) { free(p); lo = mid; } else hi = mid;
  }
  return lo;
}
