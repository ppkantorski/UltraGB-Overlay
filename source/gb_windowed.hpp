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
 *
 *   Pixel blit:
 *     LUTs encode the Tesla block-linear address formula for the scaled
 *     framebuffer.  OWV (outer-width-value, the inter-strip stride) depends on
 *     framebuffer width and is recomputed per scale.  For each source GB pixel
 *     (sx, sy) the blit writes g_win_scale × g_win_scale destination pixels.
 *     s_win_col / s_win_row are sized for max 3× (480 / 432 entries).
 *
 *   Touch drag:
 *     Same scheme as 1× — polls HID directly.  Window size in touch space
 *     scales with g_win_scale.  VI max position is recomputed per scale.
 *     Saved position is clamped to the new valid range on every createUI()
 *     so a scale change never places the window off-screen.
 *
 *   VI / touch coordinate math (scale N):
 *     FB size: GB_W×N  ×  GB_H×N   (framebuffer pixels)
 *     VI size: GB_W×N×3/2  ×  GB_H×N×3/2   (VI space, 1.5× display scale)
 *     Max safe VI pos: 1920 − VI_W  ×  1080 − VI_H
 *     Window in touch space: GB_W×N  wide  ×  GB_H×N  tall
 *     touch→VI: ×3/2      VI→touch: ×2/3
 *
 *   Included at the bottom of main.cpp, after all globals and helpers are
 *   fully defined.  g_win_scale must be declared in main.cpp before this
 *   header is included.
 ********************************************************************************/

#pragma once

#include <cstring>
#include <algorithm>
// gb_renderer.h (rgb555_to_packed, rgb565_to_packed) is already included above.

// ── Swizzle LUTs for the scaled framebuffer ───────────────────────────────────
// Tesla block-linear formula (from tesla.hpp getPixelOffset, no scissor):
//   offset = ((((y&127)>>4) + ((x>>5)<<3) + ((y>>7)*OWV)) << 9)
//            + ((y&8)<<5) + ((x&16)<<3) + ((y&6)<<4) + ((x&8)<<1)
//            + ((y&1)<<3) + (x&7)
//
// OWV (outer-width-value) depends on framebuffer width W:
//   OWV = ((W/2) >> 4) << 3  =  (W >> 5) << 3  =  (W / 32) * 8
//
//   Scale 1: W=160 → OWV=40    Scale 2: W=320 → OWV=80    Scale 3: W=480 → OWV=120
//
// The formula splits into col_part(x) + row_part(y):
//   col_part(x) = (((x>>5)<<3)<<9) + ((x&16)<<3) + ((x&8)<<1) + (x&7)
//   row_part(y) = ((((y&127)>>4) + ((y>>7)*OWV))<<9)
//                + ((y&8)<<5) + ((y&6)<<4) + ((y&1)<<3)
//
// Arrays are sized for maximum 3× scale: 480 cols, 432 rows.
static uint32_t s_win_col[GB_W * 4];  // up to 640 entries (4× scale)
static uint32_t s_win_row[GB_H * 4];  // up to 576 entries (4× scale)
static bool     s_win_lut_ready = false;
// Set true by GBWindowedGui::handleInput while a touch-drag reposition is active.
// Read by GBWindowedElement::draw to overlay a 4-pixel red border on the frame.
static bool     s_win_dragging  = false;
static int      s_focus_flash   = 0;     // counts down each draw; 0 = hidden
static bool     s_focus_flash_red = false; // true=red (focus lost), false=green (focus gained)

// Update the notification hit-test offsets to match the current VI layer position.
// Must be called whenever g_win_pos_x / g_win_pos_y change.
static void sync_notif_touch_offsets() {
    // layerEdge already drives the notification hit-test X — repurpose it.
    ult::layerEdge = (g_win_pos_x * 2) / 3;
    tsl::layerEdgeY = (g_win_pos_y * 2) / 3;
}

