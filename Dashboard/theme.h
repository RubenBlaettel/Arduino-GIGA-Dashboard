#pragma once
// Design tokens. Every color and font in the UI comes from here.
#include "lvgl.h"
#include "src/fonts/fonts.h"

// Palette: a night-blue lacquered watch dial with luminous cream, on black (or, while Spotify
// plays, on its blurred album cover: backdrop.cpp).
#define C_SCHWARZ     lv_color_hex(0x000000)  // screen background
#define C_LACK        lv_color_hex(0x0A1522)  // rim around the hands, centre pin hole
#define C_ZIFFERBLATT lv_color_hex(0x102036)  // dial face
#define C_REGISTER    lv_color_hex(0x0C1A2C)  // recessed registers
#define C_TEILUNG     lv_color_hex(0x26395A)  // hairlines, minor ticks
#define C_LEUCHT      lv_color_hex(0xECE5D0)  // hands, numerals, primary text
#define C_SCHIEFER    lv_color_hex(0x8290AA)  // secondary text, labels
#define C_BERNSTEIN   lv_color_hex(0xF2A33A)  // caution
#define C_SIGNALROT   lv_color_hex(0xF2544B)  // hot
#define C_AKZENT      lv_color_hex(0x4DB5EB)  // seconds hand and digital seconds

// Type roles (Bahnschrift = DIN 1451)
#define F_TIME   (&font_time_120)   // display: digital time
#define F_NUM48  (&font_num_48)     // display: seconds, dial "12"
#define F_NUM40  (&font_num_40)     // display: register values
#define F_TITLE  (&font_title_34)   // body: weekday
#define F_BODY   (&font_body_24)    // body: date
#define F_UI     (&font_ui_22)      // utility: hint line, now playing (Latin-1 + Latin Extended-A)
#define F_LABEL  (&font_label_18)   // utility: register names

lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, lv_color_t color, const char *text);
