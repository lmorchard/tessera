// screensaver.cpp — the idle LCARS overlay.
//
// Built once and hidden. screensaver_start() hides the tile grid, shows the
// overlay, and starts three timers: a random blink on the spine and data lights,
// a scanner that steps along the bottom bar, and a typed-data readout. Most of
// the overlay is static shapes drawn once; the animations change state in
// discrete steps (never per frame), which keeps the draw cost low on this panel.
// screensaver_stop() pauses the timers and restores the grid.

#include <Arduino.h>
#include <lvgl.h>
#include "screensaver.h"
#include "lcars.h"
#include "config.h"

#define SPINE_COUNT   8
#define CLUSTER_COUNT 6
#define BLINK_COUNT   (2 * SPINE_COUNT + CLUSTER_COUNT)
#define SCAN_COUNT    12
#define TYPE_LINES    7
#define TYPE_COLS     30
#define TYPE_MS       180    // one character every 180 ms
#define SCAN_MS       260    // scanner steps every 260 ms
#define BLINK_MIN_MS  900
#define BLINK_MAX_MS  3000

static lv_obj_t* overlay = nullptr;
static lv_obj_t* grid_view = nullptr;   // the tile view, hidden while active
static lv_obj_t* blinkers[BLINK_COUNT]; // lights that blink at random
static lv_obj_t* scan[SCAN_COUNT];
static lv_obj_t* readout = nullptr;
static lv_timer_t* blink_timer = nullptr;
static lv_timer_t* type_timer = nullptr;
static lv_timer_t* scan_timer = nullptr;
static bool active = false;

static int scan_pos = 0;
static int scan_dir = 1;

// The readout text, typed one character at a time. Lines are short, to suit a
// 480 px wide screen at the smallest font.
static const char* LINES[] = {
  "STARDATE 4471.2",
  "SENSOR SWEEP ... NOMINAL",
  "ENV CTRL LOOP ... OK",
  "LIGHTING GRID ... ONLINE",
  "HAIL: NO CONTACT",
  "SHIP SYSTEMS ... GREEN",
  "AWAITING INPUT_",
};
static const int LINE_COUNT = sizeof(LINES) / sizeof(LINES[0]);

static const uint32_t SPINE_COLORS[] = { LCARS_ORANGE, LCARS_LAVENDER, LCARS_BLUE, LCARS_TAN };
static const int SPINE_COLOR_COUNT = sizeof(SPINE_COLORS) / sizeof(SPINE_COLORS[0]);

static int type_line = 0;   // which line is being typed
static int type_col = 0;    // characters of that line shown so far
static char typed[TYPE_LINES * (TYPE_COLS + 2) + 1];

// Repaint the typed buffer: the last TYPE_LINES lines, with a cursor on the last.
static void render_readout() {
  size_t used = 0;
  int first = type_line - (TYPE_LINES - 1);
  if (first < 0) first = 0;
  for (int l = first; l <= type_line && l < LINE_COUNT; l++) {
    int n = (l == type_line) ? type_col : (int)strlen(LINES[l]);
    bool cursor = (l == type_line);
    int w = snprintf(typed + used, sizeof(typed) - used, "%.*s%s\n", n, LINES[l], cursor ? "_" : "");
    if (w < 0 || (size_t)w >= sizeof(typed) - used) break;   // would overflow: stop here
    used += w;
  }
  lv_label_set_text(readout, typed);
}

static void type_cb(lv_timer_t*) {
  const char* line = LINES[type_line];
  if (type_col < (int)strlen(line)) {
    type_col++;
  } else {
    // Line finished: move on, and start over after the last one.
    type_line = (type_line + 1) % LINE_COUNT;
    type_col = 0;
  }
  render_readout();
}

// Move the scanner one segment, bouncing between the ends of the bar.
static void scan_cb(lv_timer_t*) {
  lv_obj_set_style_bg_color(scan[scan_pos], lv_color_hex(LCARS_OFF), LV_PART_MAIN);
  scan_pos += scan_dir;
  if (scan_pos <= 0 || scan_pos >= SCAN_COUNT - 1) scan_dir = -scan_dir;
  lv_obj_set_style_bg_color(scan[scan_pos], lv_color_hex(LCARS_ORANGE), LV_PART_MAIN);
}

