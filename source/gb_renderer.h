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
 ********************************************************************************/

#pragma once

#include <tesla.hpp>
#include "gb_core.h"

// Set by gb_load_rom: true when the ROM is a CGB game (Walnut outputs RGB565).
extern bool g_fb_is_rgb565;
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
    const uint8_t r = ( c        & 0x1F) >> 1;
    const uint8_t g = ((c >>  5) & 0x1F) >> 1;
    const uint8_t b = ((c >> 10) & 0x1F) >> 1;
    return static_cast<uint16_t>(r | (g << 4) | (b << 8) | 0xF000u);
}

inline uint16_t rgb565_to_packed(const uint16_t c) {
    const uint8_t r = (c >> 12) & 0xF;
    const uint8_t g = (c >>  7) & 0xF;
    const uint8_t b = (c >>  1) & 0xF;
    return static_cast<uint16_t>(r | (g << 4) | (b << 8) | 0xF000u);
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
static bool    s_coord_ready = false;
static int16_t s_dx0[GB_W], s_dx1[GB_W];
static int16_t s_dy0[GB_H], s_dy1[GB_H];

static void init_coord_lut() {
    const int cvp_w = vp_w(), cvp_h = vp_h();
    for (int i = 0; i < GB_W; ++i) {
        s_dx0[i] = static_cast<int16_t>( i      * cvp_w / GB_W);
        s_dx1[i] = static_cast<int16_t>((i + 1) * cvp_w / GB_W);
    }
    for (int i = 0; i < GB_H; ++i) {
        s_dy0[i] = static_cast<int16_t>( i      * cvp_h / GB_H);
        s_dy1[i] = static_cast<int16_t>((i + 1) * cvp_h / GB_H);
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

// -- GB screen render ---------------------------------------------------------
// Hot path summary per frame:
//   - Outer loop: 144 GB source rows
//   - RLE scan: groups same-colour source pixels into runs (~2000-4000 total)
//   - Per run: one colour conversion (rgb555/565 -> packed uint16)
//   - Per run x output row: 8x-unrolled scatter-write via setPixelAtOffset,
//     which is: framebuffer[offset] = color  (direct write, no blend, no swizzle math)
//   - No drawRect, no per-pixel getPixelOffset, no blend math (a=0xF).
//
// When ult::expandedMemory is true, the 144 source rows are split evenly across
// ult::renderThreads (same global thread pool used by drawBitmapRGBA4444).
// Each source row maps to a disjoint set of output rows so there are zero
// shared writes — no locks needed.

static void render_gb_screen_chunk(tsl::gfx::Renderer* renderer,
                                   const bool is565,
                                   const int sy_start, const int sy_end) {
    for (int sy = sy_start; sy < sy_end; ++sy) {
        const int oy0 = s_dy0[sy];
        const int oy1 = s_dy1[sy];
        const uint16_t* __restrict__ src = g_gb_fb + sy * GB_W;

        int      run_sx  = 0;
        uint16_t run_pix = src[0];

        for (int sx = 1; sx <= GB_W; ++sx) {
            const uint16_t pix = (sx < GB_W) ? src[sx] : static_cast<uint16_t>(~run_pix);
            if (pix == run_pix) continue;

            // -- Flush run [run_sx, sx) ---------------------------------------
            const int ox0   = s_dx0[run_sx];
            const int ox1   = s_dx1[sx - 1];
            const int run_w = ox1 - ox0;

            // Colour conversion: once per run
            const uint16_t packed = is565
                ? rgb565_to_packed(run_pix)
                : rgb555_to_packed(run_pix);
            const tsl::Color color = *reinterpret_cast<const tsl::Color*>(&packed);

            for (int oy = oy0; oy < oy1; ++oy) {
                const uint32_t row_base = s_row_lut[oy];
                const uint32_t* __restrict__ cl = s_col_lut + ox0;
                int ox = 0;

                // 8x unrolled: independent stores, CPU can pipeline them
                for (; ox + 7 < run_w; ox += 8) {
                    renderer->setPixelAtOffset(row_base + cl[ox + 0], color);
                    renderer->setPixelAtOffset(row_base + cl[ox + 1], color);
                    renderer->setPixelAtOffset(row_base + cl[ox + 2], color);
                    renderer->setPixelAtOffset(row_base + cl[ox + 3], color);
                    renderer->setPixelAtOffset(row_base + cl[ox + 4], color);
                    renderer->setPixelAtOffset(row_base + cl[ox + 5], color);
                    renderer->setPixelAtOffset(row_base + cl[ox + 6], color);
                    renderer->setPixelAtOffset(row_base + cl[ox + 7], color);
                }
                for (; ox < run_w; ++ox)
                    renderer->setPixelAtOffset(row_base + cl[ox], color);
            }

            run_sx  = sx;
            run_pix = pix;
        }
    }
}

inline void render_gb_screen(tsl::gfx::Renderer* renderer) {
    if (!s_lut_ready)   init_swizzle_lut();
    if (!s_coord_ready) init_coord_lut();

    const bool is565 = g_fb_is_rgb565;

    if (!ult::expandedMemory) {
        // Single-threaded path (low-memory devices)
        render_gb_screen_chunk(renderer, is565, 0, GB_H);
        return;
    }

    // Multi-threaded path: divide GB source rows across the global thread pool.
    // GB_H=144 rows, each thread gets an equal slice.  The last thread absorbs
    // any remainder so every row is covered with no overlap.
    const int numThreads  = static_cast<int>(ult::numThreads);
    const int chunkSize   = std::max(1, GB_H / numThreads);

    for (int i = 0; i < numThreads; ++i) {
        const int sy_start = i * chunkSize;
        const int sy_end   = (i == numThreads - 1) ? GB_H : sy_start + chunkSize;
        if (sy_start >= GB_H) {
            ult::renderThreads[i] = std::thread([](){});
            continue;
        }
        ult::renderThreads[i] = std::thread(render_gb_screen_chunk,
                                             renderer, is565, sy_start, sy_end);
    }
    for (auto& t : ult::renderThreads)
        t.join();
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
    static constexpr tsl::Color C_RED = {0xF, 0x1, 0x1, 0xF};  // C
    static constexpr tsl::Color C_ORA = {0xF, 0x7, 0x0, 0xF};  // O
    static constexpr tsl::Color C_YEL = {0xF, 0xD, 0x0, 0xF};  // L
    static constexpr tsl::Color C_GRN = {0x0, 0xA, 0x2, 0xF};  // O
    static constexpr tsl::Color C_IND = {0x3, 0x2, 0xC, 0xF};  // R

    static bool measured = false;
    static u32  w_prefix = 0, wC = 0, wO = 0, wL = 0, wO2 = 0, wR = 0;

    if (!measured) {
        static const auto [wp,  hp]  = renderer->getTextDimensions("GAME BOY ", false, LOGO_SIZE);
        static const auto [wc,  hc]  = renderer->getTextDimensions("C", false, LOGO_SIZE);
        static const auto [wo,  ho]  = renderer->getTextDimensions("O", false, LOGO_SIZE);
        static const auto [wl,  hl]  = renderer->getTextDimensions("L", false, LOGO_SIZE);
        static const auto [wo2, ho2] = renderer->getTextDimensions("O", false, LOGO_SIZE);
        static const auto [wr,  hr]  = renderer->getTextDimensions("R", false, LOGO_SIZE);
        w_prefix = wp; wC = wc; wO = wo; wL = wl; wO2 = wo2; wR = wr;
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
inline void render_gb_letterbox(tsl::gfx::Renderer* renderer) {
    if (!g_vp_2x) return;
    if (!s_outer_lut_ready) init_outer_lut();

    // RGBA4444 packed: r=2, g=2, b=2, a=0xF  →  0xF222
    static constexpr uint16_t PACKED_LB = 0xE111u;
    const tsl::Color LB_COLOR = renderer->a(PACKED_LB);

    // ix/iy: inner viewport origin relative to outer LUT origin (VP_X/VP_Y).
    const int ix = vp_x() - VP_X;   // = 40 in 2× mode
    const int iy = vp_y() - VP_Y;   // = 36 in 2× mode
    const int iw = vp_w();           // = 320 in 2× mode
    const int ih = vp_h();           // = 288 in 2× mode

    // Fill a rectangular strip using the outer swizzle LUTs.
    // ox/oy are relative to VP_X/VP_Y (outer LUT indices).
    // 8× unrolled inner loop: independent stores let the CPU pipeline them.
    const auto fill = [&](const int ox0, const int ox1,
                          const int oy0, const int oy1) __attribute__((always_inline)) {
        const int w = ox1 - ox0;
        for (int oy = oy0; oy < oy1; ++oy) {
            const uint32_t row_base = s_outer_row_lut[oy];
            const uint32_t* __restrict__ cl = s_outer_col_lut + ox0;
            int ox = 0;
            for (; ox + 7 < w; ox += 8) {
                renderer->setPixelAtOffset(row_base + cl[ox + 0], LB_COLOR);
                renderer->setPixelAtOffset(row_base + cl[ox + 1], LB_COLOR);
                renderer->setPixelAtOffset(row_base + cl[ox + 2], LB_COLOR);
                renderer->setPixelAtOffset(row_base + cl[ox + 3], LB_COLOR);
                renderer->setPixelAtOffset(row_base + cl[ox + 4], LB_COLOR);
                renderer->setPixelAtOffset(row_base + cl[ox + 5], LB_COLOR);
                renderer->setPixelAtOffset(row_base + cl[ox + 6], LB_COLOR);
                renderer->setPixelAtOffset(row_base + cl[ox + 7], LB_COLOR);
            }
            for (; ox < w; ++ox)
                renderer->setPixelAtOffset(row_base + cl[ox], LB_COLOR);
        }
    };

    // Left strip:   x = [VP_X,          vp_x())          y = [VP_Y, VP_Y+VP_H)
    fill(0,          ix,                  0,     VP_H);
    // Right strip:  x = [vp_x()+vp_w(), VP_X+VP_W)       y = [VP_Y, VP_Y+VP_H)
    fill(ix + iw,    VP_W,                0,     VP_H);
    // Top strip:    x = [vp_x(),         vp_x()+vp_w())  y = [VP_Y,        vp_y())
    fill(ix,         ix + iw,             0,     iy);
    // Bottom strip: x = [vp_x(),         vp_x()+vp_w())  y = [vp_y()+vp_h(), VP_Y+VP_H)
    fill(ix,         ix + iw,             iy + ih, VP_H);


    // Power indicator LED — in the left letterbox strip, slightly above centre,
    // mimicking where the power light lives on a real GBC.
    //   x: centre of left strip (VP_X to vp_x()) in absolute coords → VP_X + ix/2
    //   y: upper third of the full strip height                      → VP_Y + VP_H/3
    //const int led_cx = VP_X + ix / 2 -2;
    //const int led_cy = VP_Y + VP_H / 3;
    //renderer->drawCircle(led_cx, led_cy, 4, true, {0x7, 0x0, 0x0, 0xF});  // dark rim
    //renderer->drawCircle(led_cx, led_cy, 3, true, {0xF, 0x2, 0x2, 0xF});  // red fill
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