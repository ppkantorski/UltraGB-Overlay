/********************************************************************************
 * File: gb_overlay.hpp
 * Description:
 *   GBOverlayElement / GBOverlayGui — in-game overlay for UltraGB.
 *
 *   GBOverlayElement handles all drawing: background fill, wallpaper, the
 *   animated "UltraGB" title, GB frame rendering (timing, fast-forward,
 *   LCD ghosting), and the virtual on-screen button layout (D-pad, A, B,
 *   Start, Select).
 *
 *   GBOverlayGui owns all input: waits for button release after launch,
 *   handles the overlay close / X-return-to-selector paths, screen-region
 *   tap and RS quick-release for scale toggle, ZR double-click-hold for
 *   fast-forward, and maps touch regions to GB key bits each frame.
 *
 *   Included at the end of main.cpp (after draw_ultragb_title and all
 *   globals/helpers are fully defined), exactly like gb_windowed.hpp.
 *   Must come before class Overlay so loadInitialGui() can instantiate
 *   GBOverlayGui via initially<GBOverlayGui>().
 *
 *  Licensed under GPLv2
 *  Copyright (c) 2026 ppkantorski
 ********************************************************************************/

#pragma once

// Per-function attributes override this file-level Os baseline where needed.
// Hot per-frame functions carry __attribute__((optimize("O3"))).
// Cold GUI / setup functions inherit Os from the push below.
#pragma GCC push_options
#pragma GCC optimize("Os")

// GBOverlayGui never calls swapTo — all navigation is done via
// setNextOverlay + close(), exactly as GBWindowedGui does.
// No forward declarations needed here.

