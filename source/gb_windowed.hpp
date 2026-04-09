/********************************************************************************
 * File: gb_windowed.hpp
 * Description:
 *   Windowed GB mode for UltraGB.
 *
 *   Activated via the "-windowed" launch argument.  The ROM path is NOT passed
 *   as an argument — game names can contain spaces, which the C runtime would
 *   split into separate argv tokens making the path unresolvable.  Instead the
 *   ROM path is written to config.ini as "windowed_rom" by the ROM click
 *   listener before calling setNextOverlay("-windowed"), and read back in
 *   WindowedOverlay::initServices().  The key is cleared immediately after
 *   reading so it never persists across unrelated launches.
 *
 *   Framebuffer:
 *     DefaultFramebufferWidth  = GB_W * g_win_scale  (160 / 320 / 480)
 *     DefaultFramebufferHeight = GB_H * g_win_scale  (144 / 288 / 432)
 *     Set in main() before tsl::loop<WindowedOverlay>.  Tesla creates a VI
 *     layer of exactly that size.  g_win_scale is read directly from
 *     config.ini before tsl::loop (load_config has not run yet at that point).
 *
 *     Scale 1: 160×144 px  — VI space 240×216   (native GB pixels)
 *     Scale 2: 320×288 px  — VI space 480×432   (2× integer scaled)
 *     Scale 3: 480×432 px  — VI space 720×648   (3× integer scaled)
 *     Scale 4: 640×576 px  — VI space 960×864   (4× integer scaled)
 *     Scale 5: 800×720 px  — VI space 1200×1080 (5× integer scaled; requires 8 MB+ heap)
 *
 *   Pixel blit:
 *     LUTs encode the Tesla block-linear address formula for the scaled
 *     framebuffer.  OWV (outer-width-value, the inter-strip stride) depends on
 *     framebuffer width and is recomputed per scale.  For each source GB pixel
 *     (sx, sy) the blit writes g_win_scale × g_win_scale destination pixels.
 *     s_win_col / s_win_row are heap-allocated to exactly GB_W*scale /
 *     GB_H*scale entries by init_win_luts() — saving up to ~6 KB vs. static
 *     BSS arrays sized for the 6× maximum.
 *
 *   Touch drag:
 *     Same scheme as 1× — polls HID directly.  Window size in touch space
 *     scales with g_win_scale.  VI max position is recomputed per scale.
 *     Saved position is clamped to the new valid range on every createUI()
 *     so a scale change never places the window off-screen.
 *
 *   VI / touch coordinate math (scale N):
 *     FB size: GB_W×N  ×  GB_H×N   (framebuffer pixels)
 *     VI size: GB_W×N×3/2  ×  GB_H×N×3/2   (VI space, Switch 1.5× display scale)
 *     Max safe VI pos: 1920 − VI_W  ×  1080 − VI_H
 *     Window in touch space: GB_W×N  wide  ×  GB_H×N  tall
 *     touch→VI: ×3/2      VI→touch: ×2/3
 *
 *   Included at the bottom of main.cpp, after all globals and helpers are
 *   fully defined.  g_win_scale must be declared in main.cpp before this
 *   header is included.
 * 
 *  Licensed under GPLv2
 *  Copyright (c) 2026 ppkantorski
 ********************************************************************************/

#pragma once


#include <cstring>
#include <algorithm>

// gb_renderer.h (rgb555_to_packed, rgb565_to_packed) is already included above.

// Per-function attributes override this file-level Os baseline where needed.
// Hot per-frame functions carry __attribute__((optimize("O3"))).
// Cold setup / pool management functions inherit Os from the push below.
#pragma GCC push_options
#pragma GCC optimize("Os")

// ── Swizzle LUTs for the scaled framebuffer ───────────────────────────────────
// Tesla block-linear formula (from tesla.hpp getPixelOffset, no scissor):
//   offset = ((((y&127)>>4) + ((x>>5)<<3) + ((y>>7)*OWV)) << 9)
//            + ((y&8)<<5) + ((x&16)<<3) + ((y&6)<<4) + ((x&8)<<1)
//            + ((y&1)<<3) + (x&7)
//
// OWV (outer-width-value) depends on framebuffer width W:
//   OWV = ((W/2) >> 4) << 3  =  (W >> 5) << 3  =  (W / 32) * 8
//
//   Scale 1: W=160 → OWV=40    Scale 2: W=320 → OWV=80
//   Scale 3: W=480 → OWV=120   Scale 4: W=640 → OWV=160
//
// The formula splits into col_part(x) + row_part(y):
//   col_part(x) = (((x>>5)<<3)<<9) + ((x&16)<<3) + ((x&8)<<1) + (x&7)
//   row_part(y) = ((((y&127)>>4) + ((y>>7)*OWV))<<9)
//                + ((y&8)<<5) + ((y&6)<<4) + ((y&1)<<3)
//
// Swizzle LUT pointers — heap-allocated to exactly GB_W*scale / GB_H*scale
// entries by init_win_luts().  Sizing to the actual runtime scale saves up to
// ~5.9 KB (at 1×) vs. static BSS arrays sized for the 6× maximum.  On NX,
// BSS and heap both commit physical RAM, so static over-sizing is a real cost.
//
// Hot-path access (win_blit_rows template): the compiler loads each pointer
// into a register once before any inner loop — zero per-pixel overhead vs.
// the former static arrays.  The NEON helpers already receive these as
// const uint32_t* parameters, so their call sites are unchanged.
static uint32_t* s_win_col = nullptr;  // GB_W * scale entries, exact
static uint32_t* s_win_row = nullptr;  // GB_H * scale entries, exact
static bool     s_win_lut_ready = false;
// Set true by GBWindowedGui::handleInput while a touch-drag reposition is active.
// Read by GBWindowedElement::draw to overlay a 4-pixel red border on the frame.
static bool     s_win_dragging  = false;
// g_focus_flash / g_focus_flash_red live in gb_globals.hpp (shared with overlay mode).
// Set in draw() when the console undocks while windowedLayerPixelPerfect is active.
// Consumed in WindowedOverlay::update() to trigger a clean relaunch with 720p sizing.
static bool     s_undock_relaunch = false;

// ── Windowed mode row-LUT update tracking ─────────────────────────────────────
// s_win_owv / s_win_fh: saved per-session constants used by update_win_row_lut
// to rebuild s_win_row in place (no realloc) when the FB y_offset changes.
static uint32_t s_win_owv           = 0u;
static int      s_win_fh            = 0;
static int      s_win_last_y_offset = -1;  // -1 forces first build

// ── Windowed framebuffer positioning helpers ───────────────────────────────────
// The framebuffer is always win_fb_height() rows (half-screen + half-game).
// The VI layer position is FIXED to one screen edge; the FB y-offset slides
// the game content within the layer so it appears at g_win_pos_y on screen.
//
//   layer_fb_top  = screen_fb_h() − win_fb_height()
//   layer_vi_top  = layer_fb_top × 3/2  (720p)  or  layer_fb_top  (1080p)
//   threshold     = layer_vi_top  (= vi_max_y_content() / 2 — exact screen midpoint)
//
//   TOP HALF   (pos_y ≤ layer_vi_top):
//     layer_vi_y  = 0                         layer FIXED — glued to screen top (ceiling)
//     y_ofs       = pos_y × 2/3  (720p)       game slides DOWN through the FB
//                 = pos_y        (1080p)
//     screen_top  = 0 + y_ofs × 1.5 = pos_y  ✓
//     Transparent rows ABOVE the game (0..y_ofs−1) are anchored to screen top.
//     Transparent rows BELOW the game extend toward the layer bottom edge.
//
//   BOTTOM HALF (pos_y > layer_vi_top):
//     layer_vi_y  = layer_vi_top               layer FIXED — glued to screen bottom (floor)
//     y_ofs       = (pos_y−layer_vi_top) × 2/3 (720p)   game slides UP toward FB bottom
//                 = (pos_y−layer_vi_top)        (1080p)
//     screen_top  = layer_vi_top + y_ofs × 1.5 = pos_y  ✓
//     Transparent rows BELOW the game (y_ofs+game_h..end) are anchored to screen bottom.
//
//   TRANSITION at pos_y = layer_vi_top — seamless by construction:
//     top    anchor: layer_vi_y = 0,           y_ofs = layer_fb_top
//       → screen = 0 + layer_fb_top × 1.5 = layer_vi_top  ✓
//     bottom anchor: layer_vi_y = layer_vi_top, y_ofs = 0
//       → screen = layer_vi_top + 0           = layer_vi_top  ✓
//     Both anchors produce the SAME on-screen game position at the threshold.
//     y_ofs changes continuously within each half so motion is smooth everywhere.
//
//   GLITCH PREVENTION:
//     setLayerPos is called from draw() AFTER the pixel blit, never from
//     handleInput().  This guarantees the layer VI position and FB y_ofs are
//     always derived from the same g_win_pos_y in the same frame — the
//     double-buffer desync (new layer Y applied to the PREVIOUS frame's FB
//     with the old y_ofs) cannot occur.
//
// Memory savings vs full-height 720-row FB:
//   Scale 1: 432 rows  (−40 %)     Scale 4: 648 rows  (−10 %)
//   Scale 2: 504 rows  (−30 %)     Scale 5: 720 rows  (same — game fills screen)
//   Scale 3: 576 rows  (−20 %)

