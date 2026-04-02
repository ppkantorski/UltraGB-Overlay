/********************************************************************************
 * File: gb_renderer.h
 * Description:
 *   Inline renderer that scales the 160x144 RGB555/RGB565 GB framebuffer into
 *   the 400x360 overlay viewport using nearest-neighbour integer scaling.
 *
 *   Performance architecture - bypasses drawRect entirely for the game screen:
 *
 *   The Tesla overlay framebuffer is RGBA4444, block-linear (swizzled).
 *   getPixelOffset(x, y) decomposes exactly as:
 *       offset(x, y) = col_part(x) + row_part(y)
 *   with zero cross terms, allowing a split LUT:
 *       s_col_lut[ox]  - swizzle contribution of output column (400 u32s = 1.6 KB)
 *       s_row_lut[oy]  - swizzle contribution of output row    (360 u32s = 1.4 KB)
 *   Total LUT: ~3 KB static.  Verified algebraically against the full formula.
 *
 *   Inner loop per output pixel (inside a colour run):
 *       framebuffer[s_row_lut[oy] + s_col_lut[ox]] = packed_color;
 *   - one addition, one store, zero swizzle math, zero blend math.
 *   Colour conversion fires exactly once per RLE run (not per pixel).
 *
 *   8x unrolled scatter-write loop lets the CPU pipeline independent stores
 *   to non-contiguous swizzled addresses.
 *
 *   offsetWidthVar for the fixed 448-wide framebuffer:
 *       ((448/2) >> 4) << 3 = 112  (compile-time constant OWV).
 * 
 *  Licensed under GPLv2
 *  Copyright (c) 2026 ppkantorski
 ********************************************************************************/

#pragma once

#include <tesla.hpp>
#include "gb_core.h"

// Set by gb_load_rom: true when the ROM is a CGB game (Walnut outputs RGB565).
extern bool g_fb_is_rgb565;
// true when g_gb_fb stores pre-packed RGBA4444 values (DMG games only).
// gb_select_dmg_palette bakes the conversion into g_dmg_flat_pal so
// gb_lcd_draw_line can write RGBA4444 directly, skipping runtime conversion.
extern bool g_fb_is_prepacked;
// PaletteMode defined in gb_core.h (included above)

// ── Runtime viewport scale toggle ─────────────────────────────────────────────
// false = 2.5× (400×360, default), true = 2× (320×288, pixel-perfect integer).
// Toggled by a quick tap on the screen region.
// VP_X/VP_Y/VP_W/VP_H constexpr values in gb_core.h are the 2.5× maximums and
// still used for static array sizing — runtime queries go through these inlines.
extern bool g_vp_2x;
inline int vp_x() { return g_vp_2x ? 64  : VP_X; }         // 2×: (448-320)/2 = 64
inline int vp_y() { return g_vp_2x ? VP_Y + (VP_H-288)/2   // 2×: centre on same midpoint
                                    : VP_Y; }               // 2.5×: original position
inline int vp_w() { return g_vp_2x ? 320 : VP_W; }
inline int vp_h() { return g_vp_2x ? 288 : VP_H; }

// -- Colour conversion to packed RGBA4444 uint16 ------------------------------
// tsl::Color layout (little-endian struct): r4 | g4<<4 | b4<<8 | a4<<12
// Both converters produce a=0xF (fully opaque).

inline uint16_t rgb555_to_packed(const uint16_t c) {
    return static_cast<uint16_t>(
        ((c >> 1) & 0x000F) |
        ((c >> 2) & 0x00F0) |
        ((c >> 3) & 0x0F00) |
        0xF000u
    );
}

inline uint16_t rgb565_to_packed(const uint16_t c) {
    return static_cast<uint16_t>(
        ((c >> 12) & 0x000F) |  // R: bits 15..12 -> 3..0
        ((c >>  3) & 0x00F0) |  // G: bits 10..7  -> 7..4
        ((c <<  7) & 0x0F00) |  // B: bits 4..1   -> 11..8
        0xF000u
    );
}

// tsl::Color versions retained for border drawing
inline tsl::Color rgb555_to_tsl(const uint16_t c) {
    return tsl::Color{
        static_cast<uint8_t>( (c        & 0x1F) >> 1),
        static_cast<uint8_t>(((c >>  5) & 0x1F) >> 1),
        static_cast<uint8_t>(((c >> 10) & 0x1F) >> 1),
        0xF
    };
}
inline tsl::Color rgb565_to_tsl(const uint16_t c) {
    return tsl::Color{
        static_cast<uint8_t>((c >> 12) & 0xF),
        static_cast<uint8_t>((c >>  7) & 0xF),
        static_cast<uint8_t>((c >>  1) & 0xF),
        0xF
    };
}

