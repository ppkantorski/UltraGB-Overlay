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
// Config persist helpers, per-game config, save/state/slot helpers, ROM
// validation, launch helpers → gb_utils.hpp
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

    const std::string wall_val = ult::parseValueFromIniSection(path, kConfigSection, kKeyOverlayWallpaper);
    if (!wall_val.empty())
        g_overlay_wallpaper = (wall_val == "1");

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

    // ovl_free_mode — only update when the session type has not already been
    // determined by main() before tsl::loop() started.
    //
    // main() sets g_overlay_free_mode (and g_overlay_mode) from argv BEFORE
    // calling tsl::loop(), which then calls initServices() → load_config().
    // If we unconditionally overwrite g_overlay_free_mode here we corrupt those
    // already-resolved flags, causing wrong routing in loadInitialGui():
    //
    //   • Windowed session  (g_win_scale_locked=true,  g_overlay_free_mode=false):
    //     If ovl_free_mode=1 is stored in config, load_config would flip
    //     g_overlay_free_mode to true, bypassing the windowed init block in
    //     initServices() and sending loadInitialGui() down the free-overlay path
    //     instead of GBWindowedGui — wrong framebuffer, wrong rendering.
    //
    //   • Fixed overlay session (g_overlay_mode=true, g_overlay_free_mode=false):
    //     Same corruption — loadInitialGui() would route to the free-overlay
    //     branch even though the layer was sized for a full-screen overlay.
    //
    // The guard mirrors the existing g_win_scale_locked guard on win_scale above.
    // Sessions that need this value freshly loaded (UI / menu) always have both
    // flags false at load_config() time.
    if (!g_win_scale_locked && !g_overlay_mode) {
        const std::string v = ult::parseValueFromIniSection(path, kConfigSection, kKeyOvlFreeMode);
        if (!v.empty()) g_overlay_free_mode = (v == "1");
    }

    // ovl_free_pos_x — VI-space X.  ovl_free_pos_y — render row offset (0..OVL_FREE_TOP_TRIM).
    // Old saves stored VI-space Y (0..126); clamping to OVL_FREE_TOP_TRIM migrates them safely.
    { int v = 0;
      if (parse_uint(ult::parseValueFromIniSection(path, kConfigSection, kKeyOvlFreePosX), v)) g_ovl_free_pos_x = v;
      if (parse_uint(ult::parseValueFromIniSection(path, kConfigSection, kKeyOvlFreePosY), v))
          g_ovl_free_pos_y = std::max(0, std::min((int)OVL_FREE_TOP_TRIM, v)); }
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
    //set_if_missing("pixel_perfect", "0");
    set_if_missing("windowed",      "0");
    set_if_missing("lcd_grid",      "0");
    set_if_missing("ingame_haptics", "1");
    set_if_missing("ingame_wallpaper", "0");
    set_if_missing("win_pos_x",     "840");
    set_if_missing("win_pos_y",     "432");
    set_if_missing("win_scale",     "1");
    set_if_missing("win_output",    "720");
    set_if_missing("ovl_free_mode", "0");
    set_if_missing("ovl_free_pos_x","0");
    set_if_missing("ovl_free_pos_y","84");
}

