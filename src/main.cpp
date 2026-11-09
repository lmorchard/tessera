// main.cpp — firmware entry point.
//
// setup() brings up LVGL, the ST7701S display, GT911 touch, then starts WiFi.
// Once WiFi is up it starts NTP and OTA, fetches the tile layout from Home
// Assistant, builds the UI from it, and starts the HA client. The layout is
// checked every 5 minutes, and a change restarts the panel. loop() pumps LVGL,
// ArduinoOTA, and the HA WebSocket, and polls the WiFi link state for the icon.
//
// Per-device secrets and tile layout live in include/config.h (gitignored; copy
// from config.h.example). See CLAUDE.md for the architecture overview.

#include <Arduino.h>
#include <WiFi.h>
#include <lvgl.h>
#include "display.h"
#include "touch.h"
#include "ui.h"
#include "ha_client.h"
#include "layout.h"
#include "ota.h"
#include "config.h"
#include "secrets.h"   // WiFi creds, TZ/NTP (gitignored; see setup wizard)

// Set by scripts/git_version.py at build time (the current git commit, plus
// "-dirty" if the working tree had uncommitted changes). Fallback here only
// matters if that script didn't run for some reason.
#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "unknown"
#endif

static lv_indev_drv_t indev_drv;

// The UI is built only after the HA layout has been fetched, so the tile grid
// always matches the layout in HA. Until then the panel shows a blank screen.
static bool ui_built = false;

// One-time bring-up of the services that need WiFi (NTP, OTA, HA). Deferred to
// the first successful association in loop() so a slow or missing AP at boot
// never blocks the UI — and so it self-heals once WiFi eventually connects.
static void start_services() {
  Serial.printf("WiFi connected: %s\n", WiFi.localIP().toString().c_str());
  configTzTime(TZ_INFO, NTP_SERVER);
  ota_init();
}

// Fetch the layout and, once it's loaded, build the UI and start the HA client.
// Retried from loop() until it succeeds. HA is the only source of the layout, so
// there's no fallback to a compiled layout here.
static void try_load_layout() {
  if (!layout_fetch()) return;
  layout_commit();
  ui_init();
  ha_init();
  ui_built = true;
}

// Check HA for a changed layout. Changes are applied by a restart, which rebuilds
// the UI from scratch. That's simpler and more reliable than rebuilding the grid
// in place, and it takes about 20 seconds.
static void check_layout_changed() {
  if (!layout_fetch()) return;
  if (layout_changed()) {
    Serial.println("layout: changed in HA, restarting to apply");
    delay(200);
    ESP.restart();
  }
  // Timing settings don't need a rebuild, so apply them now.
  layout_commit();
}

void setup() {
  Serial.begin(115200);
  Serial.printf("Tessera starting... (firmware %s)\n", FIRMWARE_VERSION);

  lv_init();
  display_init();
  touch_init();

  lv_indev_drv_init(&indev_drv);
  indev_drv.type    = LV_INDEV_TYPE_POINTER;
  indev_drv.read_cb = touch_read;
  lv_indev_drv_register(&indev_drv);

  layout_init();

  // Start WiFi non-blocking — don't stall setup() waiting to associate. The
  // connection state machine in loop() starts NTP/OTA/HA on the first connect
  // and re-kicks association if WiFi is slow or briefly unavailable.
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  Serial.printf("Connecting to %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

void loop() {
  static uint32_t last_tick = 0;
  uint32_t now = millis();
  lv_tick_inc(now - last_tick);
  last_tick = now;
  lv_timer_handler();

  // Connection state machine: bring up NTP/OTA/HA on the first WiFi connect, and
  // re-kick association if WiFi never comes up or drops for too long, so the
  // device self-heals without a power-cycle.
  static bool services_started = false;
  static uint32_t last_wifi_attempt = 0;
  static uint32_t last_layout_try = 0;
  static uint32_t last_layout_check = 0;
  if (WiFi.status() == WL_CONNECTED) {
    if (!services_started) { start_services(); services_started = true; }
    // Load the layout (and build the UI) as soon as WiFi is up, retrying every
    // 10 s until HA answers.
    if (!ui_built && now - last_layout_try > 10000) {
      last_layout_try = now;
      try_load_layout();
    }
    // Once running, check HA for a changed layout at the interval the layout
    // file sets (60 s by default).
    if (ui_built && now - last_layout_check > layout_check_seconds() * 1000UL) {
      last_layout_check = now;
      check_layout_changed();
    }
  } else if (now - last_wifi_attempt > 20000) {
    last_wifi_attempt = now;
    Serial.println("WiFi down — retrying association");
    WiFi.disconnect();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  if (ui_built) {   // only pump services once the UI and HA client are up
    ota_loop();
    ha_loop();
  }

  // Poll WiFi link state for the header WiFi icon (every 2s, update only on change).
  static uint32_t last_wifi_check = 0;
  static int last_wifi = -1;
  if (now - last_wifi_check > 2000) {
    last_wifi_check = now;
    int w = (WiFi.status() == WL_CONNECTED) ? 1 : 0;
    if (w != last_wifi) { last_wifi = w; ui_set_wifi_connected(w != 0); }
  }

  delay(5);
}