// Toggle one light at a random interval, so they don't blink in step.
static void blink_cb(lv_timer_t* t) {
  lv_obj_t* b = blinkers[random(BLINK_COUNT)];
  if (lv_obj_has_flag(b, LV_OBJ_FLAG_HIDDEN)) lv_obj_clear_flag(b, LV_OBJ_FLAG_HIDDEN);
  else                                        lv_obj_add_flag(b, LV_OBJ_FLAG_HIDDEN);
  lv_timer_set_period(t, BLINK_MIN_MS + random(BLINK_MAX_MS - BLINK_MIN_MS));
}

static lv_obj_t* make_bar(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, int radius) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_set_size(o, w, h);
  lv_obj_set_pos(o, x, y);
  lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
  lv_obj_set_style_bg_color(o, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_clear_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  return o;
}

// A rounded LCARS button: a pill-shaped bar with a short label centred on it.
static void make_pill(lv_obj_t* parent, int x, int y, int w, int h, uint32_t color, const char* text) {
  lv_obj_t* p = make_bar(parent, x, y, w, h, color, h / 2);
  lv_obj_t* t = lv_label_create(p);
  lv_label_set_text(t, text);
  lv_obj_set_style_text_font(t, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(t, lv_color_hex(LCARS_INK), LV_PART_MAIN);
  lv_obj_clear_flag(t, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_center(t);
}

static lv_obj_t* make_text(lv_obj_t* parent, int x, int y, const char* text,
                           const lv_font_t* font, uint32_t color) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text);
  lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
  lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
  lv_obj_set_pos(l, x, y);
  lv_obj_clear_flag(l, LV_OBJ_FLAG_CLICKABLE);
  return l;
}

