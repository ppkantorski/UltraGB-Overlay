/********************************************************************************
 * File: gb_utils.hpp
 * Description:
 *   One-stop utility header for UltraGB.
 *
 *   Pulls in gb_globals.hpp (which provides gb_audio.h, gb_core.h,
 *   gb_renderer.h, and all global variable definitions) then defines every
 *   small helper that is called from more than one file (main.cpp,
 *   gb_overlay.hpp, gb_windowed.hpp).
 *
 *   Include order in main.cpp:
 *     ultra.hpp / tesla.hpp          — ult::* / tsl::* namespaces
 *     elm_volume.hpp                 — VolumeTrackBar
 *     elm_ultraframe.hpp             — UltraGBOverlayFrame
 *     gb_utils.hpp  ← this file     — globals + all shared helpers
 *
 *   Rendering primitives (draw_wallpaper_direct, clear_fb_rows_transparent_448,
 *   clear_ovl_corners_448, render_ovl_free_border, and their supporting data
 *   tables) live in gb_renderer.h, which is included first via gb_globals.hpp.
 *
 *   All functions are [[gnu::noinline]] unless they are trivial one-liners
 *   where the call overhead would exceed the body.  noinline prevents LTO
 *   from re-expanding shared bodies at every call site.
 *
 *  Licensed under GPLv2
 *  Copyright (c) 2026 ppkantorski
 ********************************************************************************/

#pragma once

#include "gb_globals.hpp"       // all global variables + gb_audio.h/gb_core.h/gb_renderer.h
#include "elm_ultraframe.hpp"   // UltraGBOverlayFrame — needed by make_bare_frame

// Hot per-frame functions carry __attribute__((optimize("O3"))).
// Everything else compiles at Os to keep cold utility code lean.
#pragma GCC push_options
#pragma GCC optimize("Os")

// =============================================================================
// make_bare_frame
//
// Wraps the repeated three-line pattern in six createUI() bodies:
//   auto* frame = new UltraGBOverlayFrame("", "");
//   frame->setContent(list);
//   return frame;
// =============================================================================
[[gnu::noinline]]
static tsl::elm::Element* make_bare_frame(tsl::elm::Element* list) {
    auto* frame = new UltraGBOverlayFrame("", "");
    frame->setContent(list);
    return frame;
}

// =============================================================================
// make_slot_detail_header
//
// Builds the "Slot N ◆ GameName" category-header string used by
// SlotActionGui and SaveDataSlotActionGui.
// =============================================================================
[[gnu::noinline]]
static std::string make_slot_detail_header(int slot, const std::string& name) {
    char buf[128];
    snprintf(buf, sizeof(buf), "Slot %d %s %s",
             slot, ult::DIVIDER_SYMBOL.c_str(), name.c_str());
    return buf;
}

// =============================================================================
// combo_pressed
//
// True when the Ultrahand launch combo is down this frame.
// Called from GBOverlayGui::handleInput and GBWindowedGui::handleInput.
// =============================================================================
static bool __attribute__((optimize("O3"))) combo_pressed(u64 keysDown, u64 keysHeld) {
    return (keysDown & tsl::cfg::launchCombo) &&
           (((keysDown | keysHeld) & tsl::cfg::launchCombo) == tsl::cfg::launchCombo);
}

// =============================================================================
// show_notify
//
// Post a notification banner.  Replaces the inline ult::NOTIFY_HEADER +
// literal construction duplicated at every call site.
// =============================================================================
static void show_notify(const char* msg) {
    if (tsl::notification)
        tsl::notification->showNow(ult::NOTIFY_HEADER + msg);
}

// =============================================================================
// audio_exit_if_enabled
//
// Exit UI audio when sound effects are enabled.
// Called at every ROM-launch entry point (3 sites in main.cpp via launch_game,
// plus the createUI() of both GBOverlayGui and GBWindowedGui).
// =============================================================================
static inline void audio_exit_if_enabled() {
    if (ult::useSoundEffects && !ult::limitedMemory) ult::Audio::exit();
}

// =============================================================================
// process_zr_fast_forward
//
// ZR double-click-hold state machine shared by GBOverlayGui and GBWindowedGui.
// Caller pre-computes zr_down/zr_held (applying any pass-through guard first).
// =============================================================================
[[gnu::noinline]]
static void __attribute__((optimize("O3"))) process_zr_fast_forward(bool zr_down, bool zr_held,
                                     bool& zr_first_seen, uint32_t& zr_first_frame) {
    static constexpr uint32_t kWindow = 20;  // ~333 ms at 60 fps
    if (zr_down) {
        if (zr_first_seen && (g_frame_count - zr_first_frame) <= kWindow) {
            if (!g_fast_forward) { g_fast_forward = true; gb_audio_pause(); }
            zr_first_seen = false;
        } else {
            zr_first_seen  = true;
            zr_first_frame = g_frame_count;
        }
    }
    if (zr_first_seen && (g_frame_count - zr_first_frame) > kWindow)
        zr_first_seen = false;
    if (g_fast_forward && !zr_held) {
        g_fast_forward     = false;
        g_gb_frame_next_ns = 0;   // re-anchor so no catch-up burst
        gb_audio_ff_resume();     // resync GBAPU state before next real audio frame
    }
}

// =============================================================================
// ZLPassThroughState + process_zl_pass_through
//
// Shared ZL double-click-hold state machine used by both GBWindowedGui and
// GBOverlayGui to toggle foreground/background HID ownership.
//
// Gesture: double-tap ZL (second tap within kWindow frames) then hold for
// kHold frames.  Releases early → no toggle.  On commit:
//   • st.pass_through is flipped
//   • tsl::hlp::requestForeground(!st.pass_through) routes HID accordingly
//   • g_focus_flash / g_focus_flash_red are set so both draw() paths can
//     render the coloured border without any extra coupling.
//
// Overlay note: the calling guard (zl_down / zl_held derivation) intentionally
// omits the d-pad key exclusion used in windowed mode — the overlay skin shows
// a virtual d-pad on the right side of the screen so the physical d-pad is
// already routed to GB; ZL never overlaps with game input there.
// =============================================================================
struct ZLPassThroughState {
    bool     first_seen   = false;
    uint32_t first_frame  = 0;
    bool     second_seen  = false;
    uint32_t second_frame = 0;
    bool     pass_through = false;  // true = foreground released; background owns HID
};

[[gnu::noinline]]
static void __attribute__((optimize("O3"))) process_zl_pass_through(bool zl_down, bool zl_held,
                                    ZLPassThroughState& st) {
    static constexpr uint32_t kWindow = 20;  // ~333 ms at 60 fps
    static constexpr uint32_t kHold   = 18;  // ~300 ms at 60 fps

    if (zl_down) {
        if (st.first_seen && (g_frame_count - st.first_frame) <= kWindow) {
            // Second tap within window — enter hold phase.
            st.first_seen  = false;
            st.second_seen = true;
            st.second_frame = g_frame_count;
        } else {
            // First tap — arm; cancel any stale hold phase.
            st.first_seen  = true;
            st.first_frame = g_frame_count;
            st.second_seen = false;
        }
    }

    if (st.second_seen) {
        if (!zl_held) {
            // Released before threshold — cancel without toggling.
            st.second_seen = false;
        } else if ((g_frame_count - st.second_frame) >= kHold) {
            // Held long enough — commit the toggle.
            st.pass_through   = !st.pass_through;
            tsl::hlp::requestForeground(!st.pass_through);
            st.second_seen    = false;
            g_focus_flash_red = st.pass_through;   // true=red (lost), false=green (gained)
            g_focus_flash     = 45;
        }
    }

    // Expire a stale first-press that was never followed up.
    if (st.first_seen && (g_frame_count - st.first_frame) > kWindow)
        st.first_seen = false;
}

// =============================================================================
// process_home_foreground_release
//
// Companion to process_zl_pass_through.  Call once per update() frame in both
// GBOverlayGui and GBWindowedGui, right after process_zl_pass_through.
//
// Consumes tsl::homeButtonPressedInGame (set by Tesla's background poller when
// the home button fires during a game session where normal hiding is suppressed).
// If foreground is currently held (!st.pass_through), releases it and triggers
// the same red-flash border as the ZL double-click-hold release path, so the
// user gets clear visual feedback that HID has been handed back to the system.
// No-op when foreground is already released or the flag is not set.
// =============================================================================
[[gnu::noinline]]
static void process_home_foreground_release(ZLPassThroughState& st) {
    if (!tsl::homeButtonPressedInGame.exchange(false, std::memory_order_acq_rel))
        return;
    if (!st.pass_through) {           // currently holding foreground — release it
        st.pass_through = true;
        tsl::hlp::requestForeground(false);
        g_focus_flash_red = true;
        g_focus_flash     = 45;
    }
}