// The following functions were moved to gb_utils.hpp:
//   save_last_rom, save_lcd_grid, save_windowed_mode, save_win_pos,
//   save_win_output, save_win_scale,
//   ovl_filename, g_defaultCombos, remove_quick_combo_from_others,
//   register_quick_launch_mode,
//   rom_cgb_flag, rom_has_cgb_flag, build_game_config_path,
//   load/save_game_cfg_str, palette_mode_to/from_str, load/save_game_palette_mode,
//   next_palette_mode, palette_mode_label, load/save_game_lcd_ghosting,
//   load/save_game_no_sprite_limit,
//   build_rom_data_path, load_save, write_save,
//   STATE_MAGIC, STATE_VERSION, save_state, load_state,
//   build_game_slot_dir, build_slot_file_path, build_user_slot_{path,ts_path},
//   write/read_timestamp_to/from, write/read_slot_timestamp, make_slot_label,
//   newest_slot_label, newest_state_slot_label, newest_save_backup_slot_label,
//   build_save_backup_slot_{path,ts_path}, write/read_save_backup_timestamp,
//   file_exists, copy_file, delete_file,
//   backup_save_data_slot, restore_save_data_slot, save_user_slot, load_user_slot,
//   get_rom_size, rom_is_playable, rom_playability_message,
//   launch_windowed_mode, launch_overlay_mode, launch_game
// kROM_2MB, kROM_4MB, kROM_6MB → gb_globals.hpp

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
// gb_set_input — KEY_* macros (old libnx style used by this libtesla fork)
// GB joypad active-low: 0 = pressed
//   bit 0=A  1=B  2=Select  3=Start  4=Right  5=Left  6=Up  7=Down
// =============================================================================
void gb_set_input(u64 keysHeld) {
    if (!g_gb.running) return;
    uint8_t joy = 0xFF;
    if (keysHeld & KEY_A)     joy &= ~(1u << 0);
    if (keysHeld & KEY_B)     joy &= ~(1u << 1);
    if (keysHeld & (KEY_MINUS | KEY_Y)) joy &= ~(1u << 2);
    if (keysHeld & (KEY_PLUS  | KEY_X)) joy &= ~(1u << 3);
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
    if (quickModeSymbol && g_directMode && g_comboReturn) {
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
class GameSettingsGui;      // forward declare
class SettingsGui;          // forward declare
class SaveSlotsGui;          // forward declare  (merged SaveStatesGui + SaveDataGui)
class SlotActionGui;         // forward declare  (merged SlotActionGui + SaveDataSlotActionGui)
class ConfirmGui;            // forward declare
class QuickComboSelectorGui; // forward declare
class OvlThemeSelectorGui;   // forward declare

// Actions that flow through ConfirmGui.
// The enum is plain so it costs nothing at runtime — a single int stored in
// ConfirmGui's members, dispatched with a switch in do_yes() / do_back().
enum class ConfirmAction : uint8_t {
    StateSave,    // SlotActionGui   → save state to slot
    StateLoad,    // SlotActionGui   → load state from slot
    StateDelete,  // SlotActionGui   → delete slot file
    DataBackup,   // SaveDataSlotActionGui → backup live .sav to slot
    DataRestore,  // SaveDataSlotActionGui → restore slot .sav to live
    DataDelete,   // SaveDataSlotActionGui → delete slot .sav
};

// Returns the UI label string for a ConfirmAction.
static constexpr const char* confirm_action_label(ConfirmAction a) {
    switch (a) {
        case ConfirmAction::StateSave:   return "Save";
        case ConfirmAction::StateLoad:   return "Load";
        case ConfirmAction::StateDelete: return "Delete";
        case ConfirmAction::DataBackup:  return "Backup";
        case ConfirmAction::DataRestore: return "Restore";
        case ConfirmAction::DataDelete:  return "Delete";
        default:                         return "Confirm";
    }
}

// make_slot_detail_header — moved to gb_utils.hpp

// Deferred normal ROM launch: relaunch this overlay with -overlay so GBOverlayGui
// always starts in a fresh process (no residual menu heap / glyph state).
// All three call sites (RomSelectorGui, SlotActionGui, GameSettingsGui) use this
// single entry point — no swapTo<GBOverlayGui> anywhere in normal flow.
//static void launch_emulator(const char* romPath) {
//    // Do NOT clear g_settings_scroll here — the user expects to land back on the
//    // same Settings item after X-ing out of the emulator and returning to Settings.
//    launch_overlay_mode(romPath);
//    // tsl::swapTo<GBOverlayGui>(); — replaced by overlay relaunch above.
//}

// =============================================================================
// SlotActionGui — Save/Load/Delete (states) or Backup/Restore/Delete (save data)
//
// m_is_data=false → state slots (Save / Load / Delete)
// m_is_data=true  → save-data slots (Backup / Restore / Delete)
//
// Both modes share the same structure: a slot header, three action items with
// pre-flight empty-slot checks where appropriate, and B → parent slot list.
// Merging eliminates a full duplicate vtable + createUI + handleInput body.
// =============================================================================
class SlotActionGui : public tsl::Gui {
    std::string m_rom_path;
    std::string m_display_name;
    std::string m_jump_to;
    int         m_slot;
    bool        m_is_data;
public:
    SlotActionGui(std::string romPath, std::string displayName,
                  int slot, bool isData, std::string jumpTo = "")
        : m_rom_path(std::move(romPath))
        , m_display_name(std::move(displayName))
        , m_jump_to(std::move(jumpTo))
        , m_slot(slot)
        , m_is_data(isData)
    {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader(make_slot_detail_header(m_slot, m_display_name)));

        const std::string& romPath    = m_rom_path;
        const std::string& displayName = m_display_name;
        const int  slot   = m_slot;
        const bool isData = m_is_data;

        // ── Item 1: Save / Backup ─────────────────────────────────────────────
        // No pre-flight — ConfirmGui::do_yes handles the empty-source case.
        const ConfirmAction act1 = isData ? ConfirmAction::DataBackup : ConfirmAction::StateSave;
        auto* item1 = new tsl::elm::SilentListItem(isData ? "Backup" : "Save");
        item1->setClickListener([romPath, displayName, slot, act1](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            triggerEnterFeedback();
            tsl::swapTo<ConfirmGui>(romPath, displayName, slot, act1);
            return true;
        });
        list->addItem(item1);

        // ── Item 2: Load / Restore ────────────────────────────────────────────
        // Pre-flight: bail early if the slot file doesn't exist yet.
        const ConfirmAction act2 = isData ? ConfirmAction::DataRestore : ConfirmAction::StateLoad;
        auto* item2 = new tsl::elm::SilentListItem(isData ? "Restore" : "Load");
        item2->setClickListener([romPath, displayName, slot, isData, act2](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            char slotPath[PATH_BUFFER_SIZE] = {};
            if (isData) build_save_backup_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            else        build_user_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)) { triggerWallFeedback(); show_notify(SLOT_IS_EMPTY_WARNING); return true; }
            triggerEnterFeedback();
            tsl::swapTo<ConfirmGui>(romPath, displayName, slot, act2);
            return true;
        });
        list->addItem(item2);

        // ── Item 3: Delete ────────────────────────────────────────────────────
        // Same path builder as item 2.
        const ConfirmAction act3 = isData ? ConfirmAction::DataDelete : ConfirmAction::StateDelete;
        auto* item3 = new tsl::elm::SilentListItem("Delete");
        item3->setClickListener([romPath, displayName, slot, isData, act3](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            char slotPath[PATH_BUFFER_SIZE] = {};
            if (isData) build_save_backup_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            else        build_user_slot_path(romPath.c_str(), slot, slotPath, sizeof(slotPath));
            if (!file_exists(slotPath)) { triggerWallFeedback(); show_notify(SLOT_IS_EMPTY_WARNING); return true; }
            triggerEnterFeedback();
            tsl::swapTo<ConfirmGui>(romPath, displayName, slot, act3);
            return true;
        });
        list->addItem(item3);

        // Restore focus to the item clicked before entering ConfirmGui.
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
            tsl::swapTo<SaveSlotsGui>(m_rom_path, m_display_name,
                                      make_slot_label(m_slot), m_is_data);
            return true;
        }
        return false;
    }
};