void screensaver_init(lv_obj_t* parent, lv_obj_t* grid) {
  grid_view = grid;
  overlay = lv_obj_create(parent);
  lv_obj_set_size(overlay, 480, 480);
  lv_obj_set_pos(overlay, 0, 0);
  lv_obj_set_style_bg_opa(overlay, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(overlay, 0, LV_PART_MAIN);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_CLICKABLE);   // touches fall through to the grid
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);

  // Spines down each side, mirrored. 8 segments of 36 px with 6 px gaps, ending
  // above the bottom rule (y=452).
  const int spine_top = 64, spine_h = 36, spine_gap = 6;
  for (int i = 0; i < SPINE_COUNT; i++) {
    int y = spine_top + i * (spine_h + spine_gap);
    lv_obj_t* left = make_bar(overlay, 0, y, 26, spine_h,
                              SPINE_COLORS[i % SPINE_COLOR_COUNT], 6);
    lv_obj_t* right = make_bar(overlay, 454, y, 26, spine_h,
                               SPINE_COLORS[(SPINE_COUNT - 1 - i) % SPINE_COLOR_COUNT], 6);
    blinkers[i] = left;
    blinkers[SPINE_COUNT + i] = right;
    lv_obj_add_flag(left, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(right, LV_OBJ_FLAG_HIDDEN);
  }

  // Elbow bar under the header, and a thin rule along the bottom.
  make_bar(overlay, 40, 62, 408, 4, LCARS_TAN, 2);
  make_bar(overlay, 40, 452, 432, 4, LCARS_LAVENDER, 2);

  // Top section: a label, a row of rounded buttons, and a row of tick marks.
  make_text(overlay, 40, 72, "SHIP SYSTEMS", &lv_font_montserrat_12, LCARS_LAVENDER);
  make_pill(overlay, 40, 92, 110, 26, LCARS_LAVENDER, "ALPHA");
  make_pill(overlay, 158, 92, 56, 26, LCARS_BLUE, "07");
  make_pill(overlay, 222, 92, 150, 26, LCARS_ORANGE, "SENSOR ARRAY");
  make_pill(overlay, 380, 92, 68, 26, LCARS_TAN, "4471");
  for (int i = 0; i < 16; i++) {
    make_bar(overlay, 40 + i * 27, 128, 2, 8, LCARS_TEXT_DIM, 0);
  }

  // Stardate block, with a data cluster on the right whose lights blink.
  make_text(overlay, 40, 146, "STARDATE", &lv_font_montserrat_12, LCARS_ORANGE);
  make_text(overlay, 40, 160, "4471.2", &lv_font_montserrat_32, LCARS_TAN);
  const uint32_t cluster_colors[] = { LCARS_ORANGE, LCARS_LAVENDER, LCARS_BLUE, LCARS_TAN };
  for (int i = 0; i < CLUSTER_COUNT; i++) {
    int x = 360 + (i % 3) * 24;
    int y = 150 + (i / 3) * 24;
    blinkers[2 * SPINE_COUNT + i] = make_bar(overlay, x, y, 18, 18,
                                             cluster_colors[i % 4], 4);
    lv_obj_add_flag(blinkers[2 * SPINE_COUNT + i], LV_OBJ_FLAG_HIDDEN);
  }

  // Lower end of the top section: a cap bar, a status label, and a thin rule.
  make_bar(overlay, 40, 216, 408, 2, LCARS_LAVENDER, 0);
  make_bar(overlay, 40, 224, 60, 10, LCARS_ORANGE, 5);
  make_text(overlay, 330, 222, "SYS 04 ONLINE", &lv_font_montserrat_12, LCARS_TAN);

  // Scanner: a row of small segments above the bottom rule, one lit at a time.
  const int scan_w = 30, scan_gap = 4, scan_x0 = 40 + (432 - (SCAN_COUNT * scan_w + (SCAN_COUNT - 1) * scan_gap)) / 2;
  for (int i = 0; i < SCAN_COUNT; i++) {
    scan[i] = make_bar(overlay, scan_x0 + i * (scan_w + scan_gap), 430, scan_w, 10, LCARS_OFF, 3);
  }

  // Typed readout, in the smallest font we compile in, above the scanner.
  readout = lv_label_create(overlay);
  lv_obj_set_style_text_font(readout, &lv_font_montserrat_12, LV_PART_MAIN);
  lv_obj_set_style_text_color(readout, lv_color_hex(LCARS_TAN), LV_PART_MAIN);
  lv_obj_set_style_text_opa(readout, 220, LV_PART_MAIN);
  lv_obj_set_width(readout, 420);
  lv_label_set_long_mode(readout, LV_LABEL_LONG_CLIP);
  lv_obj_set_pos(readout, 40, 300);
  lv_label_set_text(readout, "");

  // Timers are created paused and started only while the overlay is shown.
  blink_timer = lv_timer_create(blink_cb, BLINK_MIN_MS, NULL);
  type_timer  = lv_timer_create(type_cb, TYPE_MS, NULL);
  scan_timer  = lv_timer_create(scan_cb, SCAN_MS, NULL);
  lv_timer_pause(blink_timer);
  lv_timer_pause(type_timer);
  lv_timer_pause(scan_timer);
}

void screensaver_start() {
  if (active || !overlay) return;
  active = true;
  type_line = 0;
  type_col = 0;
  scan_pos = 0;
  scan_dir = 1;
  render_readout();
  // Hide the tiles so the animation has the screen to itself. Hidden objects also
  // don't take touches, so the wake touch can't tap a tile.
  if (grid_view) lv_obj_add_flag(grid_view, LV_OBJ_FLAG_HIDDEN);
  lv_obj_clear_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  lv_obj_move_foreground(overlay);
  lv_timer_resume(blink_timer);
  lv_timer_resume(type_timer);
  lv_timer_resume(scan_timer);
}

void screensaver_stop() {
  if (!active) return;
  active = false;
  lv_timer_pause(blink_timer);
  lv_timer_pause(type_timer);
  lv_timer_pause(scan_timer);
  for (int i = 0; i < BLINK_COUNT; i++) lv_obj_add_flag(blinkers[i], LV_OBJ_FLAG_HIDDEN);
  lv_obj_add_flag(overlay, LV_OBJ_FLAG_HIDDEN);
  if (grid_view) lv_obj_clear_flag(grid_view, LV_OBJ_FLAG_HIDDEN);
}

bool screensaver_active() {
  return active;
}