// =============================================================================
// run_once_setup
//
// One-time per-session setup called from update() on the first tick of both
// GBOverlayGui and GBWindowedGui.  Guards itself — subsequent calls are no-ops.
// =============================================================================
[[gnu::noinline]]
static void run_once_setup(bool& runOnce, bool& restoreHapticState) {
    if (!runOnce) return;
    if ((g_button_haptics || g_touch_haptics) && !ult::useHapticFeedback) {
        ult::useHapticFeedback = true;
        restoreHapticState = true;
    }
    if (g_self_path[0])
        returnOverlayPath = std::string(g_self_path);
    
    runOnce = false;
}

// =============================================================================
// consume_pending_rom
//
// Consume g_pending_rom_path: clear the font cache, copy the path, attempt
// load.  Sets load_failed on failure.  No-op when the path is already empty.
// Called from createUI() of both GBOverlayGui and GBWindowedGui.
// =============================================================================
[[gnu::noinline]]
static void consume_pending_rom(bool& load_failed) {
    if (g_pending_rom_path[0] == '\0') return;
    tsl::gfx::FontManager::clearCache();
    char path[PATH_BUFFER_SIZE];
    strncpy(path, g_pending_rom_path, sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    g_pending_rom_path[0] = '\0';
    if (!gb_load_rom(path))
        load_failed = true;
}

// =============================================================================
// restore_haptic_if_needed
//
// If haptic feedback was temporarily enabled for in-game use, disable it
// again and clear the flag.  Called from:
//   • ~GBOverlayGui()             — destructor path (user closed overlay)
//   • GBWindowedGui::handleInput  — combo-exit path (launch combo pressed)
// run_once_setup() is the write side: it sets useHapticFeedback = true and
// restoreHapticState = true when g_button_haptics or g_touch_haptics is on
// but the global flag was off at session start.
// =============================================================================
static inline void restore_haptic_if_needed(bool& restoreHapticState) {
    if (restoreHapticState) {
        ult::useHapticFeedback = false;
        restoreHapticState     = false;
    }
}

// =============================================================================
// poll_console_docked
//
// Debounced wrapper around ult::consoleIsDocked().
//
// Rationale: consoleIsDocked() is a system service (IPC) call, not a simple
// register read.  Invoking it every display frame (~60×/s) adds unnecessary
// IPC overhead in the hot draw() path.  The Switch dock/undock transition
// takes several seconds; detecting it within ~1 second is imperceptible.
//
// Implementation: re-queries the service only when g_frame_count has advanced
// past g_dock_next_check.  g_dock_next_check starts at 0, so the very first
// call (g_frame_count == 0) always triggers a fresh query — this guarantees
// correctness for call sites in main() that run before tsl::loop() starts
// (clamp_win_scale, setup_windowed_framebuffer) as well as in-game draw()
// paths.  After each query, the threshold is pushed forward by the interval.
//
// kDockCheckInterval: 60 frames ≈ 1 second at 60 fps.
//
// Thread safety: all callers run on the single UI thread; no synchronisation
// needed.  The audio-resync path (gb_audio_request_resync) is async-safe.
// =============================================================================
// 6 frames ≈ 100 ms at 60 fps.  Small enough that no realistic dock→undock→dock
// cycle completes within one window (physical connector + OS signal takes longer),
// so the intermediate state is always sampled and resync is never missed.
// Still a 10× reduction in IPC calls vs the original every-frame polling.
static constexpr uint32_t kDockCheckInterval = 6;   // frames between service queries

static bool __attribute__((optimize("O3"))) poll_console_docked() {
    if (g_frame_count >= g_dock_next_check) {
        g_console_docked  = ult::consoleIsDocked();
        g_dock_next_check = g_frame_count + kDockCheckInterval;
    }
    return g_console_docked;
}

// =============================================================================
// poll_touch
//
// Direct HID touch poll shared by GBOverlayGui and GBWindowedGui.
// Bypasses Tesla's per-frame coordinate clamping, which corrupts tracking when
// touch.x exceeds cfg::FramebufferWidth (common in windowed / free-overlay mode).
// Returns true if at least one finger is down; writes the position to out_x/out_y.
// noinline: single copy in .text instead of one inlined copy per call site.
// =============================================================================
[[gnu::noinline]]
static bool __attribute__((optimize("O3"))) poll_touch(int& out_x, int& out_y) {
    HidTouchScreenState ts = {};
    hidGetTouchScreenStates(&ts, 1);
    if (ts.count > 0) {
        out_x = static_cast<int>(ts.touches[0].x);
        out_y = static_cast<int>(ts.touches[0].y);
        return true;
    }
    out_x = out_y = 0;
    return false;
}

// =============================================================================
// joy_sens
//
// x^8 joystick sensitivity curve shared by the joystick-reposition logic in
// GBOverlayGui and GBWindowedGui.  Fine at low deflection, accelerates sharply
// near full throw.  Return value is multiplied by a dt_factor at the call site.
// noinline: single copy in .text instead of one inlined copy per call site.
// =============================================================================
[[gnu::noinline]]
static float __attribute__((optimize("O3"))) joy_sens(float fx, float fy) {
    const float norm   = (fx*fx + fy*fy) / (32767.f * 32767.f);
    const float curve2 = norm   * norm;
    const float curve4 = curve2 * curve2;
    const float curve8 = curve4 * curve4;
    return kJoyBaseSens + (kJoyMaxSens - kJoyBaseSens) * curve8;
}

// =============================================================================
// draw_focus_flash
//
// Draws the ZL pass-through focus-flash border that appears in both
// GBOverlayElement and GBWindowedElement.  Shared logic: same 4-px border
// width, same alpha-fade formula, same color choice, same decrement.
//
// Parameters are the content region the border should surround:
//   x0, y0 — top-left corner (framebuffer pixels)
//   w, h   — width and height of the region
//
// Returns true if the flash was active this frame (the overlay uses this to
// decide whether to re-zero rounded corners after drawing the border).
//
// noinline: with two call sites in separate virtual functions the compiler
// would otherwise inline the body twice — one copy per element class.
// =============================================================================
[[gnu::noinline]]
static bool __attribute__((optimize("O3"))) draw_focus_flash(tsl::gfx::Renderer* renderer,
                              s32 x0, s32 y0, s32 w, s32 h,
                              bool rounded = false) {
    if (g_focus_flash <= 0) return false;
    const u8 al = g_focus_flash > 15
        ? static_cast<u8>(0xF)
        : static_cast<u8>(g_focus_flash * 0xF / 15);
    const tsl::Color fc = g_focus_flash_red
        ? tsl::Color{0xF, 0x0, 0x0, al}
        : tsl::Color{0x0, 0xF, 0x0, al};
    static constexpr int B = 4;

    if (!rounded) {
        // Square border — windowed mode and fixed overlay mode.
        renderer->drawRect(x0,         y0,          w, B,        fc);
        renderer->drawRect(x0,         y0 + h - B,  w, B,        fc);
        renderer->drawRect(x0,         y0 + B,      B, h - B*2,  fc);
        renderer->drawRect(x0 + w - B, y0 + B,      B, h - B*2,  fc);
    } else {
        // Rounded border — free overlay mode.
        // Matches the overlay player shape exactly: top corners R=10, bottom corners R=20,
        // same geometry driven by kOvlR10Arc / kOvlR20Arc in gb_renderer.h.
        //
        // Straight edges are B px thick, shortened by the respective arc radius on each
        // side so they don't butt squarely against the transparent corner cutouts.
        // Corner arcs are scan-filled directly into the framebuffer via
        // draw_thick_arc_corners_fb — see that function for a full explanation of
        // why the old B-inset drawRect loop was replaced.
        static constexpr int R_T = 10, R_B = 20;
        // Straight edges
        renderer->drawRect(x0 + R_T, y0,      w - 2*R_T, B,          fc);  // top
        renderer->drawRect(x0 + R_B, y0+h-B,  w - 2*R_B, B,          fc);  // bottom
        renderer->drawRect(x0,       y0+R_T,  B,          h-R_T-R_B, fc);  // left
        renderer->drawRect(x0+w-B,   y0+R_T,  B,          h-R_T-R_B, fc);  // right
        // Corner arcs — scan-filled directly into the framebuffer.
        // draw_thick_arc_corners_fb computes the union x-span of all B inset
        // passes per absolute row and writes it in one shot, eliminating the
        // diagonal pixel gaps the old BORD-inset drawRect loop produced.
        //
        // Every arc pixel is written via blend_one, which reads the existing
        // framebuffer pixel and applies the same (src*(15-a) + color*a)>>4
        // alpha-blend as Tesla's drawRect / blendPixelDirect.  This means the
        // corners and the straight edges fade identically: both blend back
        // toward whatever is already in the framebuffer (the wallpaper) as
        // g_focus_flash counts down toward zero, rather than the corners
        // fading toward black as the previous direct-write path did.
        //
        // Pack the colour with fc.a = al directly — the blending in
        // draw_thick_arc_corners_fb produces the correct fade without any
        // RGB pre-scaling.
        const uint16_t packed_fc = static_cast<uint16_t>(
            static_cast<unsigned>(fc.r)        |
           (static_cast<unsigned>(fc.g) <<  4) |
           (static_cast<unsigned>(fc.b) <<  8) |
           (static_cast<unsigned>(al)   << 12));
        auto* fb16 = static_cast<uint16_t*>(renderer->getCurrentFramebuffer());
        draw_thick_arc_corners_fb(fb16, x0, x0+w, y0, y0+h-1, B, packed_fc);
    }
    --g_focus_flash;
    return true;
}

// =============================================================================
// Config persist helpers
//
// One-liner wrappers around setIniFileValue.  Moved from main.cpp so any GUI
// that includes gb_utils.hpp can persist settings without needing to reach
// back into main.cpp.
// =============================================================================

// Persist the basename of the just-launched ROM so the selector can jump to it on re-entry.
static void save_last_rom(const char* fullPath) {
    const char* sl = strrchr(fullPath, '/');
    const char* base = sl ? sl + 1 : fullPath;
    strncpy(g_last_rom_path, base, sizeof(g_last_rom_path) - 1);
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyLastRom, base, "");
}

