/********************************************************************************
 * File: gb_renderer.h
 * Description:
 *   Inline renderer that scales the 160×144 RGB555 GB framebuffer into the
 *   400×360 overlay viewport at position (24, 180) using nearest-neighbour
 *   integer scaling.  Each source pixel maps to either a 2×2, 2×3, 3×2, or
 *   3×3 destination rectangle (the mix achieves the exact 2.5× scale).
 *
 *   All rendering is done through tsl::gfx::Renderer::drawRect(), which is
 *   the standard drawing primitive available in libultrahand / libtesla.
 *
 *   Performance note:
 *     This calls drawRect() up to 23,040 times per frame (160×144).  On the
 *     Switch this is typically fast enough, but if frame-rate drops are seen,
 *     consider optimising with horizontal run-length merging:
 *       for each output row, merge adjacent same-colour rects into one wider rect.
 ********************************************************************************/

#pragma once

#include <tesla.hpp>
#include "gb_core.h"

// ── Colour conversion ─────────────────────────────────────────────────────────
// DMG pixels are RGB555: bits[4:0]=R, bits[9:5]=G, bits[14:10]=B
// CGB pixels (from Walnut-CGB fixPalette) are RGB565: bits[15:11]=R, bits[10:5]=G, bits[4:0]=B
// tsl::Color layout: {r4, g4, b4, a4} — map 5→4 bit by shifting right 1.
//
// We store both formats in g_gb_fb and detect via the MSB: RGB555 always
// has bit 15 = 0 (max value 0x7FFF). RGB565 can have bit 15 = 1.
// Rather than branching per pixel, we use a single unified converter:
//   - For DMG (RGB555): R = bits[4:0]>>1, G = bits[9:5]>>1, B = bits[14:10]>>1
//   - For CGB (RGB565): R = bits[15:11]>>1, G = bits[10:5]>>2, B = bits[4:0]>>1
// We pick the converter once per frame based on gb->cgb.cgbMode.
// Since g_gb_fb is a simple uint16 array shared between draw_line and the
// renderer, we store a flag g_fb_is_rgb565 set by the draw_line callback.

inline tsl::Color rgb555_to_tsl(const uint16_t c) {
    return tsl::Color{
        static_cast<uint8_t>( (c        & 0x1F) >> 1),  // R5 → R4
        static_cast<uint8_t>(((c >>  5) & 0x1F) >> 1),  // G5 → G4
        static_cast<uint8_t>(((c >> 10) & 0x1F) >> 1),  // B5 → B4
        0xF
    };
}

// RGB565: R5[15:11], G6[10:5], B5[4:0] → RGBA4444 (each channel 0–15)
// Take the top 4 bits of each component — same approach as rgb555_to_tsl.
inline tsl::Color rgb565_to_tsl(const uint16_t c) {
    return tsl::Color{
        static_cast<uint8_t>((c >> 12) & 0xF),   // R5 → R4: bits[15:12]
        static_cast<uint8_t>((c >>  7) & 0xF),   // G6 → G4: bits[10:7]
        static_cast<uint8_t>((c >>  1) & 0xF),   // B5 → B4: bits[4:1]
        0xF
    };
}

// Set by gb_load_rom: true when the ROM is a CGB game (Walnut outputs RGB565).
extern bool g_fb_is_rgb565;
extern bool g_original_palette;

// ── Background fill ───────────────────────────────────────────────────────────
// Fills the full overlay area with solid black so the underlying game is hidden.
//inline void render_gb_background(tsl::gfx::Renderer* renderer) {
//    renderer->fillScreen(renderer->a(tsl::defaultBackgroundColor));
//    renderer->drawWallpaper();
//
//    //renderer->drawRect(15, tsl::cfg::FramebufferHeight - 73, tsl::cfg::FramebufferWidth - 30, 1, renderer->a(tsl::bottomSeparatorColor));
//    
//    #if USING_WIDGET_DIRECTIVE
//    if (m_showWidget)
//        renderer->drawWidget();
//    #endif
//}

