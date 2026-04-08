/********************************************************************************
 * File: gb_globals.hpp
 * Description:
 *   All global variable and constant definitions for UltraGB.
 *
 *   Included via gb_utils.hpp, which is the first project-specific include in
 *   main.cpp.  Because this is a single-translation-unit build (main.cpp
 *   textually includes all .hpp files), every `static` variable defined here
 *   exists as a single copy in the binary — this header is processed once.
 *
 *   Include chain provided by this header:
 *     gb_audio.h   → GBAPU, gb_audio_* API
 *     gb_core.h    → GBState, PaletteMode, GB_W/H, FB_W/H, gb_load_rom decl
 *     gb_renderer.h → render helpers, ALL_KEYS_MASK, apply_lcd_ghosting, etc.
 *
 *  Licensed under GPLv2
 *  Copyright (c) 2026 ppkantorski
 ********************************************************************************/

#pragma once

#include <string>
#include <cstring>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <sys/stat.h> 

#include "gb_audio.h"
#include "gb_core.h"
#include "gb_renderer.h"


// =============================================================================
// Paths / config
// =============================================================================
static constexpr const char* CONFIG_DIR        = "sdmc:/config/ultragb/";
static constexpr const char* CONFIG_FILE       = "sdmc:/config/ultragb/config.ini";
static constexpr const char* SAVE_BASE_DIR     = "sdmc:/config/ultragb/saves/";
static constexpr const char* STATE_BASE_DIR    = "sdmc:/config/ultragb/states/";
static constexpr const char* STATE_DIR         = "sdmc:/config/ultragb/states/internal/";
static constexpr const char* CONFIGURE_DIR     = "sdmc:/config/ultragb/configure/";
static constexpr const char* OVL_THEMES_DIR     = "sdmc:/config/ultragb/ovl_themes/";
static constexpr const char* OVL_THEME_FILE     = "sdmc:/config/ultragb/ovl_theme.ini";
static constexpr const char* OVL_WALLPAPERS_DIR = "sdmc:/config/ultragb/ovl_wallpapers/";
static constexpr const char* OVL_WALLPAPER_FILE = "sdmc:/config/ultragb/ovl_wallpaper.rgba";

static constexpr size_t PATH_BUFFER_SIZE = 128;
static char g_rom_dir[PATH_BUFFER_SIZE]             = "sdmc:/roms/gb/";
static char g_save_dir[PATH_BUFFER_SIZE]            = "sdmc:/config/ultragb/saves/internal/";
static char g_last_rom_path[PATH_BUFFER_SIZE]       = {};   // basename of last-played ROM, persisted to config.ini
static char g_pending_rom_path[PATH_BUFFER_SIZE]    = {};   // path deferred from click listener → loaded in GBOverlayGui::createUI()
static char g_rom_selector_scroll[PATH_BUFFER_SIZE] = {};   // last ROM the user interacted with in the selector
static char g_settings_scroll[64]                   = {};   // last focused item label in SettingsGui

static constexpr const char* NOT_ENOUGH_MEMORY_WARNING = "Not enough memory.";
static constexpr const char* SLOT_IS_EMPTY_WARNING     = "Slot is empty.";
static constexpr const char* REQUIRES_AT_LEAST_6MB     = "Requires at least 6MB.";
static constexpr const char* REQUIRES_AT_LEAST_8MB     = "Requires at least 8MB.";
static constexpr const char* REQUIRES_AT_LEAST_10MB    = "Requires at least 10MB.";

// Set true by GBOverlayGui when it exits to RomSelectorGui via X.
// RomSelectorGui reads and clears this in its constructor, then blocks all
// navigation input until every physical key is released — preventing L/R/ZL/ZR
// presses made in-game from firing a list-jump the moment the ROM selector appears.
static bool g_waitForInputRelease = false;

