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
// GBC / SGB title-based DMG colorisation
// ──────────────────────────────────────────────────────────────────────────────
// Both GBC and SGB modes use the same per-title palette database.
//
//   GBC mode: colorisation applied only to Nintendo-licensed games
//             (rom[0x014B]==0x01, or ==0x33 with new licensee code "01").
//             This matches what the real GBC boot ROM does.
//
//   SGB mode: licensee gate is skipped; all games in the database are
//             colorised.  A real SGB2 reads in-game PAL command packets
//             that we do not emulate; the GBC hardware palette database is
//             the best static approximation available, and is accurate for
//             the majority of games.
//
//   DMG / NATIVE modes: this system is bypassed entirely (GBC_GREEN /
//                        DMG_GREY are used respectively).
//
// Palette data is derived from Gambatte (Sindre Aamås, GPL-2.0+),
// which was reverse-engineered from real GBC hardware captures.
// Title strings match ROM bytes 0x134..0x143 exactly (null/space-trimmed).
// Unknown games in GBC or SGB mode fall back to plain grayscale.
//
// Color encoding: BGR555 = R | (G<<5) | (B<<10), each channel 0..31.
// The compile-time C() helper converts 0xRRGGBB literals to BGR555 with
// proper 8-to-5 bit rounding — zero runtime cost.
// ══════════════════════════════════════════════════════════════════════════════

struct GbcDmgPal {
    uint16_t bg[4];    // BG tile palette (BGR555)
    uint16_t obj0[4];  // Sprite palette 0
    uint16_t obj1[4];  // Sprite palette 1
};

// Compile-time RGB24 (0xRRGGBB) → BGR555.
// TO5B: rounds an 8-bit channel to 5 bits.
static constexpr uint16_t TO5B(uint32_t c) {
    return (uint16_t)((c * 31u * 2u + 255u) / (255u * 2u));
}
static constexpr uint16_t C(uint32_t rgb) {
    return (uint16_t)(  TO5B((rgb >> 16) & 0xFFu)
                      | (TO5B((rgb >>  8) & 0xFFu) << 5u)
                      | (TO5B( rgb        & 0xFFu) << 10u) );
}
// 4-color sub-palette in RGB24 notation (evaluated at compile time).
#define _C4(a,b,c,d)  { C(a), C(b), C(c), C(d) }

// Fallback: plain grayscale for unlicensed / unrecognised games.
static constexpr GbcDmgPal GBC_PAL_GREY = {
    _C4(0xFFFFFF, 0xA5A5A5, 0x525252, 0x000000),
    _C4(0xFFFFFF, 0xA5A5A5, 0x525252, 0x000000),
    _C4(0xFFFFFF, 0xA5A5A5, 0x525252, 0x000000)
};

// ── Per-game palette table ────────────────────────────────────────────────────
// Source: Gambatte gbcpalettes.h (Sindre Aamås, GPL-2.0+), hardware-verified.
// bg / obj0 / obj1 are often different per game — do NOT collapse them.
struct GbcTitleEntry {
    char      title[17];   // ROM title 0x134..0x143, null-terminated, trimmed
    GbcDmgPal pal;         // bg, obj0, obj1 in BGR555
};