// -- Swizzle decomposition LUT ------------------------------------------------
// Tesla's getPixelOffset(x, y) [no scissoring]:
//   = ((((y&127)>>4) + ((x>>5)<<3) + ((y>>7)*OWV)) << 9)
//   + ((y&8)<<5) + ((x&16)<<3) + ((y&6)<<4) + ((x&8)<<1) + ((y&1)<<3) + (x&7)
//
// Every term is purely f(x) or purely g(y).  So offset = col_part(x)+row_part(y).
// OWV = offsetWidthVar = ((framebufferWidth/2)>>4)<<3 = 112 for 448-wide FB.

static constexpr uint32_t OWV = 112u;

// LUTs indexed by viewport-relative coordinates:
//   s_col_lut[ox]  for ox in [0, VP_W)  ->  col_part(VP_X + ox)
//   s_row_lut[oy]  for oy in [0, VP_H)  ->  row_part(VP_Y + oy)
static uint32_t s_col_lut[VP_W];   // 1600 bytes
static uint32_t s_row_lut[VP_H];   // 1440 bytes
static bool     s_lut_ready = false;

// Outer LUTs cover the fixed outer viewport frame (VP_X..VP_X+VP_W, VP_Y..VP_Y+VP_H).
// Unlike s_col_lut/s_row_lut (which track the runtime-togglable inner viewport),
// these are indexed from the compile-time constants VP_X/VP_Y and are built once,
// never invalidated.  Used by render_gb_letterbox for fast scatter-writes without
// any per-pixel swizzle computation.
static uint32_t s_outer_col_lut[VP_W];   // col_part(VP_X + ox), ox = 0..VP_W-1
static uint32_t s_outer_row_lut[VP_H];   // row_part(VP_Y + oy), oy = 0..VP_H-1
static bool     s_outer_lut_ready = false;

static void init_outer_lut() {
    for (int i = 0; i < VP_W; ++i) {
        const uint32_t x = static_cast<uint32_t>(VP_X + i);
        s_outer_col_lut[i] = (((x >> 5) << 3) << 9)
                           + ((x & 16u) << 3)
                           + ((x &  8u) << 1)
                           +  (x &  7u);
    }
    for (int i = 0; i < VP_H; ++i) {
        const uint32_t y = static_cast<uint32_t>(VP_Y + i);
        s_outer_row_lut[i] = ((((y & 127u) >> 4) + ((y >> 7) * OWV)) << 9)
                           + ((y &  8u) << 5)
                           + ((y &  6u) << 4)
                           + ((y &  1u) << 3);
    }
    s_outer_lut_ready = true;
}

static void init_swizzle_lut() {
    const int cvp_x = vp_x(), cvp_y = vp_y();
    const int cvp_w = vp_w(), cvp_h = vp_h();
    for (int i = 0; i < cvp_w; ++i) {
        const uint32_t x = static_cast<uint32_t>(cvp_x + i);
        s_col_lut[i] = (((x >> 5) << 3) << 9)
                     + ((x & 16u) << 3)
                     + ((x &  8u) << 1)
                     +  (x &  7u);
    }
    for (int i = 0; i < cvp_h; ++i) {
        const uint32_t y = static_cast<uint32_t>(cvp_y + i);
        s_row_lut[i] = ((((y & 127u) >> 4) + ((y >> 7) * OWV)) << 9)
                     + ((y &  8u) << 5)
                     + ((y &  6u) << 4)
                     + ((y &  1u) << 3);
    }
    s_lut_ready = true;
}

// -- Source-pixel coordinate LUTs ---------------------------------------------
// Maps each GB source pixel index to its output viewport extent (VP-relative).
static bool s_coord_ready = false;
static int  s_dx0[GB_W], s_dx1[GB_W];   // int: no sign-extension in hot loop
static int  s_dy0[GB_H], s_dy1[GB_H];

static void init_coord_lut() {
    const int cvp_w = vp_w(), cvp_h = vp_h();
    for (int i = 0; i < GB_W; ++i) {
        s_dx0[i] = i       * cvp_w / GB_W;
        s_dx1[i] = (i + 1) * cvp_w / GB_W;
    }
    for (int i = 0; i < GB_H; ++i) {
        s_dy0[i] = i       * cvp_h / GB_H;
        s_dy1[i] = (i + 1) * cvp_h / GB_H;
    }
    s_coord_ready = true;
}

// Toggle between 2.5× and 2× scale.  Invalidates both LUTs so they rebuild
// on the very next draw() call — zero overhead on every other frame.
inline void toggle_vp_scale() {
    g_vp_2x = !g_vp_2x;
    s_lut_ready   = false;
    s_coord_ready = false;
}