// Persist the LCD grid toggle to config.ini.
static void save_lcd_grid() {
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyLcdGrid,
                         g_lcd_grid ? "1" : "0", "");
}

// Persist the windowed mode toggle to config.ini.
static void save_windowed_mode() {
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWindowed,
                         g_windowed_mode ? "1" : "0", "");
}

// Persist the dragged window position (VI coordinates) to config.ini.
static void save_win_pos() {
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinPosX,
                         ult::to_string(g_win_pos_x), "");
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinPosY,
                         ult::to_string(g_win_pos_y), "");
}

// Persist the windowed output resolution mode to config.ini.
static void save_win_output() {
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinOutput,
                         g_win_1080 ? "1080" : "720", "");
}

// Persist the windowed scale (1–6) to config.ini.
static void save_win_scale() {
    const char s[2] = { static_cast<char>('0' + g_win_scale), '\0' };
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinScale, s, "");
}

// Persist the free-overlay position toggle to config.ini.
static void save_ovl_free_mode() {
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyOvlFreeMode,
                         g_overlay_free_mode ? "1" : "0", "");
}

// Persist the free-overlay layer position (VI coordinates) to config.ini.
static void save_ovl_free_pos() {
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyOvlFreePosX,
                         ult::to_string(g_ovl_free_pos_x), "");
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyOvlFreePosY,
                         ult::to_string(g_ovl_free_pos_y), "");
}

// =============================================================================
// ROM validation helpers
// =============================================================================

// Shared fopen/fseek/ftell/fclose sequence — avoids duplicating the 4-line
// block in both rom_is_playable() and rom_playability_message().
// [[gnu::noinline]] — called from 8+ sites; one copy is always smaller.
[[gnu::noinline]]
static size_t get_rom_size(const char* path) {
    struct stat st;
    return (stat(path, &st) == 0) ? static_cast<size_t>(st.st_size) : 0;
}

// Returns true when the ROM at |path| can be loaded on the current memory tier.
// Mirrors the rejection conditions in gb_load_rom() so the selector can dim
// unplayable entries before the user tries to launch them.
// [[gnu::noinline]] — called per-ROM in RomSelectorGui list + other UI sites.
[[gnu::noinline]]
static bool rom_is_playable(const char* path) {
    if (ult::expandedMemory) return true;
    const size_t sz = get_rom_size(path);
    if (!sz) return false;
    if (ult::limitedMemory          && sz >= kROM_2MB) return false;
    //if (!ult::expandedMemory        && sz >= kROM_4MB) return false;
    //if (!ult::furtherExpandedMemory && sz >= kROM_6MB) return false;
    return true;
}

// Returns the actionable error string if the ROM at path cannot be loaded on
// the current heap tier, or nullptr if it is playable.
// [[gnu::noinline]] — called 4+ times (loadInitialGui, gb_load_rom, ...).
[[gnu::noinline]]
static const char* rom_playability_message(const char* path) {
    if (ult::expandedMemory) return nullptr;
    const size_t sz = get_rom_size(path);
    if (!sz) return nullptr;
    if ( ult::limitedMemory          && sz >= kROM_2MB && sz < kROM_4MB) return REQUIRES_AT_LEAST_6MB;
    //if (!ult::expandedMemory         && sz >= kROM_4MB && sz < kROM_6MB) return REQUIRES_AT_LEAST_8MB;
    //if (!ult::furtherExpandedMemory  && sz >= kROM_6MB)                  return REQUIRES_AT_LEAST_10MB;
    return nullptr;
}

// =============================================================================
// Quick Combo helpers
// =============================================================================

// Return the bare filename of this .ovl (e.g. "ultragb.ovl") from g_self_path.
static const char* ovl_filename() {
    if (!g_self_path[0]) return "";
    const char* sl = strrchr(g_self_path, '/');
    return sl ? sl + 1 : g_self_path;
}

// Default combos available in the Quick Combo picker.
static constexpr const char* const g_defaultCombos[] = {
    "ZL+ZR+DDOWN",  "ZL+ZR+DRIGHT", "ZL+ZR+DUP",    "ZL+ZR+DLEFT",
    "L+R+DDOWN",    "L+R+DRIGHT",   "L+R+DUP",       "L+R+DLEFT",
    "L+DDOWN",      "R+DDOWN",
    "ZL+ZR+PLUS",   "L+R+PLUS",     "ZL+ZR+MINUS",   "L+R+MINUS",
    "ZL+MINUS",     "ZR+MINUS",     "ZL+PLUS",        "ZR+PLUS",    "MINUS+PLUS",
    "LS+RS",        "L+DDOWN+RS",   "L+R+LS",         "L+R+RS",
    "ZL+ZR+LS",     "ZL+ZR+RS",     "ZL+ZR+L",        "ZL+ZR+R",    "ZL+ZR+LS+RS"
};

// Remove keyCombo from every overlay's key_combo AND mode_combos in
// overlays.ini, and from every package's key_combo in packages.ini.
[[gnu::noinline]]
static void remove_quick_combo_from_others(const std::string& keyCombo,
                                           bool skipOwnModeCombos = false) {
    if (keyCombo.empty()) return;
    if (!ult::isFile(ult::OVERLAYS_INI_FILEPATH)) return;

    const char* own = ovl_filename();
    const bool haveOwn = own && own[0];
    auto data = ult::getParsedDataFromIniFile(ult::OVERLAYS_INI_FILEPATH);
    bool dirty = false;

    for (auto& [name, section] : data) {
        const bool isSelf = haveOwn && (name == own);

        auto kcIt = section.find("key_combo");
        if (kcIt != section.end() && !kcIt->second.empty() &&
            tsl::hlp::comboStringToKeys(kcIt->second) ==
            tsl::hlp::comboStringToKeys(keyCombo)) {
            kcIt->second = "";
            dirty = true;
        }

        if (isSelf && skipOwnModeCombos) continue;

        auto mcIt = section.find("mode_combos");
        if (mcIt != section.end() && !mcIt->second.empty()) {
            auto comboList = ult::splitIniList(mcIt->second);
            bool changed = false;
            for (auto& c : comboList) {
                if (!c.empty() &&
                    tsl::hlp::comboStringToKeys(c) ==
                    tsl::hlp::comboStringToKeys(keyCombo)) {
                    c = "";
                    changed = true;
                }
            }
            if (changed) {
                mcIt->second = "(" + ult::joinIniList(comboList) + ")";
                dirty = true;
            }
        }
    }

    if (dirty)
        ult::saveIniFileData(ult::OVERLAYS_INI_FILEPATH, data);

    if (!ult::isFile(ult::PACKAGES_INI_FILEPATH)) return;
    auto pkgData = ult::getParsedDataFromIniFile(ult::PACKAGES_INI_FILEPATH);
    bool pkgDirty = false;
    for (auto& [name, section] : pkgData) {
        auto kcIt = section.find("key_combo");
        if (kcIt != section.end() && !kcIt->second.empty() &&
            tsl::hlp::comboStringToKeys(kcIt->second) ==
            tsl::hlp::comboStringToKeys(keyCombo)) {
            kcIt->second = "";
            pkgDirty = true;
        }
    }
    if (pkgDirty)
        ult::saveIniFileData(ult::PACKAGES_INI_FILEPATH, pkgData);
}

