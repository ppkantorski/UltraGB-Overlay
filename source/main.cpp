/********************************************************************************
 * File: main.cpp
 * Description:
 *   Game Boy / Game Boy Color emulator overlay for Nintendo Switch.
 *
 *   Config: sdmc:/config/ultragb/config.ini  (rom_dir=sdmc:/roms/gb/)
 *   Saves:  sdmc:/config/ultragb/saves/
 *
 *   Controls in-game:
 *     A / B / D-pad / + (Start) / - (Select) — mapped to GB buttons
 *     X — pause and return to ROM picker (state preserved)
 *     System overlay combo — hides overlay (game pauses automatically)
 * 
 *  Licensed under GPLv2
 *  Copyright (c) 2026 ppkantorski
 ********************************************************************************/

#define NDEBUG
#define STBTT_STATIC
#define TESLA_INIT_IMPL

#include <ultra.hpp>
#include <tesla.hpp>

#include "elm_volume.hpp"    // ← VolumeTrackBar
#include "elm_ultraframe.hpp" // ← UltraGBOverlayFrame (two-page frame)
#include "gb_utils.hpp"      // ← globals + all shared helpers
                             //   (pulls in gb_globals.hpp → gb_audio.h, gb_core.h, gb_renderer.h)

#include <dirent.h>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>

using namespace ult;

// =============================================================================
// Paths, globals, and data-coupled helpers → gb_globals.hpp (via gb_utils.hpp)
// =============================================================================

static void save_vol_backup() {
    char buf[4];
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyVolBackup,
                         vol_to_str(g_vol_backup, buf), "");
}


static void load_config() {
    const std::string& path = kConfigFile;

    // rom_dir
    const std::string rom_dir_val = ult::parseValueFromIniSection(path, kConfigSection, kKeyRomDir);
    if (!rom_dir_val.empty() && rom_dir_val.size() < sizeof(g_rom_dir) - 2) {
        strncpy(g_rom_dir, rom_dir_val.c_str(), sizeof(g_rom_dir) - 2);
        const size_t vlen = strlen(g_rom_dir);
        if (vlen > 0 && g_rom_dir[vlen - 1] != '/') {
            g_rom_dir[vlen]     = '/';
            g_rom_dir[vlen + 1] = '\0';
        }
    }

    // rom_dir
    const std::string save_dir_val = ult::parseValueFromIniSection(path, kConfigSection, kKeySaveDir);
    if (!save_dir_val.empty() && save_dir_val.size() < sizeof(g_save_dir) - 2) {
        strncpy(g_save_dir, save_dir_val.c_str(), sizeof(g_save_dir) - 2);
        const size_t vlen = strlen(g_save_dir);
        if (vlen > 0 && g_save_dir[vlen - 1] != '/') {
            g_save_dir[vlen]     = '/';
            g_save_dir[vlen + 1] = '\0';
        }
    }

    // original_palette key is superseded by per-game palette_mode in configure/<rom>.ini
    // (kept for legacy: if a user has it in config.ini it is silently ignored now)

    // last_rom — basename of the last-played ROM file
    const std::string last_rom_val = ult::parseValueFromIniSection(path, kConfigSection, kKeyLastRom);
    if (!last_rom_val.empty() && last_rom_val.size() < sizeof(g_last_rom_path) - 1)
        strncpy(g_last_rom_path, last_rom_val.c_str(), sizeof(g_last_rom_path) - 1);

    // volume — master GB audio volume (0–100), default 50
    { int v = 0; if (parse_uint(ult::parseValueFromIniSection(path, kConfigSection, kKeyVolume), v))
        gb_audio_set_volume(static_cast<u8>(std::clamp(v, 0, 100))); }

    // vol_backup — unmute restore target (0 treated as absent → default 50)
    { int v = 0; if (parse_uint(ult::parseValueFromIniSection(path, kConfigSection, kKeyVolBackup), v))
        g_vol_backup = static_cast<u8>(std::clamp(v, 1, 100)); }

    // lcd_grid — 0 = off (default), 1 = LCD grid effect enabled
    const std::string grid_val = ult::parseValueFromIniSection(path, kConfigSection, kKeyLcdGrid);
    if (!grid_val.empty())
        g_lcd_grid = (grid_val == "1");

    // windowed — 0 = normal (default), 1 = windowed mode
    const std::string win_val = ult::parseValueFromIniSection(path, kConfigSection, kKeyWindowed);
    if (!win_val.empty())
        g_windowed_mode = (win_val == "1");

    const std::string hap_val = ult::parseValueFromIniSection(path, kConfigSection, kKeyIngameHaptics);
    if (!hap_val.empty())
        g_ingame_haptics = (hap_val != "0");

    const std::string wall_val = ult::parseValueFromIniSection(path, kConfigSection, kKeyIngameWallpaper);
    if (!wall_val.empty())
        g_ingame_wallpaper = (wall_val == "1");

    // win_pos_x / win_pos_y — persisted VI-space window position
    { int v = 0;
      if (parse_uint(ult::parseValueFromIniSection(path, kConfigSection, kKeyWinPosX), v)) g_win_pos_x = v;
      if (parse_uint(ult::parseValueFromIniSection(path, kConfigSection, kKeyWinPosY), v)) g_win_pos_y = v; }

    // win_scale — windowed display scale: 1–6 (default 1).
    // Any absent or unrecognised value falls back to 1.
    //
    // In the NORMAL overlay (g_win_scale_locked == false) we store the raw
    // config value so the SettingsGui can display the user's stored intent —
    // make_label caps the display to the current session's effective maximum
    // via std::min(g_win_scale, maxScale) without touching the stored value.
    //
    // In the WINDOWED session (g_win_scale_locked == true) main() has already
    // read, clamped, and used g_win_scale to size the framebuffer.  We must
    // NOT override it here or the blit kernel will walk off the end of the
    // framebuffer (g_win_scale > actual framebuffer scale → crash).
    if (!g_win_scale_locked)
        g_win_scale = parse_win_scale_str(ult::parseValueFromIniSection(path, kConfigSection, kKeyWinScale));

    // win_output — "720" (default, 1.5× VI layer) or "1080" (pixel-perfect, 1:1 VI layer).
    // Takes effect on the next windowed launch.
    {
        const std::string out_val = ult::parseValueFromIniSection(path, kConfigSection, kKeyWinOutput);
        if (!out_val.empty())
            g_win_1080 = (out_val == "1080");
    }
}

// Write a config key with its default value only when the key is absent.
// Collapses the four-repetition parseValueFromIniSection → empty() → setIniFileValue
// sequence in write_default_config_if_missing into one shared body.
static void set_if_missing(const char* key, const char* def) {
    if (ult::parseValueFromIniSection(kConfigFile, kConfigSection, key).empty())
        ult::setIniFileValue(kConfigFile, kConfigSection, key, def);
}

static void write_default_config_if_missing() {
    set_if_missing("rom_dir",       g_rom_dir);
    set_if_missing("save_dir",      g_save_dir);
    set_if_missing("volume",        "50");
    set_if_missing("vol_backup",    "50");
    set_if_missing("pixel_perfect", "0");
    set_if_missing("windowed",      "0");
    set_if_missing("lcd_grid",      "0");
    set_if_missing("ingame_haptics", "1");
    set_if_missing("ingame_wallpaper", "0");
    set_if_missing("win_pos_x",     "840");
    set_if_missing("win_pos_y",     "432");
    set_if_missing("win_scale",     "1");
    set_if_missing("win_output",    "720");
}

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
// Called by GBWindowedGui on touch release after a successful drag.
static void save_win_pos() {
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinPosX,
                         ult::to_string(g_win_pos_x), "");
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinPosY,
                         ult::to_string(g_win_pos_y), "");
}

// Persist the windowed output resolution mode to config.ini.
// Takes effect on the next windowed launch (read before Tesla initialises the layer).
static void save_win_output() {
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinOutput,
                         g_win_1080 ? "1080" : "720", "");
}

// Persist the windowed scale (1/2/3) to config.ini.
// The new scale takes effect the next time the user launches a ROM in
// windowed mode (setNextOverlay reads it before Tesla initialises the layer).
static void save_win_scale() {
    const char s[2] = { static_cast<char>('0' + g_win_scale), '\0' };
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinScale, s, "");
}

// =============================================================================
// Quick Combo helpers
// =============================================================================

// Return the bare filename of this .ovl (e.g. "ultragb.ovl") from g_self_path.
// This is the section key used in overlays.ini.
static const char* ovl_filename() {
    if (!g_self_path[0]) return "";
    const char* sl = strrchr(g_self_path, '/');
    return sl ? sl + 1 : g_self_path;
}

// Default combos available in the Quick Combo picker.
// Mirrors utils.hpp defaultCombos — UltraGB does not include utils.hpp.
// const char* const[] instead of std::array<std::string>: all data in .rodata,
// zero runtime constructors/destructors, and lambda captures shrink from a full
// std::string copy (heap) to a bare pointer (8 bytes) per list item.
static constexpr const char* const g_defaultCombos[] = {
    "ZL+ZR+DDOWN",  "ZL+ZR+DRIGHT", "ZL+ZR+DUP",    "ZL+ZR+DLEFT",
    "L+R+DDOWN",    "L+R+DRIGHT",   "L+R+DUP",       "L+R+DLEFT",
    "L+DDOWN",      "R+DDOWN",
    "ZL+ZR+PLUS",   "L+R+PLUS",     "ZL+ZR+MINUS",   "L+R+MINUS",
    "ZL+MINUS",     "ZR+MINUS",     "ZL+PLUS",        "ZR+PLUS",    "MINUS+PLUS",
    "LS+RS",        "L+DDOWN+RS",   "L+R+LS",         "L+R+RS",
    "ZL+ZR+LS",     "ZL+ZR+RS",     "ZL+ZR+L",        "ZL+ZR+R",    "ZL+ZR+LS+RS"
};


// Remove keyCombo from every overlay's key_combo AND mode_combos list in
// overlays.ini, skipping our own section.  Called before assigning the combo
// so no two overlays share the same trigger — mirrors Ultrahand's behaviour.
// Remove keyCombo from every overlay's key_combo AND mode_combos in
// overlays.ini, and from every package's key_combo in packages.ini.
//
// Our own key_combo is ALWAYS checked — if UltraGB's own main open-combo
// conflicts with the mode combo being assigned it must be cleared, exactly as
// Ultrahand does when it deconflicts combos across overlays.
//
// skipOwnModeCombos=true  (startup path): our own mode_combos slot is left
//   untouched because nothing re-writes overlays.ini afterwards, so erasing it
//   would lose the stored combo permanently.
// skipOwnModeCombos=false (picker path, default): our section is fully
//   processed.  save_combo() writes the fresh combo right after this call, so
//   clearing our own mode_combos here is safe and produces a clean state.
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

        // 1. Main key_combo — always check, including our own section.
        //    UltraGB's open-combo conflicting with its own mode combo is a real
        //    conflict and must be cleared just like any other overlay's would.
        auto kcIt = section.find("key_combo");
        if (kcIt != section.end() && !kcIt->second.empty() &&
            tsl::hlp::comboStringToKeys(kcIt->second) ==
            tsl::hlp::comboStringToKeys(keyCombo)) {
            kcIt->second = "";
            dirty = true;
        }

        // 2. mode_combos — skip our own slot on the startup path only.
        if (isSelf && skipOwnModeCombos) continue;

        auto mcIt = section.find("mode_combos");
        if (mcIt != section.end() && !mcIt->second.empty()) {
            auto comboList = splitIniList(mcIt->second);
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
                mcIt->second = "(" + joinIniList(comboList) + ")";
                dirty = true;
            }
        }
    }

    if (dirty)
        ult::saveIniFileData(ult::OVERLAYS_INI_FILEPATH, data);

    // Packages — only key_combo matters (packages have no mode_combos).
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

// Register "-quicklaunch" as our single mode in overlays.ini (same pattern as
// status_main.cpp's mode registration).  Also reads mode_combos[0] into
// g_quick_combo so the Settings page can display the current combo.
// Called from Overlay::initServices() after load_config().
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

    const auto comboList = splitIniList(mc);
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
// e.g.  sdmc:/config/ultragb/configure/pokered.gb.ini
//
// Key: palette_mode = "GBC" | "SGB" | "DMG" | "Native"   (default absent = "GBC")
//   GBC    – GBC title-checksum lookup; greyscale for unknown games (default)
//   SGB    – same lookup; warm amber for unknown games (approximates SGB feel)
//   DMG    – classic green Game Boy LCD tint
//   Native – true greyscale, no colour tint at all
//
// Only meaningful for true DMG Games (header byte 0x143 bit7 clear).
// CGB Games always use hardware colour regardless of this setting.
// =============================================================================

// Peek at ROM header byte 0x143 to determine CGB support without loading the ROM.
//   0x80 = CGB compatible (also runs in DMG mode — palette selection allowed)
//   0xC0 = CGB only       (requires CGB hardware — palette locked to GBC)
// Both have bit7 set; only CGB-only also has bit6 set.
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

// Generic per-game config helpers — shared by all load_game_* / save_game_*
// functions below.  Both keep the cfgPath on the stack; no heap allocation.
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
    // SGB is no longer exposed in the UI; treat legacy configs as GBC
    return PaletteMode::GBC;  // default (empty, "GBC", or legacy "SGB")
}

static PaletteMode load_game_palette_mode(const char* romPath) {
    return str_to_palette_mode(load_game_cfg_str(romPath, "palette_mode"));
}

static void save_game_palette_mode(const char* romPath, PaletteMode m) {
    save_game_cfg_str(romPath, "palette_mode", palette_mode_to_str(m));
}

// ── Palette cycling helpers ───────────────────────────────────────────────────
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

// =============================================================================
// Per-game LCD ghosting helpers
// Stored in CONFIGURE_DIR/<rom>.ini under [config] lcd_ghosting = "0" | "1".
// Default is OFF (absent key → false) — ghosting is opt-in per game.
// =============================================================================
static bool load_game_lcd_ghosting(const char* romPath) {
    return load_game_cfg_str(romPath, "lcd_ghosting") == "1";
}

static void save_game_lcd_ghosting(const char* romPath, bool enabled) {
    save_game_cfg_str(romPath, "lcd_ghosting", enabled ? "1" : "0");
}

// =============================================================================
// Per-game no-sprite-limit helpers
// Stored in CONFIGURE_DIR/<rom>.ini under [config] no_sprite_limit = "0" | "1".
// Default is ON (absent key → true) — matches the hardcoded default in walnut.
// =============================================================================
static bool load_game_no_sprite_limit(const char* romPath) {
    return load_game_cfg_str(romPath, "no_sprite_limit") != "0";  // absent or "1" → true (on by default)
}

static void save_game_no_sprite_limit(const char* romPath, bool enabled) {
    save_game_cfg_str(romPath, "no_sprite_limit", enabled ? "1" : "0");
}



// =============================================================================
// Save helpers
// =============================================================================

// Derives a per-ROM path of the form <dir><basename_no_ext><ext>.
// Replaces the former build_save_path / build_state_path pair — both did
// the identical strrchr/strncpy/strip-ext/snprintf sequence; only the
// directory constant and extension string differed.
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
//   [8]  uint32  cart_ram_sz  cart RAM size in bytes (0 if none)
//   [12] uint32  fb_bytes     framebuffer size = GB_W*GB_H*2
//   [16] uint32  fb_flags     framebuffer format: 0=RGB565 (CGB), 1=RGBA4444 prepacked (DMG)
//   [20] gb_s    core         full emulator struct (function pointers re-patched on load)
//   [+]  uint8[] cart_ram     cart RAM contents (cart_ram_sz bytes)
//   [+]  uint16[] framebuffer last rendered frame (fb_bytes bytes)
//   [+]  GBAPU   apu_snapshot full APU runtime state
//
// Only STATE_VERSION (4) files are accepted; anything older is rejected and the
// emulator cold-boots cleanly rather than resuming with missing APU state.
// =============================================================================
static constexpr uint32_t STATE_MAGIC   = 0x47425354u; // 'GBST'
static constexpr uint32_t STATE_VERSION = 4u;


