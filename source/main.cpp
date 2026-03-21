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
static char g_rom_dir[512] = "sdmc:/roms/gb/";

static void load_config() {
    FILE* f = fopen(CONFIG_FILE, "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof(line), f)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';
        const size_t KLEN = 8; // "rom_dir="
        if (strncmp(line, "rom_dir=", KLEN) == 0) {
            const char* val = line + KLEN;
            const size_t vlen = strlen(val);
            if (vlen > 0 && vlen < sizeof(g_rom_dir) - 2) {
                strncpy(g_rom_dir, val, sizeof(g_rom_dir) - 2);
                g_rom_dir[vlen] = '\0';
                if (g_rom_dir[vlen-1] != '/') {
                    g_rom_dir[vlen]   = '/';
                    g_rom_dir[vlen+1] = '\0';
                }
            }
        }
    }
    fclose(f);
}

static void write_default_config_if_missing() {
    FILE* t = fopen(CONFIG_FILE, "r");
    if (t) { fclose(t); return; }
    FILE* f = fopen(CONFIG_FILE, "w");
    if (!f) return;
    fprintf(f, "# Ultra-Boy config\n# Set rom_dir to your .gb/.gbc folder\nrom_dir=%s\n", g_rom_dir);
    fclose(f);
}

// =============================================================================
// Global emulator state
// =============================================================================
GBState  g_gb;
uint16_t g_gb_fb[GB_W * GB_H] = {};
static bool g_emu_active = false;

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
// gb_load_rom — if same ROM is already in memory, just resume
// =============================================================================
bool gb_load_rom(const char* path) {
    if (g_gb.rom && strncmp(g_gb.romPath, path, sizeof(g_gb.romPath)) == 0) {
        g_gb.running = true;
        return true;
    }

    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    const size_t sz = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    const size_t maxRom = ult::limitedMemory ? (2u<<20) : (8u<<20);
    if (!sz || sz > maxRom) { fclose(f); return false; }
    uint8_t* rom = static_cast<uint8_t*>(malloc(sz));
    if (!rom) { fclose(f); return false; }
    if (fread(rom, 1, sz, f) != sz) { free(rom); fclose(f); return false; }
    fclose(f);

    gb_unload_rom();  // save + free any previous ROM

    g_gb.rom     = rom;
    g_gb.romSize = sz;
    strncpy(g_gb.romPath, path, sizeof(g_gb.romPath)-1);
    build_save_path(path, g_gb.savePath, sizeof(g_gb.savePath));

    const enum gb_init_error_e err =
        gb_init(&g_gb.gb, gb_rom_read, gb_cart_ram_read, gb_cart_ram_write, gb_error, nullptr);
    if (err != GB_INIT_NO_ERROR) { free(g_gb.rom); g_gb.rom = nullptr; return false; }

    size_t ramSz = 0;
    gb_get_save_size_s(&g_gb.gb, &ramSz);
    if (ramSz) {
        g_gb.cartRam   = static_cast<uint8_t*>(calloc(ramSz, 1));
        g_gb.cartRamSz = ramSz;
        load_save(g_gb);
    }

    gb_init_lcd(&g_gb.gb, gb_lcd_draw_line);

    // ── Init audio BEFORE gb_reset() so the APU callback is live from frame 1 ──
    gb_audio_init(&g_gb.gb);

    gb_reset(&g_gb.gb);
    memset(g_gb_fb, 0, sizeof(g_gb_fb));
    g_gb.running = true;
    return true;
}

void gb_unload_rom() {
    if (!g_gb.rom) return;

    g_gb.running = false;
    g_emu_active = false;

    gb_audio_shutdown();   // drain audout queue, free DMA buffers

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
        renderer->fillScreen(renderer->a(tsl::defaultBackgroundColor));
        renderer->drawWallpaper();
    
        //renderer->drawRect(15, tsl::cfg::FramebufferHeight - 73, tsl::cfg::FramebufferWidth - 30, 1, renderer->a(tsl::bottomSeparatorColor));
        
        // Use cached or current data for rendering
        const std::string& renderTitle = "UltraGB";
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
            renderer->drawString("Paused", false,
                VP_X + VP_W/2 - 24, VP_Y + VP_H/2, 20,
                tsl::defaultTextColor);
            return;
        }

        // Run one emulated frame (also triggers APU callback → ring buffer)
        gb_run_one_frame();

        // Submit any accumulated audio samples to audout
        gb_audio_submit();

        render_gb_screen(renderer);
        render_gb_border(renderer);

        const char* sl = strrchr(g_gb.romPath, '/');
        renderer->drawString(sl ? sl+1 : g_gb.romPath, false,
            VP_X+4, VP_Y-14, 12, tsl::defaultTextColor);
        renderer->drawString("X: ROM list", false,
            VP_X+4, VP_Y+VP_H+16, 12, tsl::defaultTextColor);

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
public:
    virtual tsl::elm::Element* createUI() override {
        g_emu_active = true;
        m_waitForRelease = true;
        return new GBScreenElement();
    }

    virtual void update() override {}

    virtual bool handleInput(u64 keysDown, u64 keysHeld,
                             const HidTouchState& touchPos,
                             HidAnalogStickState leftJoy,
                             HidAnalogStickState rightJoy) override {
        (void)touchPos; (void)leftJoy; (void)rightJoy;

        // Block all input until the buttons that launched us are fully released.
        // keysHeld stays non-zero as long as any button remains physically held,
        // so we wait for a clean frame before passing anything to the GB core.
        if (m_waitForRelease) {
            if (keysHeld) return true;
            m_waitForRelease = false;
        }

        // X → pause + back to ROM picker, state preserved
        if (keysDown & KEY_X) {
            g_gb.running = false;
            g_emu_active = false;
            gb_audio_pause();
            triggerExitFeedback();
            tsl::swapTo<RomSelectorGui>();
            return true;
        }

        // Pass all held keys + fresh presses to the GB core
        gb_set_input(keysHeld | keysDown);

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
                    if (inProgress) {
                        g_gb.running = true;
                        gb_audio_resume();
                    } else {
                        if (!gb_load_rom(path.c_str())) return false;
                    }
                    tsl::swapTo<GBEmulatorGui>();
                    return true;
                });
                list->addItem(item);
            }
        }

        auto* frame = new tsl::elm::OverlayFrame("UltraGB", APP_VERSION);
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
        load_config();
        write_default_config_if_missing();
        ult::createDirectory(g_rom_dir);
    }

    virtual void exitServices() override {
        gb_unload_rom();  // gb_audio_shutdown is called inside here
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