// =============================================================================
// ConfirmGui — single-purpose "Yes / No" confirmation screen
//
// Sits between SlotActionGui / SaveDataSlotActionGui and the actual action.
// The CategoryHeader shows the action name (e.g. "Save", "Delete").
//
//   Yes — executes the action via do_yes(), then navigates onward exactly as
//         the old direct-click code did.
//   No  — calls do_back(), returning to the originating action Gui with focus
//         restored to the item that was just clicked.
//   B   — identical to No.
//
// Memory: one enum byte + three strings + one int — no std::function, no heap
// closures beyond what tsl::elm already allocates for its list items.
// =============================================================================
class ConfirmGui : public tsl::Gui {
    std::string  m_rom_path;
    std::string  m_display_name;
    int          m_slot;
    ConfirmAction m_action;

    // Navigate back to the originating action Gui, restoring focus.
    void do_back() {
        triggerExitFeedback();
        const char* label  = confirm_action_label(m_action);
        const bool  isData = (m_action >= ConfirmAction::DataBackup);
        tsl::swapTo<SlotActionGui>(m_rom_path, m_display_name, m_slot, isData, label);
    }

    // Execute the confirmed action, then navigate onward.
    void do_yes() {
        switch (m_action) {

            case ConfirmAction::StateSave:
                if (save_user_slot(m_rom_path.c_str(), m_slot)) {
                    //triggerEnterFeedback();
                    //triggerRumbleClick.store(true, std::memory_order_release);
                    tsl::swapTo<SaveSlotsGui>(m_rom_path, m_display_name, make_slot_label(m_slot), false);
                    show_notify("State saved.");
                } else {
                    triggerWallFeedback();
                    show_notify("No state to save yet.");
                    do_back();
                }
                break;

            case ConfirmAction::StateLoad: {
                if (!load_user_slot(m_rom_path.c_str(), m_slot)) {
                    triggerWallFeedback();
                    show_notify("Load failed.");
                    do_back();
                    break;
                }
                // gb_load_rom will find the internal state and resume from it.
                launch_game(m_rom_path.c_str());
                break;
            }

            case ConfirmAction::StateDelete: {
                char slotPath[PATH_BUFFER_SIZE] = {};
                build_user_slot_path(m_rom_path.c_str(), m_slot, slotPath, sizeof(slotPath));
                delete_file(slotPath);
                char tsPath[PATH_BUFFER_SIZE] = {};
                build_user_slot_ts_path(m_rom_path.c_str(), m_slot, tsPath, sizeof(tsPath));
                delete_file(tsPath);
                tsl::swapTo<SaveSlotsGui>(m_rom_path, m_display_name, make_slot_label(m_slot), false);
                triggerFeedbackImpl(triggerRumbleClick, triggerMoveSound);
                show_notify("Slot deleted.");
                break;
            }

            case ConfirmAction::DataBackup:
                if (backup_save_data_slot(m_rom_path.c_str(), m_slot)) {
                    //triggerEnterFeedback();
                    show_notify("Save data backed up.");
                    tsl::swapTo<SaveSlotsGui>(m_rom_path, m_display_name, make_slot_label(m_slot), true);
                } else {
                    triggerWallFeedback();
                    show_notify("No save data found.");
                    do_back();
                }
                break;

            case ConfirmAction::DataRestore:
                if (!restore_save_data_slot(m_rom_path.c_str(), m_slot)) {
                    triggerWallFeedback();
                    show_notify("Restore failed.");
                    do_back();
                    break;
                }
                tsl::swapTo<SaveSlotsGui>(m_rom_path, m_display_name, make_slot_label(m_slot), true);
                //triggerEnterFeedback();
                show_notify("Save data restored.");
                break;

            case ConfirmAction::DataDelete: {
                char slotPath[PATH_BUFFER_SIZE] = {};
                build_save_backup_slot_path(m_rom_path.c_str(), m_slot, slotPath, sizeof(slotPath));
                delete_file(slotPath);
                char tsPath[PATH_BUFFER_SIZE] = {};
                build_save_backup_slot_ts_path(m_rom_path.c_str(), m_slot, tsPath, sizeof(tsPath));
                delete_file(tsPath);
                tsl::swapTo<SaveSlotsGui>(m_rom_path, m_display_name, make_slot_label(m_slot), true);
                triggerFeedbackImpl(triggerRumbleClick, triggerMoveSound);
                show_notify("Slot deleted.");
                break;
            }
        }
    }

public:
    ConfirmGui(std::string romPath, std::string displayName, int slot, ConfirmAction action)
        : m_rom_path(std::move(romPath))
        , m_display_name(std::move(displayName))
        , m_slot(slot)
        , m_action(action)
    {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();

        // Header names the action — "Save", "Load", "Delete", "Backup", etc.
        list->addItem(new tsl::elm::CategoryHeader(
            std::string(make_slot_label(m_slot)) +
            " " + ult::DIVIDER_SYMBOL + " " + confirm_action_label(m_action)));

        auto* yesItem = new tsl::elm::SilentListItem("Yes");
        yesItem->setClickListener([this](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            if (m_action != ConfirmAction::DataDelete)
                triggerEnterFeedback();
            do_yes();
            return true;
        });
        list->addItem(yesItem);

        auto* noItem = new tsl::elm::SilentListItem("No");
        noItem->setClickListener([this](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            do_back();
            return true;
        });
        list->addItem(noItem);

        return make_bare_frame(list);
    }

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;
        if (keysDown & KEY_B) {
            do_back();
            return true;
        }
        return false;
    }
};