// Build col/row LUT for the given scale.  Called once per windowed session
// (statics are zero-initialised on each overlay launch → s_win_lut_ready=false).
static void init_win_luts(int scale) {
    const int fw = GB_W * scale;
    const int fh = GB_H * scale;
    // OWV = (fw / 32) * 8  — integer arithmetic, exact for all three scales.
    const uint32_t OWV = (static_cast<uint32_t>(fw) >> 5u) << 3u;

    for (int i = 0; i < fw; ++i) {
        const uint32_t x = static_cast<uint32_t>(i);
        s_win_col[i] = (((x >> 5u) << 3u) << 9u)
                     + ((x & 16u) << 3u)
                     + ((x &  8u) << 1u)
                     +  (x &  7u);
    }
    for (int i = 0; i < fh; ++i) {
        const uint32_t y = static_cast<uint32_t>(i);
        s_win_row[i] = ((((y & 127u) >> 4u) + ((y >> 7u) * OWV)) << 9u)
                     + ((y &  8u) << 5u)
                     + ((y &  6u) << 4u)
                     + ((y &  1u) << 3u);
    }
    s_win_lut_ready = true;
}

// =============================================================================
// GBWindowedElement
// Blits the GB framebuffer (160×144) into the scaled Tesla framebuffer via LUTs.
// For scale N: each source pixel writes N×N destination framebuffer pixels.
// Also drives the emulation clock (mirrors GBScreenElement::draw exactly).
// This is a leaf element — no children, no focus, no touch.
// =============================================================================
class GBWindowedElement : public tsl::elm::Element {
public:
    void draw(tsl::gfx::Renderer* renderer) override {

        if (!g_gb.running || !g_emu_active || !g_gb_fb) {
            renderer->fillScreen({0x0, 0x0, 0x0, 0xF});
            return;
        }

        // ── Emulation clock ───────────────────────────────────────────────────
        ++g_frame_count;
        if (!s_win_dragging) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const int64_t now_ns = (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;

            if (g_gb_frame_next_ns == 0)
                g_gb_frame_next_ns = now_ns;

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

                if (g_cgb_bounce_frames >= 0) {
                    if (++g_cgb_bounce_frames >= 60) {
                        g_cgb_bounce_frames = -1;
                        char path[256] = {};
                        strncpy(path, g_gb.romPath, sizeof(path) - 1);
                        s_bounce_keepalive = true;
                        const bool was_evicted = g_wallpaper_evicted;
                        if (was_evicted) g_wallpaper_evicted = false;
                        gb_unload_rom();
                        if (was_evicted) g_wallpaper_evicted = true;
                        if (gb_load_rom(path)) g_emu_active = true;
                        s_bounce_keepalive = false;
                    }
                }
            }
        }

        // ── Scaled pixel blit ─────────────────────────────────────────────────
        // For scale N, each source GB pixel (sx, sy) is replicated into an
        // N×N block in the framebuffer at positions
        //   (sx*N .. sx*N+N-1, sy*N .. sy*N+N-1).
        // Uses public setPixelAtOffset() — getCurrentFramebuffer() is private.
        // tsl::Color is 16-bit RGBA4444, same bit layout as our packed uint16_t.
        if (!s_win_lut_ready) init_win_luts(g_win_scale);

        const int  scale     = g_win_scale;
        const bool is565     = g_fb_is_rgb565;
        const bool prepacked = g_fb_is_prepacked;

        for (int sy = 0; sy < GB_H; ++sy) {
            const uint16_t* src_row = g_gb_fb + sy * GB_W;
            for (int sx = 0; sx < GB_W; ++sx) {
                const uint16_t src = src_row[sx];
                uint16_t packed;
                if (prepacked)    packed = src;
                else if (is565)   packed = rgb565_to_packed(src);
                else              packed = rgb555_to_packed(src);
                const tsl::Color& col = reinterpret_cast<const tsl::Color&>(packed);

                // Write scale×scale destination pixels for this source pixel.
                for (int dy = 0; dy < scale; ++dy) {
                    const uint32_t row_base = s_win_row[sy * scale + dy];
                    for (int dx = 0; dx < scale; ++dx) {
                        renderer->setPixelAtOffset(
                            row_base + s_win_col[sx * scale + dx], col);
                    }
                }
            }
        }