// -- Shared emulation clock tick ----------------------------------------------
// Defined here so it sees the static globals declared in main.cpp:
//   g_frame_count, g_gb_frame_next_ns, GB_RENDER_FRAME_NS, g_fast_forward.
// gb_windowed.hpp is #included after this file in main.cpp and therefore
// sees this definition automatically — no duplication, one copy in .text.
//
// Increments the display-frame counter and rate-limits the GB CPU to its
// true 59.73 fps clock regardless of the 60 fps display vsync.
// See GBOverlayElement::draw() for the full timing rationale.
//#ifndef GB_TICK_FRAME_DEFINED
//#define GB_TICK_FRAME_DEFINED
inline void __attribute__((optimize("O3"))) gb_tick_frame() {
    ++g_frame_count;
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    const int64_t now_ns = (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;
    if (g_gb_frame_next_ns == 0)
        g_gb_frame_next_ns = now_ns;  // anchor on first draw after load/resume
    if (g_fast_forward) {
        for (int f = 0; f < 4; ++f) gb_run_one_frame();
        g_gb_frame_next_ns = now_ns + GB_RENDER_FRAME_NS;
    } else if (now_ns >= g_gb_frame_next_ns) {
        gb_run_one_frame();
        apply_lcd_ghosting();
        gb_audio_submit();
        g_gb_frame_next_ns += GB_RENDER_FRAME_NS;
        if (g_gb_frame_next_ns < now_ns)
            g_gb_frame_next_ns = now_ns + GB_RENDER_FRAME_NS;
    }
}
//#endif

// Set true by GBOverlayGui::handleInput while a free-overlay touch-drag or
// joystick reposition is active.  Read by GBOverlayElement::draw to overlay
// the "Paused" dim/border/text — identical to s_win_dragging in windowed mode.
static bool s_ovl_free_dragging = false;

// =============================================================================
// GBOverlayElement — drawing only; no input handling (input lives on Gui)
// =============================================================================
class GBOverlayElement : public tsl::elm::Element {
public:
    virtual void __attribute__((optimize("O3"))) draw(tsl::gfx::Renderer* renderer) override {
        // In free overlay mode set the Y render offset BEFORE any draw call so
        // init_outer_lut() (called lazily inside render_gb_screen / letterbox)
        // builds the row LUT with the trimmed viewport base.  Reset to 0 in
        // normal overlay mode so the two modes never bleed into each other.
        g_render_y_offset = g_overlay_free_mode ? (g_ovl_free_pos_y - (int)OVL_FREE_TOP_TRIM) : 0;

        // Cache values read repeatedly throughout this function.
        //   free_mode    — g_overlay_free_mode is read 8+ times; a const local
        //                  guarantees one load and lets the compiler freely
        //                  propagate the value without aliasing concerns.
        //   has_wallpaper — avoids re-evaluating the conjunction twice.
        //   fb16          — getCurrentFramebuffer() is otherwise called 4 times
        //                   (pre_wp_bg, letterbox block, two corner-zeroing sites);
        //                   one cached pointer eliminates the extra calls.
        //                   ORDERING: fb16[0] is read as pre_wp_bg AFTER fillScreen
        //                   writes to the buffer — the pointer itself is stable.
        const bool     free_mode    = g_overlay_free_mode;
        const bool     has_wallpaper = ult::expandedMemory && g_overlay_wallpaper;
        uint16_t* const fb16        = static_cast<uint16_t*>(renderer->getCurrentFramebuffer());

        // Compute content bounds once — reused for transparent-row clearing,
        // corner zeroing, focus-flash border, edge separator, and drag overlay.
        // Previously computed twice: once as pos_y/bot_row (u32, inside the free-mode
        // if block) and again as fb_top/fb_bot ternaries inside the flash scope.
        const int fb_top = free_mode ? g_ovl_free_pos_y : 0;
        const int fb_bot = free_mode
            ? g_ovl_free_pos_y + (int)OVL_FREE_CONTENT_H
            : static_cast<int>(tsl::cfg::FramebufferHeight);

        // Zero the footer-button width atomics every frame.
        // The Overlay's touch handler computes backTouched / nextPageTouched as:
        //   touchPos.x >= backLeftEdge && touchPos.x < (backLeftEdge + backWidth)
        // With backWidth == 0 the right edge equals the left edge, so the
        // condition is never satisfied — no rumble, no simulatedBack injection,
        // regardless of where the user touches.  The ROM selector's OverlayFrame
        // will recalculate correct values on its first draw after we swap back.
        ult::backWidth.store(0.0f,     std::memory_order_release);
        ult::selectWidth.store(0.0f,   std::memory_order_release);
        ult::nextPageWidth.store(0.0f, std::memory_order_release);
        ult::halfGap.store(0.0f,       std::memory_order_release);
        ult::hasNextPageButton.store(false, std::memory_order_release);

        // Background fill — use theme bg_color/bg_alpha.
        // When wallpaper is active and WILL render, force pure black (000000/D)
        // so a custom bg colour does not bleed into sub-pixel edges of the image.
        //
        // BUG FIX: wallpaperWillRender is evaluated BEFORE effBg.
        // The previous code keyed the colour choice on `has_wallpaper` alone
        // (expandedMemory && g_overlay_wallpaper), which is true whenever wallpaper
        // is enabled in settings — even when the wallpaper data was never loaded
        // because a 4 MB ROM consumed most of the 8 MB heap.  In that situation
        // `wallpaperData.empty()` is true, wallpaper cannot render, but the
        // background was still forced to black instead of the user's theme colour.
        // Gating the black-override on `wallpaperWillRender` (the same conjunction
        // draw_wallpaper_direct tests internally) fixes this: the theme colour
        // g_ovl_bg_col is used whenever wallpaper cannot actually be drawn.
        //
        // Optimization — skip fillScreen when wallpaper will cover every pixel.
        // In fixed overlay mode, draw_wallpaper_direct + render_gb_{screen,letterbox}
        // together write all 448×720 = 322,560 pixels, making fillScreen's ~645 KB
        // write entirely redundant (one full extra memory pass per frame, wasted).
        // draw_wallpaper_direct reads framebuffer[0] once for bg_r/g/b/a; we prime
        // only that slot and skip the full-frame write.
        // Free-overlay mode is excluded: its variable-height transparent rows are
        // zeroed by clear_fb_rows_transparent_448, and the coverage maths changes
        // with pos_y — fillScreen is kept there for safety.
        //
        // refreshWallpaper is snapshot once; if it flips after this point,
        // draw_wallpaper_direct returns early and one stale frame is shown —
        // the same one-frame artifact the prior code already produced.
        const bool wallpaperWillRender =
            !free_mode &&
            has_wallpaper &&
            !ult::wallpaperData.empty() &&
            !ult::refreshWallpaper.load(std::memory_order_acquire) &&
            ult::correctFrameSize;

        // Force black only when wallpaper WILL cover every pixel.
        // Fall back to the configured theme colour whenever wallpaper cannot
        // render (empty data, wrong frame size, refresh pending, free mode).
        const tsl::Color effBg  = wallpaperWillRender
            ? tsl::Color{0x0, 0x0, 0x0, g_ovl_bg_col.a} : g_ovl_bg_col;
        const tsl::Color aEffBg = renderer->a(effBg);
        if (!wallpaperWillRender) {
            renderer->fillScreen(aEffBg);
        } else {
            // Prime pixel[0] — the only framebuffer slot draw_wallpaper_direct
            // reads (for bg_r/g/b/a).  Every other pixel is written by
            // draw_wallpaper_direct or the GB screen/letterbox renderers.
            reinterpret_cast<tsl::Color*>(fb16)[0] = aEffBg;
        }

        // Draw the wallpaper, skipping the GB viewport region that render_gb_screen
        // and render_gb_letterbox will fully overwrite with opaque pixels.
        //
        // VP_X=24 and VP_X+VP_W=424 are exact multiples of 8, so the skip
        // boundaries fall cleanly on group edges (groups 3..52 skipped in
        // rows VP_Y..VP_Y+VP_H-1).  This avoids blending 144,000 pixels
        // (44.6% of the frame) that are immediately discarded.
        //
        // Previous approach: 4 scissored drawWallpaper calls, each iterating
        // all 720 rows with per-row early-continue — more loop overhead than
        // a single pass.  Current approach: draw_wallpaper_direct with a skip
        // region splits into three branch-free bands and uses NEON 8-pixel
        // stores throughout.
        //
        // pre_wp_bg: packed RGBA4444 at fb16[0], set by either fillScreen or the
        // single-pixel prime above.  Used by fill_vp_corners_448 after the
        // letterbox fill so corner blending matches draw_wallpaper_direct exactly.
        const uint16_t pre_wp_bg = fb16[0];  // read before wallpaper blends into it
        if (has_wallpaper) {
            if (free_mode) {
                // Free mode: framebuffer is OVL_FREE_FB_H (720) rows tall.
                // g_ovl_free_pos_y transparent rows sit at the top; content occupies
                // rows g_ovl_free_pos_y .. g_ovl_free_pos_y + OVL_FREE_CONTENT_H - 1.
                //
                // Wallpaper src_row_offset is (OVL_FREE_TOP_TRIM - pos_y) so that
                // every visible content row reads the same wallpaper source row as the
                // old 636-row approach — wallpaper appearance is position-invariant.
                //
                // fb_y_start = pos_y skips the transparent top rows entirely —
                // blending wallpaper into rows that are about to be zeroed by
                // clear_fb_rows_transparent_448 is pure waste.  At pos_y=84
                // (default position) this saves 84×56 = 4,704 NEON group ops
                // (~21% of total wallpaper work) per frame across all threads.
                const u32 wSrcOff  = static_cast<u32>(OVL_FREE_TOP_TRIM - g_ovl_free_pos_y);
                const u32 wFbH     = static_cast<u32>(OVL_FREE_CONTENT_H + g_ovl_free_pos_y);
                const u32 skipRowS = static_cast<u32>(VP_Y + g_render_y_offset);
                const u32 skipRowE = static_cast<u32>(VP_Y + VP_H + g_render_y_offset);
                const u32 wYStart  = static_cast<u32>(g_ovl_free_pos_y);
                draw_wallpaper_direct(renderer,
                    skipRowS,
                    skipRowE,
                    static_cast<u32>(VP_X)       / 8u,
                    static_cast<u32>(VP_X + VP_W) / 8u,
                    wSrcOff,
                    wFbH,
                    wYStart);
            } else {
                draw_wallpaper_direct(renderer,
                    static_cast<u32>(VP_Y),           // skip_row_start = 108
                    static_cast<u32>(VP_Y + VP_H),    // skip_row_end   = 468
                    static_cast<u32>(VP_X) / 8u,      // skip_grp_start = 3
                    static_cast<u32>(VP_X + VP_W) / 8u); // skip_grp_end = 53
            }
        }

        // ── Free overlay transparent row enforcement ─────────────────────────
        // Must run regardless of whether wallpaper is enabled.  fillScreen
        // (above) writes a non-transparent defaultBackgroundColor to every row;
        // the game shows through only where alpha = 0.  Tesla's drawRect blend
        // formula cannot produce alpha = 0 (a src colour with a=0 blends as
        // dst × 1 + src × 0 = dst unchanged), so we must write 0x0000 directly
        // to the raw framebuffer via clear_fb_rows_transparent_448.
        //
        //   Top    transparent rows: [0,              g_ovl_free_pos_y)
        //   Bottom transparent rows: [wFbH,           OVL_FREE_FB_H)
        //     where wFbH = g_ovl_free_pos_y + OVL_FREE_CONTENT_H
        //   When pos_y == OVL_FREE_TOP_TRIM (84): wFbH==720 → no bottom rows.
        if (free_mode) {
            if (static_cast<u32>(fb_top) > 0u)
                clear_fb_rows_transparent_448(renderer, 0u, static_cast<u32>(fb_top));
            if (static_cast<u32>(fb_bot) < static_cast<u32>(OVL_FREE_FB_H))
                clear_fb_rows_transparent_448(renderer, static_cast<u32>(fb_bot),
                                              static_cast<u32>(OVL_FREE_FB_H));
            // Zero the 10 corner pixels per corner (40 total) outside the R=5
            // quarter-circle arc, so they stay transparent.  < 100 ns overhead.
            // Called here so the non-running early-return path also gets clean corners.
            clear_ovl_corners_448(fb16, fb_top, fb_bot);
        }

        // Title and widget — only drawn in fixed overlay mode.
        // In free mode the framebuffer is full height (720 rows) but the top
        // g_ovl_free_pos_y rows are transparent padding; the title/widget
        // area doesn't exist in free mode so we skip it entirely.
        if (!free_mode) {
            draw_ultragb_title(renderer, 20, 67, 50);
#if USING_WIDGET_DIRECTIVE
            renderer->drawWidget();
#endif
        }

        //render_gb_background(renderer);

        if (!g_gb.running || !g_emu_active) {
            //renderer->drawString("Paused", false,
            //    VP_X + VP_W/2 - 24, VP_Y + VP_H/2, 20,
            //    tsl::defaultTextColor);
            return;
        }

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
            }
        }

        // Rate-limit the GB CPU to its true clock rate (59.73fps) regardless of
        // display vsync (60fps).  At 60fps the render thread would drive game logic
        // 0.45% faster than the audio thread plays it, accumulating ~1.35 seconds
        // of A/V drift after 5 minutes.  Instead, only advance the GB when a full
        // GB frame period (GB_FRAME_NS ≈ 16.743ms) has elapsed since the last one.
        // When a display frame fires but no GB frame is due, we simply re-draw the
        // previous framebuffer — one repeated frame every ~3.7 seconds, imperceptible.
        //
        // Fast-forward (ZR double-click-hold): runs 4 GB frames per display frame,
        // bypassing the clock gate entirely.  Audio is paused for the duration so
        // the SPSC ring doesn't overflow — the audio thread drains it silently and
        // preserves all GBAPU channel state, so playback resumes cleanly on release.
        //
        // Free-overlay reposition: skip the emulation tick entirely while the user
        // is dragging so the game is truly paused (not just audio-silenced).  The
        // last rendered framebuffer continues to be drawn by render_gb_screen below,
        // giving a clean frozen frame under the dim/border/"Paused" overlay.
        //
        // Screenshot capture: backgroundEventPoller sets ult::disableTransparency=true
        // for 1.5 s while the system takes the screenshot, then sets it false.
        // We mirror the drag-pause behavior exactly: pause audio on the rising edge
        // (so the SPSC ring never empties and buzzes), hold the last rendered frame
        // for the duration, then resume and re-anchor the GB clock on the falling edge
        // (so the emulator doesn't try to catch up 1.5 s of missed frames).
        // ult::disableTransparency is a plain bool written by the background thread;
        // reading it here carries the same benign race already present elsewhere in
        // the render loop and is safe in practice on ARMv8 with a single writer.
        {
            static bool s_was_screenshot = false;
            const bool  capturing        = ult::disableTransparency;
            if (capturing != s_was_screenshot) {
                if (capturing) {
                    gb_audio_pause();           // stop feeding audio ring → no buzz
                } else {
                    gb_audio_resume();
                    g_gb_frame_next_ns = 0;     // re-anchor clock; don't catch up 1.5 s
                }
                s_was_screenshot = capturing;
            }
            if (!s_ovl_free_dragging && !capturing)
                gb_tick_frame();
        }

        // Theme-aware letterbox fill: always use g_ovl_frame_col (frame_color +
        // frame_alpha keys) — even when wallpaper is active the frame color applies.
        // renderer->a() applied here so opacity, screenshot mode, and
        // disableTransparency are all respected.
        // frame_packed is computed ONCE and passed directly to render_gb_letterbox
        // so the four fill bands are written in a single pass — no redundant write.
        // After filling, zero the R=10 outer corners of the 400×360 letterbox rectangle
        // so the wallpaper/background shows through — same rounding style as the outer
        // overlay window's top corners.  Inner corners (game screen edge) stay square.
        {
            const tsl::Color fc = renderer->a(g_ovl_frame_col);
            const uint16_t frame_packed = static_cast<uint16_t>(
                static_cast<uint16_t>(fc.r)        |
                (static_cast<uint16_t>(fc.g) <<  4) |
                (static_cast<uint16_t>(fc.b) <<  8) |
                (static_cast<uint16_t>(fc.a) << 12));
            render_gb_letterbox(renderer, frame_packed);
            fill_vp_corners_448(fb16, pre_wp_bg);
        }
        render_gb_screen(renderer);
        // Theme-aware viewport border + arc border at the 4 outer letterbox corners.
        // Corner arcs use kOvlR10Arc growing down from vp_top / up from vp_bot,
        // mirrored left↔right.  14 arc drawRect calls × 2 (top+bottom) = 28 total.
        {
            const tsl::Color& BORDER = g_ovl_bdr_col;
            // Straight edges shortened by R=10 on every end — same pattern as
            // render_ovl_free_border — so they don't form square corners where
            // the arc pixels leave transparent rounded cutouts.
            static constexpr int R = 10;
            renderer->drawRect(VP_X - 1 + R,  VP_Y - 1 + g_render_y_offset,    VP_W + 2 - 2*R, 1,          BORDER);
            renderer->drawRect(VP_X - 1 + R,  VP_Y + VP_H + g_render_y_offset, VP_W + 2 - 2*R, 1,          BORDER);
            renderer->drawRect(VP_X - 1,       VP_Y + g_render_y_offset + R,    1,              VP_H - 2*R, BORDER);
            renderer->drawRect(VP_X + VP_W,    VP_Y + g_render_y_offset + R,    1,              VP_H - 2*R, BORDER);
            // Arc border pixels — shared helper draws 2 mirrored corners per call.
            // Called once for top (TL+TR, growing down) and once for bottom (BL+BR,
            // growing up) — same loop body compiled once via [[gnu::noinline]].
            const int vp_top = VP_Y + g_render_y_offset;
            const int vp_bot = VP_Y + VP_H + g_render_y_offset;
            draw_r10_2corners(renderer, VP_X, VP_X + VP_W, vp_top,     false, BORDER);
            draw_r10_2corners(renderer, VP_X, VP_X + VP_W, vp_bot - 1, true,  BORDER);
        }
        render_gbc_logo(renderer, g_ovl_text_col);

        //const char* sl = strrchr(g_gb.romPath, '/');
        //renderer->drawString(sl ? sl+1 : g_gb.romPath, false,
        //    VP_X+4, VP_Y-14, 12, tsl::defaultTextColor);
        //{
        //    static const std::vector<std::string> backButton = {"\uE0E2"};
        //    static const std::string backStr = std::string("\uE0E2 ") + ult::BACK;
        //    static const auto [bw, bh] = renderer->getTextDimensions(backStr, false, 15);
        //    renderer->drawStringWithColoredSections(backStr, false, backButton,
        //        VP_X + VP_W - bw, VP_Y+VP_H+16+2, 15, tsl::defaultTextColor, VBTN_COLOR);
        //}

        // ── Virtual Game GB controls ─────────────────────────────────────────
        // On first frame, measure each glyph's actual rendered dimensions and
        // derive the true visual centre for hit testing.  This accounts for
        // the font's internal bearing/ascender so the hit region tracks the
        // glyph exactly rather than a hand-guessed geometric centre.
        if (!g_btns_measured) {
            // No 'static' needed — the outer flag guarantees single execution.
            // Removing 'static' from all six bindings eliminates six guard variables
            // and their __cxa_guard_acquire/release sequences from .text.
            // dw/dh are also saved to g_dpad_glyph_w/h so the scissor block below
            // can read them as plain floats rather than carrying its own static guard.
            //
            // In free overlay mode g_render_y_offset = g_ovl_free_pos_y - OVL_FREE_TOP_TRIM,
            // so it varies with vertical position.  We apply it to every framebuffer
            // Y coordinate so all elements shift together as a unit.
            // at the top of draw(), so hit-center Y values are automatically placed in
            // the correct framebuffer-relative coordinate space for the trimmed layer.
            const auto [dw, dh] = renderer->getTextDimensions("\uE115", false, DPAD_SIZE);
            g_dpad_hx  = DPAD_DRAW_X  + dw / 2;
            g_dpad_hy  = (DPAD_DRAW_Y + g_render_y_offset - 10) - dh / 2 + 10;
            g_dpad_glyph_w = dw;
            g_dpad_glyph_h = dh;

            const auto [aw, ah] = renderer->getTextDimensions("\uE0E0", false, ABTN_SIZE);
            g_abtn_hx  = ABTN_DRAW_X  + aw / 2;
            g_abtn_hy  = ABTN_DRAW_Y  + g_render_y_offset - ah / 2;

            const auto [bw, bh] = renderer->getTextDimensions("\uE0E1", false, BBTN_SIZE);
            g_bbtn_hx  = BBTN_DRAW_X  + bw / 2;
            g_bbtn_hy  = BBTN_DRAW_Y  + g_render_y_offset - bh / 2;

            const auto [sw, sh] = renderer->getTextDimensions("\uE0EF", false, START_SIZE);
            g_start_hx = START_DRAW_X + sw / 2;
            g_start_hy = START_DRAW_Y + g_render_y_offset - sh / 2;

            const auto [selw, selh] = renderer->getTextDimensions("\uE0F0", false, SELECT_SIZE);
            g_select_hx = SELECT_DRAW_X + selw / 2;
            g_select_hy = SELECT_DRAW_Y + g_render_y_offset - selh / 2;

            //const auto [divw, divh] = renderer->getTextDimensions(ult::DIVIDER_SYMBOL, false, START_SIZE);
            //g_div_half_w = static_cast<int>(divw) / 2;

            g_btns_measured = true;
        }

        // ── Button backing shapes — drawn BEFORE glyphs so they sit underneath ──
        // Backdrop — opaque backing shapes drawn behind button glyphs.
        // Driven by backdrop_color theme key (default black).
        const tsl::Color& BK = g_ovl_backdrop_col;

        // D-pad backing: two rects forming a plus/cross shape.
        // All constants are directly tuneable — no getTextDimensions needed.
        // Adjust DPAD_CX/CY to centre, ARM_W/H for shaft width, FULL for span.
        // In free overlay mode g_render_y_offset = pos_y - OVL_FREE_TOP_TRIM; we apply it
        // at runtime (not compile-time) so the backing tracks the shifted viewport.
        {
            static constexpr s32 DPAD_CX      = DPAD_DRAW_X + 67;
            static constexpr s32 DPAD_CY_BASE = DPAD_DRAW_Y - 10 - 56;
            const            s32 DPAD_CY      = DPAD_CY_BASE + g_render_y_offset;
            static constexpr s32 ARM_W   = 46;
            static constexpr s32 ARM_H   = 48;   // +2 vertically taller
            static constexpr s32 FULL    = 131;  // +3 for vertical length
            static constexpr s32 LIP     = 3;   // uniform border, matches A/B circle thickness
            // Vertical bar — shifted 1px down, bottom extended 2px
            renderer->drawRect(DPAD_CX - (ARM_W + LIP*2)/2, DPAD_CY - (FULL + LIP*2)/2 + 4,
                               ARM_W + LIP*2, FULL + LIP*2 + 2-1, BK);
            // Horizontal bar — shifted 1px lower
            renderer->drawRect(DPAD_CX - (FULL + LIP*2)/2 -1, DPAD_CY - (ARM_H + LIP*2)/2 + 6,
                               FULL + LIP*2 +1, ARM_H + LIP*2 -2, BK);
        }

        // A button: filled black circle matching the hit-test radius.
        renderer->drawCircle(g_abtn_hx, g_abtn_hy + 8, ABTN_R, true, BK);

        // B button: filled black circle matching the hit-test radius.
        renderer->drawCircle(g_bbtn_hx, g_bbtn_hy + 6, BBTN_R, true, BK);

        // Plus (+) / Minus (−): individual filled circles behind each glyph,
        // matching the A/B button style.  g_start/select_hx/hy are the measured
        // visual centres from the g_btns_measured block above; START_R/SELECT_R
        // are the intended radii (both 24), defined in gb_globals.hpp.
        renderer->drawCircle(g_select_hx-3, g_select_hy+4, SELECT_R-6, true, BK);
        renderer->drawCircle(g_start_hx+3,  g_start_hy+4,  START_R-6,  true, BK);

        // ── D-pad composite — all four arrow directions ───────────────────────
        // Each glyph is treated as a 3×3 grid; we scissor to the center strip.
        // Strip width/height = 38% of the total glyph dimension — wider than
        // one third (33%) so the arrow arms are fully visible, but narrow enough
        // to hide the diagonal corners of the overlapping glyph.
        {
            const s32 baseline  = DPAD_DRAW_Y + g_render_y_offset - 10;
            // g_dpad_glyph_w/h are populated by the g_btns_measured block above on the
            // first draw — guaranteed before this point because g_btns_measured runs
            // first in every draw() call.  Reading them here eliminates the static
            // guard that was the only remaining one in this function's hot path.
            const float dw = g_dpad_glyph_w;
            const float dh = g_dpad_glyph_h;
            const s32 left      = DPAD_DRAW_X;
            const s32 top       = baseline - static_cast<s32>(dh);
            const s32 thirdH    = static_cast<s32>(dh) / 3;
            // 46% wide for the vertical (↑↓) strip, 46% tall for the horizontal (←→) strip
            const s32 stripW    = static_cast<s32>(dw) * 34 / 100;
            const s32 stripH    = static_cast<s32>(dh) * 34 / 100;
            const s32 fbH       = static_cast<s32>(tsl::cfg::FramebufferHeight);
            const s32 fbW       = static_cast<s32>(tsl::cfg::FramebufferWidth);

            // E115 (↑↓): narrow strip horizontally (stripW), but fully unbounded
            // vertically — scissor spans the entire screen height so arrow tips
            // are never clipped regardless of font ascender/descender metrics.
            renderer->enableScissoring(left + (static_cast<s32>(dw) - stripW) / 2, 0,
                                       stripW, fbH);
            renderer->drawString("\uE115", false, left, baseline, DPAD_SIZE, g_ovl_dpad_col);
            renderer->disableScissoring();

            // E116 (←→): narrow strip vertically (stripH), but fully unbounded
            // horizontally — scissor spans the entire screen width so arrow tips
            // are never clipped at either side.
            const s32 rowNudge = thirdH / 4;
            renderer->enableScissoring(0, top + (static_cast<s32>(dh) - stripH) / 2 + rowNudge +4,
                                       fbW, stripH);
            renderer->drawString("\uE116", false, left, baseline, DPAD_SIZE, g_ovl_dpad_col);
            renderer->disableScissoring();

            // Centre circle — drawn last so it sits on top of both arrow glyphs
            // at the exact crossing point of the two black backing bars.
            // Values match DPAD_CX, DPAD_CY+4, and (ARM_W+LIP*2)/2 from the backing block.
            renderer->drawCircle(DPAD_DRAW_X + 67, DPAD_DRAW_Y + g_render_y_offset - 10 - 56 + 4+1, 14, true, BK);
        }
        renderer->drawString("\uE0E0", false, ABTN_DRAW_X,  ABTN_DRAW_Y  + g_render_y_offset, ABTN_SIZE,   g_ovl_abtn_col);
        renderer->drawString("\uE0E1", false, BBTN_DRAW_X,  BBTN_DRAW_Y  + g_render_y_offset, BBTN_SIZE,   g_ovl_bbtn_col);
        renderer->drawString("\uE0F0", false, SELECT_DRAW_X -3, SELECT_DRAW_Y + g_render_y_offset + 1, SELECT_SIZE, g_ovl_select_col);
        //renderer->drawString(ult::DIVIDER_SYMBOL, false,
        //    FB_W / 2 - g_div_half_w, START_DRAW_Y + g_render_y_offset + 1, START_SIZE, 0xF444);
        renderer->drawString("\uE0EF", false, START_DRAW_X +3, START_DRAW_Y + g_render_y_offset + 1, START_SIZE, g_ovl_start_col);

        // ── Focus pass-through flash border ──────────────────────────────────
        // Draws a 4-pixel coloured border for g_focus_flash frames after the
        // ZL double-click-hold gesture toggles pass-through:
        //   red   = foreground focus released (background now owns HID)
        //   green = foreground focus regained
        // Alpha fades out over the last 15 frames so the border disappears
        // smoothly instead of cutting off.
        //
        // In free overlay mode the border is clamped to the active content
        // area (fb_top..fb_bot) so it never draws into transparent padding rows.
        {
        // fb_top / fb_bot are already computed at function scope — no ternary needed.

        // draw_focus_flash returns true when the border was drawn this frame.
        // Used below to decide whether to re-zero rounded corners.
        const bool flash_was_active = draw_focus_flash(
            renderer, 0, fb_top, static_cast<s32>(FB_W), fb_bot - fb_top,
            free_mode);  // rounded=true in free overlay mode → matches player shape

        if (!free_mode) {
            if (!ult::useRightAlignment)
                renderer->drawRect(447, 0, 448, static_cast<int>(tsl::cfg::FramebufferHeight), a(tsl::edgeSeparatorColor));
            else
                renderer->drawRect(0, 0, 1, static_cast<int>(tsl::cfg::FramebufferHeight), a(tsl::edgeSeparatorColor));
        } else {
            // Re-zero corners only when the focus-flash border was drawn this
            // frame — its drawRect calls are the only thing between the first
            // clear_ovl_corners_448 (in the transparent-row setup above) and here
            // that can write into corner pixels.  Skipping the re-zero in normal
            // gameplay (flash_was_active == false every frame) saves 124 scalar
            // stores ≈ 125 ns per frame at no cost.
            if (flash_was_active)
                clear_ovl_corners_448(fb16, fb_top, fb_bot);
            // Skip border during drag: the reposition overlay immediately draws its
            // own full-width red border on top, making the rounded border invisible.
            // Skipping saves 32 drawRect calls ≈ 10–15 µs per dragging frame.
            if (!s_ovl_free_dragging)
                render_ovl_free_border(renderer, fb_top, fb_bot, a(tsl::edgeSeparatorColor));
        }

        // ── Free overlay reposition overlay ──────────────────────────────────
        // draw_drag_dim_border (gb_renderer.h) handles dim + red border + text.
        // "Paused" is centred within the 2× pixel-perfect screen region (VP2).
        // scr_y accounts for g_render_y_offset so the text tracks pos_y correctly.
        if (free_mode && s_ovl_free_dragging) {
            const s32 fw  = static_cast<s32>(tsl::cfg::FramebufferWidth);
            const s32 fbt = static_cast<s32>(fb_top);
            const s32 fbc = static_cast<s32>(fb_bot) - fbt;  // content height
            const s32 scr_y = static_cast<s32>(VP2_Y) + g_render_y_offset;
            draw_drag_dim_border(renderer,
                fbt, fw, fbc,
                (s32)VP2_X, scr_y, (s32)VP2_W, (s32)VP2_H,
                20u, fb16);  // fb16 → rounded corners matching the overlay player frame
        }
        } // end fb_top/fb_bot scope

#ifdef GB_FPS
        // ── FPS indicator (overlay player mode) ───────────────────────────────
        // Compiled in ONLY when -DGB_FPS is supplied to the build; the #ifdef
        // guarantees zero code, zero data, and zero runtime cost in production.
        //
        // Placement: 3 px inside the top-left corner of the game viewport,
        // adjusted by g_render_y_offset so the label tracks correctly in both
        // fixed-overlay and free-overlay modes.
        //
        // Measurement: same 1-second counting window as the windowed element,
        // with count divided by actual elapsed nanoseconds so a window that ran
        // slightly over 1 s doesn't erroneously show 61 fps.
        {
            static uint64_t s_fps_win_start   = 0;   // ns timestamp of window open
            static int      s_fps_frame_cnt   = 0;   // frames counted this second
            static int      s_fps_display_x10 = 0;   // last completed 1 s measurement ×10

            const uint64_t now = ult::nowNs();
            if (s_fps_win_start == 0) s_fps_win_start = now;

            ++s_fps_frame_cnt;
            if (now - s_fps_win_start >= 1'000'000'000ULL) {
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

            // Font size: fixed 10 px — the overlay viewport is always the same
            // physical size (400×360) regardless of handheld vs docked, so a
            // constant size is appropriate (unlike the windowed element which
            // scales with g_win_scale).
            static constexpr s32 font_sz = 10;

            // Semi-transparent black shadow one pixel down-right for contrast,
            // then the yellow label on top.
            const s32 tx = VP_X + 3;
            const s32 ty = VP_Y + g_render_y_offset + font_sz + 2;
            renderer->drawString(buf, false, tx + 1, ty + 1, font_sz,
                                 tsl::Color{0x0, 0x0, 0x0, 0xA});  // shadow
            renderer->drawString(buf, false, tx,     ty,     font_sz,
                                 tsl::Color{0xF, 0xE, 0x0, 0xF});  // yellow
        }
#endif  // GB_FPS
    }  // end draw()

    virtual void layout(u16, u16, u16, u16) override {
        this->setBoundaries(0, 0, FB_W, FB_H);
    }
};

