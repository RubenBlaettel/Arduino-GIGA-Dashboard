// Right column: now playing (only while Spotify plays), weekday, date, digital time and the PC's
// CPU/GPU/RAM load. The column is centred on the dial; when the song card appears, the date/time
// block glides down to make room for it.
#include <Arduino.h>
#include <math.h>
#include <string.h>
#include <time.h>
#include "SDRAM.h"
#include "ui.h"
#include "theme.h"
#include "config.h"
#include "model.h"
#include "cover.h"

// ---- geometry ----------------------------------------------------------------------
static const int X = 500, W = 276;  // column
static const int MID_Y = 240;       // dial centre
static const int GAP = 28;          // between song card, date/time and loads
// date/time block, relative to its top: weekday, date, time, then the load row
static const int Y_DATE = 44, Y_TIME = 88, TIME_H = 89;
static const int Y_LOAD = Y_TIME + TIME_H + GAP;
static const int LOAD_W = 80, LOAD_STEP = 98, Y_LOAD_VAL = 22, Y_LOAD_BAR = 61, BAR_H = 6;
static const int BLOCK_H = Y_LOAD + Y_LOAD_BAR + BAR_H;
static const int BLOCK_PAD = 8;     // the time label starts 4 px left of the column
static const int BLOCK_SPARE = 12;  // below the loads: room for a three-line hint
// song card: cover with a hairline frame, title and artist beside it
static const int CARD_H = COVER_PX + 2;
static const int TEXT_X = CARD_H + 14, TEXT_W = W - TEXT_X;
static const int TEXT_LINES = 3;     // title (up to 2) + artist
static const int TEXT_LEADING = -3;  // the font's line height leaves room for accents on capitals
static const int TEXT_GAP = 4;       // between title and artist
static const int BLOCK_Y_IDLE = MID_Y - BLOCK_H / 2;
static const int BLOCK_Y_PLAYING = MID_Y - (CARD_H + GAP + BLOCK_H) / 2 + CARD_H + GAP;
static const int GLIDE_MS = 400;

static const char *WEEKDAYS[] = {"Sonntag", "Montag", "Dienstag", "Mittwoch", "Donnerstag", "Freitag", "Samstag"};
static const char *MONTHS[] = {"Januar", "Februar", "März", "April", "Mai", "Juni", "Juli",
                               "August", "September", "Oktober", "November", "Dezember"};
static const char *LOAD_NAMES[] = {"CPU", "GPU", "RAM"};

static lv_obj_t *block, *lblWeekday, *lblDate, *lblTime, *lblSec, *lblHint;
static lv_obj_t *loadRow, *loadVal[3], *loadBar[3];
static lv_obj_t *card, *coverImg, *lblTitle, *lblArtist;
static int blockY = BLOCK_Y_IDLE;
static bool cardOn = false;
static uint32_t coverShown = 0;  // id of the cover in coverImg
static int lastSec = -1, lastMin = -1, lastDay = -1;
static int shownLoad[3] = {-2, -2, -2};
static const char *shownHint = "";

static void show(lv_obj_t *o, bool on) {
  if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
  else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

// ---- song card ---------------------------------------------------------------------
static int text_lines(const char *t) {
  lv_point_t sz;
  lv_text_get_size(&sz, t, F_UI, 0, 0, TEXT_W, LV_TEXT_FLAG_NONE);
  int lh = lv_font_get_line_height(F_UI);
  return (sz.y + lh / 2) / lh;
}

static int lines_height(int n) { return n * lv_font_get_line_height(F_UI) + (n - 1) * TEXT_LEADING; }

// Title gets up to two lines, the artist the rest; whatever does not fit ends in "...".
static void set_song(const char *title, const char *artist) {
  bool hasArtist = artist[0] != '\0';
  int tl = constrain(text_lines(title), 1, hasArtist ? TEXT_LINES - 1 : TEXT_LINES);
  lv_obj_set_height(lblTitle, lines_height(tl));
  lv_label_set_text(lblTitle, title);
  if (hasArtist) {
    lv_obj_set_height(lblArtist, lines_height(constrain(text_lines(artist), 1, TEXT_LINES - tl)));
    lv_label_set_text(lblArtist, artist);
  }
  show(lblArtist, hasArtist);
}

static void glide_cb(void *var, int32_t y) {
  blockY = y;
  lv_obj_set_y(block, y);
}

static void glide_done(lv_anim_t *a) { show(card, cardOn); }

// The card appears once the block has made room, and goes before the block moves back up.
static void show_card(bool on) {
  if (on == cardOn) return;
  cardOn = on;
  if (!on) show(card, false);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, block);
  lv_anim_set_exec_cb(&a, glide_cb);
  lv_anim_set_values(&a, blockY, on ? BLOCK_Y_PLAYING : BLOCK_Y_IDLE);
  lv_anim_set_duration(&a, GLIDE_MS);
  lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
  lv_anim_set_completed_cb(&a, glide_done);
  lv_anim_start(&a);
}