// =============================================================================
// INI key constants
// Persistent std::string instances avoid a per-call heap alloc (CONFIG_FILE is
// 31 chars, above the ARM64 SSO threshold of 15; all ult:: INI helpers take
// const std::string&).
// =============================================================================
static const std::string kConfigFile   {CONFIG_FILE};
static const std::string kConfigSection{"config"};
static const std::string kKeyRomDir        {"rom_dir"};
static const std::string kKeySaveDir       {"save_dir"};
static const std::string kKeyLastRom       {"last_rom"};
static const std::string kKeyVolume        {"volume"};
static const std::string kKeyVolBackup     {"vol_backup"};
static const std::string kKeyGameVolume    {"game_volume"};
static const std::string kKeyGameVolBackup {"game_vol_backup"};
static const std::string kKeyLcdGrid       {"lcd_grid"};
static const std::string kKeyWindowed      {"windowed"};
static const std::string kKeyButtonHaptics    {"button_haptics"};
static const std::string kKeyTouchHaptics     {"touch_haptics"};
static const std::string kKeyOvlWallpaperName {"ovl_wallpaper"};
static const std::string kKeyWinPosX       {"win_pos_x"};
static const std::string kKeyWinPosY       {"win_pos_y"};
static const std::string kKeyWinScale      {"win_scale"};
static const std::string kKeyWinOutput     {"win_output"};   // "720" or "1080"
static const std::string kKeyWindowedRom   {"windowed_rom"};
static const std::string kKeyWinQuickExit  {"win_quick_exit"};
static const std::string kKeySettingsScroll{"settings_scroll"};
static const std::string kKeyPlayerRom     {"overlay_rom"};  // ROM path for -overlay relaunch
static const std::string kKeyOvlTheme     {"ovl_theme"};     // selected overlay theme filename stem
static const std::string kKeyOvlFreeMode  {"ovl_free_mode"};   // 0=fixed 1=free floating overlay
static const std::string kKeyOvlFreePosX  {"ovl_free_pos_x"};  // VI-space X of the free overlay layer
static const std::string kKeyOvlFreePosY  {"ovl_free_pos_y"};  // transparent rows at top of FB (0..OVL_FREE_TOP_TRIM)
static const std::string kKeyOvlOpaque    {"ovl_opaque"};       // 0=theme alpha (default), 1=force all alpha to 15

// =============================================================================
// ROM size thresholds
// Shared by gb_load_rom, rom_is_playable, rom_playability_message, and
// the -quicklaunch pre-loop size check in main().
// =============================================================================
static constexpr size_t kROM_2MB = 2u << 20;
static constexpr size_t kROM_4MB = 4u << 20;
static constexpr size_t kROM_6MB = 6u << 20;

// =============================================================================
// Stack-only integer-to-string for 0–100 volume values.
// std::to_string heap-allocates; this writes into a caller-supplied char[4].
// Returns a pointer to buf (always null-terminated).
// =============================================================================
static const char* vol_to_str(u8 v, char (&buf)[4]) {
    if (v == 100) { buf[0]='1'; buf[1]='0'; buf[2]='0'; buf[3]='\0'; }
    else if (v >= 10) { buf[0]='0'+v/10; buf[1]='0'+v%10; buf[2]='\0'; }
    else               { buf[0]='0'+v;                      buf[1]='\0'; }
    return buf;
}

// =============================================================================
// Runtime settings flags
// =============================================================================

// Unmute backup volume — the last positive volume before muting.
// Persisted to vol_backup in config.ini so it survives app exit.
// Always > 0; initialized to 50 as a safe default.
static u8 g_vol_backup = 50;

// Unmute backup for background-title (Active Title) volume.
// Persisted to game_vol_backup in config.ini so it survives app exit.
// Always > 0; initialized to 30 matching the game_volume default.
static u8 g_game_vol_backup = 30;

// ── Windowed mode ─────────────────────────────────────────────────────────────
// When true the ROM selector relaunches this overlay with -windowed <path>,
// rendering the Game Boy screen as a small draggable window with no UI chrome.
static bool g_windowed_mode     = false;
static bool g_button_haptics    = false;  // controller button presses; off by default
static bool g_touch_haptics     = false;  // screen touch (virtual d-pad/buttons, repositioning); off by default
static bool g_ovl_opaque        = false;  // when true, all overlay alpha channels are forced to 15 (fully opaque)
static bool g_overlay_wallpaper = false;  // derived: true when a wallpaper file is selected + file exists
static char g_ovl_wallpaper_name[64] = {};  // selected wallpaper filename stem (empty = none)

