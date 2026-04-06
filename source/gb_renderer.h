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
 *       s_outer_col_lut[ox]  - swizzle contribution of output column (400 u32s = 1.6 KB)
 *       s_outer_row_lut[oy]  - swizzle contribution of output row    (360 u32s = 1.4 KB)
 *   Total LUT: ~3 KB static.  s_col_lut / s_row_lut are offset pointers into
 *   the outer arrays covering the inner 2× viewport — no separate allocation.
 *   Verified algebraically against the full formula.
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

// ── Fixed 2× pixel-perfect viewport constants ─────────────────────────────────
// Overlay always renders at integer 2× scale (320×288, pixel-perfect).
// VP_X/VP_Y/VP_W/VP_H in gb_core.h are the outer border dimensions and
// still used for letterbox sizing and static array bounds.
static constexpr int VP2_X = 64;                        // (448 - 320) / 2
static constexpr int VP2_W = 320;
static constexpr int VP2_H = 288;
static constexpr int VP2_Y = VP_Y + (VP_H - VP2_H) / 2; // centred on outer border

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

// Row-rendering Y offset for free overlay mode.
// Set to -(int)OVL_FREE_TOP_TRIM (= -107) at the top of GBOverlayElement::draw()
// when g_overlay_free_mode is true; reset to 0 otherwise.
// Must be applied to every VP_Y-based coordinate passed to render functions.
static int g_render_y_offset = 0;

// -- Shared swizzle LUT construction helpers ----------------------------------
// The block-linear col_part(x) / row_part(y) formula is identical for every
// LUT in the project.  Centralising it here means the formula lives exactly
// once; init_outer_lut and init_win_luts all call these instead of repeating
// the same loop body.
//
// build_col_lut: fills dst[0..count) with col_part(start_x + i).
// build_row_lut: fills dst[0..count) with row_part(start_y + i, owv).
//
// Both are static inline — the compiler inlines the small loop into each
// caller and the helpers produce zero additional .text symbols.
static inline void build_col_lut(uint32_t* dst, int start_x, int count) {
    for (int i = 0; i < count; ++i) {
        const uint32_t x = static_cast<uint32_t>(start_x + i);
        dst[i] = (((x >> 5u) << 3u) << 9u)
               + ((x & 16u) << 3u)
               + ((x &  8u) << 1u)
               +  (x &  7u);
    }
}
static inline void build_row_lut(uint32_t* dst, int start_y, int count, uint32_t owv) {
    for (int i = 0; i < count; ++i) {
        const uint32_t y = static_cast<uint32_t>(start_y + i);
        dst[i] = ((((y & 127u) >> 4u) + ((y >> 7u) * owv)) << 9u)
               + ((y &  8u) << 5u)
               + ((y &  6u) << 4u)
               + ((y &  1u) << 3u);
    }
}

// LUTs indexed by viewport-relative coordinates:
//   s_col_lut[ox]  for ox in [0, VP2_W)  ->  col_part(VP2_X + ox)
//   s_row_lut[oy]  for oy in [0, VP2_H)  ->  row_part(VP2_Y + oy)
//
// These are NOT separate arrays.  Because VP2_X - VP_X = 40 and
// VP2_Y - VP_Y = 36, the inner-viewport entries are an exact sub-range of
// the outer LUTs:
//   s_col_lut[ox] == s_outer_col_lut[ox + 40]  for all ox in [0, VP2_W)
//   s_row_lut[oy] == s_outer_row_lut[oy + 36]  for all oy in [0, VP2_H)
//
// Storing them as offset pointers into s_outer_col/row_lut eliminates the
// former 320+288 = 608-entry (2,432-byte) BSS duplication.  All call sites
// use identical array-index syntax; the compiler loads the pointer once into
// a register before any inner loop — zero per-pixel overhead.
static const uint32_t* s_col_lut = nullptr;  // = s_outer_col_lut + (VP2_X - VP_X)
static const uint32_t* s_row_lut = nullptr;  // = s_outer_row_lut + (VP2_Y - VP_Y)