// Set the viewport scale to an explicit value and invalidate the LUTs.
// Use this instead of toggle_vp_scale() when the caller already knows the
// desired state (e.g. a MiniToggleListItem listener, where the framework
// has already flipped its own internal state before calling the listener).
// Calling toggle_vp_scale() there would double-flip g_vp_2x and leave the
// LUTs rebuilt at the WRONG scale — the exact bug the Settings toggle had.
inline void set_vp_scale(const bool want_2x) {
    g_vp_2x       = want_2x;
    s_lut_ready   = false;
    s_coord_ready = false;
}

// -- LCD grid effect ----------------------------------------------------------
// When enabled, darkens the last destination row and column of each GB source
// pixel's scaled block, simulating the dark sub-pixel gap of a real Game Boy
// Color LCD screen.
//
// g_lcd_grid is defined in main.cpp; declared extern here so gb_windowed.hpp
// (which includes this header) can share the same flag.

extern bool g_lcd_grid;  // false = off (default), true = show LCD grid

// Dim a packed RGBA4444 pixel to ~75 % brightness, alpha kept.
// Each RGB channel is reduced by one quarter: new = ch - (ch >> 2).
// This sits in the sweet spot where the grid line is visibly darker than the
// lit cell but the underlying colour still reads clearly — comparable to the
// GBC LCD gap tint.  No inter-field carry: each channel is extracted before
// the arithmetic.
static inline uint16_t dim_packed_grid(const uint16_t p) {
    const uint16_t r = (p      ) & 0xFu;
    const uint16_t g = (p >>  4) & 0xFu;
    const uint16_t b = (p >>  8) & 0xFu;
    return static_cast<uint16_t>(
        (p & 0xF000u)
        | ((b - (b >> 2u)) << 8u)
        | ((g - (g >> 2u)) << 4u)
        |  (r - (r >> 2u)));
}

// Post-pass: darken grid-line pixels in the overlay viewport.
//
// Runs after render_gb_screen_chunk — both s_coord_ready and s_lut_ready are
// guaranteed true at that point.
//
// Complexity:
//   Horizontal lines: GB_H × vp_w()   writes   (last dest-row of each GB row)
//   Vertical   lines: GB_H × (avg_cell_h-1) × GB_W   writes   (last dest-col
//     of each GB col, excluding rows already processed as horizontal lines)
//
// At 2.5×: ≈ 144×400 + 144×1.5×160 ≈ 92 K writes per frame when enabled.
// At 2×:   ≈ 144×320 + 144×1×160   ≈ 69 K writes per frame when enabled.
// Either way < 0.1 ms on the Switch CPU — negligible.
inline void render_gb_grid_overlay(tsl::gfx::Renderer* renderer) {
    if (!g_lcd_grid) return;

    tsl::Color* fb = static_cast<tsl::Color*>(renderer->getCurrentFramebuffer());
    const int cvp_w = vp_w();

    // ── Horizontal grid lines ─────────────────────────────────────────────
    // For each GB source row sy, darken every pixel in the last dest-row of
    // that row's scaled block: oy = s_dy1[sy] - 1.
    for (int sy = 0; sy < GB_H; ++sy) {
        const uint32_t row_off = s_row_lut[s_dy1[sy] - 1];
        for (int ox = 0; ox < cvp_w; ++ox) {
            uint16_t& px = reinterpret_cast<uint16_t&>(fb[row_off + s_col_lut[ox]]);
            px = dim_packed_grid(px);
        }
    }

    // ── Vertical grid lines ───────────────────────────────────────────────
    // For each dest-row that is NOT itself a horizontal grid line, darken the
    // last dest-col of each GB source column: ox = s_dx1[sx] - 1.
    // Skipping the grid rows avoids reprocessing intersections (no visible
    // difference, but saves writes and avoids a second dim pass darkening them
    // to ~1.5 % — they stay at the single-pass ~12.5 %).
    for (int sy = 0; sy < GB_H; ++sy) {
        const int oy_end = s_dy1[sy] - 1;   // exclusive upper bound (the grid row itself)
        for (int oy = s_dy0[sy]; oy < oy_end; ++oy) {
            const uint32_t row_off = s_row_lut[oy];
            for (int sx = 0; sx < GB_W; ++sx) {
                uint16_t& px = reinterpret_cast<uint16_t&>(
                    fb[row_off + s_col_lut[s_dx1[sx] - 1]]);
                px = dim_packed_grid(px);
            }
        }
    }
}

