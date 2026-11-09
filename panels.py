#!/usr/bin/env python3
"""Tessera multi-panel manager — register, identify, and (re)flash multiple
physical panels from one repo checkout.

Why this exists: config.h (device layout + OTA hostname) and secrets.h (WiFi/HA
credentials) are compile-time files under include/, so each physical panel is,
in effect, its own firmware build. Running more than one panel means keeping
more than one (config.h, secrets.h) pair straight — and telling them apart when
you plug one in for an update.

This script solves both:
  - Each panel gets a profile at panels/<name>/{config.h, secrets.h}.
  - Each panel's ESP32 has a unique, stable hardware MAC address. panels.py
    reads it via esptool and keeps a MAC -> profile-name map in
    panels/registry.json, so it can recognize which panel is connected.

Everything under panels/ is gitignored — profiles hold the same kind of
per-device secrets/layout info as include/config.h and include/secrets.h
already do. The workflow itself is documented in the README, not in panels/.

Commands:
    python panels.py list                 Show known panels and their MACs
    python panels.py identify             What panel is plugged in right now?
    python panels.py new [name]           Set up the connected board as a new panel
    python panels.py register [name]      Link the connected board to an EXISTING
                                           profile (e.g. one adopted before this
                                           board happened to be plugged in)
    python panels.py flash [name]         Sync + build + flash a panel over USB
    python panels.py flash NAME --ota     ...or over WiFi (ArduinoOTA)

With no name, `flash` auto-detects which panel is connected (by MAC) and asks
to run `new` if it doesn't recognize the board yet.

Run with the PlatformIO Python (has pyserial + esptool available) — see
setup_wizard.py's docstring for the exact command per OS.
"""

import argparse
import json
import os
import re
import shutil
import subprocess
import sys

import setup_wizard as sw  # reuses credential prompts, HA validation, flash/serial helpers

PANELS_DIR = os.path.join(sw.ROOT, "panels")
REGISTRY_PATH = os.path.join(PANELS_DIR, "registry.json")
INCLUDE_CONFIG_PATH = os.path.join(sw.ROOT, "include", "config.h")
CONFIG_EXAMPLE_PATH = os.path.join(sw.ROOT, "include", "config.h.example")


# --- MAC address identification ----------------------------------------------

def find_esptool():
    for name in ("esptool", "esptool.py"):
        path = shutil.which(name)
        if path:
            return [path]
    home = os.path.expanduser("~")
    for rel in (
        os.path.join(".platformio", "penv", "Scripts", "esptool.exe"),
        os.path.join(".platformio", "penv", "bin", "esptool"),
    ):
        cand = os.path.join(home, rel)
        if os.path.exists(cand):
            return [cand]
    return None


def read_chip_mac(port):
    """Read the connected board's MAC via esptool. Returns 'aa:bb:cc:dd:ee:ff'
    (lowercase) or None if it couldn't be determined."""
    esptool = find_esptool()
    if not esptool:
        print("  esptool not found - can't identify the board automatically.")
        return None
    cmd = esptool + (["--port", port] if port else []) + ["read_mac"]
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, timeout=30)
    except Exception as e:  # noqa: BLE001
        print(f"  esptool failed: {e}")
        return None
    m = re.search(r"MAC:\s*([0-9A-Fa-f:]{17})", result.stdout)
    if not m:
        print("  Could not read a MAC address from esptool output.")
        if result.stdout:
            print(result.stdout[-500:])
        return None
    return m.group(1).lower()


# --- registry (MAC -> panel name) --------------------------------------------

def load_registry():
    if not os.path.exists(REGISTRY_PATH):
        return {}
    with open(REGISTRY_PATH, encoding="utf-8") as f:
        return json.load(f)


def save_registry(reg):
    os.makedirs(PANELS_DIR, exist_ok=True)
    with open(REGISTRY_PATH, "w", encoding="utf-8", newline="\n") as f:
        json.dump(reg, f, indent=2, sort_keys=True)
        f.write("\n")


# --- panel profiles (panels/<name>/{config.h, secrets.h}) --------------------

def profile_dir(name):
    return os.path.join(PANELS_DIR, name)


def profile_paths(name):
    d = profile_dir(name)
    return os.path.join(d, "config.h"), os.path.join(d, "secrets.h")


def list_profiles():
    if not os.path.isdir(PANELS_DIR):
        return []
    return sorted(
        n for n in os.listdir(PANELS_DIR)
        if os.path.isfile(os.path.join(PANELS_DIR, n, "config.h"))
    )