// =============================================================================
// GBOverlayGui — input handled at the Gui level
// =============================================================================
class GBOverlayGui : public tsl::Gui {
    bool m_waitForRelease = true;  // ignore input until all buttons are released
    u64  m_prevTouchKeys  = 0;     // track previous touch state
    bool m_load_failed    = false; // deferred load failed; relaunch as normal overlay in update()
    bool m_restoreHapticState = false;
    bool runOnce = true;

    bool     m_zr_first_seen  = false; // true after first ZR press is recorded
    uint32_t m_zr_first_frame = 0;     // g_frame_count when first press fired
    ZLPassThroughState m_zl_state;     // ZL double-click-hold pass-through toggle

    // ── Free overlay touch hold-to-drag state (mirrors GBWindowedGui) ────────
    bool m_hold_armed    = false;  // touch landed inside the window
    bool m_dragging      = false;  // hold threshold passed; actively repositioning
    int  m_hold_frames   = 0;
    int  m_touch_start_x = 0;     // HID x where the hold began (touch space)
    int  m_touch_start_y = 0;     // HID y where the hold began (touch space)
    int  m_pos_start_x   = 0;     // g_ovl_free_pos_x when hold began (VI space)
    int  m_pos_start_y   = 0;     // g_ovl_free_pos_y when hold began (render offset space, 0..OVL_FREE_TOP_TRIM)
    bool m_prev_touching = false;

