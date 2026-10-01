#pragma once
// A single screen: the chronograph dial with temperature registers on the left; now playing,
// date, digital time and the PC's load in the right column.
#include <stdint.h>
#include "lvgl.h"

void ui_init();
void ui_on_sensors();           // called by link.cpp when fresh temperatures arrived
void ui_on_loads();             // ... fresh loads arrived
void ui_on_media();             // ... the song changed (or the music stopped)
void ui_on_cover(uint32_t id);  // ... a cover is complete in cover_pixels()

// page_clock.cpp: dial, temperature registers, hands
void clock_create(lv_obj_t *parent);
void clock_tick();  // every 50 ms
void clock_on_sensors();

// panel.cpp: right column
void panel_create(lv_obj_t *parent);
void panel_tick();  // every 50 ms
void panel_on_loads();
void panel_on_media();
void panel_on_cover(uint32_t id);