// Outer LUTs cover the fixed outer viewport frame (VP_X..VP_X+VP_W, VP_Y..VP_Y+VP_H).
// Built once by init_outer_lut(); also initialises s_col_lut and s_row_lut above.
// Used by render_gb_letterbox and (via s_col/row_lut) by render_gb_screen_chunk.
static uint32_t s_outer_col_lut[VP_W];   // col_part(VP_X + ox), ox = 0..VP_W-1
static uint32_t s_outer_row_lut[VP_H];   // row_part(VP_Y + oy), oy = 0..VP_H-1
static bool     s_outer_lut_ready = false;

static void init_outer_lut() {
    // Track the last g_render_y_offset the row LUT was built for.
    // In fixed overlay mode this is always 0.
    // In free overlay mode it varies as the user repositions the overlay
    // (g_render_y_offset = g_ovl_free_pos_y - OVL_FREE_TOP_TRIM, range
    // -OVL_FREE_TOP_TRIM..0 = -84..0).  Without this check the LUT is
    // built once at offset 0 and never reflects the new base row, so the
    // GB screen blit and letterbox stay frozen at the original position
    // while every other element (wallpaper, buttons, logo) correctly shifts.
    //
    // Rebuilding costs: ~400 col + ~360 row iterations at ~5 ns each ≈ 4 µs.
    // Only fires when the offset actually changes — zero cost in steady state.
    static int s_last_y_offset = INT_MIN;  // INT_MIN forces the very first build
    if (s_outer_lut_ready && g_render_y_offset == s_last_y_offset)
        return;
    s_last_y_offset = g_render_y_offset;

    build_col_lut(s_outer_col_lut, VP_X, VP_W);
    // In free overlay mode g_render_y_offset == (g_ovl_free_pos_y - OVL_FREE_TOP_TRIM),
    // shifting the LUT base from VP_Y=108 down toward VP_X=24 at the extremes.
    // The inner-viewport pointer offset (VP2_Y - VP_Y = 36) is unchanged.
    build_row_lut(s_outer_row_lut, VP_Y + g_render_y_offset, VP_H, OWV);
    // Derive inner-viewport pointers from the outer arrays — no separate allocation.
    s_col_lut = s_outer_col_lut + (VP2_X - VP_X);   // +40
    s_row_lut = s_outer_row_lut + (VP2_Y - VP_Y);   // +36
    s_outer_lut_ready = true;
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
//
// Exact same result as:
//   ch' = ch - (ch >> 2)
// for each 4-bit RGB nibble, but done in parallel with nibble-safe masked
// arithmetic instead of extracting / repacking each channel separately.
//
// RGB mask = 0x0FFF (keep alpha nibble untouched)
// Quarter-per-channel is computed with a nibble-local right shift:
//   (rgb >> 2) & 0x0333
//
// This avoids cross-nibble bleed while producing the exact same values as the
// original per-channel implementation.

// Dim a packed RGBA4444 pixel to ~75 % brightness, alpha kept.
// Exact same as: ch' = ch - (ch >> 2) per 4-bit RGB nibble
static inline uint16_t dim_packed_grid(const uint16_t p) {
    const uint16_t rgb = p & 0x0FFFu;
    return static_cast<uint16_t>((p & 0xF000u) | (rgb - ((rgb >> 2) & 0x0333u)));
}

// Vectorized version for NEON (horizontal / vertical)
static inline uint16x8_t dim_packed_grid_vec(uint16x8_t px) {
    const uint16x8_t mask = vdupq_n_u16(0x0FFFu);     // RGB mask
    const uint16x8_t rgb  = vandq_u16(px, mask);
    const uint16x8_t dim  = vsubq_u16(rgb, vandq_u16(vshrq_n_u16(rgb, 2), vdupq_n_u16(0x0333u)));
    return vbslq_u16(mask, dim, px); // keep alpha intact
}

// Shared NEON fill helper — writes packed colour to destination columns [ox0, ox1)
// via a precomputed swizzle LUT.
//
// The block-linear swizzle formula guarantees that lut[8k .. 8k+7] are always
// consecutive (+0..+7), so vst1q_u16 at any 8-aligned ox is a valid sequential
// store of 8 framebuffer pixels.  A scalar prefix loop aligns to the next
// 8-column tile boundary; a scalar suffix handles the remaining tail.
//
// Kept as a single 8-pixel NEON loop intentionally.  A 16-pixel variant
// (two vst1q_u16 per iteration) was tested and increased binary size because
// this function is static inline and is instantiated at 24+ call sites across
// 12 win_blit_rows template specialisations.  The 8-pixel body is the right
// balance: one store per tile, minimal code per inline expansion.
//
// Used by both the overlay renderer (s_col_lut) and the windowed renderer
// (s_win_col) so the logic lives exactly once.
static inline void neon_lut_fill(uint16_t* __restrict__ row_ptr,
                                  const uint32_t* __restrict__ lut,
                                  int ox0, int ox1, uint16_t packed) {
    const uint16x8_t vpk = vdupq_n_u16(packed);
    int ox = ox0;
    while (ox < ox1 && (ox & 7)) { row_ptr[lut[ox]] = packed; ++ox; }
    for (; ox + 8 <= ox1; ox += 8)
        vst1q_u16(row_ptr + lut[ox], vpk);
    while (ox < ox1) { row_ptr[lut[ox]] = packed; ++ox; }
}

// Multi-row NEON fill — writes packed colour to SCALE destination rows
// simultaneously at column positions [ox0, ox1) via a precomputed swizzle LUT.
//
// Core insight: each column LUT entry lut[ox] is loaded ONCE and reused for
// all SCALE row stores.  When neon_lut_fill is called SCALE times in a loop
// (once per dy), the same lut[ox] is loaded from memory SCALE times redundantly.
// This function eliminates that redundancy by inverting the loop order:
//   OLD: for dy: for tile ox: load lut[ox]; store row[dy]+lut[ox]  ← SCALE loads/tile
//   NEW: for tile ox: load lut[ox] ONCE; for dy: store row[dy]+lut[ox]  ← 1 load/tile
//
// SCALE is a compile-time template parameter — the inner dy loop is fully
// unrolled by the compiler.  At SCALE=5 the NEON body becomes 5 independent
// vst1q_u16 instructions per tile, which the CPU can pipeline freely.
//
// rows[0..SCALE-1] must be precomputed destination row pointers for the
// current source row sy: rows[dy] = fb + s_win_row[sy*SCALE + dy].
// Precomputing them once per source row (outside the RLE run loop) amortises
// the row-LUT loads across all runs on that row.
template <int SCALE>
static inline void neon_fill_multirow(uint16_t* const* __restrict__ rows,
                                       const uint32_t* __restrict__ lut,
                                       int ox0, int ox1, uint16_t packed) {
    const uint16x8_t vpk = vdupq_n_u16(packed);
    int ox = ox0;
    // Scalar prefix: align to the next 8-column tile boundary.
    while (ox < ox1 && (ox & 7)) {
        const uint32_t c = lut[ox++];
        for (int dy = 0; dy < SCALE; ++dy) rows[dy][c] = packed;
    }
    // NEON main loop: load column offset once, vst1q_u16 to all SCALE rows.
    // The dy loop is fully unrolled at compile time; at SCALE=5 the compiler
    // emits 5 independent stores per tile with a single preceding LUT load.
    for (; ox + 8 <= ox1; ox += 8) {
        const uint32_t c = lut[ox];
        for (int dy = 0; dy < SCALE; ++dy)
            vst1q_u16(rows[dy] + c, vpk);
    }
    // Scalar tail: remaining 1–7 pixels past the last full tile.
    while (ox < ox1) {
        const uint32_t c = lut[ox++];
        for (int dy = 0; dy < SCALE; ++dy) rows[dy][c] = packed;
    }
}


// Post-pass: darken grid-line pixels in the overlay viewport.
//
// Runs after render_gb_screen_chunk — s_outer_lut_ready is guaranteed true at that point.
//
// At 2×: ≈ 144×320 + 144×1×160 ≈ 69 K writes per frame when enabled.
// < 0.1 ms on the Switch CPU — negligible.
inline void render_gb_grid_overlay(tsl::gfx::Renderer* renderer) {
    if (!g_lcd_grid) return;

    auto* fb = reinterpret_cast<uint16_t*>(renderer->getCurrentFramebuffer());

    // At fixed 2× scale: each GB source pixel sx maps to dest cols sx*2 and sx*2+1.
    // Grid column = last dest col of each source pixel = sx*2+1.
    // Grid row    = last dest row of each source pixel = sy*2+1.
    // The first dest row (sy*2) is the non-grid row for vertical lines.

    // Precompute vertical offsets — static cache: s_col_lut is write-once
    // after s_outer_lut_ready flips, so these offsets never change at runtime.
    // Eliminates a 160-iteration loop and 320 bytes of stack every frame.
    // uint16_t is safe: max col_part value for this viewport is 45207 < 65535.
    static uint16_t v_cols[GB_W];
    static bool     v_cols_ready = false;
    if (!v_cols_ready) {
        for (int sx = 0; sx < GB_W; ++sx)
            v_cols[sx] = static_cast<uint16_t>(s_col_lut[sx * 2 + 1]);
        v_cols_ready = true;
    }

    // ── Horizontal lines: darken last dest row of each GB source row
    for (int sy = 0; sy < GB_H; ++sy) {
        uint16_t* row_ptr = fb + s_row_lut[sy * 2 + 1];
        for (int ox = 0; ox < VP2_W; ox += 8) {
            uint16x8_t px = vld1q_u16(&row_ptr[s_col_lut[ox]]);
            vst1q_u16(&row_ptr[s_col_lut[ox]], dim_packed_grid_vec(px));
        }
    }

    // ── Vertical lines: darken last dest col of each GB source col,
    //    only on the first dest row (sy*2) — the horizontal pass covers sy*2+1.
    for (int sy = 0; sy < GB_H; ++sy) {
        uint16_t* row_ptr = fb + s_row_lut[sy * 2];
        for (int sx = 0; sx < GB_W; sx += 8) {
            uint16_t temp[8];
            for (int i = 0; i < 8; ++i)
                temp[i] = row_ptr[v_cols[sx + i]];
            uint16x8_t px = vld1q_u16(temp);
            vst1q_u16(temp, dim_packed_grid_vec(px));
            for (int i = 0; i < 8; ++i)
                row_ptr[v_cols[sx + i]] = temp[i];
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
    // NEON blend pass — 8 pixels per iteration.
    //
    // Architecture:
    //   • The session-constant g_fb_is_rgb565 branch is pulled OUTSIDE the pixel
    //     loop so the compiler emits two fully-specialized inner loops with zero
    //     in-loop branching.  On Cortex-A57 (in-order) this matters.
    //
    //   • Blend condition per 8-pixel vector:
    //       has_flicker = (flicker_curr | flicker_prev) != 0  (per pixel)
    //       ne          = curr != prev
    //       blend_mask  = has_flicker & ne               → 0xFFFF or 0x0000 per lane
    //
    //   • 50/50 blend via vhaddq_u16 (NEON halving-add):
    //       channel_avg = (a + b) >> 1  — no overflow, bit-exact match to scalar.
    //
    //   • vbslq_u16(mask, blended, curr) selects blended where mask=0xFFFF,
    //     original curr where mask=0x0000.
    //
    //   • s_prev_fb is written with the ORIGINAL curr8 (captured before any blend),
    //     matching the scalar invariant: prev always holds raw source pixels.
    //
    // GB_W*GB_H = 160*144 = 23,040 = 2,880 × 8 — no scalar tail in practice,
    // but the tail loop is kept for correctness if those constants ever change.
    const int n8 = (total / 8) * 8;

    if (g_fb_is_rgb565) {
        // RGB565 layout: R[15:11] G[10:5] B[4:0]
        // Mask constants hoisted outside the loop — one movi each, loaded
        // into NEON registers once and reused across all 2,880 iterations.
        const uint16x8_t mask_g = vdupq_n_u16(0x3Fu);
        const uint16x8_t mask_b = vdupq_n_u16(0x1Fu);
        for (int i = 0; i < n8; i += 8) {
            const uint16x8_t curr8 = vld1q_u16(g_gb_fb  + i);
            const uint16x8_t prev8 = vld1q_u16(s_prev_fb + i);

            // Blend condition mask
            const uint8x8_t  fc  = vld1_u8(s_flicker_pixel_curr + i);
            const uint8x8_t  fp  = vld1_u8(s_flicker_pixel_prev + i);
            const uint16x8_t fany         = vmovl_u8(vorr_u8(fc, fp));
            const uint16x8_t has_flicker  = vtstq_u16(fany, vdupq_n_u16(0x00FFu));
            const uint16x8_t ne           = vmvnq_u16(vceqq_u16(curr8, prev8));
            const uint16x8_t blend_mask   = vandq_u16(has_flicker, ne);

            // Channel extract + halving-add (50/50 blend, no overflow)
            const uint16x8_t cr = vhaddq_u16(vshrq_n_u16(curr8, 11),
                                              vshrq_n_u16(prev8, 11));
            const uint16x8_t cg = vhaddq_u16(
                vandq_u16(vshrq_n_u16(curr8, 5), mask_g),
                vandq_u16(vshrq_n_u16(prev8, 5), mask_g));
            const uint16x8_t cb = vhaddq_u16(
                vandq_u16(curr8, mask_b),
                vandq_u16(prev8, mask_b));
            const uint16x8_t blended = vorrq_u16(vshlq_n_u16(cr, 11),
                                       vorrq_u16(vshlq_n_u16(cg, 5), cb));

            vst1q_u16(g_gb_fb  + i, vbslq_u16(blend_mask, blended, curr8));
            vst1q_u16(s_prev_fb + i, curr8);   // store original curr, not blended
        }
    } else {
        // DMG prepacked (RGBA4444) or non-prepacked (GB15):
        // Both treated as 4-bit nibble channels r4|g4<<4|b4<<8|a4<<12.
        // Alpha comes from curr only — identical to the scalar path.
        const uint16x8_t mask4 = vdupq_n_u16(0x000Fu);
        for (int i = 0; i < n8; i += 8) {
            const uint16x8_t curr8 = vld1q_u16(g_gb_fb  + i);
            const uint16x8_t prev8 = vld1q_u16(s_prev_fb + i);

            const uint8x8_t  fc  = vld1_u8(s_flicker_pixel_curr + i);
            const uint8x8_t  fp  = vld1_u8(s_flicker_pixel_prev + i);
            const uint16x8_t fany         = vmovl_u8(vorr_u8(fc, fp));
            const uint16x8_t has_flicker  = vtstq_u16(fany, vdupq_n_u16(0x00FFu));
            const uint16x8_t ne           = vmvnq_u16(vceqq_u16(curr8, prev8));
            const uint16x8_t blend_mask   = vandq_u16(has_flicker, ne);

            const uint16x8_t cr = vhaddq_u16(vandq_u16(curr8,                   mask4),
                                              vandq_u16(prev8,                   mask4));
            const uint16x8_t cg = vhaddq_u16(vandq_u16(vshrq_n_u16(curr8, 4), mask4),
                                              vandq_u16(vshrq_n_u16(prev8, 4), mask4));
            const uint16x8_t cb = vhaddq_u16(vandq_u16(vshrq_n_u16(curr8, 8), mask4),
                                              vandq_u16(vshrq_n_u16(prev8, 8), mask4));
            const uint16x8_t ca = vshrq_n_u16(curr8, 12);   // alpha from curr, no blend
            const uint16x8_t blended = vorrq_u16(cr,
                                       vorrq_u16(vshlq_n_u16(cg, 4),
                                       vorrq_u16(vshlq_n_u16(cb, 8), vshlq_n_u16(ca, 12))));

            vst1q_u16(g_gb_fb  + i, vbslq_u16(blend_mask, blended, curr8));
            vst1q_u16(s_prev_fb + i, curr8);
        }
    }

    // Scalar tail — empty when total == 23040 (GB_W*GB_H), kept for robustness.
    for (int i = n8; i < total; ++i) {
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
    // Pointer swap instead of memcpy+memset: old curr becomes new prev,
    // old prev becomes new curr and is zeroed.  Eliminates the 23 KB copy.
    uint8_t* tmp      = s_flicker_pixel_prev;
    s_flicker_pixel_prev = s_flicker_pixel_curr;
    s_flicker_pixel_curr = tmp;
    memset(s_flicker_pixel_curr, 0, total);
}

// Invalidate ghosting state on ROM switch or reset.
// Resets all sprite-flicker tracking so stale counters from the old game
// cannot bleed into the new one.  Does NOT free heap; keeps allocations live.
//
// Buffer memsets are intentionally omitted: s_prev_fb_valid = false causes
// apply_lcd_ghosting() to overwrite s_prev_fb via memcpy and zero both
// flicker pixel buffers on the very first frame it runs after a reset,
// before any blend logic executes.  Zeroing them here would be redundant.
inline void reset_lcd_ghosting() {
    s_prev_fb_valid = false;
    memset(s_sprite_rendered_curr, 0, sizeof(s_sprite_rendered_curr));
    memset(s_sprite_rendered_prev, 0, sizeof(s_sprite_rendered_prev));
    memset(s_sprite_flicker_cnt,   0, sizeof(s_sprite_flicker_cnt));
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

// -- GB screen render (fixed 2× integer scale, NEON-accelerated)
// Hot path per frame:
//   - Outer loop: GB_H source rows; each maps to exactly 2 dest rows (sy*2, sy*2+1).
//   - RLE scan groups identical source pixels into runs (~2000–4000 total).
//   - Colour conversion fires once per run via compile-time if constexpr — no
//     runtime branch in the inner loop.  Three specialisations (IS565, PREPACKED,
//     neither) are dispatched once from render_gb_screen based on session flags.
//     Previous design used runtime bools to keep a single .text copy; that tradeoff
//     was correct when the function was launched from 4 threads (4× code bloat).
//     Post-threading-removal it is called exactly once per frame, so 3 small
//     specialisations for ~2000–4000 fewer comparison instructions per frame
//     is the right tradeoff on Cortex-A57 (in-order; every saved instruction
//     matters more than on an OOO core).
//   - Contiguous-tile path: when a run stays within one 8-column swizzle tile,
//     cl[ox]=cl[0]+ox so no per-pixel LUT load is needed; compiler emits stp/str pairs.
//   - Scattered path: inlined 2-row loop loads each column LUT entry once and
//     vst1q_u16's to both destination rows — half the LUT loads of two neon_lut_fill calls.
template <bool IS565, bool PREPACKED>
static void render_gb_screen_chunk(tsl::gfx::Renderer* renderer,
                                   const int sy_start, const int sy_end) {
    uint16_t* __restrict__ fb = static_cast<uint16_t*>(renderer->getCurrentFramebuffer());

    for (int sy = sy_start; sy < sy_end; ++sy) {
        const int oy0 = sy * 2;           // always exactly 2 dest rows at 2× scale
        const uint16_t* __restrict__ src = g_gb_fb + sy * GB_W;

        int      run_sx  = 0;
        uint16_t run_pix = src[0];

        for (int sx = 1; sx <= GB_W; ++sx) {
            const uint16_t pix = (sx < GB_W) ? src[sx] : static_cast<uint16_t>(~run_pix);
            if (pix == run_pix) [[likely]] continue;

            // At 2× each source pixel maps to 2 dest columns: ox = src_x * 2.
            const int ox0   = run_sx * 2;
            const int ox1   = sx * 2;
            const int run_w = ox1 - ox0;

            // Colour conversion: compile-time specialised — zero runtime branch cost.
            uint16_t packed;
            if constexpr (IS565)       packed = rgb565_to_packed(run_pix);
            else if constexpr (PREPACKED) packed = run_pix;
            else                       packed = rgb555_to_packed(run_pix);

            const uint32_t* __restrict__ cl = s_col_lut + ox0;

            // Contiguity check — pure arithmetic, no LUT load.
            // Within the block-linear layout, columns ox0..ox1-1 are guaranteed
            // sequential in the framebuffer if and only if they all fall inside
            // the same 8-column swizzle tile (i.e. the high bits of ox0 and
            // ox1-1 match when the low 3 bits are masked off).
            // VP2_X=64 is a multiple of 8, so the viewport-relative tile index
            // equals the absolute tile index — the arithmetic is exact.
            if ((ox0 & ~7) == ((ox1 - 1) & ~7)) {
                // Same tile: cl[ox] = cl[0] + ox for every ox in [0, run_w).
                // Use base-pointer + stride — no LUT load per pixel.
                // run_w ≤ 8; compiler emits stp/str pairs, may auto-vectorize.
                uint16_t* const p0 = fb + s_row_lut[oy0]     + cl[0];
                uint16_t* const p1 = fb + s_row_lut[oy0 + 1] + cl[0];
                for (int ox = 0; ox < run_w; ++ox) {
                    p0[ox] = packed;
                    p1[ox] = packed;
                }
            } else {
                // --- Inlined 2-row scatter: column LUT loaded once per tile ---
                // Replacing two neon_lut_fill calls (which each load lut[ox])
                // with a single loop that loads lut[ox] once and stores to both
                // destination rows.  Halves column LUT loads for multi-tile runs.
                {
                    const uint16x8_t vpk = vdupq_n_u16(packed);
                    uint16_t* const r0 = fb + s_row_lut[oy0];
                    uint16_t* const r1 = fb + s_row_lut[oy0 + 1];
                    int ox = 0;
                    while (ox < run_w && ((ox0 + ox) & 7)) {
                        const uint32_t c = cl[ox++];
                        r0[c] = packed; r1[c] = packed;
                    }
                    for (; ox + 8 <= run_w; ox += 8) {
                        const uint32_t c = cl[ox];
                        vst1q_u16(r0 + c, vpk);
                        vst1q_u16(r1 + c, vpk);
                    }
                    while (ox < run_w) {
                        const uint32_t c = cl[ox++];
                        r0[c] = packed; r1[c] = packed;
                    }
                }
            }

            run_sx  = sx;
            run_pix = pix;
        }
    }
}

inline void render_gb_screen(tsl::gfx::Renderer* renderer) {
    init_outer_lut();  // rebuilds row LUT whenever g_render_y_offset changes

    // Always single-thread.
    //
    // The overlay renders at a fixed 2× scale: 160×144 → 320×288 = 92,160
    // destination pixels.  With NEON scatter-writes and a precomputed swizzle
    // LUT, the full blit completes in ~50–150 µs on Cortex-A57.
    //
    // Per-frame std::thread construction destroys the old thread object and
    // creates a new OS thread (svcCreateThread + svcStartThread) on every call.
    // On the Switch that syscall pair costs ~50–150 µs PER THREAD.  Across 4
    // threads that is 200–600 µs of overhead just to save ~30–50 µs of compute
    // — a net loss every single frame.
    //
    // The expandedMemory flag reflects heap tier, not render workload; the GB
    // framebuffer is always 160×144 regardless of ROM size or memory config.
    // There is no threshold at which threading helps for this fixed 2× blit.
    //
    // The wallpaper render pool in tesla.hpp (drawWallpaper, drawRectAdaptive)
    // handles genuinely large workloads (322 K pixels with per-pixel alpha)
    // where 4-way parallelism is worthwhile.  Those paths are unaffected.
    //
    // Dispatch to the correct compile-time specialisation based on session-constant
    // pixel format.  The branch is evaluated once here, not inside the inner loop.
    if (g_fb_is_rgb565)
        render_gb_screen_chunk<true,  false>(renderer, 0, GB_H);
    else if (g_fb_is_prepacked)
        render_gb_screen_chunk<false, true> (renderer, 0, GB_H);
    else
        render_gb_screen_chunk<false, false>(renderer, 0, GB_H);

    // LCD grid overlay — post-pass darkening inter-pixel gaps.
    // No-op when g_lcd_grid is false.
    render_gb_grid_overlay(renderer);
}

// -- Game Boy Color logo below pixel-perfect screen --------------------------
// Only drawn in 2× (pixel-perfect) mode.
// The border always spans the full 2.5× outer frame (VP_X/Y/W/H).
// In 2× mode the game image is 288px tall, leaving a 36px gap at the bottom:
//   gap top    = VP2_Y + VP2_H = 144 + 288 = 432
//   gap bottom = VP_Y  + VP_H  = 108 + 360 = 468
//   gap height = 36px → 4px padding each side → font size 18.
inline void render_gbc_logo(tsl::gfx::Renderer* renderer,
                            tsl::Color text_col = {0xF, 0xF, 0xF, 0xF}) {
    static constexpr int LOGO_SIZE = 18;
    // gap top=432, gap bottom=468, centred baseline — shifted by g_render_y_offset
    // in free overlay mode (e.g. 458 - 107 = 351 in free mode).
    const int LOGO_Y = 432 + (468 - 432) / 2 + LOGO_SIZE / 2 - 1 + g_render_y_offset;

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
        renderer->drawString("GAME BOY", false, x, LOGO_Y, LOGO_SIZE, text_col);
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

    renderer->drawString("GAME BOY ", false, x, LOGO_Y, LOGO_SIZE, text_col);
    x += static_cast<int>(w_prefix);
    renderer->drawString("C", false, x, LOGO_Y, LOGO_SIZE, C_RED); x += static_cast<int>(wC);
    renderer->drawString("O", false, x, LOGO_Y, LOGO_SIZE, C_ORA); x += static_cast<int>(wO);
    renderer->drawString("L", false, x, LOGO_Y, LOGO_SIZE, C_YEL); x += static_cast<int>(wL);
    renderer->drawString("O", false, x, LOGO_Y, LOGO_SIZE, C_GRN); x += static_cast<int>(wO2);
    renderer->drawString("R", false, x, LOGO_Y, LOGO_SIZE, C_IND);
}

// -- Letterbox fill for 2× pixel-perfect mode ---------------------------------
// Fills the four strips between the 2× game image and the fixed outer border.
//
//   Left strip:   40 × 360 = 14,400 px     Right strip: 40 × 360 = 14,400 px
//   Top strip:   320 ×  36 = 11,520 px    Bottom strip: 320 × 36 = 11,520 px
//   Total: 51,840 pixels per frame.
//
// Uses neon_lut_fill with a direct framebuffer pointer — same pattern as
// render_gb_screen_chunk.  One vst1q_u16 per 8 pixels; no renderer API call
// overhead per pixel.
//   51,840 former setPixelAtOffset calls/frame → 6,480 vst1q_u16s.
//
// Precondition: all ox0 values (0, ix=40, ix+iw=360) are multiples of 8,
// so neon_lut_fill's tile-alignment invariant holds throughout.
// VP_X=24 is also a multiple of 8 — absolute column alignment is preserved.
static inline void fill_letterbox_rect(uint16_t* __restrict__ fb,
                                       const int ox0, const int ox1,
                                       const int oy0, const int oy1,
                                       uint16_t packed) {
    const uint32_t* __restrict__ col = s_outer_col_lut + ox0;
    const int w = ox1 - ox0;
    for (int oy = oy0; oy < oy1; ++oy)
        neon_lut_fill(fb + s_outer_row_lut[oy], col, 0, w, packed);
}

inline void render_gb_letterbox(tsl::gfx::Renderer* renderer) {
    init_outer_lut();  // rebuilds row LUT whenever g_render_y_offset changes

    auto* fb = reinterpret_cast<uint16_t*>(renderer->getCurrentFramebuffer());

    // Static default fill — theme-aware override is applied inline in
    // gb_overlay.hpp after this call, where all globals are in scope.
    static constexpr uint16_t PACKED_LB = 0xD000u;

    static constexpr int ix = 40;
    static constexpr int iy = 36;
    static constexpr int iw = 320;
    static constexpr int ih = 288;

    fill_letterbox_rect(fb, 0,       ix,       0,       VP_H, PACKED_LB); // left
    fill_letterbox_rect(fb, ix + iw, VP_W,     0,       VP_H, PACKED_LB); // right
    fill_letterbox_rect(fb, ix,      ix + iw,  0,       iy,   PACKED_LB); // top
    fill_letterbox_rect(fb, ix,      ix + iw,  iy + ih, VP_H, PACKED_LB); // bottom
}

// -- Border around the viewport -----------------------------------------------
// Theme-aware version (using g_ovl_bdr_col) is inlined in gb_overlay.hpp.
inline void render_gb_border(tsl::gfx::Renderer* renderer) {
    static constexpr tsl::Color BORDER{0x3, 0x3, 0x3, 0xF};
    renderer->drawRect(VP_X - 1, VP_Y - 1 + g_render_y_offset,    VP_W + 2, 1,    BORDER);
    renderer->drawRect(VP_X - 1, VP_Y + VP_H + g_render_y_offset, VP_W + 2, 1,    BORDER);
    renderer->drawRect(VP_X - 1, VP_Y + g_render_y_offset,        1,        VP_H, BORDER);
    renderer->drawRect(VP_X + VP_W, VP_Y + g_render_y_offset,     1,        VP_H, BORDER);
}