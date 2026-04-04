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
static bool combo_pressed(u64 keysDown, u64 keysHeld) {
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
static void process_zr_fast_forward(bool zr_down, bool zr_held,
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
        gb_audio_resume();
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
static void process_zl_pass_through(bool zl_down, bool zl_held,
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
// run_once_setup
//
// One-time per-session setup called from update() on the first tick of both
// GBOverlayGui and GBWindowedGui.  Guards itself — subsequent calls are no-ops.
// =============================================================================
[[gnu::noinline]]
static void run_once_setup(bool& runOnce, bool& restoreHapticState) {
    if (!runOnce) return;
    if (g_ingame_haptics && !ult::useHapticFeedback) {
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
// restoreHapticState = true when g_ingame_haptics is on but the global flag
// was off at session start.
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

static bool poll_console_docked() {
    if (g_frame_count >= g_dock_next_check) {
        g_console_docked  = ult::consoleIsDocked();
        g_dock_next_check = g_frame_count + kDockCheckInterval;
    }
    return g_console_docked;
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
static size_t get_rom_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const size_t sz = static_cast<size_t>(ftell(f));
    fclose(f);
    return sz;
}

// Returns true when the ROM at |path| can be loaded on the current memory tier.
// Mirrors the rejection conditions in gb_load_rom() so the selector can dim
// unplayable entries before the user tries to launch them.
static bool rom_is_playable(const char* path) {
    const size_t sz = get_rom_size(path);
    if (!sz) return false;
    if (ult::limitedMemory          && sz >= kROM_2MB) return false;
    if (!ult::expandedMemory        && sz >= kROM_4MB) return false;
    if (!ult::furtherExpandedMemory && sz >= kROM_6MB) return false;
    return true;
}

// Returns the actionable error string if the ROM at path cannot be loaded on
// the current heap tier, or nullptr if it is playable.
static const char* rom_playability_message(const char* path) {
    const size_t sz = get_rom_size(path);
    if (!sz) return nullptr;
    if ( ult::limitedMemory          && sz >= kROM_2MB && sz < kROM_4MB) return REQUIRES_AT_LEAST_6MB;
    if (!ult::expandedMemory         && sz >= kROM_4MB && sz < kROM_6MB) return REQUIRES_AT_LEAST_8MB;
    if (!ult::furtherExpandedMemory  && sz >= kROM_6MB)                  return REQUIRES_AT_LEAST_10MB;
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

// =============================================================================
// Save helpers
// =============================================================================

// Derives a per-ROM path of the form <dir><basename_no_ext><ext>.
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

static std::string make_slot_label(int slot) {
    char buf[16];
    snprintf(buf, sizeof(buf), "Slot %d", slot);
    return buf;
}

static void build_save_backup_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz);

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
// draw_wallpaper_direct
//
// Drop-in replacement for renderer->drawWallpaper() with the same guards and
// identical visual output, optimised for this project's fixed call contract:
//   • always 448×720 full-screen (correctFrameSize guaranteed)
//   • always opacity 1.0 (Renderer::s_opacity == 1.0 in normal operation)
//   • always preserveAlpha == true
//   • always called immediately after fillScreen (no scissoring active)
//
// Improvements over the generic processBMPChunk path:
//
//  1. Static precomputed offset tables (yParts[720] + xGroupParts[56]).
//     processBMPChunk heap-allocates std::vector<u32>(chunkHeight) per thread
//     per call.  Here those values are computed once (≈ 3 KB static) and
//     reused every frame.
//
//  2. NEON 8-pixel contiguous stores.
//     The block-linear swizzle maps each run of 8 consecutive pixels to 8
//     consecutive framebuffer slots (offsets +0..+7).  The generic path NEON-
//     extracts 16 channels then scalar-scatters them individually back out.
//     Here vld2_u8 loads 8 pixels, all channel work stays in NEON, and a
//     single vst2_u8 (16 bytes) writes the whole group.
//
//  3. Background color read once, not per pixel.
//     fillScreen writes one constant Color to every slot, so dst.a (preserved
//     by preserveAlpha==true) is the same everywhere.  Read framebuffer[0]
//     once and broadcast into NEON lanes.
//
//  4. No scissoring checks anywhere in the hot loop.
//
//  5. Optional opaque-region skip (overlay player mode).
//     The GB screen renderer overwrites rows [skip_row_start, skip_row_end)
//     columns [skip_grp_start*8, skip_grp_end*8) with fully-opaque pixels,
//     making wallpaper work there invisible.  When those parameters are
//     supplied the loop is split into three branch-free bands:
//
//       Top    (rows 0 .. skip_row_start-1)          all 56 groups
//       Middle (rows skip_row_start .. skip_row_end-1) left + right strips
//       Bottom (rows skip_row_end .. 719)             all 56 groups
//
//     In overlay mode this skips VP_W×VP_H = 400×360 = 144,000 pixels
//     (44.6% of the frame) with zero per-pixel branching.
//     VP_X=24 and VP_X+VP_W=424 are both exact multiples of 8, so the
//     group boundaries fall cleanly at groups 3 and 53 — no partial groups.
//
//     Default parameter values (skip_row_start = skip_row_end = kH) produce
//     a degenerate "no skip" schedule: only the top band runs (all rows),
//     the middle and bottom bands have zero iterations.
//
// Byte-layout contract (verified against processBMPChunk):
//   wallpaper:  byte0 = R<<4|G   byte1 = B<<4|A
//   Color (LE): byte0 = G<<4|R   byte1 = A<<4|B
//
// Threading: identical to drawBitmapRGBA4444 — ult::numThreads threads,
// each calling ult::inPlotBarrier.arrive_and_wait() before joining.
// =============================================================================
[[gnu::noinline]]
static void draw_wallpaper_direct(tsl::gfx::Renderer* renderer,
                                   u32 skip_row_start,
                                   u32 skip_row_end,
                                   u32 skip_grp_start,
                                   u32 skip_grp_end,
                                   u32 src_row_offset,
                                   u32 fb_height) {
    // ── Same entry guards as Renderer::drawWallpaper() ──────────────────────
    if (!ult::expandedMemory || ult::refreshWallpaper.load(std::memory_order_acquire)) return;

    ult::inPlot.store(true, std::memory_order_release);

    if (!ult::wallpaperData.empty() &&
        !ult::refreshWallpaper.load(std::memory_order_acquire) &&
        // correctFrameSize is false for the 448×613 free-overlay framebuffer;
        // bypass the check when src_row_offset > 0 (free-overlay call site).
        (ult::correctFrameSize || src_row_offset > 0u))
    {
        // ── Static precomputed offset tables ─────────────────────────────────
        // yParts[y]       : y-contribution to the block-linear framebuffer offset.
        // xGroupParts[g]  : x-contribution for the start of 8-pixel group g
        //                   (g = 0..55; 56 groups × 8 pixels = 448 px/row).
        //
        // Pure functions of the fixed 448×720 / offsetWidthVar=112 geometry.
        // Initialised on the first call; never reallocated.
        // Total static storage: 720×4 + 56×4 = 3,104 bytes.
        static constexpr u32 kW  = 448u;
        static constexpr u32 kH  = 720u;
        static constexpr u32 kGW = kW / 8u;  // 56 groups of 8 pixels per row

        static u32  s_yParts[kH];
        static u32  s_xGroupParts[kGW];
        static bool s_tables_ready = false;

        if (__builtin_expect(!s_tables_ready, 0)) {
            const u32 owv = offsetWidthVar;
            for (u32 y = 0u; y < kH; ++y) {
                s_yParts[y] = ((((y & 127u) >> 4) + ((y >> 7) * owv)) << 9)
                            + ((y & 8u) << 5) + ((y & 6u) << 4) + ((y & 1u) << 3);
            }
            for (u32 g = 0u; g < kGW; ++g) {
                const u32 x = g * 8u;
                // (x & 7) == 0 always (x is a multiple of 8), so that term is 0.
                s_xGroupParts[g] = ((x >> 5) << 12) + ((x & 16u) << 3) + ((x & 8u) << 1);
            }
            s_tables_ready = true;
        }

        // ── Background color — read once from framebuffer[0] ─────────────────
        // fillScreen wrote a(defaultBackgroundColor) uniformly to every pixel;
        // the first slot is representative.  r/g/b feed the blend formula;
        // a is written back unchanged (preserveAlpha == true).
        tsl::Color* const framebuffer =
            static_cast<tsl::Color*>(renderer->getCurrentFramebuffer());

        const u8 bg_r = framebuffer[0].r;
        const u8 bg_g = framebuffer[0].g;
        const u8 bg_b = framebuffer[0].b;
        const u8 bg_a = framebuffer[0].a;

        // globalAlphaLimit mirrors the drawBitmapRGBA4444 parameter.
        const u8 globalAlphaLimit =
            static_cast<u8>(0xF * tsl::gfx::Renderer::s_opacity);

        const u8* const src_base = ult::wallpaperData.data();

        const u32 numThreads = ult::numThreads;

        // ── Work-weighted thread chunk distribution ───────────────────────────
        // Equal-row splitting produces severe imbalance when a skip region is
        // active: with skip_row_start=108 / skip_row_end=468 / 4 threads,
        // Thread 1 gets 1,080 groups (all middle) while Thread 3 gets 10,080
        // (all bottom) — a 9:1 ratio. Thread 3 is the wall-clock bottleneck.
        //
        // Instead, distribute rows by work units:
        //   full row   = kGW groups (56)
        //   middle row = skip_grp_start + (kGW - skip_grp_end) groups (= 6)
        //
        // This balances all four threads to ~5,580 groups each, giving ~1.8×
        // speedup over the equal-row approach for the overlay case.
        //
        // For the no-skip default (skip_row_start == skip_row_end == kH), every
        // row weighs kGW and the split degenerates to equal rows — identical
        // behaviour to the old chunkSize formula. No regression.
        const u32 mid_row_work = skip_grp_start + (kGW - skip_grp_end);
        const u32 total_work   = skip_row_start * kGW
                               + (skip_row_end - skip_row_start) * mid_row_work
                               + (fb_height - skip_row_end) * kGW;
        const u32 target       = (total_work + numThreads - 1u) / numThreads;

        // thread_starts[t] / thread_starts[t+1] are the row range for thread t.
        // Stack array — numThreads is always small (≤ 4 on Switch).
        u32 thread_starts[9] = {};  // [0..numThreads], max 8 threads + sentinel
        thread_starts[0] = 0u;
        {
            u32 cum = 0u, ti = 1u;
            for (u32 y = 0u; y < fb_height && ti < numThreads; ++y) {
                cum += (y >= skip_row_start && y < skip_row_end) ? mid_row_work : kGW;
                if (cum >= ti * target) thread_starts[ti++] = y + 1u;
            }
            thread_starts[numThreads] = fb_height;
        }

        for (u32 t = 0u; t < numThreads; ++t) {
            const u32 rowStart = thread_starts[t];
            const u32 rowEnd   = thread_starts[t + 1u];

            ult::renderThreads[t] = std::thread([=]() {
                // ── NEON constants — hoisted above all loops ──────────────────
                // Shared by both kernels; compiler dead-code-eliminates the
                // unused ones inside each run_bands instantiation.
                //
                // Byte-layout cross-format swap (verified against processBMPChunk):
                //   wallpaper: byte0 = R<<4|G   byte1 = B<<4|A
                //   Color(LE): byte0 = G<<4|R   byte1 = A<<4|B
                const uint8x8_t  v_mask4     = vdup_n_u8(0x0Fu);
                const uint8x8_t  v_alpha_lim = vdup_n_u8(globalAlphaLimit);
                const uint8x8_t  v_bg_a4     = vdup_n_u8(static_cast<u8>(bg_a << 4));
                // Full-kernel only (dead in fast path):
                const uint8x8_t  v15         = vdup_n_u8(15u);
                const uint16x8_t v_bg_r16    = vdupq_n_u16(bg_r);
                const uint16x8_t v_bg_g16    = vdupq_n_u16(bg_g);
                const uint16x8_t v_bg_b16    = vdupq_n_u16(bg_b);

                // ── Dark-background fast kernel ───────────────────────────────
                // When bg_r == bg_g == bg_b == 0, the bg_ch * inv_a term is
                // always zero for every channel.  The blend formula:
                //   out_ch = (bg_ch * (15-a) + src_ch * a) >> 4
                // reduces to:
                //   out_ch = (src_ch * a) >> 4
                // dropping vsub, vmovl, 3×vmulq_u16, 3×vaddq_u16 — 8 fewer ops
                // per group (33%).  vmin_u8 is kept for non-1.0 opacity support.
                const auto do_group_fast = [&](const u32 yPart, const u8* rs, const u32 g) {
                    const uint8x8x2_t raw = vld2_u8(rs + (g << 4u));

                    const uint8x8_t src_r = vshr_n_u8(raw.val[0], 4);
                    const uint8x8_t src_g = vand_u8(raw.val[0], v_mask4);
                    const uint8x8_t src_b = vshr_n_u8(raw.val[1], 4);
                    const uint8x8_t src_a = vmin_u8(vand_u8(raw.val[1], v_mask4), v_alpha_lim);

                    const uint8x8_t out_r = vshrn_n_u16(vmull_u8(src_r, src_a), 4);
                    const uint8x8_t out_g = vshrn_n_u16(vmull_u8(src_g, src_a), 4);
                    const uint8x8_t out_b = vshrn_n_u16(vmull_u8(src_b, src_a), 4);

                    const uint8x8_t byte0_out = vorr_u8(vshl_n_u8(out_g, 4), out_r);
                    const uint8x8_t byte1_out = vorr_u8(v_bg_a4, out_b);
                    vst2_u8(reinterpret_cast<u8*>(framebuffer + yPart + s_xGroupParts[g]),
                            uint8x8x2_t{{byte0_out, byte1_out}});
                };

                // ── General kernel ────────────────────────────────────────────
                // Full blend for non-black backgrounds.
                // Blend: out_ch = (bg_ch * (15-a) + src_ch * a) >> 4
                //   = blendColor(bg_ch, src_ch, a) — matches scalar path exactly.
                //   u16 intermediates prevent overflow (max = 15*15 + 15*15 = 450).
                const auto do_group_full = [&](const u32 yPart, const u8* rs, const u32 g) {
                    const uint8x8x2_t raw = vld2_u8(rs + (g << 4u));

                    const uint8x8_t src_r = vshr_n_u8(raw.val[0], 4);
                    const uint8x8_t src_g = vand_u8(raw.val[0], v_mask4);
                    const uint8x8_t src_b = vshr_n_u8(raw.val[1], 4);
                    const uint8x8_t src_a = vmin_u8(vand_u8(raw.val[1], v_mask4), v_alpha_lim);

                    const uint8x8_t  inv_a = vsub_u8(v15, src_a);
                    const uint16x8_t ia16  = vmovl_u8(inv_a);

                    const uint8x8_t out_r = vshrn_n_u16(
                        vaddq_u16(vmulq_u16(ia16, v_bg_r16), vmull_u8(src_r, src_a)), 4);
                    const uint8x8_t out_g = vshrn_n_u16(
                        vaddq_u16(vmulq_u16(ia16, v_bg_g16), vmull_u8(src_g, src_a)), 4);
                    const uint8x8_t out_b = vshrn_n_u16(
                        vaddq_u16(vmulq_u16(ia16, v_bg_b16), vmull_u8(src_b, src_a)), 4);

                    const uint8x8_t byte0_out = vorr_u8(vshl_n_u8(out_g, 4), out_r);
                    const uint8x8_t byte1_out = vorr_u8(v_bg_a4, out_b);
                    vst2_u8(reinterpret_cast<u8*>(framebuffer + yPart + s_xGroupParts[g]),
                            uint8x8x2_t{{byte0_out, byte1_out}});
                };

                // ── Three-band scheduler (generic over kernel) ────────────────
                // 'auto&&' creates a distinct template instantiation per kernel,
                // so the compiler generates two fully-specialised code paths with
                // no dead NEON ops and no inner-loop branching.
                //
                // Band schedule (each range clamped to this thread's chunk):
                //   Top    (rows < skip_row_start):           all kGW groups
                //   Middle (skip_row_start ≤ row < skip_row_end): left + right strips
                //   Bottom (rows ≥ skip_row_end):             all kGW groups
                //
                // Default (skip_row_start == skip_row_end == kH): middle and bottom
                // bands have zero iterations — only top runs, identical to the
                // original single-band loop.
                const auto run_bands = [&](auto&& do_group) {
                    const u32 top_end   = std::min(rowEnd, skip_row_start);
                    for (u32 y = rowStart; y < top_end; ++y) {
                        const u32       yPart = s_yParts[y];
                        const u8* const rs    = src_base + (y + src_row_offset) * (kW * 2u);
                        for (u32 g = 0u; g < kGW; ++g) do_group(yPart, rs, g);
                    }

                    const u32 mid_start = std::max(rowStart, skip_row_start);
                    const u32 mid_end   = std::min(rowEnd,   skip_row_end);
                    for (u32 y = mid_start; y < mid_end; ++y) {
                        const u32       yPart = s_yParts[y];
                        const u8* const rs    = src_base + (y + src_row_offset) * (kW * 2u);
                        for (u32 g = 0u;           g < skip_grp_start; ++g) do_group(yPart, rs, g);
                        for (u32 g = skip_grp_end; g < kGW;            ++g) do_group(yPart, rs, g);
                    }

                    const u32 bot_start = std::max(rowStart, skip_row_end);
                    for (u32 y = bot_start; y < rowEnd; ++y) {
                        const u32       yPart = s_yParts[y];
                        const u8* const rs    = src_base + (y + src_row_offset) * (kW * 2u);
                        for (u32 g = 0u; g < kGW; ++g) do_group(yPart, rs, g);
                    }

                    ult::inPlotBarrier.arrive_and_wait();
                };

                // Dispatch once at thread-start — zero inner-loop overhead.
                if (bg_r == 0u && bg_g == 0u && bg_b == 0u)
                    run_bands(do_group_fast);
                else
                    run_bands(do_group_full);
            });
        }

        for (auto& th : ult::renderThreads) th.join();
    }

    ult::inPlot.store(false, std::memory_order_release);
}