/********************************************************************************
 * File: main.cpp
 * Description:
 *   Game Boy / Game Boy Color emulator overlay for Nintendo Switch.
 *
 *   Config: sdmc:/config/gbemu/config.ini  (rom_dir=sdmc:/roms/gb/)
 *   Saves:  sdmc:/config/gbemu/saves/
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

#include <dirent.h>
#include <sys/stat.h>
#include <vector>
#include <string>
#include <algorithm>

using namespace ult;

// =============================================================================
// Paths / config
// =============================================================================
static constexpr const char* CONFIG_DIR  = "sdmc:/config/gbemu/";
static constexpr const char* CONFIG_FILE = "sdmc:/config/gbemu/config.ini";
static constexpr const char* SAVE_DIR    = "sdmc:/config/gbemu/saves/";
static constexpr const char* STATE_DIR   = "sdmc:/config/gbemu/states/";
static char g_rom_dir[512] = "sdmc:/roms/gb/";

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
                             "true = classic DMG greyscale, false = GBC color");
}

// =============================================================================
// Global emulator state
// =============================================================================
GBState  g_gb;
uint16_t g_gb_fb[GB_W * GB_H] = {};
static bool g_emu_active = false;
bool g_original_palette = true;   // true = DMG greyscale, false = GBC green tint
bool g_fb_is_rgb565     = false;  // true when CGB mode; set in gb_load_rom

// =============================================================================
// Virtual on-screen button layout (Game Boy skin)
// All coordinates are in overlay pixels (448 × 720).
// The screen bottom sits at VP_Y + VP_H = 140 + 360 = 500, leaving 220 px of
// controller area below it.  Layout mirrors a real DMG Game Boy:
//   D-pad   — lower-left
//   B / A   — lower-right, staggered (B lower-left of A, matching real GB)
//   Start   — centre, near bottom
//
// Draw positions (top-left x, baseline y) are compile-time constants.
// Hit-test centres are derived at runtime from getTextDimensions() so they
// track the glyph's actual visual centre regardless of font metrics.
// =============================================================================

// D-pad — \uE110
static constexpr int DPAD_DRAW_X = 29;    // left edge  = DPAD_CX - DPAD_SIZE/2
static constexpr int DPAD_DRAW_Y = 634+4;   // baseline   = DPAD_CY + DPAD_SIZE/2
static constexpr int DPAD_SIZE   = 112;
static constexpr int DPAD_R      = 54;    // hit radius for each directional arm

// A button — \uE0E0
static constexpr int ABTN_DRAW_X = 344;   // left edge
static constexpr int ABTN_DRAW_Y = 594;   // baseline
static constexpr int ABTN_SIZE   = 72;
static constexpr int ABTN_R      = 38;

// B button — \uE0E1
static constexpr int BBTN_DRAW_X = 289;   // left edge
static constexpr int BBTN_DRAW_Y = 633;   // baseline
static constexpr int BBTN_SIZE   = 58;
static constexpr int BBTN_R      = 30;

// Start — \uE0F1
// IMPORTANT: must stay above FOOTER_Y (FB_H - 73 = 647).  Any touch whose
// *initial* y is >= FOOTER_Y is treated by the framework as a footer button
// press (back / select), firing rumble and injecting KEY_B / KEY_A even when
// the bottom bar is not rendered.
static constexpr int FOOTER_Y    = FB_H;   // 647
static constexpr int START_DRAW_X = 209;
static constexpr int START_DRAW_Y = 626 + 44;         // baseline — keep below FOOTER_Y
static constexpr int START_SIZE   = 30;
static constexpr int START_R      = 24;

// Solid grey — fully opaque so glyphs read clearly against the wallpaper
static constexpr tsl::Color VBTN_COLOR{0xB, 0xB, 0xB, 0xF};

// Hit-test centres, populated on first draw from getTextDimensions().
// Initialised to geometric centres so they work even before measurement.
static int g_dpad_hx  = DPAD_DRAW_X  + DPAD_SIZE  / 2;
static int g_dpad_hy  = DPAD_DRAW_Y  - DPAD_SIZE  / 2;
static int g_abtn_hx  = ABTN_DRAW_X  + ABTN_SIZE  / 2;
static int g_abtn_hy  = ABTN_DRAW_Y  - ABTN_SIZE  / 2;
static int g_bbtn_hx  = BBTN_DRAW_X  + BBTN_SIZE  / 2;
static int g_bbtn_hy  = BBTN_DRAW_Y  - BBTN_SIZE  / 2;
static int g_start_hx = START_DRAW_X + START_SIZE  / 2;
static int g_start_hy = START_DRAW_Y - START_SIZE  / 2;
static bool g_btns_measured = false;

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

    char statePath[512] = {};
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

    char statePath[512] = {};
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
    const size_t maxRom = ult::limitedMemory ? (2u<<20) : (8u<<20);
    if (!sz || sz > maxRom) { fclose(f); return false; }

    // ── Unload the current ROM BEFORE allocating the new buffer ─────────────
    // Previously the new ROM was malloc'd first, then gb_unload_rom() freed
    // the old one — momentarily holding two large ROMs simultaneously and
    // exhausting the heap when switching between 4 MB games.  Unloading first
    // keeps peak usage to one ROM at a time.
    gb_unload_rom();  // save + free previous ROM (no-op if nothing loaded)

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
    if (err != GB_INIT_NO_ERROR) { free(g_gb.rom); g_gb.rom = nullptr; return false; }

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
        g_gb.cartRam   = static_cast<uint8_t*>(calloc(ramSz, 1));
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
        renderer->drawWallpaper();
    
        //renderer->drawRect(15, tsl::cfg::FramebufferHeight - 73, tsl::cfg::FramebufferWidth - 30, 1, renderer->a(tsl::bottomSeparatorColor));
        
        // Use cached or current data for rendering
        const std::string& renderTitle = "UltraGBC";
        const std::string& renderSubtitle = APP_VERSION;
        
        y = 50;
        renderer->drawString(renderTitle, false, 20, 50, 32, tsl::defaultOverlayColor);
        renderer->drawString(renderSubtitle, false, 20, y+2+23, 15, tsl::bannerVersionTextColor);

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
                    static char  s_path[512]    = {};    // ROM these counters belong to

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
                            char frozen_path[512];
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
        renderer->drawString("\uE0E2 "+ult::DIVIDER_SYMBOL+" "+ult::BACK, false,
            VP_X+4, VP_Y+VP_H+16, 14, tsl::defaultTextColor);

        // ── Virtual Game Boy controls ─────────────────────────────────────────
        // On first frame, measure each glyph's actual rendered dimensions and
        // derive the true visual centre for hit testing.  This accounts for
        // the font's internal bearing/ascender so the hit region tracks the
        // glyph exactly rather than a hand-guessed geometric centre.
        if (!g_btns_measured) {
            auto [dw, dh] = renderer->getTextDimensions("\uE115", false, DPAD_SIZE);
            g_dpad_hx  = DPAD_DRAW_X  + dw / 2;
            g_dpad_hy  = (DPAD_DRAW_Y - 10) - dh / 2 + 10;

            auto [aw, ah] = renderer->getTextDimensions("\uE0E0", false, ABTN_SIZE);
            g_abtn_hx  = ABTN_DRAW_X  + aw / 2;
            g_abtn_hy  = ABTN_DRAW_Y  - ah / 2;

            auto [bw, bh] = renderer->getTextDimensions("\uE0E1", false, BBTN_SIZE);
            g_bbtn_hx  = BBTN_DRAW_X  + bw / 2;
            g_bbtn_hy  = BBTN_DRAW_Y  - bh / 2;

            auto [sw, sh] = renderer->getTextDimensions("\uE0F1", false, START_SIZE);
            g_start_hx = START_DRAW_X + sw / 2;
            g_start_hy = START_DRAW_Y - sh / 2;

            g_btns_measured = true;
        }

        // ── D-pad composite — all four arrow directions ───────────────────────
        // Think of each glyph as a 3×3 grid:
        //   E115 (↑↓): arrows live in the center COLUMN (col 1 of 0-2)
        //   E116 (←→): arrows live in the center ROW    (row 1 of 0-2)
        // Scissor each to only its relevant strip; they share the center cell.
        {
            const s32 baseline  = DPAD_DRAW_Y - 10;
            const auto [dw, dh] = renderer->getTextDimensions("\uE115", false, DPAD_SIZE);
            const s32 left      = DPAD_DRAW_X;
            const s32 top       = baseline - static_cast<s32>(dh);
            const s32 thirdW    = static_cast<s32>(dw) / 3;
            const s32 thirdH    = static_cast<s32>(dh) / 3;

            // E115: center column — x constrained to middle third (+1px each side).
            // Y is intentionally over-sized (DPAD_SIZE*2) so the scissor never
            // clips the top or bottom of the ↑↓ arrows regardless of descenders.
            renderer->enableScissoring(left + thirdW - 1, top - DPAD_SIZE / 2,
                                       thirdW + 2, DPAD_SIZE * 2);
            renderer->drawString("\uE115", false, left, baseline, DPAD_SIZE, VBTN_COLOR);
            renderer->disableScissoring();

            // E116: center row — widened by 2px (1px each side), nudged down 10px total
            renderer->enableScissoring(left, top + thirdH + 10,
                                       static_cast<s32>(dw), thirdH + 2);
            renderer->drawString("\uE116", false, left, baseline, DPAD_SIZE, VBTN_COLOR);
            renderer->disableScissoring();
        }
        renderer->drawString("\uE0E0", false, ABTN_DRAW_X,  ABTN_DRAW_Y,  ABTN_SIZE,  VBTN_COLOR);
        renderer->drawString("\uE0E1", false, BBTN_DRAW_X,  BBTN_DRAW_Y,  BBTN_SIZE,  VBTN_COLOR);
        renderer->drawString("\uE0F1", false, START_DRAW_X, START_DRAW_Y, START_SIZE, VBTN_COLOR);

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

            // Start
            {
                const int dx = tx - g_start_hx, dy = ty - g_start_hy;
                if (dx*dx + dy*dy <= START_R * START_R) {
                    g_touch_keys |= KEY_PLUS;
                }
            }
        }

        // Trigger rumble ONLY on new touch presses (not holds)
        u64 newTouchPresses = g_touch_keys & ~m_prevTouchKeys;
        if (newTouchPresses) {
            triggerRumbleClick.store(true, std::memory_order_release);
        }
        m_prevTouchKeys = g_touch_keys;

        // X → back to ROM picker.  Shut audio down completely so the audout
        // session is released and ultrahand / other overlays get clean access.
        // gb_audio_init() will restart it fresh when a game is re-entered.
        if (keysDown & KEY_X) {
            g_touch_keys  = 0;
            g_gb.running  = false;
            g_emu_active  = false;
            // Restore normal clickable-item state for the ROM selector UI.
            ult::noClickableItems.store(false, std::memory_order_release);
            gb_audio_shutdown();
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
class RomSelectorGui : public tsl::Gui {
public:
    virtual tsl::elm::Element* createUI() override {
        g_emu_active = false;

        // Initialize the audio service
        if (ult::useSoundEffects && !ult::limitedMemory) {
            ult::Audio::initialize();
        }
        
        auto* list  = new tsl::elm::List();

        auto* pathHeader = new tsl::elm::CategoryHeader(g_rom_dir);
        list->addItem(pathHeader);

        const std::vector<std::string> roms = scan_roms(g_rom_dir);

        if (roms.empty()) {
            static char msg[640];
            snprintf(msg, sizeof(msg),
                "No .gb or .gbc files found in:\n%s\n\n"
                "Edit: sdmc:/config/gbemu/config.ini", g_rom_dir);
            auto* empty = new tsl::elm::CustomDrawer(
                [](tsl::gfx::Renderer* r, s32 x, s32 y, s32, s32) {
                    r->drawString(msg, false, x+16, y+30, 16,
                                  tsl::Color{0x8,0x8,0x8,0xF});
                });
            list->addItem(empty, 200);
        } else {
            for (const auto& path : roms) {
                const char* sl = strrchr(path.c_str(), '/');
                std::string label = sl ? std::string(sl+1) : path;

                const bool inProgress = g_gb.rom && g_gb.romPath[0] &&
                                        strncmp(g_gb.romPath, path.c_str(), sizeof(g_gb.romPath)) == 0;

                auto* item = new tsl::elm::ListItem(label,
                                                    inProgress ? ult::INPROGRESS_SYMBOL : "");
                item->setClickListener([path, inProgress](u64 keys) -> bool {
                    if (!(keys & KEY_A)) return false;

                    if (ult::useSoundEffects && !ult::limitedMemory) {
                        ult::Audio::exit();
                    }
                    if (inProgress) {
                        gb_audio_init(&g_gb.gb);
                        g_gb_frame_next_ns = 0;
                        g_gb.running = true;
                    } else {
                        if (!gb_load_rom(path.c_str())) return false;
                    }
                    tsl::swapTo<GBEmulatorGui>();
                    return true;
                });
                list->addItem(item);
            }
        }

        auto* frame = new tsl::elm::OverlayFrame("UltraGBC", APP_VERSION);
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