// ── GB screen render ──────────────────────────────────────────────────────────
// Scales g_gb_fb (160×144) into the overlay viewport using nearest-neighbour
// scaling with three key optimisations:
//
//  1. Static coordinate LUTs — the destination x/y extents for every source
//     pixel are pre-computed once (all inputs are compile-time constants) and
//     stored in int16 arrays. Eliminates 23,040 integer divisions per frame.
//
//  2. Horizontal RLE merging — adjacent source pixels with the same raw value
//     are merged into a single wider drawRect.  Color conversion only fires on
//     a flush, not per pixel.  Typical game frames drop from 23,040 drawRect
//     calls to roughly 2,000–4,000.
//
//  3. g_fb_is_rgb565 hoisted out of both loops — the branch resolves once per
//     frame, letting the inner loop be a pure comparison + occasional flush.
inline void render_gb_screen(tsl::gfx::Renderer* renderer) {
    // ── LUT init (runs exactly once) ────────────────────────────────────────
    static bool       lut_ready = false;
    static int16_t    dx0[GB_W], dx1[GB_W];   // destination x extents
    static int16_t    dy0[GB_H], dy1[GB_H];   // destination y extents
    if (!lut_ready) {
        for (int i = 0; i < GB_W; ++i) {
            dx0[i] = static_cast<int16_t>( i      * VP_W / GB_W);
            dx1[i] = static_cast<int16_t>((i + 1) * VP_W / GB_W);
        }
        for (int i = 0; i < GB_H; ++i) {
            dy0[i] = static_cast<int16_t>( i      * VP_H / GB_H);
            dy1[i] = static_cast<int16_t>((i + 1) * VP_H / GB_H);
        }
        lut_ready = true;
    }

    // ── Format flag — hoisted out of all loops ───────────────────────────────
    const bool is565 = g_fb_is_rgb565;

    // ── Render with horizontal RLE ───────────────────────────────────────────
    for (int sy = 0; sy < GB_H; ++sy) {
        const int         row_y  = VP_Y + dy0[sy];
        const int         row_h  = dy1[sy] - dy0[sy];
        const uint16_t*   src    = g_gb_fb + sy * GB_W;

        int      run_sx  = 0;
        uint16_t run_pix = src[0];

        for (int sx = 1; sx < GB_W; ++sx) {
            const uint16_t pix = src[sx];
            if (pix == run_pix) continue;  // extend run

            // Flush accumulated run
            renderer->drawRect(
                VP_X + dx0[run_sx], row_y,
                dx1[sx - 1] - dx0[run_sx], row_h,
                is565 ? rgb565_to_tsl(run_pix) : rgb555_to_tsl(run_pix));

            run_sx  = sx;
            run_pix = pix;
        }

        // Flush final run on this row
        renderer->drawRect(
            VP_X + dx0[run_sx], row_y,
            dx1[GB_W - 1] - dx0[run_sx], row_h,
            is565 ? rgb565_to_tsl(run_pix) : rgb555_to_tsl(run_pix));
    }
}

// ── Optional: border around the viewport ─────────────────────────────────────
// Draws a 1-pixel border in a dark colour so the GB screen has a clean edge.
inline void render_gb_border(tsl::gfx::Renderer* renderer) {
    const tsl::Color border{0x3, 0x3, 0x3, 0xF};
    // Top
    renderer->drawRect(VP_X - 1, VP_Y - 1, VP_W + 2, 1, border);
    // Bottom
    renderer->drawRect(VP_X - 1, VP_Y + VP_H, VP_W + 2, 1, border);
    // Left
    renderer->drawRect(VP_X - 1, VP_Y, 1, VP_H, border);
    // Right
    renderer->drawRect(VP_X + VP_W, VP_Y, 1, VP_H, border);
}