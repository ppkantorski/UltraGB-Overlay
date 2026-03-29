/********************************************************************************
 * File: main.cpp
 * Description:
 *   Game GB / Game GBC Color emulator overlay for Nintendo Switch.
 *
 *   Config: sdmc:/config/ultragb/config.ini  (rom_dir=sdmc:/roms/gb/)
 *   Saves:  sdmc:/config/ultragb/saves/
 *
 *   Controls in-game:
 *     A / B / D-pad / + (Start) / - (Select) — mapped to GB buttons
 *     X — pause and return to ROM picker (state preserved)
 *     System overlay combo — hides overlay (game pauses automatically)
 ********************************************************************************/

#define NDEBUG
#define STBTT_STATIC
#define TESLA_INIT_IMPL

#include <ultra.hpp>
#include <tesla.hpp>

#include "gb_audio.h"       // ← GB APU → audout bridge  (must precede gb_core.h)
#include "gb_core.h"
#include "gb_renderer.h"
#include "elm_volume.hpp"    // ← VolumeTrackBar
#include "elm_ultraframe.hpp" // ← UltraGBOverlayFrame (two-page frame)

#include <dirent.h>
#include <ctime>
#include <vector>
#include <string>
#include <algorithm>

using namespace ult;

// =============================================================================
// Paths / config
// =============================================================================
static constexpr const char* CONFIG_DIR  = "sdmc:/config/ultragb/";
static constexpr const char* CONFIG_FILE = "sdmc:/config/ultragb/config.ini";
static constexpr const char* SAVE_DIR          = "sdmc:/config/ultragb/saves/";
static constexpr const char* STATE_BASE_DIR    = "sdmc:/config/ultragb/states/";
static constexpr const char* INTERNAL_STATE_DIR = "sdmc:/config/ultragb/states/internal/";
static constexpr const char* STATE_DIR         = INTERNAL_STATE_DIR; // alias — internal quick-resume
static constexpr const char* INTERNAL_SAVE_DIR  = "sdmc:/config/ultragb/saves/internal/";  // live .sav files
static constexpr const char* CONFIGURE_DIR     = "sdmc:/config/ultragb/configure/";
static char g_rom_dir[256]       = "sdmc:/roms/gb/";
static char g_last_rom_path[256]    = {};   // basename of last-played ROM, persisted to config.ini
static char g_pending_rom_path[256] = {};   // path deferred from click listener → loaded in GBEmulatorGui::createUI()
static char g_rom_selector_scroll[256] = {};  // last ROM the user interacted with in the selector; used to restore scroll on page flip
static char g_settings_scroll[64]      = {};  // last focused item label in SettingsGui; cleared on game launch, restored on page flip

// Set true by GBEmulatorGui when it exits to RomSelectorGui via X.
// RomSelectorGui reads and clears this in its constructor, then blocks all
// navigation input (and keeps clearing jumpToTop/Bottom/skipUp/skipDown) until every
// physical key is released — preventing L/R/ZL/ZR presses made in-game from
// firing a list-jump the moment the ROM selector appears.
static bool g_waitForInputRelease = false;

// Persistent config path string — avoids a heap alloc on every setIniFileValue call
// (CONFIG_FILE is 31 chars, exceeds ARM64 libstdc++ SSO threshold of 15, so each
// std::string(CONFIG_FILE) temporary heap-allocates.  This single persistent instance
// is reused everywhere in place of per-call temporaries).
static const std::string kConfigFile{CONFIG_FILE};

// Stack-only integer-to-string for 0–100 volume values.
// std::to_string heap-allocates; this writes into a caller-supplied char[4] buffer.
// Returns a pointer to buf (always null-terminated).
static const char* vol_to_str(u8 v, char (&buf)[4]) {
    if (v == 100) { buf[0]='1'; buf[1]='0'; buf[2]='0'; buf[3]='\0'; }
    else if (v >= 10) { buf[0]='0'+v/10; buf[1]='0'+v%10; buf[2]='\0'; }
    else               { buf[0]='0'+v;                      buf[1]='\0'; }
    return buf;
}

// Unmute backup volume — the last positive volume before muting.
// Persisted to vol_backup in config.ini so it survives app exit.
// Always > 0; initialized to 100 as a safe default.
static u8 g_vol_backup = 100;

// ── Windowed mode ─────────────────────────────────────────────────────────────
// When true the ROM selector relaunches this overlay with -windowed <path>,
// rendering the Game Boy screen as a small draggable window with no UI chrome.
static bool g_windowed_mode  = false;
static bool g_ingame_haptics = true;

// Set true by main() when the overlay is relaunched with the -returning argument
// (i.e. the user exited windowed mode via the launch combo back to the normal UI).
// Read and cleared in Overlay::initServices() to restore the settings scroll
// position that was persisted to config.ini just before the windowed relaunch.
static bool g_returning_from_windowed = false;

// Full path to this .ovl — captured from argv[0] so WindowedOverlay can
// pass it to setNextOverlay when the launch combo is pressed to return here.
static char g_self_path[256] = {};

// ROM path received via the -windowed <path> argument.
// Set in main(), consumed by WindowedOverlay::loadInitialGui().
static char g_win_rom_path[256] = {};

// Quick-launch combo string (raw, e.g. "L+R+DDOWN"). Empty = none configured.
// Populated from overlays.ini mode_combos[0] in register_quick_launch_mode().
static char g_quick_combo[64] = {};

// Set true by main() when launched with the -quicklaunch mode argument.
// Causes loadInitialGui() to skip the ROM selector and boot directly into
// the last played ROM.  KEY_X (overlay) and the hide combo (windowed) then
// close the overlay entirely instead of returning to the menu.
static bool g_quick_launch = false;

// Set true when a windowed quick-launch is triggered from loadInitialGui().
// Written to config.ini as "win_quick_exit=1" before the -windowed relaunch so
// WindowedOverlay::initServices() can read and clear it across the process boundary.
// When true, GBWindowedGui's exit combo closes the overlay entirely without
// relaunching the normal Overlay to return to the menu.
static bool g_win_quick_exit = false;

// Window position in VI-space (1920×1080).
// WIN_VI_W = 1920*160/1280 = 240, WIN_VI_H = 1080*144/720 = 216.
// Default: centred on the display.
static int g_win_pos_x = (1920 - 240) / 2;   // 840  (1× default centre)
static int g_win_pos_y = (1080 - 216) / 2;   // 432  (1× default centre)

// Windowed display scale: 1, 2, or 3 (integer pixel scale factor).
// 1× = 160×144 VI layer (240×216 in VI space)
// 2× = 320×288 VI layer (480×432 in VI space)
// 3× = 480×432 VI layer (720×648 in VI space)
// Read from config.ini as "win_scale"; default 1.
// g_win_scale is read directly in main() before tsl::loop so the
// framebuffer is sized correctly before Tesla initialises.
static int g_win_scale = 1;

// Tri-state screen mode:
//   0 = Auto  — resolves to 1080p if consoleIsDocked(), else 720p
//   1 = 720p  — VI is downscaled x2/3; all four scales pixel-perfect
//   2 = 1080p — VI maps 1:1; only even scales (2x/4x) are pixel-perfect
// Persisted to config.ini as "auto" / "0" / "1". Default: auto (0).
static int  g_win_hd_mode = 0;   // 0=Auto, 1=720p, 2=1080p

// Resolved boolean: true = 1080p, false = 720p.
// Set by resolve_win_hd() from g_win_hd_mode (+ consoleIsDocked() for Auto).
// All existing logic that reads the HD flag reads this bool unchanged.
static bool g_win_hd = false;

// Set g_win_hd from g_win_hd_mode.
// Auto (0): queries whether the Switch is currently docked; resolves to true
//           (1080p) when docked, false (720p) when handheld.
// Must be called any time g_win_hd_mode is written, and in both windowed
// launch paths inside main() before the pixel-perfect scale clamps run.
static void resolve_win_hd() {
    if      (g_win_hd_mode == 2) g_win_hd = true;
    else if (g_win_hd_mode == 1) g_win_hd = false;
    else                          g_win_hd = ult::consoleIsDocked();  // Auto
}

static void save_vol_backup() {
    char buf[4];
    ult::setIniFileValue(kConfigFile, "config", "vol_backup",
                         vol_to_str(g_vol_backup, buf), "");
}

// Digit-safe unsigned integer parser (no std::stoi — exceptions disabled).
// Returns true and writes out on success; false if s is empty or non-digit.
static bool parse_uint(const std::string& s, int& out) {
    if (s.empty()) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v; return true;
}

static void load_config() {
    const std::string& path = kConfigFile;

    // rom_dir
    const std::string rom_dir_val = ult::parseValueFromIniSection(path, "config", "rom_dir");
    if (!rom_dir_val.empty() && rom_dir_val.size() < sizeof(g_rom_dir) - 2) {
        strncpy(g_rom_dir, rom_dir_val.c_str(), sizeof(g_rom_dir) - 2);
        const size_t vlen = strlen(g_rom_dir);
        if (vlen > 0 && g_rom_dir[vlen - 1] != '/') {
            g_rom_dir[vlen]     = '/';
            g_rom_dir[vlen + 1] = '\0';
        }
    }

    // original_palette key is superseded by per-game palette_mode in configure/<rom>.ini
    // (kept for legacy: if a user has it in config.ini it is silently ignored now)

    // last_rom — basename of the last-played ROM file
    const std::string last_rom_val = ult::parseValueFromIniSection(path, "config", "last_rom");
    if (!last_rom_val.empty() && last_rom_val.size() < sizeof(g_last_rom_path) - 1)
        strncpy(g_last_rom_path, last_rom_val.c_str(), sizeof(g_last_rom_path) - 1);

    // volume — master GB audio volume (0–100), default 100
    { int v = 0; if (parse_uint(ult::parseValueFromIniSection(path, "config", "volume"), v))
        gb_audio_set_volume(static_cast<u8>(std::clamp(v, 0, 100))); }

    // vol_backup — unmute restore target (0 treated as absent → default 100)
    { int v = 0; if (parse_uint(ult::parseValueFromIniSection(path, "config", "vol_backup"), v))
        g_vol_backup = static_cast<u8>(std::clamp(v, 1, 100)); }

    // pixel_perfect — 0 = 2.5× (default), 1 = 2× pixel-perfect
    const std::string pp_val = ult::parseValueFromIniSection(path, "config", "pixel_perfect");
    if (!pp_val.empty())
        g_vp_2x = (pp_val == "1");

    // windowed — 0 = normal (default), 1 = windowed mode
    const std::string win_val = ult::parseValueFromIniSection(path, "config", "windowed");
    if (!win_val.empty())
        g_windowed_mode = (win_val == "1");

    const std::string hap_val = ult::parseValueFromIniSection(path, "config", "ingame_haptics");
    if (!hap_val.empty())
        g_ingame_haptics = (hap_val != "0");

    // win_pos_x / win_pos_y — persisted VI-space window position
    { int v = 0;
      if (parse_uint(ult::parseValueFromIniSection(path, "config", "win_pos_x"), v)) g_win_pos_x = v;
      if (parse_uint(ult::parseValueFromIniSection(path, "config", "win_pos_y"), v)) g_win_pos_y = v; }

    // win_scale — windowed display scale: 1, 2, 3, or 4 (default 1).
    // Any absent or unrecognised value falls back to 1.
    // Runtime clamps are applied below after win_hd is also read — config
    // value is never overwritten here.
    {
        const std::string sv = ult::parseValueFromIniSection(path, "config", "win_scale");
        if      (sv == "2") g_win_scale = 2;
        else if (sv == "3") g_win_scale = 3;
        else if (sv == "4") g_win_scale = 4;
        else                g_win_scale = 1;
    }

    // win_hd — screen mode: "auto" (default), "0" = 720p, "1" = 1080p.
    // Resolved to g_win_hd bool via resolve_win_hd() below.
    // In 1080p mode only even scales (2x / 4x) are pixel-perfect; odd scales
    // are clamped below without overwriting the saved config value.
    {
        const std::string hd = ult::parseValueFromIniSection(path, "config", "win_hd");
        if      (hd == "1")    g_win_hd_mode = 2;
        else if (hd == "0")    g_win_hd_mode = 1;
        else                   g_win_hd_mode = 0;  // "auto" or absent
        resolve_win_hd();
    }

    // Apply runtime clamps to g_win_scale now that both win_scale and win_hd
    // are known.  These mirror the identical clamps in main() that size the
    // framebuffer — load_config() runs inside initServices() after the FB is
    // already created, so g_win_scale must end up matching what main() used.
    // Config is never written here; stored values are preserved for round-trip.
    if (ult::limitedMemory && g_win_scale == 4)
        g_win_scale = 3;
    if (g_win_hd && g_win_scale % 2 != 0)
        g_win_scale = (g_win_scale == 1) ? 2 : 4;
    if (ult::limitedMemory && g_win_scale == 4)
        g_win_scale = g_win_hd ? 2 : 3;
}

