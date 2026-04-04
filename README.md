# UltraGB Overlay
[![platform](https://img.shields.io/badge/platform-Switch-898c8c?logo=C++.svg)](https://gbatemp.net/forums/nintendo-switch.283/?prefix_id=44)
[![language](https://img.shields.io/badge/language-C++-ba1632?logo=C++.svg)](https://github.com/topics/cpp)
[![GPLv2 License](https://img.shields.io/badge/license-GPLv2-189c11.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Latest Version](https://img.shields.io/github/v/release/ppkantorski/UltraGB-Overlay?label=latest&color=blue)](https://github.com/ppkantorski/UltraGB-Overlay/releases/latest)
[![GitHub Downloads](https://img.shields.io/github/downloads/ppkantorski/UltraGB-Overlay/total?color=6f42c1)](https://somsubhra.github.io/github-release-stats/?username=ppkantorski&repository=UltraGB-Overlay&page=1&per_page=300)
[![HB App Store](https://img.shields.io/endpoint?url=https://raw.githubusercontent.com/ppkantorski/UltraGB-Overlay/main/.github/hbappstore.json&label=hb%20app%20store&color=6f42c1)](https://hb-app.store/switch/UltraGBOverlay)
[![GitHub issues](https://img.shields.io/github/issues/ppkantorski/UltraGB-Overlay?color=222222)](https://github.com/ppkantorski/UltraGB-Overlay/issues)
[![GitHub stars](https://img.shields.io/github/stars/ppkantorski/UltraGB-Overlay)](https://github.com/ppkantorski/UltraGB-Overlay/stargazers)

A Game Boy / Game Boy Color emulator overlay for the Nintendo Switch, built on [libultrahand](https://github.com/ppkantorski/libultrahand).

Play GB and GBC games on top of any running application.

---

## Features

### Emulation
- Full **Game Boy** (DMG) and **Game Boy Color** (GBC/CGB) emulation via [Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) (a fork of [Peanut-GB](https://github.com/deltabeard/Peanut-GB))
- Supports ROM-only, MBC1, MBC2, MBC3 (with RTC), and MBC5 (including rumble) cartridge types
- Accurate 59.73 Hz Game Boy clock rate, decoupled from the display vsync
- LCD ghosting / frame blending — per-game 50/50 blend of consecutive frames to reproduce the phosphor persistence that 30 Hz flickering transparency effects were designed for
- Fast-forward (4×) via ZR double-click-hold; audio pauses for the duration and resumes cleanly on release

### Display Modes

**Overlay mode** — emulator drawn inside the standard Ultrahand overlay panel (448×720 framebuffer) alongside the UltraGB menu chrome.
- **2.5× scale** (default) — 400×360 viewport, fills the overlay width
- **2× pixel-perfect** — 320×288 viewport, integer scale with letterbox; toggleable in-game by tapping the screen or a quick RS press

**Windowed mode** — framebuffer-accurate floating window placed anywhere on screen with no UI chrome.
- Integer scales: 1× through 5× (memory-gated; see Requirements)
- Repositionable by touch hold (≈1 s) or KEY_PLUS hold (2 s) + left stick
- RS / LS click to step scale up / down in-game (relaunches at new scale)
- Position and scale saved to `config.ini`

### Palette Modes
Cycled per game from the per-game config screen. All modes work for both DMG and CGB games.

| Mode | Behaviour |
|---|---|
| `GBC` (default) | CGB games use hardware colour. DMG games receive the real GBC boot ROM title-based colorisation (per-game lookup from hardware-verified palette database); unrecognised or unlicensed games fall back to greyscale. |
| `SGB` | Same title lookup as GBC, but applied to all games regardless of licensee, with a warm amber fallback for unrecognised titles. |
| `DMG` | Classic four-shade green Game Boy LCD tint. |
| `Native` | True greyscale — raw luminance values, no tint. |

### LCD Grid Effect
Simulates the dark inter-pixel gap of a real Game Boy Color LCD by dimming the last row and column of each scaled source pixel block to approximately 12.5% brightness. Applies in both overlay and windowed modes; automatically invisible at windowed 1× (no room for a gap at 1:1 scale).

### Virtual On-Screen Controls (Overlay Mode)
D-pad, A, B, Start, and Select are drawn below the game screen and mapped to touch input each frame.

---

## Controls

### Overlay Mode (In-Game)

| Input | Action |
|---|---|
| A | GB A |
| B | GB B |
| X / + | GB Start |
| Y / − | GB Select |
| D-Pad | GB D-Pad |
| Touch (virtual buttons) | GB D-Pad / A / B / Start / Select |
| ZR double-click-hold | Fast-forward (4×) |
| Quick tap on game screen | Toggle 2.5× ↔ 2× scale |
| RS quick-release | Toggle 2.5× ↔ 2× scale |
| Launch combo | Back to ROM selector (normal) / close overlay (quick-launch mode) |

### Windowed Mode (In-Game)

| Input | Action |
|---|---|
| A | GB A |
| B | GB B |
| X / + | GB Start |
| Y / − | GB Select |
| D-Pad | GB D-Pad |
| ZR double-click-hold | Fast-forward (4×) |
| ZL double-click, then hold (≈0.5 s) | Toggle controller pass-through to background app |
| Touch hold inside window (≈1 s) | Enter drag mode — move window, release to save position |
| KEY_PLUS hold (2 s) + left stick | Joystick reposition mode |
| RS click (solo) | Step scale up (1× → 2× → … → 5×) |
| LS click (solo) | Step scale down |
| Launch combo | Return to UltraGB menu / close overlay (quick-exit mode) |

### ROM Selector

| Input | Action |
|---|---|
| A | Launch ROM |
| Y | Open per-game config |
| Right / Settings footer | Go to Settings page |
| Left / Games footer | Return to ROM list (from Settings) |
| B | Close overlay |

---

## Save System

- **Battery saves (SRAM)** — written on ROM unload; stored at `sdmc:/config/ultragb/saves/`
- **Quick-resume state** — full CPU/PPU/APU snapshot saved automatically when the overlay closes; restored on next launch; stored at `sdmc:/config/ultragb/states/internal/`
- **User save states** — 10 manual slots per game; stored at `sdmc:/config/ultragb/states/`
- **SRAM backup slots** — 10 manual SRAM backup slots per game, independent of save states

---

## Per-Game Configuration

Press **Y** on any ROM in the selector to open its config screen.

| Item | Description |
|---|---|
| Save States | 10 named slots — save, load, or delete any slot |
| Save Data | 10 SRAM backup slots — manual snapshots of battery save data |
| Reset | Cold boot the game (deletes the quick-resume state) |
| Pallet Mode | Cycle GBC → DMG → Native; applied live without restarting |
| LCD Ghosting | Enable 50/50 frame blending for accurate flicker reproduction (memory-gated — see below) |

---

## Settings

| Item | Description |
|---|---|
| Volume | Slider 0–100; tap the speaker icon or press Y (when slider is focused) to mute/unmute |
| Windowed Mode | Toggle windowed mode on/off; takes effect on next ROM launch |
| Windowed Scale | Cycle 1× → 2× → 3× → 4× → 5×; capped by available heap (see below) |
| Overlay Scale | Toggle 2.5× (default) ↔ 2× pixel-perfect |
| LCD Grid | Enable/disable the LCD inter-pixel gap simulation |
| Quick Combo | Assign a button combo to launch directly to the last played ROM from anywhere |
| In-Game Haptics | Enable/disable rumble feedback during gameplay |
| In-Game Wallpaper | Show the Ultrahand wallpaper behind the game screen (requires 8 MB+ heap) |

### Quick Combo / Quick Launch
Assigning a combo in Settings registers it system-wide and deconflicts it from other overlays and packages automatically. When triggered, the overlay launches straight into the last played ROM, bypassing the selector. The launch combo then closes the overlay entirely rather than returning to the menu.

---

## Memory Tiers

The Switch overlay heap is capped per system configuration. UltraGB adapts automatically.

| Heap | Windowed max scale | In-game wallpaper | LCD Ghosting |
|---|---|---|---|
| 4 MB | 3× | — | — |
| 6 MB | 4× | — | ROMs < 2 MB only |
| 8 MB | 5× | ✓ | ROMs < 4 MB only |
| 10 MB+ | 5× | ✓ | All ROMs |

ROMs that exceed the current tier's playable size are shown in the selector with a warning colour and cannot be launched.

---

## Requirements

- Nintendo Switch with [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) custom firmware
- [Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay) and [nx-ovlloader v2.0.0](https://github.com/ppkantorski/nx-ovlloader) installed
- ROMs in `.gb` or `.gbc` format placed anywhere accessible on the SD card

---

## Installation

1. Download the latest `gbemu.ovl` from [Releases](../../releases)
2. Copy it to `sdmc:/switch/.overlays/`
3. Launch via Ultrahand

The ROM directory defaults to `sdmc:/roms/gb/` and can be changed in `sdmc:/config/ultragb/config.ini` (`rom_dir` key).

---

## Building

**Requirements:** [devkitPro](https://devkitpro.org) with `devkitARM`, `libnx`, and the libultrahand library.

```sh
export DEVKITPRO=/opt/devkitpro
make -j6
```

Output: `gbemu.ovl`

The build targets C++26, ARMv8-A with SIMD/CRC/crypto extensions tuned for Cortex-A57, full LTO with 6 parallel LTRANS jobs, and links against `libcurl`, `mbedtls`, and `libnx`.

---

## File Layout

```
sdmc:/
├── switch/
│   └── .overlays/
│       └── gbemu.ovl
└── config/
    └── ultragb/
        ├── config.ini              ← global settings (rom_dir, volume, scale, etc.)
        ├── saves/                  ← SRAM battery saves
        ├── states/
        │   ├── internal/           ← quick-resume state (one per game, auto-managed)
        │   └── <game>/             ← user save-state slots
        └── configure/
            └── <game>.ini          ← per-game settings (palette, ghosting)
```

---

## Credits

- **[ppkantorski](https://github.com/ppkantorski)** — UltraGB Overlay, Ultrahand Overlay, libultrahand
- **[Mr-PauI](https://github.com/Mr-PauI)** — [Walnut-CGB](https://github.com/Mr-PauI/Walnut-CGB) (GBC core, CGB palette system, dual-fetch optimisations)
- **[deltabeard](https://github.com/deltabeard)** — [Peanut-GB](https://github.com/deltabeard/Peanut-GB) (original GB core)
- **[LIJI32](https://github.com/LIJI32)** — [SameBoy](https://github.com/LIJI32/SameBoy) (portions used for reference in Walnut-CGB)

---

## Contributing

Contributions are welcome! If you have any ideas, suggestions, or bug reports, please raise an [issue](https://github.com/ppkantorski/UltraGB-Overlay/issues/new/choose), submit a [pull request](https://github.com/ppkantorski/UltraGB-Overlay/compare) or reach out to me directly on [GBATemp](https://gbatemp.net/threads/ultragb-overlay/).

[![ko-fi](https://ko-fi.com/img/githubbutton_sm.svg)](https://ko-fi.com/X8X3VR194)

---

## License

UltraGB Overlay is released under the [GPLv2](LICENSE).