// Register "-quicklaunch" as our single mode in overlays.ini and read the
// stored combo into g_quick_combo.  Called from Overlay::initServices().
static void register_quick_launch_mode() {
    const char* filename = ovl_filename();
    if (!filename || !filename[0]) return;
    if (!ult::isFile(ult::OVERLAYS_INI_FILEPATH)) return;

    static constexpr const char* kExpectedArgs   = "(-quicklaunch)";
    static constexpr const char* kExpectedLabels = "(Quick Launch)";

    ult::setIniFileValue(ult::OVERLAYS_INI_FILEPATH, filename, "mode_args",   kExpectedArgs);
    ult::setIniFileValue(ult::OVERLAYS_INI_FILEPATH, filename, "mode_labels", kExpectedLabels);

    g_quick_combo[0] = '\0';

    const std::string mc = ult::parseValueFromIniSection(
        ult::OVERLAYS_INI_FILEPATH, filename, "mode_combos"
    );

    const auto comboList = ult::splitIniList(mc);
    if (!comboList.empty() && !comboList[0].empty()) {
        strncpy(g_quick_combo, comboList[0].c_str(), sizeof(g_quick_combo) - 1);
        g_quick_combo[sizeof(g_quick_combo) - 1] = '\0';
    }

    if (g_quick_combo[0])
        remove_quick_combo_from_others(g_quick_combo, /*skipOwnModeCombos=*/true);
}

// =============================================================================
// Per-game config helpers
// Each ROM gets its own ini at: CONFIGURE_DIR/<filename>.ini
// =============================================================================

// Peek at ROM header byte 0x143 to determine CGB support without loading the ROM.
static uint8_t rom_cgb_flag(const char* romPath) {
    FILE* f = fopen(romPath, "rb");
    if (!f) return 0;
    uint8_t flag = 0;
    if (fseek(f, 0x143, SEEK_SET) == 0)
        fread(&flag, 1, 1, f);
    fclose(f);
    return flag;
}

// Returns true for any ROM that declares CGB support (0x80 or 0xC0).
static bool rom_has_cgb_flag(const char* romPath) {
    return (rom_cgb_flag(romPath) & 0x80) != 0;
}

static void build_game_config_path(const char* romPath, char* out, size_t outSz) {
    const char* sl = strrchr(romPath, '/');
    const char* base = sl ? sl + 1 : romPath;
    snprintf(out, outSz, "%s%s.ini", CONFIGURE_DIR, base);
}

static std::string load_game_cfg_str(const char* romPath, const char* key) {
    char cfgPath[PATH_BUFFER_SIZE];
    build_game_config_path(romPath, cfgPath, sizeof(cfgPath));
    return ult::parseValueFromIniSection(cfgPath, kConfigSection, key);
}

static void save_game_cfg_str(const char* romPath, const char* key, const char* value) {
    char cfgPath[PATH_BUFFER_SIZE];
    build_game_config_path(romPath, cfgPath, sizeof(cfgPath));
    ult::createDirectory(CONFIGURE_DIR);
    ult::setIniFileValue(cfgPath, kConfigSection, key, value, "");
}

static const char* palette_mode_to_str(PaletteMode m) {
    switch (m) {
        case PaletteMode::SGB:    return "SGB";
        case PaletteMode::DMG:    return "DMG";
        case PaletteMode::NATIVE: return "Native";
        default:                  return "GBC";
    }
}

static PaletteMode str_to_palette_mode(const std::string& s) {
    if (s == "DMG")    return PaletteMode::DMG;
    if (s == "Native") return PaletteMode::NATIVE;
    return PaletteMode::GBC;
}

static PaletteMode load_game_palette_mode(const char* romPath) {
    return str_to_palette_mode(load_game_cfg_str(romPath, "palette_mode"));
}

static void save_game_palette_mode(const char* romPath, PaletteMode m) {
    save_game_cfg_str(romPath, "palette_mode", palette_mode_to_str(m));
}

// Cycles through the three UI-exposed modes (SGB intentionally excluded).
static PaletteMode next_palette_mode(PaletteMode current) {
    switch (current) {
        case PaletteMode::GBC:    return PaletteMode::DMG;
        case PaletteMode::DMG:    return PaletteMode::NATIVE;
        case PaletteMode::NATIVE: return PaletteMode::GBC;
        default:                  return PaletteMode::GBC;
    }
}

// Short user-facing label for the three exposed modes.
static const char* palette_mode_label(PaletteMode m) {
    switch (m) {
        case PaletteMode::DMG:    return "DMG";
        case PaletteMode::NATIVE: return "Native";
        default:                  return "GBC";
    }
}

static bool load_game_lcd_ghosting(const char* romPath) {
    return load_game_cfg_str(romPath, "lcd_ghosting") == "1";
}

static void save_game_lcd_ghosting(const char* romPath, bool enabled) {
    save_game_cfg_str(romPath, "lcd_ghosting", enabled ? "1" : "0");
}

static bool load_game_no_sprite_limit(const char* romPath) {
    return load_game_cfg_str(romPath, "no_sprite_limit") != "0";
}

static void save_game_no_sprite_limit(const char* romPath, bool enabled) {
    save_game_cfg_str(romPath, "no_sprite_limit", enabled ? "1" : "0");
}

// ── Per-game audio balance ─────────────────────────────────────────────────
// Stored as a plain signed integer string ("-50"…"50") in the per-ROM .ini.
// Missing / empty key → 0 (neutral, no gain change).
// Range is clamped on load so values written by older builds are safe.
static int16_t load_game_audio_balance(const char* romPath) {
    const std::string s = load_game_cfg_str(romPath, "audio_balance");
    if (s.empty()) return 0;
    return static_cast<int16_t>(std::clamp(std::stoi(s), -200, 200));
}
static void save_game_audio_balance(const char* romPath, int16_t balance) {
    save_game_cfg_str(romPath, "audio_balance",
                      std::to_string(static_cast<int>(balance)).c_str());
}

// =============================================================================
// Save helpers
// =============================================================================

// Derives a per-ROM path of the form <dir><basename_no_ext><ext>.
// [[gnu::noinline]] — 9+ call sites (save_state, load_state, backup/restore/save/load
// slot helpers, build_game_slot_dir, SlotActionGui, gb_load_rom…); one copy in .text.
[[gnu::noinline]]
static void build_rom_data_path(const char* romPath, char* out, size_t outSz,
                                const char* dir, const char* ext) {
    const char* slash = strrchr(romPath, '/');
    const char* base  = slash ? slash + 1 : romPath;
    char bn[PATH_BUFFER_SIZE] = {};
    strncpy(bn, base, sizeof(bn) - 1);
    char* dot = strrchr(bn, '.');
    if (dot) *dot = '\0';
    snprintf(out, outSz, "%s%s%s", dir, bn, ext);
}

static void load_save(GBState& s) {
    if (!s.cartRam || !s.cartRamSz) return;
    FILE* f = fopen(s.savePath, "rb");
    if (!f) return;
    fread(s.cartRam, 1, s.cartRamSz, f);
    fclose(f);
}

static void write_save(GBState& s) {
    if (!s.cartRam || !s.cartRamSz) return;
    ult::createDirectory(g_save_dir);
    FILE* f = fopen(s.savePath, "wb");
    if (!f) return;
    fwrite(s.cartRam, 1, s.cartRamSz, f);
    fclose(f);
}