// Write a config key with its default value only when the key is absent.
// Collapses the four-repetition parseValueFromIniSection → empty() → setIniFileValue
// sequence in write_default_config_if_missing into one shared body.
static void set_if_missing(const char* key, const char* def) {
    if (ult::parseValueFromIniSection(kConfigFile, "config", key).empty())
        ult::setIniFileValue(kConfigFile, "config", key, def);
}

static void write_default_config_if_missing() {
    set_if_missing("rom_dir",       g_rom_dir);
    set_if_missing("volume",        "100");
    set_if_missing("vol_backup",    "100");
    set_if_missing("pixel_perfect", "0");
    set_if_missing("windowed",      "0");
    set_if_missing("ingame_haptics", "1");
    set_if_missing("win_pos_x",     "840");
    set_if_missing("win_pos_y",     "432");
    set_if_missing("win_scale",     "1");
    set_if_missing("win_hd",        "auto");
}

// Persist the basename of the just-launched ROM so the selector can jump to it on re-entry.
static void save_last_rom(const char* fullPath) {
    const char* sl = strrchr(fullPath, '/');
    const char* base = sl ? sl + 1 : fullPath;
    strncpy(g_last_rom_path, base, sizeof(g_last_rom_path) - 1);
    ult::setIniFileValue(kConfigFile, "config", "last_rom", base, "");
}

// Persist the current scale mode to config.ini.
static void save_pixel_perfect() {
    ult::setIniFileValue(kConfigFile, "config", "pixel_perfect",
                         g_vp_2x ? "1" : "0", "");
}

// Persist the windowed mode toggle to config.ini.
static void save_windowed_mode() {
    ult::setIniFileValue(kConfigFile, "config", "windowed",
                         g_windowed_mode ? "1" : "0", "");
}

// Persist the dragged window position (VI coordinates) to config.ini.
// Called by GBWindowedGui on touch release after a successful drag.
static void save_win_pos() {
    ult::setIniFileValue(kConfigFile, "config", "win_pos_x",
                         std::to_string(g_win_pos_x), "");
    ult::setIniFileValue(kConfigFile, "config", "win_pos_y",
                         std::to_string(g_win_pos_y), "");
}

// Persist the windowed scale (1/2/3) to config.ini.
// The new scale takes effect the next time the user launches a ROM in
// windowed mode (setNextOverlay reads it before Tesla initialises the layer).
static void save_win_scale() {
    const char* s = (g_win_scale == 4) ? "4"
                  : (g_win_scale == 3) ? "3"
                  : (g_win_scale == 2) ? "2"
                  :                      "1";
    ult::setIniFileValue(kConfigFile, "config", "win_scale", s, "");
}

// Persist the screen mode tri-state to config.ini.
// "auto" = detect via consoleIsDocked(), "0" = force 720p, "1" = force 1080p.
static void save_win_hd() {
    const char* val = (g_win_hd_mode == 2) ? "1"
                    : (g_win_hd_mode == 1) ? "0"
                    :                        "auto";
    ult::setIniFileValue(kConfigFile, "config", "win_hd", val, "");
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
static const std::array<std::string, 28> g_defaultCombos = {{
    "ZL+ZR+DDOWN",  "ZL+ZR+DRIGHT", "ZL+ZR+DUP",    "ZL+ZR+DLEFT",
    "L+R+DDOWN",    "L+R+DRIGHT",   "L+R+DUP",       "L+R+DLEFT",
    "L+DDOWN",      "R+DDOWN",
    "ZL+ZR+PLUS",   "L+R+PLUS",     "ZL+ZR+MINUS",   "L+R+MINUS",
    "ZL+MINUS",     "ZR+MINUS",     "ZL+PLUS",        "ZR+PLUS",    "MINUS+PLUS",
    "LS+RS",        "L+DDOWN+RS",   "L+R+LS",         "L+R+RS",
    "ZL+ZR+LS",     "ZL+ZR+RS",     "ZL+ZR+L",        "ZL+ZR+R",    "ZL+ZR+LS+RS"
}};

// Remove keyCombo from every overlay's key_combo AND mode_combos list in
// overlays.ini, skipping our own section.  Called before assigning the combo
// so no two overlays share the same trigger — mirrors Ultrahand's behaviour.
static void remove_quick_combo_from_others(const std::string& keyCombo) {
    if (keyCombo.empty()) return;
    if (!ult::isFile(ult::OVERLAYS_INI_FILEPATH)) return;

    const std::string own = ovl_filename();
    auto data = ult::getParsedDataFromIniFile(ult::OVERLAYS_INI_FILEPATH);
    bool dirty = false;

    for (auto& [name, section] : data) {
        if (!own.empty() && name == own) continue;   // skip ourselves

        // 1. Main key_combo field
        auto kcIt = section.find("key_combo");
        if (kcIt != section.end() && !kcIt->second.empty() &&
            tsl::hlp::comboStringToKeys(kcIt->second) ==
            tsl::hlp::comboStringToKeys(keyCombo)) {
            kcIt->second = "";
            dirty = true;
        }

        // 2. mode_combos list — clear every matching slot
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

    auto iniData = ult::getParsedDataFromIniFile(ult::OVERLAYS_INI_FILEPATH);
    auto& section = iniData[filename];

    static constexpr const char* kExpectedArgs   = "(-quicklaunch)";
    static constexpr const char* kExpectedLabels = "(Quick Launch)";

    if (section["mode_args"] != kExpectedArgs) {
        section["mode_args"]   = kExpectedArgs;
        section["mode_labels"] = kExpectedLabels;
        ult::saveIniFileData(ult::OVERLAYS_INI_FILEPATH, iniData);
    }

    // Refresh g_quick_combo from the stored mode_combos[0].
    g_quick_combo[0] = '\0';
    const std::string mc = section["mode_combos"];
    const auto comboList = splitIniList(mc);
    if (!comboList.empty() && !comboList[0].empty())
        strncpy(g_quick_combo, comboList[0].c_str(), sizeof(g_quick_combo) - 1);
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
// Returns true only for ROMs that REQUIRE CGB hardware (0xC0, bit6 set).
// CGB-only games cannot run in DMG mode; their palette is always "GBC".
static bool rom_is_cgb_only(const char* romPath) {
    return (rom_cgb_flag(romPath) & 0xC0) == 0xC0;
}
// Returns true for ROMs that support CGB but can also run in DMG mode (0x80, bit6 clear).
// These games may use a user-selected DMG palette.
static bool rom_is_cgb_compat(const char* romPath) {
    const uint8_t f = rom_cgb_flag(romPath);
    return (f & 0x80) != 0 && (f & 0x40) == 0;
}

static void build_game_config_path(const char* romPath, char* out, size_t outSz) {
    const char* sl = strrchr(romPath, '/');
    const char* base = sl ? sl + 1 : romPath;
    snprintf(out, outSz, "%s%s.ini", CONFIGURE_DIR, base);
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
    char cfgPath[640];
    build_game_config_path(romPath, cfgPath, sizeof(cfgPath));
    return str_to_palette_mode(
        ult::parseValueFromIniSection(cfgPath, "config", "palette_mode"));
}

static void save_game_palette_mode(const char* romPath, PaletteMode m) {
    char cfgPath[640];
    build_game_config_path(romPath, cfgPath, sizeof(cfgPath));
    ult::createDirectory(CONFIGURE_DIR);
    ult::setIniFileValue(cfgPath, "config", "palette_mode", palette_mode_to_str(m), "");
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
    char cfgPath[640];
    build_game_config_path(romPath, cfgPath, sizeof(cfgPath));
    const std::string val = ult::parseValueFromIniSection(cfgPath, "config", "lcd_ghosting");
    return val == "1";
}

static void save_game_lcd_ghosting(const char* romPath, bool enabled) {
    char cfgPath[640];
    build_game_config_path(romPath, cfgPath, sizeof(cfgPath));
    ult::createDirectory(CONFIGURE_DIR);
    ult::setIniFileValue(cfgPath, "config", "lcd_ghosting", enabled ? "1" : "0", "");
}


// ROM buffer is allocated in gb_load_rom and freed in gb_unload_rom.
// The wallpaper is always evicted BEFORE the ROM buffer is allocated, so that
// the new malloc sees the maximum possible contiguous free region.

GBState  g_gb;
uint16_t* g_gb_fb = nullptr;   // ~45 KB heap-allocated on game load, freed on unload
static bool g_emu_active = false;
static bool g_wallpaper_evicted = false;  // true when wallpaper was cleared for a large ROM
PaletteMode g_palette_mode   = PaletteMode::GBC;  // per-game; GBC title-lookup by default
bool g_fb_is_rgb565           = false;  // true when CGB mode; set in gb_load_rom
bool g_fb_is_prepacked        = false;  // true when g_gb_fb stores RGBA4444 (DMG games)
bool g_vp_2x                  = false;  // false=2.5× (default), true=2× pixel-perfect
bool g_lcd_ghosting           = false;  // 50/50 frame blend — simulates GBC LCD persistence; per-game, off by default
bool g_fast_forward           = false;  // true while ZR double-click-hold is active

// Monotonically incrementing display-frame counter used for double-click timing.
static uint32_t g_frame_count = 0;

// Active DMG palette sub-arrays — written by gb_select_dmg_palette(), read every scanline.
uint16_t g_dmg_flat_pal[64]  = {};   // filled by gb_select_dmg_palette()
int  g_gbc_pal_idx   = -1;           // index into GBC_TITLE_TABLE (-1 = not found)
bool g_gbc_pal_found = false;        // true if ROM was recognised in GBC_TITLE_TABLE
// =============================================================================
// Virtual on-screen button layout (Game GB skin)
// All coordinates are in overlay pixels (448 × 720).
// The screen bottom sits at VP_Y + VP_H = 140 + 360 = 500, leaving 220 px of
// controller area below it.  Layout mirrors a real DMG Game GB:
//   D-pad   — lower-left
//   B / A   — lower-right, staggered (B lower-left of A, matching real GB)
//   Start   — centre, near bottom
//
// Draw positions (top-left x, baseline y) are compile-time constants.
// Hit-test centres are derived at runtime from getTextDimensions() so they
// track the glyph's actual visual centre regardless of font metrics.
// =============================================================================

// D-pad — \uE110
static constexpr int DPAD_DRAW_X = 29;    // left edge — fixed, glyph grows right
static constexpr int DPAD_DRAW_Y = 634+4-4+14-4-2-2-2;   // baseline
static constexpr int DPAD_SIZE   = 134;   // 112 × 1.2 — 20% larger, grows right/down from fixed top-left
static constexpr int DPAD_R      = 65;    // 54 × 1.2 — hit radius scaled to match

// A button — \uE0E0
static constexpr int ABTN_DRAW_X = 344;   // left edge
static constexpr int ABTN_DRAW_Y = 594-4-2-2;   // baseline
static constexpr int ABTN_SIZE   = 72;
static constexpr int ABTN_R      = 38;

// B button — \uE0E1
static constexpr int BBTN_DRAW_X = 289;   // left edge
static constexpr int BBTN_DRAW_Y = 633-4-2-2;   // baseline
static constexpr int BBTN_SIZE   = 58;
static constexpr int BBTN_R      = 31;

// Start (+) — \uE0EF   and   Select (−) — \uE0F0
// Both buttons are centred symmetrically about the overlay midline (FB_W/2 = 224).
// A gap of 16 px separates the two glyphs (8 px each side of centre).
// IMPORTANT: must stay above FOOTER_Y (FB_H - 73 = 647).  Any touch whose
// *initial* y is >= FOOTER_Y is treated by the framework as a footer button
// press (back / select), firing rumble and injecting KEY_B / KEY_A even when
// the bottom bar is not rendered.
static constexpr int FOOTER_Y     = FB_H;   // 720
static constexpr int BTN_GAP_HALF = 10;     // half-gap between the two symbols (px)
static constexpr int START_SIZE   = 30;
static constexpr int START_R      = 24;
// Plus sits to the RIGHT of centre: its left edge is at (FB_W/2 + BTN_GAP_HALF).
static constexpr int START_DRAW_X = FB_W / 2 + BTN_GAP_HALF;
static constexpr int START_DRAW_Y = 626 + 44 + 6;    // baseline — lowered 6 px, keep below FOOTER_Y
// Select size matches Start for a balanced look.
static constexpr int SELECT_SIZE  = START_SIZE;
static constexpr int SELECT_R     = START_R;
// Minus sits to the LEFT of centre: its right edge is at (FB_W/2 - BTN_GAP_HALF),
// so its left edge (draw origin) is (FB_W/2 - BTN_GAP_HALF - SELECT_SIZE).
static constexpr int SELECT_DRAW_X = FB_W / 2 - BTN_GAP_HALF - SELECT_SIZE;
static constexpr int SELECT_DRAW_Y = START_DRAW_Y;   // same baseline as Start

// Solid grey — fully opaque so glyphs read clearly against the wallpaper
static const tsl::Color& VBTN_COLOR = tsl::buttonColor;


// Hit-test centres, populated on first draw from getTextDimensions().
// Initialised to geometric centres so they work even before measurement.
static int g_dpad_hx  = DPAD_DRAW_X  + DPAD_SIZE  / 2;
static int g_dpad_hy  = DPAD_DRAW_Y  - DPAD_SIZE  / 2;
static int g_abtn_hx  = ABTN_DRAW_X  + ABTN_SIZE  / 2;
static int g_abtn_hy  = ABTN_DRAW_Y  - ABTN_SIZE  / 2;
static int g_bbtn_hx  = BBTN_DRAW_X  + BBTN_SIZE  / 2;
static int g_bbtn_hy  = BBTN_DRAW_Y  - BBTN_SIZE  / 2;
static int g_start_hx  = START_DRAW_X  + START_SIZE  / 2;
static int g_start_hy  = START_DRAW_Y  - START_SIZE  / 2;
static int g_select_hx = SELECT_DRAW_X + SELECT_SIZE / 2;
static int g_select_hy = SELECT_DRAW_Y - SELECT_SIZE / 2;
static bool g_btns_measured = false;
static int  g_div_half_w   = 0;  // half-width of DIVIDER_SYMBOL at START_SIZE, set on first draw
static float g_dpad_glyph_w = 0.f; // measured width  of "\uE115" at DPAD_SIZE (set with g_btns_measured)
static float g_dpad_glyph_h = 0.f; // measured height of "\uE115" at DPAD_SIZE (set with g_btns_measured)

// Virtual key bitmask accumulated each frame from touch input.
static u64 g_touch_keys = 0;

// True GB frame period: 70224 T-cycles / 4194304 Hz ≈ 16.743 ms (59.73 fps).
// Used to rate-limit gb_run_one_frame() independently of the 60 fps display vsync.
static constexpr int64_t GB_RENDER_FRAME_NS =
    (int64_t)70224 * 1'000'000'000LL / (int64_t)4194304;  // 16742706 ns

// GB frame clock: 0 = unanchored, set to real time on first draw after load/resume.
static int64_t g_gb_frame_next_ns = 0;

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
    char bn[256] = {};
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
    ult::createDirectory(INTERNAL_SAVE_DIR);
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

    char statePath[256] = {};
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

    char statePath[256] = {};
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

// Full path to a user slot state file.
static void build_user_slot_path(const char* romPath, int slot, char* out, size_t outSz) {
    char dir[512] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), STATE_BASE_DIR);
    snprintf(out, outSz, "%sslot_%d.state", dir, slot);
}

// Full path to a user slot timestamp file.
static void build_user_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz) {
    char dir[512] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), STATE_BASE_DIR);
    snprintf(out, outSz, "%sslot_%d.ts", dir, slot);
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
    char tsPath[640] = {};
    build_user_slot_ts_path(romPath, slot, tsPath, sizeof(tsPath));
    write_timestamp_to(tsPath);
}
static void read_slot_timestamp(const char* romPath, int slot, char* out, size_t outSz) {
    char tsPath[640] = {};
    build_user_slot_ts_path(romPath, slot, tsPath, sizeof(tsPath));
    read_timestamp_from(tsPath, out, outSz);
}