// Set true by main() when the overlay is relaunched with the -returning argument.
// Read and cleared in Overlay::initServices() to restore the settings scroll position.
static bool g_returning_from_windowed = false;

static bool g_directMode = false;
static bool g_comboReturn = false;

// Full path to this .ovl — captured from argv[0] so WindowedOverlay can
// pass it to setNextOverlay when the launch combo is pressed to return here.
static char g_self_path[PATH_BUFFER_SIZE] = {};

// ROM path received via the -windowed <path> argument.
// Set in main(), consumed by WindowedOverlay::loadInitialGui().
static char g_win_rom_path[PATH_BUFFER_SIZE] = {};

// Quick-launch combo string (raw, e.g. "L+R+DDOWN"). Empty = none configured.
// Populated from overlays.ini mode_combos[0] in register_quick_launch_mode().
static char g_quick_combo[64] = {};

// Set true by main() when launched with the -quicklaunch mode argument.
static bool g_quick_launch = false;

// Set true by main() when launched with the -overlay argument.
static bool g_overlay_mode = false;

// ROM path received via the -overlay argument.
static char g_overlay_rom_path[PATH_BUFFER_SIZE] = {};

// ── Free overlay mode ──────────────────────────────────────────────────────────
// When true the overlay is relaunched with -freeoverlay <path>: the layer is
// full-height (720 rows) so the VI compositor's bounding-box is stable at all
// positions and screenshots work correctly.  Vertical repositioning is achieved
// by shifting content within the framebuffer (g_ovl_free_pos_y transparent rows
// at the top) rather than moving the VI layer, which stays at y=0 always.
// X repositioning still uses viSetLayerPosition normally.
//
// OVL_FREE_TOP_TRIM = VP_Y - VP_X: the maximum vertical shift is 84 rows, so
// the gap above the GB border equals the side gap (VP_X-1 = 23 px) at the
// extremes, giving equal wallpaper padding on all three exposed edges.
//   GB border top in FB = VP_Y + g_render_y_offset
//                       = VP_Y + (g_ovl_free_pos_y - OVL_FREE_TOP_TRIM)
//   When pos_y=OVL_FREE_TOP_TRIM(84): render_y_offset=0  → border at VP_Y=108 (same as fixed overlay)
//   When pos_y=0:                     render_y_offset=-84 → border at 24 = VP_X-1 ✓
//
// Layer VI size at 720p (1.5× scaling):  448*1.5 × 720*1.5 = 672 × 1080
//   VI layer is full-screen height → VI max Y = 0 (layer never moves vertically).
//   VI max X = 1920 - 672 = 1248  (X repositioning still via viSetLayerPosition).
static constexpr int OVL_FREE_TOP_TRIM  = VP_Y - VP_X;              // 84 rows
static constexpr int OVL_FREE_CONTENT_H = FB_H - OVL_FREE_TOP_TRIM; // 636 — content window height (unchanged)
static constexpr int OVL_FREE_FB_H      = FB_H;                      // 720 — full-height framebuffer; VI layer fills the full screen height so the compositor's bounding-box is position-invariant and screenshots work correctly at any position

static bool g_overlay_free_mode = false;  // true when launched with -freeoverlay
static int  g_ovl_free_pos_x    = 0;               // VI-space X; 0 = left edge
static int  g_ovl_free_pos_y    = OVL_FREE_TOP_TRIM; // transparent rows at top of FB (0..OVL_FREE_TOP_TRIM); OVL_FREE_TOP_TRIM=84 → content flush with bottom → same visual position as normal overlay

// Set true when a windowed quick-launch is triggered from loadInitialGui().
static bool g_win_quick_exit = false;

// Set true by main() when a -quicklaunch windowed ROM was too large for the
// pre-tsl::loop() heap-size check.  Overlay::loadInitialGui() clears it and
// shows an error notify (or attempts recovery) before opening the ROM selector.
static bool g_quicklaunch_windowed_toobig = false;

// Window position in VI-space (1920×1080).
// WIN_VI_W = 1920*160/1280 = 240, WIN_VI_H = 1080*144/720 = 216.
// Default: centred on the display.
static int g_win_pos_x = (1920 - 240) / 2;   // 840  (1× default centre)
static int g_win_pos_y = (1080 - 216) / 2;   // 432  (1× default centre)

