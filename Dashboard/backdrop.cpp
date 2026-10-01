// Screen background with the dial face on it, in one full-screen RGB565 canvas in SDRAM. The
// background is deep black, or while Spotify plays, its album cover slightly blurred and darkened;
// changes cross-fade. On every change the face is composed onto it: its surfaces let a little of
// the background through, its markings stay opaque, its round edge is anti-aliased here.
// (An ARGB8888 face read from SDRAM on every frame picked up stray bit errors on this board.)
//
// Background: the 96 px cover is reduced to a coarse grid, blurred and toned there, then stretched
// over the screen (bilinear) and dithered to RGB565.
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include "SDRAM.h"
#include "backdrop.h"
#include "config.h"
#include "model.h"
#include "cover.h"

// ---- look ------------------------------------------------------------------------------
// Coarse grid: one cell per 2x2 cover pixels, the cover's middle band cropped to the screen's 5:3.
// A cell spans ~17 screen pixels; that and one blur pass give the softness.
static const int GW = COVER_PX / 2, GH = 29;
static const int CROP_Y = (COVER_PX - 2 * GH) / 2;
static const int DIM = 120;         // brightness x DIM/256 ...
static const int LUMA_CAP = 50;     // ... but luma at most this (of 255): bright covers stay dark
static const int VALUE_CAP = 110;   // ... and no channel above this (saturated blue has little luma)
static const int SATURATION = 300;  // x/256: darkened colors turn muddy without a little boost
static const int FADE_MS = 600;
static const int SHOW_THROUGH = 128;  // x/256: the dial's surfaces let 50 % of the background through

struct Cell { uint16_t r, g, b; };  // 8.8 fixed point, 0..255

static lv_obj_t *canvas;
static Cell coverCells[GW * GH];  // the toned cover `coverId`
static Cell fromCells[GW * GH];   // on screen when the fade started
static Cell shownCells[GW * GH];  // on screen now; all 0 = black
static uint32_t coverId = 0;
static bool toCover = false;      // target of the running or last fade
static bool fading = false;
static uint32_t fadeStartMs;
// Screen -> grid: the outer cells' centres lie on the screen edges, so every pixel sits between two
// cells and the color changes linearly from pixel to pixel within a cell.
static uint8_t gx[SCREEN_W], gy[SCREEN_H];   // grid cell left of / above each screen pixel
static uint16_t wx[SCREEN_W], wy[SCREEN_H];  // weight of the next cell, 0..256
static int16_t cellEnd[GW];                  // first screen column right of cell gap i
static uint32_t dither[4][4];                // ordered dither thresholds for the RGB565 rounding
// The dial: per row, the face covers [holeL, holeR); its edge reaches into [bandL, bandR).
static int16_t holeL[SCREEN_H], holeR[SCREEN_H], bandL[SCREEN_H], bandR[SCREEN_H];
static int dialX, dialY, dialW, dialCx, dialCy, rimR;
static uint16_t rimColor;
static uint16_t *face;        // the face in color (SDRAM)
static uint8_t *surf;         // 1 bit per face pixel: a surface (internal RAM; SDRAM reads flip bits)
static int surfStride;        // bytes per row of `surf`

// ---- cover -> toned grid -----------------------------------------------------------------
static void grid_from_cover(const uint8_t *px) {
  for (int j = 0; j < GH; j++) {
    for (int i = 0; i < GW; i++) {
      uint32_t r = 0, g = 0, b = 0;
      for (int dy = 0; dy < 2; dy++) {
        for (int dx = 0; dx < 2; dx++) {
          int k = (CROP_Y + 2 * j + dy) * COVER_PX + 2 * i + dx;
          uint16_t p = px[2 * k] | px[2 * k + 1] << 8;  // RGB565 little-endian
          r += ((p >> 11) * 527 + 23) >> 6;
          g += (((p >> 5) & 63) * 259 + 33) >> 6;
          b += ((p & 31) * 527 + 23) >> 6;
        }
      }
      coverCells[j * GW + i] = {(uint16_t)(r << 6), (uint16_t)(g << 6), (uint16_t)(b << 6)};  // mean, 8.8
    }
  }
}

