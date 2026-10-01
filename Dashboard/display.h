#pragma once
// Display glue on top of Arduino_H7_Video (no touch: the shield's touch bus is defective).
#include <stddef.h>

void display_begin();         // Display.begin(), extra LVGL pool, own flush callback
size_t heap_largest_block();  // largest malloc() block in internal RAM (diagnostics)
int display_rotation_deg();   // 90 (flipped) or 270 (Arduino default), for screenshots
