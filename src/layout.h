#pragma once

// Runtime tile layout, fetched from Home Assistant (/local/tessera/layout.json)
// so tiles can be changed in HA without a firmware rebuild. See layout.cpp.

#define LAYOUT_MAX_TILES 27   // 3 pages x 9 cells

// Fetch and parse the layout file from HA. Returns true when a usable layout was
// parsed. On failure the previous good layout (or nothing, at first boot) is kept.
bool layout_fetch();

// Number of tiles in the last good layout (0 if none has loaded yet).
int layout_tile_count();