// =============================================================================
// State save / load  (cross-session resume)
//
// Format (all fields little-endian):
//   [0]  uint32  magic        = 0x47425354 ('GBST')
//   [4]  uint32  version      = 4
//   [8]  uint32  cart_ram_sz
//   [12] uint32  fb_bytes     = GB_W*GB_H*2
//   [16] uint32  fb_flags     0=RGB565/RGB555  1=RGBA4444 prepacked
//   [20] gb_s    core         (function pointers re-patched on load)
//   [+]  uint8[] cart_ram
//   [+]  uint16[] framebuffer
//   [+]  GBAPU   apu_snapshot
// =============================================================================
static constexpr uint32_t STATE_MAGIC   = 0x47425354u; // 'GBST'
static constexpr uint32_t STATE_VERSION = 4u;

static void save_state(GBState& s) {
    if (!s.romPath[0]) return;

    ult::createDirectory(STATE_DIR);

    char statePath[PATH_BUFFER_SIZE] = {};
    build_rom_data_path(s.romPath, statePath, sizeof(statePath), STATE_DIR, ".state");

    FILE* f = fopen(statePath, "wb");
    if (!f) return;

    const uint32_t magic   = STATE_MAGIC;
    const uint32_t version = STATE_VERSION;
    const uint32_t ramSz   = static_cast<uint32_t>(s.cartRamSz);
    const uint32_t fbBytes = static_cast<uint32_t>(GB_W * GB_H * sizeof(uint16_t));
    const uint32_t fbFlags = g_fb_is_prepacked ? 1u : 0u;

    fwrite(&magic,   sizeof(magic),   1, f);
    fwrite(&version, sizeof(version), 1, f);
    fwrite(&ramSz,   sizeof(ramSz),   1, f);
    fwrite(&fbBytes, sizeof(fbBytes), 1, f);
    fwrite(&fbFlags, sizeof(fbFlags), 1, f);
    fwrite(s.gb,     sizeof(*s.gb),   1, f);
    if (s.cartRam && s.cartRamSz)
        fwrite(s.cartRam, 1, s.cartRamSz, f);
    fwrite(g_gb_fb, 1, fbBytes, f);

    GBAPU apu_snap{};
    gb_audio_save_state(&apu_snap);
    fwrite(&apu_snap, sizeof(apu_snap), 1, f);

    fclose(f);
}

// Returns true if a valid state was loaded and the emulator is ready to run.
// *apu_restored is set true when the GBAPU snapshot was successfully restored.
static bool load_state(GBState& s, bool* apu_restored = nullptr) {
    if (apu_restored) *apu_restored = false;
    if (!s.romPath[0]) return false;

    char statePath[PATH_BUFFER_SIZE] = {};
    build_rom_data_path(s.romPath, statePath, sizeof(statePath), STATE_DIR, ".state");

    FILE* f = fopen(statePath, "rb");
    if (!f) return false;

    uint32_t magic = 0, version = 0, ramSz = 0, fbBytes = 0;
    if (fread(&magic,   sizeof(magic),   1, f) != 1 || magic != STATE_MAGIC) { fclose(f); return false; }
    if (fread(&version, sizeof(version), 1, f) != 1 || version != STATE_VERSION) { fclose(f); return false; }
    if (fread(&ramSz,   sizeof(ramSz),   1, f) != 1) { fclose(f); return false; }
    if (fread(&fbBytes, sizeof(fbBytes), 1, f) != 1) { fclose(f); return false; }

    uint32_t fbFlags = 0u;
    if (fread(&fbFlags, sizeof(fbFlags), 1, f) != 1) { fclose(f); return false; }

    if (ramSz   != static_cast<uint32_t>(s.cartRamSz))  { fclose(f); return false; }
    if (fbBytes != static_cast<uint32_t>(GB_W * GB_H * sizeof(uint16_t))) { fclose(f); return false; }

    if (fread(s.gb, sizeof(*s.gb), 1, f) != 1) { fclose(f); return false; }

    s.gb->gb_rom_read       = gb_rom_read;
    s.gb->gb_rom_read_16bit = gb_rom_read16;
    s.gb->gb_rom_read_32bit = gb_rom_read32;
    s.gb->gb_cart_ram_read  = gb_cart_ram_read;
    s.gb->gb_cart_ram_write = gb_cart_ram_write;
    s.gb->gb_error          = gb_error;
    s.gb->gb_serial_tx      = nullptr;
    s.gb->gb_serial_rx      = nullptr;
    s.gb->gb_bootrom_read   = nullptr;
    s.gb->direct.priv       = nullptr;
    gb_init_lcd(s.gb, gb_lcd_draw_line);

    if (s.cartRam && s.cartRamSz) {
        if (fread(s.cartRam, 1, s.cartRamSz, f) != s.cartRamSz) { fclose(f); return false; }
    }

    if (fread(g_gb_fb, 1, fbBytes, f) != fbBytes) { fclose(f); return false; }

    if (g_fb_is_prepacked && fbFlags == 0u) {
        for (int i = 0; i < GB_W * GB_H; ++i)
            g_gb_fb[i] = gb_pack_rgb555(g_gb_fb[i]);
    } else if (!g_fb_is_prepacked && fbFlags == 1u) {
        memset(g_gb_fb, 0, GB_W * GB_H * sizeof(uint16_t));
    }

    GBAPU apu_snap{};
    if (fread(&apu_snap, sizeof(apu_snap), 1, f) == 1) {
        gb_audio_restore_state(&apu_snap);
        if (apu_restored) *apu_restored = true;
    }

    fclose(f);
    return true;
}

// =============================================================================
// User save-state and save-data slot helpers  (10 named slots per game)
// =============================================================================

// Per-game slot directory: <baseDir><gamename_no_ext>/.
static void build_game_slot_dir(const char* romPath, char* out, size_t outSz,
                                const char* baseDir) {
    build_rom_data_path(romPath, out, outSz, baseDir, "/");
}

// Shared slot-file path builder.
// [[gnu::noinline]] — four thin wrappers (build_user_slot_path, _ts_path,
// build_save_backup_slot_path, _ts_path) each get inlined at their call sites,
// exposing this body at ~8 compile-time sites; noinline keeps one copy.
[[gnu::noinline]]
static void build_slot_file_path(const char* romPath, int slot, char* out, size_t outSz,
                                  const char* baseDir, const char* ext) {
    char dir[PATH_BUFFER_SIZE] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), baseDir);
    snprintf(out, outSz, "%sslot_%d%s", dir, slot, ext);
}

static void build_user_slot_path(const char* romPath, int slot, char* out, size_t outSz) {
    build_slot_file_path(romPath, slot, out, outSz, STATE_BASE_DIR, ".state");
}

static void build_user_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz) {
    build_slot_file_path(romPath, slot, out, outSz, STATE_BASE_DIR, ".ts");
}

// Write current wall-clock time to an arbitrary .ts file path.
static void write_timestamp_to(const char* tsPath) {
    FILE* f = fopen(tsPath, "w");
    if (!f) return;
    time_t t = time(nullptr);
    struct tm ti{};
    localtime_r(&t, &ti);
    char buf[64] = {};
    size_t len = strftime(buf, sizeof(buf), "%Y-%m-%d", &ti);
    if (len < sizeof(buf))
        len += snprintf(buf + len, sizeof(buf) - len, "%s", ult::DIVIDER_SYMBOL.c_str());
    if (len < sizeof(buf))
        strftime(buf + len, sizeof(buf) - len, "%H:%M:%S", &ti);
    fwrite(buf, 1, strlen(buf), f);
    fclose(f);
}

// Read a .ts file into out; fills ult::OPTION_SYMBOL if absent or empty.
static void read_timestamp_from(const char* tsPath, char* out, size_t outSz) {
    FILE* f = fopen(tsPath, "r");
    if (!f) { strncpy(out, ult::OPTION_SYMBOL.c_str(), outSz - 1); out[outSz - 1] = '\0'; return; }
    size_t n = fread(out, 1, outSz - 1, f);
    out[n] = '\0';
    fclose(f);
    if (!out[0]) strncpy(out, ult::OPTION_SYMBOL.c_str(), outSz - 1);
}

static void write_slot_timestamp(const char* romPath, int slot) {
    char tsPath[PATH_BUFFER_SIZE] = {};
    build_user_slot_ts_path(romPath, slot, tsPath, sizeof(tsPath));
    write_timestamp_to(tsPath);
}

