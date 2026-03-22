/********************************************************************************
 * File: gb_core.h
 * Description:
 *   Peanut-GB integration layer. Defines the global emulator state, all
 *   required Peanut-GB callbacks (rom read, cart RAM, LCD draw-line), and
 *   the public interface used by main.cpp.
 *
 *   Peanut-GB is a single-file C header-only GB/GBC core by deltabeard.
 *   GitHub: https://github.com/deltabeard/Peanut-GB
 *   Place peanut_gb.h at: source/peanut_gb.h
 *
 * IMPORTANT – CGB palette field names:
 *   Different releases of Peanut-GB use slightly different struct member names
 *   for the CGB palette arrays. If you get compile errors on gb->cgb.cgbp or
 *   gb->cgb.cgbsp, check peanut_gb.h and adjust the two macros:
 *     CGB_BG_PAL_BYTE(gb, pal, col, byte)
 *     CGB_SP_PAL_BYTE(gb, pal, col, byte)
 *   See comments near those macros below.
 ********************************************************************************/

#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>

// ── Peanut-GB compile-time options ───────────────────────────────────────────
#ifndef ENABLE_LCD
#  define ENABLE_LCD                  1
#endif
#ifndef ENABLE_SOUND
#  define ENABLE_SOUND                1
#endif
#ifndef ENABLE_GBC_SUPPORT
#  define ENABLE_GBC_SUPPORT          1
#endif
#ifndef WALNUT_FULL_GBC_SUPPORT
#  define WALNUT_FULL_GBC_SUPPORT     1  // enables gb_s::cgb — full GBC colour
#endif
#ifndef PEANUT_GB_HIGH_LCD_ACCURACY
#  define PEANUT_GB_HIGH_LCD_ACCURACY 0
#endif
#ifndef GB_INTERNAL
#  define GB_INTERNAL
#endif
// Safe dual-fetch flags (MBC/DMA/opcodes) are set to 1 directly in
// walnut_cgb.h — they are unconditional #defines so #ifndef guards here
// have no effect.  Edit walnut_cgb.h lines 62-64 to change them.

extern "C" {
    uint8_t audio_read(uint8_t addr);
    void    audio_write(uint8_t addr, uint8_t val);
}

extern "C" {
#include "walnut_cgb.h"
}

// ── Screen and viewport dimensions ───────────────────────────────────────────
static constexpr int GB_W          = LCD_WIDTH;    // 160
static constexpr int GB_H          = LCD_HEIGHT;   // 144

static constexpr int VP_X          = 24;
static constexpr int VP_Y          = (720 - 360) / 2 - 60-10;  // 120 — shifted up 60 px total to make room for virtual controls
static constexpr int VP_W          = 400;
static constexpr int VP_H          = 360;

// Overlay framebuffer dimensions (448 × 720, RGBA4444)
static constexpr int FB_W          = 448;
static constexpr int FB_H          = 720;

// ── DMG greyscale palette (RGB555) ───────────────────────────────────────────
static constexpr uint16_t DMG_GREY[4] = {
    0x7FFF,  // 0 – white
    0x5294,  // 1 – light grey
    0x294A,  // 2 – dark grey
    0x0000,  // 3 – black
};

// ── GBC green palette (RGB555) ────────────────────────────────────────────────
static constexpr uint16_t GBC_GREEN[4] = {
    0x06F3,  // 0 – lightest green
    0x06B1,  // 1 – light green
    0x1986,  // 2 – dark green
    0x04E1,  // 3 – darkest green
};

// Palette selection — defined in main.cpp, read from config.ini [config].
// true  = classic DMG greyscale
// false = GBC-style green tint (for DMG games running without CGB hardware)
extern bool g_original_palette;

// ── Global emulator state ─────────────────────────────────────────────────────
struct GBState {
    struct gb_s gb;          // Peanut-GB core state (~8 KB, on BSS)

    uint8_t*    rom       = nullptr;
    size_t      romSize   = 0;

    uint8_t*    cartRam   = nullptr;
    size_t      cartRamSz = 0;

    bool        running   = false;
    char        romPath[512] = {};  // path of the currently loaded ROM
    char        savePath[512] = {}; // derived save path
};

// Declared in main.cpp
extern GBState   g_gb;
extern uint16_t  g_gb_fb[GB_W * GB_H];  // RGB555 per pixel

// ── Peanut-GB C callbacks (static so they have C linkage via the extern "C" block) ─
// Defined in main.cpp where the global state lives.

static uint8_t gb_rom_read(struct gb_s*, const uint_fast32_t addr) {
    if (addr >= g_gb.romSize) return 0xFF;
    return g_gb.rom[addr];
}

// Walnut-CGB requires 16-bit and 32-bit ROM read functions for its dual-fetch
// CPU execution model.  These are simple unaligned little-endian reads —
// safe because g_gb.rom is a heap buffer with no alignment restrictions.
static uint16_t gb_rom_read16(struct gb_s*, const uint_fast32_t addr) {
    if (addr + 1 >= g_gb.romSize) return 0xFFFF;
    return (uint16_t)g_gb.rom[addr] | ((uint16_t)g_gb.rom[addr + 1] << 8);
}