    // ── Free overlay KEY_PLUS drag state (mirrors GBWindowedGui) ─────────────
    bool     m_plus_dragging      = false;
    bool     m_plus_armed         = false;
    uint64_t m_plus_hold_start_ns = 0;
    float    m_joy_acc_x          = 0.f;
    float    m_joy_acc_y          = 0.f;
    uint64_t m_joy_last_ns        = 0;

    // 60 frames ≈ 1 s at ~60 fps.
    static constexpr int      HOLD_FRAMES  = kHoldFrames;
    static constexpr uint64_t PLUS_HOLD_NS = kPlusHoldNs;
    static constexpr int      JOY_DEADZONE = kJoyDeadzone;

    // VI bounds — vi_max_x() / touch_win_w() are shared free functions in gb_globals.hpp.
    // vi_max_y() intentionally omitted: overlay Y repositioning is bounded by
    // OVL_FREE_TOP_TRIM, not by LayerHeight.  vi_max_y() was never called.

    // Layer footprint in HID touch space. W shared with windowed; H has no pixel-perfect branch here.
    static int touch_win_h() { return static_cast<int>(tsl::cfg::LayerHeight) * 2 / 3; }

    // Sync notification / virtual-button touch offsets to the current VI position.
    void sync_ovl_touch_offsets() {
        ult::layerEdge  = (g_ovl_free_pos_x * 2) / 3;
        tsl::layerEdgeY = 0;  // VI layer Y is always 0; vertical shift uses render offset
    }

public:
    ~GBOverlayGui() {
        restore_haptic_if_needed(m_restoreHapticState);
        tsl::disableHiding = false;
        if (g_overlay_free_mode) {
            ult::layerEdge  = 0;
            tsl::layerEdgeY = 0;
        }
    }