// Full-screen height in FB pixels (720 or 1080).
static inline int screen_fb_h() {
    return ult::windowedLayerPixelPerfect ? 1080 : 720;
}

// Framebuffer height.
//   Normal mode     — half screen + half game (rounded up): supports the dynamic
//                     top/bottom anchor, which needs room for both halves.
//   Limited-memory  — exactly GB_H*scale: no padding, layer Y = g_win_pos_y directly.
//                     Screenshots are disabled in this mode anyway.
static inline int win_fb_height() {
    const int game_h = GB_H * g_win_scale;
    if (g_win_limited_fb) return game_h;
    return screen_fb_h() / 2 + (game_h + 1) / 2;
}

// VI-space Y of the layer's top edge — FIXED to one screen edge at all times.
//
// TOP HALF   (pos_y ≤ layer_vi_top): layer_vi_y = 0           (ceiling anchor)
// BOTTOM HALF (pos_y > layer_vi_top): layer_vi_y = layer_vi_top (floor anchor)
//
// The layer never moves; win_fb_y_offset() slides the game within the FB so
// it appears at g_win_pos_y on screen.  setLayerPos is called from draw()
// after the blit so layer position and FB y_ofs are always consistent.
static inline int win_layer_vi_y() {
    if (g_win_limited_fb) return g_win_pos_y;
    const int layer_fb_top = screen_fb_h() - win_fb_height();
    const int layer_vi_top = ult::windowedLayerPixelPerfect ? layer_fb_top : (layer_fb_top * 3 / 2);
    return (g_win_pos_y <= layer_vi_top) ? 0 : layer_vi_top;
}

// FB row within the layer where game content starts.
//
// The layer is FIXED (see win_layer_vi_y); this offset slides the game within
// the FB so it appears at g_win_pos_y on screen.
//
// TOP HALF   (pos_y ≤ layer_vi_top):
//   screen_top = layer_vi_y(0) + y_ofs × scale_factor = pos_y
//   → y_ofs = pos_y / scale_factor
//     720p:  y_ofs = pos_y × 2/3    range [0, layer_fb_top]
//     1080p: y_ofs = pos_y           range [0, layer_fb_top]
//
// BOTTOM HALF (pos_y > layer_vi_top):
//   screen_top = layer_vi_top + y_ofs × scale_factor = pos_y
//   → y_ofs = (pos_y − layer_vi_top) / scale_factor
//     720p:  y_ofs = (pos_y − layer_vi_top) × 2/3    range [0, layer_fb_top]
//     1080p: y_ofs = (pos_y − layer_vi_top)            range [0, layer_fb_top]
//
// Always 0 in limited-memory mode (no padding rows, layer tracks pos_y directly).
static inline int win_fb_y_offset() {
    if (g_win_limited_fb) return 0;
    const int layer_fb_top = screen_fb_h() - win_fb_height();
    const int layer_vi_top = ult::windowedLayerPixelPerfect ? layer_fb_top : (layer_fb_top * 3 / 2);
    if (g_win_pos_y <= layer_vi_top) {
        // Ceiling anchor — layer top = screen top (VI 0).
        // Slide game down from FB row 0: y_ofs grows as pos_y increases.
        return ult::windowedLayerPixelPerfect
            ? g_win_pos_y
            : (g_win_pos_y * 2 / 3);
    } else {
        // Floor anchor — layer bottom = screen bottom.
        // Slide game up from FB row 0: y_ofs grows as pos_y increases past threshold.
        return ult::windowedLayerPixelPerfect
            ? (g_win_pos_y - layer_vi_top)
            : ((g_win_pos_y - layer_vi_top) * 2 / 3);
    }
}

// VI-space Y bounds for drag clamping — anchor-independent.
// The game may travel from screen top (0) to screen bottom continuously.
static inline int vi_min_y_content() { return 0; }
static inline int vi_max_y_content() {
    const int game_h = GB_H * g_win_scale;
    return ult::windowedLayerPixelPerfect
        ? (1080 - game_h)
        : (1080 - game_h * 3 / 2);
}

// Update the notification hit-test offsets to match the current game-content
// position.  Must be called whenever g_win_pos_x / g_win_pos_y change.
static void sync_notif_touch_offsets() {
    ult::layerEdge  = (g_win_pos_x * 2) / 3;
    tsl::layerEdgeY = (g_win_pos_y * 2) / 3;  // game content top in touch space
}

// Allocate and populate col/row LUTs sized exactly for the given scale.
// Called once per windowed session — s_win_col/s_win_row are nullptr on each
// fresh overlay launch, so the delete[] calls on first entry are safe no-ops.
//
// fw/fh are unsigned so the compiler's range for the new[] size argument is
// [0, UINT_MAX], giving a worst-case allocation of UINT_MAX×4 ≈ 17 GB which
// is below SSIZE_MAX.  With signed int the range includes negatives; cast to
// size_t, -1 becomes SIZE_MAX > SSIZE_MAX and triggers -Walloc-size-larger-than=.
[[gnu::noinline]]
static void init_win_luts(int scale) {
    delete[] s_win_col;
    delete[] s_win_row;
    const unsigned fw  = static_cast<unsigned>(GB_W) * static_cast<unsigned>(scale);
    const unsigned fh  = static_cast<unsigned>(GB_H) * static_cast<unsigned>(scale);
    const uint32_t owv = (static_cast<uint32_t>(fw) >> 5u) << 3u;
    s_win_col  = new uint32_t[fw];
    s_win_row  = new uint32_t[fh];
    s_win_owv  = owv;
    s_win_fh   = static_cast<int>(fh);
    build_col_lut(s_win_col, 0, static_cast<int>(fw));
    const int y0 = win_fb_y_offset();
    build_row_lut(s_win_row, y0, s_win_fh, s_win_owv);
    s_win_last_y_offset = y0;
    s_win_lut_ready = true;
}

// Rebuild s_win_row in place when the game content y_offset changes (drag).
// No-op in steady state — single int comparison.
static inline void __attribute__((optimize("O3"))) update_win_row_lut(int y_offset) {
    if (!s_win_lut_ready || y_offset == s_win_last_y_offset) return;
    build_row_lut(s_win_row, y_offset, s_win_fh, s_win_owv);
    s_win_last_y_offset = y_offset;
}

// =============================================================================
// Windowed pixel blit — multithreaded, RLE-compressed, NEON-accelerated.
//
// win_blit_rows<LCD_GRID, SCALE>
//   Processes GB source rows [sy_start, sy_end) into the pre-fetched scaled
//   framebuffer pointer.  Four compounding optimisations:
//
//   1. RLE grouping — scans each source row for runs of identical pixels.
//      Colour conversion (rgb555/565 → RGBA4444) fires exactly once per run,
//      not once per pixel.  For typical tile-heavy game content this halves
//      conversion work.
//
//   2. Row-pointer precomputation — all SCALE destination row pointers
//      (fb + s_win_row[sy*SCALE + dy]) are computed once per source row,
//      outside the RLE run loop.  Previously they were recomputed per-run ×
//      per-dy; hoisting saves (num_runs − 1) × SCALE row-LUT loads per row.
//
//   3. Multi-row NEON fill (neon_fill_multirow<SCALE>) — for any run in the
//      no-grid path, each 8-column swizzle tile's column LUT entry is loaded
//      ONCE and the colour is stored to all SCALE destination rows before
//      advancing to the next tile.  Compared to calling neon_lut_fill once
//      per dy row (which reloads the same LUT entry SCALE times), this reduces
//      column LUT loads by SCALE×.  At SCALE=5 with typical game content that
//      is ~5 000 fewer cache accesses per frame.
//
//   4. Compile-time SCALE — SCALE is a template int, so the compiler fully
//      unrolls the dy loop in neon_fill_multirow and (in the grid path) the
//      inner loop, with zero branch overhead.  LCD_GRID branches are also
//      compile-time eliminated.
//
// launch_win_blit<LCD_GRID, SCALE>
//   Dispatches to the persistent WinBlitPool for SCALE ≥ 3.
//   SCALE < 3 (≤ 92K pixels) always single-threads — pool overhead approaches
//   the blit time at those sizes.
// =============================================================================