void panel_on_media() {
  if (M.playing) {
    set_song(M.title, M.artist);
    show(coverImg, M.cover != 0 && M.cover == coverShown);
  }
  show_card(M.playing && pcConnected());
}

void panel_on_cover(uint32_t id) {
  lv_draw_buf_t *db = lv_canvas_get_draw_buf(coverImg);
  const uint8_t *px = cover_pixels();
  for (int y = 0; y < COVER_PX; y++) memcpy(db->data + y * db->header.stride, px + y * COVER_PX * 2, COVER_PX * 2);
  lv_image_cache_drop(db);
  lv_obj_invalidate(coverImg);
  coverShown = id;
  show(coverImg, M.cover == id);
}

// ---- loads -------------------------------------------------------------------------
void panel_on_loads() {
  for (int i = 0; i < 3; i++) {
    int v = M.load[i];
    if (v == shownLoad[i]) continue;
    shownLoad[i] = v;
    if (v < 0) {
      lv_label_set_text(loadVal[i], "–");
      lv_obj_set_style_text_color(loadVal[i], C_SCHIEFER, 0);
    } else {
      lv_label_set_text_fmt(loadVal[i], "%d%%", v);
      lv_obj_set_style_text_color(loadVal[i], C_LEUCHT, 0);
    }
    lv_bar_set_value(loadBar[i], max(v, 0), LV_ANIM_ON);
  }
}

// ---- hint, date and time -----------------------------------------------------------
// One hint line, shown only when something needs attention. It takes the place of the loads,
// which are unknown in all of these cases.
static void set_hint(const char *msg, lv_color_t color) {
  if (msg == shownHint) return;
  shownHint = msg;
  show(loadRow, msg == nullptr);
  show(lblHint, msg != nullptr);
  if (!msg) return;
  lv_label_set_text(lblHint, msg);
  lv_obj_set_style_text_color(lblHint, color, 0);
}

static void update_status() {
  bool noSensors = isnan(M.temp[0]) && isnan(M.temp[1]) && isnan(M.temp[2]);
  if (!M.everConnected) set_hint("Warte auf die PC-Bridge …", C_SCHIEFER);
  else if (!pcConnected()) set_hint("Keine Verbindung zum PC. Läuft die Bridge?", C_BERNSTEIN);
  else if (noSensors) set_hint("Keine Sensordaten. In HWiNFO „Shared Memory Support“ einschalten.", C_BERNSTEIN);
  else set_hint(nullptr, C_SCHIEFER);
  show_card(M.playing && pcConnected());  // a song stays only as long as the PC reports it
}

static void update_digital(const struct tm &tm) {
  if (tm.tm_min != lastMin) {
    lastMin = tm.tm_min;
    lv_label_set_text_fmt(lblTime, "%02d:%02d", tm.tm_hour, tm.tm_min);
    lv_obj_align_to(lblSec, lblTime, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -2);
    lv_obj_remove_flag(lblSec, LV_OBJ_FLAG_HIDDEN);
  }
  lv_label_set_text_fmt(lblSec, "%02d", tm.tm_sec);
  if (tm.tm_yday != lastDay) {
    lastDay = tm.tm_yday;
    lv_label_set_text(lblWeekday, WEEKDAYS[tm.tm_wday]);
    lv_label_set_text_fmt(lblDate, "%d. %s %d", tm.tm_mday, MONTHS[tm.tm_mon], tm.tm_year + 1900);
  }
}

void panel_tick() {
  static uint32_t lastStatusMs = 0;
  if (millis() - lastStatusMs > 1000) {  // catches connection timeouts
    lastStatusMs = millis();
    update_status();
  }
  if (!M.timeValid) return;
  time_t now = time(nullptr);
  struct tm tm;
  gmtime_r(&now, &tm);  // the bridge sends local time
  if (tm.tm_sec == lastSec) return;
  lastSec = tm.tm_sec;
  update_digital(tm);
}