static void save_state(GBState& s) {
    // Only romPath is needed to derive the state file path.
    // The ROM buffer itself is NOT read by this function — gb_s (BSS), cartRam,
    // and g_gb_fb are the only data sources.  The caller may free s.rom before
    // calling save_state() to reduce peak heap pressure.
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
    // fb_flags encodes the pixel format stored in g_gb_fb:
    //   0 = native RGB555 (DMG before prepacked opt) or RGB565 (CGB)
    //   1 = RGBA4444 pre-packed (DMG with g_fb_is_prepacked=true)
    const uint32_t fbFlags = g_fb_is_prepacked ? 1u : 0u;

    fwrite(&magic,   sizeof(magic),   1, f);
    fwrite(&version, sizeof(version), 1, f);
    fwrite(&ramSz,   sizeof(ramSz),   1, f);
    fwrite(&fbBytes, sizeof(fbBytes), 1, f);
    fwrite(&fbFlags, sizeof(fbFlags), 1, f);
    fwrite(s.gb,     sizeof(*s.gb),   1, f);  // includes CPU regs, WRAM, VRAM, OAM, HRAM
    if (s.cartRam && s.cartRamSz)
        fwrite(s.cartRam, 1, s.cartRamSz, f);
    fwrite(g_gb_fb, 1, fbBytes, f);

    // v3: full GBAPU runtime snapshot.
    // Peanut-GB delegates all APU I/O to our callbacks so APU state is NOT
    // inside gb_s.  Saving only registers is insufficient: apply_reg() never
    // sets ch.enabled (it deliberately masks trigger bits), so every channel
    // would stay silent after a save-state resume even with correct registers.
    // The GBAPU snapshot captures the full computed state (enabled, timer,
    // duty_pos, current vol, seq_step, lfsr, hp_ch, ...) so audio resumes
    // immediately with the correct sound.
    // gb_audio_shutdown() must have been called before save_state() so the
    // thread has written its final state back to s_ctrl.snapshot.
    GBAPU apu_snap{};
    gb_audio_save_state(&apu_snap);
    fwrite(&apu_snap, sizeof(apu_snap), 1, f);

    fclose(f);
}

// Returns true if a valid state was loaded and the emulator is ready to run.
// On failure, leaves the emulator in cold-boot state (gb_init already called).
// *apu_restored is set true when the GBAPU snapshot was successfully restored.
// When false, the caller must call gb_audio_reset_regs() before gb_audio_init().
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

    // Sanity checks
    if (ramSz   != static_cast<uint32_t>(s.cartRamSz))  { fclose(f); return false; }
    if (fbBytes != static_cast<uint32_t>(GB_W * GB_H * sizeof(uint16_t))) { fclose(f); return false; }

    // Read the core struct — this overwrites function pointers; we re-patch below
    if (fread(s.gb, sizeof(*s.gb), 1, f) != 1) { fclose(f); return false; }

    // Re-patch all function pointers that were serialized as garbage
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
    gb_init_lcd(s.gb, gb_lcd_draw_line);  // re-sets display.lcd_draw_line

    // Restore cart RAM
    if (s.cartRam && s.cartRamSz) {
        if (fread(s.cartRam, 1, s.cartRamSz, f) != s.cartRamSz) { fclose(f); return false; }
    }

    // Restore framebuffer so the last frame shows instantly
    if (fread(g_gb_fb, 1, fbBytes, f) != fbBytes) { fclose(f); return false; }

    // Framebuffer format fixup: reconcile the saved pixel format (fbFlags) with
    // what the current session expects (g_fb_is_prepacked).
    //
    // Case 1: state saved as RGB565/RGB555 (fbFlags=0), session wants RGBA4444
    //   Happens when a CGB-compat game switches from GBC→DMG palette between
    //   sessions, OR a pure DMG game was saved before the prepacked optimisation.
    //   gb_pack_rgb555 works on RGB565 too (loses the lowest green bit — fine
    //   for one frame before draw_line overwrites it).
    if (g_fb_is_prepacked && fbFlags == 0u) {
        for (int i = 0; i < GB_W * GB_H; ++i)
            g_gb_fb[i] = gb_pack_rgb555(g_gb_fb[i]);
    }
    // Case 2: state saved as RGBA4444 (fbFlags=1), session wants RGB565
    //   Happens when a CGB-compat game switches from DMG→GBC palette between
    //   sessions.  A lossy RGBA4444→RGB565 expansion would look wrong, so just
    //   clear — the real frame arrives from the core within 16 ms.
    else if (!g_fb_is_prepacked && fbFlags == 1u) {
        memset(g_gb_fb, 0, GB_W * GB_H * sizeof(uint16_t));
    }

    // Restore full GBAPU snapshot — channels enabled/disabled, timers, duty
    // positions, volume envelopes, LFSR, DC-blocker state, and everything else
    // computed outside gb_s.  Without it every channel starts disabled and the
    // session is silent until the game re-runs its APU init sequence.
    GBAPU apu_snap{};
    if (fread(&apu_snap, sizeof(apu_snap), 1, f) == 1) {
        gb_audio_restore_state(&apu_snap);
        if (apu_restored) *apu_restored = true;
    }

    fclose(f);
    return true;
}

// =============================================================================
// User save-state helpers  (10 named slots per game)
//
// Layout on SD card:
//   Internal (quick-resume):  sdmc:/config/ultragb/states/internal/<name>.state
//   User slots:               sdmc:/config/ultragb/states/<name>/slot_N.state
//                             sdmc:/config/ultragb/states/<name>/slot_N.ts
//
// The .ts file stores a human-readable timestamp written when the slot is saved.
// If absent the slot is considered empty and the footer shows ult::OPTION_SYMBOL.c_str().
// =============================================================================

// Per-game slot directory: <baseDir><gamename_no_ext>/.
// Replaces build_user_state_dir and build_save_backup_dir — same body, differ only in baseDir.
static void build_game_slot_dir(const char* romPath, char* out, size_t outSz,
                                const char* baseDir) {
    build_rom_data_path(romPath, out, outSz, baseDir, "/");
}

// Shared slot-file path builder — avoids 4 near-identical function bodies.
// Writes "<baseDir><gamename_no_ext>/slot_<slot><ext>" into out.
static void build_slot_file_path(const char* romPath, int slot, char* out, size_t outSz,
                                  const char* baseDir, const char* ext) {
    char dir[PATH_BUFFER_SIZE] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), baseDir);
    snprintf(out, outSz, "%sslot_%d%s", dir, slot, ext);
}

// Full path to a user slot state file.
static void build_user_slot_path(const char* romPath, int slot, char* out, size_t outSz) {
    build_slot_file_path(romPath, slot, out, outSz, STATE_BASE_DIR, ".state");
}

// Full path to a user slot timestamp file.
static void build_user_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz) {
    build_slot_file_path(romPath, slot, out, outSz, STATE_BASE_DIR, ".ts");
}

// =============================================================================
// Generic backup timestamp helpers
// Shared by both save-state slots and save-data backup slots so the
// date/time formatting logic lives in exactly one place.
// =============================================================================

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

// Thin wrappers for save-state slots — unchanged call signature.
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

// "Slot 0"–"Slot 9" (6–7 chars) fit within ARM64 libstdc++ SSO (≤15 chars) — no heap alloc.
static std::string make_slot_label(int slot) {
    char buf[16];
    snprintf(buf, sizeof(buf), "Slot %d", slot);
    return buf;
}

// Shared newest-slot scanner — finds the most-recently written .ts file across
// all 10 slots and returns its label, or "" if all slots are empty.
// buildTs is one of build_user_slot_ts_path / build_save_backup_slot_ts_path.
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

// Returns the label ("Slot N") of the most-recently written slot file,
// or "" if all slots are empty.  Uses the .ts file mtime so the comparison
// is byte-exact regardless of the DIVIDER_SYMBOL encoding.
static std::string newest_state_slot_label(const char* romPath) {
    return newest_slot_label(romPath, build_user_slot_ts_path);
}

static void build_save_backup_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz); // forward declare

static std::string newest_save_backup_slot_label(const char* romPath) {
    return newest_slot_label(romPath, build_save_backup_slot_ts_path);
}

// =============================================================================
// Save-data backup helpers  (10 named slots per game)
//
// Layout on SD card:
//   Internal (live .sav):   sdmc:/config/ultragb/saves/internal/<n>.sav
//   Backup slots:           sdmc:/config/ultragb/saves/<n>/slot_N.sav
//                           sdmc:/config/ultragb/saves/<n>/slot_N.ts
//
// Mirrors the save-state slot system exactly; only the base dir and file
// extension differ.  All path-building and timestamp logic delegates to the
// generic helpers above — no duplicated code.
// =============================================================================



// Full path to a save-data backup slot file.
static void build_save_backup_slot_path(const char* romPath, int slot, char* out, size_t outSz) {
    build_slot_file_path(romPath, slot, out, outSz, SAVE_BASE_DIR, ".sav");
}

// Full path to a save-data backup slot timestamp file.
static void build_save_backup_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz) {
    build_slot_file_path(romPath, slot, out, outSz, SAVE_BASE_DIR, ".ts");
}

// Wrappers using the generic timestamp core.
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

// Thin const char* wrappers — avoid heap-allocating a temporary std::string
// at every file-op call site while keeping identical runtime behaviour.
static inline bool file_exists(const char* p) { return ult::isFile(p); }
static inline void copy_file(const char* s, const char* d) { ult::copyFileOrDirectory(s, d); }
static inline void delete_file(const char* p) { ult::deleteFileOrDirectory(p); }

// Back up the live .sav to slot N.  Returns false if the internal .sav is absent
// (game has no save data yet).
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

// Restore slot N into the live .sav file and — if the ROM is currently loaded
// in memory — patch g_gb.cartRam directly so the change takes effect without
// requiring a relaunch.  Returns false if the slot file doesn't exist.
static bool restore_save_data_slot(const char* romPath, int slot) {
    char slotPath[PATH_BUFFER_SIZE] = {};
    build_save_backup_slot_path(romPath, slot, slotPath, sizeof(slotPath));
    if (!file_exists(slotPath)) return false;

    char internalPath[PATH_BUFFER_SIZE] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), g_save_dir, ".sav");

    copy_file(slotPath, internalPath);
    if (!file_exists(internalPath)) return false;

    // Live-patch RAM if this ROM is currently loaded and has cart RAM.
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
// Returns false if the internal state file doesn't exist (game not yet played).
static bool save_user_slot(const char* romPath, int slot) {
    char internalPath[PATH_BUFFER_SIZE] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), STATE_DIR, ".state");
    if (!file_exists(internalPath)) return false;

    // Ensure per-game directory exists before writing
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
// Returns false if the slot file doesn't exist.
// Caller is responsible for launching the game after a successful load.
static bool load_user_slot(const char* romPath, int slot) {
    char slotPath[PATH_BUFFER_SIZE] = {};
    build_user_slot_path(romPath, slot, slotPath, sizeof(slotPath));
    if (!file_exists(slotPath)) return false;

    char internalPath[PATH_BUFFER_SIZE] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), STATE_DIR, ".state");

    copy_file(slotPath, internalPath);
    return file_exists(internalPath);
}



// Reload the wallpaper that was evicted to make room for a large ROM,
// but only if we actually evicted it.  Clears the flag on success.
// Appears verbatim at five sites in gb_load_rom and one in gb_unload_rom.
static void maybe_reload_wallpaper() {
    if (g_wallpaper_evicted) {
        g_wallpaper_evicted = false;
        ult::reloadWallpaper();
    }
}

// Tear down a partially-initialised load: free the just-allocated ROM
// buffer, clear the four path/size fields, and roll back the wallpaper
// eviction.  Called on gb_init failure and on cartRam alloc failure —
// both paths did the identical five-statement sequence.
static void gb_cancel_load() {
    free(g_gb.rom);
    g_gb.rom         = nullptr;
    g_gb.romSize     = 0;
    g_gb.romPath[0]  = '\0';
    g_gb.savePath[0] = '\0';
    free(g_gb.gb);
    g_gb.gb = nullptr;
    free(g_gb_fb);
    g_gb_fb = nullptr;
    maybe_reload_wallpaper();
}

// Attempt a windowed relaunch for romPath.  Writes the path to config.ini,
// calls setNextOverlay, and closes.  Returns false when windowed mode is off.
static bool launch_windowed_mode(const char* romPath) {
    if (!g_windowed_mode || !g_self_path[0]) return false;

    skipRumbleDoubleClick = true;

    ult::launchingOverlay.store(true, std::memory_order_release);
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWindowedRom, romPath, "");
    // Persist the current settings-page scroll position so it survives the
    // overlay process restart.  The re-launched overlay reads and erases this
    // transient key in initServices() only when -returning is detected, so a
    // cold launch or abnormal exit never accidentally restores a stale value.
    if (g_settings_scroll[0])
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll,
                             g_settings_scroll, "");
    tsl::setNextOverlay(g_self_path, g_directMode ? "-quicklaunch" : "-windowed");
    if (g_directMode) {
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "1", "");
    }
    tsl::Overlay::get()->close();
    return true;
}

// =============================================================================
// launch_overlay_mode — relaunch this overlay with -overlay <romPath>
//
// Mirrors launch_windowed_mode() exactly.  Instead of in-process swapTo<GBOverlayGui>
// (which accumulates menu heap / glyph state), we restart the whole overlay so
// GBOverlayGui always starts in a pristine process with no residual UI state.
//
// Direct-mode callers (g_directMode) relaunch with -quicklaunch so the new
// process preserves the "combo closes, no ROM selector return" behaviour.
// Normal callers relaunch with -overlay; pressing launch combo inside the emulator then
// relaunches with -returning so the ROM selector opens cleanly.
//
// If g_self_path is unavailable (shouldn't happen), the call is silently
// ignored — the config key is still written so the next normal launch picks
// up the ROM as last_rom and the user can reopen from the selector.
// =============================================================================
static void launch_overlay_mode(const char* romPath) {
    if (!g_self_path[0]) return;  // no self path — can't relaunch; caller ignores

    
    ult::launchingOverlay.store(true, std::memory_order_release);

    // Persist the ROM path so the relaunched process can read it.
    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyPlayerRom, romPath, "");

    // Persist the current settings-page scroll position so it survives the
    // overlay process restart.  The re-launched overlay reads and erases this
    // transient key in initServices() via the existing -returning path.
    if (g_settings_scroll[0])
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll,
                             g_settings_scroll, "");

    // In direct mode relaunch as -quicklaunch (combo closes, no selector return).
    // Otherwise relaunch as -overlay (X returns to selector via -returning).
    tsl::setNextOverlay(g_self_path, g_directMode ? "-quicklaunch --direct" : "-overlay");
    if (g_directMode)
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "1", "");

    skipRumbleDoubleClick = true;
    tsl::Overlay::get()->close();
}

// Exit UI audio, mark last-played, then launch in windowed or overlay mode.
// Replaces the identical 4-statement sequence at three call sites:
//   SlotActionGui::Load, GameConfigGui::Reset, RomSelectorGui click.
// Returns true unconditionally so callers can write: return launch_game(path);
[[gnu::noinline]]
static bool launch_game(const char* romPath) {
    audio_exit_if_enabled();
    save_last_rom(romPath);
    if (launch_windowed_mode(romPath)) return true;
    launch_overlay_mode(romPath);
    return true;
}