def set_ota_hostname(text, name):
    """Set (or insert) #define OTA_HOSTNAME "name" in a config.h's text."""
    pattern = re.compile(r'#define\s+OTA_HOSTNAME\s+"[^"]*"')
    replacement = f'#define OTA_HOSTNAME "{name}"'
    if pattern.search(text):
        return pattern.sub(replacement, text, count=1)
    lines = text.splitlines()
    insert_at = 1 if lines and lines[0].startswith("#pragma once") else 0
    lines.insert(insert_at, f"\n{replacement}\n")
    return "\n".join(lines)


def sync_profile_to_include(name):
    """Copy a panel profile's files into include/ so the next build uses it."""
    cfg_src, sec_src = profile_paths(name)
    shutil.copyfile(cfg_src, INCLUDE_CONFIG_PATH)
    shutil.copyfile(sec_src, sw.SECRETS_PATH)
    print(f"  Activated profile '{name}' -> include/config.h, include/secrets.h")


def maybe_adopt_legacy():
    """If no profiles exist yet but include/config.h or include/secrets.h
    already does (the pre-multi-panel setup), offer to adopt it as the first
    profile so existing single-panel users aren't forced to start over."""
    if list_profiles():
        return
    have_cfg = os.path.exists(INCLUDE_CONFIG_PATH)
    have_sec = os.path.exists(sw.SECRETS_PATH)
    if not (have_cfg or have_sec):
        return
    print("\n  No panel profiles yet, but include/ already has a configured panel.")
    if input("  Adopt it as your first panel profile? [Y/n]: ").strip().lower() == "n":
        return
    text = open(INCLUDE_CONFIG_PATH, encoding="utf-8").read() if have_cfg else ""
    m = re.search(r'#define\s+OTA_HOSTNAME\s+"([^"]*)"', text)
    default_name = m.group(1) if m else "tessera"
    name = input(f"  Name for this panel [{default_name}]: ").strip() or default_name
    os.makedirs(profile_dir(name), exist_ok=True)
    cfg_dst, sec_dst = profile_paths(name)
    if have_cfg:
        shutil.copyfile(INCLUDE_CONFIG_PATH, cfg_dst)
    if have_sec:
        shutil.copyfile(sw.SECRETS_PATH, sec_dst)
    print(f"  Adopted as profile '{name}' ({profile_dir(name)}).")
    if input("  Is this panel connected over USB right now, so its MAC can be "
             "remembered? [y/N]: ").strip().lower() == "y":
        port = sw.detect_serial_port()
        mac = read_chip_mac(port)
        if mac:
            reg = load_registry()
            reg[mac] = name
            save_registry(reg)
            print(f"  Registered {mac} -> '{name}'.")


# --- OTA flashing (extends setup_wizard's USB-only flash) --------------------

def flash_ota(name, host=None):
    pio = sw.find_platformio()
    if not pio:
        print("  platformio not found on PATH.")
        return False
    target = host or f"{name}.local"
    env = dict(os.environ, PYTHONIOENCODING="utf-8", TESSERA_PORT=target)
    cmd = pio + ["run", "-e", "tessera_ota", "-t", "upload"]
    print(f"\n  OTA flashing '{name}' at {target}...\n")
    result = subprocess.run(cmd, cwd=sw.ROOT, env=env)
    if result.returncode != 0:
        print("\n  OTA flash failed. Is the panel powered on, and is the "
              "hostname/IP correct? (--host to override)")
        return False
    print("\n  OTA flash complete.")
    return True


# --- commands ------------------------------------------------------------

def cmd_list(args):
    profiles = list_profiles()
    if not profiles:
        print("No panel profiles yet. Run: python panels.py new")
        return 0
    reg = load_registry()
    by_name = {}
    for mac, name in reg.items():
        by_name.setdefault(name, []).append(mac)
    print(f"{'Panel':<20} MAC address(es)")
    for name in profiles:
        macs = ", ".join(by_name.get(name, [])) or "(not registered yet - run `identify` or `new`)"
        print(f"{name:<20} {macs}")
    return 0