// Returns the label ("Slot N") of the most-recently written slot file,
// or "" if all slots are empty.  Uses the .ts file mtime so the comparison
// is byte-exact regardless of the DIVIDER_SYMBOL encoding.
static std::string newest_state_slot_label(const char* romPath) {
    time_t best = -1;
    int    best_slot = -1;
    for (int i = 0; i < 10; ++i) {
        char tsPath[640] = {};
        build_user_slot_ts_path(romPath, i, tsPath, sizeof(tsPath));
        struct stat st{};
        if (::stat(tsPath, &st) == 0 && st.st_mtime > best) {
            best = st.st_mtime;
            best_slot = i;
        }
    }
    if (best_slot < 0) return "";
    char label[16];
    snprintf(label, sizeof(label), "Slot %d", best_slot);
    return label;
}

static void build_save_backup_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz); // forward declare

static std::string newest_save_backup_slot_label(const char* romPath) {
    time_t best = -1;
    int    best_slot = -1;
    for (int i = 0; i < 10; ++i) {
        char tsPath[640] = {};
        build_save_backup_slot_ts_path(romPath, i, tsPath, sizeof(tsPath));
        struct stat st{};
        if (::stat(tsPath, &st) == 0 && st.st_mtime > best) {
            best = st.st_mtime;
            best_slot = i;
        }
    }
    if (best_slot < 0) return "";
    char label[16];
    snprintf(label, sizeof(label), "Slot %d", best_slot);
    return label;
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
    char dir[512] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), SAVE_DIR);
    snprintf(out, outSz, "%sslot_%d.sav", dir, slot);
}

// Full path to a save-data backup slot timestamp file.
static void build_save_backup_slot_ts_path(const char* romPath, int slot, char* out, size_t outSz) {
    char dir[512] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), SAVE_DIR);
    snprintf(out, outSz, "%sslot_%d.ts", dir, slot);
}

// Wrappers using the generic timestamp core.
static void write_save_backup_timestamp(const char* romPath, int slot) {
    char tsPath[640] = {};
    build_save_backup_slot_ts_path(romPath, slot, tsPath, sizeof(tsPath));
    write_timestamp_to(tsPath);
}
static void read_save_backup_timestamp(const char* romPath, int slot, char* out, size_t outSz) {
    char tsPath[640] = {};
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
    char internalPath[256] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), INTERNAL_SAVE_DIR, ".sav");
    if (!file_exists(internalPath)) return false;

    char dir[512] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), SAVE_DIR);
    ult::createDirectory(dir);

    char slotPath[640] = {};
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
    char slotPath[640] = {};
    build_save_backup_slot_path(romPath, slot, slotPath, sizeof(slotPath));
    if (!file_exists(slotPath)) return false;

    char internalPath[256] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), INTERNAL_SAVE_DIR, ".sav");

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
    char internalPath[256] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), INTERNAL_STATE_DIR, ".state");
    if (!file_exists(internalPath)) return false;

    // Ensure per-game directory exists before writing
    char dir[512] = {};
    build_game_slot_dir(romPath, dir, sizeof(dir), STATE_BASE_DIR);
    ult::createDirectory(dir);

    char slotPath[640] = {};
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
    char slotPath[640] = {};
    build_user_slot_path(romPath, slot, slotPath, sizeof(slotPath));
    if (!file_exists(slotPath)) return false;

    char internalPath[256] = {};
    build_rom_data_path(romPath, internalPath, sizeof(internalPath), INTERNAL_STATE_DIR, ".state");

    copy_file(slotPath, internalPath);
    return file_exists(internalPath);
}



