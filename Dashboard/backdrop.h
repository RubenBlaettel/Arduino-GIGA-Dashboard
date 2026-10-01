#pragma once
// Screen background with the dial face on it: deep black, or while Spotify plays, its album cover
// slightly blurred and darkened. Changes cross-fade.
#include <stdint.h>
#include "lvgl.h"

void backdrop_create(lv_obj_t *parent);  // create first: lies below everything else; follows playback itself
// The dial face (RGB565, square) goes at (x, y). `mask` is the same face with its surfaces white
// and its markings black; the surfaces let a little of the background through. The face's round
// edge, a hairline of radius r from its centre in color `rim`, is anti-aliased onto the background.
// Both buffers may be freed afterwards.
void backdrop_set_dial(const lv_draw_buf_t *face, const lv_draw_buf_t *mask, int x, int y, int r, lv_color_t rim);
void backdrop_on_cover(uint32_t id);     // a cover is complete in cover_pixels()
