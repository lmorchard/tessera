// layout.cpp — the tile layout used by the UI and the HA client.
//
// Starts empty. layout_fetch() loads
// /local/tessera/layout.json from Home Assistant, which HA serves from
// /config/www/tessera/layout.json. That file is read without a token, so it must
// not hold secrets. A failed fetch or an unusable file keeps the current layout.

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <string.h>
#include "layout.h"
#include "config.h"
#include "secrets.h"   // HA_HOST/PORT

static LayoutTile tiles[LAYOUT_MAX_TILES];     // layout in use on screen
static LayoutTile staging[LAYOUT_MAX_TILES];   // last fetched, not yet committed
static int tile_count = 0;
static int staged_count = 0;
static uint32_t check_seconds = LAYOUT_DEFAULT_CHECK_SECONDS;   // in use
static uint32_t staged_check_seconds = LAYOUT_DEFAULT_CHECK_SECONDS;
static uint32_t screensaver_secs = SCREENSAVER_IDLE_MS / 1000;
static uint32_t staged_screensaver_secs = SCREENSAVER_IDLE_MS / 1000;

// Icon names used in layout.json, mapped to the ICON_* glyphs in config.h.
static const struct { const char* name; const char* glyph; } ICON_NAMES[] = {
  { "lightbulb",     ICON_LIGHTBULB },
  { "fan",           ICON_FAN },
  { "bed",           ICON_BED },
  { "ceiling_light", ICON_CEILING_LIGHT },
  { "sofa",          ICON_SOFA },
  { "coach_lamp",    ICON_COACH_LAMP },
  { "fan_light",     ICON_FAN_LIGHT },
  { "home_temp",     ICON_HOME_TEMP },
  { "weather",       ICON_WEATHER },
  { "thermostat",    ICON_THERMOSTAT },
};

// A supported label font size (see LAYOUT_LABEL_SIZE_DEFAULT), or `fallback`.
static uint8_t label_size_or(int size, int fallback) {
  return (size == 14 || size == 16 || size == 24 || size == 32) ? size : fallback;
}

static const char* icon_glyph(const char* name) {
  if (name)
    for (const auto& i : ICON_NAMES)
      if (strcmp(i.name, name) == 0) return i.glyph;
  return ICON_LIGHTBULB;
}

void layout_init() {
  tile_count = 0;
  staged_count = 0;
}