// ROM size thresholds — shared by gb_load_rom, rom_is_playable, GameConfigGui.
static constexpr size_t kROM_2MB = 2u << 20;
static constexpr size_t kROM_4MB = 4u << 20;
static constexpr size_t kROM_6MB = 6u << 20;

// =============================================================================
// gb_load_rom — if same ROM is already in memory, just resume
// =============================================================================
bool gb_load_rom(const char* path) {
    if (g_gb.rom && strncmp(g_gb.romPath, path, sizeof(g_gb.romPath)) == 0) {
        g_gb_frame_next_ns = 0;  // re-anchor clock on resume
        g_gb.running = true;
        return true;
    }

    // ── Size-check the new ROM before touching the current one ──────────────
    // We must know the size before unloading so we can reject oversized ROMs
    // without losing the currently running game.
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const size_t sz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    if (!sz) { fclose(f); return false; }

    // ── Heap-tier capability check ────────────────────────────────────────────
    // Memory tiers (set by tesla.hpp at startup):
    //   limitedMemory        = 4 MB heap  → max ROM 1 MB
    //   (neither)            = 6 MB heap  → max ROM 2 MB (can't fit 4 MB ROMs)
    //   expandedMemory       = 8 MB heap  → max ROM 4 MB (wallpaper evicted below)
    //   furtherExpandedMemory= >8 MB heap → max ROM 6 MB
    //
    // Notify the user with an actionable message before touching any state.
    if (ult::limitedMemory && sz >= kROM_2MB && sz < kROM_4MB) [[unlikely]] {
        show_notify(REQUIRES_AT_LEAST_6MB);
        fclose(f); return false;
    }
    // Default (6 MB heap): reject ROMs > 2 MB.
    if (!ult::expandedMemory && sz >= kROM_4MB && sz < kROM_6MB) [[unlikely]] {
        show_notify(REQUIRES_AT_LEAST_8MB);
        fclose(f); return false;
    }
    if (!ult::furtherExpandedMemory && sz >= kROM_6MB) [[unlikely]] {
        show_notify(REQUIRES_AT_LEAST_10MB);
        fclose(f); return false;
    }

    // ── Memory management: evict wallpaper FIRST, then unload old ROM ────────
    //
    // Critical ordering: the wallpaper (~630 KB) must be freed BEFORE we free
    // the old ROM buffer, so that by the time malloc(new ROM) runs both slabs
    // are gone and the allocator sees the maximum possible contiguous free
    // region.  Freeing old ROM first and wallpaper second leaves the two freed
    // regions separated by whatever was between them, risking a failed
    // malloc(4 MB) even with enough total free bytes.
    //
    // Memory tiers and wallpaper eviction:
    //   4 MB heap  (limitedMemory)         — max 2 MB ROM, no wallpaper anyway
    //   6 MB heap  (neither flag)          — max 2 MB ROM, wallpaper safe with 2 MB ROM
    //   8 MB heap  (expandedMemory)        — max 4 MB ROM, wallpaper evicted for large ROMs; ghosting disabled for 4 MB+ ROMs
    //   10 MB+ heap (furtherExpandedMemory)— max 6 MB ROM, wallpaper safe at all sizes

    static constexpr size_t WALLPAPER_EVICT_THRESHOLD = kROM_2MB;
    const bool need_evict = (sz > WALLPAPER_EVICT_THRESHOLD && !ult::furtherExpandedMemory && ult::expandedMemory);

    if (g_gb.rom && g_gb.running) {
        g_gb.running   = false;
        g_emu_active   = false;
        g_fast_forward = false;   // clear FF so it never bleeds into the next ROM
        gb_audio_shutdown();   // ← free DMA buffers BEFORE clearCache
    }

    // Step 1 — Before evicting the wallpaper or freeing the old ROM, flush the
    // glyph cache.  This is the key step that prevents intermittent malloc(4 MB)
    // failures on the 8 MB heap tier.
    //
    // Why fragmentation happens without this:
    //   After loading and unloading a 2 MB game the freed ROM block is returned
    //   to the allocator.  The menu then renders its ROM-selector list, filling
    //   that hole with scattered small glyph allocations.  When the user next
    //   picks a 4 MB game we evict the wallpaper (630 KB) and free the old ROM
    //   buffer — but those glyph allocs sit *between* the two freed regions,
    //   preventing the allocator from coalescing them.  The largest contiguous
    //   free block ends up being ~2 MB, not 4 MB, and malloc fails.
    //
    // Flushing the glyph cache first releases all those scattered small allocs
    // before we do anything else.  The freed glyph memory, freed wallpaper, and
    // freed old ROM all collapse into one large contiguous region that malloc
    // can satisfy.  The cache repopulates automatically when the user returns to
    // the ROM selector.
    //
    // We call FontManager::clearCache() directly rather than setting
    // clearGlyphCacheNow, because that atomic is consumed by endFrame() which
    // runs on the same thread and won't execute before our malloc below.
    tsl::gfx::FontManager::clearCache(); // ALWAYS CLEAR BEFORE

    // Step 2 — Evict wallpaper before touching any other allocations.
    // The swap trick guarantees deallocation (shrink_to_fit is non-binding).
    if (need_evict && !ult::wallpaperData.empty()) {
        ult::refreshWallpaper.store(true, std::memory_order_release);
        { std::vector<u8> tmp; tmp.swap(ult::wallpaperData); }
    }

    // Step 3 — Unload the old ROM.
    // Suppress the wallpaper reload inside gb_unload_rom when the new ROM is
    // also large: we just evicted the wallpaper and don't want it reloaded only
    // to be immediately evicted again (reload→evict churn adds one extra malloc/
    // free cycle on an already-tight heap).  When the new ROM is small we leave
    // g_wallpaper_evicted alone so gb_unload_rom naturally reloads the wallpaper.
    if (need_evict) g_wallpaper_evicted = false;  // suppress reload inside unload
    gb_unload_rom();   // frees old ROM buffer + cartRam, saves state, audio shutdown
    // Set the eviction flag for the new session so gb_unload_rom reloads on exit.
    // If !need_evict and prev game was large, gb_unload_rom already reloaded.
    g_wallpaper_evicted = need_evict;

    // Step 3.5 — Pre-allocate DMA buffers BEFORE the large ROM malloc.
    //
    // aligned_alloc(0x1000, 0x1000) needs a contiguous free block of ~8191
    // bytes to satisfy its alignment contract.  On a 4 MB heap, after
    // malloc(sz) claims up to ~2 MB the heap may have no such block left,
    // even with plenty of total free bytes — causing gb_audio_init (called
    // later) to fail with an invisible OOM that leaves audio dead and the
    // emulator in a broken state.
    //
    // Calling gb_audio_preinit_dma() here, while the heap is clean (glyph
    // cache flushed, old ROM freed, wallpaper freed), guarantees the four
    // 4 KB aligned buffers land in unfragmented space.  gb_audio_init()
    // detects non-null dma[] pointers and skips re-allocation.
    if (!gb_audio_preinit_dma()) {
        fclose(f);
        show_notify(NOT_ENOUGH_MEMORY_WARNING);
        maybe_reload_wallpaper();
        return false;
    }

    // Step 4 — Allocate the new ROM buffer.
    // At this point: glyph cache flushed, wallpaper freed (if need_evict), old
    // ROM freed, old cartRam freed, DMA buffers pre-allocated.
    uint8_t* rom_buf = static_cast<uint8_t*>(malloc(sz));
    if (!rom_buf) {
        fclose(f);
        show_notify(NOT_ENOUGH_MEMORY_WARNING);
        maybe_reload_wallpaper();
        return false;
    }
    if (fread(rom_buf, 1, sz, f) != sz) {
        free(rom_buf);
        fclose(f);
        maybe_reload_wallpaper();
        return false;
    }
    fclose(f);

    g_gb.rom     = rom_buf;
    g_gb.romSize = sz;
    strncpy(g_gb.romPath, path, sizeof(g_gb.romPath)-1);
    build_rom_data_path(path, g_gb.savePath, sizeof(g_gb.savePath), g_save_dir, ".sav");

    // Allocate the core emulator struct — contains WRAM (32 KB) + VRAM (16 KB) + OAM/HRAM.
    // Lives on the heap so these ~49 KB are only resident during active gameplay.
    g_gb.gb = static_cast<struct gb_s*>(calloc(1, sizeof(struct gb_s)));
    if (!g_gb.gb) {
        gb_cancel_load();
        show_notify(NOT_ENOUGH_MEMORY_WARNING);
        return false;
    }

    const enum gb_init_error_e err =
        gb_init(g_gb.gb,
                gb_rom_read, gb_rom_read16, gb_rom_read32,
                gb_cart_ram_read, gb_cart_ram_write,
                gb_error, nullptr);
    if (err != GB_INIT_NO_ERROR) {
        // g_gb.rom is the freshly-allocated rom_buf — free it now.
        gb_cancel_load();
        return false;
    }

    // Detect whether this is a CGB ROM so the renderer uses the right converter.
    // cgbMode is set by gb_init() when it reads byte 0x143 of the ROM header.
#if WALNUT_FULL_GBC_SUPPORT
    g_fb_is_rgb565 = (g_gb.gb->cgb.cgbMode != 0);
#else
    g_fb_is_rgb565 = false;
#endif

    // Any CGB game (0x80 CGB-compat OR 0xC0 CGB-only) may use a user-selected
    // DMG palette.  We intentionally leave cgbMode=1 so the game's full GBC
    // init path runs correctly — double speed, VRAM banking, CGB palettes, and
    // all other CGB hardware continue to work normally inside the core.
    //
    // Instead of downgrading cgbMode, we simply redirect the display output:
    //   g_fb_is_rgb565 = false   → renderer treats g_gb_fb as RGBA4444 (prepacked)
    //   g_fb_is_prepacked = true → gb_lcd_draw_line reads fixPalette[px] to get
    //                              the true RGB565 color, computes luminance, and
    //                              maps to shade 0–3 through g_dmg_flat_pal.
    //
    // This produces a correct monochrome look — no inverted glyphs, no wrong
    // colors — while keeping the emulator core and save states fully intact.
    // cgbMode is still 1 in any saved state, which is exactly right on reload.
#if WALNUT_FULL_GBC_SUPPORT
    if (g_fb_is_rgb565 && rom_has_cgb_flag(path)) {
        const PaletteMode savedMode = load_game_palette_mode(path);
        if (savedMode != PaletteMode::GBC) {
            g_fb_is_rgb565 = false;   // renderer uses prepacked (RGBA4444) path
            // g_fb_is_prepacked is set from g_fb_is_rgb565 on the next line
        }
    }
#endif

    // DMG games use the pre-packed path: gb_select_dmg_palette bakes RGBA4444
    // values into g_dmg_flat_pal so gb_lcd_draw_line writes display-ready pixels
    // directly into g_gb_fb, eliminating the per-run conversion in the renderer.
    // CGB games always use fixPalette (RGB565), so the CGB path is never prepacked.
    g_fb_is_prepacked = !g_fb_is_rgb565;
    // Apply per-game palette for DMG games and CGB-compat games in DMG-palette
    // mode.  For CGB-only games (cgbMode=1 and g_fb_is_rgb565=true), the palette
    // is always GBC — hardware colour, no user palette applies.
    // We check g_gb.gb->cgb.cgbMode directly rather than g_fb_is_rgb565 because
    // CGB-compat games in DMG-palette mode have cgbMode=1 but g_fb_is_rgb565=false.
#if WALNUT_FULL_GBC_SUPPORT
    g_palette_mode = (g_gb.gb->cgb.cgbMode && g_fb_is_rgb565)
                     ? PaletteMode::GBC
                     : load_game_palette_mode(path);
#else
    g_palette_mode = load_game_palette_mode(path);
#endif
    // Compute title-checksum lookup and populate g_dmg_*_pal for the draw callback.
    // For CGB games this is a no-op in the renderer (fixPalette path is used instead),
    // but calling it is harmless and keeps g_gbc_pal_found/idx accurate for the UI.
    gb_select_dmg_palette();

    // Apply per-game LCD ghosting preference (off by default).
    // Ghosting is safe only when there is enough headroom for the extra frame buffer:
    //   • furtherExpandedMemory (10 MB+ heap), any ROM size, OR
    //   • expandedMemory only (8 MB heap) with a ROM < 4 MB.
    // Every other tier — limitedMemory (4 MB), base 6 MB heap, or 8 MB with a
    // 4 MB+ ROM — forces ghosting off regardless of the per-game config.
    // Ghosting requires enough heap headroom for the extra frame buffer.
    // The minimum tier depends on ROM size:
    //   ROM < 2 MB  → needs 6 MB heap (locked only on 4 MB)
    //   ROM 2–4 MB  → needs 8 MB heap (locked on 4 MB and 6 MB)
    //   ROM >= 4 MB → needs 10 MB heap (locked on 4 MB, 6 MB, and 8 MB)
    const bool ghostingHardLocked =
        ult::limitedMemory ||
        (!ult::expandedMemory && sz >= kROM_2MB) ||
        (ult::expandedMemory && !ult::furtherExpandedMemory && sz >= kROM_4MB);
    g_lcd_ghosting = ghostingHardLocked ? false : load_game_lcd_ghosting(path);

    size_t ramSz = 0;
    gb_get_save_size_s(g_gb.gb, &ramSz);
    // MBC3 cartridges (especially Zelda Oracle / Pokémon Gold/Silver) can have
    // bad ROM headers that declare only 8KB of SRAM when the game actually uses
    // 4 banks × 8KB = 32KB.  If the game writes to banks 1–3 and gets 0xFF back
    // it will enter a corrupted state during the intro save-RAM initialisation.
    // Clamp MBC3 to a minimum of 32KB so all four banks are always available.
    if (g_gb.gb->mbc == 3 && ramSz > 0 && ramSz < 0x8000)
        ramSz = 0x8000;
    if (ramSz) {
        g_gb.cartRam = static_cast<uint8_t*>(calloc(ramSz, 1));
        if (!g_gb.cartRam) {
            // Can't allocate cart RAM — running without it would silently corrupt
            // any game that uses SRAM. Clean up and fail.
            gb_cancel_load();
            return false;
        }
        g_gb.cartRamSz = ramSz;
        load_save(g_gb);
    }

    gb_init_lcd(g_gb.gb, gb_lcd_draw_line);
    // Apply per-game no-sprite-limit preference (on by default).
    // gb_init_lcd sets no_sprite_limit=true; override here for games that
    // have the hardware cap re-enabled in their config.
    g_gb.gb->direct.no_sprite_limit = load_game_no_sprite_limit(path);

    // Allocate the framebuffer — ~45 KB heap, only resident during active gameplay.
    g_gb_fb = static_cast<uint16_t*>(calloc(GB_W * GB_H, sizeof(uint16_t)));
    if (!g_gb_fb) {
        gb_cancel_load();
        show_notify(NOT_ENOUGH_MEMORY_WARNING);
        return false;
    }

    // Clear the framebuffer BEFORE gb_reset() — peanut_gb fires gb_lcd_draw_line
    // for line 0 during reset itself.  If memset runs after, that first draw is
    // erased and row 0 stays black until the second frame completes.
    memset(g_gb_fb, 0, GB_W * GB_H * sizeof(uint16_t));

    // ── Attempt to restore a previously saved state ───────────────────────────
    // save_state() is called from gb_unload_rom() so every clean unload leaves a
    // .state file.  If it loads successfully the CPU/memory/framebuffer are
    // restored exactly where the overlay left off.  On failure (no file, wrong
    // version, truncated) we fall through to a normal cold boot — no harm done.
    bool apu_restored = false;
    const bool resumed = load_state(g_gb, &apu_restored);
    // load_state calls gb_init_lcd internally to re-patch function pointers,
    // which resets no_sprite_limit to true.  Re-apply the per-game preference.
    g_gb.gb->direct.no_sprite_limit = load_game_no_sprite_limit(path);

    if (!apu_restored) {
        // No v3 APU snapshot available (cold boot, v1 file, or read error).
        // Reset the register shadow and invalidate the snapshot so the audio
        // thread starts fresh.  NR52 is primed to 0x80 (power ON) so the
        // thread's apply_reg() gate accepts channel writes immediately — without
        // this prime a mid-game resume would stay silent because the game never
        // re-runs its APU init sequence.
        gb_audio_reset_regs();
    }

    // ── Init audio BEFORE gb_reset() so the APU callback is live from frame 1 ──
    gb_audio_init(g_gb.gb);
    gb_audio_set_gb_ptr(g_gb.gb);  // give audio_write() cycle-accurate LY+lcd_count offsets

    // If load_state succeeded, gb_reset() must NOT be called — it would wipe
    // the restored CPU state.  If cold-booting, gb_reset() is already called
    // implicitly by gb_init() above, so the commented-out call below stays out.
    //gb_reset(g_gb.gb);
    (void)resumed;

    g_gb_frame_next_ns = 0;  // anchor clock on first draw()
    reset_lcd_ghosting();    // don't bleed prev-game pixels into first frame
    g_gb.running = true;
    return true;
}