// =============================================================================
// SaveSlotsGui — 10-slot list for save states (isData=false) or save-data
//               backups (isData=true).
//
// Merges the former SaveStatesGui and SaveDataGui — only three runtime values
// differ between the two modes: the header prefix, the timestamp reader, and
// the B-handler jump label.  A single vtable + createUI + handleInput covers
// both, saving two full class definitions and their swapTo instantiations.
// =============================================================================
class SaveSlotsGui : public tsl::Gui {
    std::string m_rom_path;
    std::string m_display_name;
    std::string m_jump_to;
    bool        m_is_data;
public:
    SaveSlotsGui(std::string romPath, std::string displayName,
                 std::string jumpTo, bool isData)
        : m_rom_path(std::move(romPath))
        , m_display_name(std::move(displayName))
        , m_jump_to(std::move(jumpTo))
        , m_is_data(isData)
    {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader(
            (m_is_data ? "Save Data" : "Save States") +
            (" " + ult::DIVIDER_SYMBOL + " " + m_display_name)));

        const std::string romPath    = m_rom_path;
        const std::string displayName = m_display_name;
        const bool isData = m_is_data;

        for (int i = 0; i < 10; ++i) {
            char ts[48] = {};
            if (isData) read_save_backup_timestamp(romPath.c_str(), i, ts, sizeof(ts));
            else        read_slot_timestamp(romPath.c_str(), i, ts, sizeof(ts));

            const int slot = i;
            auto* item = new tsl::elm::MiniListItem(make_slot_label(i), ts);
            item->setClickListener([romPath, displayName, slot, isData](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                tsl::swapTo<SlotActionGui>(romPath, displayName, slot, isData);
                return true;
            });
            list->addItem(item);
        }

        const std::string jumpTarget = !m_jump_to.empty()
            ? m_jump_to
            : (isData ? newest_save_backup_slot_label(m_rom_path.c_str())
                      : newest_state_slot_label(m_rom_path.c_str()));
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
            tsl::swapTo<GameSettingsGui>(m_rom_path, m_display_name,
                m_is_data ? "Save Data" : "Save States");
            return true;
        }
        return false;
    }
};

