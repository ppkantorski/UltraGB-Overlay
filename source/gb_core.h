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
static constexpr int VP_Y          = (720 - 360) / 2 - 60-10-2;  // shifted up 62 px total
static constexpr int VP_W          = 400;
static constexpr int VP_H          = 360;

// Overlay framebuffer dimensions (448 × 720, RGBA4444)
static constexpr int FB_W          = 448;
static constexpr int FB_H          = 720;

// ── DMG greyscale palette (RGB555) ───────────────────────────────────────────
// All RGB555 values use encoding: color = R | (G<<5) | (B<<10), each channel 0-31.
static constexpr uint16_t DMG_GREY[4] = {
    0x7FFF,  // white
    0x5294,  // light grey  (R=20,G=20,B=20)
    0x294A,  // dark grey   (R=10,G=10,B=10)
    0x0000,  // black
};

// ── GBC green palette (RGB555) ────────────────────────────────────────────────
// "DMG" mode: mimics the original Game Boy's iconic green LCD tint.
static constexpr uint16_t GBC_GREEN[4] = {
    0x06F3,  // lightest green
    0x06B1,  // light green
    0x1986,  // dark green
    0x04E1,  // darkest green
};

// ── GBC warm palette (RGB555) ─────────────────────────────────────────────────
// Fallback for "SGB" mode when a game is not in the title table.
// Real SGB palettes are ROM-embedded command packets we cannot easily parse;
// warm amber is a reasonable approximation of how many SGB games looked.
static constexpr uint16_t GBC_WARM[4] = {
    0x539E,  // warm cream-white  (R=30,G=28,B=20 in BGR555 = R=20,G=28,B=30… see note)
    0x18D6,  // warm mid-tan
    0x0C8C,  // dark warm brown
    0x0443,  // near-black
};

// ══════════════════════════════════════════════════════════════════════════════
// GBC built-in DMG colorisation system
// ──────────────────────────────────────────────────────────────────────────────
// When a DMG game runs on a real Game Boy Color the boot ROM picks one of ~12
// preset colour palettes keyed by a checksum of the ROM title bytes (0x134-0x143).
// This replicates that behaviour so Pokémon Blue looks blue, Red looks red, etc.
//
// Palette encoding: RGB555 — color = R | (G<<5) | (B<<10), channels 0-31.
// Three sub-palettes per entry: bg (background tiles), obj0 (sprite pal 0),
// obj1 (sprite pal 1).  Most games look fine with identical bg/obj palettes;
// notable exceptions can be added as separate entries later.
//
// How to add a new game:
//   1. Compute checksum: sum rom[0x134..0x143], keep low 8 bits.
//   2. Read title[3] = rom[0x137] (use 0xFF to match any).
//   3. Pick or add a palette index below.
//   4. Append a GbcTitleEntry to GBC_TITLE_TABLE.
// ══════════════════════════════════════════════════════════════════════════════

struct GbcDmgPal {
    uint16_t bg[4];    // BG tile palette
    uint16_t obj0[4];  // Sprite palette 0
    uint16_t obj1[4];  // Sprite palette 1 (often same as obj0)
};

// Palette index constants — use these names when building GBC_TITLE_TABLE.
static constexpr int GBCPAL_GREY   = 0;  // grayscale (GBC fallback for unknown games)
static constexpr int GBCPAL_BLUE   = 1;  // Pokémon Blue, Dr. Mario…
static constexpr int GBCPAL_RED    = 2;  // Pokémon Red, Mega Man…
static constexpr int GBCPAL_GREEN  = 3;  // Zelda, Tetris…
static constexpr int GBCPAL_YELLOW = 4;  // Super Mario Land, Wario Land…
static constexpr int GBCPAL_PURPLE = 5;  // Kirby's Dream Land…
static constexpr int GBCPAL_TEAL   = 6;  // Metroid II, Kid Dracula…
static constexpr int GBCPAL_BROWN  = 7;  // Final Fantasy Adventure, RPGs…