    virtual tsl::elm::Element* createUI() override {
        tsl::disableHiding = true;
        audio_exit_if_enabled();

        // Ensure screenshots do not work.
        //screenshotsAreDisabled.store(true, std::memory_order_release);
        //screenshotsAreForceDisabled.store(true, std::memory_order_release);
        //tsl::gfx::Renderer::get().removeScreenshotStacks();
        
        consume_pending_rom(m_load_failed);

        if (m_load_failed) {
            g_emu_active = false;
            ult::noClickableItems.store(false, std::memory_order_release);
        } else {
            g_emu_active = true;
            m_waitForRelease = true;
            ult::noClickableItems.store(true, std::memory_order_release);
        }

        // ── Free overlay: position the VI layer at the saved location ─────────
        // Clamp to valid VI bounds derived from the layer size Tesla configured.
        if (g_overlay_free_mode) {
            // Disable screenshots
            //screenshotsAreDisabled.store(true, std::memory_order_release);
            //screenshotsAreForceDisabled.store(true, std::memory_order_release);
            //tsl::gfx::Renderer::get().removeScreenshotStacks();

            g_ovl_free_pos_x = std::max(0, std::min(vi_max_x(), g_ovl_free_pos_x));
            g_ovl_free_pos_y = std::max(0, std::min((int)OVL_FREE_TOP_TRIM, g_ovl_free_pos_y));
            tsl::gfx::Renderer::get().setLayerPos(
                static_cast<u32>(g_ovl_free_pos_x),
                0u);  // VI Y is always 0; vertical repositioning uses render offset
            sync_ovl_touch_offsets();
        }

        return new GBOverlayElement();
    }

