// The chronograph dial whose three registers are the PC's temperatures (CPU, GPU, RAM).
// The right column (now playing, date, digital time, loads) lives in panel.cpp.
#include <Arduino.h>
#include <mbed.h>
#include <math.h>
#include <time.h>
#include "SDRAM.h"
#include "ui.h"
#include "theme.h"
#include "config.h"
#include "model.h"
#include "backdrop.h"

// ---- geometry (tile coordinates) -------------------------------------------
static const int DIAL = 440;              // dial canvas size
static const int DX = 24, DY = 20;        // dial canvas position
static const int C = DIAL / 2;            // dial centre inside the canvas
static const int R_FACE = 218;
static const int REG_OFF = 106, REG_R = 62;

struct HandSpec { int back, len, width; };
static const HandSpec H_HOUR = {16, 112, 10};
static const HandSpec H_MIN = {20, 176, 7};
static const HandSpec H_SEC = {40, 196, 2};

struct Register {
  int cx, cy;                 // centre inside the dial canvas
  const SensorScale *scale;
  const char *name;
  lv_obj_t *value;            // numeric readout in the register centre
  float shown;                // pointer position (animated), NAN = no data
  float target;
};
static Register regs[3] = {
  {C - REG_OFF, C, &SCALE_CPU, "CPU", nullptr, NAN, NAN},
  {C + REG_OFF, C, &SCALE_GPU, "GPU", nullptr, NAN, NAN},
  {C, C + REG_OFF, &SCALE_RAM, "RAM", nullptr, NAN, NAN},
};

static lv_obj_t *regLayer, *handLayer;
static float angH = 305.0f, angM = 63.5f, angS = 210.0f;  // rest pose 10:10:35 while no time is known
static int lastSec = -1;

// ---- drawing helpers ----------------------------------------------------------
static lv_point_t polar(float cx, float cy, float deg, float r) {
  float a = deg * (float)M_PI / 180.0f;
  lv_point_t p;
  p.x = (int32_t)lroundf(cx + r * sinf(a));
  p.y = (int32_t)lroundf(cy - r * cosf(a));
  return p;
}

static void draw_line(lv_layer_t *L, lv_point_t a, lv_point_t b, int w, lv_color_t c, bool round = false) {
  lv_draw_line_dsc_t d;
  lv_draw_line_dsc_init(&d);
  d.p1.x = a.x; d.p1.y = a.y;
  d.p2.x = b.x; d.p2.y = b.y;
  d.width = w;
  d.color = c;
  d.round_start = round;
  d.round_end = round;
  lv_draw_line(L, &d);
}

static void draw_disc(lv_layer_t *L, int x, int y, int r, lv_color_t c) {
  lv_draw_rect_dsc_t d;
  lv_draw_rect_dsc_init(&d);
  d.bg_color = c;
  d.bg_opa = LV_OPA_COVER;
  d.radius = LV_RADIUS_CIRCLE;
  lv_area_t a = {x - r, y - r, x + r, y + r};
  lv_draw_rect(L, &d, &a);
}

// Arc between two clock angles (0 = 12 o'clock, clockwise).
static void draw_ring(lv_layer_t *L, int x, int y, int r, int w, lv_color_t c, float from = 0, float to = 360) {
  lv_draw_arc_dsc_t d;
  lv_draw_arc_dsc_init(&d);
  d.center.x = x;
  d.center.y = y;
  d.radius = r;
  d.width = w;
  d.color = c;
  if (to - from >= 360.0f) {
    d.start_angle = 0;
    d.end_angle = 360;
  } else {
    d.start_angle = (int32_t)lroundf(fmodf(from + 270.0f, 360.0f));  // LVGL: 0 deg = 3 o'clock
    d.end_angle = (int32_t)lroundf(fmodf(to + 270.0f, 360.0f));
  }
  lv_draw_arc(L, &d);
}

