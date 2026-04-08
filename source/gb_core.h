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
 * 
 *  Licensed under GPLv2
 *  Copyright (c) 2026 ppkantorski
 ********************************************************************************/

#pragma once


#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <arm_neon.h>


// Forward-declare the audio callbacks with an explicit O3 pragma so the
// declaration attribute always matches the definition in gb_audio.h (which is
// compiled at O3 via its own push/pop).  Wrapping with push/pop makes this
// immune to whatever surrounding pragma is active when gb_core.h is included
// (e.g. elm_ultraframe.hpp pushes Os before pulling in gb_renderer.h which
// then includes this file — a bare declaration would inherit Os and cause a
// -Wattributes mismatch).
#pragma GCC push_options
#pragma GCC optimize("O3")
extern "C" {
    uint8_t audio_read(uint8_t addr);
    void    audio_write(uint8_t addr, uint8_t val);
}


// ── Walnut-GB compile-time options ───────────────────────────────────────────
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
#ifndef WALNUT_GB_HIGH_LCD_ACCURACY // already enabled by default in walnut
#  define WALNUT_GB_HIGH_LCD_ACCURACY 1
#endif
#ifndef GB_INTERNAL
#  define GB_INTERNAL
#endif
// Safe dual-fetch flags (MBC/DMA/opcodes) are set to 1 directly in
// walnut_cgb.h — they are unconditional #defines so #ifndef guards here
// have no effect.  Edit walnut_cgb.h lines 62-64 to change them.
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
// Per-DMG-game setting stored in sdmc:/config/ultragbc/configure/<rom>.ini
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
    struct gb_s* gb = nullptr;  // Peanut-GB core state — heap-allocated on game load,
                                // freed on unload (~49 KB WRAM+VRAM off BSS when idle)

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
// Heap-allocated in gb_load_rom, freed in gb_unload_rom (~45 KB off BSS when idle).
extern uint16_t* g_gb_fb;
extern bool      g_fb_is_prepacked;  // true → g_gb_fb already contains RGBA4444

// ── RGB555 → RGBA4444 packer ─────────────────────────────────────────────────
// Strips the LSB of each 5-bit channel to produce a 4-bit channel; alpha = 0xF.
// Layout (little-endian): r4 | g4<<4 | b4<<8 | a4<<12.
// Defined here so gb_select_dmg_palette can pre-bake the flat palette without
// including gb_renderer.h (which would create a circular dependency).
// ── Per-function optimization ─────────────────────────────────────────────────
// Default to Os for cold setup paths (gb_select_dmg_palette, gb_error, etc.).
// Hot callbacks used every emulation frame carry __attribute__((optimize("O3"))).
// push/pop confines the pragma to this file — does not bleed into includers.
#pragma GCC push_options
#pragma GCC optimize("Os")

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

// ── Walnut-CGB C callbacks (static so they have C linkage via the extern "C" block) ─
// Defined in main.cpp where the global state lives.

static uint8_t __attribute__((optimize("O3"))) gb_rom_read(struct gb_s*, const uint_fast32_t addr) {
    if (addr >= g_gb.romSize) return 0xFF;
    return g_gb.rom[addr];
}

// Walnut-CGB requires 16-bit and 32-bit ROM read functions for its dual-fetch
// CPU execution model.  These are simple unaligned little-endian reads —
// safe because g_gb.rom is a heap buffer with no alignment restrictions.
static uint16_t __attribute__((optimize("O3"))) gb_rom_read16(struct gb_s*, const uint_fast32_t addr) {
    if (addr + 1 >= g_gb.romSize) return 0xFFFF;
    return (uint16_t)g_gb.rom[addr] | ((uint16_t)g_gb.rom[addr + 1] << 8);
}

static uint32_t __attribute__((optimize("O3"))) gb_rom_read32(struct gb_s*, const uint_fast32_t addr) {
    if (addr + 3 >= g_gb.romSize) return 0xFFFFFFFF;
    return (uint32_t)g_gb.rom[addr]
         | ((uint32_t)g_gb.rom[addr + 1] << 8)
         | ((uint32_t)g_gb.rom[addr + 2] << 16)
         | ((uint32_t)g_gb.rom[addr + 3] << 24);
}