static void read_slot_timestamp(const char* romPath, int slot, char* out, size_t outSz) {
    char tsPath[PATH_BUFFER_SIZE] = {};
    build_user_slot_ts_path(romPath, slot, tsPath, sizeof(tsPath));
    read_timestamp_from(tsPath, out, outSz);
}

// [[gnu::noinline]] — called 13+ times (10-item loop in SaveSlotsGui +
// SlotActionGui header + swap_to_slots); one snprintf+std::string per call,
// one copy in .text beats 13 inline copies.
[[gnu::noinline]]
static std::string make_slot_label(int slot) {
    char buf[16];
    snprintf(buf, sizeof(buf), "Slot %d", slot);
    return buf;
}

static void build_save_backup_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz);

// [[gnu::noinline]] — body loops 10 slots (stat calls); inlined into each of
// the two wrapper functions (newest_state_slot_label, newest_save_backup_slot_label)
// would duplicate the loop body twice.
[[gnu::noinline]]
static std::string newest_slot_label(const char* romPath,
    void(*buildTs)(const char*, int, char*, size_t)) {
    time_t best = -1;
    int    best_slot = -1;
    for (int i = 0; i < 10; ++i) {
        char tsPath[PATH_BUFFER_SIZE] = {};
        buildTs(romPath, i, tsPath, sizeof(tsPath));
        struct stat st{};
        if (::stat(tsPath, &st) == 0 && st.st_mtime > best) {
            best = st.st_mtime;
            best_slot = i;
        }
    }
    return best_slot >= 0 ? make_slot_label(best_slot) : std::string{};
}

static std::string newest_state_slot_label(const char* romPath) {
    return newest_slot_label(romPath, build_user_slot_ts_path);
}

static std::string newest_save_backup_slot_label(const char* romPath) {
    return newest_slot_label(romPath, build_save_backup_slot_ts_path);
}

static void build_save_backup_slot_path(const char* romPath, int slot, char* out, size_t outSz) {
    build_slot_file_path(romPath, slot, out, outSz, SAVE_BASE_DIR, ".sav");
}

static void build_save_backup_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz) {
    build_slot_file_path(romPath, slot, out, outSz, SAVE_BASE_DIR, ".ts");
}

static void write_save_backup_timestamp(const char* romPath, int slot) {
    char tsPath[PATH_BUFFER_SIZE] = {};
    build_save_backup_slot_ts_path(romPath, slot, tsPath, sizeof(tsPath));
    write_timestamp_to(tsPath);
}

static void read_save_backup_timestamp(const char* romPath, int slot, char* out, size_t outSz) {
    char tsPath[PATH_BUFFER_SIZE] = {};
    build_save_backup_slot_ts_path(romPath, slot, tsPath, sizeof(tsPath));
    read_timestamp_from(tsPath, out, outSz);
}

static inline bool file_exists(const char* p) { return ult::isFile(p); }
static inline void copy_file(const char* s, const char* d) { ult::copyFileOrDirectory(s, d); }
static inline void delete_file(const char* p) { ult::deleteFileOrDirectory(p); }

// Back up the live .sav to slot N.  Returns false if the internal .sav is absent.
static bool backup_save_data_slot(const char* romPath, int slot) {
    char internalPath[PATH_BUFFER_SIZE] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), g_save_dir, ".sav");
    if (!file_exists(internalPath)) return false;

    char dir[PATH_BUFFER_SIZE] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), SAVE_BASE_DIR);
    ult::createDirectory(dir);

    char slotPath[PATH_BUFFER_SIZE] = {};
    build_save_backup_slot_path(romPath, slot, slotPath, sizeof(slotPath));

    copy_file(internalPath, slotPath);
    if (!file_exists(slotPath)) return false;
    write_save_backup_timestamp(romPath, slot);
    return true;
}

// Restore slot N into the live .sav file.  Returns false if the slot file doesn't exist.
static bool restore_save_data_slot(const char* romPath, int slot) {
    char slotPath[PATH_BUFFER_SIZE] = {};
    build_save_backup_slot_path(romPath, slot, slotPath, sizeof(slotPath));
    if (!file_exists(slotPath)) return false;

    char internalPath[PATH_BUFFER_SIZE] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), g_save_dir, ".sav");

    copy_file(slotPath, internalPath);
    if (!file_exists(internalPath)) return false;

    if (g_gb.rom &&
        strncmp(g_gb.romPath, romPath, sizeof(g_gb.romPath)) == 0 &&
        g_gb.cartRam && g_gb.cartRamSz) {
        FILE* f = fopen(slotPath, "rb");
        if (f) {
            fread(g_gb.cartRam, 1, g_gb.cartRamSz, f);
            fclose(f);
        }
    }
    return true;
}

// Save the current internal quick-resume state to user slot N.
static bool save_user_slot(const char* romPath, int slot) {
    char internalPath[PATH_BUFFER_SIZE] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), STATE_DIR, ".state");
    if (!file_exists(internalPath)) return false;

    char dir[PATH_BUFFER_SIZE] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), STATE_BASE_DIR);
    ult::createDirectory(dir);

    char slotPath[PATH_BUFFER_SIZE] = {};
    build_user_slot_path(romPath, slot, slotPath, sizeof(slotPath));

    copy_file(internalPath, slotPath);
    if (!file_exists(slotPath)) return false;
    write_slot_timestamp(romPath, slot);
    return true;
}

// Load user slot N into the internal quick-resume state file.
static bool load_user_slot(const char* romPath, int slot) {
    char slotPath[PATH_BUFFER_SIZE] = {};
    build_user_slot_path(romPath, slot, slotPath, sizeof(slotPath));
    if (!file_exists(slotPath)) return false;

    char internalPath[PATH_BUFFER_SIZE] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), STATE_DIR, ".state");

    copy_file(slotPath, internalPath);
    return file_exists(internalPath);
}

// =============================================================================
// Launch helpers
// =============================================================================

// Attempt a windowed relaunch for romPath.  Returns false when windowed mode is off.
static bool launch_windowed_mode(const char* romPath) {
    if (!g_windowed_mode || !g_self_path[0]) return false;

    skipRumbleDoubleClick = true;

    ult::launchingOverlay.store(true, std::memory_order_release);
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWindowedRom, romPath, "");
    if (g_settings_scroll[0])
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll,
                             g_settings_scroll, "");
    tsl::setNextOverlay(g_self_path, g_directMode ? "-quicklaunch" : "-windowed");
    if (g_directMode)
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "1", "");
    tsl::Overlay::get()->close();
    return true;
}

// Relaunch this overlay with -overlay <romPath>.
static void launch_overlay_mode(const char* romPath) {
    if (!g_self_path[0]) return;

    ult::launchingOverlay.store(true, std::memory_order_release);
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyPlayerRom, romPath, "");
    if (g_settings_scroll[0])
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll,
                             g_settings_scroll, "");
    tsl::setNextOverlay(g_self_path, g_directMode ? "-quicklaunch --direct" : "-overlay");
    if (g_directMode)
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "1", "");

    skipRumbleDoubleClick = true;
    tsl::Overlay::get()->close();
}

// Relaunch with -freeoverlay <romPath> — trimmed, repositionable overlay.
// The new process detects -freeoverlay and sets DefaultFramebufferHeight=613
// before Tesla initialises, giving a 448×613 layer that floats freely.
static void launch_free_overlay_mode(const char* romPath) {
    if (!g_self_path[0]) return;

    ult::launchingOverlay.store(true, std::memory_order_release);
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyPlayerRom, romPath, "");
    if (g_settings_scroll[0])
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll,
                             g_settings_scroll, "");
    tsl::setNextOverlay(g_self_path, g_directMode ? "-quicklaunch --direct" : "-freeoverlay");
    if (g_directMode)
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "1", "");

    skipRumbleDoubleClick = true;
    tsl::Overlay::get()->close();
}