// Post a notification banner.  The null-check and string concatenation is
// duplicated at every call site — one helper eliminates five copies of the
// ult::NOTIFY_HEADER + "literal" temporary construction.
static void show_notify(const char* msg) {
    if (tsl::notification)
        tsl::notification->showNow(ult::NOTIFY_HEADER + msg);
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

// ─────────────────────────────────────────────────────────────────────────────
// Shared helpers — eliminate repeated patterns across the codebase
// ─────────────────────────────────────────────────────────────────────────────

// Exit UI audio if sound effects are enabled.  Repeated at three launch sites.
static inline void audio_exit_if_enabled() {
    if (ult::useSoundEffects && !ult::limitedMemory) ult::Audio::exit();
}

// Attempt a windowed relaunch for romPath.  Writes the path to config.ini,
// calls setNextOverlay, and closes.  Returns false when windowed mode is off.
static bool launch_windowed(const char* romPath) {
    if (!g_windowed_mode || !g_self_path[0]) return false;
    ult::setIniFileValue(kConfigFile, "config", "windowed_rom", romPath, "");
    // Persist the current settings-page scroll position so it survives the
    // overlay process restart.  The re-launched overlay reads and erases this
    // transient key in initServices() only when -returning is detected, so a
    // cold launch or abnormal exit never accidentally restores a stale value.
    if (g_settings_scroll[0])
        ult::setIniFileValue(kConfigFile, "config", "settings_scroll",
                             std::string(g_settings_scroll), "");
    tsl::setNextOverlay(std::string(g_self_path), "-windowed");
    tsl::Overlay::get()->close();
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
    if (ult::limitedMemory && sz >= kROM_2MB && sz < kROM_4MB) {
        show_notify("Requires at least 6MB.");
        fclose(f); return false;
    }
    // Default (6 MB heap): reject ROMs > 2 MB.
    if (!ult::expandedMemory && sz >= kROM_4MB && sz < kROM_6MB) {
        show_notify("Requires at least 8MB.");
        fclose(f); return false;
    }
    if (!ult::furtherExpandedMemory && sz >= kROM_6MB) {
        show_notify("Requires at least 10MB.");
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
        show_notify("Not enough memory.");
        maybe_reload_wallpaper();
        return false;
    }

    // Step 4 — Allocate the new ROM buffer.
    // At this point: glyph cache flushed, wallpaper freed (if need_evict), old
    // ROM freed, old cartRam freed, DMA buffers pre-allocated.
    uint8_t* rom_buf = static_cast<uint8_t*>(malloc(sz));
    if (!rom_buf) {
        fclose(f);
        show_notify("Not enough memory.");
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
    build_rom_data_path(path, g_gb.savePath, sizeof(g_gb.savePath), INTERNAL_SAVE_DIR, ".sav");

    // Allocate the core emulator struct — contains WRAM (32 KB) + VRAM (16 KB) + OAM/HRAM.
    // Lives on the heap so these ~49 KB are only resident during active gameplay.
    g_gb.gb = static_cast<struct gb_s*>(calloc(1, sizeof(struct gb_s)));
    if (!g_gb.gb) {
        gb_cancel_load();
        show_notify("Not enough memory.");
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

    // Allocate the framebuffer — ~45 KB heap, only resident during active gameplay.
    g_gb_fb = static_cast<uint16_t*>(calloc(GB_W * GB_H, sizeof(uint16_t)));
    if (!g_gb_fb) {
        gb_cancel_load();
        show_notify("Not enough memory.");
        return false;
    }

    // Clear the framebuffer BEFORE gb_reset() — peanut_gb fires gb_lcd_draw_line
    // for line 0 during reset itself.  If memset runs after, that first draw is
    // erased and row 0 stays black until the second frame completes.
    memset(g_gb_fb, 0, GB_W * GB_H * sizeof(uint16_t));

    // ── Attempt to restore a previously saved state ───────────────────────────
    // save_state() is called from gb_unload_rom() so every clean unload leaves a
    // .state file.  If it loads successfully the CPU/memory/framebuffer are
    // restored exactly where the player left off.  On failure (no file, wrong
    // version, truncated) we fall through to a normal cold boot — no harm done.
    bool apu_restored = false;
    const bool resumed = load_state(g_gb, &apu_restored);

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

    // Keep ghosting buffers allocated across ROM switches — freeing and
    // re-mallocing 68 KB every game creates heap holes on the 6 MB tier.
    // reset_lcd_ghosting() clears the valid flag so the first frame of the
    // next game seeds s_prev_fb fresh (no inter-game pixel bleed).

    //reset_lcd_ghosting(); // WRONG WE MUST TO FREE IT NOT RESET

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
// rom_is_playable — size-tier check used by the ROM selector
//
// Returns true when the ROM at |path| can be loaded on the current memory tier.
// Mirrors the rejection conditions in gb_load_rom() so the selector can dim
// unplayable entries BEFORE the user tries to launch them.
// =============================================================================
static bool rom_is_playable(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const size_t sz = static_cast<size_t>(ftell(f));
    fclose(f);
    if (!sz) return false;
    if (ult::limitedMemory          && sz >= kROM_2MB) return false;
    if (!ult::expandedMemory        && sz >= kROM_4MB) return false;
    if (!ult::furtherExpandedMemory && sz >= kROM_6MB) return false;
    return true;
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
    if (keysHeld & KEY_MINUS) joy &= ~(1u << 2);
    if (keysHeld & KEY_PLUS)  joy &= ~(1u << 3);
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

// draw_ultraboy_title — shared animated title renderer.
// Mirrors the launcher pattern exactly:
//   Dynamic: "Ultra" = wave between dynamicLogoRGB2 and dynamicLogoRGB1
//            "GB"   = logoColor2 (fixed accent, same as "hand")
//   Static:  "Ultra" = logoColor1 (white)
//            "GB"   = logoColor2 (fixed accent)
// Uses a stack char[2] buffer per letter to avoid per-call heap allocation.
static s32 draw_ultraboy_title(tsl::gfx::Renderer* renderer,
                                const s32 x, const s32 y, const u32 fontSize) {
    static constexpr double CYCLE  = 1.6;
    static constexpr double WSCALE = 2.0 * ult::_M_PI / CYCLE;
    static constexpr double PSHIFT = ult::_M_PI / 2.0;

    // Stack buffer: avoids std::string heap allocation per letter per frame
    char buf[2] = {0, 0};
    s32 cx = x;

    if (ult::useDynamicLogo) {
        // Compute time base once outside the letter loop
        const double t = std::fmod(static_cast<double>(ult::nowNs()) / 1e9, CYCLE);
        float offset = 0.f;
        for (const char ch : ult::SPLIT_PROJECT_NAME_1) {
            const double p  = WSCALE * (t + static_cast<double>(offset));
            const double rp = (ult::cos(p - PSHIFT) + 1.0) * 0.5;
            const double s1 = rp * rp * (3.0 - 2.0 * rp);
            const double bl = s1 * s1 * (3.0 - 2.0 * s1);
            const tsl::Color col = lerpColor(tsl::dynamicLogoRGB2, tsl::dynamicLogoRGB1,
                                             std::max(0.0, std::min(1.0, bl)));
            buf[0] = ch;
            cx += renderer->drawString(buf, false, cx, y, fontSize, col).first;
            offset -= static_cast<float>(CYCLE / 8.0);
        }
    } else {
        for (const char ch : ult::SPLIT_PROJECT_NAME_1) {
            buf[0] = ch;
            cx += renderer->drawString(buf, false, cx, y, fontSize, tsl::logoColor1).first;
        }
    }
    cx += renderer->drawString("GB", false, cx, y, fontSize, tsl::logoColor2).first;
    return cx;
}

// =============================================================================
// GBScreenElement — drawing only; no input handling (input lives on Gui)
// =============================================================================
class GBScreenElement : public tsl::elm::Element {
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
        if (ult::expandedMemory)
            renderer->drawWallpaper();
    
        //renderer->drawRect(15, tsl::cfg::FramebufferHeight - 73, tsl::cfg::FramebufferWidth - 30, 1, renderer->a(tsl::bottomSeparatorColor));
        
        draw_ultraboy_title(renderer, 20, 67, 50);

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
        {
            ++g_frame_count;

            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const int64_t now_ns = (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;

            if (g_gb_frame_next_ns == 0)
                g_gb_frame_next_ns = now_ns;  // anchor on first draw

            if (g_fast_forward) {
                // ── Fast-forward path ─────────────────────────────────────────
                // Run 4 GB frames per display tick.  Audio is paused (set in
                // handleInput when FF activates) so APU events are drained by the
                // audio thread rather than accumulating.  Ghosting is also skipped —
                // blending across 4 frames would produce motion smear.
                for (int f = 0; f < 4; ++f)
                    gb_run_one_frame();
                // Re-anchor clock so that when FF releases we don't get a
                // catch-up burst of normal frames.
                g_gb_frame_next_ns = now_ns + GB_RENDER_FRAME_NS;
            } else if (now_ns >= g_gb_frame_next_ns) {
                gb_run_one_frame();
                apply_lcd_ghosting();   // 50/50 blend of raw current vs raw prev frame
                gb_audio_submit();

                g_gb_frame_next_ns += GB_RENDER_FRAME_NS;
                if (g_gb_frame_next_ns < now_ns)
                    g_gb_frame_next_ns = now_ns + GB_RENDER_FRAME_NS;
            }
        }

        render_gb_letterbox(renderer);
        render_gb_screen(renderer);
        render_gb_border(renderer);
        render_gbc_logo(renderer);

        //const char* sl = strrchr(g_gb.romPath, '/');
        //renderer->drawString(sl ? sl+1 : g_gb.romPath, false,
        //    VP_X+4, VP_Y-14, 12, tsl::defaultTextColor);
        {
            static const std::vector<std::string> backButton = {"\uE0E2"};
            static const std::string backStr = std::string("\uE0E2 ") + ult::BACK;
            static const auto [bw, bh] = renderer->getTextDimensions(backStr, false, 15);
            renderer->drawStringWithColoredSections(backStr, false, backButton,
                VP_X + VP_W - bw, VP_Y+VP_H+16+2, 15, tsl::defaultTextColor, VBTN_COLOR);
        }

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
            renderer->drawRectAdaptive(DPAD_CX - (ARM_W + LIP*2)/2, DPAD_CY - (FULL + LIP*2)/2 + 4,
                               ARM_W + LIP*2, FULL + LIP*2 + 2, BK);
            // Horizontal bar — shifted 1px lower
            renderer->drawRectAdaptive(DPAD_CX - (FULL + LIP*2)/2 -1, DPAD_CY - (ARM_H + LIP*2)/2 + 6,
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
            FB_W / 2 - g_div_half_w, START_DRAW_Y + 1, START_SIZE, tsl::textSeparatorColor);
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

class RomSelectorGui;       // forward declare
class GameConfigGui;        // forward declare
class SettingsGui;          // forward declare
class SaveStatesGui;        // forward declare
class SlotActionGui;        // forward declare
class SaveDataGui;          // forward declare
class SaveDataSlotActionGui; // forward declare
class QuickComboSelectorGui; // forward declare
// =============================================================================
// GBEmulatorGui — input handled at the Gui level (same pattern as TetrisGui)
// =============================================================================
class GBEmulatorGui : public tsl::Gui {
    bool m_waitForRelease = true;  // ignore input until all buttons are released
    u64  m_prevTouchKeys  = 0;     // track previous touch state
    bool m_vp_tap_pending = false; // true while a screen-region tap is in progress
    int  m_vp_tap_frames  = 0;     // frames held so far for the current tap
    bool m_rs_tap_pending = false; // true while an RS quick-release is in progress
    int  m_rs_tap_frames  = 0;     // frames RS has been held so far
    bool m_load_failed    = false; // deferred load failed; swap back to selector in update()

    // ── ZR double-click-hold → fast-forward ───────────────────────────────
    // First ZR press arms the detector; a second press within ZR_DCLICK_WINDOW
    // frames that is then held activates fast-forward until ZR is released.
    static constexpr int ZR_DCLICK_WINDOW = 20; // ~333 ms at 60 fps
    bool     m_zr_first_seen  = false; // true after first ZR press is recorded
    uint32_t m_zr_first_frame = 0;     // g_frame_count when first press fired
public:
    ~GBEmulatorGui() {
        tsl::gfx::FontManager::clearCache(); // ALWAYS CLEAR BEFORE
    }

    virtual tsl::elm::Element* createUI() override {
        // ── Deferred ROM load ─────────────────────────────────────────────────
        // g_pending_rom_path is set by the click listener instead of calling
        // gb_load_rom() there.  By the time createUI() runs, ~RomSelectorGui()
        // has already destroyed all MiniListItem objects and their std::string
        // labels, so the heap is fully defragmented before we call malloc(ROM).
        // We also clear the glyph cache here (belt-and-suspenders: ~RomSelectorGui
        // already cleared it, but a second clear is free if the cache is empty).
        if (g_pending_rom_path[0] != '\0') {
            tsl::gfx::FontManager::clearCache(); // ensure glyphs are gone

            char path[256];
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
        return new GBScreenElement();
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

        // ── Screen-region tap → scale toggle ─────────────────────────────────
        // A quick tap (< 20 frames, ~333 ms) anywhere on the GB screen area
        // that doesn't hit a game button toggles between 2.5× and 2× scale.
        // Game buttons are below the screen so there is zero overlap — the VP
        // is a completely dead zone for all other input.
        {
            const bool touching = (touchPos.x != 0 || touchPos.y != 0) &&
                                  static_cast<int>(touchPos.y) < FOOTER_Y;
            const int tx = static_cast<int>(touchPos.x) - static_cast<int>(ult::layerEdge);
            const int ty = static_cast<int>(touchPos.y);
            const bool in_vp = touching &&
                tx >= vp_x() && tx < vp_x() + vp_w() &&
                ty >= vp_y() && ty < vp_y() + vp_h();

            if (in_vp && !m_vp_tap_pending) {
                // Touch just entered the screen region — arm the tap detector.
                m_vp_tap_pending = true;
                m_vp_tap_frames  = 0;
            }
            if (m_vp_tap_pending) {
                if (!touching) {
                    // Finger lifted — it was a quick tap, fire the toggle.
                    if (m_vp_tap_frames < 20) {
                        toggle_vp_scale();
                        save_pixel_perfect();
                    }
                    m_vp_tap_pending = false;
                } else if (!in_vp) {
                    // Finger dragged out of the VP without releasing — not a tap.
                    m_vp_tap_pending = false;
                } else {
                    ++m_vp_tap_frames;
                }
            }
        }

        // ── RS quick-release → scale toggle ──────────────────────────────────
        // A quick press-and-release of RS (right stick click) with no other
        // buttons held toggles between 2.5× and 2× pixel-perfect scale and
        // persists the choice to config.ini.  "Quick" = released within 20
        // frames (~333 ms).  Requiring keysHeld == KEY_RSTICK during the hold
        // ensures accidental combos (e.g. RS + ZL for system overlay) don't fire.
        {
            const bool rs_down      = (keysDown & KEY_RSTICK) != 0;
            const bool rs_held_alone = (keysHeld == KEY_RSTICK);

            if (rs_down && !m_rs_tap_pending) {
                m_rs_tap_pending = true;
                m_rs_tap_frames  = 0;
            }
            if (m_rs_tap_pending) {
                if (!(keysHeld & KEY_RSTICK)) {
                    // RS released — fire toggle only if it was a quick solo tap.
                    if (m_rs_tap_frames < 20) {
                        toggle_vp_scale();
                        save_pixel_perfect();
                    }
                    m_rs_tap_pending = false;
                } else if (!rs_held_alone) {
                    // Another button joined the hold — abort, treat as a combo.
                    m_rs_tap_pending = false;
                } else {
                    ++m_rs_tap_frames;
                }
            }
        }

        // ── ZR double-click-hold → fast-forward ──────────────────────────────
        // First ZR press arms the detector.  A second ZR press within
        // ZR_DCLICK_WINDOW frames (~333 ms) that is then held activates
        // fast-forward for as long as ZR remains held.
        // On activate: audio is paused — the audio thread drains the SPSC ring
        // silently and preserves all GBAPU state, so playback resumes cleanly.
        // On release: audio resumes and the frame clock is re-anchored so there
        // is no catch-up burst of normal frames after the fast-forward ends.
        {
            const bool zr_down = (keysDown & KEY_ZR) != 0;
            const bool zr_held = (keysHeld  & KEY_ZR) != 0;

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
        g_touch_keys = 0;
        if ((touchPos.x != 0 || touchPos.y != 0) &&
            static_cast<int>(touchPos.y) < FOOTER_Y) {
            const int tx = static_cast<int>(touchPos.x) - static_cast<int>(ult::layerEdge);
            const int ty = static_cast<int>(touchPos.y);

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

        // X → back to ROM picker (normal) or full overlay exit (quick-launch).
        if (keysDown & KEY_X) {
            g_touch_keys = 0;
            // Restore normal clickable-item state for any UI that follows.
            ult::noClickableItems.store(false, std::memory_order_release);

            if (g_quick_launch) {
                // Quick-launch mode: trigger feedback and close immediately.
                // exitServices() will call gb_unload_rom() (saves state + SRAM,
                // frees ROM/audio) after the overlay visually disappears, so the
                // exit feels instant rather than freezing during the disk write.
                triggerExitFeedback();
                tsl::Overlay::get()->close();
            } else {
                // Normal mode: save + unload first, then swap to the ROM selector.
                // Fire feedback before the slow disk write so the haptic/sound
                // schedules on the same frame (fires on the next frame either way,
                // but this keeps the intent clear).
                gb_unload_rom();  // saves state, writes SRAM, shuts down audio

                if (ult::useSoundEffects && !ult::limitedMemory)
                    ult::Audio::initialize();

                triggerExitFeedback();
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

        // Pass physical held + fresh presses + virtual touch keys to the GB core
        gb_set_input(keysHeld | keysDown | g_touch_keys);

        // Trigger rumble on ANY new button press
        if (g_ingame_haptics &&
            (keysDown & (KEY_A | KEY_B | KEY_PLUS | KEY_MINUS |
                         KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT))) {
            triggerRumbleClick.store(true, std::memory_order_release);
        }

        return true;  // consume all input while in-game
    }
};

// Deferred normal ROM launch: store path and swap to the emulator Gui.
// Defined here (after GBEmulatorGui) because swapTo<GBEmulatorGui> requires
// the complete type.
static void launch_emulator(const char* romPath) {
    strncpy(g_pending_rom_path, romPath, sizeof(g_pending_rom_path) - 1);
    g_pending_rom_path[sizeof(g_pending_rom_path) - 1] = '\0';
    // Do NOT clear g_settings_scroll here — the user expects to land back on the
    // same Settings item after X-ing out of the emulator and returning to Settings.
    tsl::swapTo<GBEmulatorGui>();
}

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
        const std::string slotLabel = "Slot " + std::to_string(m_slot) + " " + ult::DIVIDER_SYMBOL + " " + m_display_name;
        list->addItem(new tsl::elm::CategoryHeader(slotLabel));

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
                char label[16];
                snprintf(label, sizeof(label), "Slot %d", slot);
                //triggerEnterFeedback();
                triggerRumbleClick.store(true, std::memory_order_release);
                tsl::swapTo<SaveStatesGui>(romPath, displayName, std::string(label));
                show_notify("State saved.");
            } else {
                triggerWallFeedback();
                show_notify("No state to save yet.");
            }
            return true;
        });
        list->addItem(saveItem);

        // ── Load ──────────────────────────────────────────────────────────────
        auto* loadItem = new tsl::elm::ListItem("Load");
        loadItem->setClickListener([romPath, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            // Check if slot file exists first
            char slotPath[640] = {};
            build_user_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)) {
                triggerWallFeedback();
                show_notify("Slot is empty.");
                return true;
            }
            // Copy slot → internal state
            if (!load_user_slot(romPath.c_str(), slot)) {
                triggerWallFeedback();
                show_notify("Load failed.");
                return true;
            }
            // Launch the game — gb_load_rom will find the internal state and resume
            audio_exit_if_enabled();
            // Mark as last-played so the ROM selector shows the inProgress
            // indicator and scrolls to this game when we return via X.
            save_last_rom(romPath.c_str());
            // Windowed mode: relaunch self with -windowed so the game runs in
            // the small draggable window, matching what the ROM selector does.
            if (launch_windowed(romPath.c_str())) {
                triggerEnterSound.store(true, std::memory_order_release);
                return true;
            }
            triggerEnterFeedback();
            launch_emulator(romPath.c_str());
            return true;
        });
        list->addItem(loadItem);

        // ── Delete ────────────────────────────────────────────────────────────
        auto* deleteItem = new tsl::elm::SilentListItem("Delete");
        deleteItem->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            // Check if the slot file exists first
            char slotPath[640] = {};
            build_user_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)) {
                triggerWallFeedback();
                show_notify("Slot is empty.");
                return true;
            }
            // Delete the state file and its timestamp
            delete_file(slotPath);
            char tsPath[640] = {};
            build_user_slot_ts_path(romPath.c_str(), slot, tsPath, sizeof(tsPath));
            delete_file(tsPath);
            
            // Return to SaveStatesGui, scrolling back to this slot
            char label[16];
            snprintf(label, sizeof(label), "Slot %d", slot);
            

            tsl::swapTo<SaveStatesGui>(romPath, displayName, std::string(label));
            triggerFeedbackImpl(triggerRumbleClick, triggerMoveSound);
            show_notify("Slot deleted.");
            return true;
        });
        list->addItem(deleteItem);

        auto* frame = new UltraGBOverlayFrame("", "");
        frame->setContent(list);
        return frame;
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            char label[16];
            snprintf(label, sizeof(label), "Slot %d", m_slot);
            tsl::swapTo<SaveStatesGui>(m_rom_path, m_display_name, std::string(label));
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

        const std::string slotLabel =
            "Slot " + std::to_string(m_slot) + " " + ult::DIVIDER_SYMBOL + " " + m_display_name;
        list->addItem(new tsl::elm::CategoryHeader(slotLabel));

        const std::string& romPath     = m_rom_path;
        const std::string& displayName = m_display_name;
        const int slot = m_slot;

        // ── Backup ────────────────────────────────────────────────────────────
        auto* backupItem = new tsl::elm::SilentListItem("Backup");
        backupItem->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            if (backup_save_data_slot(romPath.c_str(), slot)) {
                char label[16];
                snprintf(label, sizeof(label), "Slot %d", slot);
                triggerEnterFeedback();
                show_notify("Save data backed up.");
                tsl::swapTo<SaveDataGui>(romPath, displayName, std::string(label));
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
            char slotPath[640] = {};
            build_save_backup_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)) {
                triggerWallFeedback();
                show_notify("Slot is empty.");
                return true;
            }
            if (!restore_save_data_slot(romPath.c_str(), slot)) {
                triggerWallFeedback();
                show_notify("Restore failed.");
                return true;
            }
            
            char label[16];
            snprintf(label, sizeof(label), "Slot %d", slot);
            tsl::swapTo<SaveDataGui>(romPath, displayName, std::string(label));
            triggerEnterFeedback();
            show_notify("Save data restored.");
            return true;
        });
        list->addItem(restoreItem);

        // ── Delete ────────────────────────────────────────────────────────────
        auto* deleteItem = new tsl::elm::SilentListItem("Delete");
        deleteItem->setClickListener([romPath, displayName, slot](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            char slotPath[640] = {};
            build_save_backup_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)){
                show_notify("Slot is empty.");
                triggerWallFeedback();
                return true;
            }
            delete_file(slotPath);
            char tsPath[640] = {};
            build_save_backup_slot_ts_path(romPath.c_str(), slot, tsPath, sizeof(tsPath));
            delete_file(tsPath);
            
            char label[16];
            snprintf(label, sizeof(label), "Slot %d", slot);
            triggerFeedbackImpl(triggerRumbleClick, triggerMoveSound);
            
            tsl::swapTo<SaveDataGui>(romPath, displayName, std::string(label));
            triggerFeedbackImpl(triggerRumbleClick, triggerMoveSound);
            show_notify("Slot deleted.");
            return true;
        });
        list->addItem(deleteItem);

        auto* frame = new UltraGBOverlayFrame("", "");
        frame->setContent(list);
        return frame;
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            char label[16];
            snprintf(label, sizeof(label), "Slot %d", m_slot);
            tsl::swapTo<SaveDataGui>(m_rom_path, m_display_name, std::string(label));
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

        list->addItem(new tsl::elm::CategoryHeader(
            "Save Data " + ult::DIVIDER_SYMBOL + " " + m_display_name));

        const std::string romPath     = m_rom_path;
        const std::string displayName = m_display_name;

        for (int i = 0; i < 10; ++i) {
            char ts[48] = {};
            read_save_backup_timestamp(romPath.c_str(), i, ts, sizeof(ts));

            char label[16];
            snprintf(label, sizeof(label), "Slot %d", i);

            auto* item = new tsl::elm::MiniListItem(label, ts);
            const int slot = i;
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

        auto* frame = new UltraGBOverlayFrame("", "");
        frame->setContent(list);
        return frame;
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            tsl::swapTo<GameConfigGui>(m_rom_path, m_display_name, std::string("Save Data"));
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

            char label[16];
            snprintf(label, sizeof(label), "Slot %d", i);

            auto* item = new tsl::elm::MiniListItem(label, ts);
            const int slot = i;
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

        auto* frame = new UltraGBOverlayFrame("", "");
        frame->setContent(list);
        return frame;
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            triggerExitFeedback();
            tsl::swapTo<GameConfigGui>(m_rom_path, m_display_name, std::string("Save States"));
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
        auto* resetItem = new tsl::elm::ListItem("Reset");
        resetItem->setClickListener([romPath](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            
            // Delete the internal quick-resume state so gb_load_rom cold-boots
            char internalPath[256] = {};
            build_rom_data_path(romPath.c_str(), internalPath, sizeof(internalPath),
                                INTERNAL_STATE_DIR, ".state");
            delete_file(internalPath);
            // Launch the game — no state file means cold boot
            audio_exit_if_enabled();
            // Mark as last-played so the ROM selector shows the inProgress
            // indicator and scrolls to this game when we return via X.
            save_last_rom(romPath.c_str());
            
            // Windowed mode: relaunch self with -windowed so the game runs in
            // the small draggable window, matching what the ROM selector does.
            if (launch_windowed(romPath.c_str())) {
                triggerEnterSound.store(true, std::memory_order_release);
                return true;
            }
            triggerEnterFeedback();
            launch_emulator(romPath.c_str());
            return true;
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
        size_t ghostingRomSz = 0;
        {
            FILE* fsz = fopen(romPath.c_str(), "rb");
            if (fsz) { fseek(fsz, 0, SEEK_END); ghostingRomSz = static_cast<size_t>(ftell(fsz)); fclose(fsz); }
        }
        const bool ghostingLocked =
            ult::limitedMemory ||
            (!ult::expandedMemory && ghostingRomSz >= kROM_2MB) ||
            (ult::expandedMemory && !ult::furtherExpandedMemory && ghostingRomSz >= kROM_4MB);
        const char* ghostingMsg =
            (ghostingRomSz >= kROM_4MB) ? "Requires at least 10MB." :
            (ghostingRomSz >= kROM_2MB) ? "Requires at least 8MB."  :
                                           "Requires at least 6MB.";

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

        auto* frame = new UltraGBOverlayFrame("", "");
        frame->setContent(list);
        return frame;
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
    VolumeTrackBar*    m_vol_slider      = nullptr;
    u8                 m_vol             = 100;
    u8                 m_vol_backup      = 100;
    std::string        m_rom_scroll;       // ROM name to scroll to when returning to RomSelectorGui
    std::string        m_settings_scroll; // label of currently focused item; saved to g_settings_scroll on page-flip

    // Focusable item pointers — used in update() to track which item has focus.
    tsl::elm::Element* m_scale_item      = nullptr;
    tsl::elm::Element* m_win_mode_item   = nullptr;
    tsl::elm::Element* m_win_scale_item  = nullptr;
    tsl::elm::Element* m_screen_mode_item = nullptr;
    tsl::elm::Element* m_haptics_item    = nullptr;
    tsl::elm::Element* m_quick_combo_item = nullptr;

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
        ult::setIniFileValue(kConfigFile, "config", "volume",
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

    // Poll focus each frame to remember the last focused item's label.
    // This is saved to g_settings_scroll on page-flip so the next visit
    // to Settings restores the cursor to wherever the user left it.
    virtual void update() override {
        if      (m_vol_slider     && m_vol_slider->hasFocus())          m_settings_scroll = "Game Boy";
        else if (m_win_mode_item  && m_win_mode_item->hasFocus())       m_settings_scroll = "Windowed Mode";
        else if (m_screen_mode_item && m_screen_mode_item->hasFocus())  m_settings_scroll = "Screen Mode";
        else if (m_win_scale_item && m_win_scale_item->hasFocus())      m_settings_scroll = "Windowed Scale";
        else if (m_scale_item     && m_scale_item->hasFocus())          m_settings_scroll = "Overlay Scale";
        else if (m_quick_combo_item && m_quick_combo_item->hasFocus())  m_settings_scroll = "Quick Combo";
        else if (m_haptics_item   && m_haptics_item->hasFocus())        m_settings_scroll = "In-Game Haptics";
    }

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();

        // ── Volume ────────────────────────────────────────────────────────────
        list->addItem(new tsl::elm::CategoryHeader(
            "Volume " + ult::DIVIDER_SYMBOL + " \uE0E3 Toggle Mute"));

        m_vol        = gb_audio_get_volume();
        m_vol_backup = g_vol_backup;  // load persisted backup; never 0

        auto* vol_slider = new VolumeTrackBar(
            "\uE13C", false, false, true, "Game Boy", "%", false);
        vol_slider->setProgress(m_vol);
        vol_slider->setValueChangedListener([this](u8 value) {
            m_vol = value;
            gb_audio_set_volume(value);
            char vbuf[4];
            ult::setIniFileValue(kConfigFile, "config", "volume",
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
        m_win_mode_item = win_item;

        // ── Screen Mode + Windowed Scale ──────────────────────────────────────
        // These two items share make_label so Screen Mode can immediately update
        // the Windowed Scale display when the user toggles 720p ↔ 1080p.
        // The strategy: define make_label once, create both items, then wire the
        // Screen Mode click listener last so scale_item can be captured by value.
        //
        // Screen Mode: declares whether the Switch is outputting 720p or 1080p.
        //   720p  — VI is downscaled ×2/3 before reaching the display, which
        //           exactly cancels the 1.5× FB→VI mapping.  All four scale
        //           factors (1×/2×/3×/4×) are pixel-perfect.
        //   1080p — VI maps 1:1 to physical pixels, so the 1.5× FB→VI mapping
        //           stays.  Only even scales (2× → ×3, 4× → ×6 display pixels
        //           per GB source pixel) produce integers; odd scales (1×/3×)
        //           land on half-pixels and blur.  Switching here clamps any
        //           saved odd scale to the nearest even value immediately.
        //
        // Windowed Scale: the FB multiplier applied at the next windowed launch.
        //   720p  : cycles 1× → 2× → 3× → 4× → 1×  (3× cap on 4 MB heap)
        //   1080p : cycles 2× ↔ 4× only              (fixed at 2× on 4 MB heap)
        {
            const int maxScale = ult::limitedMemory ? 3 : 4;

            // Returns the scale value that should be displayed/used right now,
            // honouring g_win_hd and the memory cap.
            // In 1080p mode: stored scale >= 3 → display 4×; < 3 → display 2×.
            // This means a user on 1x or 2x before switching to 1080p sees 2×,
            // and a user on 3x or 4x sees 4×, without touching the stored value
            // until they explicitly press A on Windowed Scale.
            auto effective_scale = [maxScale]() -> int {
                if (g_win_hd)
                    return (g_win_scale >= 3 && !ult::limitedMemory) ? 4 : 2;
                return std::min(g_win_scale, maxScale);
            };

            // Label string for the current effective scale.
            // 1080p mode uses the same label format as 720p — no "(HD)" suffix.
            auto make_label = [effective_scale]() -> std::string {
                const int s = effective_scale();
                static const char* lbs[] = {
                    "1x", "2x", "3x", "4x"
                };
                return lbs[s - 1];
            };

            // Label string for the current Screen Mode tri-state.
            auto make_hd_label = []() -> std::string {
                if (g_win_hd_mode == 2) return "1080p";
                if (g_win_hd_mode == 1) return "720p";
                return "Auto";  // 0 = Auto
            };

            // Create Windowed Scale item and wire its listener immediately.
            auto* scale_item = new tsl::elm::ListItem("Windowed Scale", make_label());
            scale_item->setClickListener([scale_item, maxScale,
                                          effective_scale, make_label](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                if (g_win_hd) {
                    // Cycle: displaying 2× (stored < 3) → store 4; displaying 4× → store 2.
                    // On limited memory 4× is unavailable so the A press is a no-op.
                    if (!ult::limitedMemory) {
                        g_win_scale = (g_win_scale < 3) ? 4 : 2;
                        save_win_scale();
                    }
                } else {
                    // Standard 720p cycle: 1 → 2 → 3 → 4 → 1
                    // (caps at 3 on limited memory).
                    const int cur = std::min(g_win_scale, maxScale);
                    g_win_scale   = (cur % maxScale) + 1;
                    save_win_scale();
                }
                scale_item->setValue(make_label());
                return true;
            });
            list->addItem(scale_item);
            m_win_scale_item = scale_item;


            // Create Screen Mode item first — listener wired below after scale_item exists.
            auto* hd_item = new tsl::elm::ListItem("Screen Mode", make_hd_label());
            list->addItem(hd_item);
            m_screen_mode_item = hd_item;

            // Wire Screen Mode listener now that scale_item is valid.
            // Captures scale_item by value so it can call setValue immediately,
            // keeping the Windowed Scale label in sync without a page flip.
            // Cycle: Auto (0) -> 720p (1) -> 1080p (2) -> Auto (0) -> ...
            hd_item->setClickListener([hd_item, scale_item,
                                       make_hd_label, make_label](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                g_win_hd_mode = (g_win_hd_mode + 1) % 3;
                resolve_win_hd();
                hd_item->setValue(make_hd_label());
                save_win_hd();
                // Refresh Windowed Scale label immediately to reflect the new mode.
                // g_win_scale is intentionally left untouched here - effective_scale()
                // maps stored scale to the nearest pixel-perfect value (stored < 3 -> 2x,
                // stored >= 3 -> 4x) without overwriting the saved value.  The user's
                // original scale is preserved so switching back to 720p restores it
                // exactly.  g_win_scale is only written when the user explicitly presses
                // A on Windowed Scale.
                scale_item->setValue(make_label());
                if (g_win_hd_mode == 0)
                    show_notify("Auto: detects docked/handheld at launch");
                else if (g_win_hd_mode == 2)
                    show_notify("1080p: 2x / 4x are pixel-perfect");
                else
                    show_notify("720p: all scales pixel-perfect");
                return true;
            });
        }

        // ── Overlay Scale ─────────────────────────────────────────────────────
        // Cycles 2.5× (default, fills overlay width) ↔ 2× (integer pixel-perfect)
        // on each A press.  Also togglable in-game by tapping the screen or RS.
        // Uses set_vp_scale() to invalidate the swizzle/coord LUTs together with
        // the flag — direct assignment to g_vp_2x alone would leave stale LUTs.
        {
            const char* scale_labels[] = { "2.5x", "2x" };
            auto* scale_item = new tsl::elm::ListItem("Overlay Scale",
                                                       scale_labels[g_vp_2x ? 1 : 0]);
            scale_item->setClickListener([scale_item](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                set_vp_scale(!g_vp_2x);
                save_pixel_perfect();
                const char* labels[] = { "2.5x", "2x" };
                scale_item->setValue(labels[g_vp_2x ? 1 : 0]);
                return true;
            });
            list->addItem(scale_item);
            m_scale_item = scale_item;
        }


        // ── Input ────────────────────────────────────
        list->addItem(new tsl::elm::CategoryHeader("Input"));

        // ── Quick Combo ───────────────────────────────────────────────────────
        // A combo assigned here lets the user open the last played ROM directly
        // (bypassing the ROM selector) from anywhere on the Switch.  Selecting
        // a combo removes it from any other overlay/package that currently holds
        // it — identical deconfliction to Ultrahand's combo picker.
        {
            // Display the current combo converted to its unicode button symbols,
            // or the OPTION_SYMBOL placeholder when none is assigned.
            std::string comboDisplay = g_quick_combo[0]
                ? std::string(g_quick_combo)
                : std::string(ult::OPTION_SYMBOL);
            if (g_quick_combo[0])
                ult::convertComboToUnicode(comboDisplay);

            const std::string capturedDisplay = comboDisplay;
            auto* qc_item = new tsl::elm::ListItem("Quick Combo", comboDisplay);
            qc_item->setClickListener([this](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                // Persist the current scroll target before leaving so SettingsGui
                // can restore it when the user presses B inside QuickComboSelectorGui.
                strncpy(g_settings_scroll, "Quick Combo",
                        sizeof(g_settings_scroll) - 1);
                g_settings_scroll[sizeof(g_settings_scroll) - 1] = '\0';
                tsl::swapTo<QuickComboSelectorGui>(m_rom_scroll, m_settings_scroll);
                return true;
            });
            list->addItem(qc_item);
            m_quick_combo_item = qc_item;
        }

        auto* haptics_item = new tsl::elm::ToggleListItem(
            "In-Game Haptics", g_ingame_haptics, ult::ON, ult::OFF);
        haptics_item->setStateChangedListener([](bool state) {
            g_ingame_haptics = state;
            ult::setIniFileValue(kConfigFile, "config", "ingame_haptics",
                                 state ? "1" : "0", "");
        });
        list->addItem(haptics_item);
        m_haptics_item = haptics_item;

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
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;

        // simulatedNextPage fires when the user taps the footer page button.
        // On the Settings page the footer shows left-arrow "Games", so treat
        // simulatedNext as "go left" — same as KEY_LEFT.
        const bool simulatedNext = ult::simulatedNextPage.exchange(
            false, std::memory_order_acq_rel);
        // Only block page navigation if the volume slider itself is focused AND unlocked.
        // If focus has moved to another item (e.g. Overlay Scale), allowSlide being true
        // is irrelevant and should not prevent page changes.
        const bool sliderActive = m_vol_slider && m_vol_slider->hasFocus()
                                  && ult::allowSlide.load(std::memory_order_acquire);
        const bool wantLeft = !sliderActive && (simulatedNext || (keysDown & KEY_LEFT));

        if (wantLeft) {
            triggerNavigationFeedback();
            // Reset slider lock state so it comes up locked on the next page visit.
            ult::allowSlide.store(false, std::memory_order_release);
            // Persist the focused item so the next visit to Settings lands here.
            strncpy(g_settings_scroll, m_settings_scroll.c_str(), sizeof(g_settings_scroll) - 1);
            g_settings_scroll[sizeof(g_settings_scroll) - 1] = '\0';
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
                triggerNavigationFeedback();
                return true;
            });
            list->addItem(item);
        }

        // ── Predefined combos ─────────────────────────────────────────────────
        std::string jumpTarget = currentCombo.empty() ? std::string(ult::OPTION_SYMBOL) : "";

        for (const auto& combo : g_defaultCombos) {
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
            item->setClickListener([this, item, combo, display](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                // 1. Deconflict: remove this combo from every other overlay/package.
                remove_quick_combo_from_others(combo);
                // 2. Persist and reload.
                save_combo(combo);
                if (m_lastSelected && m_lastSelected != item)
                    m_lastSelected->setValue("");
                item->setValue(ult::CHECKMARK_SYMBOL);
                m_lastSelected = item;
                triggerNavigationFeedback();
                return true;
            });
            list->addItem(item);
        }

        // Scroll to the currently selected item when the page opens.
        if (!jumpTarget.empty())
            list->jumpToItem(jumpTarget, "", true);

        auto* frame = new UltraGBOverlayFrame("", "");
        frame->setContent(list);
        return frame;
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
        // If GBEmulatorGui flagged that L/R/ZL/ZR may still be physically held,
        // arm the release-gate so handleInput swallows those keys until the
        // controller is clean.
        if (g_waitForInputRelease) {
            g_waitForInputRelease = false;
            m_waitForRelease = true;
        }
    }

    // No destructor needed: Tesla deletes m_topElement (frame) →
    // ~UltraGBOverlayFrame deletes m_contentElement (list) →
    // ~List deletes all items.
    //
    // Note on glyph cache: GBEmulatorGui::createUI() calls
    // FontManager::clearCache() before the ROM malloc, so no flush is
    // needed here — the cache repopulates automatically when the user
    // returns to this screen.

    virtual tsl::elm::Element* createUI() override {
        g_emu_active = false;

        //if (ult::useSoundEffects && !ult::limitedMemory)
        //    ult::Audio::initialize();

        // ── ROM list ─────────────────────────────────────────────────────
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader(
            "Games " + ult::DIVIDER_SYMBOL + " \uE0E3 Configure"));

        // ── ROM list via scandir — no std::vector, no full-path string per entry ──
        // scandir returns a sorted dirent**; each entry is freed immediately after use.
        // The lambda captures only the 255-byte basename inline (char[256] in the
        // closure) — the full path is snprintf'd onto the stack only at click time.
        struct dirent** romEntries = nullptr;
        const int nRoms = scandir(g_rom_dir, &romEntries, gb_rom_filter, alphasort);

        if (nRoms <= 0) {
            static char msg[640];
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
            std::string jumpLabel = m_jump_to;
            std::string inProgressLabel;  // basename of last-played ROM (for auto-scroll on first open)
            char fullPath[512];

            for (int ri = 0; ri < nRoms; ri++) {
                const char* name = romEntries[ri]->d_name;
                snprintf(fullPath, sizeof(fullPath), "%s%s", g_rom_dir, name);

                // g_gb.rom is null here — gb_unload_rom() was called before any
                // swapTo<RomSelectorGui>, so there is no live ROM to fast-resume.
                const bool isLast = g_last_rom_path[0] &&
                    strcmp(name, g_last_rom_path) == 0;
                // inProgress symbol = only the genuinely last-played ROM.
                // Only show if playable on this tier.
                const bool inProgress = isLast && rom_is_playable(fullPath);

                if (inProgress && inProgressLabel.empty())
                    inProgressLabel = name;

                auto* item = new tsl::elm::MiniListItem(
                    name, inProgress ? ult::INPROGRESS_SYMBOL : "");
                if (!rom_is_playable(fullPath))
                    item->setTextColor(tsl::warningTextColor);

                // Capture only the basename (≤255 bytes) inline in the closure.
                // Full path is reconstructed onto the stack at click time only —
                // zero extra heap per ROM during list construction.
                char romName[256];
                strncpy(romName, name, sizeof(romName) - 1);
                romName[sizeof(romName) - 1] = '\0';

                item->setClickListener([=](u64 keys) -> bool {
                    char p[512];
                    snprintf(p, sizeof(p), "%s%s", g_rom_dir, romName);
                    // Track the last item navigated to so page-flip can restore scroll.
                    strncpy(g_rom_selector_scroll, romName, sizeof(g_rom_selector_scroll) - 1);
                    g_rom_selector_scroll[sizeof(g_rom_selector_scroll) - 1] = '\0';
                    // Y → open per-game configuration screen
                    // Do NOT call save_last_rom here — that would move the inProgress
                    // indicator to whatever game you configured rather than what you played.
                    if (keys & KEY_Y) {
                        triggerRumbleClick.store(true, std::memory_order_release);
                        triggerSettingsSound.store(true, std::memory_order_release);
                        tsl::swapTo<GameConfigGui>(std::string(p), std::string(romName));
                        return true;
                    }
                    if (!(keys & KEY_A)) return false;
                    if (!rom_is_playable(p)) {
                        gb_load_rom(p);  // shows "too large" notification
                        return false;
                    }
                    save_last_rom(p);
                    audio_exit_if_enabled();
                    if (launch_windowed(p)) return true;
                    launch_emulator(p);
                    return true;
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
        const bool wantRight = !sliderActive && (simulatedNext || (keysDown & KEY_RIGHT));

        if (wantRight) {
            triggerNavigationFeedback();
            ult::allowSlide.store(false, std::memory_order_release);
            tsl::swapTo<SettingsGui>(std::string(g_rom_selector_scroll),
                                     std::string(g_settings_scroll));
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
// Windowed mode — GBWindowedElement / GBWindowedGui / WindowedOverlay.
// Included here so it has access to all globals, helpers, and gb_load_rom/
// gb_unload_rom defined above.  Must come before class Overlay below.
// =============================================================================
#include "gb_windowed.hpp"

// =============================================================================
// Overlay
// =============================================================================
class Overlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        ult::COPY_BUFFER_SIZE = 1024; // minimize copy buffer
        tsl::overrideBackButton = true;
        ult::createDirectory(CONFIG_DIR);
        ult::createDirectory(SAVE_DIR);
        ult::createDirectory(INTERNAL_SAVE_DIR);
        ult::createDirectory(STATE_BASE_DIR);
        ult::createDirectory(INTERNAL_STATE_DIR);
        ult::createDirectory(CONFIGURE_DIR);
        load_config();
        write_default_config_if_missing();
        ult::createDirectory(g_rom_dir);

        // Register "-quicklaunch" mode in overlays.ini so Ultrahand can
        // display and assign a combo to it.  Also refreshes g_quick_combo.
        register_quick_launch_mode();

        // Restore the settings-page scroll position when returning from windowed
        // mode.  g_returning_from_windowed is set by main() when -returning is
        // present in argv; the transient "settings_scroll" ini key was written by
        // launch_windowed() immediately before the windowed setNextOverlay call.
        //
        // The key is always erased here regardless of the return-path flag so
        // that an abnormal windowed exit (process killed by the OS) cannot leave
        // a stale value that sneaks in on the next normal cold launch.
        {
            const std::string ss = ult::parseValueFromIniSection(
                kConfigFile, "config", "settings_scroll");
            if (g_returning_from_windowed && !ss.empty()
                    && ss.size() < sizeof(g_settings_scroll) - 1) {
                strncpy(g_settings_scroll, ss.c_str(), sizeof(g_settings_scroll) - 1);
                g_settings_scroll[sizeof(g_settings_scroll) - 1] = '\0';
            }
            g_returning_from_windowed = false;  // consumed; clear for safety
            if (!ss.empty())
                ult::setIniFileValue(kConfigFile, "config", "settings_scroll", "", "");
        }

        // Migrate legacy .sav files from saves/ root → saves/internal/
        // Runs once after the first update; files that already exist in the
        // destination are skipped automatically by moveFilesOrDirectoriesByPattern.
        //ult::moveFilesOrDirectoriesByPattern(std::string(SAVE_DIR) + "*.sav",
        //                                     INTERNAL_SAVE_DIR);
    }

    virtual void exitServices() override {
        gb_audio_pause();
        gb_unload_rom();   // save state, write SRAM, shut down audio, free ROM buffer
        gb_audio_free_dma();  // release DMA buffers held across sessions (see gb_audio.h)
        free_lcd_ghosting();  // release ghosting heap held across sessions
    }

    // Pause when overlay is hidden with the system combo.
    // gb_audio_pause() signals the audio thread to submit silence and clear
    // its GBAPU state — prevents the last sound from looping while hidden.
    virtual void onHide() override {
        g_gb.running = false;
        g_emu_active = false;
        gb_audio_pause();
    }

    // Resume when overlay is shown again.
    // gb_audio_resume() re-enables sample generation.  The GBAPU repopulates
    // from gb_run_frame() register events within one frame (~16.7 ms).
    virtual void onShow() override {
        if (g_gb.rom) {
            g_gb_frame_next_ns = 0;
            g_gb.running = true;
            g_emu_active = true;
            gb_audio_resume();
        }
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override {
        if (g_quick_launch && g_last_rom_path[0]) {
            // Build full path from dir + basename persisted by save_last_rom().
            const std::string romPath = std::string(g_rom_dir) + g_last_rom_path;

            if (g_windowed_mode) {
                // ── Windowed quick-launch fallback ────────────────────────────
                // The fast path in main() normally launches WindowedOverlay
                // directly.  This branch fires only if that path was skipped
                // (e.g. g_self_path not yet valid, edge-case startup order).
                // Write config keys and setNextOverlay; placeholder Gui is never
                // rendered because close() fires immediately after.
                if (g_self_path[0]) {
                    ult::setIniFileValue(kConfigFile, "config", "windowed_rom", romPath, "");
                    ult::setIniFileValue(kConfigFile, "config", "win_quick_exit", "1", "");
                    tsl::setNextOverlay(std::string(g_self_path), "-windowed");
                    tsl::Overlay::get()->close();
                }
                return initially<RomSelectorGui>();
            }

            // ── Normal quick-launch ───────────────────────────────────────────
            // Skip the ROM selector and boot directly into the emulator.
            // UltraGBOverlayFrame normally calls loadWallpaperFileWhenSafe() in
            // its constructor; since we bypass that frame, trigger it here so the
            // wallpaper is available when GBScreenElement draws its first frame.
            ult::loadWallpaperFileWhenSafe();

            strncpy(g_pending_rom_path, romPath.c_str(), sizeof(g_pending_rom_path) - 1);
            g_pending_rom_path[sizeof(g_pending_rom_path) - 1] = '\0';
            return initially<GBEmulatorGui>();
        }

        return initially<RomSelectorGui>();
    }
};

int main(int argc, char* argv[]) {
    // Build the full path to this overlay so setNextOverlay can relaunch us.
    // argv[0] on NX is just the bare filename (e.g. "gbemu.ovl"), NOT a full
    // path.  All overlays live in sdmc:/switch/.overlays/ — prepend that prefix
    // exactly as Status Monitor does with (folderpath + filename).
    if (argc > 0) {
        const std::string full_path = std::string("sdmc:/switch/.overlays/") + argv[0];
        strncpy(g_self_path, full_path.c_str(), sizeof(g_self_path) - 1);
        g_self_path[sizeof(g_self_path) - 1] = '\0';
    }

    // ── Windowed launch ───────────────────────────────────────────────────────
    // The ROM path is in config.ini under "windowed_rom" (written by the ROM
    // click listener before calling setNextOverlay).  We only detect the flag
    // here and set the framebuffer size; the path is read in initServices().
    //
    // -returning is passed by GBWindowedGui when the user exits windowed mode
    // via the launch combo (setNextOverlay g_self_path "-returning").  It is NOT
    // a windowed launch — tsl::loop<Overlay> runs normally — but Overlay::
    // initServices() needs to know it should restore the persisted settings
    // scroll position from config.ini rather than starting fresh.
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-returning") == 0)
            g_returning_from_windowed = true;

        if (strcmp(argv[i], "-quicklaunch") == 0) {
            g_quick_launch = true;

            // ── Windowed quick-launch fast path ───────────────────────────────
            // When windowed mode is configured and a last-played ROM is saved,
            // launch WindowedOverlay directly instead of going through the
            // Overlay → setNextOverlay → close → WindowedOverlay cycle.
            // That cycle flashes the RomSelector placeholder and adds a full
            // process-restart delay before the game appears.
            {
                const std::string wval    = ult::parseValueFromIniSection(kConfigFile, "config", "windowed");
                const std::string lastRom = ult::parseValueFromIniSection(kConfigFile, "config", "last_rom");
                if (wval == "1" && !lastRom.empty()) {
                    // Build full ROM path: dir (with trailing slash) + basename.
                    std::string romDir = ult::parseValueFromIniSection(kConfigFile, "config", "rom_dir");
                    if (romDir.empty()) romDir = "sdmc:/roms/gb/";
                    if (romDir.back() != '/') romDir += '/';
                    const std::string fullPath = romDir + lastRom;

                    // Read win_scale with the same early-limited-memory clamp
                    // used in the -windowed block below.
                    {
                        const std::string sv = ult::parseValueFromIniSection(kConfigFile, "config", "win_scale");
                        if      (sv == "2") g_win_scale = 2;
                        else if (sv == "3") g_win_scale = 3;
                        else if (sv == "4") g_win_scale = 4;
                        else                g_win_scale = 1;
                        const bool earlyLimited =
                            (ult::getCurrentHeapSize() == ult::OverlayHeapSize::Size_4MB);
                        if (earlyLimited && g_win_scale == 4) g_win_scale = 3;

                        // Apply win_hd pixel-perfect constraint (same logic as
                        // the -windowed block; runtime clamp only, no config write).
                        {
                            const std::string hd = ult::parseValueFromIniSection(
                                kConfigFile, "config", "win_hd");
                            if      (hd == "1") g_win_hd_mode = 2;
                            else if (hd == "0") g_win_hd_mode = 1;
                            else                g_win_hd_mode = 0;  // "auto" or absent
                            resolve_win_hd();
                        }
                        if (g_win_hd && g_win_scale % 2 != 0)
                            g_win_scale = (g_win_scale == 1) ? 2 : 4;
                        if (earlyLimited && g_win_scale == 4)
                            g_win_scale = g_win_hd ? 2 : 3;
                    }

                    // Write to config so WindowedOverlay::initServices() picks
                    // them up (it always reads from config.ini, not from globals).
                    ult::setIniFileValue(kConfigFile, "config", "windowed_rom", fullPath, "");
                    ult::setIniFileValue(kConfigFile, "config", "win_quick_exit", "1", "");

                    ult::DefaultFramebufferWidth  = GB_W * g_win_scale;
                    ult::DefaultFramebufferHeight = GB_H * g_win_scale;
                    return tsl::loop<WindowedOverlay, tsl::impl::LaunchFlags::None>(argc, argv);
                }
            }
            // No windowed mode or no saved ROM — fall through to normal Overlay.
        }

        if (strcmp(argv[i], "-windowed") == 0) {
            // load_config() has not run yet at this point (it runs inside
            // WindowedOverlay::initServices(), which tsl::loop calls after
            // the framebuffer is already created).  We must read win_scale
            // directly so we can size the VI layer correctly before Tesla
            // initialises.  Absent / invalid key → safe default of 1×.
            {
                const std::string sv = ult::parseValueFromIniSection(
                    kConfigFile, "config", "win_scale");
                if      (sv == "2") g_win_scale = 2;
                else if (sv == "3") g_win_scale = 3;
                else if (sv == "4") g_win_scale = 4;
                else                g_win_scale = 1;

                // Mirror Tesla's heap check (tsl::loop does the same at startup).
                // If we are on a 4 MB heap and the saved scale is 4×, clamp it
                // to 3× now — before the VI layer is sized — but do NOT save,
                // so switching back to a normal-memory session restores 4×.
                const bool earlyLimitedMemory =
                    (ult::getCurrentHeapSize() == ult::OverlayHeapSize::Size_4MB);
                if (earlyLimitedMemory && g_win_scale == 4)
                    g_win_scale = 3;  // runtime clamp only — config keeps "4"

                // Read win_hd so we can enforce pixel-perfect constraints before
                // the VI layer is sized.  Clamping here is runtime-only; the saved
                // config value is never overwritten by a launch-time clamp.
                {
                    const std::string hd = ult::parseValueFromIniSection(
                        kConfigFile, "config", "win_hd");
                    if      (hd == "1") g_win_hd_mode = 2;
                    else if (hd == "0") g_win_hd_mode = 1;
                    else                g_win_hd_mode = 0;  // "auto" or absent
                    resolve_win_hd();
                }
                // In 1080p mode only even scales are pixel-perfect (the 1.5× FB→VI
                // mapping combined with the 1:1 VI→display mapping at 1080p means
                // odd scales land on half-pixels).  Clamp without saving.
                if (g_win_hd && g_win_scale % 2 != 0)
                    g_win_scale = (g_win_scale == 1) ? 2 : 4;
                // Apply the memory cap again after the HD clamp (HD can push scale
                // from 3→4, which would violate the 4 MB limit).
                if (earlyLimitedMemory && g_win_scale == 4)
                    g_win_scale = g_win_hd ? 2 : 3;
            }
            ult::DefaultFramebufferWidth  = GB_W * g_win_scale;
            ult::DefaultFramebufferHeight = GB_H * g_win_scale;
            return tsl::loop<WindowedOverlay, tsl::impl::LaunchFlags::None>(argc, argv);
        }
    }

    // ── Normal launch ─────────────────────────────────────────────────────────
    return tsl::loop<Overlay, tsl::impl::LaunchFlags::None>(argc, argv);
}