static void draw_text(lv_layer_t *L, const char *t, const lv_font_t *f, lv_color_t c, int cx, int cy, int ls = 0) {
  lv_draw_label_dsc_t d;
  lv_draw_label_dsc_init(&d);
  d.text = t;
  d.font = f;
  d.color = c;
  d.align = LV_TEXT_ALIGN_CENTER;
  d.letter_space = ls;
  int h = lv_font_get_line_height(f);
  lv_area_t a = {cx - 80, cy - h / 2, cx + 80, cy - h / 2 + h - 1};
  lv_draw_label(L, &d, &a);
}

static float reg_angle(const SensorScale *s, float v) {
  float f = (v - s->min) / (float)(s->max - s->min);
  f = constrain(f, 0.0f, 1.0f);
  return 225.0f + f * 270.0f;  // 270 degree sweep, gap at the bottom
}

static lv_color_t temp_color(const SensorScale *s, float v) {
  if (v >= s->hot) return C_SIGNALROT;
  if (v >= s->warn) return C_BERNSTEIN;
  return C_LEUCHT;
}

// ---- static dial face (drawn once, kept by backdrop.cpp) ------------------------
// Drawn twice: in color, and as a mask of its surfaces (white) against its markings (black), with
// which backdrop.cpp lets some of the background show through the surfaces.
static bool maskPass = false;
static lv_color_t surface(lv_color_t c) { return maskPass ? lv_color_white() : c; }
static lv_color_t marking(lv_color_t c) { return maskPass ? lv_color_black() : c; }

static void draw_register_face(lv_layer_t *L, const Register &r) {
  const SensorScale *s = r.scale;
  lv_color_t guilloche = surface(lv_color_mix(C_ZIFFERBLATT, C_REGISTER, 110));
  draw_disc(L, r.cx, r.cy, REG_R, surface(C_REGISTER));
  for (int rr = 10; rr <= REG_R - 8; rr += 4) draw_ring(L, r.cx, r.cy, rr, 1, guilloche);
  draw_ring(L, r.cx, r.cy, REG_R - 1, 3, marking(C_BERNSTEIN), reg_angle(s, s->warn), reg_angle(s, s->hot));
  draw_ring(L, r.cx, r.cy, REG_R - 1, 3, marking(C_SIGNALROT), reg_angle(s, s->hot), reg_angle(s, s->max));
  int mid = (s->min + s->max) / 2;
  for (int v = s->min; v <= s->max; v += 10) {
    bool major = (v == s->min || v == s->max || v == mid);
    float a = reg_angle(s, v);
    draw_line(L, polar(r.cx, r.cy, a, major ? 45 : 50), polar(r.cx, r.cy, a, 57), major ? 3 : 2,
              marking(major ? C_LEUCHT : C_SCHIEFER));
  }
  draw_text(L, r.name, F_LABEL, marking(C_SCHIEFER), r.cx + 1, r.cy + 38, 2);
}

static void draw_face(lv_obj_t *canvas) {
  lv_color_t minor = marking(lv_color_mix(C_SCHIEFER, C_TEILUNG, 90));
  lv_canvas_fill_bg(canvas, C_SCHWARZ, LV_OPA_COVER);
  lv_layer_t L;
  lv_canvas_init_layer(canvas, &L);
  draw_disc(&L, C, C, R_FACE, surface(C_ZIFFERBLATT));
  draw_ring(&L, C, C, R_FACE, 2, marking(C_TEILUNG));
  for (int i = 0; i < 60; i++) {  // minute track
    float a = i * 6.0f;
    if (i % 5 == 0) draw_line(&L, polar(C, C, a, 200), polar(C, C, a, 212), 3, marking(C_LEUCHT));
    else draw_line(&L, polar(C, C, a, 204), polar(C, C, a, 212), 2, minor);
  }
  for (int h = 1; h < 12; h++) {  // hour batons; short where a register sits
    bool reg = (h == 3 || h == 6 || h == 9);
    draw_line(&L, polar(C, C, h * 30.0f, reg ? 176 : 160), polar(C, C, h * 30.0f, 194), 7, marking(C_LEUCHT));
  }
  draw_text(&L, "12", F_NUM48, marking(C_LEUCHT), C, C - 174);
  for (const Register &r : regs) draw_register_face(&L, r);
  lv_canvas_finish_layer(canvas, &L);
}