// -- LCD ghosting (targeted per-sprite flicker blend) -------------------------
// Simulates GBC LCD phosphor persistence for games that create transparency
// effects by alternating sprite visibility at 30 Hz.
//
// Design — targeted per-sprite OAM tracking + instant 50/50 blend:
//
//   walnut_cgb.h tracks which OAM entries rendered pixels each frame vs the
//   previous frame.  When an entry alternates rendered/not-rendered on
//   consecutive frames (the 30 Hz flicker pattern), s_sprite_flicker_cnt[s]
//   increments and pixels drawn by that sprite are marked in s_flicker_pixel_curr[].
//
//   apply_lcd_ghosting() blends ONLY those marked pixels at exactly 50/50
//   using (curr + prev) >> 1.  This is perfectly symmetric: ON frame and OFF
//   frame produce IDENTICAL output colour → zero visible oscillation.
//
//   When a non-flickering sprite (Link, BG tiles) draws OVER a position that
//   was previously marked, walnut clears both s_flicker_pixel_curr[i] and
//   s_flicker_pixel_prev[i] at that pixel.  This prevents any ghost bleed
//   at positions now covered by a solid, non-flickering sprite.
//
// Why moving sprites are never blurred:
//   Link renders every frame → rendered_curr == rendered_prev == true every
//   frame → flicker_cnt stays 0 → flickering_this_frame = false → walnut clears
//   the flicker marks at Link's pixels → no blend. ✓
//
// Why transparency sprites blend perfectly from frame 1:
//   A ghost sprite alternates visible/invisible every frame → flickering_this_frame
//   is true on its ON frame → pixel marked in curr → is_flicker=true → instant
//   50/50 blend.  OFF frame: curr=0 but prev=1 (carried) → is_flicker=true →
//   50/50 blend again.  Both frames output (sprite+bg)/2.  No ramp-up delay.
//   No oscillation. ✓
//
// Why camera scroll doesn't kill transparency:
//   Background pixels are never marked as flicker pixels → no blend there.
//   No bulk-change early return needed; the pixel marks gate everything. ✓
//
// Memory (heap, freed on ROM unload):
//   s_prev_fb            — raw previous frame pixels    45,056 bytes
//   s_flicker_pixel_curr — per-pixel flicker flag        23,040 bytes
//   s_flicker_pixel_prev — carries ON-frame flag to OFF  23,040 bytes
//   Static tracking arrays in walnut_cgb.h: 120 bytes BSS.

static uint16_t* s_prev_fb       = nullptr;   // 45 KB — raw previous frame
static bool      s_prev_fb_valid = false;

// s_flicker_pixel_curr / s_flicker_pixel_prev are declared in walnut_cgb.h.
// apply_lcd_ghosting() owns their heap allocations.

// Update per-sprite flicker counters and reset rendered flags for next frame.
// Called once per frame from apply_lcd_ghosting().
static inline void update_sprite_flicker_state() {
    for (int s = 0; s < NUM_SPRITES; ++s) {
        // Detect alternation immediately
        bool alternated = s_sprite_rendered_curr[s] != s_sprite_rendered_prev[s];

        // Advance flicker counter if alternated, otherwise reset
        if (alternated)
            s_sprite_flicker_cnt[s] = (s_sprite_flicker_cnt[s] < 255u)
                                        ? s_sprite_flicker_cnt[s] + 1u : 255u;
        else
            s_sprite_flicker_cnt[s] = 0;

        // Transfer current rendered flag to prev and reset curr for next frame
        s_sprite_rendered_prev[s] = s_sprite_rendered_curr[s];
        s_sprite_rendered_curr[s] = false;
    }
}