// Binomial blur (1 4 6 4 1) along one row or column, edges clamped.
static void blur_line(Cell *c, int n, int stride) {
  static const uint8_t K[5] = {1, 4, 6, 4, 1};
  Cell line[GW > GH ? GW : GH];
  for (int i = 0; i < n; i++) line[i] = c[i * stride];
  for (int i = 0; i < n; i++) {
    uint32_t r = 0, g = 0, b = 0;
    for (int t = 0; t < 5; t++) {
      const Cell &s = line[constrain(i + t - 2, 0, n - 1)];
      r += s.r * K[t];
      g += s.g * K[t];
      b += s.b * K[t];
    }
    c[i * stride] = {(uint16_t)(r >> 4), (uint16_t)(g >> 4), (uint16_t)(b >> 4)};
  }
}

// A little more saturation, then darker: DIM, capped by luma and by the brightest channel.
static void tone(Cell &c) {
  int32_t r = c.r, g = c.g, b = c.b;
  int32_t y = (54 * r + 183 * g + 19 * b) >> 8;
  r = max(0L, y + ((r - y) * SATURATION >> 8));
  g = max(0L, y + ((g - y) * SATURATION >> 8));
  b = max(0L, y + ((b - y) * SATURATION >> 8));
  y = (54 * r + 183 * g + 19 * b) >> 8;
  int32_t v = max(r, max(g, b));
  int32_t k = DIM;  // x/256
  if (y * k > LUMA_CAP * 65536L) k = LUMA_CAP * 65536L / y;
  if (v * k > VALUE_CAP * 65536L) k = VALUE_CAP * 65536L / v;
  c = {(uint16_t)(r * k >> 8), (uint16_t)(g * k >> 8), (uint16_t)(b * k >> 8)};
}

// ---- grid -> screen ------------------------------------------------------------------------
static void axis(int n, int cells, uint8_t *idx, uint16_t *w) {
  for (int x = 0; x < n; x++) {
    int32_t u = (int32_t)x * (cells - 1) * 256 / (n - 1);  // position in cells, 8.8
    int i = min(u >> 8, (int32_t)cells - 2);
    idx[x] = i;
    w[x] = u - i * 256;
  }
}

static inline uint16_t pack(int32_t R, int32_t G, int32_t B, uint32_t t) {
  return (uint32_t)(R + t) >> 16 << 11 | (uint32_t)(G + t) >> 16 << 5 | (uint32_t)(B + t) >> 16;
}

// Pixels [x0, x1) of one row: the background, or with `fc` (face row) and `sf` (its surface bits),
// the dial face whose surfaces let SHOW_THROUGH of the background through. Within a cell gap the
// background is stepped from pixel to pixel, kept scaled to the output's 5/6/5 bits with 16
// fraction bits (then the dither threshold rounds it).
static void render_span(uint16_t *row, const Cell *col, const uint32_t *d, int x0, int x1,
                        const uint16_t *fc = nullptr, const uint8_t *sf = nullptr) {
  static const int32_t KEEP = (256 - SHOW_THROUGH) * 256;  // face share, in the scaled units
  for (int x = x0; x < x1;) {
    int i = gx[x], end = min((int)cellEnd[i], x1);
    const Cell &l = col[i], &r = col[i + 1];
    int32_t dr = r.r - l.r, dg = r.g - l.g, db = r.b - l.b, w = wx[x];
    int32_t R = l.r * 31 + (dr * 31 * w >> 8), G = l.g * 63 + (dg * 63 * w >> 8), B = l.b * 31 + (db * 31 * w >> 8);
    int32_t sR = dr * 31 * (GW - 1) / (SCREEN_W - 1), sG = dg * 63 * (GW - 1) / (SCREEN_W - 1),
            sB = db * 31 * (GW - 1) / (SCREEN_W - 1);
    for (; x < end; x++, R += sR, G += sG, B += sB) {
      uint32_t t = d[x & 3];
      if (!fc) {
        row[x] = pack(R, G, B, t);
        continue;
      }
      int k = x - dialX;
      uint16_t f = fc[k];
      if (sf[k >> 3] >> (k & 7) & 1) {
        row[x] = pack((f >> 11) * KEEP + (R * SHOW_THROUGH >> 8), ((f >> 5) & 63) * KEEP + (G * SHOW_THROUGH >> 8),
                      (f & 31) * KEEP + (B * SHOW_THROUGH >> 8), t);
      } else {
        row[x] = f;
      }
    }
  }
}