// Windowed display scale: 1–6 (integer pixel scale factor).
// g_win_scale serves two roles — see main.cpp save/load comments for full detail.
static int  g_win_scale        = 1;
static bool g_win_scale_locked = false;

// True when running on a 4 MB heap at scale 3 in windowed mode.
// In this mode the framebuffer is exactly game-sized (no anchor padding),
// the layer is positioned directly at g_win_pos_y, and screenshots are
// disabled.  Set once in main() before setup_windowed_framebuffer().
static bool g_win_limited_fb = false;

// Windowed output resolution mode.
// false = 720p-scaled (default)   true = 1080p pixel-perfect.
static bool g_win_1080 = false;

// =============================================================================
// Tiny data-coupled helpers
// These operate purely on the variables above; no ult:: I/O calls.
// =============================================================================

// Parse "1"–"6" win_scale string → int; anything else → 1.
static int parse_win_scale_str(const std::string& sv) {
    if (sv.size() == 1 && sv[0] >= '2' && sv[0] <= '6') return sv[0] - '0';
    return 1;
}

// Persist a label string into g_settings_scroll (max 63 chars + NUL).
static void set_settings_scroll(const char* label) {
    strncpy(g_settings_scroll, label, sizeof(g_settings_scroll) - 1);
    g_settings_scroll[sizeof(g_settings_scroll) - 1] = '\0';
}

// Digit-safe unsigned integer parser (exceptions disabled on NX).
// Returns true and writes out on success; false if s is empty or non-digit.
// [[gnu::noinline]] — called ~13+ times across load_config and main();
// LTO would otherwise inline all 13 copies (each ~20 instructions).
[[gnu::noinline]]
static bool parse_uint(const std::string& s, int& out) {
    if (s.empty()) return false;
    int v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    out = v; return true;
}

// =============================================================================
// Emulator state
// (gb_core.h declares GBState, PaletteMode, and the extern declarations for
// g_gb, g_gb_fb, g_fb_is_prepacked — we define them here.)
// =============================================================================

GBState   g_gb;
uint16_t* g_gb_fb          = nullptr;  // ~45 KB heap-allocated on game load
static bool g_emu_active       = false;
static bool g_wallpaper_evicted = false;  // true when wallpaper was cleared for a large ROM
PaletteMode g_palette_mode     = PaletteMode::GBC;
bool g_fb_is_rgb565            = false;   // true when CGB mode; set in gb_load_rom
bool g_fb_is_prepacked         = false;   // true when g_gb_fb stores RGBA4444 (DMG games)
bool g_lcd_ghosting            = false;   // 50/50 frame blend — per-game, off by default
bool g_lcd_grid                = false;   // LCD grid overlay — darkens inter-pixel gaps
bool g_fast_forward            = false;   // true while ZR double-click-hold is active

// Monotonically incrementing display-frame counter used for double-click timing.
static uint32_t g_frame_count = 0;

// Active DMG palette sub-arrays — written by gb_select_dmg_palette(), read every scanline.
uint16_t g_dmg_flat_pal[64] = {};   // filled by gb_select_dmg_palette()
int  g_gbc_pal_idx   = -1;          // index into GBC_TITLE_TABLE (-1 = not found)
bool g_gbc_pal_found = false;       // true if ROM was recognised in GBC_TITLE_TABLE

// =============================================================================
// Virtual on-screen button layout (overlay skin)
// All coordinates are in overlay pixels (448 × 720).
// Draw positions (top-left x, baseline y) are compile-time constants.
// Hit-test centres are derived at runtime from getTextDimensions() so they
// track the glyph's actual visual centre regardless of font metrics.
// =============================================================================

// D-pad — \uE110
static constexpr int DPAD_DRAW_X = 29;
static constexpr int DPAD_DRAW_Y = 634+4-4+14-4-2-2-2-10+4-2;   // baseline
static constexpr int DPAD_SIZE   = 134;
static constexpr int DPAD_R      = 65;

// A button — \uE0E0
static constexpr int ABTN_DRAW_X = 344;
static constexpr int ABTN_DRAW_Y = 594-4-2-2-10+4-2;
static constexpr int ABTN_SIZE   = 72;
static constexpr int ABTN_R      = 38;