inline void apply_lcd_ghosting() {
    if (!g_lcd_ghosting || ult::limitedMemory) return;
    const int total = GB_W * GB_H;

    if (!s_prev_fb) {
        s_prev_fb = static_cast<uint16_t*>(malloc(total * sizeof(uint16_t)));
        if (!s_prev_fb) return;
    }
    if (!s_flicker_pixel_curr) {
        s_flicker_pixel_curr = static_cast<uint8_t*>(calloc(total, 1));
        if (!s_flicker_pixel_curr) return;
    }
    if (!s_flicker_pixel_prev) {
        s_flicker_pixel_prev = static_cast<uint8_t*>(calloc(total, 1));
        if (!s_flicker_pixel_prev) return;
    }

    if (!s_prev_fb_valid) {
        memcpy(s_prev_fb, g_gb_fb, total * sizeof(uint16_t));
        memset(s_flicker_pixel_curr, 0, total);
        memset(s_flicker_pixel_prev, 0, total);
        s_prev_fb_valid = true;
        update_sprite_flicker_state();
        return;
    }

    // Blend pass.
    //
    // For every pixel marked as a flickering-sprite pixel (curr OR prev flag set),
    // output the exact midpoint between this frame and the previous frame:
    //
    //   output = (curr + prev) >> 1
    //
    // Addition is commutative, so ON frame and OFF frame produce identical output:
    //   ON  frame: (sprite + bg)   >> 1 = X
    //   OFF frame: (bg    + sprite) >> 1 = X        ← same X, zero oscillation ✓
    //
    // No ramp-up delay: blend fires at full 50/50 on frame 1.
    // No bulk-change early-return: background pixels are never marked as flicker
    // pixels, so scrolling the camera never affects the blend at sprite positions.
    //
    // Non-flickering sprites (Link, outlines):
    //   walnut clears curr[i] AND prev[i] whenever a non-flickering sprite draws
    //   over a pixel → is_flicker=false instantly → no blend, no ghost trail. ✓
    for (int i = 0; i < total; ++i) {
        const uint16_t curr = g_gb_fb[i];
        const uint16_t prev = s_prev_fb[i];

        if ((s_flicker_pixel_curr[i] | s_flicker_pixel_prev[i]) && curr != prev) {
            if (g_fb_is_rgb565) {
                const uint8_t cr = (uint8_t)((((curr >> 11) & 0x1Fu) + ((prev >> 11) & 0x1Fu)) >> 1);
                const uint8_t cg = (uint8_t)((((curr >>  5) & 0x3Fu) + ((prev >>  5) & 0x3Fu)) >> 1);
                const uint8_t cb = (uint8_t)(( (curr        & 0x1Fu) + ( prev        & 0x1Fu)) >> 1);
                g_gb_fb[i] = (uint16_t)((cr << 11) | (cg << 5) | cb);
            } else {
                const uint8_t cr = (uint8_t)(( (curr        & 0xFu) + ( prev        & 0xFu)) >> 1);
                const uint8_t cg = (uint8_t)((((curr >>  4) & 0xFu) + ((prev >>  4) & 0xFu)) >> 1);
                const uint8_t cb = (uint8_t)((((curr >>  8) & 0xFu) + ((prev >>  8) & 0xFu)) >> 1);
                const uint8_t ca = (uint8_t)(  (curr >> 12) & 0xFu);
                g_gb_fb[i] = (uint16_t)((ca << 12) | (cb << 8) | (cg << 4) | cr);
            }
        }
        
        s_prev_fb[i] = curr;
    }

    update_sprite_flicker_state();

    // Carry this frame's flicker marks to prev, clear curr for next frame.
    // This keeps is_flicker=true on the OFF frame: prev[i] holds the mark
    // that walnut set during the previous ON frame.
    memcpy(s_flicker_pixel_prev, s_flicker_pixel_curr, total);
    memset(s_flicker_pixel_curr, 0, total);
}

// Invalidate ghosting state on ROM switch or reset.
// Resets all sprite-flicker tracking so stale counters from the old game
// cannot bleed into the new one.  Does NOT free heap; keeps allocations live.
inline void reset_lcd_ghosting() {
    s_prev_fb_valid = false;
    memset(s_sprite_rendered_curr, 0, sizeof(s_sprite_rendered_curr));
    memset(s_sprite_rendered_prev, 0, sizeof(s_sprite_rendered_prev));
    memset(s_sprite_flicker_cnt,   0, sizeof(s_sprite_flicker_cnt));
    if (s_prev_fb)            memset(s_prev_fb,            0, GB_W * GB_H * sizeof(uint16_t));
    if (s_flicker_pixel_curr) memset(s_flicker_pixel_curr, 0, GB_W * GB_H);
    if (s_flicker_pixel_prev) memset(s_flicker_pixel_prev, 0, GB_W * GB_H);
}
// Release all ghosting heap memory.  Called from gb_unload_rom() so the
// memory returns to the heap when no game is running.
inline void free_lcd_ghosting() {
    free(s_prev_fb);
    free(s_flicker_pixel_curr);
    free(s_flicker_pixel_prev);
    s_prev_fb            = nullptr;
    s_flicker_pixel_curr = nullptr;
    s_flicker_pixel_prev = nullptr;
    s_prev_fb_valid      = false;
    memset(s_sprite_rendered_curr, 0, sizeof(s_sprite_rendered_curr));
    memset(s_sprite_rendered_prev, 0, sizeof(s_sprite_rendered_prev));
    memset(s_sprite_flicker_cnt,   0, sizeof(s_sprite_flicker_cnt));
}

