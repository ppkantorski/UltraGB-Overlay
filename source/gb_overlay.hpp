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

// GBOverlayGui calls tsl::swapTo<RomSelectorGui>() — forward declaration is
// sufficient because swapTo only uses the type as a template argument.
class RomSelectorGui;

// -- Shared emulation clock tick ----------------------------------------------
// Defined here so it sees the static globals declared in main.cpp:
//   g_frame_count, g_gb_frame_next_ns, GB_RENDER_FRAME_NS, g_fast_forward.
// gb_windowed.hpp is #included after this file in main.cpp and therefore
// sees this definition automatically — no duplication, one copy in .text.
//
// Increments the display-frame counter and rate-limits the GB CPU to its
// true 59.73 fps clock regardless of the 60 fps display vsync.
// See GBOverlayElement::draw() for the full timing rationale.
#ifndef GB_TICK_FRAME_DEFINED
#define GB_TICK_FRAME_DEFINED
inline void gb_tick_frame() {
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
#endif

// =============================================================================
// GBOverlayElement — drawing only; no input handling (input lives on Gui)
// =============================================================================
class GBOverlayElement : public tsl::elm::Element {
public:
    virtual void draw(tsl::gfx::Renderer* renderer) override {
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
        renderer->fillScreen(renderer->a(tsl::defaultBackgroundColor));

        // Draw the full wallpaper in a single pass regardless of game state.
        //
        // Previous approach: 4 scissored drawWallpaper calls when running,
        // each iterating all 720 rows with an early-continue for excluded rows.
        // Total cost: 4 × 720 row iterations even though ~360 rows per pass
        // were skipped — more row-loop overhead than a single full-framebuffer pass.
        //
        // Current approach: one drawWallpaper call covers the entire 448×720
        // framebuffer.  render_gb_letterbox and render_gb_screen both use direct
        // setPixelAtOffset writes with a=0xF (fully opaque), so they overwrite
        // the wallpaper pixels in the game viewport area.  The extra ~92K pixels
        // blended under the viewport cost far less than the 3 saved full passes.
        if (ult::expandedMemory && g_ingame_wallpaper)
            renderer->drawWallpaper();
    
        //renderer->drawRect(15, tsl::cfg::FramebufferHeight - 73, tsl::cfg::FramebufferWidth - 30, 1, renderer->a(tsl::bottomSeparatorColor));
        
        draw_ultragb_title(renderer, 20, 67, 50);

        #if USING_WIDGET_DIRECTIVE
        //if (m_showWidget)
        renderer->drawWidget();
        #endif

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
        // Cost in steady state: one bool comparison per display frame (~0 ns).
        {
            const bool  is_docked    = ult::consoleIsDocked();
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
        gb_tick_frame();

        render_gb_letterbox(renderer);
        render_gb_screen(renderer);
        render_gb_border(renderer);
        render_gbc_logo(renderer);

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
            const auto [dw, dh] = renderer->getTextDimensions("\uE115", false, DPAD_SIZE);
            g_dpad_hx  = DPAD_DRAW_X  + dw / 2;
            g_dpad_hy  = (DPAD_DRAW_Y - 10) - dh / 2 + 10;
            g_dpad_glyph_w = dw;
            g_dpad_glyph_h = dh;

            const auto [aw, ah] = renderer->getTextDimensions("\uE0E0", false, ABTN_SIZE);
            g_abtn_hx  = ABTN_DRAW_X  + aw / 2;
            g_abtn_hy  = ABTN_DRAW_Y  - ah / 2;

            const auto [bw, bh] = renderer->getTextDimensions("\uE0E1", false, BBTN_SIZE);
            g_bbtn_hx  = BBTN_DRAW_X  + bw / 2;
            g_bbtn_hy  = BBTN_DRAW_Y  - bh / 2;

            const auto [sw, sh] = renderer->getTextDimensions("\uE0EF", false, START_SIZE);
            g_start_hx = START_DRAW_X + sw / 2;
            g_start_hy = START_DRAW_Y - sh / 2;

            const auto [selw, selh] = renderer->getTextDimensions("\uE0F0", false, SELECT_SIZE);
            g_select_hx = SELECT_DRAW_X + selw / 2;
            g_select_hy = SELECT_DRAW_Y - selh / 2;

            const auto [divw, divh] = renderer->getTextDimensions(ult::DIVIDER_SYMBOL, false, START_SIZE);
            g_div_half_w = static_cast<int>(divw) / 2;

            g_btns_measured = true;
        }

        // ── Button backing shapes — drawn BEFORE glyphs so they sit underneath ──
        static constexpr tsl::Color BK{0x0, 0x0, 0x0, 0xF};  // solid black

        // D-pad backing: two rects forming a plus/cross shape.
        // All constants are directly tuneable — no getTextDimensions needed.
        // Adjust DPAD_CX/CY to centre, ARM_W/H for shaft width, FULL for span.
        {
            static constexpr s32 DPAD_CX = DPAD_DRAW_X + 67;   // horizontal centre
            static constexpr s32 DPAD_CY = DPAD_DRAW_Y - 10 - 56; // vertical centre
            static constexpr s32 ARM_W   = 46;
            static constexpr s32 ARM_H   = 48;   // +2 vertically taller
            static constexpr s32 FULL    = 131;  // +3 for vertical length
            static constexpr s32 LIP     = 4;   // uniform border, matches A/B circle thickness
            // Vertical bar — shifted 1px down, bottom extended 2px
            renderer->drawRect(DPAD_CX - (ARM_W + LIP*2)/2, DPAD_CY - (FULL + LIP*2)/2 + 4,
                               ARM_W + LIP*2, FULL + LIP*2 + 2, BK);
            // Horizontal bar — shifted 1px lower
            renderer->drawRect(DPAD_CX - (FULL + LIP*2)/2 -1, DPAD_CY - (ARM_H + LIP*2)/2 + 6,
                               FULL + LIP*2 +1, ARM_H + LIP*2 -1, BK);
        }

        // A button: filled black circle matching the hit-test radius.
        renderer->drawCircle(g_abtn_hx, g_abtn_hy + 8, ABTN_R, true, BK);

        // B button: filled black circle matching the hit-test radius.
        renderer->drawCircle(g_bbtn_hx, g_bbtn_hy + 6, BBTN_R, true, BK);

        // Plus/Minus: pill-shaped rounded rect spanning both glyphs (and the
        // divider between them) with even padding.  Width is computed at runtime
        // so it automatically expands if the divider symbol is wider than the gap.
        {
            static constexpr int PAD_X = 6, PAD_Y = 4, Y_SHIFT = 1;
            // Left edge: leftmost of (select glyph, divider left half)
            const int left_edge  = std::min(SELECT_DRAW_X,      FB_W / 2 - g_div_half_w);
            // Right edge: rightmost of (start glyph right, divider right half)
            const int right_edge = std::max(START_DRAW_X + START_SIZE, FB_W / 2 + g_div_half_w);
            const int rx = left_edge  - PAD_X;
            const int ry = (START_DRAW_Y - START_SIZE) - PAD_Y + Y_SHIFT + 4;
            const int rw = (right_edge - left_edge) + PAD_X * 2;
            const int rh = START_SIZE + PAD_Y * 2;
            renderer->drawUniformRoundedRect(rx, ry, rw, rh, BK);
        }

        // ── D-pad composite — all four arrow directions ───────────────────────
        // Each glyph is treated as a 3×3 grid; we scissor to the center strip.
        // Strip width/height = 38% of the total glyph dimension — wider than
        // one third (33%) so the arrow arms are fully visible, but narrow enough
        // to hide the diagonal corners of the overlapping glyph.
        {
            const s32 baseline  = DPAD_DRAW_Y - 10;
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

            // E115 (↑↓): narrow strip horizontally (stripW), but fully unbounded
            // vertically — scissor spans the entire screen height so arrow tips
            // are never clipped regardless of font ascender/descender metrics.
            renderer->enableScissoring(left + (static_cast<s32>(dw) - stripW) / 2, 0,
                                       stripW, FB_H);
            renderer->drawString("\uE115", false, left, baseline, DPAD_SIZE, VBTN_COLOR);
            renderer->disableScissoring();

            // E116 (←→): narrow strip vertically (stripH), but fully unbounded
            // horizontally — scissor spans the entire screen width so arrow tips
            // are never clipped at either side.
            const s32 rowNudge = thirdH / 4;
            renderer->enableScissoring(0, top + (static_cast<s32>(dh) - stripH) / 2 + rowNudge +4,
                                       FB_W, stripH);
            renderer->drawString("\uE116", false, left, baseline, DPAD_SIZE, VBTN_COLOR);
            renderer->disableScissoring();

            // Centre circle — drawn last so it sits on top of both arrow glyphs
            // at the exact crossing point of the two black backing bars.
            // Values match DPAD_CX, DPAD_CY+4, and (ARM_W+LIP*2)/2 from the backing block.
            renderer->drawCircle(DPAD_DRAW_X + 67, DPAD_DRAW_Y - 10 - 56 + 4+1, 14, true, BK);
        }
        renderer->drawString("\uE0E0", false, ABTN_DRAW_X,  ABTN_DRAW_Y,  ABTN_SIZE,  VBTN_COLOR);
        renderer->drawString("\uE0E1", false, BBTN_DRAW_X,  BBTN_DRAW_Y,  BBTN_SIZE,  VBTN_COLOR);
        renderer->drawString("\uE0F0", false, SELECT_DRAW_X, SELECT_DRAW_Y + 1, SELECT_SIZE, VBTN_COLOR);
        renderer->drawString(ult::DIVIDER_SYMBOL, false,
            FB_W / 2 - g_div_half_w, START_DRAW_Y + 1, START_SIZE, 0xF444);
        renderer->drawString("\uE0EF", false, START_DRAW_X,  START_DRAW_Y  + 1, START_SIZE,  VBTN_COLOR);

        if (!ult::useRightAlignment)
            renderer->drawRect(447, 0, 448, 720, a(tsl::edgeSeparatorColor));
        else
            renderer->drawRect(0, 0, 1, 720, a(tsl::edgeSeparatorColor));
    }

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
    bool m_load_failed    = false; // deferred load failed; swap back to selector in update()
    bool m_restoreHapticState = false;
    bool runOnce = true;

    // ── ZR double-click-hold → fast-forward ───────────────────────────────
    // First ZR press arms the detector; a second press within ZR_DCLICK_WINDOW
    // frames that is then held activates fast-forward until ZR is released.
    static constexpr int ZR_DCLICK_WINDOW = 20; // ~333 ms at 60 fps
    bool     m_zr_first_seen  = false; // true after first ZR press is recorded
    uint32_t m_zr_first_frame = 0;     // g_frame_count when first press fired

public:
    ~GBOverlayGui() {
        if (m_restoreHapticState) {
            ult::useHapticFeedback = false;
            m_restoreHapticState = false;
        }
        //tsl::gfx::FontManager::clearCache(); // ALWAYS CLEAR BEFORE
        tsl::disableHiding = false;
    }

    virtual tsl::elm::Element* createUI() override {
        tsl::disableHiding = true;
        // ALWAYS release ult::Audio's audio session in windowed mode.
        ult::Audio::exit();
        // ── Deferred ROM load ─────────────────────────────────────────────────
        // g_pending_rom_path is set by the click listener instead of calling
        // gb_load_rom() there.  By the time createUI() runs, ~RomSelectorGui()
        // has already destroyed all MiniListItem objects and their std::string
        // labels, so the heap is fully defragmented before we call malloc(ROM).
        // We also clear the glyph cache here (belt-and-suspenders: ~RomSelectorGui
        // already cleared it, but a second clear is free if the cache is empty).
        if (g_pending_rom_path[0] != '\0') {
            tsl::gfx::FontManager::clearCache(); // ensure glyphs are gone

            char path[PATH_BUFFER_SIZE];
            strncpy(path, g_pending_rom_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            g_pending_rom_path[0] = '\0'; // consume before the load attempt

            if (!gb_load_rom(path)) {
                // Load failed (OOM, bad ROM, etc.) — notification already shown
                // by gb_load_rom.  Signal update() to swap back to the selector.
                m_load_failed = true;
            }
        }

        if (m_load_failed) {
            // Don't activate the emulator; update() will swap back immediately.
            g_emu_active = false;
            ult::noClickableItems.store(false, std::memory_order_release);
        } else {
            g_emu_active = true;
            m_waitForRelease = true;
            // No bottom bar is drawn in-game. Tell the framework there are no
            // clickable items so footer-zone touches don't highlight the select
            // button or fire its rumble / simulation callbacks.
            ult::noClickableItems.store(true, std::memory_order_release);
        }
        return new GBOverlayElement();
    }

    virtual void update() override {
        // Deferred load failed (OOM, bad ROM header, etc.):
        // In all modes — including quick-launch — swap to the ROM selector so the
        // user sees the memory-tier notification and can pick a compatible game.
        // "Can't load → open overlay like normal" rather than silently closing.
        if (m_load_failed) {
            m_load_failed = false;
            tsl::swapTo<RomSelectorGui>();
        }

        if (runOnce) {
            if (g_ingame_haptics && !ult::useHapticFeedback) {
                ult::useHapticFeedback = true;
                m_restoreHapticState = true;
            }
            if (g_self_path[0]) {
                returnOverlayPath = std::string(g_self_path);
            }
            runOnce = false;
        }
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)leftJoy; (void)rightJoy;

        // Block all input until the buttons that launched us are fully released.
        // keysHeld stays non-zero as long as any button remains physically held,
        // so we wait for a clean frame before passing anything to the GB core.
        if (m_waitForRelease) {
            if (keysHeld) return true;
            g_touch_keys = 0;
            m_prevTouchKeys = 0;
            m_waitForRelease = false;
        }

        // ── Overlay overlay mode: Ultrahand launch combo → close ───────────────
        // In quick-launch (overlay) mode the Ultrahand show/hide combo closes
        // the overlay entirely.  Hiding is disabled in initServices() so the
        // combo never hides — it reaches us here instead, exactly mirroring the
        // combo_pressed() path in GBWindowedGui.
        // gb_audio_pause() before close() matches windowed behaviour.
        // launchComboHasTriggered suppresses Tesla's own directMode double-click
        // so we don't get duplicate exit feedback.
        if (g_quick_launch || g_directMode || g_overlay_mode) {
            if ((keysDown & tsl::cfg::launchCombo) && (((keysDown | keysHeld) & tsl::cfg::launchCombo) == tsl::cfg::launchCombo)) {
                g_touch_keys = 0;
                ult::noClickableItems.store(false, std::memory_order_release);
                gb_audio_pause();
    
                launchComboHasTriggered.store(true, std::memory_order_release);
                if (!g_directMode && g_self_path[0]) {
                    const std::string returnArg = "-returning";
                    tsl::setNextOverlay(std::string(g_self_path), returnArg);
                }
    
                tsl::Overlay::get()->close();
                return true;
            }
        }

        // ── Normal mode only: X → back to ROM picker ──────────────────────────
        // Checked before any other work so touch mapping, tap-detector updates,
        // haptic setup, and gb_set_input are all skipped on the exit frame.
        // In overlay overlay mode KEY_X is a free button passed to the GB core.
        if (!g_quick_launch && combo_pressed(keysDown, keysHeld)) {
            g_touch_keys = 0;
            ult::noClickableItems.store(false, std::memory_order_release);

            triggerExitFeedback();

            if (g_overlay_mode && g_self_path[0]) {
                // Player mode: mirror the windowed combo exit exactly.
                // Pause audio immediately so output stops before the process winds
                // down; exitServices() will call gb_unload_rom() (saves state +
                // SRAM) and gb_audio_free_dma() in the correct teardown order.
                gb_audio_pause();
                tsl::setNextOverlay(g_self_path, "-returning");
                tsl::Overlay::get()->close();
            } else {
                // Normal in-process swap: save + unload here because the Overlay
                // process stays alive and RomSelectorGui needs a clean slate.
                // exitServices() is NOT called on swapTo, so we do it ourselves.
                gb_unload_rom();  // saves state, writes SRAM, shuts down audio

                if (ult::useSoundEffects && !ult::limitedMemory)
                    ult::Audio::initialize();

                jumpToTop.store(false, std::memory_order_release);
                jumpToBottom.store(false, std::memory_order_release);
                skipUp.store(false, std::memory_order_release);
                skipDown.store(false, std::memory_order_release);
                // Signal RomSelectorGui to swallow input until all keys are
                // released — in-game button presses must not fire list jumps.
                g_waitForInputRelease = true;
                tsl::swapTo<RomSelectorGui>();
            }
            return true;
        }

        // ── Shared touch state — computed once, used by vp-tap and touch-keys ──
        // tx/ty/touching were previously computed independently inside both the
        // vp-tap block and the touch-keys block via identical cast expressions.
        // Hoisting them here means one set of casts and one predicate per frame.
        const int  tx       = static_cast<int>(touchPos.x) - static_cast<int>(ult::layerEdge);
        const int  ty       = static_cast<int>(touchPos.y);
        const bool touching = (touchPos.x != 0 || touchPos.y != 0) &&
                              ty < FOOTER_Y;

        // ── ZR double-click-hold → fast-forward ──────────────────────────────
        // First ZR press arms the detector.  A second ZR press within
        // ZR_DCLICK_WINDOW frames (~333 ms) that is then held activates
        // fast-forward for as long as ZR remains held.
        // On activate: audio is paused — the audio thread drains the SPSC ring
        // silently and preserves all GBAPU state, so playback resumes cleanly.
        // On release: audio resumes and the frame clock is re-anchored so there
        // is no catch-up burst of normal frames after the fast-forward ends.
        {
            const bool zr_down = (keysDown & KEY_ZR);
            const bool zr_held = (keysHeld  & KEY_ZR);

            if (zr_down) {
                if (m_zr_first_seen &&
                    (g_frame_count - m_zr_first_frame) <= (uint32_t)ZR_DCLICK_WINDOW) {
                    // Second press within window — activate fast-forward
                    if (!g_fast_forward) {
                        g_fast_forward = true;
                        gb_audio_pause();
                    }
                    m_zr_first_seen = false;
                } else {
                    // First press — arm the double-click detector
                    m_zr_first_seen  = true;
                    m_zr_first_frame = g_frame_count;
                }
            }

            // Expire the first-press if the window elapsed with no second press
            if (m_zr_first_seen &&
                (g_frame_count - m_zr_first_frame) > (uint32_t)ZR_DCLICK_WINDOW)
                m_zr_first_seen = false;

            // Disengage when ZR is released
            if (g_fast_forward && !zr_held) {
                g_fast_forward     = false;
                g_gb_frame_next_ns = 0;   // re-anchor so no catch-up burst
                gb_audio_resume();
            }
        }

        // ── Touch → virtual button state ──────────────────────────────────────
        // Sample the first active touch point each frame and map it to GB keys.
        // D-pad touch area is split into 4 directional arms by comparing the
        // absolute x and y deltas from the pad centre — whichever axis dominates
        // determines the pressed direction.  A, B, and Start are circular zones.
        // HidTouchState in this libtesla fork is a single resolved touch point
        // with flat .x / .y fields — no .count or .touches[] array.
        // (0, 0) is the sentinel meaning "no active touch".
        //
        // Any touch with an initial y >= FOOTER_Y is intercepted by the
        // framework as a footer button press even when the bar isn't drawn.
        // We simply ignore those touches so they never reach the GB core.
        // tx / ty / touching are already resolved above.
        g_touch_keys = 0;
        if (touching) {
            // D-pad — stop-sign layout aligned to the drawn black rectangles.
            //
            // The cross is divided into 9 zones matching the physical shape:
            //   • Centre overlap (|dx|<=HALF_ARM && |dy|<=HALF_ARM) → dead zone
            //   • Top / bottom protrusion (|dx|<=HALF_ARM, outside centre) → UP / DOWN
            //   • Left / right protrusion (|dy|<=HALF_ARM, outside centre) → LEFT / RIGHT
            //   • Four corner triangles (outside both arms' widths) → two simultaneous
            //     directions, e.g. top-right corner → UP + RIGHT.
            //
            // HALF_ARM matches the backing rectangle arm half-widths (ARM_W=46, LIP=4 → 27 px).
            // HALF_SPAN matches half the full arm length (FULL=131, LIP*2=8, +pad → 70 px).
            // D_CY is the backing rect visual centre (DPAD_CY + 5 ≈ 577).
            {
                static constexpr int D_CX        = DPAD_DRAW_X + 67;  // backing rect centre X
                static constexpr int D_CY        = DPAD_DRAW_Y - 61;  // backing rect centre Y
                static constexpr int D_HALF_ARM  = 27;  // half the narrow arm width / height
                static constexpr int D_HALF_SPAN = 70;  // half the full arm length

                const int dx = tx - D_CX;
                const int dy = ty - D_CY;

                if (std::abs(dx) <= D_HALF_SPAN && std::abs(dy) <= D_HALF_SPAN) {
                    const bool in_v = std::abs(dx) <= D_HALF_ARM;  // within vertical bar's width
                    const bool in_h = std::abs(dy) <= D_HALF_ARM;  // within horizontal bar's height

                    if (in_v && in_h) {
                        // Centre overlap — dead zone, no input
                    } else if (in_v) {
                        // Top or bottom protrusion
                        g_touch_keys |= (dy > 0) ? KEY_DOWN : KEY_UP;
                    } else if (in_h) {
                        // Left or right protrusion
                        g_touch_keys |= (dx > 0) ? KEY_RIGHT : KEY_LEFT;
                    } else {
                        // Corner right-triangle — two simultaneous directions
                        g_touch_keys |= (dx > 0) ? KEY_RIGHT : KEY_LEFT;
                        g_touch_keys |= (dy > 0) ? KEY_DOWN : KEY_UP;
                    }
                }
            }

            // A button
            {
                const int dx = tx - g_abtn_hx, dy = ty - g_abtn_hy;
                if (dx*dx + dy*dy <= ABTN_R * ABTN_R) {
                    g_touch_keys |= KEY_A;
                }
            }

            // B button
            {
                const int dx = tx - g_bbtn_hx, dy = ty - g_bbtn_hy;
                if (dx*dx + dy*dy <= BBTN_R * BBTN_R) {
                    g_touch_keys |= KEY_B;
                }
            }

            // Start (+)
            {
                const int dx = tx - g_start_hx, dy = ty - g_start_hy;
                if (dx*dx + dy*dy <= START_R * START_R) {
                    g_touch_keys |= KEY_PLUS;
                }
            }

            // Select (−)
            {
                const int dx = tx - g_select_hx, dy = ty - g_select_hy;
                if (dx*dx + dy*dy <= SELECT_R * SELECT_R) {
                    g_touch_keys |= KEY_MINUS;
                }
            }
        }

        // Trigger rumble ONLY on new touch presses (not holds)
        u64 newTouchPresses = g_touch_keys & ~m_prevTouchKeys;
        if (newTouchPresses && g_ingame_haptics) {
            triggerRumbleClick.store(true, std::memory_order_release);
        }
        m_prevTouchKeys = g_touch_keys;

        // Pass physical button state and virtual touch keys to the GB core.
        // keysDown is always a subset of keysHeld in libnx (padGetButtonsDown
        // returns only buttons that transitioned to held this update, all of
        // which are already present in padGetButtons), so OR-ing keysDown in
        // separately is redundant — keysHeld alone covers every pressed button.
        gb_set_input(keysHeld | g_touch_keys);

        // Trigger rumble on ANY new button press
        if (g_ingame_haptics &&
            (keysDown & (KEY_A | KEY_B | KEY_X | KEY_Y | KEY_PLUS | KEY_MINUS |
                         KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))) {
            triggerRumbleClick.store(true, std::memory_order_release);
        }

        return true;  // consume all input while in-game
    }
};