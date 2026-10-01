#pragma once
// Dashboard configuration. Colors and fonts live in theme.h.

#define FW_VERSION "2.3"

#define SCREEN_W 800
#define SCREEN_H 480

// Landscape orientation of the portrait panel: 0 = Arduino default (270 degrees),
// 1 = turned by 180 degrees (90 degrees). Ruben's desk setup uses 1.
#define DISPLAY_FLIPPED 1

// Temperature scales of the three chronograph registers (degrees C).
struct SensorScale {
  int min, max;   // scale range, 270 degree sweep
  int warn, hot;  // amber from warn, red from hot
};
static const SensorScale SCALE_CPU = {20, 100, 80, 90};
static const SensorScale SCALE_GPU = {20, 100, 75, 85};
static const SensorScale SCALE_RAM = {20, 80, 55, 65};

// Album cover of the song playing on the PC: square, RGB565, sent by the bridge in base64 chunks.
#define COVER_PX 96
#define COVER_CHUNK 144                                   // raw bytes per chunk (192 base64 chars)
#define COVER_CHUNKS (COVER_PX * COVER_PX * 2 / COVER_CHUNK)  // 128

// The bridge sends time every second; after this long without data the UI says so.
#define PC_TIMEOUT_MS 5000

// Without the PC the clock runs on the RTC. An RTC time before this (2025-01-01) means the RTC
// lost power and was reset, so the clock waits for the PC instead.
#define RTC_VALID_FROM 1735689600L