void gb_unload_rom() {
    if (!g_gb.rom) return;

    g_gb.running = false;
    g_emu_active = false;

    gb_audio_shutdown();   // drain audout queue, stop thread.  DMA buffers stay live.

    free_lcd_ghosting();

    // Free the ROM buffer BEFORE save_state so the ~1 MB slab is released while
    // save_state writes to disk.  save_state() only reads gb_s (BSS), cartRam,
    // and g_gb_fb — none of which depend on the ROM buffer being live.
    // On a 4 MB heap this eliminates the peak where ROM + cartRam + FILE buffer
    // were all simultaneously allocated, which was causing intermittent malloc
    // failures when loading the next game.
    free(g_gb.rom);
    g_gb.rom     = nullptr;
    g_gb.romSize = 0;

    // Persist state on every unload, not just overlay exit.
    // This covers the game-switch path (X -> pick different ROM) which previously
    // lost the current game's progress silently.
    save_state(g_gb);

    write_save(g_gb);
    if (g_gb.cartRam) { free(g_gb.cartRam); g_gb.cartRam = nullptr; }
    g_gb.cartRamSz   = 0;

    // Free the core emulator struct and framebuffer — these are now demand-allocated
    // so they only occupy memory during active gameplay (~49 KB WRAM+VRAM + ~45 KB FB).
    free(g_gb.gb);  g_gb.gb  = nullptr;
    free(g_gb_fb);  g_gb_fb  = nullptr;

    g_gb.romPath[0]  = '\0';
    g_gb.savePath[0] = '\0';

    // ── Wallpaper reload ──────────────────────────────────────────────────────
    // Only reload if we actually evicted the wallpaper for this ROM.
    // reloadWallpaper() waits for inPlot=false, repopulates wallpaperData, and
    // clears refreshWallpaper — the full safe sequence.
    maybe_reload_wallpaper();
}

// =============================================================================
// get_rom_size — shared fopen/fseek/ftell/fclose sequence.
//
// Returns the byte size of the file at path, or 0 if the file cannot be opened
// or is empty.  Extracted to avoid duplicating the identical 4-line sequence
// in both rom_is_playable() and rom_playability_message().
// =============================================================================
static size_t get_rom_size(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    const size_t sz = static_cast<size_t>(ftell(f));
    fclose(f);
    return sz;
}

// =============================================================================
// rom_is_playable — size-tier check used by the ROM selector
//
// Returns true when the ROM at |path| can be loaded on the current memory tier.
// Mirrors the rejection conditions in gb_load_rom() so the selector can dim
// unplayable entries BEFORE the user tries to launch them.
// =============================================================================
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
// Mirrors the rejection conditions in gb_load_rom() exactly.
// Used by quick-launch paths to bail out before touching any overlay state.
static const char* rom_playability_message(const char* path) {
    const size_t sz = get_rom_size(path);
    if (!sz) return nullptr;  // can't size it — let it fail naturally later
    // Each check is bounded to its exact tier range so the correct message is
    // returned regardless of ROM size.  Mirrors gb_load_rom() exactly:
    //   4 MB heap  (limitedMemory)           — 2 MB+ ROMs blocked; 4 MB+ fall through
    //   6 MB heap  (!expandedMemory)         — 4 MB+ ROMs blocked; 6 MB+ fall through
    //   10 MB+ heap (furtherExpandedMemory)  — all ROMs pass
    if ( ult::limitedMemory          && sz >= kROM_2MB && sz < kROM_4MB) return REQUIRES_AT_LEAST_6MB;
    if (!ult::expandedMemory         && sz >= kROM_4MB && sz < kROM_6MB) return REQUIRES_AT_LEAST_8MB;
    if (!ult::furtherExpandedMemory  && sz >= kROM_6MB)                  return REQUIRES_AT_LEAST_10MB;
    return nullptr;  // playable
}

// =============================================================================
// gb_set_input — KEY_* macros (old libnx style used by this libtesla fork)
// GB joypad active-low: 0 = pressed
//   bit 0=A  1=B  2=Select  3=Start  4=Right  5=Left  6=Up  7=Down
// =============================================================================
void gb_set_input(u64 keysHeld) {
    if (!g_gb.running) return;
    uint8_t joy = 0xFF;
    if (keysHeld & KEY_A)     joy &= ~(1u << 0);
    if (keysHeld & KEY_B)     joy &= ~(1u << 1);
    if ((keysHeld & KEY_MINUS) || (keysHeld & KEY_Y)) joy &= ~(1u << 2);
    if ((keysHeld & KEY_PLUS)  || (keysHeld & KEY_X)) joy &= ~(1u << 3);
    if (keysHeld & KEY_RIGHT) joy &= ~(1u << 4);
    if (keysHeld & KEY_LEFT)  joy &= ~(1u << 5);
    if (keysHeld & KEY_UP)    joy &= ~(1u << 6);
    if (keysHeld & KEY_DOWN)  joy &= ~(1u << 7);
    g_gb.gb->direct.joypad = joy;
}

// =============================================================================
// ROM scanner
// =============================================================================
static bool is_gb_rom(const char* name) {
    const size_t len = strlen(name);
    if (len >= 4) {
        const char* e = name + len - 4;
        if (e[0]=='.' && tolower((uint8_t)e[1])=='g' &&
            tolower((uint8_t)e[2])=='b' && tolower((uint8_t)e[3])=='c') return true;
    }
    if (len >= 3) {
        const char* e = name + len - 3;
        if (e[0]=='.' && tolower((uint8_t)e[1])=='g' && tolower((uint8_t)e[2])=='b') return true;
    }
    return false;
}
// scan_roms_scandir — filter function for scandir(); accepts .gb / .gbc files only.
static int gb_rom_filter(const struct dirent* e) {
    return is_gb_rom(e->d_name) ? 1 : 0;
}

// draw_ultragb_title — shared animated title renderer.
// Mirrors the launcher pattern exactly:
//   Dynamic: "Ultra" = wave between dynamicLogoRGB2 and dynamicLogoRGB1
//            "GB"   = logoColor2 (fixed accent, same as "hand")
//   Static:  "Ultra" = logoColor1 (white)
//            "GB"   = logoColor2 (fixed accent)
// Uses a stack char[2] buffer per letter to avoid per-call heap allocation.
static s32 draw_ultragb_title(tsl::gfx::Renderer* renderer,
                                const s32 x, const s32 y, const u32 fontSize, bool quickModeSymbol = false) {
    static constexpr double CYCLE  = 1.6;
    static constexpr double WSCALE = 2.0 * ult::_M_PI / CYCLE;
    static constexpr double PSHIFT = ult::_M_PI / 2.0;

    // Stack buffer: avoids std::string heap allocation per letter per frame
    char buf[2] = {0, 0};
    s32 cx = x;

    if (ult::useDynamicLogo) {
        // Compute time base once outside the letter loop
        const float t = std::fmodf(static_cast<float>(ult::nowNs()) / 1e9f, CYCLE);
        float offset = 0.f;
        for (const char ch : ult::SPLIT_PROJECT_NAME_1) {
            const float p = WSCALE * (t + offset);
            const float rp = (ult::cos(p - PSHIFT) + 1.f) * 0.5f;
            const double s1 = rp * rp * (3.0 - 2.0 * rp);
            const double bl = s1 * s1 * (3.0 - 2.0 * s1);
            const tsl::Color col = lerpColor(tsl::dynamicLogoRGB2, tsl::dynamicLogoRGB1,
                                             std::max(0.0, std::min(1.0, bl)));
            buf[0] = ch;
            cx += renderer->drawString(buf, false, cx, y, fontSize, col).first;
            offset -= static_cast<float>(CYCLE / 8.0);
        }
    } else {
        cx += renderer->drawString(ult::SPLIT_PROJECT_NAME_1, false, cx, y, fontSize, tsl::logoColor1).first;
    }
    cx += renderer->drawString("GB",  false, cx, y, fontSize, tsl::logoColor2).first;
    if (quickModeSymbol && g_directMode) {
        static const auto directModeColor = tsl::RGB888("6ea0f0");
        cx += renderer->drawString("\uE08E",  false, cx+5, y-2, fontSize-8, directModeColor).first;
    }
    return cx;
}

// In-game overlay — GBOverlayElement / GBOverlayGui.
// Included here so it has access to draw_ultragb_title and all globals and
// helpers defined above.  Must come before class Overlay below.
// =============================================================================
#include "gb_overlay.hpp"

class RomSelectorGui;       // forward declare
class GameConfigGui;        // forward declare
class SettingsGui;          // forward declare
class SaveStatesGui;        // forward declare
class SlotActionGui;        // forward declare
class SaveDataGui;          // forward declare
class SaveDataSlotActionGui; // forward declare
class QuickComboSelectorGui; // forward declare

// make_slot_detail_header — moved to gb_utils.hpp

// Deferred normal ROM launch: relaunch this overlay with -overlay so GBOverlayGui
// always starts in a fresh process (no residual menu heap / glyph state).
// All three call sites (RomSelectorGui, SlotActionGui, GameConfigGui) use this
// single entry point — no swapTo<GBOverlayGui> anywhere in normal flow.
//static void launch_emulator(const char* romPath) {
//    // Do NOT clear g_settings_scroll here — the user expects to land back on the
//    // same Settings item after X-ing out of the emulator and returning to Settings.
//    launch_overlay_mode(romPath);
//    // tsl::swapTo<GBOverlayGui>(); — replaced by overlay relaunch above.
//}

// =============================================================================
// SlotActionGui — Save / Load for a single user save-state slot
//
// Opened by clicking a slot in SaveStatesGui.
// Save  — copies internal quick-resume state → slot file + writes timestamp.
// Load  — copies slot file → internal state, then cold-boots into the game
//         (gb_load_rom will find the state and resume from it).
// B     — returns to SaveStatesGui, jumping back to the clicked slot item.
// =============================================================================
class SlotActionGui : public tsl::Gui {
    std::string m_rom_path;
    std::string m_display_name;
    int         m_slot;
public:
    SlotActionGui(std::string romPath, std::string displayName, int slot)
        : m_rom_path(std::move(romPath))
        , m_display_name(std::move(displayName))
        , m_slot(slot)
    {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();

        // Slot sub-header
        list->addItem(new tsl::elm::CategoryHeader(make_slot_detail_header(m_slot, m_display_name)));

        const std::string& romPath = m_rom_path;
        const std::string& displayName = m_display_name;
        const int slot = m_slot;

        // ── Save ──────────────────────────────────────────────────────────────
        auto* saveItem = new tsl::elm::SilentListItem("Save");
        saveItem->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            if (save_user_slot(romPath.c_str(), slot)) {
                triggerEnterFeedback();
                // Return to SaveStatesGui, jumping back to the correct slot item
                //triggerEnterFeedback();
                triggerRumbleClick.store(true, std::memory_order_release);
                tsl::swapTo<SaveStatesGui>(romPath, displayName, make_slot_label(slot));
                show_notify("State saved.");
            } else {
                triggerWallFeedback();
                show_notify("No state to save yet.");
            }
            return true;
        });
        list->addItem(saveItem);

        // ── Load ──────────────────────────────────────────────────────────────
        auto* loadItem = new tsl::elm::SilentListItem("Load");
        loadItem->setClickListener([romPath, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            // Check if slot file exists first
            char slotPath[PATH_BUFFER_SIZE] = {};
            build_user_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)) {
                triggerWallFeedback();
                show_notify(SLOT_IS_EMPTY_WARNING);
                return true;
            }
            // Copy slot → internal state
            if (!load_user_slot(romPath.c_str(), slot)) {
                triggerWallFeedback();
                show_notify("Load failed.");
                return true;
            }
            // Launch the game — gb_load_rom will find the internal state and resume
            return launch_game(romPath.c_str());
        });
        list->addItem(loadItem);

        // ── Delete ────────────────────────────────────────────────────────────
        auto* deleteItem = new tsl::elm::SilentListItem("Delete");
        deleteItem->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            // Check if the slot file exists first
            char slotPath[PATH_BUFFER_SIZE] = {};
            build_user_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)) {
                triggerWallFeedback();
                show_notify(SLOT_IS_EMPTY_WARNING);
                return true;
            }
            // Delete the state file and its timestamp
            delete_file(slotPath);
            char tsPath[PATH_BUFFER_SIZE] = {};
            build_user_slot_ts_path(romPath.c_str(), slot, tsPath, sizeof(tsPath));
            delete_file(tsPath);
            
            // Return to SaveStatesGui, scrolling back to this slot
            tsl::swapTo<SaveStatesGui>(romPath, displayName, make_slot_label(slot));
            triggerFeedbackImpl(triggerRumbleClick, triggerMoveSound);
            show_notify("Slot deleted.");
            return true;
        });
        list->addItem(deleteItem);

        return make_bare_frame(list);
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            tsl::swapTo<SaveStatesGui>(m_rom_path, m_display_name, make_slot_label(m_slot));
            return true;
        }
        return false;
    }
};

// =============================================================================
// SaveDataSlotActionGui — Backup / Restore / Delete for one save-data slot
//
// Mirrors SlotActionGui exactly but operates on .sav files instead of .state
// files.  "Backup" copies the live internal .sav to the slot; "Restore" does
// the reverse (and live-patches g_gb.cartRam if the game is running);
// "Delete" removes the slot .sav and its .ts.
// =============================================================================
class SaveDataSlotActionGui : public tsl::Gui {
    std::string m_rom_path;
    std::string m_display_name;
    int         m_slot;
public:
    SaveDataSlotActionGui(std::string romPath, std::string displayName, int slot)
        : m_rom_path(std::move(romPath))
        , m_display_name(std::move(displayName))
        , m_slot(slot)
    {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader(make_slot_detail_header(m_slot, m_display_name)));

        const std::string& romPath     = m_rom_path;
        const std::string& displayName = m_display_name;
        const int slot = m_slot;

