/********************************************************************************
 * File: sgb_core.h
 * Description:
 *   Super Game Boy 2 colour-packet decoder.
 *
 *   Intercepts FF00 (joypad) writes to extract PAL01/PAL23/PAL03/PAL12
 *   commands sent by SGB-enhanced games, converting the decoded RGB555
 *   palette directly into g_dmg_flat_pal (the branchless draw-line table).
 *
 * Protocol (verified against SGB2 boot ROM disassembly):
 *   Bit encoding over FF00 writes (upper nibble only, lower nibble = input):
 *     0x00  reset / start-of-packet pulse
 *     0x10  bit = 1  (P14 low: direction row selected)
 *     0x20  bit = 0  (P15 low: button row selected)
 *     0x30  idle / strobe between bits (ignored)
 *
 *   Each packet = 128 bits (16 bytes), transmitted LSB-first.
 *   Byte 0 format: bits[7:3] = command, bits[2:0] = packet count (usually 1).
 *
 *   PAL commands (only these are decoded; all others are silently ignored):
 *     PAL01 cmd=0: pal0 + pal1   byte0 = (0<<3)|1 = 0x01
 *     PAL23 cmd=1: pal2 + pal3   byte0 = (1<<3)|1 = 0x09
 *     PAL03 cmd=2: pal0 + pal3   byte0 = (2<<3)|1 = 0x11
 *     PAL12 cmd=3: pal1 + pal2   byte0 = (3<<3)|1 = 0x19
 *
 *   Packet payload (bytes 1-14):
 *     Bytes  1-2:  Color 0 — shared by palA and palB   (RGB555 LE)
 *     Bytes  3-4:  palA Color 1
 *     Bytes  5-6:  palA Color 2
 *     Bytes  7-8:  palA Color 3
 *     Bytes  9-10: palB Color 1
 *     Bytes 11-12: palB Color 2
 *     Bytes 13-14: palB Color 3
 *     Byte  15:    unused
 *
 * Mapping to DMG sub-palettes (ATTR commands not emulated):
 *   BG   → SGB palette 0  (primary game graphics, set by every SGB-aware title)
 *   OBJ0 → SGB palette 1  (sprites)
 *   OBJ1 → SGB palette 1  (fallback — same as OBJ0 since ATTR_BLK is ignored)
 *
 * Usage:
 *   1. #include "sgb_core.h" in gb_core.h BEFORE the walnut_cgb.h include block.
 *   2. Call sgb_observe_joyp(val) from the FF00 write handler in walnut_cgb.h,
 *      gated on (gb->hram_io[IO_BOOT] != 0) so boot-ROM init packets are skipped.
 *   3. Call sgb_init() in gb_load_rom() after gb_select_dmg_palette().
 *   4. Call sgb_apply_if_dirty() in the per-frame loop after gb_run_one_frame().
 *
 * Dependencies: none beyond stdint / string / g_dmg_flat_pal (forward-declared).
 ********************************************************************************/

#pragma once
#include <stdint.h>
#include <string.h>

// Forward declaration: defined in main.cpp, declared in gb_core.h.
// sgb_core.h is included before gb_core.h defines it, so we declare it here.
extern uint16_t g_dmg_flat_pal[64];

// ── SGB palette storage ───────────────────────────────────────────────────────
// Four SGB sub-palettes, each with four RGB555 colours (R=bits4:0, G=bits9:5,
// B=bits14:10) — same encoding as GBC BGRx555 / GBC_TITLE_TABLE entries.
static uint16_t s_sgb_pal[4][4]  = {};
static bool     s_sgb_dirty      = false;

// ── Packet decoder state ──────────────────────────────────────────────────────
// Tiny two-state FSM: IDLE waits for a reset pulse; RECV accumulates bits.
enum class SgbDecState : uint8_t { IDLE, RECV };
static SgbDecState s_sgb_dec     = SgbDecState::IDLE;
static uint8_t     s_sgb_buf[16] = {};   // 128-bit packet buffer
static int         s_sgb_bits    = 0;    // bits received so far (0..127)

// ── sgb_init ─────────────────────────────────────────────────────────────────
// Reset decoder + palette. Call once when a ROM is loaded.
// Leaves g_dmg_flat_pal as-is — gb_select_dmg_palette() already populated it
// with the best static approximation; sgb_apply_if_dirty() will overwrite once
// the game sends real palette commands.
static inline void sgb_init() {
    s_sgb_dec   = SgbDecState::IDLE;
    s_sgb_bits  = 0;
    memset(s_sgb_buf, 0, sizeof(s_sgb_buf));
    memset(s_sgb_pal, 0, sizeof(s_sgb_pal));
    s_sgb_dirty = false;
}