bool layout_fetch() {
  String url = String("http://") + HA_HOST + ":" + HA_PORT + "/local/tessera/layout.json";
  HTTPClient http;
  http.begin(url);
  http.setTimeout(5000);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("layout: GET %s -> %d\n", url.c_str(), code);
    http.end();
    return false;
  }

  // The filter must list every key the loop below reads (see the undersized-filter
  // note in ha_client.cpp). Anything else in the file is dropped during parsing.
  StaticJsonDocument<1024> filter;
  filter["check_seconds"] = true;
  filter["screensaver_seconds"] = true;
  filter["label_size"] = true;
  filter["tiles"][0]["type"] = true;
  filter["tiles"][0]["label"] = true;
  filter["tiles"][0]["entity"] = true;
  filter["tiles"][0]["page"] = true;
  filter["tiles"][0]["icon"] = true;
  filter["tiles"][0]["kelvin"] = true;
  filter["tiles"][0]["pct"] = true;
  filter["tiles"][0]["label_size"] = true;

  DynamicJsonDocument doc(8192);
  String body = http.getString();
  http.end();
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("layout: parse failed (%s), keeping current layout\n", err.c_str());
    return false;
  }

  JsonArrayConst src = doc["tiles"];
  if (src.isNull()) {
    Serial.println("layout: no 'tiles' array, keeping current layout");
    return false;
  }

  // Top-level default label size, used by any tile that doesn't set its own.
  int default_size = label_size_or(doc["label_size"] | LAYOUT_LABEL_SIZE_DEFAULT, LAYOUT_LABEL_SIZE_DEFAULT);

  int n = 0;
  int per_page[LAYOUT_MAX_PAGES] = { 0 };
  for (JsonObjectConst item : src) {
    const char* type = item["type"] | "tile";
    if (strcmp(type, "tile") != 0) {
      Serial.printf("layout: skipping '%s' entry (not supported yet)\n", type);
      continue;
    }
    const char* label = item["label"];
    const char* entity = item["entity"];
    int page = item["page"] | 0;
    if (!label || !entity || strlen(entity) >= LAYOUT_ENTITY_LEN || page < 0 || page >= LAYOUT_MAX_PAGES) {
      Serial.println("layout: skipping malformed tile");
      continue;
    }
    if (n >= LAYOUT_MAX_TILES || per_page[page] >= LAYOUT_PER_PAGE) {
      Serial.printf("layout: no room for '%s' on page %d, skipped\n", label, page);
      continue;
    }
    LayoutTile& t = staging[n++];
    per_page[page]++;
    snprintf(t.label, sizeof(t.label), "%s", label);
    snprintf(t.entity, sizeof(t.entity), "%s", entity);
    t.page = page;
    t.on_pct = item["pct"] | 0.0f;
    t.on_kelvin = item["kelvin"] | 0;
    t.label_size = label_size_or(item["label_size"] | default_size, default_size);
    t.icon = icon_glyph(item["icon"].as<const char*>());
  }

  // An empty list would blank the panel, so treat it as unusable.
  if (n == 0) {
    Serial.println("layout: no usable tiles, keeping current layout");
    return false;
  }

  staged_count = n;
  // Clamp to 10 s minimum so a typo can't hammer HA, and 1 day maximum.
  uint32_t secs = doc["check_seconds"] | LAYOUT_DEFAULT_CHECK_SECONDS;
  staged_check_seconds = secs < 10 ? 10 : (secs > 86400 ? 86400 : secs);

  // Screensaver delay: never before the dim (IDLE_DIM_MS), and at most a day.
  uint32_t ss = doc["screensaver_seconds"] | (SCREENSAVER_IDLE_MS / 1000);
  uint32_t ss_min = IDLE_DIM_MS / 1000 + 1;
  staged_screensaver_secs = ss < ss_min ? ss_min : (ss > 86400 ? 86400 : ss);
  return true;
}

static bool same_tile(const LayoutTile& a, const LayoutTile& b) {
  return strcmp(a.label, b.label) == 0 && strcmp(a.entity, b.entity) == 0 &&
         a.page == b.page && a.on_pct == b.on_pct && a.on_kelvin == b.on_kelvin &&
         a.label_size == b.label_size && strcmp(a.icon, b.icon) == 0;
}

bool layout_changed() {
  if (staged_count != tile_count) return true;
  for (int i = 0; i < tile_count; i++)
    if (!same_tile(staging[i], tiles[i])) return true;
  return false;
}

void layout_commit() {
  bool first = (tile_count == 0);
  bool timing_changed = (check_seconds != staged_check_seconds) ||
                        (screensaver_secs != staged_screensaver_secs);
  memcpy(tiles, staging, sizeof(LayoutTile) * staged_count);
  tile_count = staged_count;
  check_seconds = staged_check_seconds;
  screensaver_secs = staged_screensaver_secs;
  if (first || timing_changed)
    Serial.printf("layout: %d tiles, checking every %us, screensaver after %us\n",
                  tile_count, (unsigned)check_seconds, (unsigned)screensaver_secs);
}

uint32_t layout_check_seconds() {
  return check_seconds;
}

uint32_t layout_screensaver_seconds() {
  return screensaver_secs;
}

int layout_count() {
  return tile_count;
}

const LayoutTile* layout_get(int idx) {
  if (idx < 0 || idx >= tile_count) return nullptr;
  return &tiles[idx];
}