        // ── Backup ────────────────────────────────────────────────────────────
        auto* backupItem = new tsl::elm::SilentListItem("Backup");
        backupItem->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            if (backup_save_data_slot(romPath.c_str(), slot)) {
                triggerEnterFeedback();
                show_notify("Save data backed up.");
                tsl::swapTo<SaveDataGui>(romPath, displayName, make_slot_label(slot));
            } else {
                triggerWallFeedback();
                show_notify("No save data found.");
            }
            return true;
        });
        list->addItem(backupItem);

        // ── Restore ───────────────────────────────────────────────────────────
        auto* restoreItem = new tsl::elm::SilentListItem("Restore");
        restoreItem->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            // Verify the slot exists first.
            char slotPath[PATH_BUFFER_SIZE] = {};
            build_save_backup_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)) {
                triggerWallFeedback();
                show_notify(SLOT_IS_EMPTY_WARNING);
                return true;
            }
            if (!restore_save_data_slot(romPath.c_str(), slot)) {
                triggerWallFeedback();
                show_notify("Restore failed.");
                return true;
            }
            
            tsl::swapTo<SaveDataGui>(romPath, displayName, make_slot_label(slot));
            triggerEnterFeedback();
            show_notify("Save data restored.");
            return true;
        });
        list->addItem(restoreItem);

        // ── Delete ────────────────────────────────────────────────────────────
        auto* deleteItem = new tsl::elm::SilentListItem("Delete");
        deleteItem->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            char slotPath[PATH_BUFFER_SIZE] = {};
            build_save_backup_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)){
                show_notify(SLOT_IS_EMPTY_WARNING);
                triggerWallFeedback();
                return true;
            }
            delete_file(slotPath);
            char tsPath[PATH_BUFFER_SIZE] = {};
            build_save_backup_slot_ts_path(romPath.c_str(), slot, tsPath, sizeof(tsPath));
            delete_file(tsPath);
            
            tsl::swapTo<SaveDataGui>(romPath, displayName, make_slot_label(slot));
            triggerFeedbackImpl(triggerRumbleClick, triggerMoveSound);
            show_notify("Slot deleted.");
            return true;
        });
        list->addItem(deleteItem);

        return make_bare_frame(list);
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            tsl::swapTo<SaveDataGui>(m_rom_path, m_display_name, make_slot_label(m_slot));
            return true;
        }
        return false;
    }
};

// =============================================================================
// SaveDataGui — 10 save-data backup slots
//
// Each slot shows a timestamp footer (ult::OPTION_SYMBOL if empty).
// Clicking a slot opens SaveDataSlotActionGui.
// B returns to GameConfigGui, jumping to "Save Data".
// =============================================================================
class SaveDataGui : public tsl::Gui {
    std::string m_rom_path;
    std::string m_display_name;
    std::string m_jump_to;
public:
    SaveDataGui(std::string romPath, std::string displayName, std::string jumpTo = "")
        : m_rom_path(std::move(romPath))
        , m_display_name(std::move(displayName))
        , m_jump_to(std::move(jumpTo))
    {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Save Data " + ult::DIVIDER_SYMBOL + " " + m_display_name));

        const std::string romPath     = m_rom_path;
        const std::string displayName = m_display_name;

        for (int i = 0; i < 10; ++i) {
            char ts[48] = {};
            read_save_backup_timestamp(romPath.c_str(), i, ts, sizeof(ts));

            const int slot = i;
            auto* item = new tsl::elm::MiniListItem(make_slot_label(i), ts);
            item->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                tsl::swapTo<SaveDataSlotActionGui>(romPath, displayName, slot);
                return true;
            });
            list->addItem(item);
        }

        // Jump to explicit target (returning from SaveDataSlotActionGui) or,
        // on first entry, to the most recently written backup slot.
        const std::string jumpTarget = !m_jump_to.empty()
            ? m_jump_to
            : newest_save_backup_slot_label(m_rom_path.c_str());
        if (!jumpTarget.empty())
            list->jumpToItem(jumpTarget, "", true);

        return make_bare_frame(list);
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            tsl::swapTo<GameConfigGui>(m_rom_path, m_display_name, "Save Data");
            return true;
        }
        return false;
    }
};

// =============================================================================
// SaveStatesGui — 10 user save-state slots
//
// Each slot shows a timestamp footer (ult::OPTION_SYMBOL.c_str() if empty).
// Clicking a slot opens SlotActionGui.
// B returns to GameConfigGui, jumping to "Save States".
// =============================================================================
class SaveStatesGui : public tsl::Gui {
    std::string m_rom_path;
    std::string m_display_name;
    std::string m_jump_to;   // item label to restore scroll position on re-entry
public:
    SaveStatesGui(std::string romPath, std::string displayName, std::string jumpTo = "")
        : m_rom_path(std::move(romPath))
        , m_display_name(std::move(displayName))
        , m_jump_to(std::move(jumpTo))
    {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Save States "+ ult::DIVIDER_SYMBOL + " " + m_display_name));

        const std::string romPath      = m_rom_path;
        const std::string displayName  = m_display_name;

        for (int i = 0; i < 10; ++i) {
            char ts[48] = {};
            read_slot_timestamp(romPath.c_str(), i, ts, sizeof(ts));

            const int slot = i;
            auto* item = new tsl::elm::MiniListItem(make_slot_label(i), ts);
            item->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                tsl::swapTo<SlotActionGui>(romPath, displayName, slot);
                return true;
            });
            list->addItem(item);
        }

        // Jump to explicit target (returning from SlotActionGui) or, on first
        // entry, to the most recently written slot so focus lands there.
        const std::string jumpTarget = !m_jump_to.empty()
            ? m_jump_to
            : newest_state_slot_label(m_rom_path.c_str());
        if (!jumpTarget.empty())
            list->jumpToItem(jumpTarget, "", true);

        return make_bare_frame(list);
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            tsl::swapTo<GameConfigGui>(m_rom_path, m_display_name, "Save States");
            return true;
        }
        return false;
    }
};

// =============================================================================
// GameConfigGui — per-game configuration screen
//
// Opened by pressing Y on a ROM entry in RomSelectorGui.
//
// Items shown:
//   • "Pallet Mode" — cycling (GBC → DMG → Native) for DMG and CGB-compatible games;
//                    read-only "GBC" for CGB-only games (0xC0 header flag)
//   • "Save States" — opens SaveStatesGui with 10 named slots
//   • "Reset"       — cold-boots the game (deletes internal state first)
//
// Header: "Configure ◆ <GAMENAME>"
// B returns to RomSelectorGui.
// =============================================================================
class GameConfigGui : public tsl::Gui {
    std::string m_rom_path;
    std::string m_display_name;
    std::string m_jump_to;   // item label to restore scroll on re-entry
public:
    GameConfigGui(std::string romPath, std::string displayName, std::string jumpTo = "")
        : m_rom_path(std::move(romPath))
        , m_display_name(std::move(displayName))
        , m_jump_to(std::move(jumpTo))
    {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();

        // Header: "GAMENAME"
        list->addItem(new tsl::elm::CategoryHeader(m_display_name));

        const std::string romPath     = m_rom_path;
        const std::string displayName = m_display_name;

        // ── Save States ───────────────────────────────────────────────────
        auto* statesItem = new tsl::elm::ListItem("Save States");
        statesItem->setValue(ult::DROPDOWN_SYMBOL);
        statesItem->setClickListener([romPath, displayName](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            triggerEnterFeedback();
            tsl::swapTo<SaveStatesGui>(romPath, displayName);
            return true;
        });
        list->addItem(statesItem);

        // ── Save Data ─────────────────────────────────────────────────────
        auto* saveDataItem = new tsl::elm::ListItem("Save Data");
        saveDataItem->setValue(ult::DROPDOWN_SYMBOL);
        saveDataItem->setClickListener([romPath, displayName](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            triggerEnterFeedback();
            tsl::swapTo<SaveDataGui>(romPath, displayName);
            return true;
        });
        list->addItem(saveDataItem);

        // ── Reset (cold boot) ─────────────────────────────────────────────
        auto* resetItem = new tsl::elm::SilentListItem("Reset");
        resetItem->setClickListener([romPath](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            // Delete the internal quick-resume state so gb_load_rom cold-boots
            char internalPath[PATH_BUFFER_SIZE] = {};
            build_rom_data_path(romPath.c_str(), internalPath, sizeof(internalPath),
                                STATE_DIR, ".state");
            delete_file(internalPath);
            // Launch the game — no state file means cold boot
            return launch_game(romPath.c_str());
        });
        list->addItem(resetItem);

        // ── Display ───────────────────────────────────────────────────────────
        list->addItem(new tsl::elm::CategoryHeader("Display"));


        // isCgbOnly is no longer needed — all CGB games (0x80 and 0xC0) support
        // user-selected palettes via luminance-mapped output. Removed.

        // ── Pallet Mode ───────────────────────────────────────────────────
        // All games cycle GBC → DMG → Native on each A press and apply live.
        // Pure DMG games use the flat-palette path (cgbMode=0, always prepacked).
        // CGB games (both 0x80 compat and 0xC0 only) stay in cgbMode=1; the
        // renderer reads fixPalette[px], computes luminance, and maps to shade.
        {
            const PaletteMode current = load_game_palette_mode(romPath.c_str());
            const char* modeLabel = palette_mode_label(current);

            auto* palItem = new tsl::elm::ListItem("Pallet Mode", modeLabel);
            palItem->setClickListener([romPath, palItem](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                // Cycle to the next mode, save, then update the item value in-place.
                // No swapTo needed — setValue() redraws the label immediately.
                const PaletteMode next = next_palette_mode(
                    load_game_palette_mode(romPath.c_str()));
                save_game_palette_mode(romPath.c_str(), next);
                // Live-apply if this ROM is currently loaded (paused in background).
                // All palette transitions are safe without a restart:
                //   • Pure DMG games:  cgbMode=0, always prepacked. Just rebake palette.
                //   • All CGB games:   cgbMode=1. Renderer flags follow the palette
                //                      (GBC → rgb565=true/prepacked=false;
                //                       DMG/Native → rgb565=false/prepacked=true).
                //                      gb_lcd_draw_line picks the correct branch on
                //                      the very next scanline — no restart needed.
                if (g_gb.rom &&
                    strncmp(g_gb.romPath, romPath.c_str(), sizeof(g_gb.romPath)) == 0) {
                    g_palette_mode    = next;
                    // Recalculate renderer flags. cgbMode stays 1 for all CGB games;
                    // we redirect between the rgb565 and prepacked output paths only.
                    const bool cgbCore = (g_gb.gb->cgb.cgbMode != 0);
                    g_fb_is_rgb565    = cgbCore && (next == PaletteMode::GBC);
                    g_fb_is_prepacked = !g_fb_is_rgb565;
                    gb_select_dmg_palette();  // rebakes g_dmg_flat_pal for new mode
                    reset_lcd_ghosting();     // flush ghosting buffer on format switch
                }
                palItem->setValue(palette_mode_label(next));
                return true;
            });
            list->addItem(palItem);
        }

        // No Sprite Limit — per-game, on by default.
        // When on, lifts the 10-sprites-per-scanline hardware cap so games
        // that flicker sprites to fake transparency show all sprites every
        // frame.  Disable only if a specific game relies on the hardware
        // cap for correct behavior.
        {
            const bool nslOn = load_game_no_sprite_limit(romPath.c_str());
            auto* nsl_item = new tsl::elm::ToggleListItem("No Sprite Limit", nslOn,
                                                           ult::ON, ult::OFF);
            nsl_item->setStateChangedListener([romPath](bool state) {
                save_game_no_sprite_limit(romPath.c_str(), state);
                // Apply live if this ROM is currently loaded
                if (g_gb.rom && g_gb.gb &&
                    strncmp(g_gb.romPath, romPath.c_str(), sizeof(g_gb.romPath)) == 0) {
                    g_gb.gb->direct.no_sprite_limit = state;
                }
            });
            list->addItem(nsl_item);
        }

        // LCD Ghosting — per-game, off by default.
        // Available only on furtherExpandedMemory (10 MB+ heap, any ROM), or on
        // expandedMemory (8 MB heap) with a ROM < 4 MB.  All other tiers —
        // limitedMemory (4 MB), base 6 MB heap, or 8 MB with a 4 MB+ ROM —
        // show a locked warning item pointing the user toward 10 MB+.
        // Ghosting lock and message — both derived from ROM size and current heap tier.
        // The message always names the MINIMUM tier that would allow ghosting for
        // this specific ROM:
        //   ROM < 2 MB  → needs 6 MB heap  (locked only on 4 MB)
        //   ROM 2–4 MB  → needs 8 MB heap  (locked on 4 MB and 6 MB)
        //   ROM >= 4 MB → needs 10 MB heap (locked on 4 MB, 6 MB, and 8 MB)
        size_t ghostingRomSz = get_rom_size(romPath.c_str());
        const bool ghostingLocked =
            ult::limitedMemory ||
            (!ult::expandedMemory && ghostingRomSz >= kROM_2MB) ||
            (ult::expandedMemory && !ult::furtherExpandedMemory && ghostingRomSz >= kROM_4MB);
        const char* ghostingMsg =
            (ghostingRomSz >= kROM_4MB) ? REQUIRES_AT_LEAST_10MB :
            (ghostingRomSz >= kROM_2MB) ? REQUIRES_AT_LEAST_8MB  :
                                           REQUIRES_AT_LEAST_6MB;

        if (ghostingLocked) {
            auto* ghost_item = new tsl::elm::ListItem("LCD Ghosting", ult::OFF);
            ghost_item->setTextColor(tsl::warningTextColor);
            ghost_item->setValueColor(tsl::offTextColor);
            ghost_item->setClickListener([ghostingMsg](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                show_notify(ghostingMsg);
                triggerNavigationFeedback();
                return true;
            });
            list->addItem(ghost_item);
        } else {
            const bool ghostingOn = load_game_lcd_ghosting(romPath.c_str());
            auto* ghost_item = new tsl::elm::ToggleListItem("LCD Ghosting", ghostingOn,
                                                             ult::ON, ult::OFF);
            ghost_item->setStateChangedListener([romPath](bool state) {
                save_game_lcd_ghosting(romPath.c_str(), state);
                // Apply live if this ROM is currently loaded
                if (g_gb.rom &&
                    strncmp(g_gb.romPath, romPath.c_str(), sizeof(g_gb.romPath)) == 0) {
                    g_lcd_ghosting = state;
                    reset_lcd_ghosting();  // flush prev-frame buffer on toggle
                }
            });
            list->addItem(ghost_item);
        }

        // Restore scroll position when returning from a sub-screen
        if (!m_jump_to.empty())
            list->jumpToItem(m_jump_to, "", true);

        return make_bare_frame(list);
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            tsl::swapTo<RomSelectorGui>(m_display_name);
            return true;
        }
        return false;
    }
};

// =============================================================================
// SettingsGui — right page (Volume / Settings)
//
// Lives only while the user is on the Settings page.  swapTo<RomSelectorGui>()
// destroys this Gui — frame + list + all items — before constructing a fresh
// RomSelectorGui.  Only one list is ever alive at a time.
//
// Memory path (swapTo pops the old Gui before pushing the new one):
//   swapTo<RomSelectorGui>()
//     → ~SettingsGui()          (default — no manual deletes needed)
//     → ~Gui() deletes m_topElement (the UltraGBOverlayFrame)
//     → ~UltraGBOverlayFrame() deletes m_contentElement (the List)
//     → ~List() deletes all child items
// =============================================================================
class SettingsGui : public tsl::Gui {
    // Raw pointer into the list owned by the frame; valid for the lifetime of
    // this Gui (the slider is destroyed together with the list).
    VolumeTrackBar*     m_vol_slider      = nullptr;
    tsl::elm::ListItem* m_scale_item      = nullptr;  ///< "Windowed Scale" — refreshed when Windowed Docked changes.
    tsl::elm::Element*  m_lastFocused     = nullptr;  ///< Tracks last-seen focus for update() change detection.
    u8                 m_vol             = 100;
    u8                 m_vol_backup      = 100;
    std::string        m_rom_scroll;       // ROM name to scroll to when returning to RomSelectorGui
    std::string        m_settings_scroll; // scroll target passed in at construction; forwarded to QuickComboSelectorGui so it can restore position on return