// Inner per-thread blit kernel.
// fb        — raw RGBA4444 framebuffer base pointer (cached once before launch).
// src_fb    — g_gb_fb (160×144 source pixels).
// is565     — source pixel format: true=RGB565 (CGB), false=RGB555 (DMG).
// prepacked — source already holds RGBA4444 (DMG pre-packed palette path).
// sy_start/end — half-open range of GB source rows this thread owns.
template <bool LCD_GRID, int SCALE>
static void __attribute__((optimize("O3"))) win_blit_rows(uint16_t* __restrict__ fb,
                          const uint16_t* __restrict__ src_fb,
                          bool is565, bool prepacked,
                          int sy_start, int sy_end) {
    for (int sy = sy_start; sy < sy_end; ++sy) {
        const uint16_t* src_row = src_fb + sy * GB_W;

        // Precompute all SCALE destination row pointers once per source row.
        //
        // Previously these were recomputed inside the RLE run loop: once per
        // run × per dy = num_runs × SCALE row-LUT loads per source row.
        // Hoisting here costs exactly SCALE loads per source row regardless of
        // run count — for 100 runs at SCALE=5 that is 495 fewer LUT loads/row.
        //
        // Also consumed by neon_fill_multirow<SCALE> which needs all pointers
        // simultaneously so it can load each column LUT entry only once and
        // store to all SCALE rows before moving to the next tile.
        uint16_t* row_ptrs[SCALE];
        for (int dy = 0; dy < SCALE; ++dy)
            row_ptrs[dy] = fb + s_win_row[sy * SCALE + dy];

        // ── RLE: detect same-colour horizontal runs ───────────────────────────
        int      run_sx  = 0;
        uint16_t run_pix = src_row[0];

        for (int sx = 1; sx <= GB_W; ++sx) {
            // Sentinel at sx==GB_W forces a final flush of the last run.
            const uint16_t pix = (sx < GB_W)
                ? src_row[sx]
                : static_cast<uint16_t>(~run_pix);
            if (pix == run_pix) [[likely]] continue;

            // ── Colour conversion — once per run, never per pixel ─────────────
            uint16_t packed;
            if (prepacked)  packed = run_pix;
            else if (is565) packed = rgb565_to_packed(run_pix);
            else            packed = rgb555_to_packed(run_pix);

            // Grid colour: dimmed version of packed (compile-time branch).
            uint16_t grid_packed = 0;
            if constexpr (LCD_GRID)
                grid_packed = dim_packed_grid(packed);

            // Destination column span for this run [ox0, ox1).
            const int ox0 = run_sx * SCALE;
            const int ox1 = sx     * SCALE;

            // ── Write SCALE destination rows ──────────────────────────────────
            if constexpr (!LCD_GRID) {
                // ── Fast path: no grid ────────────────────────────────────────
                // neon_fill_multirow loads each column LUT entry once and stores
                // to all SCALE rows before advancing to the next tile.
                // At SCALE=5 this is 5× fewer column LUT loads vs. calling
                // neon_lut_fill once per dy.  Inner dy loop is fully unrolled.
                neon_fill_multirow<SCALE>(row_ptrs, s_win_col, ox0, ox1, packed);

            } else {
                // ── LCD-grid path ─────────────────────────────────────────────
                // Inverted loop order vs. the old implementation:
                // each s_win_col[ox] is loaded ONCE, then written to all
                // SCALE-1 non-grid rows.  This is strictly fewer LUT loads
                // than reloading the same s_win_col[ox] for every dy row.
                //
                // For each source-pixel block:
                //   - columns [bx0, bgx) use normal colour on rows 0..SCALE-2
                //   - column  bgx       uses grid colour   on rows 0..SCALE-2
                //   - final dest row    uses grid colour across the full run

                for (int bsx = run_sx; bsx < sx; ++bsx) {
                    const int bx0 = bsx * SCALE;
                    const int bgx = bx0 + SCALE - 1;  // grid (last) col

                    // Normal interior columns for rows 0..SCALE-2
                    for (int ox = bx0; ox < bgx; ++ox) {
                        const uint32_t c = s_win_col[ox];
                        for (int dy = 0; dy < SCALE - 1; ++dy)
                            row_ptrs[dy][c] = packed;
                    }

                    // Rightmost grid column for rows 0..SCALE-2
                    {
                        const uint32_t c = s_win_col[bgx];
                        for (int dy = 0; dy < SCALE - 1; ++dy)
                            row_ptrs[dy][c] = grid_packed;
                    }
                }

                // Last dest row of each source-pixel block: entire column span
                // gets the grid colour (simulates the horizontal LCD gap).
                neon_lut_fill(row_ptrs[SCALE - 1], s_win_col, ox0, ox1, grid_packed);
            }

            run_sx  = sx;
            run_pix = pix;
        }
    }
}


// =============================================================================
// WinBlitPool — persistent 3-worker thread pool for windowed-mode pixel blit.
//
// WHY A POOL INSTEAD OF per-frame std::thread:
//
//   The previous launch_win_blit assigned ult::renderThreads[i] = std::thread(...)
//   each frame.  That assignment destroys the old thread object and constructs a
//   brand-new OS thread via svcCreateThread + svcStartThread — every frame, 60×/s.
//   On the Switch that syscall pair costs ~50–150 µs per thread.  Across 4 threads:
//   200–600 µs of pure overhead before a pixel is touched.
//
//   A persistent pool pays that cost exactly ONCE (at init).  Between frames the
//   workers sleep in the kernel on a condition_variable — zero CPU burn, zero
//   scheduler pressure on TotK's threads.  Per-frame cost falls to three mutex
//   signals + one condvar wait ≈ 10–30 µs, well below the blit savings at scale ≥ 3.
//
// SCALE THRESHOLDS:
//
//   Scale 1  ( 160× 144 =  23 K px):  single-thread always — too small to split.
//   Scale 2  ( 320× 288 =  92 K px):  single-thread — pool overhead ≈ compute time.
//   Scale 3  ( 480× 432 = 207 K px):  4-way → ~3× speedup.  Pool overhead ~10–30 µs,
//                                      compute ~200–500 µs single-thread.
//   Scale 4  ( 640× 576 = 369 K px):  4-way → ~3–4× speedup.
//   Scale 5  ( 800× 720 = 576 K px):  4-way → ~3–4× speedup.
//   Scale 6  ( 960× 864 = 829 K px):  4-way → ~3–4× speedup.
//
//   The expandedMemory guard is removed — the pool does not need 8 MB; it is
//   just 3 sleeping threads.  Threading decisions are now based purely on SCALE.
//
// ARCHITECTURE:
//   • 3 worker threads created once at pool.init().
//   • Main (Tesla draw) thread processes chunk 0 concurrently with workers.
//   • Workers sleep on cv_start between frames — no spinning, no wasted cycles.
//   • After each dispatch the main thread blocks on cv_done until all 3 workers
//     signal completion — caller sees a synchronous blit, just like the old join().
//
// LIFECYCLE:
//   pool.init()     — call once in GBWindowedGui::createUI().
//   pool.dispatch() — call per frame from launch_win_blit (scale ≥ 3).
//   pool.shutdown() — call once in WindowedOverlay::exitServices().
// =============================================================================

class WinBlitPool {
public:
    // Type-erased blit work item — plain struct, no heap allocation.
    // fn is a captureless lambda pointer capturing LCD_GRID/SCALE as template args.
    struct Work {
        void (*fn)(uint16_t*, const uint16_t*, bool, bool, int, int) = nullptr;
        uint16_t*       fb        = nullptr;
        const uint16_t* src_fb    = nullptr;
        bool            is565     = false;
        bool            prepacked = false;
        int             sy0       = 0;
        int             sy1       = 0;

        void run() const {
            if (fn && sy1 > sy0)
                fn(fb, src_fb, is565, prepacked, sy0, sy1);
        }
    };

    static constexpr int kWorkers = 3;  // + calling thread = 4-way parallelism

    // Create and start all worker threads.  Call once before the first dispatch.
    void init() {
        for (int i = 0; i < kWorkers; ++i)
            m_threads[i] = std::thread([this, i] { worker_loop(i); });
    }

    // Dispatch tasks[0..kWorkers-1] to worker threads, run main_work on the
    // calling thread, then block until every worker has signalled completion.
    // The caller sees a fully synchronous blit — workers overlap with main_work.
    void dispatch(const Work (&tasks)[kWorkers], const Work& main_work) {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_n_done = 0;
            for (int i = 0; i < kWorkers; ++i) {
                m_work[i]  = tasks[i];
                m_ready[i] = true;
            }
        }
        m_cv_start.notify_all();   // wake all 3 workers simultaneously

        main_work.run();           // calling thread processes chunk 0 in parallel

        // Wait for all workers to finish their chunks.
        std::unique_lock<std::mutex> lk(m_mu);
        m_cv_done.wait(lk, [this] { return m_n_done >= kWorkers; });
    }

    // Signal workers to exit and join them.  Safe to call even if init() was
    // never called (threads will be unjoinable — the join is guarded).
    void shutdown() {
        {
            std::lock_guard<std::mutex> lk(m_mu);
            m_stop = true;
            for (int i = 0; i < kWorkers; ++i)
                m_ready[i] = true;   // unblock any sleeping worker
        }
        m_cv_start.notify_all();
        for (int i = 0; i < kWorkers; ++i)
            if (m_threads[i].joinable()) m_threads[i].join();
    }