    virtual void update() override {
        if (m_load_failed) {
            m_load_failed = false;
            if (g_self_path[0])
                tsl::setNextOverlay(std::string(g_self_path));
            tsl::Overlay::get()->close();
        }
        run_once_setup(runOnce, m_restoreHapticState);

        // Periodic background-title volume correction — safety net for edge cases
        // where the title's PID changed without triggering Overlay::onShow() (e.g.
        // a new title launched while the overlay layer was never hidden).
        // Also catches system-side volume resets on the same title.
        // Runs every 120 frames (~2 s) so the pmdmnt + audproc IPC cost is
        // completely negligible.  Counter is a plain u8; wrapping at 256 is harmless
        // (fires one frame early every ~4 s — no correctness impact).
        // Suppressed while pass-through is active: the title volume was intentionally
        // lifted to 1.0f by gb_game_vol_suppress() and must not be re-lowered until
        // foreground is reclaimed.
        static u8 s_vol_recheck_ctr = 0;
        if (++s_vol_recheck_ctr >= 120) {
            s_vol_recheck_ctr = 0;
            if (!m_zl_state.pass_through)
                gb_game_vol_recheck();
        }
    }

    virtual __attribute__((optimize("O3"))) bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {

        if (m_waitForRelease) {
            if (keysHeld) return true;
            g_touch_keys = 0;
            m_prevTouchKeys = 0;
            m_waitForRelease = false;
        }

        // ── Overlay close combo ───────────────────────────────────────────────
        if (g_quick_launch || g_directMode || g_overlay_mode) {
            if ((keysDown & tsl::cfg::launchCombo) && (((keysDown | keysHeld) & tsl::cfg::launchCombo) == tsl::cfg::launchCombo)) {
                g_touch_keys = 0;
                ult::noClickableItems.store(false, std::memory_order_release);
                gb_audio_pause();
                launchComboHasTriggered.store(true, std::memory_order_release);
                if (g_quick_launch || g_directMode) {
                    g_settings_scroll[0] = '\0';
                    ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll, "", "");
                }
                if (!g_directMode && g_self_path[0])
                    tsl::setNextOverlay(std::string(g_self_path), "-returning");
                tsl::Overlay::get()->close();
                return true;
            }
        }

