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
 *     ZL + ZR — pause and return to ROM picker (state preserved)
 *     System overlay combo — hides overlay (game pauses automatically)
 ********************************************************************************/

#define NDEBUG
#define STBTT_STATIC
#define TESLA_INIT_IMPL

#include <ultra.hpp>
#include <tesla.hpp>

#include "gb_core.h"
#include "gb_renderer.h"
#include "gb_audio.h"       // ← GB APU → audout bridge
#include "elm_volume.hpp"    // ← VolumeTrackBar

#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include <algorithm>

using namespace ult;

// =============================================================================
// Paths / config
// =============================================================================
static constexpr const char* CONFIG_DIR  = "sdmc:/config/ultragb/";
static constexpr const char* CONFIG_FILE = "sdmc:/config/ultragb/config.ini";
static constexpr const char* SAVE_DIR    = "sdmc:/config/ultragb/saves/";
static constexpr const char* STATE_DIR   = "sdmc:/config/ultragb/states/";
static char g_rom_dir[256]       = "sdmc:/roms/gb/";
static char g_last_rom_path[256] = {};   // basename of last-played ROM, persisted to config.ini

static void load_config() {
    const std::string path = CONFIG_FILE;

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

    // original_palette
    const std::string pal_val = ult::parseValueFromIniSection(path, "config", "original_palette");
    if (!pal_val.empty())
        g_original_palette = (pal_val == "true" || pal_val == "1");

    // last_rom — basename of the last-played ROM file
    const std::string last_rom_val = ult::parseValueFromIniSection(path, "config", "last_rom");
    if (!last_rom_val.empty() && last_rom_val.size() < sizeof(g_last_rom_path) - 1)
        strncpy(g_last_rom_path, last_rom_val.c_str(), sizeof(g_last_rom_path) - 1);

    // volume — master GB audio volume (0–100), default 100
    const std::string vol_val = ult::parseValueFromIniSection(path, "config", "volume");
    if (!vol_val.empty()) {
        const int v = std::stoi(vol_val);
        gb_audio_set_volume(static_cast<u8>(std::clamp(v, 0, 100)));
    }
}

static void write_default_config_if_missing() {
    const std::string path = CONFIG_FILE;
    // Write keys only if they don't already exist
    const std::string existing_dir = ult::parseValueFromIniSection(path, "config", "rom_dir");
    if (existing_dir.empty())
        ult::setIniFileValue(path, "config", "rom_dir", g_rom_dir,
                             "Set rom_dir to your .gb/.gbc folder");
    const std::string existing_pal = ult::parseValueFromIniSection(path, "config", "original_palette");
    if (existing_pal.empty())
        ult::setIniFileValue(path, "config", "original_palette", "true",
                             "true = classic DMG greyscale, false = GB color");
    const std::string existing_vol = ult::parseValueFromIniSection(path, "config", "volume");
    if (existing_vol.empty())
        ult::setIniFileValue(path, "config", "volume", "100",
                             "Master GB audio volume (0-100)");
}

// Persist the basename of the just-launched ROM so the selector can jump to it on re-entry.
static void save_last_rom(const char* fullPath) {
    const char* sl = strrchr(fullPath, '/');
    const char* base = sl ? sl + 1 : fullPath;
    strncpy(g_last_rom_path, base, sizeof(g_last_rom_path) - 1);
    ult::setIniFileValue(std::string(CONFIG_FILE), "config", "last_rom", base, "");
}


// =============================================================================
GBState  g_gb;
uint16_t g_gb_fb[GB_W * GB_H] = {};
static bool g_emu_active = false;
static bool g_wallpaper_evicted = false;  // true when wallpaper was cleared for a large ROM
bool g_original_palette = true;   // true = DMG greyscale, false = GB green tint
bool g_fb_is_rgb565     = false;  // true when CGB mode; set in gb_load_rom

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

