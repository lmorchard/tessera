# Tessera

A Home Assistant wall-panel controller for the **Guition ESP32-S3-4848S040** — a 4" 480×480 capacitive-touch display. Tessera shows a grid of device tiles that toggle Home Assistant entities over the WebSocket API, plus indoor/outdoor temperatures and a full thermostat control view.

> **Why "Tessera"?** A *tessera* is a single small tile used to build a mosaic. The name fits a screen made of device tiles, which are defined in a layout file in Home Assistant.

<p align="center">
  <img src="docs/tessera-panel.jpg" alt="Tessera running on a Guition ESP32-S3-4848S040 in a 3D-printed stand: a 3×3 grid of device tiles with a header clock and indoor/outdoor temperatures" width="420">
</p>

## Features

- **Tile grid from a Home Assistant layout file**, so tiles change without a firmware rebuild. Each page picks a 2×2, 2×3, or 3×3 grid from its tile count.
- **Toggle lights / switches / fans** via the HA WebSocket API, with optimistic UI updates
- **Per-tile icons** (Material Design Icons) and **live value text** (brightness %, fan speed %)
- **Availability pips** — green when the entity is reachable, amber when `unavailable`
- **Fan low-start** — optionally turn a fan on at a set speed instead of 100%
- **Header**: live clock + indoor and outdoor temperatures (read from HA, no third-party API)
- **Thermostat control** — tap the thermostat tile for a detail view with current temp, mode (Off/Heat/Cool/Auto) and setpoint +/- (dual setpoints in heat_cool)
- **Swipe paging**, idle dimming, and a wake-touch that only wakes the screen

## Hardware