        // ── Touch state — layer-relative coordinates ──────────────────────────
        // In free overlay mode the layer may not be at (0,0), so subtract the
        // layer's touch-space offset (= VI pos × 2/3) to get layer-relative coords.
        const int  tx       = static_cast<int>(touchPos.x) - static_cast<int>(ult::layerEdge);
        const int  ty_raw   = static_cast<int>(touchPos.y);
        const int  ty       = g_overlay_free_mode
                                ? ty_raw - static_cast<int>(tsl::layerEdgeY)
                                : ty_raw;
        const int  footer_y = g_overlay_free_mode
                                ? static_cast<int>(OVL_FREE_FB_H)
                                : FOOTER_Y;
        const bool touching = (touchPos.x != 0 || touchPos.y != 0) &&
                              ty < footer_y;

        // ── ZR double-click-hold → fast-forward ──────────────────────────────
        {
            const bool zr_down = !m_zl_state.pass_through && (keysDown & KEY_ZR);
            const bool zr_held = !m_zl_state.pass_through && (keysHeld  & KEY_ZR);
            process_zr_fast_forward(zr_down, zr_held, m_zr_first_seen, m_zr_first_frame);
        }

        // ── ZL double-click-hold: toggle background pass-through ─────────────
        {
            const bool zl_down = ((keysDown & KEY_ZL) && !(keysHeld & ~KEY_ZL & ALL_KEYS_MASK));
            const bool zl_held = ((keysHeld  & KEY_ZL) && !(keysHeld & ~KEY_ZL & ALL_KEYS_MASK));
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

        // ── Touch → virtual button state ──────────────────────────────────────
        // Suppressed while dragging so the game never receives button presses
        // during reposition, and suppressed during pass-through.
        g_touch_keys = 0;
        if (touching && !m_zl_state.pass_through && !s_ovl_free_dragging) {
            {
                static constexpr int D_CX        = DPAD_DRAW_X + 67;
                static constexpr int D_CY        = DPAD_DRAW_Y - 61;
                static constexpr int D_HALF_ARM  = 27;
                static constexpr int D_HALF_SPAN = 70;

                const int dx = tx - D_CX;
                const int dy = ty - D_CY;

                if (std::abs(dx) <= D_HALF_SPAN && std::abs(dy) <= D_HALF_SPAN) {
                    const bool in_v = std::abs(dx) <= D_HALF_ARM;
                    const bool in_h = std::abs(dy) <= D_HALF_ARM;

                    if (in_v && in_h) {
                        // Centre overlap — dead zone
                    } else if (in_v) {
                        g_touch_keys |= (dy > 0) ? KEY_DOWN : KEY_UP;
                    } else if (in_h) {
                        g_touch_keys |= (dx > 0) ? KEY_RIGHT : KEY_LEFT;
                    } else {
                        g_touch_keys |= (dx > 0) ? KEY_RIGHT : KEY_LEFT;
                        g_touch_keys |= (dy > 0) ? KEY_DOWN : KEY_UP;
                    }
                }
            }
            { const int dx = tx - g_abtn_hx,  dy = ty - g_abtn_hy;  if (dx*dx + dy*dy <= ABTN_R   * ABTN_R)   g_touch_keys |= KEY_A; }
            { const int dx = tx - g_bbtn_hx,  dy = ty - g_bbtn_hy;  if (dx*dx + dy*dy <= BBTN_R   * BBTN_R)   g_touch_keys |= KEY_B; }
            { const int dx = tx - g_start_hx, dy = ty - g_start_hy; if (dx*dx + dy*dy <= START_R   * START_R)  g_touch_keys |= KEY_PLUS; }
            { const int dx = tx - g_select_hx,dy = ty - g_select_hy;if (dx*dx + dy*dy <= SELECT_R  * SELECT_R) g_touch_keys |= KEY_MINUS; }
        }

        u64 newTouchPresses = g_touch_keys & ~m_prevTouchKeys;
        if (newTouchPresses && g_touch_haptics && !m_zl_state.pass_through)
            triggerRumbleClick.store(true, std::memory_order_release);
        m_prevTouchKeys = g_touch_keys;

        // Physical buttons → GB joypad.  Suppressed during drag and pass-through.
        if (!m_zl_state.pass_through && !s_ovl_free_dragging) {
            gb_set_input(keysHeld | g_touch_keys);
            if (g_button_haptics &&
                (keysDown & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_PLUS | KEY_MINUS |
                             KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)))
                triggerRumbleClick.store(true, std::memory_order_release);
        }