// B button — \uE0E1
static constexpr int BBTN_DRAW_X = 289;
static constexpr int BBTN_DRAW_Y = 633-4-2-2-10+4-2;
static constexpr int BBTN_SIZE   = 58;
static constexpr int BBTN_R      = 31;

// Start (+) / Select (−)
static constexpr int FOOTER_Y     = FB_H;
static constexpr int BTN_GAP_HALF = 10;
static constexpr int START_SIZE   = 30;
static constexpr int START_R      = 24;
static constexpr int START_DRAW_X = FB_W / 2 + BTN_GAP_HALF;
static constexpr int START_DRAW_Y = 626 + 44 + 10;
static constexpr int SELECT_SIZE  = START_SIZE;
static constexpr int SELECT_R     = START_R;
static constexpr int SELECT_DRAW_X = FB_W / 2 - BTN_GAP_HALF - SELECT_SIZE;
static constexpr int SELECT_DRAW_Y = START_DRAW_Y;

// =============================================================================
// Overlay theme color globals
//
// Loaded by load_ovl_theme() at startup (gb_utils.hpp).
// Applied ONLY in overlay player modes; windowed mode never reads these.
// When a wallpaper is active, bg_col/bg_packed are overridden at draw time
// to 000000/D — not baked in here so toggling wallpaper takes effect live.
//
// Defaults: bg black/alpha-13, buttons+border #333333 (GBC-style dark grey).
// =============================================================================
static char       g_ovl_theme_name[64] = "default";
static tsl::Color g_ovl_bg_col    {0x0, 0x0, 0x0, 0xD};  // bg_color + bg_alpha
static tsl::Color g_ovl_dpad_col  {0x3, 0x3, 0x3, 0xF};  // dpad_button_color
static tsl::Color g_ovl_abtn_col  {0x3, 0x3, 0x3, 0xF};  // a_button_color
static tsl::Color g_ovl_bbtn_col  {0x3, 0x3, 0x3, 0xF};  // b_button_color
static tsl::Color g_ovl_start_col {0x3, 0x3, 0x3, 0xF};  // start_button_color
static tsl::Color g_ovl_select_col{0x3, 0x3, 0x3, 0xF};  // select_button_color
static tsl::Color g_ovl_bdr_col   {0x3, 0x3, 0x3, 0xF};  // border_color
static uint16_t   g_ovl_bg_packed = 0xD000u;              // packed RGBA4444 for direct-fb writes

// backdrop_color — opaque black backing shapes drawn behind button glyphs.
// Default: pure black, always fully opaque.
static tsl::Color g_ovl_backdrop_col {0x0, 0x0, 0x0, 0xF};

// frame_color / frame_alpha — fill color for the letterbox region (the area
// around the 2× game screen where "GAME BOY COLOR" is written).
// Default: #111111 at alpha 14 (0xE), matching the original 0xE111u packed value.
static tsl::Color g_ovl_frame_col  {0x1, 0x1, 0x1, 0xE};
static uint16_t   g_ovl_frame_packed = 0xE111u;  // packed RGBA4444; recomputed by load_ovl_theme()

// gb_text_color — color of the "GAME BOY" (and "GAME BOY COLOR") prefix text
// rendered inside the letterbox.  Default: white {0xF,0xF,0xF,0xF}.
static tsl::Color g_ovl_text_col {0xF, 0xF, 0xF, 0xF};

// Per-button glyph colors — driven by active overlay theme.
// BK (the backing shapes behind glyphs) is separate, driven by backdrop_color.

// Hit-test centres, populated on first draw from getTextDimensions().
static int   g_dpad_hx       = DPAD_DRAW_X  + DPAD_SIZE  / 2;
static int   g_dpad_hy       = DPAD_DRAW_Y  - DPAD_SIZE  / 2;
static int   g_abtn_hx       = ABTN_DRAW_X  + ABTN_SIZE  / 2;
static int   g_abtn_hy       = ABTN_DRAW_Y  - ABTN_SIZE  / 2;
static int   g_bbtn_hx       = BBTN_DRAW_X  + BBTN_SIZE  / 2;
static int   g_bbtn_hy       = BBTN_DRAW_Y  - BBTN_SIZE  / 2;
static int   g_start_hx      = START_DRAW_X + START_SIZE  / 2;
static int   g_start_hy      = START_DRAW_Y - START_SIZE  / 2;
static int   g_select_hx     = SELECT_DRAW_X + SELECT_SIZE / 2;
static int   g_select_hy     = SELECT_DRAW_Y - SELECT_SIZE / 2;
static bool  g_btns_measured = false;
//static int   g_div_half_w    = 0;
static float g_dpad_glyph_w  = 0.f;
static float g_dpad_glyph_h  = 0.f;