    // Toggle mute on/off and persist both volume and backup.
    // Called by the speaker-icon tap, the icon-tap callback, and KEY_Y.
    void do_vol_toggle() {
        if (m_vol > 0) {
            // Muting — capture current positive volume as the restore target.
            m_vol_backup = m_vol;
            g_vol_backup = m_vol;
            save_vol_backup();
            m_vol = 0;
        } else {
            // Unmuting — restore to the persisted backup (always > 0).
            m_vol = m_vol_backup;
        }
        m_vol_slider->setProgress(m_vol);
        gb_audio_set_volume(m_vol);
        char vbuf[4];
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeyVolume,
                             vol_to_str(m_vol, vbuf), "");
        triggerNavigationFeedback();
    }

public:
    explicit SettingsGui(std::string romScroll = "", std::string settingsScroll = "")
        : m_rom_scroll(std::move(romScroll))
        , m_settings_scroll(std::move(settingsScroll)) {}

    // No destructor needed: Tesla deletes m_topElement (frame) →
    // ~UltraGBOverlayFrame deletes m_contentElement (list) →
    // ~List deletes all items.

    // -----------------------------------------------------------------------
    // update() — called every frame by Tesla.
    //
    // Keeps g_settings_scroll continuously in sync with whatever item is
    // currently focused.  This ensures that if the overlay process is
    // terminated by an external event (e.g. the mode combo fires Tesla's
    // own relaunch while the user is mid-browse), exitServices() can write
    // the correct focused-item label to the INI transient key — even if the
    // user never pressed LEFT to navigate back to the ROM selector.
    //
    // Only updated when focus actually changes (pointer comparison) so the
    // common case (no movement) costs a single branch and two pointer loads.
    // -----------------------------------------------------------------------
    virtual void update() override {
        auto* focused = getFocusedElement();
        if (focused && focused != m_lastFocused) {
            m_lastFocused = focused;
            const std::string label = (focused == m_vol_slider)
                ? "Game Boy"
                : static_cast<tsl::elm::ListItem*>(focused)->getText();
            set_settings_scroll(label.c_str());
        }
    }

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();

        // ── Volume ────────────────────────────────────────────────────────────
        // Static: ult::DIVIDER_SYMBOL is set once at startup and never changes,
        // so this concatenation only needs to run on the very first createUI() call.
        static const std::string kVolumeHeader =
            "Volume " + ult::DIVIDER_SYMBOL + " \uE0E3 Toggle Mute";
        list->addItem(new tsl::elm::CategoryHeader(kVolumeHeader));

        m_vol        = gb_audio_get_volume();
        m_vol_backup = g_vol_backup;  // load persisted backup; never 0

        auto* vol_slider = new VolumeTrackBar(
            "\uE13C", false, false, true, "Game Boy", "%", false);
        vol_slider->setProgress(m_vol);
        vol_slider->setValueChangedListener([this](u8 value) {
            m_vol = value;
            gb_audio_set_volume(value);
            char vbuf[4];
            ult::setIniFileValue(kConfigFile, kConfigSection, kKeyVolume,
                                 vol_to_str(value, vbuf), "");
            // Keep the unmute backup up to date:
            //   • positive value → track it so mute/unmute stays meaningful
            //   • zero (slider dragged/clicked to 0) → reset to 100 so unmuting
            //     doesn't restore a 1% volume that would feel like still muted
            const u8 newBackup = (value > 0) ? value : static_cast<u8>(100);
            if (newBackup != m_vol_backup) {
                m_vol_backup = newBackup;
                g_vol_backup = newBackup;
                save_vol_backup();
            }
        });
        m_vol_slider = vol_slider;
        vol_slider->setIconTapCallback([this]() { do_vol_toggle(); });
        list->addItem(vol_slider);

        // ── Display ───────────────────────────────────────────────────────────
        list->addItem(new tsl::elm::CategoryHeader("Display"));


        // ── Windowed Mode ─────────────────────────────────────────────────────
        // When ON, launching a ROM relaunches this overlay with -windowed <path>
        // so the game runs as a small draggable 160×144 window with no UI chrome.
        // Tap-hold (1 s) inside the window to reposition; launch combo to return.
        auto* win_item = new tsl::elm::ToggleListItem("Windowed Mode", g_windowed_mode,
                                                       ult::ON, ult::OFF);
        win_item->setStateChangedListener([](bool state) {
            g_windowed_mode = state;
            save_windowed_mode();
        });
        list->addItem(win_item);

        // ── Windowed Scale ────────────────────────────────────────────────────
        // Cycles 1× → 2× → 3× → 4× → 5× → (6×) → 1× on each A press.
        // Capped at 3× on 4 MB heap sessions.  Takes effect on the next
        // windowed launch (framebuffer is sized before Tesla initialises).
        // 6× is only available on expandedMemory + docked + 1080p.
        // On the plain 8 MB heap (not furtherExpandedMemory), only ROMs < 4 MB
        // have enough headroom; larger ROMs are capped at 5×.
        {
            const bool romSmall = [&]() -> bool {
                if (ult::furtherExpandedMemory) return true;
                if (!g_last_rom_path[0])        return false;
                const std::string full = std::string(g_rom_dir) + g_last_rom_path;
                return get_rom_size(full.c_str()) < kROM_4MB;
            }();
            // make_label captures only romSmall (stable for the session) and
            // recomputes can6x / maxScale on every call so the displayed value
            // stays correct if g_win_1080 changes via the Windowed Docked toggle.
            auto make_label = [romSmall]() -> std::string {
                const bool c6_ = ult::expandedMemory && poll_console_docked() && g_win_1080 && romSmall;
                const int  ms_ = ult::limitedMemory ? 3 : (!ult::expandedMemory ? 4 : (c6_ ? 6 : 5));
                const int s = std::min(g_win_scale, ms_);
                static const char* lbs[] = { "1x", "2x", "3x", "4x", "5x", "6x"};
                return lbs[s - 1];
            };

            auto* scale_item = new tsl::elm::ListItem("Windowed Scale", make_label());
            scale_item->setClickListener([scale_item, romSmall, make_label](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                const bool c6_ = ult::expandedMemory && poll_console_docked() && g_win_1080 && romSmall;
                const int  ms_ = ult::limitedMemory ? 3 : (!ult::expandedMemory ? 4 : (c6_ ? 6 : 5));
                const int cur = std::min(g_win_scale, ms_);
                g_win_scale   = (cur % ms_) + 1;
                save_win_scale();
                scale_item->setValue(make_label());
                return true;
            });
            m_scale_item = scale_item;
            list->addItem(scale_item);
        }

        // ── Windowed Docked ───────────────────────────────────────────────────
        // "720p"  — VI layer is ×1.5 the framebuffer (default).  Larger window
        //           on-screen but non-integer scaling; LCD Grid looks blurry.
        // "1080p" — VI layer equals the framebuffer exactly (1:1 display pixels).
        //           Pixel-perfect; LCD Grid renders with clean integer boundaries.
        //           Window appears smaller since no enlargement is applied.
        // Takes effect on the next windowed launch.
        {
            auto* out_item = new tsl::elm::ListItem("Windowed Docked",
                                                     g_win_1080 ? "1080p" : "720p");
            out_item->setClickListener([this, out_item](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                g_win_1080 = !g_win_1080;
                save_win_output();
                out_item->setValue(g_win_1080 ? "1080p" : "720p");
                // Sync the Windowed Scale display — the effective maxScale may have
                // changed (e.g. 1080p+docked unlocks 6×, 720p caps at 5×).
                // g_win_scale (the stored INI intent) is never touched here; only the
                // visible label is corrected so the user sees the value that would
                // actually take effect on the next windowed launch.
                if (m_scale_item) {
                    const bool rs = ult::furtherExpandedMemory ? true
                                  : (g_last_rom_path[0] && get_rom_size(
                                         (std::string(g_rom_dir) + g_last_rom_path).c_str()) < kROM_4MB);
                    const bool c6  = ult::expandedMemory && poll_console_docked() && g_win_1080 && rs;
                    const int  ms  = ult::limitedMemory ? 3 : (!ult::expandedMemory ? 4 : (c6 ? 6 : 5));
                    static const char* lbs[] = { "1x", "2x", "3x", "4x", "5x", "6x" };
                    m_scale_item->setValue(lbs[std::min(g_win_scale, ms) - 1]);
                }
                return true;
            });
            list->addItem(out_item);
        }

        // ── LCD Grid ──────────────────────────────────────────────────────────
        // Simulates the dark inter-pixel gap of a real Game Boy Color LCD by
        // dimming the last row and column of each scaled source-pixel block to
        // ~12.5 % brightness, leaving a visible "grid" between lit cells.
        //
        // Applies to both the overlay renderer (post-pass over the viewport) and
        // the windowed renderer (inline in the blit via a compile-time template).
        // Automatically has no visible effect at windowed 1× scale (each GB
        // pixel is a single framebuffer pixel — no room for a gap); the flag is
        // still persisted so it activates as soon as the user scales up.
        {
            auto* grid_item = new tsl::elm::ToggleListItem(
                "LCD Grid", g_lcd_grid, ult::ON, ult::OFF);
            grid_item->setStateChangedListener([](bool state) {
                g_lcd_grid = state;
                save_lcd_grid();
            });
            list->addItem(grid_item);
        }

        
        // ── Misc ────────────────────────────────────
        list->addItem(new tsl::elm::CategoryHeader("Miscellaneous"));

        // ── Quick Combo ───────────────────────────────────────────────────────
        // A combo assigned here lets the user open the last played ROM directly
        // (bypassing the ROM selector) from anywhere on the Switch.  Selecting
        // a combo removes it from any other overlay/package that currently holds
        // it — identical deconfliction to Ultrahand's combo picker.
        {
            // Display the current combo converted to its unicode button symbols,
            // or the OPTION_SYMBOL placeholder when none is assigned.
            std::string comboDisplay = g_quick_combo[0]
                ? g_quick_combo
                : ult::OPTION_SYMBOL;
            if (g_quick_combo[0])
                ult::convertComboToUnicode(comboDisplay);

            const std::string capturedDisplay = comboDisplay;
            auto* qc_item = new tsl::elm::ListItem("Quick Combo", comboDisplay);
            qc_item->setClickListener([this](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                // Persist the current scroll target before leaving so SettingsGui
                // can restore it when the user presses B inside QuickComboSelectorGui.
                set_settings_scroll("Quick Combo");
                tsl::swapTo<QuickComboSelectorGui>(m_rom_scroll, std::string("Quick Combo"));
                return true;
            });
            list->addItem(qc_item);
        }

        auto* haptics_item = new tsl::elm::ToggleListItem(
            "In-Game Haptics", g_ingame_haptics, ult::ON, ult::OFF);
        haptics_item->setStateChangedListener([](bool state) {
            g_ingame_haptics = state;
            ult::setIniFileValue(kConfigFile, kConfigSection, kKeyIngameHaptics,
                                 state ? "1" : "0", "");
        });
        list->addItem(haptics_item);

        if (ult::expandedMemory) {
            auto* wallpaper_item = new tsl::elm::ToggleListItem(
                "In-Game Wallpaper", g_ingame_wallpaper, ult::ON, ult::OFF);
            wallpaper_item->setStateChangedListener([](bool state) {
                g_ingame_wallpaper = state;
                ult::setIniFileValue(kConfigFile, kConfigSection, kKeyIngameWallpaper,
                                     state ? "1" : "0", "");
            });
            list->addItem(wallpaper_item);
        }

        // Restore the previously focused item (empty on first visit or after a game launch).
        if (!m_settings_scroll.empty())
            list->jumpToItem(m_settings_scroll, "", true);

        // Footer: left-arrow "Games" button.
        // frame takes ownership of list; Tesla takes ownership of frame.
        auto* frame = new UltraGBOverlayFrame("Games", "");
        frame->setContent(list);
        return frame;
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)touchPos; (void)leftJoy; (void)rightJoy;

        // simulatedNextPage fires when the user taps the footer page button.
        // On the Settings page the footer shows left-arrow "Games", so treat
        // simulatedNext as "go left" — same as KEY_LEFT.
        const bool simulatedNext = ult::simulatedNextPage.exchange(
            false, std::memory_order_acq_rel);
        // Only block page navigation if the volume slider itself is focused AND unlocked.
        // If focus has moved to another item, allowSlide being true
        // is irrelevant and should not prevent page changes.
        const bool sliderActive = m_vol_slider && m_vol_slider->hasFocus()
                                  && ult::allowSlide.load(std::memory_order_acquire);
        const bool wantLeft = !sliderActive && (simulatedNext || ((keysDown & KEY_LEFT) && !(keysHeld & ~KEY_LEFT & ALL_KEYS_MASK)));

        if (wantLeft) {
            triggerNavigationFeedback();
            // Reset slider lock state so it comes up locked on the next page visit.
            ult::allowSlide.store(false, std::memory_order_release);
            // Capture the focused item's label right now — no per-frame polling needed.
            // Tesla already tracks m_focusedElement; we read it once at the moment it
            // actually matters (page flip).  m_vol_slider is the only non-ListItem, so
            // it gets its label by pointer comparison; every other focusable item is a
            // ListItem subclass and exposes getText() directly.
            if (auto* focused = getFocusedElement()) {
                const std::string label = (focused == m_vol_slider)
                    ? "Game Boy"
                    : static_cast<tsl::elm::ListItem*>(focused)->getText();
                    set_settings_scroll(label.c_str());
            }
            tsl::swapTo<RomSelectorGui>(m_rom_scroll);
            return true;
        }

        // Y — mute/unmute toggle when the volume slider is focused
        if ((keysDown & KEY_Y) && m_vol_slider && m_vol_slider->hasFocus()) {
            do_vol_toggle();
            return true;
        }

        // B — close the overlay entirely.  Clear the saved scroll so a future
        // cold open of Settings starts at the top rather than mid-list.
        if (keysDown & KEY_B) {
            g_settings_scroll[0] = '\0';
            tsl::Overlay::get()->close();
            return true;
        }

        return false;
    }
};

// =============================================================================
// QuickComboSelectorGui — combo picker for the Quick Launch feature
//
// Swapped to from SettingsGui when the user presses A on "Quick Combo".
// Shows OPTION_SYMBOL (none) then every entry in g_defaultCombos.
// On selection:
//   1. Removes the chosen combo from all other overlays/packages (deconflict).
//   2. Writes the combo to overlays.ini mode_combos[0] under our own filename.
//   3. Calls tsl::hlp::loadEntryKeyCombos() so Tesla picks up the new binding.
//   4. Updates g_quick_combo so SettingsGui displays the new value on return.
// B returns to SettingsGui, scrolling back to the "Quick Combo" item.
// =============================================================================
class QuickComboSelectorGui : public tsl::Gui {
    std::string m_rom_scroll;
    std::string m_settings_scroll;
    tsl::elm::ListItem* m_lastSelected = nullptr;

    // Write combo (or "" to clear) into our overlays.ini mode_combos[0].
    // Also updates g_quick_combo for the Settings display.
    void save_combo(const std::string& combo) {
        const char* fn = ovl_filename();
        if (!fn || !fn[0]) return;

        auto iniData = ult::getParsedDataFromIniFile(ult::OVERLAYS_INI_FILEPATH);
        auto& section = iniData[fn];
        auto comboList = splitIniList(section["mode_combos"]);
        if (comboList.empty()) comboList.emplace_back();
        comboList[0] = combo;
        section["mode_combos"] = "(" + joinIniList(comboList) + ")";
        ult::saveIniFileData(ult::OVERLAYS_INI_FILEPATH, iniData);
        tsl::hlp::loadEntryKeyCombos();

        g_quick_combo[0] = '\0';
        if (!combo.empty())
            strncpy(g_quick_combo, combo.c_str(), sizeof(g_quick_combo) - 1);
    }

public:
    QuickComboSelectorGui(std::string romScroll, std::string settingsScroll)
        : m_rom_scroll(std::move(romScroll))
        , m_settings_scroll(std::move(settingsScroll)) {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader("Quick Combo"));

