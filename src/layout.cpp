// layout.cpp — fetches the tile layout from Home Assistant at runtime.
//
// The file is served by HA from /config/www/tessera/layout.json as
// /local/tessera/layout.json. It's read over plain HTTP (no token), so it must
// not contain secrets. A failed fetch or parse keeps the previous good layout.

#include <Arduino.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "layout.h"
#include "secrets.h"   // HA_HOST/PORT

static int tile_count = 0;

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

  // Parse with a filter so only the fields we use are kept. The filter document
  // must hold every key listed (see the undersized-filter note in ha_client.cpp).
  StaticJsonDocument<768> filter;
  filter["tiles"][0]["label"] = true;
  filter["tiles"][0]["entity"] = true;
  filter["tiles"][0]["page"] = true;
  filter["tiles"][0]["icon"] = true;
  filter["tiles"][0]["kelvin"] = true;
  filter["tiles"][0]["pct"] = true;
  filter["tiles"][0]["type"] = true;

  DynamicJsonDocument doc(6144);
  String body = http.getString();
  http.end();
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("layout: parse failed (%s), keeping previous layout\n", err.c_str());
    return false;
  }

  JsonArrayConst tiles = doc["tiles"];
  if (tiles.isNull()) {
    Serial.println("layout: no 'tiles' array, keeping previous layout");
    return false;
  }

  int n = 0;
  for (JsonObjectConst t : tiles) {
    if (n >= LAYOUT_MAX_TILES) {
      Serial.println("layout: more than LAYOUT_MAX_TILES tiles, extras ignored");
      break;
    }
    n++;
  }
  tile_count = n;
  Serial.printf("layout: loaded %d tiles\n", tile_count);
  return true;
}

int layout_tile_count() {
  return tile_count;
}