def cmd_register(args):
    """Link the connected board's MAC to an EXISTING profile, without touching
    its config.h/secrets.h. For the case `new` can't handle: a profile that
    already exists (e.g. adopted from a legacy include/ setup) but whose board
    wasn't plugged in at the time, so it was never added to the registry."""
    profiles = list_profiles()
    name = args.name
    if not name:
        if not profiles:
            print("No panel profiles yet. Run: python panels.py new")
            return 1
        print("\n  Link the connected board to which panel?")
        for i, p in enumerate(profiles, 1):
            print(f"    {i}. {p}")
        choice = input(f"  Choice [1-{len(profiles)}]: ").strip()
        if not (choice.isdigit() and 1 <= int(choice) <= len(profiles)):
            print("  Aborted.")
            return 1
        name = profiles[int(choice) - 1]
    if name not in profiles:
        known = ", ".join(profiles) or "(none yet)"
        print(f"  No profile named '{name}'. Known panels: {known}. Use `new` to create one.")
        return 1

    port = args.port or sw.detect_serial_port()
    mac = read_chip_mac(port)
    if not mac:
        return 1

    reg = load_registry()
    existing = reg.get(mac)
    if existing and existing != name:
        if input(f"  This board is currently registered as '{existing}'. "
                 f"Re-register it as '{name}' instead? [y/N]: ").strip().lower() != "y":
            print("  Aborted.")
            return 1

    reg[mac] = name
    save_registry(reg)
    print(f"  Registered {mac} -> '{name}'.")
    return 0


def cmd_identify(args):
    port = args.port or sw.detect_serial_port()
    if not port:
        print("Could not detect a serial port. Pass one with --port.")
        return 1
    mac = read_chip_mac(port)
    if not mac:
        return 1
    reg = load_registry()
    name = reg.get(mac)
    if name:
        print(f"MAC {mac} on {port} -> panel '{name}'")
    else:
        print(f"MAC {mac} on {port} -> not registered. Run: python panels.py new")
    return 0


def cmd_new(args):
    maybe_adopt_legacy()
    port = args.port or sw.detect_serial_port()
    mac = read_chip_mac(port) if port else None
    if mac:
        existing = load_registry().get(mac)
        if existing:
            print(f"  This board (MAC {mac}) is already registered as '{existing}'.")
            print(f"  Use: python panels.py flash {existing}")
            return 1

    name = args.name or input("\n  Name for this new panel (e.g. 'kitchen', 'office'): ").strip()
    if not name:
        print("  A name is required.")
        return 1
    if name in list_profiles():
        print(f"  A profile named '{name}' already exists. Pick another name.")
        return 1

    # --- config.h: start from an existing panel's layout, or the blank template ---
    profiles = list_profiles()
    base_cfg = CONFIG_EXAMPLE_PATH
    if profiles:
        print("\n  Base this panel's device layout on:")
        print("    0. Blank template (include/config.h.example)")
        for i, p in enumerate(profiles, 1):
            print(f"    {i}. Copy panel '{p}'s layout (edit the tiles afterward)")
        choice = input(f"  Choice [0-{len(profiles)}] (default 0): ").strip()
        if choice.isdigit() and 1 <= int(choice) <= len(profiles):
            base_cfg = profile_paths(profiles[int(choice) - 1])[0]

    cfg_text = open(base_cfg, encoding="utf-8").read()
    cfg_text = set_ota_hostname(cfg_text, name)
    os.makedirs(profile_dir(name), exist_ok=True)
    cfg_dst, sec_dst = profile_paths(name)
    with open(cfg_dst, "w", encoding="utf-8", newline="\n") as f:
        f.write(cfg_text)
    print(f'  Wrote {cfg_dst} (OTA_HOSTNAME set to "{name}" - this panel\'s devices come from its HA layout.json)')

    # --- secrets.h: copy from an existing panel, or run the credential wizard ---
    values = {}
    if profiles:
        print("\n  Reuse WiFi/HA credentials from an existing panel?")
        print("    0. No - enter fresh credentials")
        for i, p in enumerate(profiles, 1):
            print(f"    {i}. Copy from '{p}'")
        choice = input(f"  Choice [0-{len(profiles)}] (default 0): ").strip()
        if choice.isdigit() and 1 <= int(choice) <= len(profiles):
            values = sw.read_existing(profile_paths(profiles[int(choice) - 1])[1])
            print("  Loaded - press Enter on each prompt to keep, or type to change:")

    for key, label, default, is_secret in sw.FIELDS:
        if key in sw.HINTS:
            print(sw.HINTS[key])
        if key == "TZ_INFO":
            values[key] = sw.ask_timezone(values.get(key, default))
        else:
            values[key] = sw.ask(label, values.get(key, default), is_secret)

    while True:
        print("\n  Validating HA token against your HA instance...")
        ok, msg = sw.validate_ha(values["HA_HOST"], values["HA_PORT"], values["HA_TOKEN"])
        print(f"  -> {msg}")
        if ok:
            break
        choice = input("\n  Validation failed. (r)e-enter Home Assistant info, "
                       "(c)ontinue anyway, or (q)uit? [r/c/q]: ").strip().lower()
        if choice == "c":
            break
        if choice == "q":
            print("  Aborted - profile not completed.")
            return 1
        for key in ("HA_HOST", "HA_PORT", "HA_TOKEN"):
            lbl = next(f[1] for f in sw.FIELDS if f[0] == key)
            sec = next(f[3] for f in sw.FIELDS if f[0] == key)
            values[key] = sw.ask(lbl, values[key], sec)

    sw.write_secrets(values, sec_dst)
    sync_profile_to_include(name)

    if mac:
        reg = load_registry()
        reg[mac] = name
        save_registry(reg)
        print(f"  Registered {mac} -> '{name}'.")
    elif port:
        print(f"  Couldn't read this board's MAC to register it automatically - "
              f"`python panels.py identify` after flashing, or just keep using "
              f"`python panels.py flash {name}` (by name) for this board.")

    if input(f"\n  Flash '{name}' to this board now? [Y/n]: ").strip().lower() != "n":
        if sw.flash(port):
            if input("  Watch serial to confirm WiFi + HA connect? [Y/n]: ").strip().lower() != "n":
                sw.serial_confirm(port)
    print(f"\nDone. Edit panels/{name}/config.h any time, then: python panels.py flash {name}")
    return 0