        // Read the current combo fresh from overlays.ini (g_quick_combo may lag
        // if the user picked a combo and immediately re-entered this screen).
        std::string currentCombo;
        {
            const char* fn = ovl_filename();
            if (fn && fn[0] && ult::isFile(ult::OVERLAYS_INI_FILEPATH)) {
                const std::string mc = ult::parseValueFromIniSection(
                    ult::OVERLAYS_INI_FILEPATH, fn, "mode_combos");
                const auto cl = splitIniList(mc);
                if (!cl.empty()) currentCombo = cl[0];
            }
        }

        // ── None (clear) ──────────────────────────────────────────────────────
        {
            auto* item = new tsl::elm::ListItem(ult::OPTION_SYMBOL);
            if (currentCombo.empty()) {
                item->setValue(ult::CHECKMARK_SYMBOL);
                m_lastSelected = item;
            }
            item->setClickListener([this, item](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                save_combo("");
                if (m_lastSelected && m_lastSelected != item)
                    m_lastSelected->setValue("");
                item->setValue(ult::CHECKMARK_SYMBOL);
                m_lastSelected = item;
                //triggerNavigationFeedback();
                return true;
            });
            list->addItem(item);
        }

        // ── Predefined combos ─────────────────────────────────────────────────
        std::string jumpTarget = currentCombo.empty() ? ult::OPTION_SYMBOL : "";

        for (const auto& combo : g_defaultCombos) {
            // Skip any combo that matches Tesla's current show/hide combo so the
            // user cannot accidentally reassign the overlay open trigger to Quick
            // Launch — that would make the overlay unreachable.
            const u64 comboKeys = tsl::hlp::comboStringToKeys(combo);
            if (comboKeys == tsl::cfg::launchCombo)
                continue;
            std::string display = combo;
            ult::convertComboToUnicode(display);

            const bool isCurrent = !currentCombo.empty() &&
                tsl::hlp::comboStringToKeys(combo) ==
                tsl::hlp::comboStringToKeys(currentCombo);

            if (isCurrent && jumpTarget.empty())
                jumpTarget = display;

            auto* item = new tsl::elm::ListItem(display);
            if (isCurrent) {
                item->setValue(ult::CHECKMARK_SYMBOL);
                m_lastSelected = item;
            }
            item->setClickListener([this, item, combo](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                // 1. Deconflict: remove this combo from every other overlay/package.
                remove_quick_combo_from_others(combo);
                // 2. Persist and reload.
                save_combo(combo);
                if (m_lastSelected && m_lastSelected != item)
                    m_lastSelected->setValue("");
                item->setValue(ult::CHECKMARK_SYMBOL);
                m_lastSelected = item;
                //triggerNavigationFeedback();
                return true;
            });
            list->addItem(item);
        }

        // Scroll to the currently selected item when the page opens.
        if (!jumpTarget.empty())
            list->jumpToItem(jumpTarget, "", true);

        return make_bare_frame(list);
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            tsl::swapTo<SettingsGui>(m_rom_scroll, m_settings_scroll);
            return true;
        }
        return false;
    }
};

// =============================================================================
// RomSelectorGui — left page (ROM picker)
//
// Each visit constructs a fresh list as a local in createUI() and hands it to
// the frame, which takes ownership.  swapTo<SettingsGui>() destroys this Gui
// (frame + list + all MiniListItems) before building SettingsGui — and vice
// versa — so only one list is ever resident in memory at a time.
//
// jumpToItem() restores the scroll position to the last-played ROM on every
// re-entry, so the user lands back where they left off.
// =============================================================================
class RomSelectorGui : public tsl::Gui {
    std::string m_jump_to;  // item to scroll to on createUI; empty = scroll to inProgress
    bool m_waitForRelease = false;  // true while we wait for all keys to clear after emulator exit
public:
    explicit RomSelectorGui(std::string jumpTo = "") : m_jump_to(std::move(jumpTo)) {
        // If GBOverlayGui flagged that L/R/ZL/ZR may still be physically held,
        // arm the release-gate so handleInput swallows those keys until the
        // controller is clean.
        if (g_waitForInputRelease) {
            g_waitForInputRelease = false;
            m_waitForRelease = true;
        }
    }
    //~RomSelectorGui() {
    //    tsl::gfx::FontManager::clearCache();
    //}
    // No destructor needed: Tesla deletes m_topElement (frame) →
    // ~UltraGBOverlayFrame deletes m_contentElement (list) →
    // ~List deletes all items.
    //
    // Note on glyph cache: GBOverlayGui::createUI() calls
    // FontManager::clearCache() before the ROM malloc, so no flush is
    // needed here — the cache repopulates automatically when the user
    // returns to this screen.

    virtual tsl::elm::Element* createUI() override {
        g_emu_active = false;

        //if (ult::useSoundEffects && !ult::limitedMemory)
        //    ult::Audio::initialize();

        // ── ROM list ─────────────────────────────────────────────────────
        auto* list = new tsl::elm::List();
        // Static: ult::DIVIDER_SYMBOL is set once at startup and never changes,
        // so this concatenation only needs to run on the very first createUI() call.
        static const std::string kRomListHeader =
            "Game Boy Games " + ult::DIVIDER_SYMBOL + " \uE0E3 Configure";
        list->addItem(new tsl::elm::CategoryHeader(kRomListHeader));

        // ── ROM list via scandir — no std::vector, no full-path string per entry ──
        // scandir returns a sorted dirent**; each entry is freed immediately after use.
        // The lambda captures only the 255-byte basename inline (char[256] in the
        // closure) — the full path is snprintf'd onto the stack only at click time.
        struct dirent** romEntries = nullptr;
        const int nRoms = scandir(g_rom_dir, &romEntries, gb_rom_filter, alphasort);

        if (nRoms <= 0) {
            static char msg[512];
            snprintf(msg, sizeof(msg),
                "No .gb or .gbc files found in:\n%s\n\n"
                "Edit: sdmc:/config/ultragb/config.ini", g_rom_dir);
            auto* empty = new tsl::elm::CustomDrawer(
                [](tsl::gfx::Renderer* r, s32 x, s32 y, s32, s32) {
                    r->drawString(msg, false, x + 16, y + 30, 16,
                                  tsl::Color{0x8, 0x8, 0x8, 0xF});
                });
            list->addItem(empty, 200);
        } else {
            // If returning from GameConfigGui, m_jump_to holds the configured ROM's
            // basename so we land back on it.  On first open (m_jump_to empty) we
            // scroll to the last-played (inProgress) entry instead.
            const std::string& jumpLabel = m_jump_to;
            std::string inProgressLabel;  // basename of last-played ROM (for auto-scroll on first open)
            char fullPath[PATH_BUFFER_SIZE];

            for (int ri = 0; ri < nRoms; ri++) {
                const char* name = romEntries[ri]->d_name;
                snprintf(fullPath, sizeof(fullPath), "%s%s", g_rom_dir, name);

                // g_gb.rom is null here — gb_unload_rom() was called before any
                // swapTo<RomSelectorGui>, so there is no live ROM to fast-resume.
                const bool isLast = g_last_rom_path[0] &&
                    strcmp(name, g_last_rom_path) == 0;
                // inProgress symbol = only the genuinely last-played ROM.
                // Only show if playable on this tier.
                const bool playable   = rom_is_playable(fullPath);
                const bool inProgress = isLast && playable;

                if (inProgress && inProgressLabel.empty())
                    inProgressLabel = name;

                auto* item = new tsl::elm::SilentListItem(name, inProgress ? ult::INPROGRESS_SYMBOL : "", true);
                if (!playable)
                    item->setTextColor(tsl::warningTextColor);

                // Capture only the basename (≤255 bytes) inline in the closure.
                // Full path is reconstructed onto the stack at click time only —
                // zero extra heap per ROM during list construction.
                const std::string romNameStr(name);
                
                item->setClickListener([romNameStr, playable](u64 keys) -> bool {
                    char p[PATH_BUFFER_SIZE];
                    snprintf(p, sizeof(p), "%s%s", g_rom_dir, romNameStr.c_str());
                    strncpy(g_rom_selector_scroll, romNameStr.c_str(), sizeof(g_rom_selector_scroll) - 1);
                    g_rom_selector_scroll[sizeof(g_rom_selector_scroll) - 1] = '\0';
                    if (keys & KEY_Y) {
                        triggerRumbleClick.store(true, std::memory_order_release);
                        triggerSettingsSound.store(true, std::memory_order_release);
                        tsl::swapTo<GameConfigGui>(p, romNameStr);
                        return true;
                    }
                    if (!(keys & KEY_A)) return false;
                    if (!playable) { gb_load_rom(p); return false; }
                    return launch_game(p);
                });
                list->addItem(item);

                free(romEntries[ri]);   // free each dirent immediately — peak = 1 at a time
            }
            free(romEntries);

            // Restore scroll position:
            //   • returning from configure → jump to the configured ROM (jumpLabel set)
            //   • first open               → jump to the last-played / inProgress ROM
            const std::string& scrollTarget = !jumpLabel.empty() ? jumpLabel : inProgressLabel;
            if (!scrollTarget.empty())
                list->jumpToItem(scrollTarget, "", true);
        }

        // ── Frame ────────────────────────────────────────────────────────
        // Footer: right-arrow "Settings" button.
        // frame takes ownership of list; Tesla takes ownership of frame.
        auto* frame = new UltraGBOverlayFrame("", "Settings");
        frame->setContent(list);
        return frame;
    }

    // -------------------------------------------------------------------------
    // overrideBackButton=true means the framework never auto-calls goBack() —
    // we handle KEY_B explicitly.
    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)touchPos; (void)leftJoy; (void)rightJoy;

        // Wait for a clean controller state after returning from the emulator.
        // L/R/ZL/ZR pressed during gameplay arm Tesla's static Overlay-level
        // jump state machines (jumpToTop/jumpToBottom/skipUp/skipDown).  Keep clearing those
        // signals and swallowing all input until every physical key is released,
        // so the first UP/DOWN press the user makes in the selector is intentional.
        if (m_waitForRelease) {
            jumpToTop.store(false, std::memory_order_release);
            jumpToBottom.store(false, std::memory_order_release);
            skipUp.store(false, std::memory_order_release);
            skipDown.store(false, std::memory_order_release);
            if (keysHeld) return true;
            m_waitForRelease = false;
            return true;
        }

        // simulatedNextPage fires when the user taps the footer page button.
        // On the Games page the footer shows right-arrow "Settings".
        const bool simulatedNext = ult::simulatedNextPage.exchange(
            false, std::memory_order_acq_rel);
        const bool sliderActive = ult::allowSlide.load(std::memory_order_acquire);
        const bool wantRight = !sliderActive && (simulatedNext || ((keysDown & KEY_RIGHT) && !(keysHeld & ~KEY_RIGHT & ALL_KEYS_MASK)));

        if (wantRight) {
            triggerNavigationFeedback();
            ult::allowSlide.store(false, std::memory_order_release);
            tsl::swapTo<SettingsGui>(g_rom_selector_scroll,
                                     g_settings_scroll);
            return true;
        }

        // B — close the overlay (full exit: clear saved settings scroll position)
        if (keysDown & KEY_B) {
            g_settings_scroll[0] = '\0';
            tsl::Overlay::get()->close();
            return true;
        }

        return false;
    }
};


// =============================================================================
// Windowed mode — GBWindowedElement / GBWindowedGui.
// Included here so it has access to all globals, helpers, and gb_load_rom/
// gb_unload_rom defined above.  Must come before class Overlay below.
// The commented-out WindowedOverlay class at the bottom of gb_windowed.hpp
// is kept for reference only — it is never dispatched.
// =============================================================================
#include "gb_windowed.hpp"

// =============================================================================
// Overlay — single unified overlay class for every launch path:
//
//   Game sessions  (m_game_session = true, resolved in initServices()):
//     • Windowed mode       (-windowed, -quicklaunch with windowed=1)
//     • Player overlay mode (-overlay relaunch)
//     • Quick-launch mode   (-quicklaunch with windowed=0)
//
//   UI sessions (m_game_session = false):
//     • Cold starts, -returning from windowed, windowed-quicklaunch tooLarge fallback
//
// One class → one vtable → one tsl::loop<Overlay> instantiation in main().
// The large block of common initServices() code (directory creation, load_config,
// write_default_config_if_missing, scroll restore) is compiled exactly once,
// eliminating the ~320–400 byte duplication that existed when GameOverlay and
// Overlay were separate classes with identical setup bodies.
// =============================================================================
class Overlay : public tsl::Overlay {
    // Guard for onShow: resume audio only after a genuine onHide() call.
    // Prevents gb_audio_resume() firing on first show when the audio thread was
    // started (not paused) by gb_load_rom inside GBWindowedGui / GBOverlayGui.
    bool m_audio_paused = false;

