# Card Craft — M5Stack Tab5 port (ST7121)

A port of [Card Craft](https://github.com/therezor/card-craft) by therezor (MIT license,
see `LICENSE`) from the M5Stack Cardputer to the **M5Stack Tab5** (ESP32-P4).
Upstream commit: `2cf7513` (2026-08-24).

For now it targets **only the Tab5 revision with the ST7121 panel** (BSP "board version 3",
DSI lane rate 965 Mbps). Older ILI9881C / ST7123 units are untested.

The Tab5 has an ESP32-P4 **rev v1.x**, so `sdkconfig.defaults` sets
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` (IDF 6.x otherwise builds for rev ≥ v3.1 and flashing is refused).

## How it is built

- **ESP-IDF v6.1** + BSP `espressif/m5stack_tab5 ~1.3.2` (the only one that drives the ST7121).
- `main/game/` — the original game code, **unchanged** (raycaster, world, mobs, crafting, sounds).
- `main/compat/` — thin `Arduino.h`, `Preferences.h` (NVS) and `M5GFX.h` shims, so the
  original compiles without the Arduino core and M5GFX.
- `main/hal_tab5.cpp` — the `hal.h` implementation:
  - the game's 240×135 frame is scaled straight into one of the two DSI framebuffers
    in PSRAM (no intermediate copy), then flipped on the panel's frame-done,
  - two display layouts, switched automatically by keyboard detection
    (see [Display layouts](#display-layouts)),
  - on-screen touch controls (multitouch, up to 5 fingers),
  - keyboard mapping (Tab5 Keyboard and USB, both at once),
  - BUILD pacing: one block per press, slower repeat when held.
- `main/tab5_kbd.cpp` — **M5Stack Tab5 Keyboard** (SKU A164): I2C 0x6D on I2C0
  (SDA GPIO0, SCL GPIO1), normal mode (press and release), hot-plug.
  Protocol per [M5Tab5-Keyboard-UserDemo](https://github.com/m5stack/M5Tab5-Keyboard-UserDemo) (MIT).
- `main/usb_kbd.cpp` — USB HID keyboard (boot protocol) on the USB-A port.
- `main/audio_tab5.cpp` — 8-channel mixer (wavetable voices + 8-bit PCM) into the ES8388, 48 kHz.

## Controls

| Action | Touch | Keyboard |
|---|---|---|
| Move / turn | D-pad, bottom left (corners = diagonal forward) | arrows |
| Mine / attack (hold) | MINE | `W` |
| Build (press = 1 block, hold = repeat after 0.45 s, 4/s) | BUILD | `A` |
| Jump | JUMP | `SPACE` |
| Look up / down | LOOK▲ / LOOK▼ (right) | `E` / `S` |
| Centre the view | CTR | `ENTER` |
| Hotbar slot 1–9 | numbers, bottom centre | `1`–`9` |
| Drop | DROP | `Q` |
| Craft | CRAFT | `TAB` |
| Confirm / back | OK / ESC | `ENTER` / `ESC` |

## Display layouts

The 720×1280 panel is used in landscape (1280×720). The layout follows whether a
keyboard is attached:

| | No keyboard (touch layout) | Keyboard attached (keyboard layout) |
|---|---|---|
| Game view | 960×540, 4× integer scale, top centre | 1280×720 full screen, 5.33× nearest-neighbour |
| Touch controls | in the strips left, right and below the view | hidden, touch ignored |
| In-game hints | button names (`MINE`, `BUILD`, `OK`…) | key names (`W`, `A`, `ENTER`…) |

The game is 16:9 like the panel, so the full-screen view keeps its proportions; game pixels
simply alternate between 5 and 6 panel pixels.

Switching happens on its own, also in the middle of a game:

- **Tab5 Keyboard** — probed once a second while absent; once found it is polled every
  5 ms and counted as gone after 20 failed reads in a row (well under a second).
- **USB keyboard** — follows the USB host's connect / disconnect events.
- Either keyboard selects the keyboard layout; the touch layout returns when neither is
  attached. Both framebuffers are repainted, so a switch takes two frames.

## Build and flash

From an ESP-IDF v6.x environment:

```sh
idf.py set-target esp32p4      # first time, or after changing sdkconfig.defaults
idf.py build
idf.py -p <PORT> flash monitor
```

On Windows, `build_flash.bat <PORT> [clean|nobuild]` does the same from an ESP-IDF command prompt.

The default orientation is `TAB5_ROTATE_180=1`; `-DTAB5_ROTATE_180=0` turns the picture
by 180°, and touch turns with it.
