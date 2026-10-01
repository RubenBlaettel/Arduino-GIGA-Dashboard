#include <Arduino.h>
#include <mbed.h>
#include <math.h>
#include <stdarg.h>
#include "dsi.h"  // Arduino_H7_Video: framebuffer access for screenshots
#include "lvgl.h"
#include "config.h"
#include "link.h"
#include "model.h"
#include "ui.h"
#include "diag.h"
#include "display.h"
#include "cover.h"

Model M;

static char rxBuf[384];  // longest line: P with title and artist (the bridge caps each at 120 bytes)
static size_t rxLen = 0;
static uint32_t lastHelloMs = 0;
static bool helloSent = false;
static bool clockCheck = true;  // report the RTC's deviation on the next T (after boot or a connection loss)
static uint32_t offlineS = 0;   // how long the PC was silent before that

bool pcConnected() {
  return M.everConnected && (millis() - M.lastRxMs) < PC_TIMEOUT_MS;
}

void link_begin() {
  Serial.begin(115200);
  for (int i = 0; i < 3; i++) {
    M.temp[i] = NAN;
    M.load[i] = -1;
  }
  // The RTC keeps running across resets (and power loss with a VRTC coin cell): show its time
  // right away, the PC corrects it with its first T. First call also starts the RTC (LSE).
  M.timeValid = time(nullptr) >= RTC_VALID_FROM;
}

void link_sendf(const char *fmt, ...) {
  if (!Serial) return;  // no host has the port open; don't block
  char line[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);
  Serial.print(line);
  Serial.print('\n');
}

static bool unknown(const char *s) { return s[0] == '-' && s[1] == '\0'; }

static float parseTemp(const char *s) { return unknown(s) ? NAN : atof(s); }

// P: the song Spotify plays; without fields: nothing plays.
static void set_media(char **f, int n) {
  bool playing = n >= 4;
  uint32_t cover = playing && !unknown(f[1]) ? strtoul(f[1], nullptr, 16) : 0;
  const char *title = playing ? f[2] : "", *artist = playing ? f[3] : "";
  if (playing == M.playing && cover == M.cover && !strncmp(title, M.title, sizeof(M.title) - 1) &&
      !strncmp(artist, M.artist, sizeof(M.artist) - 1))
    return;
  M.playing = playing;
  M.cover = cover;
  snprintf(M.title, sizeof(M.title), "%s", title);
  snprintf(M.artist, sizeof(M.artist), "%s", artist);
  ui_on_media();
}

// Debug aid: stream the raw RGB565 framebuffer (physical portrait orientation) to the PC.
static void send_screenshot() {
  diag_phase(PH_SHOT);
  uint32_t w = dsi_getDisplayXSize(), h = dsi_getDisplayYSize();
  const uint8_t *fb = (const uint8_t *)dsi_getActiveFrameBuffer();
  link_sendf("SHOT|%lu|%lu|%d", (unsigned long)w, (unsigned long)h, display_rotation_deg());
  size_t total = (size_t)w * h * 2;
  for (size_t off = 0; off < total; off += 4096) {
    diag_phase(PH_SHOT);  // a slow stream (every other one takes ~9 s) must not trip the watchdog
    Serial.write(fb + off, min((size_t)4096, total - off));
  }
}

// The RTC runs the clock; the PC only corrects it. Local time, kept as if UTC.
static void set_clock(time_t pc) {
  if (clockCheck) {
    clockCheck = false;
    if (M.timeValid) link_sendf("SYNC|%ld|%lu", (long)(time(nullptr) - pc), (unsigned long)offlineS);
    else link_sendf("SYNC|-|%lu", (unsigned long)offlineS);
  }
  set_time(pc);
  M.timeValid = true;
}

static void handle(char **f, int n) {
  const char *cmd = f[0];
  if (!strcmp(cmd, "T") && n >= 2) {
    set_clock((time_t)strtoul(f[1], nullptr, 10));
  } else if (!strcmp(cmd, "S") && n >= 4) {
    for (int i = 0; i < 3; i++) M.temp[i] = parseTemp(f[1 + i]);
    ui_on_sensors();
  } else if (!strcmp(cmd, "L") && n >= 4) {
    for (int i = 0; i < 3; i++) M.load[i] = unknown(f[1 + i]) ? -1 : constrain(atoi(f[1 + i]), 0, 100);
    ui_on_loads();
  } else if (!strcmp(cmd, "P")) {
    set_media(f, n);
  } else if (!strcmp(cmd, "A") && n >= 4) {
    uint32_t id = strtoul(f[1], nullptr, 16);
    if (cover_chunk(id, atoi(f[2]), f[3])) {
      link_sendf("COVER|%08lx", (unsigned long)id);
      ui_on_cover(id);
    }
  } else if (!strcmp(cmd, "SHOT")) {
    send_screenshot();
  } else if (!strcmp(cmd, "MEM")) {
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    link_sendf("MEM|%u|%u|%u|%u|%u|%u|%u", (unsigned)mon.total_size, (unsigned)mon.free_size,
               (unsigned)mon.free_biggest_size, (unsigned)mon.max_used, mon.used_pct, mon.frag_pct,
               (unsigned)heap_largest_block());
  } else if (!strcmp(cmd, "DIAG") && n >= 2) {  // test the crash/hang recovery
    if (!strcmp(f[1], "hang")) for (;;) {}
    if (!strcmp(f[1], "fault")) *(volatile uint32_t *)0xFFFFFFF0u = 1;  // bus fault
  }
}

static void dispatch(char *line) {
  char *fields[8];
  int n = 0;
  fields[n++] = line;
  for (char *p = line; *p && n < 8; p++) {
    if (*p == '|') {
      *p = '\0';
      fields[n++] = p + 1;
    }
  }
  if (!pcConnected()) {  // first line after boot or after a connection loss
    clockCheck = true;
    offlineS = (millis() - (M.everConnected ? M.lastRxMs : 0)) / 1000;
  }
  M.lastRxMs = millis();
  M.everConnected = true;
  // Report the last reset reason once per boot, even if the bridge was quicker than our HELLO.
  if (!helloSent) {
    helloSent = true;
    link_sendf("HELLO|%s|%s", FW_VERSION, diag_last_reset());
  }
  handle(fields, n);
}

void link_poll() {
  // A cover arrives as ~26 KB in a burst; take it in slices so the UI keeps running.
  for (int budget = 2048; budget > 0 && Serial.available() > 0; budget--) {
    int c = Serial.read();
    if (c < 0) break;
    if (c == '\n') {
      rxBuf[rxLen] = '\0';
      if (rxLen && rxBuf[rxLen - 1] == '\r') rxBuf[rxLen - 1] = '\0';
      if (rxLen) dispatch(rxBuf);
      rxLen = 0;
    } else if (rxLen < sizeof(rxBuf) - 1) {
      rxBuf[rxLen++] = (char)c;
    } else {
      rxLen = 0;  // overlong line: drop it
    }
  }
  // Ask for data while the bridge is silent (after boot or reconnect).
  if (!pcConnected() && millis() - lastHelloMs > 3000) {
    lastHelloMs = millis();
    if (Serial) helloSent = true;  // counts only once a host has the port open
    link_sendf("HELLO|%s|%s", FW_VERSION, diag_last_reset());
  }
}