private:
    void worker_loop(int id) {
        while (true) {
            Work w;
            {
                std::unique_lock<std::mutex> lk(m_mu);
                m_cv_start.wait(lk, [this, id] {
                    return m_stop || m_ready[id];
                });
                if (m_stop) return;
                w           = m_work[id];
                m_ready[id] = false;
            }
            w.run();
            {
                std::lock_guard<std::mutex> lk(m_mu);
                ++m_n_done;
            }
            m_cv_done.notify_one();   // wake main thread if it's the last done
        }
    }

    std::thread             m_threads[kWorkers];
    std::mutex              m_mu;
    std::condition_variable m_cv_start;   // workers wait here between frames
    std::condition_variable m_cv_done;    // main thread waits here for completion
    Work                    m_work[kWorkers];
    bool                    m_ready[kWorkers] = {};
    int                     m_n_done = 0;
    bool                    m_stop   = false;
};

// Single static instance — threads created once at windowed-mode init.
// No heap allocation; workers are sleeping kernel objects between frames.
static WinBlitPool s_win_pool;
static bool        s_win_pool_active = false;


// Thread-pool launcher.  Splits source rows across the persistent WinBlitPool
// and the calling thread, then blocks until all workers complete.
//
// SCALE < 3:  single-thread only.  At ≤ 92 K pixels the pool dispatch overhead
//             (~10–30 µs) approaches the entire blit time — threading does not
//             help and can make things slightly worse.
//
// SCALE ≥ 3:  4-way split (main thread + 3 workers = 36 source rows each at
//             GB_H=144).  Pool overhead ~10–30 µs; compute ~200–800 µs
//             single-thread → ~3–4× speedup.
//
// The expandedMemory guard is gone — the pool costs nothing while idle (sleeping
// workers).  The decision is now purely SCALE-based.  If the pool has not been
// initialised (s_win_pool_active false) the function falls through to
// single-thread as a safe default.
template <bool LCD_GRID, int SCALE>
static void __attribute__((optimize("O3"))) launch_win_blit(uint16_t* fb, const uint16_t* src_fb,
                             bool is565, bool prepacked) {
    // Compile-time elimination: single-thread for scale 1 and 2.
    if constexpr (SCALE < 3) {
        win_blit_rows<LCD_GRID, SCALE>(fb, src_fb, is565, prepacked, 0, GB_H);
        return;
    }

    // Runtime fallback if pool was not initialised (e.g. first frame race).
    if (!s_win_pool_active) {
        win_blit_rows<LCD_GRID, SCALE>(fb, src_fb, is565, prepacked, 0, GB_H);
        return;
    }

    // Captureless lambda: LCD_GRID and SCALE are compile-time template args,
    // not runtime captures, so the lambda converts to a plain fn pointer.
    // Each template instantiation gets its own unique static fn_ptr.
    static constexpr auto fn_ptr =
        +[](uint16_t* f, const uint16_t* s, bool i5, bool pp, int sy0, int sy1) {
            win_blit_rows<LCD_GRID, SCALE>(f, s, i5, pp, sy0, sy1);
        };

    // 4-way split: calling thread takes [0, chunk), workers take the rest.
    // GB_H=144, kTotal=4 → chunk=36 (exact division, no remainder).
    constexpr int kTotal = WinBlitPool::kWorkers + 1;    // = 4
    constexpr int chunk  = (GB_H + kTotal - 1) / kTotal; // = 36

    // Build worker descriptors: slots 0–2 cover rows 36–72, 72–108, 108–144.
    WinBlitPool::Work tasks[WinBlitPool::kWorkers];
    for (int i = 0; i < WinBlitPool::kWorkers; ++i) {
        const int sy0 = (i + 1) * chunk;
        const int sy1 = std::min(sy0 + chunk, GB_H);
        tasks[i] = { fn_ptr, fb, src_fb, is565, prepacked, sy0, sy1 };
    }

    // Dispatch workers and process chunk 0 [0, 36) on the calling thread.
    s_win_pool.dispatch(tasks, { fn_ptr, fb, src_fb, is565, prepacked, 0, chunk });
}
// =============================================================================
// GBWindowedElement
// Blits the GB framebuffer (160×144) into the scaled Tesla framebuffer via LUTs.
// For scale N: each source pixel writes N×N destination framebuffer pixels.
// Also drives the emulation clock (mirrors GBOverlayElement::draw exactly).
// This is a leaf element — no children, no focus, no touch.
// =============================================================================
class GBWindowedElement : public tsl::elm::Element {
public:
    void __attribute__((optimize("O3"))) draw(tsl::gfx::Renderer* renderer) override {

        // ── Always clear the full framebuffer to transparent first ────────────
        // Fixes stale-pixel corruption on both double-buffer frames: any row
        // outside [y_ofs, y_ofs + game_h) must be transparent so the anchored
        // half-screen FB doesn't show garbage below/above the game content.
        // This also acts as the early-return clear when the game isn't running.
        //
        // IMPORTANT: use the block-linear–aligned height, not the logical height.
        //
        // Tesla uses Tegra block-linear with 128-row super-rows (the `y>>7` term
        // in getPixelOffset).  For a framebuffer whose logical height H is not a
        // multiple of 128, pixels in the last partial super-row have block-linear
        // addresses that exceed W×H — they land in the alignment-padding region
        // that libnx allocates but that a naive W×H×2 memset doesn't reach.
        //
        // Concretely (scale 2, H=504, W=320):
        //   logical size  = 320×504 = 161,280 uint16 slots
        //   addr(319,503) = 163,583             → overflows by 2,303 slots
        //   libnx alloc   = 320×512×2 = 327,680 bytes (rounds H up to 512)
        //   naive memset  = 320×504×2 = 322,560 bytes  ← misses the overflow slots
        //
        // Those un-zeroed slots hold stale data from previous frames; VI
        // composites them faithfully and they appear as repeated/corrupt pixels
        // just beyond the game-window edge.
        //
        // Fix: round H up to the next 128-row boundary (matching libnx's own
        // size formula) so the memset covers every block-linear slot.
        uint16_t* const fb          = static_cast<uint16_t*>(renderer->getCurrentFramebuffer());
        const int       fb_h_log    = win_fb_height();
        const int       fb_h_phys   = (fb_h_log + 127) & ~127;  // round up to 128-row block
        const size_t    fb_bytes    = static_cast<size_t>(GB_W * g_win_scale)
                                    * static_cast<size_t>(fb_h_phys)
                                    * sizeof(uint16_t);
        std::memset(fb, 0, fb_bytes);

        if (!g_gb.running || !g_emu_active || !g_gb_fb) return;

        // Detect operation-mode change (handheld ↔ docked).
        // When the mode changes the audio output device changes and the kernel
        // silently invalidates all queued DMA buffers.  Request an async resync
        // so the audio thread flushes and restarts the stream on its next tick.
        // poll_console_docked() rate-limits the underlying IPC call to once per
        // kDockCheckInterval frames (~1 s); cost in steady state is one uint32
        // comparison per frame.
        {
            const bool  is_docked    = poll_console_docked();
            static bool s_was_docked = is_docked;

            if (is_docked != s_was_docked) {
                s_was_docked = is_docked;
                gb_audio_request_resync();
                // Relaunch whenever the dock state changes and the resulting
                // pixel-perfect setting would differ from the current session:
                //   • docked   + g_win_1080=true  → need 1080p sizing (was 720p)
                //   • undocked + pixel-perfect=true → need 720p sizing (was 1080p)
                // exitServices() saves SRAM/audio state before the process restarts.
                if (( is_docked && g_win_1080) ||
                    (!is_docked && ult::windowedLayerPixelPerfect))
                    s_undock_relaunch = true;
            }
        }

        // ── Emulation clock ───────────────────────────────────────────────────
        // gb_tick_frame() (defined in gb_overlay.hpp, visible here because that
        // header is included first in main.cpp) increments g_frame_count and
        // rate-limits the GB CPU to 59.73 fps.  During a touch-drag the game is
        // frozen, so timing is suppressed — but the display-frame counter still
        // advances so double-click detectors keep working.
        //
        // Screenshot capture: mirror the drag-pause behavior — pause audio on the
        // rising edge of disableTransparency so the SPSC ring never empties and
        // buzzes, hold the last frame for the 1.5 s window, then resume and
        // re-anchor the GB clock on the falling edge so we don't try to catch up.
        {
            static bool s_was_screenshot = false;
            const bool  capturing        = ult::disableTransparency;
            if (capturing != s_was_screenshot) {
                if (capturing) {
                    gb_audio_pause();
                } else {
                    gb_audio_resume();
                    g_gb_frame_next_ns = 0;
                }
                s_was_screenshot = capturing;
            }
            if (!s_win_dragging && !capturing) gb_tick_frame();
            else                               ++g_frame_count;
        }

        // ── Row LUT: rebuild if the game's y_offset changed since last frame ──
        // Happens on every drag frame; zero-cost in steady state (one int cmp).
        const int y_ofs = win_fb_y_offset();
        update_win_row_lut(y_ofs);

        // ── Scaled pixel blit ─────────────────────────────────────────────────
        // Delegates to launch_win_blit<LCD_GRID, SCALE> which:
        //   • Caches the raw framebuffer pointer once (no per-pixel indirection).
        //   • Splits source rows across the WinBlitPool worker threads.
        //   • Uses RLE to convert colour once per same-colour horizontal run.
        //   • Uses NEON vst1q_u16 for 8-pixel aligned destination tile spans.
        //   • Unrolls inner loops fully via compile-time SCALE template.
        // See win_blit_rows / launch_win_blit above for the full implementation.
        if (!s_win_lut_ready) init_win_luts(g_win_scale);

        // fb pointer already obtained at the top of draw() — reuse it.
        // (getCurrentFramebuffer() always returns the same pointer within a frame.)
        const int  scale   = g_win_scale;
        const bool is565   = g_fb_is_rgb565;
        const bool prpk    = g_fb_is_prepacked;
        const bool do_grid = g_lcd_grid && scale >= 2;

        // Scale 1: LCD grid has no visible effect at 1× (do_grid is always false
        // because do_grid = g_lcd_grid && scale >= 2).  Handle it directly here
        // so the compiler never needs to instantiate launch_win_blit<true,1> /
        // win_blit_rows<true,1> — those are dead code at scale 1.
        // NOTE: no early return here — the post-blit setLayerPos / overlay draws
        // must run for scale 1 exactly as they do for all other scales.
        if (scale == 1) {
            launch_win_blit<false, 1>(fb, g_gb_fb, is565, prpk);
        } else {
            // Resolve SCALE at compile time so inner loops are fully unrolled.
            // LCD_GRID branches are compile-time eliminated in each instantiation.
            // case 1 is gone — handled above, so the GRID=true lambda never
            // references launch_win_blit<true,1>.
            const auto dispatch = [&]<bool GRID>() __attribute__((always_inline)) {
                switch (scale) {
                    case 2: launch_win_blit<GRID, 2>(fb, g_gb_fb, is565, prpk); break;
                    case 3: launch_win_blit<GRID, 3>(fb, g_gb_fb, is565, prpk); break;
                    case 4: launch_win_blit<GRID, 4>(fb, g_gb_fb, is565, prpk); break;
                    case 5: launch_win_blit<GRID, 5>(fb, g_gb_fb, is565, prpk); break;
                    case 6: launch_win_blit<GRID, 6>(fb, g_gb_fb, is565, prpk); break;
                    default: launch_win_blit<GRID, 2>(fb, g_gb_fb, is565, prpk); break;
                }
            };
            if (do_grid) dispatch.operator()<true>();
            else         dispatch.operator()<false>();
        }

        // ── Sync layer position with the just-rendered FB ────────────────────
        // Called here, after the blit, so the layer VI position and y_ofs are
        // always derived from the same g_win_pos_y in the same frame.
        // Keeping setLayerPos out of handleInput() prevents the double-buffer
        // desync that occurs when viSetLayerPosition fires on the currently-
        // displayed (front) buffer before draw() has updated the back buffer.
        tsl::gfx::Renderer::get().setLayerPos(
            static_cast<u32>(g_win_pos_x),
            static_cast<u32>(win_layer_vi_y()));

        // ── Pass-through flash border + drag overlay ─────────────────────────
        // fw/fh: game content dimensions in FB pixels.
        // y_ofs: FB row where game content starts (0 for top anchor when game
        //        is at screen top; nonzero when the game has been dragged down).
        // All drawRect calls are offset by y_ofs so they land on the game region,
        // not the transparent padding rows at the top/bottom of the FB.
        const s32 fw = static_cast<s32>(GB_W * g_win_scale);
        const s32 fh = static_cast<s32>(GB_H * g_win_scale);

        draw_focus_flash(renderer, 0, y_ofs, fw, fh);

        // ── Reposition overlay ────────────────────────────────────────────────
        // While dragging: dim the frozen frame and show "Paused" centred.
        // draw_drag_dim_border (gb_renderer.h) handles dim + red border + text.
        // Text is centred within the full game-content rect (x=0, y=y_ofs, fw×fh).
        if (s_win_dragging)
            draw_drag_dim_border(renderer,
                y_ofs, fw, fh,
                0, y_ofs, fw, fh,
                static_cast<u32>(14 * g_win_scale));

#ifdef GB_FPS
        // ── FPS indicator ─────────────────────────────────────────────────────
        // Counts display frames rendered in the past second and overlays the
        // result as a small decimal in the top-left corner of the game window.
        //
        // Compiled in ONLY when -DGB_FPS is supplied to the build; the #ifdef
        // guarantees zero code, zero data, and zero runtime cost in production.
        //
        // Placement: (2, y_ofs + font_sz + 1) puts the baseline just inside the
        // top-left of the game-content region regardless of anchor or scale.
        // Font size grows linearly with g_win_scale (8 px at 1×, 18 px at 5×)
        // so the label is always legible without overflowing a narrow window.
        {
            static uint64_t s_fps_win_start   = 0;   // ns timestamp of window open
            static int      s_fps_frame_cnt   = 0;   // frames counted this second
            static int      s_fps_display_x10 = 0;   // last completed 1 s measurement ×10

            const uint64_t now = ult::nowNs();
            if (s_fps_win_start == 0) s_fps_win_start = now;

            ++s_fps_frame_cnt;
            if (now - s_fps_win_start >= 1'000'000'000ULL) {
                // Divide by actual elapsed ns, not a nominal 1 s, so a window
                // that ran 1.017 s with 61 ticks still resolves to ~60.0 fps.
                // Store FPS × 10 for one decimal place without using floats.
                const uint64_t elapsed = now - s_fps_win_start;
                s_fps_display_x10 = static_cast<int>(
                    (static_cast<uint64_t>(s_fps_frame_cnt) * 10ULL * 1'000'000'000ULL + elapsed / 2ULL) / elapsed);
                s_fps_frame_cnt = 0;
                s_fps_win_start = now;
            }

            // Stack-only fixed-point formatter — no heap, no <cstdio> overhead.
            // Formats:
            //   6.0
            //  59.7
            // 100.0
            char buf[8];
            int  v     = s_fps_display_x10;
            int  whole = v / 10;
            int  frac  = v % 10;
            int  len   = 0;

            if (whole >= 100) {
                buf[len++] = '0' + whole / 100;
                whole %= 100;
                buf[len++] = '0' + whole / 10;
                buf[len++] = '0' + whole % 10;
            } else if (whole >= 10) {
                buf[len++] = '0' + whole / 10;
                buf[len++] = '0' + whole % 10;
            } else {
                buf[len++] = '0' + whole;
            }
            buf[len++] = '.';
            buf[len++] = '0' + frac;
            buf[len]   = '\0';

            // Font size: 8 px at scale 1, +2 px per scale step (18 px at scale 5).
            const s32 font_sz = 6 + g_win_scale * 2;

            // Semi-transparent black shadow one pixel down-right for contrast,
            // then the yellow label on top.
            const s32 tx = 3;
            const s32 ty = y_ofs + font_sz + 2;
            renderer->drawString(buf, false, tx + 1, ty + 1, font_sz,
                                 tsl::Color{0x0, 0x0, 0x0, 0xA});  // shadow
            renderer->drawString(buf, false, tx,     ty,     font_sz,
                                 tsl::Color{0xF, 0xE, 0x0, 0xF});  // yellow
        }
#endif  // GB_FPS
    }

    void layout(u16, u16, u16, u16) override {
        // The element covers the entire allocated framebuffer — game content rows
        // plus the transparent padding rows above/below determined by the anchor.
        // Width = GB_W*scale, Height = win_fb_height() (half-screen + half-game).
        setBoundaries(0, 0, GB_W * g_win_scale, win_fb_height());
    }

    tsl::elm::Element* requestFocus(tsl::elm::Element*, tsl::FocusDirection) override {
        return nullptr;  // leaf — no focusable content
    }
};

// =============================================================================
// GBWindowedFrame
// Minimal owning frame for the scaled windowed layer.
//
// Rationale: createUI() must return an Element that Tesla owns via m_topElement.
// Returning GBWindowedElement bare works for rendering but leaves ownership and
// layout semantics ambiguous.  This thin wrapper:
//   • Holds RAII ownership of GBWindowedElement (deleted in destructor).
//   • Passes layout through so the element fills the entire framebuffer.
//   • Returns nullptr from requestFocus (no footer chrome, no nav).
//   • Returns false from onTouch — ALL touch is handled in
//     GBWindowedGui::handleInput() via direct HID polling.
//   • Draws no chrome whatsoever: no background fill, no header, no footer.
//     The entire layer is the game image.
// =============================================================================
class GBWindowedFrame : public tsl::elm::Element {
public:
    ~GBWindowedFrame() override { delete m_content; }

    // ── Rendering ─────────────────────────────────────────────────────────────
    void __attribute__((optimize("O3"))) draw(tsl::gfx::Renderer* renderer) override {
        // No chrome.  Just render the game element.
        if (m_content) m_content->frame(renderer);
    }

    // ── Layout ────────────────────────────────────────────────────────────────
    void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override {
        // The frame occupies the entire framebuffer.
        setBoundaries(parentX, parentY, parentWidth, parentHeight);
        // Let the content element do its own layout.
        if (m_content) m_content->invalidate();
    }

    // ── Focus / touch ─────────────────────────────────────────────────────────
    tsl::elm::Element* requestFocus(tsl::elm::Element*, tsl::FocusDirection) override {
        return nullptr;  // no focusable items in windowed mode
    }

    bool onTouch(tsl::elm::TouchEvent, s32, s32, s32, s32, s32, s32) override {
        // Intentionally suppress element-tree touch routing.
        // All touch logic lives in GBWindowedGui::handleInput() via HID polling.
        return false;
    }

    // ── Content ownership ─────────────────────────────────────────────────────
    void setContent(tsl::elm::Element* el) {
        delete m_content;
        m_content = el;
        if (el) {
            el->setParent(this);
            invalidate();
        }
    }

private:
    tsl::elm::Element* m_content = nullptr;  ///< Owning — deleted in destructor.
};

// =============================================================================
// GBWindowedGui
// =============================================================================
class GBWindowedGui : public tsl::Gui {

    // ── Hold-to-drag state ────────────────────────────────────────────────────
    bool m_hold_armed    = false;  // touch landed inside the window
    bool m_dragging      = false;  // hold threshold passed; actively repositioning
    int  m_hold_frames   = 0;      // frames since finger-down (resets on lift/cancel)
    int  m_touch_start_x = 0;      // HID x where the hold began (touch space)
    int  m_touch_start_y = 0;      // HID y where the hold began (touch space)
    int  m_pos_start_x   = 0;      // g_win_pos_x when hold began (VI space)
    int  m_pos_start_y   = 0;      // g_win_pos_y when hold began (VI space)
    bool m_prev_touching = false;  // touch state last frame (from HID poll)
    bool m_restoreHapticState = false;
    bool runOnce = true;


    // ── KEY_PLUS 2s hold → joystick reposition state ─────────────────────────
    bool     m_plus_dragging      = false;  // actively repositioning via joystick
    bool     m_plus_armed         = false;  // KEY_PLUS held alone; timer running
    uint64_t m_plus_hold_start_ns = 0;      // ns timestamp when hold began
    float    m_joy_acc_x          = 0.f;    // sub-pixel accumulator, VI-space X
    float    m_joy_acc_y          = 0.f;    // sub-pixel accumulator, VI-space Y
    uint64_t m_joy_last_ns        = 0;      // timestamp of previous joystick-active frame (for dt scaling)

    // 60 frames ≈ 1 second at the GB's 59.73 Hz render rate.
    static constexpr int      HOLD_FRAMES  = kHoldFrames;
    // KEY_PLUS must be held alone for this long before joystick drag activates.
    static constexpr uint64_t PLUS_HOLD_NS = kPlusHoldNs;
    // Joystick deadzone (HidAnalogStickState range: –32767..32767).
    static constexpr int      JOY_DEADZONE = kJoyDeadzone;
    // Mask of all physical buttons — used to confirm KEY_PLUS is held *alone*.
    
    bool     m_zr_first_seen  = false;
    uint32_t m_zr_first_frame = 0;

    // ZL double-click-hold: background pass-through toggle.
    ZLPassThroughState m_zl_state;  // pass_through, first/second seen/frame

    // True until the first frame where no keys AND no touch are active.
    // Prevents the ROM-tap touch (or any button held at launch) from
    // accidentally arming the hold timer on the very first frame.
    bool m_waitForRelease = true;

    bool m_load_failed = false;

    // VI bounds (scale-dependent) — derived from cfg::LayerWidth/Height.
    // vi_max_x() / touch_win_w() are shared free functions in gb_globals.hpp.
    // vi_max_y() intentionally omitted: windowed Y repositioning uses vi_max_y_content().
    // Game content height in HID touch space.
    //   720p:  touch = GB_H*scale   1080p: touch = GB_H*scale * 2/3
    static int touch_win_h() {
        return ult::windowedLayerPixelPerfect
            ? (GB_H * g_win_scale * 2 / 3)
            : (GB_H * g_win_scale);
    }

public:
    ~GBWindowedGui() {
        ult::noClickableItems.store(false, std::memory_order_release);
    }

    // ── createUI ─────────────────────────────────────────────────────────────
    tsl::elm::Element* createUI() override {
        audio_exit_if_enabled();

        // Ensure screenshot do nots work.
        //screenshotsAreDisabled.store(true, std::memory_order_release);
        //screenshotsAreForceDisabled.store(true, std::memory_order_release);
        //tsl::gfx::Renderer::get().removeScreenshotStacks();


        // Load the ROM.  g_pending_rom_path was set from windowed_rom in
        // WindowedOverlay::loadInitialGui().
        consume_pending_rom(m_load_failed);

        g_emu_active     = !m_load_failed;
        m_waitForRelease = !m_load_failed;

        // In limited-memory mode (4 MB heap) the FB is exactly game-sized and the
        // anchor-padding trick that makes screenshots work is not used.  The
        // screenshot layer would display garbage, so remove it entirely.
        if (g_win_limited_fb) {
            screenshotsAreDisabled.store(true, std::memory_order_release);
            screenshotsAreForceDisabled.store(true, std::memory_order_release);
            tsl::gfx::Renderer::get().removeScreenshotStacks();
        }

        // Suppress Tesla's footer touch handling entirely.
        ult::noClickableItems.store(true,  std::memory_order_release);
        ult::backWidth.store(0.0f,         std::memory_order_release);
        ult::selectWidth.store(0.0f,       std::memory_order_release);
        ult::nextPageWidth.store(0.0f,     std::memory_order_release);
        ult::halfGap.store(0.0f,           std::memory_order_release);
        ult::hasNextPageButton.store(false, std::memory_order_release);

        // Clamp to the valid range for the current anchor mode and scale.
        {
            const int mx  = vi_max_x();
            const int mny = vi_min_y_content();
            const int mxy = vi_max_y_content();
            g_win_pos_x = std::max(0,   std::min(mx,  g_win_pos_x));
            g_win_pos_y = std::max(mny, std::min(mxy, g_win_pos_y));
        }

        // Initial layer position before the first draw().  After that, draw()
        // calls setLayerPos each frame so it stays in sync with the FB y_ofs.
        tsl::gfx::Renderer::get().setLayerPos(
            static_cast<u32>(g_win_pos_x),
            static_cast<u32>(win_layer_vi_y()));
        sync_notif_touch_offsets();

        // Initialise the persistent blit thread pool once per windowed session.
        // Creates 3 worker threads that sleep between frames — no CPU cost while
        // idle.  Avoids per-frame std::thread construction (200–600 µs overhead).
        if (!s_win_pool_active) {
            s_win_pool.init();
            s_win_pool_active = true;
        }

        // Minimal frame: no chrome, owns GBWindowedElement.
        auto* frame = new GBWindowedFrame();
        frame->setContent(new GBWindowedElement());
        return frame;
    }

    // ── update ───────────────────────────────────────────────────────────────
    void update() override {
        if (m_load_failed) {
            m_load_failed = false;
            if (g_self_path[0])
                tsl::setNextOverlay(std::string(g_self_path));
            tsl::Overlay::get()->close();
        }
        // Relaunch with 720p sizing when console undocks while in 1080p mode.
        // exitServices() (called automatically on close) saves SRAM and audio
        // snapshot so the restarted session resumes exactly where it left off.
        // windowedLayerPixelPerfect will be false on next launch because
        // consoleIsDocked() returns false, giving correct 720p VI sizing.
        if (s_undock_relaunch) {
            s_undock_relaunch = false;
            if (g_gb.romPath[0])
                ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWindowedRom,
                                     std::string(g_gb.romPath), "");
            if (g_win_quick_exit)
                ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "1", "");
            launchComboHasTriggered.store(true, std::memory_order_release);
            if (g_self_path[0])
                tsl::setNextOverlay(std::string(g_self_path), "-windowed");
            tsl::Overlay::get()->close();
        }
        run_once_setup(runOnce, m_restoreHapticState);

        // Periodic background-title volume correction — same safety net as
        // GBOverlayGui::update().  See that comment for full rationale.
        // Runs every 120 frames (~2 s); u8 wrap at 256 is harmless.
        // Suppressed while pass-through is active: the title volume was
        // intentionally lifted to 1.0f by gb_game_vol_suppress() and must not
        // be re-lowered until foreground is reclaimed.
        static u8 s_vol_recheck_ctr = 0;
        if (++s_vol_recheck_ctr >= 120) {
            s_vol_recheck_ctr = 0;
            if (!m_zl_state.pass_through)
                gb_game_vol_recheck();
        }
    }