// =============================================================================
// GameSettingsGui — per-game settings screen
//
// Opened by pressing Y on a ROM entry in RomSelectorGui.
//
// Items shown:
//   • "Pallet Mode" — cycling (GBC → DMG → Native) for DMG and CGB-compatible games;
//                    read-only "GBC" for CGB-only games (0xC0 header flag)
//   • "Save States" — opens SaveStatesGui with 10 named slots
//   • "Reset"       — cold-boots the game (deletes internal state first)
//
// Header: "Settings ◆ <GAMENAME>"
// B returns to RomSelectorGui.
// =============================================================================
class GameSettingsGui : public tsl::Gui {
    std::string m_rom_path;
    std::string m_display_name;
    std::string m_jump_to;   // item label to restore scroll on re-entry
public:
    GameSettingsGui(std::string romPath, std::string displayName, std::string jumpTo = "")
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
            tsl::swapTo<SaveSlotsGui>(romPath, displayName, "", false);
            return true;
        });
        list->addItem(statesItem);

        // ── Save Data ─────────────────────────────────────────────────────
        auto* saveDataItem = new tsl::elm::ListItem("Save Data");
        saveDataItem->setValue(ult::DROPDOWN_SYMBOL);
        saveDataItem->setClickListener([romPath, displayName](u64 keys) -> bool {
            if (!(keys & KEY_A)) return false;
            triggerEnterFeedback();
            tsl::swapTo<SaveSlotsGui>(romPath, displayName, "", true);
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

        // ── Core Presets ───────────────────────────────────────────────────
        list->addItem(new tsl::elm::CategoryHeader("Core Presets"));


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
// =============================================================================
// OvlThemeSelectorGui — overlay colour theme picker
//
// Lists all .ini files in OVL_THEMES_DIR (excluding any "default.ini") plus
// a built-in "default" entry.  Selecting a theme:
//   • copies the chosen .ini over OVL_THEME_FILE (or rewrites defaults for
//     the built-in "default" entry)
//   • calls load_ovl_theme() so colors take effect immediately
//   • swaps back to SettingsGui (same as clicking A on a theme)
// B also returns to SettingsGui without changing the theme.
//
// No heap is held between visits — the list is rebuilt on each createUI().
// =============================================================================
static int ovl_theme_filter(const struct dirent* e) {
    const char* n = e->d_name;
    const size_t len = strlen(n);
    if (len < 5) return 0;                          // too short to end in ".ini"
    if (strcmp(n, "default.ini") == 0) return 0;   // "default" is the built-in entry
    return (strcmp(n + len - 4, ".ini") == 0) ? 1 : 0;
}

class OvlThemeSelectorGui : public tsl::Gui {
    std::string m_rom_scroll;
    std::string m_settings_scroll;
    tsl::elm::ListItem* m_lastSelected = nullptr;

    // Overwrite OVL_THEME_FILE with the chosen theme and reload all colors.
    // Persist the display name to config.ini (ovl_theme) separately.
    // srcPath == nullptr means "default" — write canonical default values.
    void apply_theme(const char* name, const char* srcPath) {
        if (srcPath) {
            copy_file(srcPath, OVL_THEME_FILE);
        } else {
            // "default" — overwrite OVL_THEME_FILE with canonical values.
            ult::setIniFileValue(OVL_THEME_FILE, "theme", "bg_color",      "000000",  "");
            ult::setIniFileValue(OVL_THEME_FILE, "theme", "bg_alpha",      "13",      "");
            ult::setIniFileValue(OVL_THEME_FILE, "theme", "button_color",  "333333",  "");
            ult::setIniFileValue(OVL_THEME_FILE, "theme", "border_color",  "333333",  "");
            ult::setIniFileValue(OVL_THEME_FILE, "theme", "backdrop_color","000000",  "");
            ult::setIniFileValue(OVL_THEME_FILE, "theme", "frame_color",   "111111",  "");
            ult::setIniFileValue(OVL_THEME_FILE, "theme", "frame_alpha",   "14",      "");
            ult::setIniFileValue(OVL_THEME_FILE, "theme", "gb_text_color", "ffffff",  "");
        }
        ult::setIniFileValue(kConfigFile, kConfigSection, kKeyOvlTheme, name, "");
        load_ovl_theme();
    }

public:
    OvlThemeSelectorGui(std::string romScroll, std::string settingsScroll)
        : m_rom_scroll(std::move(romScroll))
        , m_settings_scroll(std::move(settingsScroll)) {}

    virtual tsl::elm::Element* createUI() override {
        auto* list = new tsl::elm::List();
        list->addItem(new tsl::elm::CategoryHeader("Overlay Theme"));

        const std::string current(g_ovl_theme_name);
        std::string jumpTarget;

        // ── Built-in "default" entry ──────────────────────────────────────────
        {
            const bool isCurrent = (current == "default");
            if (isCurrent) jumpTarget = "default";

            auto* item = new tsl::elm::ListItem("default");
            if (isCurrent) {
                item->setValue(ult::CHECKMARK_SYMBOL);
                m_lastSelected = item;
            }
            item->setClickListener([this, item](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                apply_theme("default", nullptr);
                if (m_lastSelected && m_lastSelected != item)
                    m_lastSelected->setValue("");
                item->setValue(ult::CHECKMARK_SYMBOL);
                m_lastSelected = item;
                triggerEnterFeedback();
                tsl::swapTo<SettingsGui>(m_rom_scroll, m_settings_scroll);
                return true;
            });
            list->addItem(item);
        }

        // ── Theme files from OVL_THEMES_DIR ───────────────────────────────────
        // scandir returns alphabetically-sorted entries; each is freed immediately
        // after the ListItem is built so peak allocation is one dirent at a time.
        {
            struct dirent** entries = nullptr;
            const int n = scandir(OVL_THEMES_DIR, &entries, ovl_theme_filter, alphasort);
            for (int i = 0; i < n; ++i) {
                const std::string fname{entries[i]->d_name};
                free(entries[i]);
                entries[i] = nullptr;

                // Strip ".ini" → display and key name
                const std::string tname = fname.substr(0, fname.size() - 4);
                const std::string tpath = std::string(OVL_THEMES_DIR) + fname;

                const bool isCurrent = (current == tname);
                if (isCurrent && jumpTarget.empty()) jumpTarget = tname;

                auto* item = new tsl::elm::ListItem(tname);
                if (isCurrent) {
                    item->setValue(ult::CHECKMARK_SYMBOL);
                    m_lastSelected = item;
                }
                item->setClickListener([this, item, tname, tpath](u64 keys) -> bool {
                    if (!(keys & KEY_A)) return false;
                    apply_theme(tname.c_str(), tpath.c_str());
                    if (m_lastSelected && m_lastSelected != item)
                        m_lastSelected->setValue("");
                    item->setValue(ult::CHECKMARK_SYMBOL);
                    m_lastSelected = item;
                    triggerEnterFeedback();
                    tsl::swapTo<SettingsGui>(m_rom_scroll, m_settings_scroll);
                    return true;
                });
                list->addItem(item);
            }
            free(entries);  // safe on nullptr (n <= 0 case)
        }

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
    tsl::elm::ListItem* m_theme_item      = nullptr;  ///< "Overlay Theme"  — value refreshed on return from OvlThemeSelectorGui.
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

        // ── Overlay Position ──────────────────────────────────────────────────
        // "Fixed" (default) — full overlay with title/widget chrome at the top,
        //   identical to the current behaviour.
        // "Free"  — the top OVL_FREE_TOP_TRIM rows (title/widget region) are
        //   stripped from the layer.  The layer can be repositioned by holding
        //   KEY_PLUS for 1 s then moving the left stick, exactly like windowed
        //   mode.  Position is stored independently from the windowed position.
        {
            auto* pos_item = new tsl::elm::ListItem(
                "Overlay Position",
                g_overlay_free_mode ? "Free" : "Fixed");
            pos_item->setClickListener([pos_item](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                g_overlay_free_mode = !g_overlay_free_mode;
                pos_item->setValue(g_overlay_free_mode ? "Free" : "Fixed");
                save_ovl_free_mode();
                return true;
            });
            list->addItem(pos_item);
        }

        if (ult::expandedMemory) {
            auto* wallpaper_item = new tsl::elm::ToggleListItem(
                "Overlay Wallpaper", g_overlay_wallpaper, ult::ON, ult::OFF);
            wallpaper_item->setStateChangedListener([](bool state) {
                g_overlay_wallpaper = state;
                ult::setIniFileValue(kConfigFile, kConfigSection, kKeyOverlayWallpaper,
                                     state ? "1" : "0", "");
            });
            list->addItem(wallpaper_item);
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

        // ── Overlay Theme ─────────────────────────────────────────────────────
        // Selects bg/border/button-glyph colours for overlay player modes only.
        {
            auto* th_item = new tsl::elm::ListItem("Overlay Theme",
                                                    std::string(g_ovl_theme_name));
            th_item->setClickListener([this](u64 keys) -> bool {
                if (!(keys & KEY_A)) return false;
                set_settings_scroll("Overlay Theme");
                tsl::swapTo<OvlThemeSelectorGui>(m_rom_scroll,
                                                  std::string("Overlay Theme"));
                return true;
            });
            m_theme_item = th_item;
            list->addItem(th_item);
        }

        auto* haptics_item = new tsl::elm::ToggleListItem(
            "In-Game Haptics", g_ingame_haptics, ult::ON, ult::OFF);
        haptics_item->setStateChangedListener([](bool state) {
            g_ingame_haptics = state;
            ult::setIniFileValue(kConfigFile, kConfigSection, kKeyIngameHaptics,
                                 state ? "1" : "0", "");
        });
        list->addItem(haptics_item);

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
            close_overlay_direct_mode();
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
// strip_rom_extension
//
// Returns the ROM basename with its file extension removed for display purposes.
// Strips ".gb" and ".gbc" (case-insensitive).  The original filename (with
// extension) is still used for all path operations; this affects display only.
// =============================================================================
static std::string strip_rom_extension(const char* name) {
    std::string s(name);
    const size_t dot = s.rfind('.');
    if (dot == std::string::npos) return s;
    std::string ext = s.substr(dot);
    for (char& c : ext) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    if (ext == ".gb" || ext == ".gbc")
        s.erase(dot);
    return s;
}

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
            "Game Boy Games " + ult::DIVIDER_SYMBOL + " \uE0E3 Settings";
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
            // If returning from GameSettingsGui, m_jump_to holds the configured ROM's
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

                // Strip extension for display — path reconstruction still uses `name`.
                const std::string displayStr = strip_rom_extension(name);

                if (inProgress && inProgressLabel.empty())
                    inProgressLabel = displayStr;

                auto* item = new tsl::elm::SilentListItem(displayStr.c_str(), inProgress ? ult::INPROGRESS_SYMBOL : "", true);
                if (!playable)
                    item->setTextColor(tsl::warningTextColor);

                // Capture the basename WITH extension for path reconstruction, and
                // the stripped display string for UI labels and scroll restoration.
                const std::string romNameStr(name);
                
                item->setClickListener([romNameStr, displayStr, playable](u64 keys) -> bool {
                    char p[PATH_BUFFER_SIZE];
                    snprintf(p, sizeof(p), "%s%s", g_rom_dir, romNameStr.c_str());
                    strncpy(g_rom_selector_scroll, displayStr.c_str(), sizeof(g_rom_selector_scroll) - 1);
                    g_rom_selector_scroll[sizeof(g_rom_selector_scroll) - 1] = '\0';
                    if (keys & KEY_Y) {
                        triggerRumbleClick.store(true, std::memory_order_release);
                        triggerSettingsSound.store(true, std::memory_order_release);
                        tsl::swapTo<GameSettingsGui>(p, displayStr);
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
        // Footer: right-arrow "Config" button.
        // frame takes ownership of list; Tesla takes ownership of frame.
        auto* frame = new UltraGBOverlayFrame("", "Configure");
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
        // On the Games page the footer shows right-arrow "Config".
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
            close_overlay_direct_mode();
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
        write_default_ovl_theme_if_missing();
        load_ovl_theme();
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
        if (g_win_scale_locked && !g_overlay_free_mode) {
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

            // Free-overlay quicklaunch: g_overlay_rom_path wasn't written to
            // config (no launch_free_overlay_mode call), so construct it from
            // g_last_rom_path + g_rom_dir — both now populated by load_config().
            if (g_overlay_free_mode && !g_overlay_rom_path[0] && g_quick_launch && g_last_rom_path[0]) {
                char full[PATH_BUFFER_SIZE];
                snprintf(full, sizeof(full), "%s%s", g_rom_dir, g_last_rom_path);
                strncpy(g_overlay_rom_path, full, sizeof(g_overlay_rom_path) - 1);
                g_overlay_rom_path[sizeof(g_overlay_rom_path) - 1] = '\0';
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
            if (g_win_scale_locked && !g_overlay_free_mode) {
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
        if (g_win_scale_locked && !g_overlay_free_mode)
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

        // ── Free overlay player ──────────────────────────────────────────────────
        // Must be checked before g_win_scale_locked because free overlay sets
        // g_win_scale_locked=true for windowed layer sizing while still needing
        // GBOverlayGui (not GBWindowedGui).
        if (g_overlay_free_mode && g_overlay_rom_path[0]) {
            if (const char* msg = rom_playability_message(g_overlay_rom_path)) {
                show_notify(msg);
                g_overlay_mode      = false;
                g_overlay_free_mode = false;
                if (g_self_path[0]) {
                    tsl::setNextOverlay(g_self_path);
                    tsl::Overlay::get()->close();
                }
                return initially<RomSelectorGui>();
            }
            if (ult::expandedMemory)
                ult::loadWallpaperFileWhenSafe();
            strncpy(g_pending_rom_path, g_overlay_rom_path, sizeof(g_pending_rom_path) - 1);
            g_pending_rom_path[sizeof(g_pending_rom_path) - 1] = '\0';
            g_overlay_rom_path[0] = '\0';
            return initially<GBOverlayGui>();
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
    ult::DefaultFramebufferWidth   = static_cast<u32>(GB_W * g_win_scale);
    // windowedLayerPixelPerfect must be resolved BEFORE win_fb_height() so the
    // half-screen height uses the correct base (720 for 720p, 1080 for 1080p).
    ult::windowedLayerPixelPerfect = g_win_1080 && poll_console_docked();
    // Anchor mode (top vs bottom) is computed dynamically from g_win_pos_y each
    // frame via anchor_bottom() in gb_windowed.hpp — no session flag needed.
    ult::DefaultFramebufferHeight  = static_cast<u32>(win_fb_height());
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
    Menu,              // cold start, -returning, quicklaunch-windowed tooLarge fallback
    Windowed,          // -windowed, or -quicklaunch with windowed=1 and ROM fits
    OverlayPlayer,     // -overlay  (ROM path written to config by launch_overlay_mode)
    FreeOverlayPlayer, // -freeoverlay (trimmed 448×613 layer, repositionable)
    QuickLaunch,       // -quicklaunch with windowed=0
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
        else if (strcmp(argv[i], "--comboReturn") == 0) { g_comboReturn = true;}
        else if (strcmp(argv[i], "-overlay")     == 0) { g_overlay_mode = true; wantOverlayPlayer = true; skipRumbleDoubleClick = false; }
        else if (strcmp(argv[i], "-freeoverlay") == 0) { g_overlay_mode = true; g_overlay_free_mode = true; wantOverlayPlayer = true; skipRumbleDoubleClick = false; }
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
        const bool        windowed    = ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyWindowed)     == "1";
        const bool        overlayFree = !windowed &&
                                        ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyOvlFreeMode) == "1";
        // Set globals early so Phase 3 and initServices() can see them.
        if (overlayFree) {
            g_overlay_free_mode = true;
            g_overlay_mode      = true;
        }

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
            } else if (overlayFree) {
                // Free-overlay quicklaunch: route through FreeOverlayPlayer so
                // Phase 3 sets DefaultFramebufferHeight = OVL_FREE_FB_H before
                // tsl::loop() and Renderer::init() allocate the framebuffer.
                session = SessionType::FreeOverlayPlayer;
            } else {
                session = SessionType::QuickLaunch;
            }
        }
        // lastRom empty → stays SessionType::Menu.

    } else if (wantWindowed) {
        // Explicit relaunch — ROM path already in config under "windowed_rom".
        session = SessionType::Windowed;

    } else if (wantOverlayPlayer) {
        // ROM path already in config under "overlay_rom" (set by launch_overlay_mode /
        // launch_free_overlay_mode).  Route to the correct session sub-type.
        session = g_overlay_free_mode ? SessionType::FreeOverlayPlayer
                                      : SessionType::OverlayPlayer;
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
        // Read the saved window Y position now so setup_windowed_framebuffer()
        // so g_win_pos_y is populated before setup_windowed_framebuffer() and createUI().
        // centre).  X is also read here for consistency; load_config() will
        // re-read both later but g_win_scale_locked prevents it from clobbering
        // the stored value.  parse_uint leaves globals at their defaults (0)
        // on empty/invalid string, which is a safe top-anchor position.
        { int v = 0;
          if (parse_uint(ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyWinPosX), v))
              g_win_pos_x = v;
          if (parse_uint(ult::parseValueFromIniSection(kConfigFile, kConfigSection, kKeyWinPosY), v))
              g_win_pos_y = v; }
        g_win_limited_fb = (earlyLimited && g_win_scale == 3);  // simple FB: only scale 3 on 4MB heap needs this
        setup_windowed_framebuffer();   // sets g_win_scale_locked; reads g_win_limited_fb via win_fb_height()

    } else if (session == SessionType::FreeOverlayPlayer) {
        // Free overlay: 448×OVL_FREE_FB_H framebuffer, windowed-style floating
        // layer sized at 1.5× (= 672×954 VI pixels at 720p).  Setting
        // g_win_scale_locked=true makes Tesla use windowed layer sizing instead
        // of the full-screen 1280×720 stretch, so the layer can be freely
        // repositioned in both X and Y.  loadInitialGui() checks g_overlay_free_mode
        // before g_win_scale_locked so it still routes to GBOverlayGui, not
        // GBWindowedGui.
        g_win_scale_locked            = true;
        ult::DefaultFramebufferWidth  = static_cast<u32>(FB_W);
        ult::DefaultFramebufferHeight = static_cast<u32>(OVL_FREE_FB_H);
        ult::windowedLayerPixelPerfect = false;  // 720p 1.5× scaling

    } else if (session == SessionType::Menu) {
        returnOverlayPath = ult::OVERLAY_PATH + "ovlmenu.ovl";
    }

    // One tsl::loop<Overlay> instantiation handles every session type.
    // Overlay::initServices() reads g_win_scale_locked / g_overlay_mode /
    // g_quick_launch to set m_game_session and route accordingly.
    return tsl::loop<Overlay, tsl::impl::LaunchFlags::None>(argc, argv);
}