// ---- register pointers ---------------------------------------------------------
static void invalidate_register(const Register &r) {
  lv_area_t a;
  lv_obj_get_coords(regLayer, &a);
  lv_area_t box = {a.x1 + r.cx - REG_R, a.y1 + r.cy - REG_R, a.x1 + r.cx + REG_R, a.y1 + r.cy + REG_R};
  lv_obj_invalidate_area(regLayer, &box);
}

static void reg_draw_cb(lv_event_t *e) {
  lv_layer_t *L = lv_event_get_layer(e);
  lv_area_t a;
  lv_obj_get_coords(regLayer, &a);
  for (const Register &r : regs) {
    if (isnan(r.shown)) continue;
    float ang = reg_angle(r.scale, r.shown);
    int x = a.x1 + r.cx, y = a.y1 + r.cy;
    draw_line(L, polar(x, y, ang, 30), polar(x, y, ang, 57), 4, temp_color(r.scale, r.target), true);
  }
}

static void reg_anim_cb(void *var, int32_t v) {
  Register *r = (Register *)var;
  r->shown = v / 10.0f;
  invalidate_register(*r);
}

void clock_on_sensors() {
  for (int i = 0; i < 3; i++) {
    Register &r = regs[i];
    float t = M.temp[i];
    if (isnan(t)) {
      if (!isnan(r.target)) {
        lv_anim_delete(&r, reg_anim_cb);
        r.shown = r.target = NAN;
        invalidate_register(r);
        lv_label_set_text(r.value, "–");
        lv_obj_set_style_text_color(r.value, C_SCHIEFER, 0);
      }
      continue;
    }
    bool hadValue = !isnan(r.target);
    int before = hadValue ? (int)lroundf(r.target) : -999;
    r.target = t;
    if (!hadValue) {
      r.shown = t;
      invalidate_register(r);
    } else if (fabsf(t - r.shown) >= 0.2f) {  // glide like a needle
      lv_anim_t a;
      lv_anim_init(&a);
      lv_anim_set_var(&a, &r);
      lv_anim_set_exec_cb(&a, reg_anim_cb);
      lv_anim_set_values(&a, lroundf(r.shown * 10), lroundf(t * 10));
      lv_anim_set_duration(&a, 600);
      lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
      lv_anim_start(&a);
    }
    if ((int)lroundf(t) != before || !hadValue) {
      lv_label_set_text_fmt(r.value, "%d°", (int)lroundf(t));
      lv_obj_set_style_text_color(r.value, temp_color(r.scale, t), 0);
    }
  }
}

// ---- hands ---------------------------------------------------------------------------
static lv_area_t hand_area(float ang, const HandSpec &h, int pad) {
  lv_area_t o;
  lv_obj_get_coords(handLayer, &o);
  lv_point_t p1 = polar(o.x1 + C, o.y1 + C, ang + 180.0f, h.back);
  lv_point_t p2 = polar(o.x1 + C, o.y1 + C, ang, h.len);
  lv_area_t a = {min(p1.x, p2.x) - pad, min(p1.y, p2.y) - pad, max(p1.x, p2.x) + pad, max(p1.y, p2.y) + pad};
  return a;
}

static void move_hand(float &ang, float to, const HandSpec &h, int pad) {
  if (ang == to) return;
  lv_area_t before = hand_area(ang, h, pad);
  lv_obj_invalidate_area(handLayer, &before);
  ang = to;
  lv_area_t after = hand_area(ang, h, pad);
  lv_obj_invalidate_area(handLayer, &after);
}

static void draw_hand(lv_layer_t *L, int ox, int oy, float ang, const HandSpec &h, lv_color_t c, bool outline) {
  lv_point_t a = polar(ox, oy, ang + 180.0f, h.back), b = polar(ox, oy, ang, h.len);
  if (outline) draw_line(L, a, b, h.width + 4, C_LACK);
  draw_line(L, a, b, h.width, c);
}