    // ── handleInput ──────────────────────────────────────────────────────────
    bool __attribute__((optimize("O3"))) handleInput(u64 keysDown, u64 keysHeld,
                     const HidTouchState& /*touchPos*/,  // ignored — we poll HID directly
                     HidAnalogStickState leftJoy, HidAnalogStickState) override {

        // ── Wait for launch touch and keys to fully clear ─────────────────────
        if (m_waitForRelease) {
            int _tx, _ty;
            const bool touch_still_down = poll_touch(_tx, _ty);
            if (keysHeld || touch_still_down) return true;
            g_touch_keys     = 0;
            m_waitForRelease = false;
        }

        // ── Launch combo: exit windowed mode, return to normal UltraGB ────────
        if (combo_pressed(keysDown, keysHeld)) {
            restore_haptic_if_needed(m_restoreHapticState);

            // Quick-exit mode (triggered via Quick Combo): close the overlay
            // entirely without relaunching the normal UltraGB UI.
            // Normal mode: return to UltraGB via setNextOverlay so it can
            // restore the Settings scroll position.
            // Use the synchronous standalone call — triggerExitFeedback() sets
            // an atomic for the background feedback thread, but close() stops
            // that thread before it gets a chance to process it, so the rumble
            // would never fire.  rumbleDoubleClickStandalone() fires immediately,
            // matching exactly what Tesla does at its own directMode exit path.

            // Suppress Tesla's own directMode close-time double-click
            // (tesla.hpp fires rumbleDoubleClickStandalone() on close when
            // directMode=true and launchComboHasTriggered=false — session 1
            // gets directMode from Ultrahand's --direct flag).  We fire our
            // own exit feedback above, so we don't want a second one.
            gb_audio_pause();

            launchComboHasTriggered.store(true, std::memory_order_release);

            // Reset the settings scroll only for genuine exits — quick-exit mode
            // (g_win_quick_exit, triggered by the Quick Combo fast path) and
            // direct mode — where pressing the combo means "leave entirely".
            //
            // In normal windowed mode (g_win_quick_exit=false, g_directMode=false)
            // this combo returns to the ROM selector via -returning, exactly as
            // X/back does in overlay mode.  The scroll position must survive so
            // SettingsGui lands at the right item on the next RIGHT-press.
            // exitServices() will write the intact g_settings_scroll to INI, and
            // the -returning process's initServices() will restore it.
            if (g_win_quick_exit || g_directMode) {
                g_settings_scroll[0] = '\0';
                ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll, "", "");
            }

            if (!g_win_quick_exit && g_self_path[0]) {
                const std::string returnArg = g_directMode ? "-returning --direct" : "-returning";
                tsl::setNextOverlay(std::string(g_self_path), returnArg);
            }

            tsl::Overlay::get()->close();
            
            return true;
        }