static constexpr GbcTitleEntry GBC_TITLE_TABLE[] = {

    { "ALLEY WAY",
      { _C4(0xA59CFF,0xFFFF00,0x006300,0x000000),
        _C4(0xA59CFF,0xFFFF00,0x006300,0x000000),
        _C4(0xA59CFF,0xFFFF00,0x006300,0x000000) } },

    { "ASTEROIDS/MISCMD",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "ATOMIC PUNK",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "BA.TOSHINDEN",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    { "BALLOON KID",
      { _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000) } },

    { "BASEBALL",
      { _C4(0x52DE00,0xFF8400,0xFFFF00,0xFFFFFF),
        _C4(0xFFFFFF,0xFFFFFF,0x63A5FF,0x0000FF),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "BOMBERMAN GB",
      { _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "BOY AND BLOB GB1",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "BOY AND BLOB GB2",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "BT2RAGNAROKWORLD",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    { "DEFENDER/JOUST",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "DMG FOOTBALL",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "DONKEY KONG",
      { _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "DONKEYKONGLAND",
      { _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000),
        _C4(0xFFC542,0xFFD600,0x943A00,0x4A0000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "DONKEYKONGLAND 2",
      { _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000),
        _C4(0xFFC542,0xFFD600,0x943A00,0x4A0000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "DONKEYKONGLAND 3",
      { _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000),
        _C4(0xFFC542,0xFFD600,0x943A00,0x4A0000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "DONKEYKONGLAND95",
      { _C4(0xFFFF9C,0x94B5FF,0x639473,0x003A3A),
        _C4(0xFFC542,0xFFD600,0x943A00,0x4A0000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    // Dr. Mario: bg=obj0=blue, obj1=red — the pill colours.
    { "DR.MARIO",
      { _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "DYNABLASTER",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "F1RACE",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "FOOTBALL INT'L",
      { _C4(0x6BFF00,0xFFFFFF,0xFF524A,0x000000),
        _C4(0xFFFFFF,0xFFFFFF,0x63A5FF,0x0000FF),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "G&W GALLERY",
      { _C4(0xFFFFFF,0x7BFF00,0xB57300,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "GALAGA&GALAXIAN",
      { _C4(0x000000,0x008484,0xFFDE00,0xFFFFFF),
        _C4(0x000000,0x008484,0xFFDE00,0xFFFFFF),
        _C4(0x000000,0x008484,0xFFDE00,0xFFFFFF) } },

    { "GAME&WATCH",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "GAMEBOY GALLERY",
      { _C4(0xFFFFFF,0x7BFF00,0xB57300,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "GAMEBOY GALLERY2",
      { _C4(0xFFFFFF,0x7BFF00,0xB57300,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "GBWARS",
      { _C4(0xFFFFFF,0xADAD84,0x42737B,0x000000),
        _C4(0xFFFFFF,0xFF7300,0x944200,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "GBWARST",
      { _C4(0xFFFFFF,0xADAD84,0x42737B,0x000000),
        _C4(0xFFFFFF,0xFF7300,0x944200,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "GOLF",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "Game and Watch 2",
      { _C4(0xFFFFFF,0x7BFF00,0xB57300,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    // Japanese title of Kirby's Dream Land
    { "HOSHINOKA-BI",
      { _C4(0xA59CFF,0xFFFF00,0x006300,0x000000),
        _C4(0xFF6352,0xD60000,0x630000,0x000000),
        _C4(0x0000FF,0xFFFFFF,0xFFFF7B,0x0084FF) } },

    { "JAMES  BOND  007",
      { _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000) } },

    { "KAERUNOTAMENI",
      { _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000) } },

    { "KEN GRIFFEY JR",
      { _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "KID ICARUS",
      { _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "KILLERINSTINCT95",
      { _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "KINGOFTHEZOO",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "KIRAKIRA KIDS",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "KIRBY BLOCKBALL",
      { _C4(0xA59CFF,0xFFFF00,0x006300,0x000000),
        _C4(0xFF6352,0xD60000,0x630000,0x000000),
        _C4(0x0000FF,0xFFFFFF,0xFFFF7B,0x0084FF) } },

    // Kirby's Dream Land: purple BG, red-orange sprites, blue-yellow-lit obj1
    { "KIRBY DREAM LAND",
      { _C4(0xA59CFF,0xFFFF00,0x006300,0x000000),
        _C4(0xFF6352,0xD60000,0x630000,0x000000),
        _C4(0x0000FF,0xFFFFFF,0xFFFF7B,0x0084FF) } },

    { "KIRBY'S PINBALL",
      { _C4(0xA59CFF,0xFFFF00,0x006300,0x000000),
        _C4(0xFF6352,0xD60000,0x630000,0x000000),
        _C4(0xFF6352,0xD60000,0x630000,0x000000) } },

    { "KIRBY2",
      { _C4(0xA59CFF,0xFFFF00,0x006300,0x000000),
        _C4(0xFF6352,0xD60000,0x630000,0x000000),
        _C4(0x0000FF,0xFFFFFF,0xFFFF7B,0x0084FF) } },

    { "LOLO2",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "MAGNETIC SOCCER",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "MANSELL",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "MARIO & YOSHI",
      { _C4(0xFFFFFF,0x52FF00,0xFF4200,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "MARIO'S PICROSS",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    // Mario Land 2: teal BG, warm orange sprites, blue obj1
    { "MARIOLAND2",
      { _C4(0xFFFFCE,0x63EFEF,0x9C8431,0x5A5A5A),
        _C4(0xFFFFFF,0xFF7300,0x944200,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "MEGA MAN 2",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    { "MEGAMAN",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    { "MEGAMAN3",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    // Metroid II: blue BG, yellow-red sprite (Samus suit), green obj1
    { "METROID2",
      { _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFF00,0xFF0000,0x630000,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    { "MILLI/CENTI/PEDE",
      { _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "MOGURANYA",
      { _C4(0xFFFFFF,0xADAD84,0x42737B,0x000000),
        _C4(0xFFFFFF,0xFF7300,0x944200,0x000000),
        _C4(0xFFFFFF,0xFF7300,0x944200,0x000000) } },

    // Mystic Quest / Seiken Densetsu: green BG, red sprites, blue obj1
    { "MYSTIC QUEST",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "NETTOU KOF 95",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    { "NEW CHESSMASTER",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "OTHELLO",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "PAC-IN-TIME",
      { _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "PENGUIN WARS",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "PENGUINKUNWARSVS",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "PICROSS 2",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "PINOCCHIO",
      { _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000),
        _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000),
        _C4(0xFFC542,0xFFD600,0x943A00,0x4A0000) } },

    { "POKEBOM",
      { _C4(0xFFFFFF,0x8C8CDE,0x52528C,0x000000),
        _C4(0xFFC542,0xFFD600,0x943A00,0x4A0000),
        _C4(0xFFC542,0xFFD600,0x943A00,0x4A0000) } },

    // Pokémon Blue: blue BG+obj1, red-pink sprites (obj0)
    { "POKEMON BLUE",
      { _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "POKEMON GREEN",
      { _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000) } },

    // Pokémon Red: red/pink BG+obj1, bright green sprites (obj0)
    { "POKEMON RED",
      { _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "POKEMON YELLOW",
      { _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000) } },

    { "QIX",
      { _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "RADARMISSION",
      { _C4(0xFFFFFF,0xADAD84,0x42737B,0x000000),
        _C4(0xFFFFFF,0xFF7300,0x944200,0x000000),
        _C4(0xFFFFFF,0xADAD84,0x42737B,0x000000) } },

    { "ROCKMAN WORLD",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    { "ROCKMAN WORLD2",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    { "ROCKMANWORLD3",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    // Seiken Densetsu (JP title of Final Fantasy Adventure / Secret of Mana GB)
    { "SEIKEN DENSETSU",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "SOCCER",
      { _C4(0x6BFF00,0xFFFFFF,0xFF524A,0x000000),
        _C4(0xFFFFFF,0xFFFFFF,0x63A5FF,0x0000FF),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "SOLARSTRIKER",
      { _C4(0x000000,0x008484,0xFFDE00,0xFFFFFF),
        _C4(0x000000,0x008484,0xFFDE00,0xFFFFFF),
        _C4(0x000000,0x008484,0xFFDE00,0xFFFFFF) } },

    { "SPACE INVADERS",
      { _C4(0x000000,0x008484,0xFFDE00,0xFFFFFF),
        _C4(0x000000,0x008484,0xFFDE00,0xFFFFFF),
        _C4(0x000000,0x008484,0xFFDE00,0xFFFFFF) } },

    { "STAR STACKER",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "STAR WARS",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "STAR WARS-NOA",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "STREET FIGHTER 2",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    // Note: ROM title is "SUPER BOMBLISS  " (two trailing spaces); our trim strips them → key is no trailing space.
    { "SUPER BOMBLISS",
      { _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000) } },

    // Super Mario Land: periwinkle BG, inverted sprite palette (black base)
    { "SUPER MARIOLAND",
      { _C4(0xB5B5FF,0xFFFF94,0xAD5A42,0x000000),
        _C4(0x000000,0xFFFFFF,0xFF8484,0x943A3A),
        _C4(0x000000,0xFFFFFF,0xFF8484,0x943A3A) } },

    { "SUPER RC PRO-AM",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000) } },

    { "SUPERDONKEYKONG",
      { _C4(0xFFFF9C,0x94B5FF,0x639473,0x003A3A),
        _C4(0xFFC542,0xFFD600,0x943A00,0x4A0000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    // Wario Land: tan/teal BG, orange sprites, cyan-red obj1
    { "SUPERMARIOLAND3",
      { _C4(0xFFFFFF,0xADAD84,0x42737B,0x000000),
        _C4(0xFFFFFF,0xFF7300,0x944200,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    // Tamagotchi (USA/Europe, SGB Enhanced) — ROM title is "GB TAMAGOTCHI".
    // Bandai licensee (0x33/B2), not Nintendo, so the licensee gate must run
    // AFTER the table lookup (see step 3 comment below).
    { "GB TAMAGOTCHI",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "TENNIS",
      { _C4(0x6BFF00,0xFFFFFF,0xFF524A,0x000000),
        _C4(0xFFFFFF,0xFFFFFF,0x63A5FF,0x0000FF),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "TETRIS",
      { _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000) } },

    { "TETRIS ATTACK",
      { _C4(0xFFFFFF,0x52FF00,0xFF4200,0x000000),
        _C4(0xFFFFFF,0x52FF00,0xFF4200,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "TETRIS BLAST",
      { _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000) } },

    { "TETRIS FLASH",
      { _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "TETRIS PLUS",
      { _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "TETRIS2",
      { _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFFFF00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "THE CHESSMASTER",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "TOPRANKINGTENNIS",
      { _C4(0x6BFF00,0xFFFFFF,0xFF524A,0x000000),
        _C4(0xFFFFFF,0xFFFFFF,0x63A5FF,0x0000FF),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "TOPRANKTENNIS",
      { _C4(0x6BFF00,0xFFFFFF,0xFF524A,0x000000),
        _C4(0xFFFFFF,0xFFFFFF,0x63A5FF,0x0000FF),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "TOY STORY",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "VEGAS STAKES",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "WARIO BLAST",
      { _C4(0xFFFFFF,0x7BFF31,0x0063C5,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "WARIOLAND2",
      { _C4(0xFFFFFF,0xADAD84,0x42737B,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },

    { "WAVERACE",
      { _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFFFF7B,0x0084FF,0xFF0000) } },

    { "WORLD CUP",
      { _C4(0xFFFFFF,0x7BFF31,0x008400,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    { "X",
      { _C4(0xFFFFFF,0xA5A5A5,0x525252,0x000000),
        _C4(0xFFFFFF,0xA5A5A5,0x525252,0x000000),
        _C4(0xFFFFFF,0xA5A5A5,0x525252,0x000000) } },

    { "YAKUMAN",
      { _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000),
        _C4(0xFFFFFF,0xFFAD63,0x843100,0x000000) } },

    { "YOSHI'S COOKIE",
      { _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "YOSSY NO COOKIE",
      { _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0xFF9C00,0xFF0000,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "YOSSY NO PANEPON",
      { _C4(0xFFFFFF,0x52FF00,0xFF4200,0x000000),
        _C4(0xFFFFFF,0x52FF00,0xFF4200,0x000000),
        _C4(0xFFFFFF,0x5ABDFF,0xFF0000,0x0000FF) } },

    { "YOSSY NO TAMAGO",
      { _C4(0xFFFFFF,0x52FF00,0xFF4200,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000) } },

    // Zelda: Link's Awakening — red/pink BG, bright-green Link sprites, blue obj1
    { "ZELDA",
      { _C4(0xFFFFFF,0xFF8484,0x943A3A,0x000000),
        _C4(0xFFFFFF,0x00FF00,0x318400,0x004A00),
        _C4(0xFFFFFF,0x63A5FF,0x0000FF,0x000000) } },
};
#undef _C4

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
extern int      g_gbc_pal_idx;        // index into GBC_TITLE_TABLE (-1 = not found)
extern bool     g_gbc_pal_found;      // true if game was found in GBC_TITLE_TABLE

// ── LCD ghosting (frame blending) ─────────────────────────────────────────────
// When true, each rendered frame is blended 50/50 with the previous raw frame,
// simulating the GBC LCD's physical phosphor persistence.  Games that flicker
// sprites on/off at 30Hz (e.g. Chain Chomp's chain in Link's Awakening) were
// designed for this hardware characteristic — the blend produces the intended
// semi-transparent ghost rather than a harsh flicker.
// The blend is applied by apply_lcd_ghosting() in gb_renderer.h after each frame.
extern bool g_lcd_ghosting;

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
// Reads the ROM title string, performs a Nintendo licensee check (GBC mode
// only), looks up the per-game palette from GBC_TITLE_TABLE, and pre-bakes
// g_dmg_flat_pal so that gb_lcd_draw_line is a branchless table lookup.
static inline void gb_select_dmg_palette() {
    // Step 1: extract and trim the ROM title from 0x134..0x143 (16 bytes).
    // Null-terminate at the first embedded 0x00, then strip trailing spaces.
    char title[17] = {};
    if (g_gb.rom && g_gb.romSize > 0x143)
        memcpy(title, g_gb.rom + 0x134, 16);
    title[16] = '\0';
    int len = 0;
    while (len < 16 && title[len] != '\0') ++len;
    while (len > 0 && title[len - 1] == ' ') --len;
    title[len] = '\0';

    // Step 2: title string lookup — always run first, regardless of licensee.
    // If the game is in our table we know its correct palette and always apply
    // it, even for non-Nintendo publishers (e.g. Bandai, Konami).
    // The licensee gate (step 3) only affects games NOT in the table.
    g_gbc_pal_found = false;
    g_gbc_pal_idx   = -1;
    const GbcDmgPal* found_pal = nullptr;
    for (int i = 0; i < GBC_TITLE_TABLE_SIZE; ++i) {
        if (strcmp(GBC_TITLE_TABLE[i].title, title) == 0) {
            found_pal       = &GBC_TITLE_TABLE[i].pal;
            g_gbc_pal_idx   = i;
            g_gbc_pal_found = true;
            break;
        }
    }

    // Step 3: Nintendo licensee gate (GBC mode, unknown games only).
    // The real GBC boot ROM only colourises Nintendo-licensed games it doesn't
    // recognise.  If we already have an explicit entry we bypass this entirely.
    // SGB mode has no licensee restriction at all.
    bool allow_color = g_gbc_pal_found
                    || (g_palette_mode == PaletteMode::SGB);
    if (!allow_color && g_gb.rom && g_gb.romSize > 0x145) {
        const uint8_t old_lic = g_gb.rom[0x014B];
        if (old_lic == 0x01) {
            allow_color = true;   // old licensee code: Nintendo
        } else if (old_lic == 0x33 &&
                   g_gb.rom[0x0144] == '0' &&
                   g_gb.rom[0x0145] == '1') {
            allow_color = true;   // new licensee code "01": Nintendo
        }
    }
    if (!allow_color) {
        found_pal       = nullptr;
        g_gbc_pal_found = false;
        g_gbc_pal_idx   = -1;
    }

    // Step 4: choose the source sub-palette arrays based on mode and match.
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

        case PaletteMode::GBC:
        case PaletteMode::SGB:
        default:
            if (found_pal) {
                bg_src  = found_pal->bg;
                ob0_src = found_pal->obj0;
                ob1_src = found_pal->obj1;
            } else {
                // Unknown / unlicensed game: plain grayscale.
                bg_src = ob0_src = ob1_src = GBC_PAL_GREY.bg;
            }
            break;
    }

    // Step 5: pre-pack into the flat lookup table (RGBA4444, ready for the
    // renderer).  Layout mirrors Walnut pixel encoding:
    //   0x00-0x03 = OBJ0,  0x10-0x13 = OBJ1,  0x20-0x23 = BG.
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
//
// LCD ghosting is applied as a post-frame pass by apply_lcd_ghosting()
// in gb_renderer.h, which blends g_gb_fb (raw current frame) against
// s_prev_fb (raw previous frame).  See gb_renderer.h for details.
static void gb_lcd_draw_line(struct gb_s* gb,
                              const uint8_t* pixels,
                              const uint_fast8_t line)
{
    if ((int)line >= GB_H) return;
    uint16_t* row = g_gb_fb + (int)line * GB_W;

#if WALNUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode) {
        if (g_fb_is_prepacked) {
            // CGB-compatible game running in CGB hardware mode but displaying
            // with a user-chosen DMG palette (DMG-green / Native grey).
            // The emulator core keeps cgbMode=1 so all CGB hardware (double
            // speed, VRAM banking, CGB palettes) runs correctly.  Only the
            // display output is re-routed through the DMG flat palette.
            //
            // CGB pixel-index layout (Walnut CGB scanline renderer):
            //   0x00–0x1F  BG  (palette_num[4:2] × 4 + color_id[1:0])
            //   0x20–0x3F  OBJ (palette_num[4:2] × 4 + color_id[1:0] + 0x20)
            //   bits[1:0]  = raw color_id from tile data — NOT a shade value.
            //
            // WHY WE CANNOT USE (px & 0x03) AS SHADE:
            //   In CGB mode, color_id (bits[1:0]) is a raw index into the
            //   game's CGB palette registers.  The game defines what RGB color
            //   each index means.  A CGB-compat game may put color 0 = black
            //   and color 3 = white (inverse of DMG convention), producing
            //   inverted glyphs and colors when the raw index is used as shade.
            //
            // CORRECT APPROACH: look up the true RGB565 color from
            //   fixPalette[px], compute perceptual luminance, and map to
            //   shade 0 (lightest) … 3 (darkest) for the DMG palette.
            //
            // BT.601-approximate luma (all channels normalised to 5-bit):
            //   luma = (R×2 + G5×5 + B) / 8,  range 0–31
            //   shade = 3 − (luma >> 3)         (inverted: high luma = light)
            for (int x = 0; x < GB_W; x++) {
                const uint8_t  px  = pixels[x];
                const uint16_t rgb = gb->cgb.fixPalette[px];
                // RGB565: R=[15:11] 5-bit, G=[10:5] 6-bit, B=[4:0] 5-bit
                const uint8_t r  = (uint8_t)((rgb >> 11) & 0x1Fu);
                const uint8_t g5 = (uint8_t)((rgb >>  6) & 0x1Fu);  // top 5 of 6 green bits
                const uint8_t b  = (uint8_t)( rgb        & 0x1Fu);
                // Perceptual luma 0–31 (max = (31×2+31×5+31)/8 = 31)
                const uint8_t luma  = (uint8_t)((r * 2u + g5 * 5u + b) / 8u);
                // shade 0 = lightest (luma 24–31), shade 3 = darkest (luma 0–7)
                const uint8_t shade = 3u - (luma >> 3u);
                // Route through BG or OBJ sub-palette slot
                row[x] = g_dmg_flat_pal[(px < 0x20u) ? (0x20u | shade) : shade];
            }
        } else {
            // Normal CGB path: fixPalette is pre-built RGB565 — one lookup per pixel.
            for (int x = 0; x < GB_W; x++)
                row[x] = gb->cgb.fixPalette[pixels[x]];
        }
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
        // Stamp the frame origin so every audio_write() call during this frame
        // can compute an accurate T-cycle offset for generate_samples().
        // Must be called immediately before gb_run_frame_dualfetch() so that
        // delta_ns = (audio_write time) - (frame start time) is proportional
        // to T-cycles elapsed in the frame.
        gb_audio_mark_frame_start();
        gb_run_frame_dualfetch(&g_gb.gb);
        //gb_run_frame(&g_gb.gb);
    }
}

// Update the joypad from the overlay's keysHeld bitmask.
// Uses Peanut-GB's active-low convention: 0 = pressed.
void gb_set_input(uint64_t keysHeld);