// -- GB screen render (Ultimate Switch NEON, fully vectorized, contiguous run detection)
// Hot path summary per frame:
//   - Outer loop: 144 GB source rows
//   - RLE scan: groups same-colour source pixels into runs (~2000-4000 total)
//   - Per run: colour conversion fires exactly once (or zero times if IS_PREPACKED)
//   - Per run x output row: NEON vector store, 16–32 pixels in one go when possible
// Template parameters:
//   IS565       — true for CGB games
//   IS_PREPACKED— true for DMG games
template <bool IS565, bool IS_PREPACKED>
static void render_gb_screen_chunk_impl(tsl::gfx::Renderer* renderer,
                                        const int sy_start, const int sy_end) {
    for (int sy = sy_start; sy < sy_end; ++sy) {
        const int oy0 = s_dy0[sy];
        const int oy1 = s_dy1[sy];
        const uint16_t* __restrict__ src = g_gb_fb + sy * GB_W;

        int run_sx  = 0;
        uint16_t run_pix = src[0];

        for (int sx = 1; sx <= GB_W; ++sx) {
            const uint16_t pix = (sx < GB_W) ? src[sx] : static_cast<uint16_t>(~run_pix);
            if (pix == run_pix) [[likely]] continue;

            // Correct horizontal mapping: exclusive s_dx1
            const int ox0   = s_dx0[run_sx];
            const int ox1   = s_dx1[sx - 1];
            const int run_w = ox1 - ox0;

            // Colour conversion: once per run
            uint16_t packed;
            if constexpr (IS_PREPACKED) {
                packed = run_pix;
            } else if constexpr (IS565) {
                packed = rgb565_to_packed(run_pix);
            } else {
                packed = rgb555_to_packed(run_pix);
            }

            uint16x8_t vcolor_lo = vdupq_n_u16(packed);
            uint16x8_t vcolor_hi = vdupq_n_u16(packed);

            for (int oy = oy0; oy < oy1; ++oy) {
                // Base framebuffer pointer for this row
                tsl::Color* framebuffer =
                    static_cast<tsl::Color*>(renderer->getCurrentFramebuffer()) + s_row_lut[oy];
                const uint32_t* __restrict__ cl = s_col_lut + ox0;

                int ox = 0;

                // --- Contiguous memory path ---
                if (cl[0] + run_w - 1 == cl[run_w - 1]) {
                    while (ox + 15 < run_w) {
                        vst1q_u16(reinterpret_cast<uint16_t*>(framebuffer + cl[ox + 0]), vcolor_lo);
                        vst1q_u16(reinterpret_cast<uint16_t*>(framebuffer + cl[ox + 8]), vcolor_hi);
                        ox += 16;
                    }
                    // leftover pixels
                    for (; ox < run_w; ++ox)
                        framebuffer[cl[ox]] = *reinterpret_cast<tsl::Color*>(&packed);

                } else {
                    // --- Scattered memory path ---
                    // 8-pixel unrolled stores
                    const int limit = run_w & ~7; // largest multiple of 8 ≤ run_w
                    for (; ox < limit; ox += 8) {
                        framebuffer[cl[ox + 0]] = *reinterpret_cast<tsl::Color*>(&packed);
                        framebuffer[cl[ox + 1]] = *reinterpret_cast<tsl::Color*>(&packed);
                        framebuffer[cl[ox + 2]] = *reinterpret_cast<tsl::Color*>(&packed);
                        framebuffer[cl[ox + 3]] = *reinterpret_cast<tsl::Color*>(&packed);
                        framebuffer[cl[ox + 4]] = *reinterpret_cast<tsl::Color*>(&packed);
                        framebuffer[cl[ox + 5]] = *reinterpret_cast<tsl::Color*>(&packed);
                        framebuffer[cl[ox + 6]] = *reinterpret_cast<tsl::Color*>(&packed);
                        framebuffer[cl[ox + 7]] = *reinterpret_cast<tsl::Color*>(&packed);
                    }
                    // leftover pixels
                    for (; ox < run_w; ++ox)
                        framebuffer[cl[ox]] = *reinterpret_cast<tsl::Color*>(&packed);
                }
            }

            run_sx  = sx;
            run_pix = pix;
        }
    }
}

// Thin wrapper with the original signature so std::thread(render_gb_screen_chunk, ...)
// works without changes.  The dispatch is one branch per thread launch — entirely
// outside the pixel loop — so the cost is negligible.
static void render_gb_screen_chunk(tsl::gfx::Renderer* renderer,
                                   const bool is565,
                                   const int sy_start, const int sy_end) {
    if (g_fb_is_prepacked)
        render_gb_screen_chunk_impl<false, true>(renderer, sy_start, sy_end);
    else if (is565)
        render_gb_screen_chunk_impl<true,  false>(renderer, sy_start, sy_end);
    else
        render_gb_screen_chunk_impl<false, false>(renderer, sy_start, sy_end);
}