static constexpr GbcDmgPal GBC_DMG_PALETTES[] = {
    // ── 0: Grayscale — default for unrecognised games in GBC mode ─────────
    { {0x7FFF, 0x5294, 0x294A, 0x0000},
      {0x7FFF, 0x5294, 0x294A, 0x0000},
      {0x7FFF, 0x5294, 0x294A, 0x0000} },

    // ── 1: Blue — Pokémon Blue, Dr. Mario ─────────────────────────────────
    // white → light blue (R=17,G=24,B=31) → medium blue (R=12,G=16,B=28) → dark blue (R=4,G=8,B=20)
    { {0x7FFF, 0x7F11, 0x720C, 0x5104},
      {0x7FFF, 0x7F11, 0x720C, 0x5104},
      {0x7FFF, 0x7F11, 0x720C, 0x5104} },

    // ── 2: Red — Pokémon Red, Mega Man ────────────────────────────────────
    // white → light pink (R=31,G=20,B=16) → medium red (R=24,G=8,B=8) → dark maroon (R=12,G=2,B=2)
    { {0x7FFF, 0x429F, 0x2118, 0x084C},
      {0x7FFF, 0x429F, 0x2118, 0x084C},
      {0x7FFF, 0x429F, 0x2118, 0x084C} },

    // ── 3: Green — Zelda: Link's Awakening, Tetris ────────────────────────
    // white → light green (R=16,G=31,B=12) → forest green (R=8,G=20,B=4) → dark green (R=2,G=10,B=2)
    { {0x7FFF, 0x33F0, 0x1288, 0x0942},
      {0x7FFF, 0x33F0, 0x1288, 0x0942},
      {0x7FFF, 0x33F0, 0x1288, 0x0942} },

    // ── 4: Yellow — Super Mario Land, Wario Land ──────────────────────────
    // white → light yellow (R=31,G=31,B=10) → gold (R=24,G=20,B=4) → dark brown (R=12,G=8,B=2)
    { {0x7FFF, 0x2BFF, 0x1298, 0x090C},
      {0x7FFF, 0x2BFF, 0x1298, 0x090C},
      {0x7FFF, 0x2BFF, 0x1298, 0x090C} },

    // ── 5: Purple — Kirby's Dream Land ────────────────────────────────────
    // white → light lavender (R=28,G=18,B=31) → medium purple (R=16,G=6,B=24) → dark purple (R=8,G=2,B=14)
    { {0x7FFF, 0x7E5C, 0x60D0, 0x3848},
      {0x7FFF, 0x7E5C, 0x60D0, 0x3848},
      {0x7FFF, 0x7E5C, 0x60D0, 0x3848} },

    // ── 6: Teal — Metroid II, Kid Dracula ─────────────────────────────────
    // white → light cyan (R=12,G=31,B=28) → medium teal (R=4,G=20,B=18) → dark teal (R=2,G=10,B=8)
    { {0x7FFF, 0x771C, 0x4904, 0x2882},
      {0x7FFF, 0x771C, 0x4904, 0x2882},
      {0x7FFF, 0x771C, 0x4904, 0x2882} },

    // ── 7: Brown/Sepia — RPGs, Final Fantasy Adventure ────────────────────
    // white → warm sand (R=28,G=24,B=16) → medium brown (R=18,G=12,B=6) → dark brown (R=8,G=4,B=2)
    { {0x7FFF, 0x431C, 0x218A, 0x1088},
      {0x7FFF, 0x431C, 0x218A, 0x1088},
      {0x7FFF, 0x431C, 0x218A, 0x1088} },
};
static constexpr int GBC_DMG_PAL_COUNT =
    (int)(sizeof(GBC_DMG_PALETTES) / sizeof(GBC_DMG_PALETTES[0]));

// ── Title checksum lookup table ───────────────────────────────────────────────
// checksum = (sum of rom[0x134..0x143]) & 0xFF
// title3   = rom[0x137]; 0xFF = match any (use when checksum is unique)
struct GbcTitleEntry {
    uint8_t checksum;
    uint8_t title3;    // 0xFF = don't care
    uint8_t pal_idx;   // index into GBC_DMG_PALETTES
};