// Exit UI audio, mark last-played, then launch in windowed, free-overlay, or overlay mode.
[[gnu::noinline]]
static bool launch_game(const char* romPath) {
    audio_exit_if_enabled();
    save_last_rom(romPath);
    if (launch_windowed_mode(romPath)) return true;
    if (g_overlay_free_mode) {
        launch_free_overlay_mode(romPath);
        return true;
    }
    launch_overlay_mode(romPath);
    return true;
}
// =============================================================================
// fill_vp_corners_448
//
// Renders the R=10 corner pixels at the four outer corners of the 400×360
// letterbox viewport rectangle in one 124-pixel pass — replacing both the
// previous zero-clear and a separate wallpaper fill.
//
//  • Wallpaper active: blends the wallpaper src pixel at each corner position
//    using the same formula as draw_wallpaper_direct (fast dark-bg path or
//    full blend), making the corners visually continuous with the wallpaper
//    in the overlay margin despite being inside the viewport skip region.
//
//  • No wallpaper: fills with bg_packed (the overlay background colour read
//    from framebuffer[0] before any wallpaper was applied) so the corners
//    match the overlay margin colour instead of showing as transparent holes.
//
// bg_packed   pre-wallpaper framebuffer[0] — the fillScreen background colour
//             as a packed uint16 in tsl::Color byte layout.  Must be captured
//             before draw_wallpaper_direct is called.
//
// Called after fill_letterbox_rect and before render_gb_screen.
// s_outer_col_lut / s_outer_row_lut must already be initialised (they are,
// via init_outer_lut inside render_gb_letterbox which precedes this call).
//
// Wallpaper src rows are canonical (position-invariant):
//   top corners:    wallpaper row VP_Y + dy      = 108 + dy
//   bottom corners: wallpaper row VP_Y+VP_H-1-dy = 467 − dy
// This holds because abs_y + src_row_offset = VP_Y ± dy for all overlay
// positions (g_render_y_offset + src_row_offset = 0 in both fixed and free
// overlay mode by construction in draw_wallpaper_direct's call sites).
//
// Cost: 31 × 4 = 124 scalar stores + blend ops ≈ 1–2 µs on Cortex-A57.
// No heap allocs, no threads, no swizzle-table init — zero startup overhead.
// =============================================================================
[[gnu::noinline]]
static void fill_vp_corners_448(uint16_t* const fb16, const uint16_t bg_packed) {
    const bool has_wp = ult::expandedMemory && g_overlay_wallpaper
                        && !ult::wallpaperData.empty()
                        && !ult::refreshWallpaper.load(std::memory_order_acquire);

    if (!has_wp) {
        // No wallpaper — restore overlay background colour to the corners.
        // Two-level loop: hoist rt/rb (row FB addresses) to the outer dy pass.
        // 10 outer iterations × 2 row lookups = 20 lookups; flat loop would be 31 × 2 = 62.
        for (int dy = 0; dy < 10; ++dy) {
            const u32 rt = s_outer_row_lut[dy];
            const u32 rb = s_outer_row_lut[(int)VP_H-1-dy];
            for (int dx = 0; dx < kCornerCnts[dy]; ++dx) {
                const u32 cl = s_outer_col_lut[dx];
                const u32 cr = s_outer_col_lut[(int)VP_W-1-dx];
                fb16[rt+cl] = bg_packed;  fb16[rt+cr] = bg_packed;
                fb16[rb+cl] = bg_packed;  fb16[rb+cr] = bg_packed;
            }
        }
        return;
    }

    // Wallpaper path — match draw_wallpaper_direct's blend exactly.
    // 'fast' is a register-held constant so the branch inside blend is perfectly
    // predicted across all iterations — zero branch cost in practice.
    const u8* const src_base = ult::wallpaperData.data();
    const tsl::Color bg = *reinterpret_cast<const tsl::Color*>(&bg_packed);
    const u8 bg_r = bg.r, bg_g = bg.g, bg_b = bg.b, bg_a = bg.a;
    const u8 al   = static_cast<u8>(0xF * tsl::gfx::Renderer::s_opacity);
    const bool fast = (bg_r == 0u && bg_g == 0u && bg_b == 0u);
    static constexpr int kRS = static_cast<int>(kWP_W * 2u);

    // Blend one wallpaper pixel into dst.
    // Wallpaper fmt: byte0 = R<<4|G, byte1 = B<<4|A.
    // Output fmt:    byte0 = G<<4|R, byte1 = bg_a<<4|B.
    const auto blend = [&](const u8* p, uint16_t* dst) {
        const u8 a = std::min<u8>(p[1] & 0x0Fu, al);
        u8 r, g, b;
        if (fast) {
            r = static_cast<u8>(static_cast<u8>(p[0] >> 4)    * a >> 4);
            g = static_cast<u8>(static_cast<u8>(p[0] & 0x0Fu) * a >> 4);
            b = static_cast<u8>(static_cast<u8>(p[1] >> 4)    * a >> 4);
        } else {
            const u8 inv = 15u - a;
            r = static_cast<u8>((bg_r * inv + static_cast<u8>(p[0] >> 4)    * a) >> 4);
            g = static_cast<u8>((bg_g * inv + static_cast<u8>(p[0] & 0x0Fu) * a) >> 4);
            b = static_cast<u8>((bg_b * inv + static_cast<u8>(p[1] >> 4)    * a) >> 4);
        }
        *dst = static_cast<uint16_t>((g << 4 | r) | (static_cast<u32>(bg_a << 4 | b) << 8));
    };

    // Two-level loop: hoist sr_t, sr_b, rt, rb (4 dy-dependent values) to the outer
    // pass — 10 outer iterations × 4 values = 40 computations vs flat 31 × 4 = 124.
    for (int dy = 0; dy < 10; ++dy) {
        const u8* const sr_t = src_base + (VP_Y + dy) * kRS;
        const u8* const sr_b = src_base + (VP_Y + (int)VP_H - 1 - dy) * kRS;
        const u32 rt = s_outer_row_lut[dy];
        const u32 rb = s_outer_row_lut[(int)VP_H-1-dy];
        for (int dx = 0; dx < kCornerCnts[dy]; ++dx) {
            const int x_l = (VP_X + dx) * 2;
            const int x_r = (VP_X + (int)VP_W - 1 - dx) * 2;
            const u32 cl  = s_outer_col_lut[dx];
            const u32 cr  = s_outer_col_lut[(int)VP_W-1-dx];
            blend(sr_t + x_l, fb16 + rt + cl);  // TL
            blend(sr_t + x_r, fb16 + rt + cr);  // TR
            blend(sr_b + x_l, fb16 + rb + cl);  // BL
            blend(sr_b + x_r, fb16 + rb + cr);  // BR
        }
    }
}
// =============================================================================
// write_theme_defaults
//
// Writes the eight canonical default key/value pairs to an [theme] section
// in `path`.  Shared by write_default_ovl_theme_if_missing() and
// DropdownSelectorGui::apply_theme() — both call identical sets of values,
// so a single loop over a static table eliminates 13 redundant call-site
// expansions in the binary.
// =============================================================================
[[gnu::noinline]]
static void write_theme_defaults(const std::string& path) {
    static constexpr const char* kv[][2] = {
        {"bg_color",       "000000"},
        {"bg_alpha",       "13"    },
        {"dpad_button_color",   "333333"},
        {"a_button_color",     "333333"},
        {"b_button_color",     "333333"},
        {"start_button_color", "333333"},
        {"select_button_color","333333"},
        {"border_color",   "333333"},
        {"backdrop_color", "000000"},
        {"frame_color",    "111111"},
        {"frame_alpha",    "14"    },
        {"gb_text_color",  "ffffff"},
    };
    for (const auto& p : kv)
        ult::setIniFileValue(path, "theme", p[0], p[1], "");
}

// =============================================================================
// write_default_ovl_theme_if_missing
//
// Creates OVL_THEMES_DIR and writes canonical default values to OVL_THEME_FILE
// if that file does not yet exist.  The active theme is always at OVL_THEME_FILE;
// the display name is tracked separately in config.ini (key: ovl_theme).
// =============================================================================
static void write_default_ovl_theme_if_missing() {
    ult::createDirectory(OVL_THEMES_DIR);
    const std::string path(OVL_THEME_FILE);
    if (ult::isFile(path)) return;
    write_theme_defaults(path);
}