def cmd_flash(args):
    maybe_adopt_legacy()
    name = args.name
    port = args.port or sw.detect_serial_port()

    if not name:
        mac = read_chip_mac(port) if port else None
        name = load_registry().get(mac) if mac else None
        if not name:
            print("\n  This board isn't registered to a known panel profile yet.")
            if input("  Set it up now as a new panel? [Y/n]: ").strip().lower() != "n":
                return cmd_new(argparse.Namespace(name=None, port=port))
            print("  Aborted.")
            return 1
        print(f"  Detected panel: '{name}'")

    if name not in list_profiles():
        known = ", ".join(list_profiles()) or "(none yet)"
        print(f"  No profile named '{name}'. Known panels: {known}")
        return 1

    sync_profile_to_include(name)

    if args.ota:
        return 0 if flash_ota(name, args.host) else 1
    if sw.flash(port):
        if not args.no_confirm:
            sw.serial_confirm(port)
    return 0


# --- main --------------------------------------------------------------------

def build_parser():
    p = argparse.ArgumentParser(description="Manage and flash multiple Tessera panels.")
    sub = p.add_subparsers(dest="cmd")

    sp = sub.add_parser("list", help="Show known panel profiles and their MAC addresses")
    sp.set_defaults(func=cmd_list)

    sp = sub.add_parser("identify", help="Show which panel is connected right now")
    sp.add_argument("--port")
    sp.set_defaults(func=cmd_identify)

    sp = sub.add_parser("new", help="Set up the currently-connected board as a new panel")
    sp.add_argument("name", nargs="?", help="Panel name (prompted if omitted)")
    sp.add_argument("--port")
    sp.set_defaults(func=cmd_new)

    sp = sub.add_parser("register", help="Link the connected board to an EXISTING profile "
                                          "(e.g. one adopted before this board was plugged in)")
    sp.add_argument("name", nargs="?", help="Existing panel name (prompted if omitted)")
    sp.add_argument("--port")
    sp.set_defaults(func=cmd_register)

    sp = sub.add_parser("flash", help="Sync a panel's profile into include/, then build + flash it")
    sp.add_argument("name", nargs="?", help="Panel name (auto-detected from the connected board if omitted)")
    sp.add_argument("--port")
    sp.add_argument("--ota", action="store_true", help="Flash over WiFi (ArduinoOTA) instead of USB")
    sp.add_argument("--host", help="OTA target host/IP (default: <name>.local)")
    sp.add_argument("--no-confirm", action="store_true", help="Skip the post-flash serial check")
    sp.set_defaults(func=cmd_flash)

    return p


def main():
    parser = build_parser()
    args = parser.parse_args()
    if not getattr(args, "cmd", None):
        parser.print_help()
        return 1
    return args.func(args) or 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\nAborted.")
        sys.exit(130)