        // ── Pass-through flash border ────────────────────────────────────────
        if (s_focus_flash > 0) {
            const s32 fw = static_cast<s32>(GB_W * g_win_scale);
            const s32 fh = static_cast<s32>(GB_H * g_win_scale);
            const u8  al = s_focus_flash > 15
                ? static_cast<u8>(0xF)
                : static_cast<u8>(s_focus_flash * 0xF / 15);
            const tsl::Color fc = s_focus_flash_red
                ? tsl::Color{0xF, 0x0, 0x0, al}
                : tsl::Color{0x0, 0xF, 0x0, al};
            static constexpr int B = 4;
            renderer->drawRect(0,      0,       fw, B,           fc);
            renderer->drawRect(0,      fh - B,  fw, B,           fc);
            renderer->drawRect(0,      B,       B,  fh - B * 2,  fc);
            renderer->drawRect(fw - B, B,       B,  fh - B * 2,  fc);
            --s_focus_flash;
        }

        // ── Reposition overlay ────────────────────────────────────────────────
        // While dragging: dim the frozen frame and show "Paused" centred.
        if (s_win_dragging) {
            const s32 fw = static_cast<s32>(GB_W * g_win_scale);
            const s32 fh = static_cast<s32>(GB_H * g_win_scale);

            // Semi-transparent black veil (~50 % opacity in RGBA4444).
            static constexpr tsl::Color DIM = {0x0, 0x0, 0x0, 0x8};
            renderer->drawRect(0, 0, fw, fh, DIM);

            // Red border (4 px) so the window boundary is obvious.
            static constexpr int        BORD = 4;
            static constexpr tsl::Color RED  = {0xF, 0x0, 0x0, 0xF};
            renderer->drawRect(0,         0,          fw,   BORD,              RED);
            renderer->drawRect(0,         fh - BORD,  fw,   BORD,              RED);
            renderer->drawRect(0,         BORD,       BORD, fh - BORD * 2,     RED);
            renderer->drawRect(fw - BORD, BORD,       BORD, fh - BORD * 2,     RED);

            // "Paused" centred in white.
            static constexpr tsl::Color WHITE = {0xF, 0xF, 0xF, 0xF};
            const u32 fontSize = static_cast<u32>(14 * g_win_scale);
            const auto [tw, th] = renderer->getTextDimensions("Paused", false, fontSize);
            renderer->drawString("Paused", false,
                (fw - static_cast<s32>(tw)) / 2,
                (fh + static_cast<s32>(th)) / 2,
                fontSize, WHITE);
        }
    }

    void layout(u16, u16, u16, u16) override {
        // The framebuffer IS the layer; always fill it entirely.
        // Size = GB_W*scale × GB_H*scale, matching DefaultFramebufferWidth/Height.
        setBoundaries(0, 0, GB_W * g_win_scale, GB_H * g_win_scale);
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
    void draw(tsl::gfx::Renderer* renderer) override {
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

    // ── KEY_PLUS 2s hold → joystick reposition state ─────────────────────────
    bool     m_plus_dragging      = false;  // actively repositioning via joystick
    bool     m_plus_armed         = false;  // KEY_PLUS held alone; timer running
    uint64_t m_plus_hold_start_ns = 0;      // ns timestamp when hold began
    float    m_joy_acc_x          = 0.f;    // sub-pixel accumulator, VI-space X
    float    m_joy_acc_y          = 0.f;    // sub-pixel accumulator, VI-space Y

    // 60 frames ≈ 1 second at the GB's 59.73 Hz render rate.
    static constexpr int      HOLD_FRAMES      = 60;
    // ZR double-click window for fast-forward toggle.
    static constexpr int      ZR_DCLICK_WINDOW = 20;
    // ZL double-click window for background pass-through toggle.
    static constexpr int      ZL_DCLICK_WINDOW = 20;
    // KEY_PLUS must be held alone for this long before joystick drag activates.
    static constexpr uint64_t PLUS_HOLD_NS     = 2'000'000'000ULL;  // 2 seconds
    // Joystick deadzone (HidAnalogStickState range: –32767..32767).
    static constexpr int      JOY_DEADZONE     = 20;
    // Mask of all physical buttons — used to confirm KEY_PLUS is held *alone*.
    
    bool     m_zr_first_seen  = false;
    uint32_t m_zr_first_frame = 0;

    // ZL double-click-hold: background pass-through toggle.
    bool     m_zl_first_seen  = false;
    uint32_t m_zl_first_frame = 0;
    bool     m_pass_through   = false;  // true = foreground released; background gets HID

    // True until the first frame where no keys AND no touch are active.
    // Prevents the ROM-tap touch (or any button held at launch) from
    // accidentally arming the hold timer on the very first frame.
    bool m_waitForRelease = true;

    bool m_load_failed = false;

    // ── Helpers ───────────────────────────────────────────────────────────────
    static bool combo_pressed(u64 keysDown, u64 keysHeld) {
        const u64 combo = tsl::cfg::launchCombo;
        return ((keysDown & combo) != 0) &&
               (((keysDown | keysHeld) & combo) == combo);
    }

    // Poll the HID touch screen.  Returns true if at least one finger is down.
    // We bypass the touchPos parameter from Tesla because Tesla clears its own
    // internal oldTouchPos/initialTouchPos every frame when touchPos.x exceeds
    // cfg::FramebufferWidth, corrupting its tracking.  The real state is
    // always available directly from HID.
    static bool poll_touch(int& out_x, int& out_y) {
        HidTouchScreenState ts = {};
        hidGetTouchScreenStates(&ts, 1);
        if (ts.count > 0) {
            out_x = static_cast<int>(ts.touches[0].x);
            out_y = static_cast<int>(ts.touches[0].y);
            return true;
        }
        out_x = 0;
        out_y = 0;
        return false;
    }

    // ── VI bounds helpers (scale-dependent) ───────────────────────────────────
    // VI space is 1920×1080.  The layer must fit entirely on screen.
    // The VI layer is GB_W*scale × GB_H*scale framebuffer pixels, which the
    // Switch display maps to (GB_W*scale × 3/2) × (GB_H*scale × 3/2) VI units.
    //
    // Maximum safe VI origin (top-left of layer):
    //   max_x = 1920 - GB_W * scale * 3 / 2
    //   max_y = 1080 - GB_H * scale * 3 / 2
    //
    // All three scale factors produce exact integers:
    //   1×: max_x=1680  max_y=864
    //   2×: max_x=1440  max_y=648
    //   3×: max_x=1200  max_y=432
    static int vi_max_x() { return 1920 - GB_W * g_win_scale * 3 / 2; }
    static int vi_max_y() { return 1080 - GB_H * g_win_scale * 3 / 2; }

    // Window footprint in HID touch space (0–1279 × 0–719):
    //   width  = GB_W * scale   height = GB_H * scale
    // (same numbers as framebuffer pixels, because touch→VI is exactly ×3/2)
    static int touch_win_w() { return GB_W * g_win_scale; }
    static int touch_win_h() { return GB_H * g_win_scale; }

public:
    ~GBWindowedGui() {
        ult::noClickableItems.store(false, std::memory_order_release);
    }

    // ── createUI ─────────────────────────────────────────────────────────────
    tsl::elm::Element* createUI() override {
        // ALWAYS release ult::Audio's audio session in windowed mode.
        ult::Audio::exit();

        // Ensure screenshots work.
        screenshotsAreDisabled.store(false, std::memory_order_release);
        screenshotsAreForceDisabled.store(false, std::memory_order_release);
        tsl::gfx::Renderer::get().addScreenshotStacks();

        // Load the ROM.  g_pending_rom_path was set from windowed_rom in
        // WindowedOverlay::loadInitialGui().
        if (g_pending_rom_path[0] != '\0') {
            tsl::gfx::FontManager::clearCache();
            char path[256];
            strncpy(path, g_pending_rom_path, sizeof(path) - 1);
            path[sizeof(path) - 1] = '\0';
            g_pending_rom_path[0]  = '\0';
            if (!gb_load_rom(path))
                m_load_failed = true;
        }

        g_emu_active     = !m_load_failed;
        m_waitForRelease = !m_load_failed;

        // Suppress Tesla's footer touch handling entirely.
        ult::noClickableItems.store(true,  std::memory_order_release);
        ult::backWidth.store(0.0f,         std::memory_order_release);
        ult::selectWidth.store(0.0f,       std::memory_order_release);
        ult::nextPageWidth.store(0.0f,     std::memory_order_release);
        ult::halfGap.store(0.0f,           std::memory_order_release);
        ult::hasNextPageButton.store(false, std::memory_order_release);

        // Clamp the saved/default position to the valid range for the current
        // scale.  This is the only guard needed: if the user changed the scale
        // between sessions the old position may land outside the screen.
        //   E.g. pos_x=1600 was valid at 1× (max=1680) but invalid at 3× (max=1200).
        {
            const int mx = vi_max_x();
            const int my = vi_max_y();
            g_win_pos_x = std::max(0, std::min(mx, g_win_pos_x));
            g_win_pos_y = std::max(0, std::min(my, g_win_pos_y));
        }

        // Position the VI layer at the clamped/default location.
        tsl::gfx::Renderer::get().setLayerPos(
            static_cast<u32>(g_win_pos_x),
            static_cast<u32>(g_win_pos_y));
        sync_notif_touch_offsets();

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
    }

    // ── handleInput ──────────────────────────────────────────────────────────
    bool handleInput(u64 keysDown, u64 keysHeld,
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
            if (g_self_path[0])
                tsl::setNextOverlay(std::string(g_self_path));
            tsl::Overlay::get()->close();
            return true;
        }

        // ── ZR double-click-hold: fast-forward ────────────────────────────────
        {
            const bool zr_down = (keysDown & KEY_ZR) != 0;
            const bool zr_held = (keysHeld  & KEY_ZR) != 0;
            if (zr_down) {
                if (m_zr_first_seen &&
                    (g_frame_count - m_zr_first_frame) <= static_cast<uint32_t>(ZR_DCLICK_WINDOW)) {
                    if (!g_fast_forward) { g_fast_forward = true; gb_audio_pause(); }
                    m_zr_first_seen = false;
                } else {
                    m_zr_first_seen  = true;
                    m_zr_first_frame = g_frame_count;
                }
            }
            if (m_zr_first_seen &&
                (g_frame_count - m_zr_first_frame) > static_cast<uint32_t>(ZR_DCLICK_WINDOW))
                m_zr_first_seen = false;
            if (g_fast_forward && !zr_held) {
                g_fast_forward     = false;
                g_gb_frame_next_ns = 0;
                gb_audio_resume();
            }
        }

        // ── Physical buttons → GB joypad ──────────────────────────────────────
        // Suppress all input while any reposition mode is active (touch drag or
        // joystick drag) so the game never sees buttons held during repositioning.
        // Also suppress when pass-through is active: foreground has been released
        // so the background app owns HID natively; we must not double-route input.
        if (!m_dragging && !m_plus_dragging && !m_pass_through)
            gb_set_input(keysHeld | keysDown);

        // ── ZL double-click-hold: toggle background pass-through ──────────────
        // Double-clicking ZL (second press within ZL_DCLICK_WINDOW frames) toggles
        // pass-through mode.  In pass-through mode requestForeground(false) releases
        // HID ownership so the Switch routes controller input natively to whatever
        // app/game is running underneath.  requestForeground(true) reclaims it so
        // gb_set_input can drive the emulator again.
        // Note: ZL reaching handleInput means the overlay is still shown — Tesla
        // continues drawing the game frame, but controller input bypasses us.
        {
            const bool zl_down = (keysDown & KEY_ZL) != 0;
            if (zl_down) {
                if (m_zl_first_seen &&
                    (g_frame_count - m_zl_first_frame) <= static_cast<uint32_t>(ZL_DCLICK_WINDOW)) {
                    // Second press within the window — commit the toggle.
                    m_pass_through = !m_pass_through;
                    tsl::hlp::requestForeground(!m_pass_through);
                    m_zl_first_seen  = false;
                    s_focus_flash_red = m_pass_through; // true=lost focus, false=gained
                    s_focus_flash     = 45;
                } else {
                    // First press — record it and wait.
                    m_zl_first_seen  = true;
                    m_zl_first_frame = g_frame_count;
                }
            }
            // Expire a stale first-press that was never followed up.
            if (m_zl_first_seen &&
                (g_frame_count - m_zl_first_frame) > static_cast<uint32_t>(ZL_DCLICK_WINDOW))
                m_zl_first_seen = false;
        }

        // ── Touch hold-to-drag (via direct HID poll) ──────────────────────────
        // Coordinate spaces:
        //   Touch: 0..1279 × 0..719  (HID native, Switch screen pixels)
        //   VI:    0..1919 × 0..1079 (× 1.5 display scale factor)
        //
        // Window in touch space:
        //   origin:  (g_win_pos_x*2/3,  g_win_pos_y*2/3)
        //   size:    GB_W*scale × GB_H*scale  (= framebuffer pixels at scale N)
        //
        // Converting touch delta → VI delta:  delta_vi = delta_touch * 3/2
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
                    triggerNavigationFeedback();  // haptic: drag started
                }

                if (m_dragging) {
                    // Convert touch delta to VI delta (×3/2) and apply.
                    // Clamp to the scale-appropriate VI bounds so the window
                    // never crosses a screen edge regardless of scale.
                    const int dx = (tx - m_touch_start_x) * 3 / 2;
                    const int dy = (ty - m_touch_start_y) * 3 / 2;
                    const int nx = std::max(0, std::min(vi_max_x(), m_pos_start_x + dx));
                    const int ny = std::max(0, std::min(vi_max_y(), m_pos_start_y + dy));
                    if (nx != g_win_pos_x || ny != g_win_pos_y) {
                        g_win_pos_x = nx;
                        g_win_pos_y = ny;
                        tsl::gfx::Renderer::get().setLayerPos(
                            static_cast<u32>(g_win_pos_x),
                            static_cast<u32>(g_win_pos_y));
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
                    triggerExitFeedback(); // haptic: position locked
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
                && !(keysHeld & ~static_cast<u64>(KEY_PLUS) & ALL_KEYS_MASK);

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
                        triggerNavigationFeedback(); // haptic: drag started
                    }
                }

                if (m_plus_dragging) {
                    // Move the window with the left stick.
                    // x^8 sensitivity curve: stays fine at low deflection, accelerates near full.
                    if (std::abs(leftJoy.x) > JOY_DEADZONE
                        || std::abs(leftJoy.y) > JOY_DEADZONE) {
                        const float mag  = sqrtf(
                            static_cast<float>(leftJoy.x) * static_cast<float>(leftJoy.x)
                            + static_cast<float>(leftJoy.y) * static_cast<float>(leftJoy.y));
                        const float norm  = mag / 32767.f;
                        const float curve = powf(norm, 8.f);
                        static constexpr float BASE_SENS = 0.00008f;
                        static constexpr float MAX_SENS  = 0.0005f;
                        const float sens = BASE_SENS + (MAX_SENS - BASE_SENS) * curve;

                        // Accumulate fractional VI-space movement; Y axis is inverted
                        // (stick up → negative y → window moves up).
                        m_joy_acc_x +=  static_cast<float>(leftJoy.x) * sens;
                        m_joy_acc_y += -static_cast<float>(leftJoy.y) * sens;

                        const int dx = static_cast<int>(m_joy_acc_x);
                        const int dy = static_cast<int>(m_joy_acc_y);
                        m_joy_acc_x -= static_cast<float>(dx);
                        m_joy_acc_y -= static_cast<float>(dy);

                        const int nx = std::max(0, std::min(vi_max_x(), g_win_pos_x + dx));
                        const int ny = std::max(0, std::min(vi_max_y(), g_win_pos_y + dy));
                        if (nx != g_win_pos_x || ny != g_win_pos_y) {
                            g_win_pos_x = nx;
                            g_win_pos_y = ny;
                            tsl::gfx::Renderer::get().setLayerPos(
                                static_cast<u32>(g_win_pos_x),
                                static_cast<u32>(g_win_pos_y));
                            sync_notif_touch_offsets();
                        sync_notif_touch_offsets();
                        }
                    }
                }
            } else {
                // KEY_PLUS released (or another key was combined with it).
                if (m_plus_armed) {
                    if (m_plus_dragging) {
                        save_win_pos();        // persist VI coords to config.ini
                        triggerExitFeedback(); // haptic: position locked
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

// =============================================================================
// WindowedOverlay
// =============================================================================
class WindowedOverlay : public tsl::Overlay {
    // Guard for onShow/onHide: only resume audio when we actually paused it.
    // On the very first show, gb_audio_init() inside gb_load_rom has already
    // started the audio thread — calling gb_audio_resume() on a never-paused
    // system causes glitches.  We only resume after a genuine onHide() call.
    bool m_audio_paused = false;

public:
    void initServices() override {
        tsl::overrideBackButton = true;
        ult::COPY_BUFFER_SIZE   = 1024;

        // Prevent Tesla from hiding the overlay when the user touches outside
        // the framebuffer region.  disableHiding suppresses it.
        // All hiding is done explicitly via the launch combo.
        tsl::disableHiding = true;

        ult::createDirectory(CONFIG_DIR);
        ult::createDirectory(SAVE_DIR);
        ult::createDirectory(INTERNAL_SAVE_DIR);
        ult::createDirectory(STATE_BASE_DIR);
        ult::createDirectory(INTERNAL_STATE_DIR);
        ult::createDirectory(CONFIGURE_DIR);

        load_config();
        write_default_config_if_missing();

        // Read the ROM path from config.ini.
        const std::string wrom = ult::parseValueFromIniSection(
            kConfigFile, "config", "windowed_rom");
        if (!wrom.empty() && wrom.size() < sizeof(g_win_rom_path) - 1) {
            strncpy(g_win_rom_path, wrom.c_str(), sizeof(g_win_rom_path) - 1);
            g_win_rom_path[sizeof(g_win_rom_path) - 1] = '\0';
        }
        // Erase immediately so the key never persists across unrelated launches.
        ult::setIniFileValue(kConfigFile, "config", "windowed_rom", "", "");
    }

    void exitServices() override {
        tsl::hlp::requestForeground(true);  // reclaim HID if pass-through was active
        tsl::disableHiding = false;  // restore default for any subsequent overlay
        ult::layerEdge  = 0;       // restore for normal overlay hit-tests
        tsl::layerEdgeY = 0;
        gb_unload_rom();             // saves quick-resume state + SRAM
        gb_audio_free_dma();
        free_lcd_ghosting();
    }

    void onHide() override {
        tsl::hlp::requestForeground(true);  // reclaim HID if pass-through was active
        g_gb.running   = false;
        g_emu_active   = false;
        gb_audio_pause();
        m_audio_paused = true;
    }

    void onShow() override {
        if (g_gb.rom) {
            g_gb_frame_next_ns = 0;
            g_gb.running  = true;
            g_emu_active  = true;
            if (m_audio_paused) {
                gb_audio_resume();
                m_audio_paused = false;
            }
        }
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        if (g_win_rom_path[0]) {
            strncpy(g_pending_rom_path, g_win_rom_path, sizeof(g_pending_rom_path) - 1);
            g_pending_rom_path[sizeof(g_pending_rom_path) - 1] = '\0';
        }
        return initially<GBWindowedGui>();
    }
};