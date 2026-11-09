#pragma once

// Runtime tile layout, fetched from Home Assistant (/local/tessera/layout.json).
// Tiles are changed in HA, not in firmware.

#include <stdint.h>

#define LAYOUT_MAX_TILES  27   // 3 pages x 9 cells
#define LAYOUT_MAX_PAGES  8
#define LAYOUT_PER_PAGE   9    // 3x3 grid
#define LAYOUT_LABEL_LEN  24
#define LAYOUT_ENTITY_LEN 64

struct LayoutTile {
  char label[LAYOUT_LABEL_LEN];
  char entity[LAYOUT_ENTITY_LEN];
  uint8_t page;
  float on_pct;
  uint16_t on_kelvin;
  uint8_t label_size;   // tile name font size in px: 14, 16, 24 or 32
  const char* icon;   // glyph, from the ICON_* names in config.h
};

// Font sizes a tile label can use. These must be enabled in lv_conf.h.
#define LAYOUT_LABEL_SIZE_DEFAULT 14

// Clear the layout state. Call once at boot, before WiFi starts.
void layout_init();

// Fetch the layout from HA into a staging copy. Returns true when the fetched
// file is usable. On failure the staged copy is left unchanged.
bool layout_fetch();

// True when the staged layout differs from the one in use.
bool layout_changed();

// Make the staged layout the one in use.
void layout_commit();

// How often to check HA for a changed layout, in seconds. Read from the file's
// "check_seconds" field, or LAYOUT_DEFAULT_CHECK_SECONDS when absent.
#define LAYOUT_DEFAULT_CHECK_SECONDS 60
uint32_t layout_check_seconds();

// Idle seconds before the screensaver starts, from the file's "screensaver_seconds"
// field, or SCREENSAVER_IDLE_MS (config.h) when absent. Never less than the dim time.
uint32_t layout_screensaver_seconds();

int layout_count();

// Returns nullptr if idx is out of range.
const LayoutTile* layout_get(int idx);