static uint8_t __attribute__((optimize("O3"))) gb_cart_ram_read(struct gb_s*, const uint_fast32_t addr) {
    if (!g_gb.cartRam || addr >= g_gb.cartRamSz) return 0xFF;
    return g_gb.cartRam[addr];
}

static void __attribute__((optimize("O3"))) gb_cart_ram_write(struct gb_s*, const uint_fast32_t addr, const uint8_t val) {
    if (!g_gb.cartRam || addr >= g_gb.cartRamSz) return;
    g_gb.cartRam[addr] = val;
}

static void gb_error(struct gb_s*, const enum gb_error_e, const uint16_t) {
    // Silently swallow emulation errors; the game will just glitch rather than crash.
}

// ── LCD draw-line callback (Absolute fastest NEON-safe, color-correct) ──────
//
// Notes:
// 1. CGB mode (gb->cgb.cgbMode != 0):
//    - Pixel bytes are direct indexes into gb->cgb.fixPalette[] (prebuilt RGB565).
//    - Upper 0x20 bit selects BG/OBJ.
//    - Computes luma/shade fully vectorized for 8 pixels at a time.
//
// 2. DMG mode (non-CGB ROM):
//    - Pixel bits[1:0] = color index, bits[5:4] = source.
//    - Uses g_dmg_flat_pal[] lookup.
//    - Fully NEON vectorized for 8 pixels at a time.
//
// 3. Remainder pixels handled scalar for safety.
// 4. Framebuffer is g_gb_fb (raw 16-bit RGB565), ghosting handled later.
static void __attribute__((optimize("O3"))) gb_lcd_draw_line(struct gb_s* gb,
                             const uint8_t* pixels,
                             const uint_fast8_t line)
{
    if ((int)line >= GB_H) return;                // Out-of-bounds check
    uint16_t* row = g_gb_fb + (int)line * GB_W;  // Pointer to current line

#if WALNUT_FULL_GBC_SUPPORT
    if (gb->cgb.cgbMode) {
        if (g_fb_is_prepacked) {
            int x = 0;

            // --- CGB prepacked DMG display path ---
            for (; x + 7 < GB_W; x += 8) {
                // Load 8 pixel indices
                uint8x8_t px8 = vld1_u8(pixels + x);

                // --- Per-pixel scatter-load from fixPalette ---
                // Each pixel independently indexes fixPalette at px8[i] — these
                // are non-contiguous addresses, so vld1_u16 (sequential load) is
                // wrong here.  Load each entry individually into its own lane.
                uint16x8_t rgb = {
                    gb->cgb.fixPalette[px8[0]],
                    gb->cgb.fixPalette[px8[1]],
                    gb->cgb.fixPalette[px8[2]],
                    gb->cgb.fixPalette[px8[3]],
                    gb->cgb.fixPalette[px8[4]],
                    gb->cgb.fixPalette[px8[5]],
                    gb->cgb.fixPalette[px8[6]],
                    gb->cgb.fixPalette[px8[7]]
                };

                // Extract R/G/B channels (5-bit each)
                uint16x8_t r  = vandq_u16(vshrq_n_u16(rgb, 11), vdupq_n_u16(0x1F));
                uint16x8_t g5 = vandq_u16(vshrq_n_u16(rgb, 6),  vdupq_n_u16(0x1F));
                uint16x8_t b  = vandq_u16(rgb, vdupq_n_u16(0x1F));

                // Compute luma = (2*r + 5*g5 + b) >> 3
                uint16x8_t luma = vshrq_n_u16(
                    vaddq_u16(vaddq_u16(vshlq_n_u16(r,1), vmulq_n_u16(g5,5)), b), 3
                );

                // Compute shade = 3 - (luma >> 3)
                uint16x8_t shade = vsubq_u16(vdupq_n_u16(3), vshrq_n_u16(luma, 3));

                // BG/OBJ mask: if px & 0x20 == 0 select OBJ (0x20)
                uint16x8_t mask = vbslq_u16(
                    vandq_u16(vmovl_u8(px8), vdupq_n_u16(0x20)),
                    vdupq_n_u16(0), vdupq_n_u16(0x20)
                );

                uint16x8_t idx = vorrq_u16(shade, mask);

                // --- Fully unrolled store, avoids vgetq_lane issues ---
                row[x+0] = g_dmg_flat_pal[idx[0]];
                row[x+1] = g_dmg_flat_pal[idx[1]];
                row[x+2] = g_dmg_flat_pal[idx[2]];
                row[x+3] = g_dmg_flat_pal[idx[3]];
                row[x+4] = g_dmg_flat_pal[idx[4]];
                row[x+5] = g_dmg_flat_pal[idx[5]];
                row[x+6] = g_dmg_flat_pal[idx[6]];
                row[x+7] = g_dmg_flat_pal[idx[7]];
            }

            // Scalar remainder
            for (; x < GB_W; x++) {
                const uint8_t px = pixels[x];
                const uint16_t rgb = gb->cgb.fixPalette[px];
                const uint8_t r  = (rgb >> 11) & 0x1F;
                const uint8_t g5 = (rgb >> 6)  & 0x1F;
                const uint8_t b  = rgb & 0x1F;
                const uint8_t luma = (r*2 + g5*5 + b) >> 3;
                const uint8_t shade = 3 - (luma >> 3);
                row[x] = g_dmg_flat_pal[shade | ((px & 0x20u) ? 0 : 0x20u)];
            }

            return;
        } else {
            // Normal CGB path: 8-wide scalar unroll.
            //
            // fixPalette[] is 64 entries × 2 bytes = 128 bytes — fits in two
            // cache lines and stays hot in L1 after the first scanline.
            //
            // Pure scalar beats vld1_u8 + vget_lane_u8 here: each vget_lane_u8
            // crosses from NEON to the ARM integer unit (~8-cycle stall on
            // Cortex-A57).  With 8 extractions per iteration that is ≥64 stall
            // cycles per loop body — worse than 8 independent ldrb instructions
            // that the OOO integer pipeline overlaps freely.
            //
            // Caching fixPalette in a local lets the compiler hold the base
            // pointer in one register across all 20 loop iterations (GB_W=160).
            // GB_W=160 is divisible by 8, so the loop covers every pixel exactly.
            const uint16_t* const fp = gb->cgb.fixPalette;
            for (int x = 0; x < GB_W; x += 8) {
                row[x+0] = fp[pixels[x+0]];
                row[x+1] = fp[pixels[x+1]];
                row[x+2] = fp[pixels[x+2]];
                row[x+3] = fp[pixels[x+3]];
                row[x+4] = fp[pixels[x+4]];
                row[x+5] = fp[pixels[x+5]];
                row[x+6] = fp[pixels[x+6]];
                row[x+7] = fp[pixels[x+7]];
            }
        }
        return;
    }
#endif

    // --- DMG path ---
    int x = 0;
    for (; x + 7 < GB_W; x += 8) {
        uint8x8_t px = vld1_u8(pixels + x);
        uint8x8_t idx = vand_u8(px, vdup_n_u8(0x33));

        // Fully unrolled vectorized store
        row[x+0] = g_dmg_flat_pal[idx[0]];
        row[x+1] = g_dmg_flat_pal[idx[1]];
        row[x+2] = g_dmg_flat_pal[idx[2]];
        row[x+3] = g_dmg_flat_pal[idx[3]];
        row[x+4] = g_dmg_flat_pal[idx[4]];
        row[x+5] = g_dmg_flat_pal[idx[5]];
        row[x+6] = g_dmg_flat_pal[idx[6]];
        row[x+7] = g_dmg_flat_pal[idx[7]];
    }

    // Scalar remainder
    for (; x < GB_W; x++)
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
inline void __attribute__((optimize("O3"))) gb_run_one_frame() {
    if (g_gb.running) {
        // Stamp the frame origin so every audio_write() call during this frame
        // can compute an accurate T-cycle offset for generate_samples().
        // Must be called immediately before gb_run_frame_dualfetch() so that
        // delta_ns = (audio_write time) - (frame start time) is proportional
        // to T-cycles elapsed in the frame.
        gb_audio_mark_frame_start();
        gb_run_frame_dualfetch(g_gb.gb);
        //gb_run_frame(g_gb.gb);
    }
}

// Update the joypad from the overlay's keysHeld bitmask.
// Uses Peanut-GB's active-low convention: 0 = pressed.
void gb_set_input(uint64_t keysHeld);

#pragma GCC pop_options