inline void render_gb_screen(tsl::gfx::Renderer* renderer) {
    if (!s_lut_ready)   init_swizzle_lut();
    if (!s_coord_ready) init_coord_lut();
    

    render_gb_screen_chunk(renderer, g_fb_is_rgb565, 0, GB_H);

    // LCD grid overlay — post-pass that darkens the last pixel of every
    // scaled source-pixel block to simulate the dark gap between LCD cells.
    // No-op when g_lcd_grid is false (single branch at function entry).
    render_gb_grid_overlay(renderer);

    //const bool is565 = g_fb_is_rgb565;
    //if (!ult::expandedMemory) {
    //    render_gb_screen_chunk(renderer, is565, 0, GB_H);
    //    return;
    //}
    //
    //const int numThreads = static_cast<int>(ult::numThreads); // should be 4
    //const int chunkSize  = (GB_H + numThreads - 1) / numThreads; // ceil divide
    //
    //for (int i = 0; i < numThreads; ++i) {
    //    const int sy_start = i * chunkSize;
    //    const int sy_end   = std::min(sy_start + chunkSize, GB_H);
    //    
    //    ult::renderThreads[i] = std::thread(
    //        render_gb_screen_chunk,
    //        renderer, is565, sy_start, sy_end
    //    );
    //}
    //
    //for (auto& t : ult::renderThreads)
    //    t.join();
}

// -- Game Boy Color logo below pixel-perfect screen --------------------------
// Only drawn in 2× (pixel-perfect) mode.
// The border always spans the full 2.5× outer frame (VP_X/Y/W/H).
// In 2× mode the game image is 288px tall, leaving a 36px gap at the bottom:
//   gap top    = vp_y() + vp_h() = 144 + 288 = 432
//   gap bottom = VP_Y   + VP_H   = 108 + 360 = 468
//   gap height = 36px → 4px padding each side → font size 18.
inline void render_gbc_logo(tsl::gfx::Renderer* renderer) {
    if (!g_vp_2x) return;

    static constexpr int LOGO_SIZE = 18;
    // gap top=432, gap bottom=468, centred baseline:
    static constexpr int LOGO_Y    = 432 + (468 - 432) / 2 + LOGO_SIZE / 2 - 1;

    static constexpr tsl::Color WHITE = {0xF, 0xF, 0xF, 0xF};

    // In DMG or Native palette mode the hardware colour block is absent — show
    // plain "GAME BOY" only (no rainbow "COLOR" suffix).
    if (g_palette_mode == PaletteMode::DMG || g_palette_mode == PaletteMode::NATIVE) {
        static bool measured_plain = false;
        static u32  w_plain = 0;
        if (!measured_plain) {
            w_plain = renderer->getTextDimensions("GAME BOY", false, LOGO_SIZE).first;
            measured_plain = true;
        }
        const int x = static_cast<int>(FB_W / 2 - w_plain / 2);
        renderer->drawString("GAME BOY", false, x, LOGO_Y, LOGO_SIZE, WHITE);
        return;
    }

    // GBC mode — draw "GAME BOY COLOR" with rainbow letters.
    static constexpr tsl::Color C_RED = {0xF, 0x1, 0x1, 0xF};  // C
    static constexpr tsl::Color C_ORA = {0xF, 0x7, 0x0, 0xF};  // O
    static constexpr tsl::Color C_YEL = {0xF, 0xD, 0x0, 0xF};  // L
    static constexpr tsl::Color C_GRN = {0x0, 0xA, 0x2, 0xF};  // O
    static constexpr tsl::Color C_IND = {0x3, 0x2, 0xC, 0xF};  // R

    static bool measured = false;
    static u32  w_prefix = 0, wC = 0, wO = 0, wL = 0, wO2 = 0, wR = 0;

    if (!measured) {
        // No 'static' needed — the outer 'measured' flag already guarantees this
        // block runs exactly once.  Removing 'static' eliminates six hidden guard
        // variables and their __cxa_guard_acquire/release call sequences from .text.
        // wO2 measures the same glyph ("O") at the same size as wO — reuse it.
        w_prefix = renderer->getTextDimensions("GAME BOY ", false, LOGO_SIZE).first;
        wC       = renderer->getTextDimensions("C",         false, LOGO_SIZE).first;
        wO       = renderer->getTextDimensions("O",         false, LOGO_SIZE).first;
        wL       = renderer->getTextDimensions("L",         false, LOGO_SIZE).first;
        wO2      = wO;  // same glyph, same size — no second call needed
        wR       = renderer->getTextDimensions("R",         false, LOGO_SIZE).first;
        measured = true;
    }

    const u32 total_w = w_prefix + wC + wO + wL + wO2 + wR;
    int x = static_cast<int>(FB_W / 2 - total_w / 2);

    renderer->drawString("GAME BOY ", false, x, LOGO_Y, LOGO_SIZE, WHITE);
    x += static_cast<int>(w_prefix);
    renderer->drawString("C", false, x, LOGO_Y, LOGO_SIZE, C_RED); x += static_cast<int>(wC);
    renderer->drawString("O", false, x, LOGO_Y, LOGO_SIZE, C_ORA); x += static_cast<int>(wO);
    renderer->drawString("L", false, x, LOGO_Y, LOGO_SIZE, C_YEL); x += static_cast<int>(wL);
    renderer->drawString("O", false, x, LOGO_Y, LOGO_SIZE, C_GRN); x += static_cast<int>(wO2);
    renderer->drawString("R", false, x, LOGO_Y, LOGO_SIZE, C_IND);
}