    // True when this session will load and run a ROM.  Set in initServices()
    // after load_config() populates g_windowed_mode and g_last_rom_path.
    // Guards all game-specific teardown and audio handling in the other virtuals.
    bool m_game_session = false;

public:
    void initServices() override {
        tsl::overrideBackButton = true;
        ult::COPY_BUFFER_SIZE   = 1024;

        // ── Common init (every session type) ─────────────────────────────────
        ult::createDirectory(CONFIG_DIR);
        ult::createDirectory(SAVE_BASE_DIR);
        //ult::createDirectory(INTERNAL_SAVE_BASE_DIR);
        ult::createDirectory(STATE_BASE_DIR);
        ult::createDirectory(STATE_DIR);
        ult::createDirectory(CONFIGURE_DIR);

        load_config();                   // populates g_windowed_mode, g_last_rom_path, …
        write_default_config_if_missing();
        ult::createDirectory(g_rom_dir);
        ult::createDirectory(g_save_dir);

        // ── Determine session type ────────────────────────────────────────────
        // g_win_scale_locked is set by setup_windowed_framebuffer() in main()
        // BEFORE tsl::loop() starts — it is the reliable indicator that this
        // launch is actually a windowed session.  g_windowed_mode is only a
        // config preference (windowed=1/0 in config.ini) and must NOT be used
        // here: a user with windowed=1 stored would cause every cold start and
        // -returning launch to be misidentified as a windowed game session,
        // setting tsl::disableHiding and routing to GBWindowedGui incorrectly.
        m_game_session = g_win_scale_locked                      ||
                         g_overlay_mode                          ||
                        (g_quick_launch && g_last_rom_path[0]   &&
                         !g_quicklaunch_windowed_toobig);

        if (m_game_session)
            tsl::disableHiding = true;   // launch combo must close, not hide

        // ── Mode-specific init ────────────────────────────────────────────────
        if (g_win_scale_locked) {
            // Windowed session — setup_windowed_framebuffer() set this flag in
            // main() before tsl::loop().  Read quick-exit flag and ROM path.
            {
                const std::string qe = ult::parseValueFromIniSection(
                    kConfigFile, kConfigSection, kKeyWinQuickExit);
                g_win_quick_exit = (qe == "1");
                if (g_win_quick_exit)
                    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "", "");
            }
            {
                const std::string wrom = ult::parseValueFromIniSection(
                    kConfigFile, kConfigSection, kKeyWindowedRom);
                if (!wrom.empty() && wrom.size() < sizeof(g_win_rom_path) - 1) {
                    strncpy(g_win_rom_path, wrom.c_str(), sizeof(g_win_rom_path) - 1);
                    g_win_rom_path[sizeof(g_win_rom_path) - 1] = '\0';
                }
                ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWindowedRom, "", "");
            }
        } else {
            // All non-windowed sessions (game or UI):

            if (g_overlay_mode) {
                // Player overlay: read the ROM path written by launch_overlay_mode().
                // Erase immediately so it never persists across unrelated launches.
                const std::string pr = ult::parseValueFromIniSection(
                    kConfigFile, kConfigSection, kKeyPlayerRom);
                if (!pr.empty() && pr.size() < sizeof(g_overlay_rom_path) - 1) {
                    strncpy(g_overlay_rom_path, pr.c_str(), sizeof(g_overlay_rom_path) - 1);
                    g_overlay_rom_path[sizeof(g_overlay_rom_path) - 1] = '\0';
                }
                if (!pr.empty())
                    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyPlayerRom, "", "");
            }

            if (!m_game_session) {
                // UI-only session: register quick-launch mode in overlays.ini so
                // Ultrahand can display and assign a combo to it.  Also refreshes g_quick_combo.
                register_quick_launch_mode();
            }

            // Restore settings-page scroll position.
            //
            // Shared by overlay game sessions (-overlay, -quicklaunch) and UI
            // sessions (-returning, tooLarge fallback).  The multi-hop chain:
            //
            //   SettingsGui → RomSelectorGui
            //     → launch writes INI key
            //       → game session reads + erases key → g_settings_scroll
            //         → game plays → combo return writes key again → -returning
            //           → UI session restores → SettingsGui reopens at correct item
            //
            // Always erased immediately so an abnormal exit can't leak stale data.
            {
                const std::string ss = ult::parseValueFromIniSection(
                    kConfigFile, kConfigSection, kKeySettingsScroll);
                if (!ss.empty() && ss.size() < sizeof(g_settings_scroll) - 1)
                    set_settings_scroll(ss.c_str());
                g_returning_from_windowed = false;   // consumed; clear for safety
                if (!ss.empty())
                    ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll, "", "");
            }
        }
    }

    void exitServices() override {
        tsl::disableHiding = false;

        if (m_game_session) {
            if (g_win_scale_locked) {
                // ── Windowed teardown ─────────────────────────────────────────
                tsl::hlp::requestForeground(false);  // reclaim HID if pass-through was active
                ult::layerEdge  = 0;                 // restore for normal overlay hit-tests
                tsl::layerEdgeY = 0;
                // Shut down the blit thread pool before freeing the framebuffer.
                // Workers must not be running when gb_unload_rom frees g_gb_fb.
                // shutdown() joins all workers so the blit is complete before we continue.
                if (s_win_pool_active) {
                    s_win_pool.shutdown();
                    s_win_pool_active = false;
                }
            } else {
                // ── Overlay teardown ──────────────────────────────────────────
                // Persist scroll — catches Tesla mode-combo relaunch mid-game.
                if (g_settings_scroll[0])
                    ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll,
                                         g_settings_scroll, "");
                gb_audio_pause();
            }
            // Common game teardown — runs for both windowed and overlay sessions.
            gb_unload_rom();      // saves state, writes SRAM, frees ROM buffer
            gb_audio_free_dma();  // releases DMA buffers held across sessions
            free_lcd_ghosting();  // releases ghosting heap held across sessions
        } else {
            // ── UI session teardown ───────────────────────────────────────────
            // Persist scroll — catches Tesla mode-combo close while browsing settings.
            if (g_settings_scroll[0])
                ult::setIniFileValue(kConfigFile, kConfigSection, kKeySettingsScroll,
                                     g_settings_scroll, "");
        }
    }

    // Pause the emulator when the overlay is hidden.
    void onHide() override {
        if (!m_game_session) return;
        if (g_win_scale_locked)
            tsl::hlp::requestForeground(true);  // reclaim HID during pass-through
        g_gb.running   = false;
        g_emu_active   = false;
        gb_audio_pause();
        m_audio_paused = true;
    }

    // Resume the emulator when the overlay is shown again.
    void onShow() override {
        if (!m_game_session || !g_gb.rom) return;
        g_gb_frame_next_ns = 0;
        g_gb.running  = true;
        g_emu_active  = true;
        if (m_audio_paused) {
            gb_audio_resume();
            m_audio_paused = false;
        }
    }

    std::unique_ptr<tsl::Gui> loadInitialGui() override {
        // ── Windowed quick-launch tooLarge fallback (UI session) ─────────────────
        // main() set this flag when the pre-tsl::loop() ROM size check rejected
        // the windowed quick-launch.  Re-check post-loop for a tier-aware message;
        // if the pre-loop check was overly conservative, attempt recovery relaunch.
        if (g_quicklaunch_windowed_toobig) {
            g_quicklaunch_windowed_toobig = false;
            char romPathBuf[PATH_BUFFER_SIZE];
            snprintf(romPathBuf, sizeof(romPathBuf), "%s%s", g_rom_dir, g_last_rom_path);
            if (const char* msg = rom_playability_message(romPathBuf)) {
                show_notify(msg);
            } else if (g_self_path[0]) {
                // Pre-loop check was overly conservative — attempt recovery.
                ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWindowedRom, romPathBuf, "");
                ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "1", "");
                tsl::setNextOverlay(g_self_path, "-windowed");
                tsl::Overlay::get()->close();
            }
            return initially<RomSelectorGui>();
        }

        // ── Windowed session ─────────────────────────────────────────────────────
        if (g_win_scale_locked) {
            if (g_win_rom_path[0]) {
                strncpy(g_pending_rom_path, g_win_rom_path, sizeof(g_pending_rom_path) - 1);
                g_pending_rom_path[sizeof(g_pending_rom_path) - 1] = '\0';
            }
            return initially<GBWindowedGui>();
        }

        // ── Player overlay relaunch ──────────────────────────────────────────────
        if (g_overlay_mode && g_overlay_rom_path[0]) {
            if (const char* msg = rom_playability_message(g_overlay_rom_path)) {
                show_notify(msg);
                g_overlay_mode = false;
                if (g_self_path[0]) {
                    tsl::setNextOverlay(g_self_path);
                    tsl::Overlay::get()->close();
                }
                return initially<RomSelectorGui>();
            }
            // UltraGBOverlayFrame bypassed — trigger wallpaper load manually.
            if (ult::expandedMemory)
                ult::loadWallpaperFileWhenSafe();
            strncpy(g_pending_rom_path, g_overlay_rom_path, sizeof(g_pending_rom_path) - 1);
            g_pending_rom_path[sizeof(g_pending_rom_path) - 1] = '\0';
            g_overlay_rom_path[0] = '\0';  // consumed
            return initially<GBOverlayGui>();
        }

        // ── Quick-launch (non-windowed) ──────────────────────────────────────────
        if (g_quick_launch && g_last_rom_path[0]) {
            char romPathBuf[PATH_BUFFER_SIZE];
            snprintf(romPathBuf, sizeof(romPathBuf), "%s%s", g_rom_dir, g_last_rom_path);
            if (const char* msg = rom_playability_message(romPathBuf)) {
                show_notify(msg);
                if (g_self_path[0]) {
                    tsl::setNextOverlay(g_self_path);
                    tsl::Overlay::get()->close();
                }
                return initially<RomSelectorGui>();
            }
            // UltraGBOverlayFrame bypassed — trigger wallpaper load manually.
            ult::loadWallpaperFileWhenSafe();
            strncpy(g_pending_rom_path, romPathBuf, sizeof(g_pending_rom_path) - 1);
            g_pending_rom_path[sizeof(g_pending_rom_path) - 1] = '\0';
            return initially<GBOverlayGui>();
        }

        // ── Default — cold start, -returning, unexpected edge cases ──────────────
        return initially<RomSelectorGui>();
    }
};




// Clamp g_win_scale to the maximum the current heap tier can support.
// romPath is used only for the earlyExpanded (8 MB) 6× check; pass nullptr
// when the ROM path is not yet known (forces 5× on that tier).
// Called from main() before the framebuffer is sized — must run before tsl::loop.
static void clamp_win_scale(bool earlyLimited, bool earlyRegularMemory,
                             bool earlyExpanded, const char* romPath) {
    if (earlyLimited       && g_win_scale >= 4) g_win_scale = 3;
    if (earlyRegularMemory && g_win_scale >= 5) g_win_scale = 4;
    // 8 MB heap with a 4 MB+ ROM: the ROM itself consumes most of the heap,
    // leaving insufficient room for the scale-5 framebuffer (~1.1 MB extra).
    // Cap at 4× regardless of output mode.  ROM < 4 MB falls through to the
    // 6× logic below where docked + 1080p is still allowed.
    if (earlyExpanded && g_win_scale >= 5) {
        const size_t rsz = (romPath && romPath[0]) ? get_rom_size(romPath) : 0;
        if (rsz >= kROM_4MB) { g_win_scale = 4; return; }
    }
    if (g_win_scale < 6) return;
    // earlyExpanded == true means EXACTLY 8 MB.
    // For 10 MB+ none of earlyLimited/Regular/Expanded are true.
    const bool isFurtherExpanded = !earlyLimited && !earlyRegularMemory && !earlyExpanded;
    const bool consoleIsDocked = poll_console_docked();
    if (isFurtherExpanded) {
        // 10 MB+: 6× allowed for any ROM size; need docked + 1080p.
        if (!(consoleIsDocked && g_win_1080)) g_win_scale = 5;
    } else if (earlyExpanded) {
        // 8 MB: 6× only with docked + 1080p + ROM < 4 MB.
        const size_t rsz = (romPath && romPath[0]) ? get_rom_size(romPath) : 0;
        if (!(consoleIsDocked && g_win_1080 && rsz < kROM_4MB)) g_win_scale = 5;
    } else {
        // 4 MB / 6 MB — unreachable (already clamped to 3 or 4 above).
        g_win_scale = 5;
    }
}

// Finalise the windowed VI layer configuration after g_win_scale and g_win_1080
// are resolved.  Called from both the -quicklaunch and -windowed argv paths in
// main() — the 4-line block was identical at both sites.
static void setup_windowed_framebuffer() {
    g_win_scale_locked             = true;
    ult::DefaultFramebufferWidth   = GB_W * g_win_scale;
    ult::DefaultFramebufferHeight  = GB_H * g_win_scale;
    ult::windowedLayerPixelPerfect = g_win_1080 && poll_console_docked();
}

// =============================================================================
// Session type — resolved once in main() from argv + config.  Each enumerator
// corresponds to a distinct Overlay::initServices() / loadInitialGui() path.
//
// To add a new game mode:
//   1. Add an enumerator below.
//   2. Add its argv token to the Phase 1 parse loop.
//   3. Handle it in the Phase 2 resolution block.
//   4. Add matching branches in Overlay::initServices() / loadInitialGui().
// =============================================================================
enum class SessionType {
    Menu,           // cold start, -returning, quicklaunch-windowed tooLarge fallback
    Windowed,       // -windowed, or -quicklaunch with windowed=1 and ROM fits
    OverlayPlayer,  // -overlay  (ROM path written to config by launch_overlay_mode)
    QuickLaunch,    // -quicklaunch with windowed=0
};

int main(int argc, char* argv[]) {
    if (argc > 0)
        snprintf(g_self_path, sizeof(g_self_path), "sdmc:/switch/.overlays/%s", argv[0]);

    //ult::logFilePath    = "sdmc:/config/ultragb/log.txt";
    //ult::disableLogging = false;
    skipRumbleDoubleClick = false;

    const auto currentHeapSize    = ult::getCurrentHeapSize();
    const bool earlyLimited       = (currentHeapSize == ult::OverlayHeapSize::Size_4MB);
    const bool earlyRegularMemory = (currentHeapSize == ult::OverlayHeapSize::Size_6MB);
    const bool earlyExpanded      = (currentHeapSize == ult::OverlayHeapSize::Size_8MB);

    // ── Phase 1: parse argv into raw intent flags ─────────────────────────────
    // Flags only — no branching.  Session resolution happens in Phase 2.
    bool wantWindowed      = false;
    bool wantOverlayPlayer = false;
    bool wantQuickLaunch   = false;

    for (int i = 1; i < argc; ++i) {
        if      (strcmp(argv[i], "-returning")   == 0) { g_returning_from_windowed = true; skipRumbleDoubleClick = false; }
        else if (strcmp(argv[i], "--direct")     == 0) { g_directMode = true;              skipRumbleDoubleClick = false; }
        else if (strcmp(argv[i], "-overlay")     == 0) { g_overlay_mode = true; wantOverlayPlayer = true; skipRumbleDoubleClick = false; }
        else if (strcmp(argv[i], "-quicklaunch") == 0) { g_quick_launch = true; wantQuickLaunch   = true; }
        else if (strcmp(argv[i], "-windowed")    == 0) { wantWindowed   = true; }
    }

    // ── Phase 2: resolve session type ────────────────────────────────────────
    //
    // Reads config only when needed.  Sets globals (g_win_scale_locked,
    // g_quicklaunch_windowed_toobig) that Overlay::initServices() consumes.
    //
    // NOTE: rom_playability_message() is unavailable here — Tesla's memory-tier
    // atomics only initialise inside tsl::loop().  Use getCurrentHeapSize() /
    // get_rom_size() directly for pre-loop ROM size checks.
    SessionType session = SessionType::Menu;

    if (wantQuickLaunch) {
        const std::string lastRom = ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyLastRom);
        const bool        windowed = ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyWindowed) == "1";

        if (!lastRom.empty()) {
            if (windowed) {
                std::string romDir = ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyRomDir);
                if (romDir.empty()) romDir = "sdmc:/roms/gb/";
                if (romDir.back() != '/') romDir += '/';
                const std::string fullPath = romDir + lastRom;

                const size_t rsz      = get_rom_size(fullPath.c_str());
                const bool   tooLarge = rsz > 0 &&
                    ((earlyLimited       && rsz >= kROM_2MB) ||
                     (earlyRegularMemory && rsz >= kROM_4MB) ||
                     (earlyExpanded      && rsz >= kROM_6MB));

                if (!tooLarge) {
                    // Write transient keys consumed by initServices().
                    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWindowedRom,  fullPath);
                    ult::setIniFileValue(kConfigFile, kConfigSection, kKeyWinQuickExit, "1");
                    session = SessionType::Windowed;
                } else {
                    // ROM too large — open menu with a post-loop error notify.
                    g_quicklaunch_windowed_toobig = true;
                    session = SessionType::Menu;
                }
            } else {
                session = SessionType::QuickLaunch;
            }
        }
        // lastRom empty → stays SessionType::Menu.

    } else if (wantWindowed) {
        // Explicit relaunch — ROM path already in config under "windowed_rom".
        session = SessionType::Windowed;

    } else if (wantOverlayPlayer) {
        // ROM path already in config under "overlay_rom" (set by launch_overlay_mode).
        session = SessionType::OverlayPlayer;
    }

    // ── Phase 3: session-specific pre-loop setup, then single dispatch ────────
    //
    // Windowed sessions must size the VI layer before tsl::loop() because
    // load_config() only runs later inside initServices().
    // All other sessions need no pre-loop work beyond what Phase 2 already did.
    if (session == SessionType::Windowed) {
        g_win_1080  = (ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyWinOutput) == "1080");
        g_win_scale = parse_win_scale_str(ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyWinScale));
        const std::string wrom = ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyWindowedRom);
        clamp_win_scale(earlyLimited, earlyRegularMemory, earlyExpanded,
                        wrom.empty() ? nullptr : wrom.c_str());
        setup_windowed_framebuffer();   // sets g_win_scale_locked = true

    } else if (session == SessionType::Menu) {
        returnOverlayPath = ult::OVERLAY_PATH + "ovlmenu.ovl";
    }

    // One tsl::loop<Overlay> instantiation handles every session type.
    // Overlay::initServices() reads g_win_scale_locked / g_overlay_mode /
    // g_quick_launch to set m_game_session and route accordingly.
    return tsl::loop<Overlay, tsl::impl::LaunchFlags::None>(argc, argv);
}