// Start (+) — \uE0F1   and   Select (−) — \uE0F2
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
static void build_save_path(const char* romPath, char* out, size_t outSz) {
    const char* slash = strrchr(romPath, '/');
    const char* base  = slash ? slash + 1 : romPath;
    char bn[256] = {};
    strncpy(bn, base, sizeof(bn)-1);
    char* dot = strrchr(bn, '.');
    if (dot) *dot = '\0';
    snprintf(out, outSz, "%s%s.sav", SAVE_DIR, bn);
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
    mkdir(SAVE_DIR, 0777);
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
//   [4]  uint32  version      = 3
//   [8]  uint32  cart_ram_sz  cart RAM size in bytes (0 if none)
//   [12] uint32  fb_bytes     framebuffer size = GB_W*GB_H*2
//   [16] gb_s    core         full emulator struct (function pointers re-patched on load)
//   [+]  uint8[] cart_ram     cart RAM contents (cart_ram_sz bytes)
//   [+]  uint16[] framebuffer last rendered frame (fb_bytes bytes)
//   [+]  GBAPU   apu_snapshot full APU runtime state (v3+)
//
// v3 adds the GBAPU snapshot so audio resumes correctly without any silence
// gap.  v1 files are still accepted; APU state is not restored from them
// (gb_audio_reset_regs() is called to get a safe starting point instead).
// =============================================================================
static constexpr uint32_t STATE_MAGIC   = 0x47425354u; // 'GBST'
static constexpr uint32_t STATE_VERSION = 3u;

static void build_state_path(const char* romPath, char* out, size_t outSz) {
    const char* slash = strrchr(romPath, '/');
    const char* base  = slash ? slash + 1 : romPath;
    char bn[256] = {};
    strncpy(bn, base, sizeof(bn)-1);
    char* dot = strrchr(bn, '.');
    if (dot) *dot = '\0';
    snprintf(out, outSz, "%s%s.state", STATE_DIR, bn);
}

static void save_state(GBState& s) {
    if (!s.rom || !s.romPath[0]) return;

    mkdir(STATE_DIR, 0777);

    char statePath[256] = {};
    build_state_path(s.romPath, statePath, sizeof(statePath));

    FILE* f = fopen(statePath, "wb");
    if (!f) return;

    const uint32_t magic   = STATE_MAGIC;
    const uint32_t version = STATE_VERSION;
    const uint32_t ramSz   = static_cast<uint32_t>(s.cartRamSz);
    const uint32_t fbBytes = static_cast<uint32_t>(GB_W * GB_H * sizeof(uint16_t));

    fwrite(&magic,   sizeof(magic),   1, f);
    fwrite(&version, sizeof(version), 1, f);
    fwrite(&ramSz,   sizeof(ramSz),   1, f);
    fwrite(&fbBytes, sizeof(fbBytes), 1, f);
    fwrite(&s.gb,    sizeof(s.gb),    1, f);  // includes CPU regs, WRAM, VRAM, OAM, HRAM
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
// *apu_restored is set true only when a v3 save successfully restored the full
// GBAPU snapshot via gb_audio_restore_state().  When false, the caller must
// call gb_audio_reset_regs() before gb_audio_init().
static bool load_state(GBState& s, bool* apu_restored = nullptr) {
    if (apu_restored) *apu_restored = false;
    if (!s.romPath[0]) return false;

    char statePath[256] = {};
    build_state_path(s.romPath, statePath, sizeof(statePath));

    FILE* f = fopen(statePath, "rb");
    if (!f) return false;

    uint32_t magic = 0, version = 0, ramSz = 0, fbBytes = 0;
    if (fread(&magic,   sizeof(magic),   1, f) != 1 || magic != STATE_MAGIC) { fclose(f); return false; }
    if (fread(&version, sizeof(version), 1, f) != 1 || version < 1u || version > 3u) { fclose(f); return false; }
    if (fread(&ramSz,   sizeof(ramSz),   1, f) != 1) { fclose(f); return false; }
    if (fread(&fbBytes, sizeof(fbBytes), 1, f) != 1) { fclose(f); return false; }

    // Sanity checks
    if (ramSz   != static_cast<uint32_t>(s.cartRamSz))  { fclose(f); return false; }
    if (fbBytes != static_cast<uint32_t>(GB_W * GB_H * sizeof(uint16_t))) { fclose(f); return false; }

    // Read the core struct — this overwrites function pointers; we re-patch below
    if (fread(&s.gb, sizeof(s.gb), 1, f) != 1) { fclose(f); return false; }

    // Re-patch all function pointers that were serialized as garbage
    s.gb.gb_rom_read       = gb_rom_read;
    s.gb.gb_rom_read_16bit = gb_rom_read16;
    s.gb.gb_rom_read_32bit = gb_rom_read32;
    s.gb.gb_cart_ram_read  = gb_cart_ram_read;
    s.gb.gb_cart_ram_write = gb_cart_ram_write;
    s.gb.gb_error          = gb_error;
    s.gb.gb_serial_tx      = nullptr;
    s.gb.gb_serial_rx      = nullptr;
    s.gb.gb_bootrom_read   = nullptr;
    s.gb.direct.priv       = nullptr;
    gb_init_lcd(&s.gb, gb_lcd_draw_line);  // re-sets display.lcd_draw_line

    // Restore cart RAM
    if (s.cartRam && s.cartRamSz) {
        if (fread(s.cartRam, 1, s.cartRamSz, f) != s.cartRamSz) { fclose(f); return false; }
    }

    // Restore framebuffer so the last frame shows instantly
    if (fread(g_gb_fb, 1, fbBytes, f) != fbBytes) { fclose(f); return false; }

    // v3: restore full GBAPU snapshot.
    // This includes ch.enabled, timer, duty_pos, current vol, seq_step, lfsr,
    // hp_ch, and every other computed field.  Without it the audio thread starts
    // with all channels disabled (enabled=false) because apply_reg() never sets
    // that flag (it masks trigger bits), causing silence for the entire session.
    if (version >= 3u) {
        GBAPU apu_snap{};
        if (fread(&apu_snap, sizeof(apu_snap), 1, f) == 1) {
            gb_audio_restore_state(&apu_snap);
            if (apu_restored) *apu_restored = true;
        }
    }

    fclose(f);
    return true;
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
    //   limitedMemory        = 4 MB heap  → max ROM 2 MB
    //   (neither)            = 6 MB heap  → max ROM 2 MB (can't fit 4 MB ROMs)
    //   expandedMemory       = 8 MB heap  → max ROM 4 MB (wallpaper evicted below)
    //   furtherExpandedMemory= >8 MB heap → max ROM 8 MB
    //
    // Notify the user with an actionable message before touching any state.
    static constexpr size_t ROM_2MB = 2u << 20;
    static constexpr size_t ROM_4MB = 4u << 20;
    static constexpr size_t ROM_8MB = 8u << 20;

    if (sz > ROM_2MB && (ult::limitedMemory || !ult::expandedMemory)) {
        // 4 MB heap can't fit a 2 MB ROM; 6 MB heap can't fit a 4 MB ROM.
        const std::string msg = ult::limitedMemory ? "Requires at least 6MB."
                                                   : "Requires at least 8MB.";
        if (tsl::notification) tsl::notification->showNow(ult::NOTIFY_HEADER + msg);
        fclose(f); return false;
    }
    if (sz > ROM_4MB && ult::expandedMemory && !ult::furtherExpandedMemory) {
        if (tsl::notification) tsl::notification->showNow(ult::NOTIFY_HEADER + "Requires at least 10MB.");
        fclose(f); return false;
    }
    const size_t maxRom = ult::furtherExpandedMemory ? ROM_8MB
                        : ult::expandedMemory        ? ROM_4MB
                        : ROM_2MB;
    if (sz > maxRom) { fclose(f); return false; }

    // ── Unload the current ROM BEFORE allocating the new buffer ─────────────
    // Previously the new ROM was malloc'd first, then gb_unload_rom() freed
    // the old one — momentarily holding two large ROMs simultaneously and
    // exhausting the heap when switching between 4 MB games.  Unloading first
    // keeps peak usage to one ROM at a time.
    gb_unload_rom();  // save + free previous ROM (no-op if nothing loaded)

    // ── Wallpaper memory management ──────────────────────────────────────────
    // ROMs larger than 2 MB (GBC titles) cannot safely coexist with the wallpaper
    // buffer on an 8 MB heap unless furtherExpandedMemory is true.
    //
    // draw() and handleInput() run sequentially on one thread: by the time
    // handleInput() calls gb_load_rom(), draw() has fully returned and all
    // render worker threads are joined.  inPlot is guaranteed false — no
    // spin-wait or mutex is required.
    //
    // We set refreshWallpaper=true first so drawWallpaper() skips the blit for
    // the entire game session.  The wallpaper is freed with the swap trick
    // (guaranteed deallocation — shrink_to_fit is non-binding in C++ and newlib
    // may ignore it, leaving the buffer allocated and causing OOM on malloc).
    // reloadWallpaper() in gb_unload_rom() restores the wallpaper and clears
    // refreshWallpaper when the game exits.
    static constexpr size_t WALLPAPER_EVICT_THRESHOLD = 2u << 20;  // 2 MB
    if (sz > WALLPAPER_EVICT_THRESHOLD && !ult::furtherExpandedMemory
            && !ult::wallpaperData.empty()) {
        // Signal drawWallpaper to skip the blit path from now on.
        // draw() and handleInput() run sequentially on one thread — by the time
        // handleInput() fires gb_load_rom(), the current frame's draw() has fully
        // returned and all render worker threads are joined, so inPlot is
        // guaranteed false.  No spin-wait or mutex is needed.
        ult::refreshWallpaper.store(true, std::memory_order_release);

        // Guaranteed deallocation via swap trick.
        // shrink_to_fit() is non-binding in the C++ standard and newlib may
        // ignore it entirely, leaving ~630 KB allocated and causing OOM when
        // we immediately malloc(4 MB) below.  The swap with a default-constructed
        // vector is the only portable way to guarantee the buffer is freed.
        { std::vector<u8> tmp; tmp.swap(ult::wallpaperData); }

        g_wallpaper_evicted = true;
    }

    uint8_t* rom = static_cast<uint8_t*>(malloc(sz));
    if (!rom) { fclose(f); return false; }
    if (fread(rom, 1, sz, f) != sz) { free(rom); fclose(f); return false; }
    fclose(f);

    g_gb.rom     = rom;
    g_gb.romSize = sz;
    strncpy(g_gb.romPath, path, sizeof(g_gb.romPath)-1);
    build_save_path(path, g_gb.savePath, sizeof(g_gb.savePath));

    const enum gb_init_error_e err =
        gb_init(&g_gb.gb,
                gb_rom_read, gb_rom_read16, gb_rom_read32,
                gb_cart_ram_read, gb_cart_ram_write,
                gb_error, nullptr);
    if (err != GB_INIT_NO_ERROR) {
        free(g_gb.rom); g_gb.rom = nullptr;
        g_gb.romSize = 0;
        g_gb.romPath[0] = '\0';
        g_gb.savePath[0] = '\0';
        return false;
    }

    // Detect whether this is a CGB ROM so the renderer uses the right converter.
    // cgbMode is set by gb_init() when it reads byte 0x143 of the ROM header.
#if WALNUT_FULL_GBC_SUPPORT
    g_fb_is_rgb565 = (g_gb.gb.cgb.cgbMode != 0);
#else
    g_fb_is_rgb565 = false;
#endif

    size_t ramSz = 0;
    gb_get_save_size_s(&g_gb.gb, &ramSz);
    // MBC3 cartridges (especially Zelda Oracle / Pokémon Gold/Silver) can have
    // bad ROM headers that declare only 8KB of SRAM when the game actually uses
    // 4 banks × 8KB = 32KB.  If the game writes to banks 1–3 and gets 0xFF back
    // it will enter a corrupted state during the intro save-RAM initialisation.
    // Clamp MBC3 to a minimum of 32KB so all four banks are always available.
    if (g_gb.gb.mbc == 3 && ramSz > 0 && ramSz < 0x8000)
        ramSz = 0x8000;
    if (ramSz) {
        g_gb.cartRam = static_cast<uint8_t*>(calloc(ramSz, 1));
        if (!g_gb.cartRam) {
            // Can't allocate cart RAM — running without it would silently corrupt
            // any game that uses SRAM. Clean up and fail.
            free(g_gb.rom); g_gb.rom = nullptr;
            g_gb.romSize = 0; g_gb.romPath[0] = '\0'; g_gb.savePath[0] = '\0';
            return false;
        }
        g_gb.cartRamSz = ramSz;
        load_save(g_gb);
    }

    gb_init_lcd(&g_gb.gb, gb_lcd_draw_line);

    // Clear the framebuffer BEFORE gb_reset() — peanut_gb fires gb_lcd_draw_line
    // for line 0 during reset itself.  If memset runs after, that first draw is
    // erased and row 0 stays black until the second frame completes.
    memset(g_gb_fb, 0, sizeof(g_gb_fb));

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
    gb_audio_init(&g_gb.gb);

    // If load_state succeeded, gb_reset() must NOT be called — it would wipe
    // the restored CPU state.  If cold-booting, gb_reset() is already called
    // implicitly by gb_init() above, so the commented-out call below stays out.
    //gb_reset(&g_gb.gb);
    (void)resumed;
    g_gb_frame_next_ns = 0;  // anchor clock on first draw()
    g_gb.running = true;
    return true;
}

void gb_unload_rom() {
    if (!g_gb.rom) return;

    g_gb.running = false;
    g_emu_active = false;

    gb_audio_shutdown();   // drain audout queue, free DMA buffers.
                           // Thread writes s_ctrl.snapshot = local before exiting,
                           // so save_state() below captures the full settled APU state.

    // Persist state on every unload, not just overlay exit.
    // This covers the game-switch path (X -> pick different ROM) which previously
    // lost the current game's progress silently.
    save_state(g_gb);

    write_save(g_gb);
    if (g_gb.cartRam) { free(g_gb.cartRam); g_gb.cartRam = nullptr; }
    g_gb.cartRamSz   = 0;
    free(g_gb.rom);
    g_gb.rom         = nullptr;
    g_gb.romSize     = 0;
    g_gb.romPath[0]  = '\0';
    g_gb.savePath[0] = '\0';

    // ── Wallpaper reload ──────────────────────────────────────────────────────
    // Only reload if we actually evicted the wallpaper for this ROM.
    // reloadWallpaper() waits for inPlot=false, repopulates wallpaperData, and
    // clears refreshWallpaper — the full safe sequence.
    if (g_wallpaper_evicted) {
        g_wallpaper_evicted = false;
        ult::reloadWallpaper();
    }
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
    g_gb.gb.direct.joypad = joy;
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
static std::vector<std::string> scan_roms(const char* dir) {
    std::vector<std::string> list;
    DIR* d = opendir(dir);
    if (!d) return list;
    struct dirent* ent;
    while ((ent = readdir(d))) {
        if (!is_gb_rom(ent->d_name)) continue;
        std::string full = dir;
        if (full.back() != '/') full += '/';
        full += ent->d_name;
        list.push_back(std::move(full));
    }
    closedir(d);
    std::sort(list.begin(), list.end());
    return list;
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

        // When the game is running, skip rendering the wallpaper behind the GB
        // screen — render_gb_screen will overwrite it with fully-opaque game
        // pixels anyway.  Instead draw the wallpaper only in the four strips
        // that are actually visible around the viewport.
        //
        // processBMPChunk's row-level scissor check (hasScissor && baseY outside
        // range → continue) skips the entire NEON inner loop for excluded rows,
        // saving all blend work for the ~144,000 pixels the game covers (44% of
        // the 448x720 framebuffer).
        //
        // VP geometry: x=[VP_X, VP_X+VP_W) = [24,424), y=[VP_Y, VP_Y+VP_H)
        if (g_gb.running && g_emu_active) {
            // Top strip: full width, above the game screen
            if constexpr (VP_Y > 0) {
                renderer->enableScissoring(0, 0, FB_W, VP_Y);
                renderer->drawWallpaper();
                renderer->disableScissoring();
            }
            // Bottom strip: full width, below the game screen
            if constexpr (VP_Y + VP_H < FB_H) {
                renderer->enableScissoring(0, VP_Y + VP_H, FB_W, FB_H - (VP_Y + VP_H));
                renderer->drawWallpaper();
                renderer->disableScissoring();
            }
            // Left strip: beside the game screen (vertical band)
            if constexpr (VP_X > 0) {
                renderer->enableScissoring(0, VP_Y, VP_X, VP_H);
                renderer->drawWallpaper();
                renderer->disableScissoring();
            }
            // Right strip: beside the game screen (vertical band)
            if constexpr (VP_X + VP_W < FB_W) {
                renderer->enableScissoring(VP_X + VP_W, VP_Y, FB_W - (VP_X + VP_W), VP_H);
                renderer->drawWallpaper();
                renderer->disableScissoring();
            }
        } else {
            renderer->drawWallpaper();
        }
    
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
        {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            const int64_t now_ns = (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec;

            if (g_gb_frame_next_ns == 0)
                g_gb_frame_next_ns = now_ns;  // anchor on first draw

            if (now_ns >= g_gb_frame_next_ns) {
                gb_run_one_frame();
                gb_audio_submit();
                g_gb_frame_next_ns += GB_RENDER_FRAME_NS;
                if (g_gb_frame_next_ns < now_ns)
                    g_gb_frame_next_ns = now_ns + GB_RENDER_FRAME_NS;

                // ── Freeze detection (solid-colour framebuffer) ────────────────
                // Oracle of Seasons (and similar CGB games) sometimes cold-boot
                // into a freeze: the game turns off the LCD mid-intro, the
                // framebuffer is left showing all-white, and the CPU gets stuck.
                // The user's workaround — exit, re-enter, resume from save state —
                // works because gb_init() resets walnut's internal counters before
                // load_state() restores the CPU registers.  We replicate that exact
                // sequence automatically.
                //
                // Detection: after the framebuffer has produced at least one
                // non-uniform frame (real game content), if it then shows the
                // SAME colour on every pixel for ≥180 consecutive frames (~3 s),
                // we treat that as a freeze and auto-recover.
                //
                // A full 160×144 pixel scan costs ~5 µs — negligible.
                // False-positive risk is near zero: real game frames virtually
                // never sustain a perfectly solid colour for 3 seconds.
                {
                    static bool  s_had_varied   = false; // saw a non-solid frame
                    static int   s_solid_frames = 0;     // consecutive solid frames
                    static char  s_path[256]    = {};    // ROM these counters belong to

                    // Reset counters whenever ROM changes
                    if (strncmp(s_path, g_gb.romPath, sizeof(s_path)) != 0) {
                        strncpy(s_path, g_gb.romPath, sizeof(s_path) - 1);
                        s_had_varied   = false;
                        s_solid_frames = 0;
                    }

                    // Is the entire framebuffer one solid colour?
                    const uint16_t first = g_gb_fb[0];
                    bool all_same = true;
                    for (int i = 1; i < GB_W * GB_H; ++i) {
                        if (g_gb_fb[i] != first) { all_same = false; break; }
                    }

                    if (!all_same) {
                        s_had_varied   = true;  // real content visible — healthy
                        s_solid_frames = 0;
                    } else if (s_had_varied) {
                        // Solid colour after real content → potential freeze
                        if (++s_solid_frames >= 180) {
                            char frozen_path[256];
                            strncpy(frozen_path, g_gb.romPath,
                                    sizeof(frozen_path) - 1);

                            gb_unload_rom();              // saves state, shuts audio
                            if (gb_load_rom(frozen_path)) // gb_init + load_state
                                g_emu_active = true;

                            // ROM path changes → counters auto-reset next frame
                        }
                    }
                    // If !s_had_varied: still in the solid-black pre-game boot —
                    // don't count and don't interfere.
                }
            }
        }

        render_gb_screen(renderer);
        render_gb_border(renderer);

        //const char* sl = strrchr(g_gb.romPath, '/');
        //renderer->drawString(sl ? sl+1 : g_gb.romPath, false,
        //    VP_X+4, VP_Y-14, 12, tsl::defaultTextColor);
        {
            const std::string backStr = "\uE0E2 "+ult::DIVIDER_SYMBOL+" "+ult::BACK;
            static auto [bw, bh] = renderer->getTextDimensions(backStr, false, 15);
            renderer->drawStringWithColoredSections(backStr, false, tsl::s_dividerSpecialChars,
                VP_X + VP_W - bw, VP_Y+VP_H+16+2, 15, tsl::defaultTextColor, tsl::textSeparatorColor);
        }

        // ── Virtual Game GB controls ─────────────────────────────────────────
        // On first frame, measure each glyph's actual rendered dimensions and
        // derive the true visual centre for hit testing.  This accounts for
        // the font's internal bearing/ascender so the hit region tracks the
        // glyph exactly rather than a hand-guessed geometric centre.
        if (!g_btns_measured) {
            static auto [dw, dh] = renderer->getTextDimensions("\uE115", false, DPAD_SIZE);
            g_dpad_hx  = DPAD_DRAW_X  + dw / 2;
            g_dpad_hy  = (DPAD_DRAW_Y - 10) - dh / 2 + 10;

            static auto [aw, ah] = renderer->getTextDimensions("\uE0E0", false, ABTN_SIZE);
            g_abtn_hx  = ABTN_DRAW_X  + aw / 2;
            g_abtn_hy  = ABTN_DRAW_Y  - ah / 2;

            static auto [bw, bh] = renderer->getTextDimensions("\uE0E1", false, BBTN_SIZE);
            g_bbtn_hx  = BBTN_DRAW_X  + bw / 2;
            g_bbtn_hy  = BBTN_DRAW_Y  - bh / 2;

            static auto [sw, sh] = renderer->getTextDimensions("\uE0F1", false, START_SIZE);
            g_start_hx = START_DRAW_X + sw / 2;
            g_start_hy = START_DRAW_Y - sh / 2;

            static auto [selw, selh] = renderer->getTextDimensions("\uE0F2", false, SELECT_SIZE);
            g_select_hx = SELECT_DRAW_X + selw / 2;
            g_select_hy = SELECT_DRAW_Y - selh / 2;

            static auto [divw, divh] = renderer->getTextDimensions(ult::DIVIDER_SYMBOL, false, START_SIZE);
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
            static constexpr s32 LIP     = 2;
            // Vertical bar — shifted 1px down, bottom extended 2px
            renderer->drawRect(DPAD_CX - (ARM_W + LIP*2)/2, DPAD_CY - (FULL + LIP*2)/2 + 3,
                               ARM_W + LIP*2, FULL + LIP*2 + 2, BK);
            // Horizontal bar — shifted 1px lower
            renderer->drawRect(DPAD_CX - (FULL + LIP*2)/2, DPAD_CY - (ARM_H + LIP*2)/2 + 5,
                               FULL + LIP*2, ARM_H + LIP*2, BK);
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
            static const auto [dw, dh] = renderer->getTextDimensions("\uE115", false, DPAD_SIZE);
            const s32 left      = DPAD_DRAW_X;
            const s32 top       = baseline - static_cast<s32>(dh);
            const s32 thirdH    = static_cast<s32>(dh) / 3;
            // 46% wide for the vertical (↑↓) strip, 46% tall for the horizontal (←→) strip
            const s32 stripW    = static_cast<s32>(dw) * 46 / 100;
            const s32 stripH    = static_cast<s32>(dh) * 46 / 100;

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
            renderer->enableScissoring(0, top + (static_cast<s32>(dh) - stripH) / 2 + rowNudge,
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
        renderer->drawString("\uE0F2", false, SELECT_DRAW_X, SELECT_DRAW_Y + 1, SELECT_SIZE, VBTN_COLOR);
        renderer->drawString(ult::DIVIDER_SYMBOL, false,
            FB_W / 2 - g_div_half_w, START_DRAW_Y + 1, START_SIZE, tsl::textSeparatorColor);
        renderer->drawString("\uE0F1", false, START_DRAW_X,  START_DRAW_Y  + 1, START_SIZE,  VBTN_COLOR);

        if (!ult::useRightAlignment)
            renderer->drawRect(447, 0, 448, 720, a(tsl::edgeSeparatorColor));
        else
            renderer->drawRect(0, 0, 1, 720, a(tsl::edgeSeparatorColor));
    }

    virtual void layout(u16, u16, u16, u16) override {
        this->setBoundaries(0, 0, FB_W, FB_H);
    }
};

class RomSelectorGui; // forward declare
// =============================================================================
// GBEmulatorGui — input handled at the Gui level (same pattern as TetrisGui)
// =============================================================================
class GBEmulatorGui : public tsl::Gui {
    bool m_waitForRelease = true;  // ignore input until all buttons are released
    u64 m_prevTouchKeys = 0;       // track previous touch state
public:
    virtual tsl::elm::Element* createUI() override {
        g_emu_active = true;
        m_waitForRelease = true;
        // No bottom bar is drawn in-game. Tell the framework there are no
        // clickable items so footer-zone touches don't highlight the select
        // button or fire its rumble / simulation callbacks.
        ult::noClickableItems.store(true, std::memory_order_release);
        return new GBScreenElement();
    }

    virtual void update() override {}

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
            const int tx = static_cast<int>(touchPos.x);
            const int ty = static_cast<int>(touchPos.y);

            // D-pad — split into 4 arms from the measured glyph centre
            {
                const int dx = tx - g_dpad_hx;
                const int dy = ty - g_dpad_hy;
                if (dx*dx + dy*dy <= DPAD_R * DPAD_R) {
                    if (std::abs(dx) >= std::abs(dy)) {
                        g_touch_keys |= (dx >= 0) ? KEY_RIGHT : KEY_LEFT;
                    }
                    else {
                        g_touch_keys |= (dy >= 0) ? KEY_DOWN : KEY_UP;
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
        if (newTouchPresses) {
            triggerRumbleClick.store(true, std::memory_order_release);
        }
        m_prevTouchKeys = g_touch_keys;

        // X → back to ROM picker.  Fully unload the ROM (saves state, frees
        // ROM buffer + cart RAM, shuts down audio) so the heap is clean for
        // the next game.  The save state written here means re-entering the
        // same game resumes exactly where the player left off via load_state.
        if (keysDown & KEY_X) {
            g_touch_keys  = 0;
            // Restore normal clickable-item state for the ROM selector UI.
            ult::noClickableItems.store(false, std::memory_order_release);
            gb_unload_rom();  // sets running=false, emu_active=false, frees all
            triggerExitFeedback();
            tsl::swapTo<RomSelectorGui>();
            return true;
        }

        // Pass physical held + fresh presses + virtual touch keys to the GB core
        gb_set_input(keysHeld | keysDown | g_touch_keys);

        // Trigger rumble on ANY new button press
        if (keysDown & (KEY_A | KEY_B | KEY_PLUS | KEY_MINUS |
                        KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT)) {
            triggerRumbleClick.store(true, std::memory_order_release);
        }

        return true;  // consume all input while in-game
    }
};

// =============================================================================
// RomSelectorGui
// =============================================================================

// OverlayFrame subclass that replaces the static title with the animated
// "UltraGB" wave effect.  We pass "" as the title so OverlayFrame draws
// nothing in the header region, then paint the animated version ourselves.
class AnimatedOverlayFrame : public tsl::elm::OverlayFrame {
public:
    AnimatedOverlayFrame(const std::string& /*title*/, const std::string& subtitle)
        : tsl::elm::OverlayFrame("", "") {}

    virtual void draw(tsl::gfx::Renderer* renderer) override {
        tsl::elm::OverlayFrame::draw(renderer);
        draw_ultraboy_title(renderer, 20, 67, 50);
    }
};

// Returns true if the ROM at path is within the playable size for the current
// memory tier — same thresholds used in gb_load_rom().
static bool rom_is_playable(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return false;
    const size_t sz = static_cast<size_t>(st.st_size);
    if (ult::furtherExpandedMemory) return sz <= (8u << 20);
    if (ult::expandedMemory)        return sz <= (4u << 20);
    return sz <= (2u << 20);  // 4 MB or 6 MB heap
}

class RomSelectorGui : public tsl::Gui {
    VolumeTrackBar* m_vol_slider  = nullptr;
    u8              m_vol         = 100;
    u8              m_vol_backup  = 100;
public:
    virtual tsl::elm::Element* createUI() override {
        g_emu_active = false;

        // Initialize the audio service
        if (ult::useSoundEffects && !ult::limitedMemory) {
            ult::Audio::initialize();
        }
        
        auto* list  = new tsl::elm::List();

        auto* pathHeader = new tsl::elm::CategoryHeader("Game Boy ROMs");
        list->addItem(pathHeader);

        const std::vector<std::string> roms = scan_roms(g_rom_dir);

        if (roms.empty()) {
            static char msg[640];
            snprintf(msg, sizeof(msg),
                "No .gb or .gbc files found in:\n%s\n\n"
                "Edit: sdmc:/config/ultragb/config.ini", g_rom_dir);
            auto* empty = new tsl::elm::CustomDrawer(
                [](tsl::gfx::Renderer* r, s32 x, s32 y, s32, s32) {
                    r->drawString(msg, false, x+16, y+30, 16,
                                  tsl::Color{0x8,0x8,0x8,0xF});
                });
            list->addItem(empty, 200);
        } else {
            std::string jumpLabel;  // label of the item to scroll to on first display
            for (const auto& path : roms) {
                const char* sl = strrchr(path.c_str(), '/');
                std::string label = sl ? std::string(sl+1) : path;

                // Show in-progress symbol if:
                //   a) ROM is currently loaded in this session (returning from game via X), OR
                //   b) No ROM is loaded yet but basename matches last_rom in config (cold launch)
                const bool isLive = g_gb.rom && g_gb.romPath[0] &&
                                    strncmp(g_gb.romPath, path.c_str(), sizeof(g_gb.romPath)) == 0;
                const bool isLast = !isLive && !g_gb.rom && g_last_rom_path[0] &&
                                    strcmp(label.c_str(), g_last_rom_path) == 0;
                // Only show in-progress state if the ROM is actually playable on this
                // memory tier — a saved last_rom that requires more RAM than available
                // can't be resumed, so showing the symbol would be misleading.
                const bool inProgress = (isLive || isLast) && rom_is_playable(path);

                if (inProgress && jumpLabel.empty())
                    jumpLabel = label;

                auto* item = new tsl::elm::MiniListItem(label,
                                                    inProgress ? ult::INPROGRESS_SYMBOL : "");
                if (!rom_is_playable(path))
                    item->setTextColor(tsl::warningTextColor);
                // Only isLive (ROM already in memory) can use the fast-resume path.
                // isLast only affects the symbol/jump — the ROM still needs loading.
                item->setClickListener([path, isLive](u64 keys) -> bool {
                    if (!(keys & KEY_A)) return false;

                    // Reject unplayable ROMs before touching any audio state.
                    // gb_load_rom() will fire the notification and return false.
                    if (!isLive && !rom_is_playable(path)) {
                        gb_load_rom(path.c_str());
                        return false;
                    }

                    // Audio::exit() shuts down UI sound effects before the game
                    // takes over audio.  Must come before gb_load_rom() / gb_audio_init()
                    // since both start GB audio internally.
                    // Audio::initialize() is called by RomSelectorGui::createUI() when
                    // the user returns to the selector, restoring UI sound effects.
                    if (ult::useSoundEffects && !ult::limitedMemory)
                        ult::Audio::exit();

                    if (isLive) {
                        gb_audio_init(&g_gb.gb);
                        g_gb_frame_next_ns = 0;
                        g_gb.running = true;
                    } else {
                        if (!gb_load_rom(path.c_str())) return false;
                    }
                    save_last_rom(path.c_str());
                    tsl::swapTo<GBEmulatorGui>();
                    return true;
                });
                list->addItem(item);
            }

            // Scroll to and centre the in-progress item immediately — works both on
            // cold launch (last_rom from config) and when returning from a game (X).
            if (!jumpLabel.empty())
                list->jumpToItem(jumpLabel, "", true);
        }

        // ── Volume ───────────────────────────────────────────────────────────
        list->addItem(new tsl::elm::CategoryHeader(
            "Volume " + ult::DIVIDER_SYMBOL + "  Toggle Mute"));

        m_vol        = gb_audio_get_volume();
        m_vol_backup = (m_vol > 0) ? m_vol : static_cast<u8>(100);

        auto* vol_slider = new VolumeTrackBar("\uE13C", false, false, true, "Game Boy", "%", false);
        vol_slider->setProgress(m_vol);
        vol_slider->setValueChangedListener([this](u8 value) {
            m_vol = value;
            gb_audio_set_volume(value);
            ult::setIniFileValue(std::string(CONFIG_FILE), "config", "volume",
                                 std::to_string(value), "");
        });
        m_vol_slider = vol_slider;
        vol_slider->setIconTapCallback([this]() {
            if (m_vol > 0) {
                m_vol_backup = m_vol;
                m_vol = 0;
            } else {
                m_vol = (m_vol_backup > 0) ? m_vol_backup : static_cast<u8>(100);
            }
            m_vol_slider->setProgress(m_vol);
            gb_audio_set_volume(m_vol);
            ult::setIniFileValue(std::string(CONFIG_FILE), "config", "volume",
                                 std::to_string(m_vol), "");
        });
        list->addItem(vol_slider);

        auto* frame = new AnimatedOverlayFrame("UltraGB", APP_VERSION);
        frame->m_showWidget = true;
        frame->setContent(list);
        return frame;
    }

    // B closes the overlay. overrideBackButton=true means the framework will
    // never auto-call goBack() for us — we must handle KEY_B explicitly here.
    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)keysHeld; (void)touchPos; (void)leftJoy; (void)rightJoy;

        // Y — mute/unmute toggle (mirrors icon-tap callback logic).
        if (keysDown & KEY_Y) {
            if (m_vol_slider) {
                if (m_vol > 0) {
                    m_vol_backup = m_vol;
                    m_vol = 0;
                } else {
                    m_vol = (m_vol_backup > 0) ? m_vol_backup : static_cast<u8>(100);
                }
                m_vol_slider->setProgress(m_vol);
                gb_audio_set_volume(m_vol);
                ult::setIniFileValue(std::string(CONFIG_FILE), "config", "volume",
                                     std::to_string(m_vol), "");
                triggerRumbleClick.store(true, std::memory_order_release);
            }
            return true;
        }

        if (keysDown & KEY_B) {
            tsl::Overlay::get()->close();
            return true;
        }
        return false;
    }
};

// =============================================================================
// Overlay
// =============================================================================
class Overlay : public tsl::Overlay {
public:
    virtual void initServices() override {
        tsl::overrideBackButton = true;
        ult::createDirectory(CONFIG_DIR);
        ult::createDirectory(SAVE_DIR);
        ult::createDirectory(STATE_DIR);
        load_config();
        write_default_config_if_missing();
        ult::createDirectory(g_rom_dir);
    }

    virtual void exitServices() override {
        gb_unload_rom();   // save state, write SRAM, free memory, shut down audio
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
        return initially<RomSelectorGui>();
    }
};

int main(int argc, char* argv[]) {
    return tsl::loop<Overlay, tsl::impl::LaunchFlags::None>(argc, argv);
}