// =============================================================================
// load_ovl_theme
//
// Reads the selected theme name from config.ini (key: ovl_theme), constructs
// the path OVL_THEMES_DIR/<name>.ini, and updates all overlay-color globals.
// g_ovl_theme_name is populated from the config value (the filename stem) so
// the settings UI can display it without touching the theme file itself.
// Falls back to compile-time defaults for any malformed or absent key.
// =============================================================================
static void load_ovl_theme() {
    // Default hex strings as plain literals — avoids four function-local
    // static const std::string objects, each of which needs a __cxa_guard
    // acquire/release pair (~32 bytes overhead) on every call to load_ovl_theme.
    // valid_hex returns by value; std::move for the 6-char hit path, a cheap
    // SSO construction for the default path — net cost is identical to before.
    const auto valid_hex = [](std::string v, const char* def) -> std::string {
        return v.size() == 6 ? std::move(v) : def;
    };

    // Determine which theme file to load from config.ini.
    const std::string nm = ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyOvlTheme);
    const std::string theme_name = nm.empty() ? "default" : nm;

    strncpy(g_ovl_theme_name, theme_name.c_str(), sizeof(g_ovl_theme_name) - 1);
    g_ovl_theme_name[sizeof(g_ovl_theme_name) - 1] = '\0';

    // Load colors from the single active theme file.
    const std::string path(OVL_THEME_FILE);
    if (!ult::isFile(path)) return;  // compile-time defaults remain in effect

    const std::string bg_hex  = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "bg_color"),      "000000");
    const std::string dpad_hex  = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "dpad_button_color"),   "333333");
    const std::string abtn_hex  = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "a_button_color"),      "333333");
    const std::string bbtn_hex  = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "b_button_color"),      "333333");
    const std::string start_hex = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "start_button_color"),  "333333");
    const std::string sel_hex   = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "select_button_color"), "333333");
    const std::string bdr_hex = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "border_color"),  "333333");
    const std::string bkd_hex = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "backdrop_color"),"000000");
    const std::string frm_hex = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "frame_color"),   "111111");
    const std::string txt_hex = valid_hex(
        ult::parseValueFromIniSection(path, "theme", "gb_text_color"), "ffffff");

    int alpha = 13;
    {
        const std::string av = ult::parseValueFromIniSection(path, "theme", "bg_alpha");
        int v = 0;
        if (!av.empty() && parse_uint(av, v) && v <= 15) alpha = v;
    }

    int frame_alpha = 14;
    {
        const std::string av = ult::parseValueFromIniSection(path, "theme", "frame_alpha");
        int v = 0;
        if (!av.empty() && parse_uint(av, v) && v <= 15) frame_alpha = v;
    }

    // Opacity override — when g_ovl_opaque is set, force both variable alpha
    // channels to 15 regardless of what the theme specifies.  btn, bdr,
    // backdrop, and text are always 15u, so only bg and frame need adjustment.
    if (g_ovl_opaque) { alpha = 15; frame_alpha = 15; }

    g_ovl_bg_col      = tsl::RGB888(bg_hex,   static_cast<size_t>(alpha));
    g_ovl_dpad_col    = tsl::RGB888(dpad_hex, 15u);
    g_ovl_abtn_col    = tsl::RGB888(abtn_hex, 15u);
    g_ovl_bbtn_col    = tsl::RGB888(bbtn_hex, 15u);
    g_ovl_start_col   = tsl::RGB888(start_hex,15u);
    g_ovl_select_col  = tsl::RGB888(sel_hex,  15u);
    g_ovl_bdr_col     = tsl::RGB888(bdr_hex, 15u);
    g_ovl_backdrop_col= tsl::RGB888(bkd_hex, 15u);
    g_ovl_frame_col   = tsl::RGB888(frm_hex, static_cast<size_t>(frame_alpha));
    g_ovl_text_col    = tsl::RGB888(txt_hex, 15u);

    // tsl HUD / logo overrides — only applied if the key is present and valid in
    // ovl_theme.ini.  Absent keys leave the tsl:: variable at whatever value
    // Ultrahand's own theme set, so the regular overlay UI is unaffected.
    {
        const auto tryColor = [&](const char* key, tsl::Color& target) {
            const std::string v = ult::parseValueFromIniSection(path, "theme", key);
            if (v.size() == 6) target = tsl::RGB888(v, 15u);
        };
        tryColor("dynamic_logo_color_1",  tsl::dynamicLogoRGB1);
        tryColor("dynamic_logo_color_2",  tsl::dynamicLogoRGB2);
        tryColor("logo_color_1",          tsl::logoColor1);
        tryColor("logo_color_2",          tsl::logoColor2);
        tryColor("top_separator_color",   tsl::topSeparatorColor);
        tryColor("clock_color",           tsl::clockColor);
        tryColor("temperature_color",     tsl::temperatureColor);
        tryColor("battery_color",         tsl::batteryColor);
        tryColor("battery_charging_color",tsl::batteryChargingColor);
        tryColor("battery_low_color",     tsl::batteryLowColor);

        // widget_backdrop: alpha and color are linked — only apply if color is present.
        const std::string wbkd_raw = ult::parseValueFromIniSection(path, "theme", "widget_backdrop_color");
        if (wbkd_raw.size() == 6) {
            const std::string wbkd_a = ult::parseValueFromIniSection(path, "theme", "widget_backdrop_alpha");
            int va = static_cast<int>(tsl::widgetBackdropAlpha);  // keep existing alpha as default
            int vp = 0;
            if (!wbkd_a.empty() && parse_uint(wbkd_a, vp) && vp <= 15) va = vp;
            tsl::widgetBackdropAlpha = static_cast<size_t>(va);
            tsl::widgetBackdropColor = tsl::RGB888(wbkd_raw, tsl::widgetBackdropAlpha);
        }

        // dynamic_widget_colors — optional boolean (1/0).  Only applied when the
        // key is present; absent key leaves ult::dynamicWidgetColors untouched so
        // the user's own Ultrahand theme setting is preserved.
        {
            const std::string dwc = ult::parseValueFromIniSection(path, "theme", "dynamic_widget_colors");
            if (!dwc.empty())
                ult::dynamicWidgetColors = (dwc != ult::FALSE_STR);
        }
    }

    // Recompute packed RGBA4444 for direct-fb writes.
    // Layout: r nibble at bit 0, g at 4, b at 8, a at 12.
    g_ovl_bg_packed = static_cast<uint16_t>(
        static_cast<uint16_t>(g_ovl_bg_col.r)        |
        (static_cast<uint16_t>(g_ovl_bg_col.g) <<  4) |
        (static_cast<uint16_t>(g_ovl_bg_col.b) <<  8) |
        (static_cast<uint16_t>(g_ovl_bg_col.a) << 12));

    g_ovl_frame_packed = static_cast<uint16_t>(
        static_cast<uint16_t>(g_ovl_frame_col.r)        |
        (static_cast<uint16_t>(g_ovl_frame_col.g) <<  4) |
        (static_cast<uint16_t>(g_ovl_frame_col.b) <<  8) |
        (static_cast<uint16_t>(g_ovl_frame_col.a) << 12));
}

// =============================================================================
// close_overlay_direct_mode
//
// KEY_B close handler shared by SettingsGui and RomSelectorGui.
//
// Both GUIs do the same thing on B: clear the saved settings-scroll position,
// optionally fire the envSetNextLoad dance to return to Ultrahand when running
// under --direct --comboReturn, then close the overlay.
//
// Without ICF (--icf is not in the Makefile), these virtual handleInput bodies
// stay as two separate copies in the binary.  Extracting here with noinline
// gives one canonical copy that both call sites jump to.
// =============================================================================
[[gnu::noinline]]
static void close_overlay_direct_mode() {
    g_settings_scroll[0] = '\0';
    if (g_directMode && g_comboReturn) {
        ult::launchingOverlay.store(true, std::memory_order_release);
        ult::setIniFileValue(
            ult::ULTRAHAND_CONFIG_INI_PATH,
            ult::ULTRAHAND_PROJECT_NAME,
            ult::IN_OVERLAY_STR,
            ult::TRUE_STR
        );
        disableSound.store(true, std::memory_order_release);
        skipRumbleDoubleClick = true;

        const std::string selfFilename = ult::getNameFromPath(std::string(g_self_path));
        const bool needsFgFix = ult::resetForegroundCheck.load(std::memory_order_acquire) ||
                                ult::lastTitleID != ult::getTitleIdAsString();
        std::string argvStr = selfFilename + " --skipCombo"
                            + " --foregroundFix " + (needsFgFix ? '1' : '0')
                            + " --lastTitleID " + ult::lastTitleID;
        envSetNextLoad(returnOverlayPath.c_str(), argvStr.c_str());
    }
    tsl::Overlay::get()->close();
}
#pragma GCC pop_options