static uint16_t mix565(uint16_t a, uint16_t b, uint32_t w) {  // w/256 of a
  uint32_t v = 256 - w;
  return ((a >> 11) * w + (b >> 11) * v + 128) >> 8 << 11 | (((a >> 5) & 63) * w + ((b >> 5) & 63) * v + 128) >> 8 << 5 |
         ((a & 31) * w + (b & 31) * v + 128) >> 8;
}

// The dial's edge, a hairline of radius rimR, over the background in [x0, x1) of row y.
static void blend_rim(uint16_t *row, int y, int x0, int x1) {
  float dy = y - dialCy;
  for (int x = x0; x < x1; x++) {
    float dx = x - dialCx;
    float cover = rimR + 1 - sqrtf(dx * dx + dy * dy);  // 1 on the hairline, 0 past its edge
    if (cover > 0) row[x] = mix565(rimColor, row[x], cover >= 1 ? 256 : (uint32_t)(cover * 256));
  }
}

static void render() {
  static uint16_t row[SCREEN_W];  // one row in internal RAM, then copied to SDRAM in one go
  lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
  for (int y = 0; y < SCREEN_H; y++) {
    Cell col[GW];  // the grid interpolated to this row
    const Cell *a = shownCells + gy[y] * GW, *b = a + GW;
    int32_t w = wy[y];
    for (int i = 0; i < GW; i++) {
      col[i] = {(uint16_t)(a[i].r + ((b[i].r - a[i].r) * w >> 8)), (uint16_t)(a[i].g + ((b[i].g - a[i].g) * w >> 8)),
                (uint16_t)(a[i].b + ((b[i].b - a[i].b) * w >> 8))};
    }
    int l = holeL[y], r = holeR[y];
    render_span(row, col, dither[y & 3], 0, l);
    render_span(row, col, dither[y & 3], r, SCREEN_W);
    blend_rim(row, y, bandL[y], l);
    blend_rim(row, y, r, bandR[y]);
    uint8_t *dst = db->data + y * db->header.stride;
    if (l < r && face) {
      int j = y - dialY;
      render_span(row, col, dither[y & 3], l, r, face + j * dialW, surf + j * surfStride);
      memcpy(dst, row, SCREEN_W * 2);
    } else {  // no face to compose: keep what the canvas has there
      memcpy(dst, row, l * 2);
      memcpy(dst + r * 2, row + r, (SCREEN_W - r) * 2);
    }
  }
  lv_image_cache_drop(db);
}

// ---- fade --------------------------------------------------------------------------------
static void start_fade(bool cover) {
  toCover = cover;
  memcpy(fromCells, shownCells, sizeof(shownCells));
  fadeStartMs = millis();
  fading = true;
}

static void fade_step() {
  uint32_t t = (millis() - fadeStartMs) * 256 / FADE_MS;
  if (t >= 256) {
    t = 256;
    fading = false;
  }
  int32_t e = t * t * (768 - 2 * t) >> 16;  // smoothstep, 0..256
  for (int k = 0; k < GW * GH; k++) {
    const Cell &f = fromCells[k];
    Cell to = toCover ? coverCells[k] : Cell{0, 0, 0};
    shownCells[k] = {(uint16_t)(f.r + ((to.r - f.r) * e >> 8)), (uint16_t)(f.g + ((to.g - f.g) * e >> 8)),
                     (uint16_t)(f.b + ((to.b - f.b) * e >> 8))};
  }
  render();
  lv_obj_invalidate(canvas);
}

