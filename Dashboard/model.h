#pragma once
// State mirrored from the PC bridge. Written by link.cpp, read by the UI.
#include <stdint.h>

struct Model {
  uint32_t lastRxMs;
  bool everConnected;
  bool timeValid;  // the RTC holds a real time (set by the PC, or kept across a reset)
  float temp[3];  // 0 cpu, 1 gpu, 2 ram in degrees C; NAN when unknown
  int load[3];    // 0 cpu, 1 gpu, 2 ram in percent; -1 when unknown
  // Now playing (Spotify on the PC). Only valid while `playing`.
  bool playing;
  uint32_t cover;  // id of the album cover (CRC32 of its pixels), 0 = none
  char title[128];
  char artist[128];
};

extern Model M;

bool pcConnected();