- Guition ESP32-S3-4848S040 (ESP32-S3-WROOM-1 N16R8, 16 MB flash, 8 MB PSRAM)
- ST7701S RGB display, GT911 capacitive touch
- CH340 USB-serial (appears as a COM port)
- **3D-printed stand** for the Guition ESP32-S3-4848S040: [Thingiverse thing:7373058](https://www.thingiverse.com/thing:7373058)

## Toolchain

- [PlatformIO](https://platformio.org/) + Arduino framework
- LVGL 8.4, Arduino_GFX, links2004/WebSockets, ArduinoJson (pinned in `platformio.ini`); `TAMC_GT911` is vendored under `lib/` (patched — see [Known setup notes](#known-setup-notes))

## Requirements

Before you start, you'll need:

- **A running Home Assistant instance** on your network, reachable over HTTP
  (default port `8123`) with the WebSocket API enabled (on by default). Tessera is
  a controller — it does **not** run Home Assistant itself.
- **A Home Assistant long-lived access token** (HA → your profile → **Security** →
  *Long-Lived Access Tokens*, at the bottom of the page) for the panel to
  authenticate with.
- **The entities you want to control** already configured in Home Assistant —
  lights, switches, fans, a `climate` thermostat, and a `weather` entity for the
  outdoor temperature.
- **A 2.4 GHz WiFi network** — the ESP32-S3 does not support 5 GHz. The panel and
  Home Assistant must be reachable on the same network.
- **A computer with [PlatformIO](https://platformio.org/)** and a USB cable for the
  initial flash (later updates can go over the network via OTA).

## Setup

Configuration is split in two, and **neither file is committed**:

- **Credentials** — WiFi, HA host/port/token, timezone — are gathered by a setup
  wizard and stored in `include/secrets.h`.
- **Devices** — your tiles and the header thermostat/weather entities — live in
  `include/config.h`. This file holds no secrets, so it's safe to share or paste
  into an LLM (see [Adding devices](#adding-devices)).

1. **Install PlatformIO** — the build/flash toolchain. Install either the
   [PlatformIO IDE extension for VS Code](https://platformio.org/install/ide?install=vscode)
   or the command-line core (`pip install platformio`); see
   [platformio.org/install](https://platformio.org/install) for all options.

2. **Get the code**
   ```sh
   git clone https://github.com/cruftbox/tessera.git
   cd tessera
   ```

3. **Connect the panel** to your computer with a **data-capable** USB cable (a
   charge-only cable won't work). It appears as a serial port — `COMx` on Windows,
   `/dev/tty.*` on macOS/Linux — which the wizard auto-detects when it flashes.

4. **Add your devices**
   ```sh
   cp include/config.h.example include/config.h
   ```
   Edit `include/config.h` to set the header thermostat/weather entities. Your
   tiles go in a layout file in Home Assistant — see [Adding devices](#adding-devices).

5. **Run the setup wizard** (gathers credentials, then flashes)
   ```sh
   python setup_wizard.py
   ```
   It prompts for your WiFi, HA host/port, a **long-lived access token** (it tells
   you where to find it), and timezone (pick from a list); **validates the token
   against your Home Assistant instance** before continuing; writes
   `include/secrets.h`; then offers to flash the panel over USB and confirm it
   connects. Run it with the PlatformIO Python, which bundles `pyserial` — on
   Windows that's
   `& "$env:USERPROFILE\.platformio\penv\Scripts\python.exe" setup_wizard.py`;
   the script header lists the macOS/Linux paths.

   *Prefer to do it by hand?* Copy `include/secrets.h.example` to
   `include/secrets.h`, fill it in, and build with `pio run --target upload` (set
   your panel's port with `--upload-port <port>` or in `platformio.ini`).

## Updating over the air (OTA)

After the first USB flash, later updates can go over WiFi — no cable. Flash with
the `tessera_ota` environment, pointed at *your* panel's IP:

```sh
pio run -e tessera_ota -t upload --upload-port <panel-ip>
```

Find the IP in your router's device list (or the panel's serial log on first
boot). The `upload_port` baked into `platformio.ini` is only a default —
override it per panel with `--upload-port`. Managing more than one panel?
`python panels.py flash <name> --ota` does this for you — see
[Running multiple panels](#running-multiple-panels).

On **Windows**, the host firewall must let PlatformIO's `espota` helper receive
the panel's connection back, or the upload fails with *"No response from
device"*. If you hit that, allow
`…\.platformio\penv\Scripts\python.exe` through the firewall (Private **and**
Public profiles). A failed OTA is safe — it writes to the inactive flash slot and
only switches over after a complete, verified transfer. Serial logging needs USB;
it isn't available over OTA.

## Running multiple panels

`config.h` and `secrets.h` are compiled into the firmware, so each physical
panel is really its own build. Running more than one means keeping more than
one `(config.h, secrets.h)` pair straight — and telling the panels apart when
one is plugged in for an update. `panels.py` handles both:

```sh
python panels.py new kitchen      # set up the connected board as a new panel, named "kitchen"
python panels.py new              # ...or get prompted for a name
python panels.py register office  # link the connected board to an EXISTING
                                   #   profile that isn't registered to a MAC yet
python panels.py flash            # auto-detects which panel is connected, syncs
                                   #   its files into include/, and reflashes it
python panels.py flash kitchen    # or flash a specific one by name
python panels.py flash kitchen --ota   # ...over WiFi instead of USB
python panels.py list             # show known panels and their MAC addresses
python panels.py identify         # what panel is plugged in right now?
```

Run it with the same PlatformIO Python used for `setup_wizard.py` (see
[Setup](#setup) above) — it needs `pyserial` and `esptool`, which that
interpreter already has. Every command also takes `--port` to skip
auto-detection, and `flash` takes `--host` (override the OTA target) and
`--no-confirm` (skip the post-flash serial check); run with `--help` for the
full list.

Each panel gets its own folder at `panels/<name>/config.h` + `secrets.h`
(gitignored, same as `include/config.h`/`secrets.h`) and a unique
`OTA_HOSTNAME`, so panels don't collide on the same `<name>.local`. Under the
hood, `panels.py` reads the board's hardware MAC address with `esptool` and
keeps a MAC → panel-name map in `panels/registry.json`, so `flash` with no
name works even if you can't remember which panel is on which USB port.

`new` will offer to base a new panel's layout, or its WiFi/HA credentials, on
an existing panel — handy since most panels in one house share the same
network and Home Assistant instance and only differ in which devices they
control. If you already have a single-panel setup (just `include/config.h` and
`include/secrets.h`, no `panels/` folder yet), the first time you run `new` or
`flash` it offers to adopt your existing setup as your first named panel — if
that board wasn't plugged in at the time (so its MAC couldn't be recorded),
plug it in later and run `python panels.py register <name>` to link it.

Once you're using `panels.py`, treat `panels/<name>/config.h` as the source of
truth for that panel and edit it directly — `include/config.h` is just a
staging copy of whichever panel you last synced with `new` or `flash`, and
gets overwritten the next time you sync a different one.

## Working with an LLM (recommended)

Tessera is built to be configured and extended with an AI coding assistant
(Claude, etc.) — and doing so is genuinely the easiest path. The config and the
code are deliberately self-describing for exactly this.

- **Setup & install** — paste this README and `include/config.h.example` into an
  LLM and describe your setup ("Home Assistant at `192.168.x.x`, these devices…").
  It can fill in the layout file, pick icons, and walk you through running the
  setup wizard (which gathers your token and flashes the panel).
- **Adding or changing devices** — paste your `layout.json` and say *"add my garage
  light, entity `switch.garage`."* Every field (`label`, `entity`, `page`, `icon`,
  `pct`, `kelvin`) is documented in `layout.example.json`, so the model has what it
  needs to produce a correct tile.
- **Running more than one panel** — paste `panels.py` alongside a description of
  your panels ("a kitchen one and an office one, same WiFi/HA") and it can walk
  you through `new`/`flash` for each.
- **Modifying the firmware** — each source file carries a module-header comment
  describing its role and gotchas, so an assistant can orient quickly to make UI
  or behavior changes.
- **Troubleshooting** — share serial output or symptoms; common pitfalls (the
  touch coordinate transform, ArduinoJson filter sizing, HA token / IP-ban) are
  documented in the code and below.

You don't *need* an LLM — everything is editable by hand — but the project is
structured to make AI-assisted setup and extension fast and reliable.

## Adding devices

Tiles come from a layout file in Home Assistant, not from the firmware. The panel
downloads `/local/tessera/layout.json` (served from `/config/www/tessera/layout.json`)
at boot and checks it every 60 seconds. A changed layout restarts the panel, which
rebuilds the screen. Adding, moving, or renaming a tile is an HA edit, with no
rebuild or flash.

Each tile is an object in the `tiles` array:

| Field | Meaning |
|---|---|
| `label` | Text shown under the icon (keep short — ~14 chars fits; 24 max) |
| `entity` | The Home Assistant entity, e.g. `light.kitchen`. Tapping the tile toggles it. |
| `page` | Which screen the tile is on (0-based). Up to 9 tiles per page, swipe left/right to change pages. |
| `icon` | An icon name: `lightbulb`, `fan`, `bed`, `ceiling_light`, `sofa`, `coach_lamp`, `fan_light`, `home_temp`, `weather`, or `thermostat`. Defaults to `lightbulb`. |
| `pct` | Optional. For a fan, makes "turn on" start at that % speed instead of 100%. |
| `kelvin` | Optional. For a tunable-white light, the colour temperature in Kelvin to use when turning on (e.g. `3000`). |
| `label_size` | Optional. Font size of the name, in pixels: `14`, `16`, `24` or `32`. Overrides the top-level default for this tile. Larger names wrap onto more lines. |

The file also takes two optional top-level settings:

- `check_seconds`: how often to check for a changed layout (default 60, clamped to
  10 seconds to a day).
- `label_size`: the default name font size for every tile (default 14). A tile's own
  `label_size` overrides it.
- `screensaver_seconds`: idle seconds before the LCARS screensaver starts, counted
  from the last touch (default 300). It never starts before the screen dims, and
  it can be changed without a restart. Only applies when the screensaver is enabled
  in `config.h`.

Each page picks its grid from its tile count: up to 4 tiles use 2×2, 5–6 use 2×3,
and 7–9 use 3×3. The Nest thermostat counts toward its page when it's enabled.

Example — add a garage light on the first page, after the existing tiles:

```json
{ "label": "Garage", "entity": "switch.garage", "page": 0, "icon": "lightbulb" }
```

The entity must already exist in Home Assistant. Save the file in the File editor
and the panel picks it up within a minute.

**Easiest path:** paste your `layout.json` into an LLM and say *"add my garage
light, entity `switch.garage`."* The template `layout.example.json` documents
every field.

## Icons

Tile/header icons come from a generated LVGL font (`src/mdi_icons.c`) built from
[Material Design Icons](https://pictogrammers.com/library/mdi/). The font is
committed, so a normal build needs no extra steps. The `ICON_*` names at the top
of `config.h` are the **complete catalog** — only those glyphs are compiled in.

To add a glyph that isn't in the catalog:

1. Find the icon at [pictogrammers.com/library/mdi](https://pictogrammers.com/library/mdi/)
   and note its hex codepoint (e.g. `F0335`).
2. From `tools/`, install the tooling once, then regenerate the font with your new
   codepoint **appended** to the existing `--range` list:
   ```sh
   cd tools
   npm install            # once — fetches @mdi/font + lv_font_conv
   npx lv_font_conv \
     --font node_modules/@mdi/font/fonts/materialdesignicons-webfont.ttf \
     --size 26 --bpp 4 --format lvgl --no-compress --no-kerning \
     --lv-include lvgl.h -o ../src/mdi_icons.c \
     --range 0xF0335,0xF1797,0xF02E3,0xF0769,0xF04B9,0xF1020,0xF1798,0xF0F55,0xF0595,0xF0393,0xF0XXX
   ```
   (Keep every existing codepoint; just add yours as `0xF0XXX`.)
3. Add a matching define in `config.h`, and a row in the icon table in
   `src/layout.cpp` so layout files can use it by name. The glyph is a UCN with the
   codepoint zero-padded to 8 hex digits — e.g. `F0608` becomes:
   ```c
   #define ICON_GARAGE "\U000F0608"   // garage
   ```
   ```c
   { "garage", ICON_GARAGE },   // in ICON_NAMES[]
   ```

## Known setup notes

- **GT911 patch**: the upstream `TAMC_GT911` library calls `pinMode()` on the
  INT/RST pins even when they're unused (`-1`), which on this board logs
  `Invalid IO 255`. A patched copy is **vendored under `lib/TAMC_GT911`** (the
  INT/RST pin operations in `reset()` are guarded), so a fresh `git clone` + build
  works with no manual patching — and it is intentionally **not** in `lib_deps`, so
  a dependency install can't overwrite it. See `lib/TAMC_GT911/PATCH.md`.
- Serial output requires `ARDUINO_USB_CDC_ON_BOOT=0` (already set in `platformio.ini`)
  so logging goes to the CH340 UART rather than native USB-CDC.
- **Every build embeds the current git commit** (via `scripts/git_version.py`,
  no manual version bumping) and prints it on boot — `Tessera starting...
  (firmware a1b2c3d)`. A `-dirty` suffix means the working tree had
  uncommitted changes at build time, so what's on the panel isn't exactly
  what's in git history. Useful for telling which commit is running on which
  panel, especially with `panels.py` managing more than one.
- **HA IP-bans repeated failed logins.** If the panel retries a wrong token, Home
  Assistant will ban its IP and every request — including a *correct* token
  afterward — gets rejected until the ban clears. This is why the wizards
  validate your token **from the PC** before ever flashing it to the panel. If
  you do get banned: clear the entry in HA's `ip_bans.yaml` and restart Home
  Assistant.
- **The serial port is hardcoded to `COM7`** in `serial_read.py` and `platformio.ini`
  (`monitor_port`/`upload_port`). `setup_wizard.py` and `panels.py` both
  auto-detect the port when they flash, so this only matters for the serial
  monitor or a manual `pio` upload. To target a different port without editing
  `platformio.ini`, set `TESSERA_PORT` (a COM port for USB, or an IP/`*.local`
  hostname for the `tessera_ota` env) — see `scripts/select_port.py`.

## Project layout

```
src/             main, display, touch, ui, ha_client, ota, mdi_icons (generated font)
include/         config.h.example (devices), secrets.h.example (credentials), lv_conf.h
tools/           icon-font generation (@mdi/font + lv_font_conv)
scripts/         select_port.py (TESSERA_PORT override for pio upload/monitor)
panels/          per-panel profiles + registry.json (gitignored; created by panels.py)
setup_wizard.py  first-run credential wizard — writes include/secrets.h, flashes
panels.py        multi-panel manager — register/identify/flash by name or MAC
serial_read.py   serial-monitor helper (reads the boot log over USB)
platformio.ini, partitions_16MB_ota.csv
```

## License

[MIT](LICENSE).