// ---- public ------------------------------------------------------------------------------
void backdrop_set_dial(const lv_draw_buf_t *faceBuf, const lv_draw_buf_t *mask, int x, int y, int r, lv_color_t rim) {
  if (!canvas) return;
  lv_draw_buf_t *db = lv_canvas_get_draw_buf(canvas);
  int w = faceBuf->header.w, h = faceBuf->header.h;
  for (int j = 0; j < h; j++) memcpy(db->data + (y + j) * db->header.stride + x * 2, faceBuf->data + j * faceBuf->header.stride, w * 2);
  dialX = x;
  dialY = y;
  dialW = w;
  surfStride = (w + 7) / 8;
  face = (uint16_t *)SDRAM.malloc(w * h * 2);
  surf = (uint8_t *)calloc(surfStride * h, 1);
  if (face && surf) {
    for (int j = 0; j < h; j++) {
      memcpy(face + j * w, faceBuf->data + j * faceBuf->header.stride, w * 2);
      const uint16_t *m = (const uint16_t *)(mask->data + j * mask->header.stride);
      for (int i = 0; i < w; i++)
        if (m[i] & 0x0400) surf[j * surfStride + i / 8] |= 1 << (i & 7);  // green >= 50 %: surface
    }
  } else {  // the face stays as copied, opaque
    if (face) SDRAM.free(face);
    free(surf);
    face = nullptr;
  }
  dialCx = x + w / 2;
  dialCy = y + h / 2;
  rimR = r;
  rimColor = lv_color_to_u16(rim);
  float in = r - 0.5f, out = r + 1.0f;  // the face is kept within `in`, its edge fades out until `out`
  for (int yy = 0; yy < SCREEN_H; yy++) {
    int dy = yy - dialCy;
    holeL[yy] = holeR[yy] = bandL[yy] = bandR[yy] = 0;
    if (abs(dy) >= out) continue;
    int ho = (int)ceilf(sqrtf(out * out - dy * dy));
    bandL[yy] = dialCx - ho;
    bandR[yy] = dialCx + ho + 1;
    int hi = abs(dy) <= in ? (int)sqrtf(in * in - dy * dy) : -1;
    holeL[yy] = hi < 0 ? dialCx : dialCx - hi;
    holeR[yy] = hi < 0 ? dialCx : dialCx + hi + 1;
  }
  render();
  lv_obj_invalidate(canvas);
}

void backdrop_on_cover(uint32_t id) {
  if (!canvas || id == coverId) return;  // the same cover again (resend)
  grid_from_cover(cover_pixels());
  for (int j = 0; j < GH; j++) blur_line(coverCells + j * GW, GW, 1);
  for (int i = 0; i < GW; i++) blur_line(coverCells + i, GH, GW);
  for (Cell &c : coverCells) tone(c);
  coverId = id;
  if (toCover) start_fade(true);  // cross-fade from the previous cover
}

// Follows playback and runs the fade, at about the display's refresh rate (LV_DEF_REFR_PERIOD).
static void on_timer(lv_timer_t *t) {
  bool want = toCover;  // a new cover still on its way: keep the previous one meanwhile
  if (!M.playing || !pcConnected() || M.cover == 0) want = false;
  else if (M.cover == coverId) want = true;
  if (want != toCover) start_fade(want);
  if (fading) fade_step();
}

void backdrop_create(lv_obj_t *parent) {
  static const uint8_t BAYER[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
  for (int y = 0; y < 4; y++)
    for (int x = 0; x < 4; x++) dither[y][x] = BAYER[y][x] * 4096 + 2048;
  axis(SCREEN_W, GW, gx, wx);
  axis(SCREEN_H, GH, gy, wy);
  for (int x = 0; x < SCREEN_W; x++) cellEnd[gx[x]] = x + 1;
  uint8_t *buf = (uint8_t *)SDRAM.malloc(LV_CANVAS_BUF_SIZE(SCREEN_W, SCREEN_H, 16, LV_DRAW_BUF_STRIDE_ALIGN));
  if (!buf) return;
  canvas = lv_canvas_create(parent);
  lv_canvas_set_buffer(canvas, buf, SCREEN_W, SCREEN_H, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(canvas, 0, 0);
  render();  // black until the dial arrives
  lv_timer_create(on_timer, 30, nullptr);
}