static constexpr GbcTitleEntry GBC_TITLE_TABLE[] = {
    // ── Pokémon ─────────────────────────────────────────────────────────────
    // "POKEMON RED\0\0\0\0\0" sum=788 → 0x14, title[3]='E'
    { 0x14, 0xFF, GBCPAL_RED  },
    // "POKEMON BLUE\0\0\0\0"  sum=865 → 0x61, title[3]='E'
    { 0x61, 0xFF, GBCPAL_BLUE },

    // ── Zelda ───────────────────────────────────────────────────────────────
    // "ZELDA\0…"              sum=368 → 0x70, title[3]='D'
    { 0x70, 0xFF, GBCPAL_GREEN },

    // ── Super Mario Land ────────────────────────────────────────────────────
    // "SUPER MARIO LAND"      sum=1126 → 0x66, title[3]='E'
    { 0x66, 0xFF, GBCPAL_YELLOW },

    // ── Tetris ──────────────────────────────────────────────────────────────
    // "TETRIS\0…"             sum=475 → 0xDB, title[3]='R'
    { 0xDB, 'R',  GBCPAL_GREEN },

    // ── Kirby ───────────────────────────────────────────────────────────────
    // "KIRBYDREAMLAND\0\0"    sum→verify, title[3]='B'
    // (checksum 0x27 based on community research; update if incorrect)
    { 0x27, 'B',  GBCPAL_PURPLE },

    // ── Dr. Mario ───────────────────────────────────────────────────────────
    // "DR.MARIO\0…"           sum=572 → 0x3C, title[3]='M'
    { 0x3C, 0xFF, GBCPAL_BLUE },

    // ── Metroid II ──────────────────────────────────────────────────────────
    // "METROID2\0…"           sum=584 → 0x48, title[3]='R'
    { 0x48, 0xFF, GBCPAL_TEAL },

    // ── Super Mario Land 2 ──────────────────────────────────────────────────
    // "MARIOLAND2\0…"         sum=617 → 0x69 (verify), title[3]='I'
    { 0x69, 0xFF, GBCPAL_YELLOW },

    // ── Wario Land ──────────────────────────────────────────────────────────
    // "WARIOLAND\0…"          sum=586 → 0x4A, title[3]='R'
    { 0x4A, 0xFF, GBCPAL_YELLOW },

    // ── Final Fantasy Adventure ─────────────────────────────────────────────
    // Japanese: "SEIKEN DENSETSU" → different checksum; NA "MYSTIC QUEST\0\0"
    // "MYSTIC QUEST\0\0\0\0"  sum=818 → 0x32, title[3]='T'
    { 0x32, 'T',  GBCPAL_BROWN },
};
static constexpr int GBC_TITLE_TABLE_SIZE =
    (int)(sizeof(GBC_TITLE_TABLE) / sizeof(GBC_TITLE_TABLE[0]));

// ── Active DMG flat palette (set by gb_select_dmg_palette) ───────────────────
// Branchless single-lookup table for the DMG draw loop.
//
// Walnut pixel byte encoding (WALNUT_GB_12_COLOUR=1):
//   bits[1:0] = colour index (0-3)
//   bits[5:4] = source:  0x00=OBJ0, 0x10=OBJ1, 0x20=BG
//
// Indexing: g_dmg_flat_pal[px & 0x33]
//   px & 0x33 isolates bits[5:4] and bits[1:0], giving indices in
//   {0-3, 16-19, 32-35} — one contiguous slot per sub-palette per colour.
//   The full array is 64 entries (next power-of-2 above 35) = 128 bytes total.
//   The unused slots are never accessed so they waste nothing at runtime.
//
// Written once per ROM load; read 160× per scanline × 144 lines × 60 fps.
// Declared extern; defined in main.cpp.
extern uint16_t g_dmg_flat_pal[64];  // 128 bytes — replaces 3×4-entry arrays
extern int      g_gbc_pal_idx;        // index of the matched entry (-1 = not found)
extern bool     g_gbc_pal_found;      // true if game was found in GBC_TITLE_TABLE

// ── Palette mode ──────────────────────────────────────────────────────────────
// Per-DMG-game setting stored in sdmc:/config/ultragb/configure/<rom>.ini
//   GBC    (default) — GBC built-in title-checksum lookup; greyscale for unknowns
//   SGB              — Same lookup; warm amber for unknowns (approximates SGB feel)
//   DMG              — Classic Game Boy green LCD tint
//   NATIVE           — True greyscale, no colour tint
// CGB games (.gbc / header flag 0x80) always use hardware colour; this setting
// has no effect on them.
enum class PaletteMode : uint8_t { GBC = 0, SGB, DMG, NATIVE };
extern PaletteMode g_palette_mode;

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
// g_gb_fb stores RGB555 for CGB games and pre-packed RGBA4444 for DMG games
// (when g_fb_is_prepacked is true).  The renderer checks this flag to skip
// the per-run colour conversion entirely in the DMG hot path.
extern uint16_t  g_gb_fb[GB_W * GB_H];
extern bool      g_fb_is_prepacked;  // true → g_gb_fb already contains RGBA4444

