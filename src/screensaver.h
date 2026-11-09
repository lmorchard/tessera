#pragma once

#include <lvgl.h>

// Idle screensaver: a light LCARS-style overlay (slow blinking blocks and a
// small typed-data readout) shown after the screen has dimmed. Off unless
// SCREENSAVER_ENABLED is 1 in config.h. See screensaver.cpp.

// Create the overlay, hidden. Call once from ui_init(), after the grid exists.
// `grid` is the tile view, which is hidden while the screensaver runs.
void screensaver_init(lv_obj_t* parent, lv_obj_t* grid);

// Show the overlay and start its timers. Idempotent.
void screensaver_start();

// Hide the overlay and pause its timers. Idempotent.
void screensaver_stop();

bool screensaver_active();