static uint32_t gb_rom_read32(struct gb_s*, const uint_fast32_t addr) {
    if (addr + 3 >= g_gb.romSize) return 0xFFFFFFFF;
    return (uint32_t)g_gb.rom[addr]
         | ((uint32_t)g_gb.rom[addr + 1] << 8)
         | ((uint32_t)g_gb.rom[addr + 2] << 16)
         | ((uint32_t)g_gb.rom[addr + 3] << 24);
}

static uint8_t gb_cart_ram_read(struct gb_s*, const uint_fast32_t addr) {
    if (!g_gb.cartRam || addr >= g_gb.cartRamSz) return 0xFF;
    return g_gb.cartRam[addr];
}

static void gb_cart_ram_write(struct gb_s*, const uint_fast32_t addr, const uint8_t val) {
    if (!g_gb.cartRam || addr >= g_gb.cartRamSz) return;
    g_gb.cartRam[addr] = val;
}

static void gb_error(struct gb_s*, const enum gb_error_e, const uint16_t) {
    // Silently swallow emulation errors; the game will just glitch rather than crash.
}

// ── LCD draw-line callback ────────────────────────────────────────────────────
// Walnut-CGB pixel byte encoding:
//
//   CGB mode (gb->cgb.cgbMode != 0):
//     Pixel byte is a direct index into gb->cgb.fixPalette[]:
//       [0x00..0x1F] = BG palettes  (8 palettes × 4 colours, RGB565 LE)
//       [0x20..0x3F] = OBJ palettes (8 palettes × 4 colours, RGB565 LE)
//     Just use fixPalette[pixel] directly — Walnut pre-converts BGR555→RGB565.
//
//   DMG mode (non-CGB ROM):
//     WALNUT_GB_12_COLOUR=1: bits[1:0]=colour index, bits[5:4]=source
//       (LCD_PALETTE_BG=0x20=BG, else sprite).  We only need bits[1:0].
//     We apply g_original_palette ? DMG_GREY : GBC_GREEN per-pixel.
static void gb_lcd_draw_line(struct gb_s* gb,
                              const uint8_t* pixels,
                              const uint_fast8_t line)
{
    if ((int)line >= GB_H) return;
    uint16_t* row = g_gb_fb + (int)line * GB_W;

#if WALNUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode) {
        // CGB path: fixPalette is pre-built RGB565 — one lookup per pixel.
        for (int x = 0; x < GB_W; x++)
            row[x] = gb->cgb.fixPalette[pixels[x]];
        return;
    }
#endif

    // DMG path: map 2-bit colour index through selected palette (RGB555).
    const uint16_t* pal = g_original_palette ? DMG_GREY : GBC_GREEN;
    for (int x = 0; x < GB_W; x++)
        row[x] = pal[pixels[x] & 3];
}

// ── Public interface (implemented in main.cpp) ────────────────────────────────

// Load a ROM file, allocate cart RAM, and reset the core.
// Returns true on success.
bool gb_load_rom(const char* romPath);

// Save cart RAM to file and free all emulator resources.
void gb_unload_rom();

// Run exactly one Game Boy frame using Walnut-CGB's dual-fetch execution path.
// The three fixes applied to walnut_cgb.h that make Oracle of Seasons (and other
// CGB games) work correctly:
//
//   1. LCD-off VBlank interrupt (the main freeze fix):
//      Real hardware fires VBLANK_INTR even when the LCD is disabled. Walnut was
//      not setting IO_IF |= VBLANK_INTR in the lcd_off_count path, so any game
//      that turns off the LCD during a scene transition and then HALTs waiting
//      for VBlank would spin in the do-while forever (IO_IF & IO_IE stayed 0).
//
//   2. HALT lcd_cycles double-speed scaling:
//      The HALT handler computed lcd_cycles in display-clock ticks but used the
//      value raw as inst_cycles, which is then halved by >>doubleSpeed. In
//      double-speed mode this causes a Zeno-paradox convergence to lcd_count=455
//      where 1>>1=0 stops all progress. Fix: scale lcd_cycles << doubleSpeed.
//
//   3. inst_cycles>1 guard removal:
//      The lcd_count += path guarded the >>doubleSpeed shift with inst_cycles>1,
//      skipping it for 1-cycle instructions and making lcd_count advance too fast.
//
// Must only be called when g_gb.running == true.
inline void gb_run_one_frame() {
    if (g_gb.running) {
        gb_run_frame_dualfetch(&g_gb.gb);
        //gb_run_frame(&g_gb.gb);
    }
}

// Update the joypad from the overlay's keysHeld bitmask.
// Uses Peanut-GB's active-low convention: 0 = pressed.
void gb_set_input(uint64_t keysHeld);