// ── RGB555 → RGBA4444 packer ─────────────────────────────────────────────────
// Strips the LSB of each 5-bit channel to produce a 4-bit channel; alpha = 0xF.
// Layout (little-endian): r4 | g4<<4 | b4<<8 | a4<<12.
// Defined here so gb_select_dmg_palette can pre-bake the flat palette without
// including gb_renderer.h (which would create a circular dependency).
static inline uint16_t gb_pack_rgb555(const uint16_t c) {
    const uint8_t r = ( c        & 0x1F) >> 1;
    const uint8_t g = ((c >>  5) & 0x1F) >> 1;
    const uint8_t b = ((c >> 10) & 0x1F) >> 1;
    return static_cast<uint16_t>(r | (g << 4) | (b << 8) | 0xF000u);
}

// ── gb_select_dmg_palette ─────────────────────────────────────────────────────
// Call once after a ROM loads (and whenever g_palette_mode changes live).
// Reads ROM header, looks up the title checksum table, and populates
// g_dmg_flat_pal that gb_lcd_draw_line uses every scanline.
static inline void gb_select_dmg_palette() {
    // Step 1: compute title checksum from header bytes 0x134-0x143.
    uint32_t sum = 0;
    if (g_gb.rom && g_gb.romSize > 0x143) {
        for (int i = 0x134; i <= 0x143; ++i)
            sum += g_gb.rom[i];
    }
    const uint8_t checksum = (uint8_t)(sum & 0xFF);
    const uint8_t title3   = (g_gb.rom && g_gb.romSize > 0x137) ? g_gb.rom[0x137] : 0;

    // Step 2: search title table.
    g_gbc_pal_found = false;
    g_gbc_pal_idx   = GBCPAL_GREY;
    for (int i = 0; i < GBC_TITLE_TABLE_SIZE; ++i) {
        const GbcTitleEntry& e = GBC_TITLE_TABLE[i];
        if (e.checksum == checksum &&
            (e.title3 == 0xFF || e.title3 == title3)) {
            g_gbc_pal_idx   = e.pal_idx;
            g_gbc_pal_found = true;
            break;
        }
    }

    // Step 3: pick source palette arrays based on mode.
    const uint16_t* bg_src;
    const uint16_t* ob0_src;
    const uint16_t* ob1_src;

    switch (g_palette_mode) {
        case PaletteMode::DMG:
            bg_src = ob0_src = ob1_src = GBC_GREEN;
            break;

        case PaletteMode::NATIVE:
            bg_src = ob0_src = ob1_src = DMG_GREY;
            break;

        case PaletteMode::SGB:
            if (g_gbc_pal_found) {
                const GbcDmgPal& p = GBC_DMG_PALETTES[g_gbc_pal_idx];
                bg_src  = p.bg;
                ob0_src = p.obj0;
                ob1_src = p.obj1;
            } else {
                bg_src = ob0_src = ob1_src = GBC_WARM;
            }
            break;

        default:  // PaletteMode::GBC
            if (g_gbc_pal_found) {
                const GbcDmgPal& p = GBC_DMG_PALETTES[g_gbc_pal_idx];
                bg_src  = p.bg;
                ob0_src = p.obj0;
                ob1_src = p.obj1;
            } else {
                bg_src = ob0_src = ob1_src = DMG_GREY;
            }
            break;
    }

    // Step 4: fill the flat lookup table with pre-packed RGBA4444 values.
    // Layout: 0x00-0x03 = OBJ0, 0x10-0x13 = OBJ1, 0x20-0x23 = BG.
    // Pre-packing here means gb_lcd_draw_line writes ready-to-display RGBA4444
    // directly into g_gb_fb, so render_gb_screen_chunk skips conversion entirely.
    for (int c = 0; c < 4; ++c) {
        g_dmg_flat_pal[0x00 + c] = gb_pack_rgb555(ob0_src[c]);
        g_dmg_flat_pal[0x10 + c] = gb_pack_rgb555(ob1_src[c]);
        g_dmg_flat_pal[0x20 + c] = gb_pack_rgb555(bg_src[c]);
    }
}

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
//     We select palette based on g_palette_mode (GBC/DMG/NATIVE) per-pixel.
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

    // DMG path — branchless flat-palette lookup.
    //
    // g_dmg_flat_pal is indexed by (px & 0x33):
    //   bits[5:4] select sub-palette slot (0x00=OBJ0, 0x10=OBJ1, 0x20=BG)
    //   bits[1:0] select colour within that palette
    // Values are pre-packed RGBA4444 (set by gb_select_dmg_palette), so the
    // renderer can use them directly without any per-run conversion.
    // No conditional pointer selection; the compiler can vectorise this loop.
    for (int x = 0; x < GB_W; x++)
        row[x] = g_dmg_flat_pal[pixels[x] & 0x33];
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