// ---- build -------------------------------------------------------------------------
static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h) {
  lv_obj_t *o = lv_obj_create(parent);
  lv_obj_remove_style_all(o);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_size(o, w, h);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

static void create_card(lv_obj_t *parent) {
  card = box(parent, X, BLOCK_Y_PLAYING - GAP - CARD_H, W, CARD_H);
  lv_obj_add_flag(card, LV_OBJ_FLAG_HIDDEN);

  // Recessed square with a hairline; it stays empty until the cover has arrived.
  lv_obj_t *frame = box(card, 0, 0, CARD_H, CARD_H);
  lv_obj_set_style_bg_color(frame, C_REGISTER, 0);
  lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
  lv_obj_set_style_border_color(frame, C_TEILUNG, 0);
  lv_obj_set_style_border_width(frame, 1, 0);
  uint8_t *buf = (uint8_t *)SDRAM.malloc(LV_CANVAS_BUF_SIZE(COVER_PX, COVER_PX, 16, LV_DRAW_BUF_STRIDE_ALIGN));
  coverImg = lv_canvas_create(frame);
  lv_canvas_set_buffer(coverImg, buf, COVER_PX, COVER_PX, LV_COLOR_FORMAT_RGB565);
  lv_obj_set_pos(coverImg, 0, 0);  // inside the border
  lv_obj_add_flag(coverImg, LV_OBJ_FLAG_HIDDEN);

  lv_obj_t *text = box(card, TEXT_X, 0, TEXT_W, CARD_H);
  lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_flex_align(text, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
  lv_obj_set_style_pad_row(text, TEXT_GAP, 0);
  lblTitle = mk_label(text, F_UI, C_LEUCHT, "");
  lblArtist = mk_label(text, F_UI, C_SCHIEFER, "");
  lv_obj_t *labels[] = {lblTitle, lblArtist};
  for (lv_obj_t *l : labels) {
    lv_obj_set_width(l, TEXT_W);
    lv_obj_set_style_text_line_space(l, TEXT_LEADING, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
  }
}

static void create_loads(lv_obj_t *parent) {
  loadRow = box(parent, 0, Y_LOAD, W, Y_LOAD_BAR + BAR_H);
  for (int i = 0; i < 3; i++) {
    int x = i * LOAD_STEP;
    lv_obj_t *name = mk_label(loadRow, F_LABEL, C_SCHIEFER, LOAD_NAMES[i]);
    lv_obj_set_style_text_letter_space(name, 2, 0);
    lv_obj_set_pos(name, x, 0);
    loadVal[i] = mk_label(loadRow, F_NUM40, C_SCHIEFER, "–");
    lv_obj_set_pos(loadVal[i], x, Y_LOAD_VAL);
    lv_obj_t *bar = lv_bar_create(loadRow);
    lv_obj_remove_style_all(bar);
    lv_obj_set_pos(bar, x, Y_LOAD_BAR);
    lv_obj_set_size(bar, LOAD_W, BAR_H);
    lv_bar_set_range(bar, 0, 100);
    lv_obj_set_style_bg_color(bar, C_TEILUNG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, C_LEUCHT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_anim_duration(bar, 600, LV_PART_MAIN);  // glides like the register needles
    loadBar[i] = bar;
  }
}

void panel_create(lv_obj_t *parent) {
  create_card(parent);

  block = box(parent, X - BLOCK_PAD, BLOCK_Y_IDLE, W + 2 * BLOCK_PAD, BLOCK_H + BLOCK_SPARE);
  lv_obj_set_style_pad_hor(block, BLOCK_PAD, 0);
  lblWeekday = mk_label(block, F_TITLE, C_LEUCHT, "");
  lblDate = mk_label(block, F_BODY, C_SCHIEFER, "");
  lv_obj_set_pos(lblDate, 0, Y_DATE);
  lblTime = mk_label(block, F_TIME, C_LEUCHT, "--:--");
  lv_obj_set_pos(lblTime, -4, Y_TIME);
  lblSec = mk_label(block, F_NUM48, C_AKZENT, "00");
  lv_obj_add_flag(lblSec, LV_OBJ_FLAG_HIDDEN);
  lv_obj_align_to(lblSec, lblTime, LV_ALIGN_OUT_RIGHT_BOTTOM, 10, -2);

  create_loads(block);
  lblHint = mk_label(block, F_UI, C_SCHIEFER, "");
  lv_obj_set_pos(lblHint, 0, Y_LOAD);
  lv_obj_set_width(lblHint, W);
  lv_obj_set_style_text_line_space(lblHint, -4, 0);  // the extended font's line height has room for accents
  lv_label_set_long_mode(lblHint, LV_LABEL_LONG_WRAP);
  lv_obj_add_flag(lblHint, LV_OBJ_FLAG_HIDDEN);

  update_status();
}
