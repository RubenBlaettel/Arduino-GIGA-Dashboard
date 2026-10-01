#include <Arduino.h>
#include <string.h>
#include "SDRAM.h"
#include "config.h"
#include "cover.h"

static uint8_t *buf;         // chunks of the cover being received
static uint32_t rxId = 0;    // id of that cover
static uint32_t got[(COVER_CHUNKS + 31) / 32];
static int gotCount = 0;

void cover_begin() {
  buf = (uint8_t *)SDRAM.malloc(COVER_CHUNKS * COVER_CHUNK);
}

static int b64val(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return -1;
}

// Decodes exactly `n` bytes (n divisible by 3, no padding). False on malformed input.
static bool b64decode(const char *s, uint8_t *out, int n) {
  for (int i = 0; i < n; i += 3, s += 4) {
    int a = b64val(s[0]), b = b64val(s[1]), c = b64val(s[2]), d = b64val(s[3]);
    if ((a | b | c | d) < 0) return false;
    uint32_t v = (a << 18) | (b << 12) | (c << 6) | d;
    out[i] = v >> 16;
    out[i + 1] = v >> 8;
    out[i + 2] = v;
  }
  return s[0] == '\0';
}

bool cover_chunk(uint32_t id, int index, const char *b64) {
  if (!buf || id == 0 || index < 0 || index >= COVER_CHUNKS) return false;
  if (id != rxId) {  // a new cover starts
    rxId = id;
    memset(got, 0, sizeof(got));
    gotCount = 0;
  }
  uint32_t bit = 1u << (index & 31);
  if (got[index / 32] & bit) return gotCount == COVER_CHUNKS && index == COVER_CHUNKS - 1;  // resend
  if (!b64decode(b64, buf + index * COVER_CHUNK, COVER_CHUNK)) return false;
  got[index / 32] |= bit;
  return ++gotCount == COVER_CHUNKS;
}

const uint8_t *cover_pixels() { return buf; }