        // ── ZR double-click-hold: fast-forward ────────────────────────────────
        // pass-through guard applied before calling the shared helper so that
        // the background app owns ZR undisturbed when pass-through is active.
        {
            const bool zr_down = !m_zl_state.pass_through && (keysDown & KEY_ZR);
            const bool zr_held = !m_zl_state.pass_through && (keysHeld & KEY_ZR);
            process_zr_fast_forward(zr_down, zr_held, m_zr_first_seen, m_zr_first_frame);
        }

        // ── Physical buttons → GB joypad ──────────────────────────────────────
        // Suppress all input while any reposition mode is active (touch drag or
        // joystick drag) so the game never sees buttons held during repositioning.
        // Also suppress when pass-through is active: foreground has been released
        // so the background app owns HID natively; we must not double-route input.
        if (!m_dragging && !m_plus_dragging && !m_zl_state.pass_through) {
            gb_set_input(keysHeld | keysDown);
            if (g_button_haptics &&
                (keysDown & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_PLUS | KEY_MINUS |
                             KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)))
                triggerRumbleClick.store(true, std::memory_order_release);
        }

        // ── ZL double-click-hold: toggle background pass-through ──────────────
        // Double-tap ZL (second tap within ~333 ms) then hold ≈300 ms to commit.
        // requestForeground(false) releases HID to the background app;
        // requestForeground(true) reclaims it.  The coloured flash border is
        // written to g_focus_flash / g_focus_flash_red by the helper and drawn
        // in GBWindowedElement::draw().  The d-pad keys are excluded from the
        // ZL-alone guard because they are separate physical axes, not buttons.
        //
        // Audio coupling: on foreground loss the GB audio is silenced and the
        // background Switch title's volume is lifted back to 1.0f so the user
        // can hear it normally.  On foreground regain the GB audio is restored
        // and gb_game_vol_recheck() re-applies g_game_volume to the title.
        {
            const bool zl_down = ((keysDown & KEY_ZL) && !(keysHeld & ~KEY_ZL & (ALL_KEYS_MASK | KEY_DOWN | KEY_UP | KEY_RIGHT | KEY_LEFT)));
            const bool zl_held = ((keysHeld  & KEY_ZL) && !(keysHeld & ~KEY_ZL & (ALL_KEYS_MASK | KEY_DOWN | KEY_UP | KEY_RIGHT | KEY_LEFT)));
            const bool was_pass_through = m_zl_state.pass_through;
            process_zl_pass_through(zl_down, zl_held, m_zl_state);
            process_home_foreground_release(m_zl_state);
            if (!was_pass_through && m_zl_state.pass_through) {
                // Foreground just released — silence GB, restore title volume.
                gb_audio_pause();
                gb_game_vol_suppress();
            } else if (was_pass_through && !m_zl_state.pass_through) {
                // Foreground just reclaimed — un-silence GB, re-apply title volume.
                gb_audio_resume();
                gb_game_vol_recheck();
            }
        }

        // ── Right/Left stick click: resize window ────────────────────────────
        // Right stick click steps up one scale; left stick click steps down.
        // Cycles 1× → 2× → 3× → 4× → 5× → (6×) capped by heap tier and mode.
        //   4 MB heap  : max 3×
        //   6 MB heap  : max 4×
        //   8 MB+ heap : max 5×  (or 6× when expandedMemory + docked + 1080p)
        //
        // Both are disabled while pass-through is active so the background
        // app receives the stick click undisturbed.
        // Relaunch: writes the new scale + current ROM path to config.ini
        // and restarts in windowed mode; the current game state is saved
        // automatically via exitServices() → gb_unload_rom().
        if (!m_zl_state.pass_through && !m_dragging && !m_plus_dragging) {
            const bool rstick = (keysDown & KEY_RSTICK && !(keysHeld & ~KEY_RSTICK & ALL_KEYS_MASK));
            const bool lstick = (keysDown & KEY_LSTICK && !(keysHeld & ~KEY_LSTICK & ALL_KEYS_MASK));
            if (rstick || lstick) {
                // Heap tier is fixed for the session — use the flags Tesla already
                // set in tsl::loop() instead of re-querying getCurrentHeapSize().
                // earlyExpanded means exactly 8 MB (not 10 MB+), matching the
                // pre-loop earlyExpanded naming convention used in main().
                const bool earlyLimited         = ult::limitedMemory;
                const bool earlyRegularMemory   = !ult::expandedMemory && !ult::limitedMemory;
                const bool earlyExpanded        = ult::expandedMemory && !ult::furtherExpandedMemory;
                const bool earlyFurtherExpanded = ult::furtherExpandedMemory;

                // 6× is only available when expandedMemory + docked + 1080p pixel-perfect.
                // On plain 8 MB heap (not furtherExpandedMemory), also requires ROM < 4 MB.
                const bool romSmall = earlyExpanded
                    ? (get_rom_size(g_gb.romPath) < kROM_4MB)
                    : true;  // furtherExpandedMemory or smaller tiers — no extra ROM-size gate
                const int max_scale = earlyLimited              ? 3
                                    : earlyRegularMemory        ? 4
                                    : (earlyExpanded && !romSmall) ? 4  // 8 MB + 4 MB+ ROM — not enough heap for scale 5
                                    : ((earlyFurtherExpanded || (earlyExpanded && romSmall)) && poll_console_docked() && ult::windowedLayerPixelPerfect) ? 6
                                    :                              5;
                const int new_scale = rstick
                    ? std::min(g_win_scale + 1, max_scale)
                    : std::max(g_win_scale - 1, 1);
                if (new_scale != g_win_scale) {
                    // Persist new scale and ROM path so the relaunch picks them up.
                    const char* sv =
                        (new_scale == 6) ? "6" :
                        (new_scale == 5) ? "5" :
                        (new_scale == 4) ? "4" :
                        (new_scale == 3) ? "3" :
                        (new_scale == 2) ? "2" : "1";
                    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinScale, sv, "");
                    if (g_gb.romPath[0])
                        ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWindowedRom,
                                             std::string(g_gb.romPath), "");
                    // Re-propagate the quick-exit flag so the relaunched windowed
                    // session still exits cleanly on the combo instead of returning
                    // to the UltraGB settings menu.  initServices() already cleared
                    // it from config on this session's launch, so we must write it
                    // again before each resize relaunch.
                    if (g_win_quick_exit)
                        ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "1", "");
                    // Simple click — confirms the scale step without implying exit.
                    // triggerNavigationFeedback (navigation sound + rumble) feels
                    // exit-like right before the overlay closes; a bare click is
                    // unambiguous and consistent with other button confirmations.
                    triggerRumbleClick.store(true, std::memory_order_release);
                    // Suppress Tesla's directMode close-time double-click rumble.
                    // tesla.hpp fires rumbleDoubleClickStandalone() at process exit
                    // when directMode=true and launchComboHasTriggered=false.
                    // Session 1 (launched by Ultrahand with --direct) would produce
                    // an unwanted double-click on the very first resize; subsequent
                    // sessions have directMode=false so they're unaffected.  Setting
                    // this flag here suppresses that extra rumble in all sessions.
                    launchComboHasTriggered.store(true, std::memory_order_release);
                    if (g_self_path[0])
                        tsl::setNextOverlay(std::string(g_self_path), "-windowed");
                    tsl::Overlay::get()->close();
                    return true;
                }
                // Already at the limit — swallow the press so nothing fires.
                return true;
            }
        }

        // ── Touch hold-to-drag (via direct HID poll) ──────────────────────────
        // Coordinate spaces:
        //   Touch: 0..1279 × 0..719  (HID native, always fixed)
        //   VI:    0..1919 × 0..1079 (always fixed, touch→VI is ×3/2)
        //
        // Window in touch space:
        //   origin: (g_win_pos_x*2/3, g_win_pos_y*2/3)   [VI pos ÷ 1.5, always]
        //   size:   touch_win_w() × touch_win_h()
        //             720p mode:  GB_W*scale  × GB_H*scale   (layer = FB×1.5 → touch = layer×2/3 = FB)
        //             1080p mode: GB_W*scale*2/3 × GB_H*scale*2/3  (layer = FB×1.0 → touch = FB×2/3)
        //
        // Converting touch delta → VI delta:  delta_vi = delta_touch * 3/2  (always, both modes)
        {
            int tx = 0, ty = 0;
            const bool touching = poll_touch(tx, ty);

            // Window top-left in touch space (VI pos ÷ 1.5)
            const int win_tx = (g_win_pos_x * 2) / 3;
            const int win_ty = (g_win_pos_y * 2) / 3;

            // Window footprint in touch space — scales with g_win_scale.
            const int win_w = touch_win_w();
            const int win_h = touch_win_h();

            // Is the current touch point inside our window?
            const bool in_window = touching
                && tx >= win_tx && tx < win_tx + win_w
                && ty >= win_ty && ty < win_ty + win_h;

            // ── Finger-down inside window: arm the hold timer ─────────────────
            if (!m_prev_touching && touching && in_window) {
                m_hold_armed    = true;
                m_dragging      = false;
                m_hold_frames   = 0;
                m_touch_start_x = tx;
                m_touch_start_y = ty;
                m_pos_start_x   = g_win_pos_x;
                m_pos_start_y   = g_win_pos_y;
            }

            // ── Finger held: tick timer; enter/continue drag ──────────────────
            if (m_hold_armed && touching) {
                ++m_hold_frames;

                if (!m_dragging && m_hold_frames >= HOLD_FRAMES) {
                    m_dragging      = true;
                    s_win_dragging  = true;
                    gb_audio_pause();
                    // Re-anchor to wherever the finger is right now so the
                    // window doesn't jump from any drift during the hold period.
                    m_touch_start_x = tx;
                    m_touch_start_y = ty;
                    m_pos_start_x   = g_win_pos_x;
                    m_pos_start_y   = g_win_pos_y;
                    if (g_touch_haptics) triggerNavigationFeedback();  // haptic: drag started
                }

                if (m_dragging) {
                    const int dx     = (tx - m_touch_start_x) * 3 / 2;
                    const int dy     = (ty - m_touch_start_y) * 3 / 2;
                    const int nx     = std::max(0, std::min(vi_max_x(), m_pos_start_x + dx));
                    const int ny     = std::max(vi_min_y_content(),
                                                std::min(vi_max_y_content(), m_pos_start_y + dy));

                    if (nx != g_win_pos_x || ny != g_win_pos_y) {
                        g_win_pos_x = nx;
                        g_win_pos_y = ny;
                        // Layer position is updated in draw() after the blit
                        // so VI position and FB y_ofs are always in sync.
                        sync_notif_touch_offsets();
                    }
                }

                // Finger drifted out of the window before the threshold: cancel.
                // (Once dragging is active the finger can be anywhere on screen.)
                if (!m_dragging && !in_window) {
                    m_hold_armed  = false;
                    m_hold_frames = 0;
                }
            }

            // ── Finger-up: save position if we were dragging ──────────────────
            if (m_prev_touching && !touching) {
                if (m_dragging) {
                    save_win_pos();        // persist VI coords to config.ini
                    if (g_touch_haptics) triggerExitFeedback(); // haptic: position locked
                    gb_audio_resume();
                    g_gb_frame_next_ns = 0;  // don't try to catch up after pause
                }
                s_win_dragging = false;
                m_hold_armed  = false;
                m_dragging    = false;
                m_hold_frames = 0;
            }

            m_prev_touching = touching;
        }

        // ── KEY_PLUS 2s hold → joystick reposition ───────────────────────────
        // Holding KEY_PLUS alone for 2 s arms joystick repositioning.  Once active
        // the left stick moves the window in VI space with sub-pixel accumulation
        // and an x^8 sensitivity curve (same as Mini).  Releasing KEY_PLUS saves
        // the position and exits, identical to the touch-drag path.
        {
            const bool plus_only = (keysHeld & KEY_PLUS)
                && !(keysHeld & ~KEY_PLUS & ALL_KEYS_MASK);

            if (plus_only) {
                if (!m_plus_armed) {
                    // KEY_PLUS just pressed alone — record timestamp.
                    m_plus_armed         = true;
                    m_plus_hold_start_ns = ult::nowNs();
                }

                if (!m_plus_dragging) {
                    // Check whether the 2 s threshold has been reached.
                    if (ult::nowNs() - m_plus_hold_start_ns >= PLUS_HOLD_NS) {
                        m_plus_dragging = true;
                        s_win_dragging  = true;
                        gb_audio_pause();
                        m_joy_acc_x     = 0.f;
                        m_joy_acc_y     = 0.f;
                        m_joy_last_ns   = 0;      // will be anchored on first active frame
                        if (g_touch_haptics) triggerNavigationFeedback(); // haptic: drag started
                    }
                }

                if (m_plus_dragging) {
                    // Move the window with the left stick.
                    // x^8 sensitivity curve: stays fine at low deflection, accelerates near full.
                    if (std::abs(leftJoy.x) > JOY_DEADZONE || std::abs(leftJoy.y) > JOY_DEADZONE) {
                        const float fx = static_cast<float>(leftJoy.x);
                        const float fy = static_cast<float>(leftJoy.y);
                
                        // x^8 sensitivity curve — same as overlay mode.
                        const float sens = joy_sens(fx, fy);
                
                        // Delta-time scaling: sens values are tuned for 60 fps.
                        // Multiply by (actual_dt * 60) so the real-world velocity
                        // is constant regardless of frame rate — at 60 fps the factor
                        // is exactly 1.0 (no change in feel); at lower frame rates
                        // each frame contributes proportionally more so the window
                        // moves at the same speed it would at full 60 fps.
                        // Clamped to [1/4 frame .. 4 frames] to guard against
                        // spurious spikes (first frame, timer hiccup, etc.).
                        const uint64_t now_ns = ult::nowNs();
                        if (m_joy_last_ns == 0) m_joy_last_ns = now_ns;
                        const float dt_ns = static_cast<float>(now_ns - m_joy_last_ns);
                        const float dt_factor = std::max(0.25f, std::min(4.0f, dt_ns * (60.0f / 1e9f)));
                        m_joy_last_ns = now_ns;
                
                        // Accumulate fractional VI-space movement; Y axis is inverted
                        // (stick up → negative y → window moves up).
                        m_joy_acc_x += fx * sens * dt_factor;
                        m_joy_acc_y += -fy * sens * dt_factor;
                
                        const int dx = static_cast<int>(m_joy_acc_x);
                        const int dy = static_cast<int>(m_joy_acc_y);
                        m_joy_acc_x -= static_cast<float>(dx);
                        m_joy_acc_y -= static_cast<float>(dy);
                
                        const int nx     = std::max(0, std::min(vi_max_x(), g_win_pos_x + dx));
                        const int ny     = std::max(vi_min_y_content(),
                                                    std::min(vi_max_y_content(), g_win_pos_y + dy));

                        if (nx != g_win_pos_x || ny != g_win_pos_y) {
                            g_win_pos_x = nx;
                            g_win_pos_y = ny;
                            // Layer position updated in draw() after blit.
                            sync_notif_touch_offsets();
                        }
                    } else {
                        // Stick returned to deadzone — reset the timestamp so the
                        // next active frame doesn't accumulate a long idle gap.
                        m_joy_last_ns = 0;
                    }
                }
            } else {
                // KEY_PLUS released (or another key was combined with it).
                if (m_plus_armed) {
                    if (m_plus_dragging) {
                        save_win_pos();        // persist VI coords to config.ini
                        if (g_touch_haptics) triggerExitFeedback(); // haptic: position locked
                        gb_audio_resume();
                        g_gb_frame_next_ns = 0;  // don't try to catch up after pause
                        s_win_dragging  = false;
                        m_plus_dragging = false;
                    }
                    m_plus_armed         = false;
                    m_plus_hold_start_ns = 0;
                }
            }
        }

        // Swallow input while any drag mode is active so Tesla doesn't fire
        // footer button events.
        return m_dragging || m_plus_dragging;
    }
};

#pragma GCC pop_options