// -- Letterbox fill for 2× pixel-perfect mode ---------------------------------
// Fills the strips between the 2× game image and the fixed outer border.
//
// Previous implementation: drawRectAdaptive → processRectChunk → setPixelBlendDst
// → getPixelOffset per pixel.  That computed the full swizzle formula and a blend
// for every one of ~51,840 letterbox pixels.
//
// Current implementation: precomputed outer LUT scatter-write — identical pattern
// to render_gb_screen_chunk.  Per-pixel cost is one LUT lookup + one addition +
// one direct store, with zero swizzle math and zero blending.
//   Left strip:   40 × 360 = 14,400 px     Right strip: 40 × 360 = 14,400 px
//   Top strip:   320 ×  36 = 11,520 px    Bottom strip: 320 × 36 = 11,520 px
//
// Using a=0xF (fully opaque) instead of the previous a=0xE to allow direct writes.
// The visual difference over any wallpaper is imperceptible for near-black fills.
// No-op in 2.5× mode (g_vp_2x == false).
static inline void fill_letterbox_rect(tsl::gfx::Renderer* renderer,
                                       const int ox0, const int ox1,
                                       const int oy0, const int oy1,
                                       const tsl::Color color) {
    const int w = ox1 - ox0;
    const uint32_t* __restrict__ col = s_outer_col_lut + ox0;

    for (int oy = oy0; oy < oy1; ++oy) {
        const uint32_t row_base = s_outer_row_lut[oy];
        int ox = 0;

        for (; ox + 7 < w; ox += 8) {
            renderer->setPixelAtOffset(row_base + col[ox + 0], color);
            renderer->setPixelAtOffset(row_base + col[ox + 1], color);
            renderer->setPixelAtOffset(row_base + col[ox + 2], color);
            renderer->setPixelAtOffset(row_base + col[ox + 3], color);
            renderer->setPixelAtOffset(row_base + col[ox + 4], color);
            renderer->setPixelAtOffset(row_base + col[ox + 5], color);
            renderer->setPixelAtOffset(row_base + col[ox + 6], color);
            renderer->setPixelAtOffset(row_base + col[ox + 7], color);
        }
        for (; ox < w; ++ox)
            renderer->setPixelAtOffset(row_base + col[ox], color);
    }
}

inline void render_gb_letterbox(tsl::gfx::Renderer* renderer) {
    if (!g_vp_2x) return;
    if (!s_outer_lut_ready) init_outer_lut();

    static constexpr uint16_t PACKED_LB = 0xE111u;
    const tsl::Color LB_COLOR = renderer->a(PACKED_LB);

    // 2× mode inner viewport relative to outer border frame
    static constexpr int ix = 40;
    static constexpr int iy = 36;
    static constexpr int iw = 320;
    static constexpr int ih = 288;

    fill_letterbox_rect(renderer, 0,       ix,       0,      VP_H, LB_COLOR); // left
    fill_letterbox_rect(renderer, ix + iw, VP_W,     0,      VP_H, LB_COLOR); // right
    fill_letterbox_rect(renderer, ix,      ix + iw,  0,      iy,   LB_COLOR); // top
    fill_letterbox_rect(renderer, ix,      ix + iw,  iy + ih, VP_H, LB_COLOR); // bottom
}

// -- Border around the viewport -----------------------------------------------
// Always drawn at the fixed 2.5× outer frame (VP_X/Y/W/H) regardless of scale.
// In 2× mode this creates the letterbox frame that encloses the game image,
// fills, and logo as one cohesive unit.
inline void render_gb_border(tsl::gfx::Renderer* renderer) {
    static constexpr tsl::Color BORDER{0x3, 0x3, 0x3, 0xF};
    renderer->drawRect(VP_X - 1, VP_Y - 1,    VP_W + 2, 1,    BORDER);  // top
    renderer->drawRect(VP_X - 1, VP_Y + VP_H, VP_W + 2, 1,    BORDER);  // bottom
    renderer->drawRect(VP_X - 1, VP_Y,        1,        VP_H, BORDER);  // left
    renderer->drawRect(VP_X + VP_W, VP_Y,     1,        VP_H, BORDER);  // right
}