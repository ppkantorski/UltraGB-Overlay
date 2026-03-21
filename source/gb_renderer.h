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
// GB/GBC RGB555 layout: bits[4:0]=R, bits[9:5]=G, bits[14:10]=B
// tsl::Color layout   : {r4, g4, b4, a4} each 0–15 (4-bit)
// Conversion: shift right by 1 to map 5-bit → 4-bit.
//
// If GBC colours appear tinted, the palette may store BGR555 instead of RGB555.
// In that case swap the r/b assignments below.
inline tsl::Color rgb555_to_tsl(const uint16_t c) {
    return tsl::Color{
        static_cast<uint8_t>( (c        & 0x1F) >> 1),  // R: bits[4:0]
        static_cast<uint8_t>(((c >>  5) & 0x1F) >> 1),  // G: bits[9:5]
        static_cast<uint8_t>(((c >> 10) & 0x1F) >> 1),  // B: bits[14:10]
        0xF
    };
}

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
// Scales g_gb_fb (160×144 RGB555) into the overlay viewport (VP_X, VP_Y, VP_W, VP_H).
// Call once per frame from inside a tsl::elm::Element::draw() override.
inline void render_gb_screen(tsl::gfx::Renderer* renderer) {
    for (int sy = 0; sy < GB_H; ++sy) {
        // Destination y range for this source row
        const int dy0 = (sy       * VP_H) / GB_H;
        const int dy1 = ((sy + 1) * VP_H) / GB_H;
        const int dh  = dy1 - dy0;  // 2 or 3

        const uint16_t* srcRow = g_gb_fb + sy * GB_W;

        for (int sx = 0; sx < GB_W; ++sx) {
            // Destination x range for this source column
            const int dx0 = (sx       * VP_W) / GB_W;
            const int dx1 = ((sx + 1) * VP_W) / GB_W;
            const int dw  = dx1 - dx0;  // 2 or 3

            renderer->drawRect(
                VP_X + dx0, VP_Y + dy0,
                dw, dh,
                rgb555_to_tsl(srcRow[sx])
            );
        }
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
