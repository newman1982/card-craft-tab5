# Card Craft — port pro M5Stack Tab5 (ST7121)

Port hry [Card Craft](https://github.com/therezor/card-craft) (therezor, MIT licence,
viz `LICENSE`) z M5Stack Cardputeru na **M5Stack Tab5** (ESP32-P4).
Zdrojový commit originálu: `2cf7513` (2026-08-24).

Zatím cílí **jen na revizi Tab5 s panelem ST7121** (BSP „board version 3“,
DSI lane 965 Mbps). Starší ILI9881C / ST7123 kusy nejsou laděné.

Tab5 má ESP32-P4 **rev. v1.x** — `sdkconfig.defaults` proto nastavuje
`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` (IDF 6.x jinak staví pro rev ≥ v3.1 a flash odmítne).

## Jak je to postavené

- **ESP-IDF v6.1** + BSP `espressif/m5stack_tab5 ~1.3.2` (jen ten umí ST7121).
- `main/game/` — herní kód originálu **beze změn** (raycaster, svět, moby, crafting, zvuky).
- `main/compat/` — tenké shimy `Arduino.h`, `Preferences.h` (NVS), `M5GFX.h`,
  aby originál šel přeložit bez Arduino core a M5GFX.
- `main/hal_tab5.cpp` — implementace `hal.h`:
  - snímek 240×135 se 4× škáluje na 960×540 (landscape), přímo do jednoho ze
    dvou DSI framebufferů v PSRAM, pak flip na vsync,
  - dotykové ovládání v pásech kolem obrazu (multitouch, až 5 prstů),
  - mapování klávesnice (Tab5 Keyboard i USB, obě najednou).
- `main/tab5_kbd.cpp` — **M5Stack Tab5 Keyboard** (SKU A164): I2C 0x6D na I2C0
  (SDA GPIO0, SCL GPIO1), normální režim (stisk i uvolnění), hot-plug.
  Protokol podle [M5Tab5-Keyboard-UserDemo](https://github.com/m5stack/M5Tab5-Keyboard-UserDemo) (MIT).
- `main/usb_kbd.cpp` — USB HID klávesnice (boot protokol) na USB-A portu.
- `main/audio_tab5.cpp` — 8kanálový mixer (wavetable hlasy + 8bit PCM) do ES8388, 48 kHz.

## Ovládání

| Akce | Dotyk | Klávesnice |
|---|---|---|
| Pohyb / otáčení | D-pad vlevo dole (rohy = šikmo vpřed) | šipky |
| Těžit / útok (držet) | MINE | `W` |
| Stavět (stisk = 1 kostka, držení = opakování po 0,45 s, 4×/s) | BUILD | `A` |
| Skok | JUMP | `SPACE` |
| Pohled nahoru / dolů | LOOK▲ / LOOK▼ (vpravo) | `E` / `S` |
| Vycentrovat pohled | CTR | `ENTER` |
| Slot 1–9 | čísla dole uprostřed | `1`–`9` |
| Zahodit | DROP | `Q` |
| Crafting | CRAFT | `TAB` |
| Potvrdit / zpět | OK / ESC | `ENTER` / `ESC` |

Když je připojená klávesnice, dotyková tlačítka zmizí, obraz se roztáhne na celou
obrazovku 1280×720 (16:9 jako hra, měřítko 5,33×, nearest-neighbor) a nápovědy ve hře ukazují názvy kláves. Po odpojení se vrátí 4× obraz s tlačítky.

## Build a flash

```bat
build_flash.bat              :: build + flash na COM13 + monitor
build_flash.bat COM5
build_flash.bat COM13 clean  :: po změně sdkconfig.defaults
```

Orientace je `TAB5_ROTATE_180=1` (ověřeno na ST7121 kusu); `-DTAB5_ROTATE_180=0`
ji otočí zpět a dotyk se otočí s ní.