// Virtual key bitmask accumulated each frame from touch input.
static u64 g_touch_keys = 0;

// True GB frame period: 70224 T-cycles / 4194304 Hz ≈ 16.743 ms (59.73 fps).
static constexpr int64_t GB_RENDER_FRAME_NS =
    (int64_t)70224 * 1'000'000'000LL / (int64_t)4194304;  // 16742706 ns

// GB frame clock: 0 = unanchored, set to real time on first draw after load/resume.
static int64_t g_gb_frame_next_ns = 0;

// Focus-flash border — shared by GBWindowedElement and GBOverlayElement.
// Written by process_zl_pass_through(); read in both draw() paths.
// g_focus_flash counts down from 45 each draw; 0 = hidden.
// g_focus_flash_red: true = red (focus released), false = green (focus regained).
static int  g_focus_flash     = 0;
static bool g_focus_flash_red = false;

// =============================================================================
// Shared drag/reposition constants
// Used by both GBOverlayGui and GBWindowedGui for touch hold-to-drag and
// joystick reposition.  Defined once here so the two classes stay in sync.
// =============================================================================
static constexpr int      kHoldFrames   = 60;               // frames before drag activates (~1 s at 60 fps)
static constexpr uint64_t kPlusHoldNs   = 1'000'000'000ULL; // ns KEY_PLUS must be held alone before joystick drag
static constexpr int      kJoyDeadzone  = 20;               // HID stick dead-zone (range –32767..32767)
static constexpr float    kJoyBaseSens  = 0.00008f;         // x^8 curve minimum sensitivity
static constexpr float    kJoyMaxSens   = 0.0005f;          // x^8 curve maximum sensitivity

// =============================================================================
// Debounced dock-state cache
//
// ult::consoleIsDocked() is a system service call — calling it every display
// frame introduces unnecessary IPC overhead.  These two variables support the
// poll_console_docked() helper in gb_utils.hpp, which re-queries the service
// at most once per kDockCheckInterval frames (~1 second at 60 fps).
//
// g_dock_next_check is initialised to 0 so the very first call (when
// g_frame_count is also 0) satisfies the g_frame_count >= g_dock_next_check
// predicate and performs an immediate query — correct for all call sites
// including those in main() that run before tsl::loop() starts.
//
// RULE: never call ult::consoleIsDocked() directly in per-frame draw() or
// update() paths.  Use poll_console_docked() (gb_utils.hpp) everywhere.
// One-shot sites in main() (clamp_win_scale, setup_windowed_framebuffer) and
// event-driven sites (handleInput stick-press) should also use the helper so
// the cached value stays consistent across the codebase.
// =============================================================================
static bool     g_console_docked  = false;  // last known dock state
static uint32_t g_dock_next_check = 0;      // g_frame_count value at which to re-query

// =============================================================================
// Shared VI / touch-space bounds helpers
//
// Both GBWindowedGui and GBOverlayGui need these two identical calculations.
// Defining them once here as file-scope functions (internal linkage via static)
// gives the compiler a single definition to inline or emit — no duplicate
// symbols in .text, no separate static-member stubs per class.
//
// vi_max_x()    — maximum safe VI-space X so the layer stays on screen.
// touch_win_w() — layer width in HID touch space (0–1279); VI×2/3.
//
// Note: touch_win_h() intentionally stays per-class — windowed has a
// pixel-perfect branch while overlay does not.
// =============================================================================
static inline int vi_max_x()    { return 1920 - static_cast<int>(tsl::cfg::LayerWidth); }
static inline int touch_win_w() { return static_cast<int>(tsl::cfg::LayerWidth) * 2 / 3; }