static void hands_draw_cb(lv_event_t *e) {
  lv_layer_t *L = lv_event_get_layer(e);
  lv_area_t o;
  lv_obj_get_coords(handLayer, &o);
  int ox = o.x1 + C, oy = o.y1 + C;
  lv_color_t acc = C_AKZENT;
  draw_hand(L, ox, oy, angH, H_HOUR, C_LEUCHT, true);
  draw_hand(L, ox, oy, angM, H_MIN, C_LEUCHT, true);
  draw_hand(L, ox, oy, angS, H_SEC, acc, false);
  lv_point_t cw = polar(ox, oy, angS + 180.0f, 30);
  draw_disc(L, cw.x, cw.y, 6, acc);
  draw_disc(L, ox, oy, 9, acc);
  draw_disc(L, ox, oy, 3, C_LACK);
}

static void sec_anim_cb(void *var, int32_t v) {
  move_hand(angS, fmodf(v / 10.0f, 360.0f), H_SEC, 12);
}

void clock_tick() {
  if (!M.timeValid) return;
  time_t now = time(nullptr);
  struct tm tm;
  gmtime_r(&now, &tm);  // the bridge sends local time
  if (tm.tm_sec == lastSec) return;

  float s = tm.tm_sec * 6.0f;
  float m = tm.tm_min * 6.0f + tm.tm_sec * 0.1f;
  float h = (tm.tm_hour % 12) * 30.0f + tm.tm_min * 0.5f + tm.tm_sec / 120.0f;
  bool step = lastSec >= 0 && ((tm.tm_sec - lastSec + 60) % 60) == 1;
  lastSec = tm.tm_sec;

  move_hand(angH, h, H_HOUR, 12);
  move_hand(angM, m, H_MIN, 12);
  if (step) {  // quartz step: a short move with a slight overshoot
    int32_t from = lroundf(angS * 10), to = lroundf(s * 10);
    if (to < from) to += 3600;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &angS);
    lv_anim_set_exec_cb(&a, sec_anim_cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, 160);
    lv_anim_set_path_cb(&a, lv_anim_path_overshoot);
    lv_anim_start(&a);
  } else {
    lv_anim_delete(&angS, sec_anim_cb);
    move_hand(angS, s, H_SEC, 12);
  }
}

// ---- build ---------------------------------------------------------------------------
static lv_obj_t *overlay_layer(lv_obj_t *tile, lv_event_cb_t draw_cb) {
  lv_obj_t *o = lv_obj_create(tile);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, DX, DY);
  lv_obj_set_size(o, DIAL, DIAL);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_event_cb(o, draw_cb, LV_EVENT_DRAW_MAIN, nullptr);
  return o;
}

void clock_create(lv_obj_t *tile) {
  // Face and surface mask are drawn once; the backdrop keeps them and composes them onto the
  // background, together with the face's round edge.
  size_t size = LV_CANVAS_BUF_SIZE(DIAL, DIAL, 16, LV_DRAW_BUF_STRIDE_ALIGN);
  uint8_t *buf[2];
  lv_obj_t *canvas[2];
  for (int i = 0; i < 2; i++) {
    buf[i] = (uint8_t *)SDRAM.malloc(size);
    canvas[i] = lv_canvas_create(tile);
    lv_canvas_set_buffer(canvas[i], buf[i], DIAL, DIAL, LV_COLOR_FORMAT_RGB565);
    maskPass = i == 1;
    draw_face(canvas[i]);
  }
  maskPass = false;
  backdrop_set_dial(lv_canvas_get_draw_buf(canvas[0]), lv_canvas_get_draw_buf(canvas[1]), DX, DY, R_FACE, C_TEILUNG);
  for (int i = 0; i < 2; i++) {
    lv_obj_delete(canvas[i]);
    SDRAM.free(buf[i]);
  }

  regLayer = overlay_layer(tile, reg_draw_cb);
  for (Register &r : regs) {
    r.value = mk_label(tile, F_NUM40, C_SCHIEFER, "–");
    lv_obj_set_width(r.value, 120);
    lv_obj_set_style_text_align(r.value, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(r.value, DX + r.cx - 60, DY + r.cy - 19);
  }
  handLayer = overlay_layer(tile, hands_draw_cb);
}
