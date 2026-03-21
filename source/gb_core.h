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
// These MUST be defined here, before peanut_gb.h is included, because the
// header uses #if (not #ifdef) to gate entire struct members such as gb_s::cgb.
// Relying solely on -D flags in the Makefile is not sufficient when the header
// is pulled in through an extern "C" block in a precompiled-header context.
// All are guarded with #ifndef so Makefile -D flags are respected without warning.
#ifndef ENABLE_LCD
#  define ENABLE_LCD                  1
#endif
#ifndef ENABLE_SOUND
#  define ENABLE_SOUND                1  // enables audio_write()/audio_read() hooks
#endif
#ifndef ENABLE_GBC_SUPPORT
#  define ENABLE_GBC_SUPPORT          1
#endif
#ifndef PEANUT_GB_HIGH_LCD_ACCURACY
#  define PEANUT_GB_HIGH_LCD_ACCURACY 0
#endif
#ifndef GB_INTERNAL
#  define GB_INTERNAL
#endif

// Forward-declare the APU hooks that peanut_gb.h calls when ENABLE_SOUND=1.
// These must be visible as C symbols before peanut_gb.h is parsed, because
// peanut_gb.h calls them directly inside __gb_read/__gb_write with no prior
// declaration of its own.  The definitions live in gb_audio.h.
extern "C" {
    uint8_t audio_read(uint8_t addr);
    void    audio_write(uint8_t addr, uint8_t val);
}

// Include Peanut-GB as C inside C++ — required to avoid name-mangling issues.
extern "C" {
#include "peanut_gb.h"
}

// ── Screen and viewport dimensions ───────────────────────────────────────────
static constexpr int GB_W          = LCD_WIDTH;    // 160
static constexpr int GB_H          = LCD_HEIGHT;   // 144

static constexpr int VP_X          = 24;
static constexpr int VP_Y          = (720 - 360) / 2;  // 180
static constexpr int VP_W          = 400;
static constexpr int VP_H          = 360;

// Overlay framebuffer dimensions (448 × 720, RGBA4444)
static constexpr int FB_W          = 448;
static constexpr int FB_H          = 720;

// ── TO ENABLE FULL GBC COLOUR ────────────────────────────────────────────────
// GBC support requires knowing two things that differ between Peanut-GB forks:
//   1. The macro that enables the `cgb` member inside `struct gb_s`
//   2. The exact field names for the BG and sprite palette arrays
//
// Run this from your project root to find them:
//   grep -n "GBC\|cgb\|palette" lib/Peanut-GB/peanut_gb.h | head -80
//
// Once you have them, define ULTRABOY_GBC and fill in the three items below,
// then add -DULTRABOY_GBC to CFLAGS in the Makefile.
//
// Example (deltabeard mainline, ~2024):
//   In gb_core.h, add before #include "peanut_gb.h":
//     #define ENABLE_GBC_SUPPORT 1   (or whatever your peanut_gb.h checks)
//   Then set:
//     #define ULTRABOY_GBC
//     #define PGB_CGB_BG_PAL(gb)  (gb)->cgb.cgbp    // uint8_t[64]
//     #define PGB_CGB_SP_PAL(gb)  (gb)->cgb.cgbsp   // uint8_t[64]
//     #define PGB_CGB_MODE(gb)    (gb)->cgb.mode     // nonzero = GBC ROM
//
#if defined(ULTRABOY_GBC)
#  ifndef PGB_CGB_BG_PAL
#    error "Define PGB_CGB_BG_PAL, PGB_CGB_SP_PAL, PGB_CGB_MODE — see gb_core.h"
#  endif
#  define CGB_BG_PAL_BYTE(gb, pal, col, byte) \
       (PGB_CGB_BG_PAL(gb)[((pal)*4+(col))*2+(byte)])
#  define CGB_SP_PAL_BYTE(gb, pal, col, byte) \
       (PGB_CGB_SP_PAL(gb)[((pal)*4+(col))*2+(byte)])
#  define CGB_BG_COLOR(gb, pal, col) \
       ((uint16_t)CGB_BG_PAL_BYTE(gb,pal,col,0) | \
        ((uint16_t)CGB_BG_PAL_BYTE(gb,pal,col,1)<<8))
#  define CGB_SP_COLOR(gb, pal, col) \
       ((uint16_t)CGB_SP_PAL_BYTE(gb,pal,col,0) | \
        ((uint16_t)CGB_SP_PAL_BYTE(gb,pal,col,1)<<8))
#endif  // ULTRABOY_GBC

// ── DMG greyscale palette (RGB555) ───────────────────────────────────────────
static constexpr uint16_t DMG_GREY[4] = {
    0x7FFF,  // 0 – white
    0x5294,  // 1 – light grey
    0x294A,  // 2 – dark grey
    0x0000,  // 3 – black
};

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
// Called 144 times per gb_run_frame(), once per horizontal scanline.
// Writes one row into g_gb_fb in RGB555 format.
static void gb_lcd_draw_line(struct gb_s* gb,
                              const uint8_t* pixels,
                              const uint_fast8_t line)
{
    (void)gb;  // unused until ULTRABOY_GBC is enabled
    if ((int)line >= GB_H) return;
    uint16_t* row = g_gb_fb + (int)line * GB_W;

#if defined(ULTRABOY_GBC)
    // Full GBC colour path — only active when ULTRABOY_GBC is defined in the
    // Makefile AND PGB_CGB_* macros are set correctly for your peanut_gb.h.
    if (PGB_CGB_MODE(gb)) {
        // Peanut-GB pixel byte:
        //   bits [1:0] = colour index within palette (0–3)
        //   bits [4:2] = palette number (0–7)
        //   bit  [6]   = sprite pixel (use sprite palette bank)
        for (int x = 0; x < GB_W; x++) {
            const uint8_t pix      = pixels[x];
            const uint8_t col      = pix & 0x03;
            const uint8_t pal      = (pix >> 2) & 0x07;
            const bool    isSprite = (pix >> 6) & 0x01;
            row[x] = isSprite ? CGB_SP_COLOR(gb, pal, col)
                               : CGB_BG_COLOR(gb, pal, col);
        }
        return;
    }
#endif

    // DMG greyscale: map 2-bit palette indices → RGB555
    for (int x = 0; x < GB_W; x++)
        row[x] = DMG_GREY[pixels[x] & 3];
}

// ── Public interface (implemented in main.cpp) ────────────────────────────────

// Load a ROM file, allocate cart RAM, and reset the core.
// Returns true on success.
bool gb_load_rom(const char* romPath);

// Save cart RAM to file and free all emulator resources.
void gb_unload_rom();

// Run exactly one Game Boy frame (calls Peanut-GB's gb_run_frame()).
// Must only be called when g_gb.running == true.
inline void gb_run_one_frame() {
    if (g_gb.running)
        gb_run_frame(&g_gb.gb);
}

// Update the joypad from the overlay's keysHeld bitmask.
// Uses Peanut-GB's active-low convention: 0 = pressed.
void gb_set_input(uint64_t keysHeld);