        // ── Free overlay reposition (touch + joystick) ────────────────────────
        // Only active in free overlay mode; identical control scheme to windowed.
        if (g_overlay_free_mode) {

            // ── Touch hold-to-drag ────────────────────────────────────────────
            // Coordinate spaces:
            //   Touch: 0..1279 × 0..719   (HID native, always fixed)
            //   VI:    0..1919 × 0..1079  (always fixed, touch→VI is ×3/2)
            //
            // Window top-left in touch space = (pos_x*2/3, pos_y*2/3).
            // Window size in touch space     = LayerWidth*2/3 × LayerHeight*2/3.
            {
                int htx = 0, hty = 0;
                const bool htouching = poll_touch(htx, hty);

                const int win_tx = (g_ovl_free_pos_x * 2) / 3;
                const int win_ty = 0;   // VI layer Y is always 0; touch window top = 0
                const int win_w  = touch_win_w();
                const int win_h  = touch_win_h();

                // Restrict drag-arming to the 2× GB screen region only (not the
                // full layer which also includes the virtual button area).
                // Framebuffer pixels map 1:1 to touch-space pixels within the layer
                // (1.5× FB→VI, then VI×2/3→touch cancels out), so we can add the
                // screen's framebuffer offsets directly to the layer's touch-space origin.
                // scr_ty accounts for the render offset: as pos_y grows the screen moves
                // down within the framebuffer, shifting the touch target accordingly.
                const int scr_tx = win_tx + VP2_X;
                const int scr_ty = static_cast<int>(VP2_Y) - OVL_FREE_TOP_TRIM + g_ovl_free_pos_y;
                const bool in_screen = htouching
                    && htx >= scr_tx && htx < scr_tx + VP2_W
                    && hty >= scr_ty && hty < scr_ty + VP2_H;

                // Still track whether the finger is anywhere in the window so we
                // can cancel the hold when it drifts fully outside.
                const bool in_window = htouching
                    && htx >= win_tx && htx < win_tx + win_w
                    && hty >= win_ty && hty < win_ty + win_h;

                // Finger-down inside the 2× screen: arm the hold timer.
                if (!m_prev_touching && htouching && in_screen && !m_plus_dragging) {
                    m_hold_armed    = true;
                    m_dragging      = false;
                    m_hold_frames   = 0;
                    m_touch_start_x = htx;
                    m_touch_start_y = hty;
                    m_pos_start_x   = g_ovl_free_pos_x;
                    m_pos_start_y   = g_ovl_free_pos_y;
                }

                // Finger held: tick timer; enter/continue drag.
                if (m_hold_armed && htouching) {
                    ++m_hold_frames;

                    if (!m_dragging && m_hold_frames >= HOLD_FRAMES) {
                        m_dragging         = true;
                        s_ovl_free_dragging = true;
                        gb_audio_pause();
                        // Re-anchor to avoid jump from drift during hold period.
                        m_touch_start_x = htx;
                        m_touch_start_y = hty;
                        m_pos_start_x   = g_ovl_free_pos_x;
                        m_pos_start_y   = g_ovl_free_pos_y;
                        if (g_touch_haptics) triggerNavigationFeedback();
                    }

                    if (m_dragging) {
                        const int dx = (htx - m_touch_start_x) * 3 / 2;
                        const int dy =  hty - m_touch_start_y;   // render-offset space: 1 touch px = 1 row
                        const int nx = std::max(0, std::min(vi_max_x(), m_pos_start_x + dx));
                        const int ny = std::max(0, std::min((int)OVL_FREE_TOP_TRIM, m_pos_start_y + dy));
                        if (nx != g_ovl_free_pos_x || ny != g_ovl_free_pos_y) {
                            g_ovl_free_pos_x = nx;
                            g_ovl_free_pos_y = ny;
                            g_btns_measured  = false;  // hit centres depend on render offset
                            tsl::gfx::Renderer::get().setLayerPos(
                                static_cast<u32>(g_ovl_free_pos_x),
                                0u);  // VI Y always 0
                            sync_ovl_touch_offsets();
                        }
                    }

                    // Finger drifted out of the entire window before hold threshold — cancel.
                    if (!m_dragging && !in_window) {
                        m_hold_armed  = false;
                        m_hold_frames = 0;
                    }
                }

                // Finger-up: save if we were dragging.
                if (m_prev_touching && !htouching) {
                    if (m_dragging) {
                        save_ovl_free_pos();
                        if (g_touch_haptics) triggerExitFeedback();
                        gb_audio_resume();
                        g_gb_frame_next_ns = 0;
                        g_btns_measured    = false;  // recalculate hit centres at new render offset
                    }
                    s_ovl_free_dragging = false;
                    m_hold_armed        = false;
                    m_dragging          = false;
                    m_hold_frames       = 0;
                }

                m_prev_touching = htouching;
            }

            // ── KEY_PLUS 1 s hold → left-stick reposition ────────────────────
            // Identical to GBWindowedGui: x^8 sensitivity curve, dt-scaled,
            // both X and Y axes, clamped to VI bounds.
            {
                const bool plus_only = (keysHeld & KEY_PLUS)
                    && !(keysHeld & ~KEY_PLUS & ALL_KEYS_MASK);

                if (plus_only) {
                    if (!m_plus_armed) {
                        m_plus_armed         = true;
                        m_plus_hold_start_ns = ult::nowNs();
                    }

                    if (!m_plus_dragging) {
                        if (ult::nowNs() - m_plus_hold_start_ns >= PLUS_HOLD_NS) {
                            m_plus_dragging     = true;
                            s_ovl_free_dragging = true;
                            gb_audio_pause();
                            m_joy_acc_x   = 0.f;
                            m_joy_acc_y   = 0.f;
                            m_joy_last_ns = 0;
                            if (g_touch_haptics) triggerNavigationFeedback();
                        }
                    }

                    if (m_plus_dragging) {
                        if (std::abs(leftJoy.x) > JOY_DEADZONE || std::abs(leftJoy.y) > JOY_DEADZONE) {
                            const float fx = static_cast<float>(leftJoy.x);
                            const float fy = static_cast<float>(leftJoy.y);

                            // x^8 sensitivity curve — same as windowed mode.
                            const float sens = joy_sens(fx, fy);

                            const uint64_t now_ns = ult::nowNs();
                            if (m_joy_last_ns == 0) m_joy_last_ns = now_ns;
                            const float dt_ns    = static_cast<float>(now_ns - m_joy_last_ns);
                            const float dt_factor = std::max(0.25f, std::min(4.0f, dt_ns * (60.f / 1e9f)));
                            m_joy_last_ns = now_ns;

                            // Stick up (−y) → window moves up → smaller pos_y.
                            m_joy_acc_x += fx  * sens * dt_factor;
                            m_joy_acc_y += -fy * sens * dt_factor;

                            const int dx = static_cast<int>(m_joy_acc_x);
                            const int dy = static_cast<int>(m_joy_acc_y);
                            m_joy_acc_x -= static_cast<float>(dx);
                            m_joy_acc_y -= static_cast<float>(dy);

                            const int nx = std::max(0, std::min(vi_max_x(), g_ovl_free_pos_x + dx));
                            const int ny = std::max(0, std::min((int)OVL_FREE_TOP_TRIM, g_ovl_free_pos_y + dy));
                            if (nx != g_ovl_free_pos_x || ny != g_ovl_free_pos_y) {
                                g_ovl_free_pos_x = nx;
                                g_ovl_free_pos_y = ny;
                                g_btns_measured  = false;  // hit centres depend on render offset
                                tsl::gfx::Renderer::get().setLayerPos(
                                    static_cast<u32>(g_ovl_free_pos_x),
                                    0u);  // VI Y always 0
                                sync_ovl_touch_offsets();
                            }
                        } else {
                            m_joy_last_ns = 0;
                        }
                    }
                } else {
                    if (m_plus_armed) {
                        if (m_plus_dragging) {
                            save_ovl_free_pos();
                            if (g_touch_haptics) triggerExitFeedback();
                            gb_audio_resume();
                            g_gb_frame_next_ns = 0;
                            g_btns_measured    = false;  // recalculate hit centres at new render offset
                            s_ovl_free_dragging = false;
                            m_plus_dragging     = false;
                        }
                        m_plus_armed         = false;
                        m_plus_hold_start_ns = 0;
                    }
                }
            }

            // Swallow all input while repositioning.
            if (s_ovl_free_dragging) return true;
        }

        // ── Left/Right stick click: swap overlay mode ─────────────────────────
        // Active in both fixed and free overlay player modes.
        // Mirrors the windowed-mode resize scheme but switches overlay type instead
        // of scale.  Settings (ovl_free_mode) are saved before relaunch so the new
        // session starts in the correct mode immediately.
        //
        //   Left  stick click → reload as fixed overlay  (-overlay)
        //   Right stick click → reload as free overlay   (-freeoverlay)
        //
        // Not active during pass-through or while a drag reposition is in progress.
        // Game state is preserved: exitServices() → gb_unload_rom() saves state.
        if (!m_zl_state.pass_through && !s_ovl_free_dragging) {
            const bool lstick = (keysDown & KEY_LSTICK) && !(keysHeld & ~KEY_LSTICK & ALL_KEYS_MASK);
            const bool rstick = (keysDown & KEY_RSTICK) && !(keysHeld & ~KEY_RSTICK & ALL_KEYS_MASK);
            if (lstick || rstick) {
                const bool wantFree = rstick;   // lstick → fixed (false), rstick → free (true)
                g_overlay_free_mode = wantFree;
                save_ovl_free_mode();
                triggerRumbleClick.store(true, std::memory_order_release);
                // Suppress directMode exit double-click, matching windowed resize behaviour.
                launchComboHasTriggered.store(true, std::memory_order_release);
                if (g_gb.romPath[0]) {
                    if (wantFree)
                        launch_free_overlay_mode(g_gb.romPath);
                    else
                        launch_overlay_mode(g_gb.romPath);
                }
                return true;
            }
        }

        return true;  // consume all input while in-game
    }
};

#pragma GCC pop_options