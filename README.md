# UltraGB Overlay

A Game Boy / Game Boy Color emulator overlay for the Nintendo Switch, built on [libultrahand](https://github.com/ppkantorski/libultrahand). Play GB and GBC games anywhere.

---

## Features

### Emulation
- Full **Game Boy** (DMG) and **Game Boy Color** (GBC/CGB) emulation via [Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) (a fork of [Peanut-GB](https://github.com/deltabeard/Peanut-GB))
- Supports MBC1, MBC2, MBC3 (with RTC), MBC5, and ROM-only cartridges
- LCD ghosting / frame blending for accurate DMG flicker reproduction
- Fast-forward (4× speed) via ZR double-click-hold

### Display
- **Overlay mode** — emulator drawn inside the standard Ultrahand overlay panel (448×720 framebuffer), scaled to fit
- **Windowed mode** — framebuffer-accurate floating window placed anywhere on screen
  - Integer scales: 1×, 2×, 3×, 4× (4× requires non-limited memory)
  - Repositionable by touch hold (≈1 s) or KEY_PLUS hold (≈1 s) + left stick
  - Position saved per session in `config.ini`
- **Per-game palette modes**, cycled with the A button:
  - `GBC` — native GBC colour (default for colour titles; uses hardware colour lookup for DMG games on GBC)
  - `DMG` — classic four-shade greyscale
  - `Native` — raw pixel values passed through unchanged

### Controls (Overlay Mode)
| Input | Action |
|---|---|
| A / B / X / Y | GB A / B / A / B |
| D-Pad | GB D-Pad |
| L / R | GB Start / Select |
| ZR double-click-hold | Fast-forward (4×) |
| ZL double-click | Toggle background pass-through |
| Y (ROM list) | Open per-game config |
| Launch combo | Open/close overlay |

### Controls (Windowed Mode)
| Input | Action |
|---|---|
| A / B / X / Y | GB A / B / A / B |
| D-Pad | GB D-Pad |
| L / R | GB Start / Select |
| ZR double-click-hold | Fast-forward (4×) |
| ZL double-click | Toggle controller pass-through to background app |
| Touch hold (≈1 s) | Reposition window — drag while held, releases to save |
| KEY_PLUS hold (≈1 s) + left stick | Reposition window via joystick |
| Launch combo | Exit windowed mode, return to normal overlay |

### Save System
- **Battery saves (SRAM)** — written on ROM unload, stored at `sdmc:/config/ultragb/saves/`
- **Quick-resume state** — full CPU/PPU/APU snapshot saved automatically on overlay close, restored on next launch; stored at `sdmc:/config/ultragb/states/internal/`
- **Per-game user save states** — manual slots stored at `sdmc:/config/ultragb/states/<game>/`

### Per-Game Configuration
Each ROM has an individual settings screen (`Y` in the ROM list) with:
- Palette mode selection
- Save data management
- Manual save state slots

### Settings
- Windowed mode toggle and scale (1×–4×, memory-aware)
- Wallpaper support (Ultrahand wallpaper system)
- Sound effects toggle
- All settings stored in `sdmc:/config/ultragb/config.ini`

---

## Requirements

- Nintendo Switch with [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) custom firmware
- [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) and [nx-ovlloader v2.0.0+](https://github.com/ppkantorski/nx-ovlloader) installed
- ROMs placed anywhere accessible on the SD card (`.gb` / `.gbc`)

---

## Installation

1. Download the latest `gbemu.ovl` from [Releases](../../releases)
2. Copy it to `sdmc:/switch/.overlays/`
3. Launch via Ultrahand

---

## Building

**Requirements:** [devkitPro](https://devkitpro.org) with `devkitARM`, `libnx`, and the Ultrahand libultrahand library.

```sh
export DEVKITPRO=/opt/devkitpro
make -j6
```

Output: `gbemu.ovl`

The build uses C++26, ARMv8-A SIMD/CRC/crypto tuned for Cortex-A57, LTO with 6 parallel LTRANS jobs, and links against `libcurl`, `mbedtls`, and `libnx`.

---

## File Layout

```
sdmc:/
├── switch/
│   └── .overlays/
│       └── gbemu.ovl
└── config/
    └── ultragb/
        ├── config.ini          ← global settings
        ├── saves/              ← SRAM battery saves
        │   └── <game>/
        ├── states/             ← save states
        │   └── <game>/
        └── configure/          ← per-game settings
            └── <game>.ini
```

---

## Credits

- **[ppkantorski](https://github.com/ppkantorski)** — UltraGB Overlay, Ultrahand Overlay, libultrahand
- **[Mr-PauI](https://github.com/Mr-PauI)** — [Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) (GBC core, CGB palette, dual-fetch optimisations)
- **[deltabeard](https://github.com/deltabeard)** — [Peanut-GB](https://github.com/deltabeard/Peanut-GB) (original GB core)
- **[LIJI32](https://github.com/LIJI32)** — [SameBoy](https://github.com/LIJI32/SameBoy) (portions used in Walnut-CGB)

---

## License

UltraGB Overlay is released under the [GPLv2](LICENSE).