// ── sgb_process_packet ───────────────────────────────────────────────────────
// Called when 128 bits have been received. Decodes PAL commands and stores
// the resulting colours in s_sgb_pal[]. All other command codes are ignored.
static inline void sgb_process_packet() {
    // Byte 0 format: bits[7:3] = command, bits[2:0] = packet count
    const uint8_t cmd = s_sgb_buf[0] >> 3;
    if (cmd > 3u) return;  // not a PAL command — discard

    // Determine which two SGB palettes this command targets.
    uint8_t palA, palB;
    switch (cmd) {
        case 0: palA = 0; palB = 1; break;   // PAL01
        case 1: palA = 2; palB = 3; break;   // PAL23
        case 2: palA = 0; palB = 3; break;   // PAL03
        default: palA = 1; palB = 2; break;  // PAL12 (cmd == 3)
    }

    // Color 0 is shared across both palettes (bytes 1-2, RGB555 LE).
    const uint16_t col0 = (uint16_t)s_sgb_buf[1]
                        | ((uint16_t)s_sgb_buf[2] << 8);
    s_sgb_pal[palA][0] = col0;
    s_sgb_pal[palB][0] = col0;

    // palA colors 1-3 (bytes 3-4, 5-6, 7-8).
    for (int i = 1; i < 4; ++i) {
        const int base = 1 + i * 2;   // i=1→3, i=2→5, i=3→7
        s_sgb_pal[palA][i] = (uint16_t)s_sgb_buf[base]
                            | ((uint16_t)s_sgb_buf[base + 1] << 8);
    }

    // palB colors 1-3 (bytes 9-10, 11-12, 13-14).
    for (int i = 1; i < 4; ++i) {
        const int base = 7 + i * 2;   // i=1→9, i=2→11, i=3→13
        s_sgb_pal[palB][i] = (uint16_t)s_sgb_buf[base]
                            | ((uint16_t)s_sgb_buf[base + 1] << 8);
    }

    s_sgb_dirty = true;
}

// ── sgb_observe_joyp ─────────────────────────────────────────────────────────
// Called from the FF00 write handler in walnut_cgb.h with the raw value the
// game writes — before any input-state masking. Cost: ~3 comparisons when idle,
// one bit-shift + array write during packet reception.
//
// Must be called ONLY after IO_BOOT != 0 (boot ROM has exited) so the boot
// ROM's own init packets (which carry zeroed WRAM data) are ignored.
static inline void sgb_observe_joyp(uint8_t val) {
    // Mask to bits[5:4] only — the input bits in the lower nibble are joypad
    // state, not SGB signalling.
    val &= 0x30u;

    if (val == 0x00u) {
        // Reset / start-of-packet pulse — (re)start the decoder.
        s_sgb_dec  = SgbDecState::RECV;
        s_sgb_bits = 0;
        memset(s_sgb_buf, 0, sizeof(s_sgb_buf));
        return;
    }

    if (s_sgb_dec != SgbDecState::RECV) return;
    if (val == 0x30u) return;   // idle strobe between bits — ignore

    // 0x10 → bit=1 (P14 low), 0x20 → bit=0 (P15 low).
    const uint8_t bit = (val == 0x10u) ? 1u : 0u;

    const int byte_idx = s_sgb_bits >> 3;
    const int bit_pos  = s_sgb_bits & 7;

    if (byte_idx < 16)   // bounds guard (should always hold)
        s_sgb_buf[byte_idx] |= (uint8_t)(bit << bit_pos);

    if (++s_sgb_bits == 128) {
        sgb_process_packet();
        s_sgb_dec = SgbDecState::IDLE;
    }
}

// ── sgb_apply_if_dirty ───────────────────────────────────────────────────────
// Converts decoded SGB RGB555 colours to RGBA4444 and writes them into the
// branchless flat-palette table g_dmg_flat_pal that gb_lcd_draw_line reads.
// Call once per frame after gb_run_one_frame(). Is a no-op when no new palette
// commands have been received since the last call.
//
// RGBA4444 packing (mirrors gb_pack_rgb555 in gb_core.h):
//   Strip LSB of each 5-bit channel → 4-bit channel; alpha = 0xF.
//   Layout (LE): r4 | (g4<<4) | (b4<<8) | 0xF000.
static inline void sgb_apply_if_dirty() {
    if (!s_sgb_dirty) return;
    s_sgb_dirty = false;

    // Inline rgb555→rgba4444 conversion (same formula as gb_pack_rgb555).
    auto pack = [](const uint16_t c) -> uint16_t {
        const uint8_t r = ( c        & 0x1Fu) >> 1u;
        const uint8_t g = ((c >>  5) & 0x1Fu) >> 1u;
        const uint8_t b = ((c >> 10) & 0x1Fu) >> 1u;
        return (uint16_t)(r | (uint16_t)(g << 4) | (uint16_t)(b << 8) | 0xF000u);
    };

    // BG   → SGB palette 0  (primary game colours)
    // OBJ0 → SGB palette 1  (sprite colours)
    // OBJ1 → SGB palette 1  (same; ATTR_BLK not emulated)
    for (int c = 0; c < 4; ++c) {
        g_dmg_flat_pal[0x20 + c] = pack(s_sgb_pal[0][c]);  // BG
        g_dmg_flat_pal[0x00 + c] = pack(s_sgb_pal[1][c]);  // OBJ0
        g_dmg_flat_pal[0x10 + c] = pack(s_sgb_pal[1][c]);  // OBJ1
    }
}