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
 *     0x10  bit = 1  (P15 low: bit5=0 in FF00, action/button row selected)
 *     0x20  bit = 0  (P14 low: bit4=0 in FF00, direction row selected)
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

// ── Active flag ───────────────────────────────────────────────────────────────
// Set true only when a DMG game is running in SGB palette mode.
// Gates both the walnut joypad hook and sgb_apply_if_dirty so non-SGB games
// pay zero cost (one short-circuit branch, before any memory reads or writes).
// Reset to false by sgb_init() on every ROM load; set true by sgb_activate()
// in main.cpp once the SGB cold boot is confirmed.
static bool     s_sgb_active     = false;

// ── Packet decoder state ──────────────────────────────────────────────────────
// Tiny two-state FSM: IDLE waits for a reset pulse; RECV accumulates bits.
enum class SgbDecState : uint8_t { IDLE, RECV };
static SgbDecState s_sgb_dec     = SgbDecState::IDLE;
static uint8_t     s_sgb_buf[16] = {};   // 128-bit packet buffer
static int         s_sgb_bits    = 0;    // bits received so far (0..127)
// Previous masked write value — needed to distinguish the SGB clock-low (0x00
// written immediately after a data bit, i.e. prev=0x10/0x20) from the
// packet-start/abort-reset pulse (0x00 written after an idle strobe, i.e.
// prev=0x30).  Without this, a reset on every 0x00 wipes the buffer after
// every single bit and the decoder never accumulates 128 bits.
static uint8_t     s_sgb_prev    = 0x30u;

// ── sgb_init ─────────────────────────────────────────────────────────────────
// Reset decoder + palette. Call once when a ROM is loaded.
// Leaves g_dmg_flat_pal as-is — gb_select_dmg_palette() already populated it
// with the best static approximation; sgb_apply_if_dirty() will overwrite once
// the game sends real palette commands.
static inline void sgb_init() {
    s_sgb_active = false;   // disabled until sgb_activate() is called
    s_sgb_dec   = SgbDecState::IDLE;
    s_sgb_bits  = 0;
    s_sgb_prev  = 0x30u;   // idle/strobe — clean slate for first packet
    memset(s_sgb_buf, 0, sizeof(s_sgb_buf));
    memset(s_sgb_pal, 0, sizeof(s_sgb_pal));
    s_sgb_dirty = false;
}

// ── sgb_activate ─────────────────────────────────────────────────────────────
// Enable the decoder and apply hook. Call in main.cpp after confirming that
// the SGB boot ROM is running (or has run in a resumed session).
// Must be called AFTER sgb_init() so the flag is not immediately cleared.
static inline void sgb_activate() { s_sgb_active = true; }

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
// game writes — before any input-state masking.
//
// Must be called ONLY after IO_BOOT != 0 (boot ROM has exited) so the boot
// ROM's own init packets are ignored.
//
// SGB serial protocol (per Pan Docs, verified against SGB2 ASM):
//   Packet start:  ... 0x30, 0x00, 0x30 ...
//   Per data bit:  0x00 (clock-low), 0x10 (bit=1) or 0x20 (bit=0), 0x30 (clock-high)
//   Stop bit:      0x00, 0x20, 0x30  (always 0, ignored — packet done at bit 128)
//
// Critical detail: 0x00 is written BEFORE EVERY BIT as the clock-low pulse.
// The previous implementation reset the decoder on every 0x00, which meant
// the buffer was wiped after every single bit — 128 bits never accumulated.
//
// Fix: track s_sgb_prev.  A decoder reset fires only when 0x00 follows 0x30
// (the actual packet-start / abort-restart signal).  Clock-low 0x00 writes
// (prev = 0x10 or 0x20 from the previous data write) are silently ignored.
//
// Normal joypad polling (e.g. 0x30 → 0x10 → read → 0x30) is also harmless:
// a 0x10 or 0x20 write is only counted as a data bit when it follows a 0x00,
// so 0x30→0x10/0x20 sequences from normal joypad reads never accumulate bits.
static inline void sgb_observe_joyp(uint8_t val) {
    val &= 0x30u;

    if (val == 0x30u) {
        // Idle / clock-high / strobe — always ignore, but remember it so the
        // next 0x00 can trigger a proper packet-start reset.
        s_sgb_prev = 0x30u;
        return;
    }

    if (val == 0x00u) {
        // Packet-start reset: 0x00 arriving after a 0x30 (idle strobe), OR
        // when the decoder is IDLE (first write ever / after a completed packet).
        // Clock-low 0x00 writes that arrive after a data bit (prev = 0x10/0x20)
        // are silently dropped — they are NOT a reset.
        if (s_sgb_dec == SgbDecState::IDLE || s_sgb_prev == 0x30u) {
            s_sgb_dec  = SgbDecState::RECV;
            s_sgb_bits = 0;
            memset(s_sgb_buf, 0, sizeof(s_sgb_buf));
        }
        s_sgb_prev = 0x00u;
        return;
    }

    // val == 0x10 or 0x20 (data bit).
    //
    // Only count as a valid SGB bit when:
    //   1. Decoder is in RECV mode, AND
    //   2. The immediately previous write was 0x00 (the clock-low pulse).
    //
    // This rejects normal joypad polling (0x30 → 0x10/0x20) and any stray
    // 0x10/0x20 writes that are not part of a properly clocked SGB transfer.
    if (s_sgb_dec == SgbDecState::RECV && s_sgb_prev == 0x00u) {
        // Pan Docs SGB Command Packet spec:
        //   Bit = 1 → P15 LOW (bit5=0, action/button row)  → game writes 0x10
        //   Bit = 0 → P14 LOW (bit4=0, direction row)      → game writes 0x20
        const uint8_t bit = (val == 0x10u) ? 1u : 0u;

        const int byte_idx = s_sgb_bits >> 3;
        const int bit_pos  = s_sgb_bits & 7;

        if (byte_idx < 16)   // bounds guard
            s_sgb_buf[byte_idx] |= (uint8_t)(bit << bit_pos);

        if (++s_sgb_bits == 128) {
            sgb_process_packet();
            s_sgb_dec = SgbDecState::IDLE;
        }
    }

    s_sgb_prev = val;
}

// ── sgb_apply_if_dirty ───────────────────────────────────────────────────────
// Converts decoded SGB RGB555 colours to RGBA4444 and writes them into the
// branchless flat-palette table g_dmg_flat_pal that gb_lcd_draw_line reads.
// Call once per frame after gb_run_one_frame(). Is a no-op when no new palette
// commands have been received since the last call, or when not in SGB mode.
//
// RGBA4444 packing — single expression, no temporaries:
//   Extract bits[4:1]→[3:0] (R),  bits[9:6]→[7:4] (G),  bits[14:11]→[11:8] (B),
//   then OR alpha nibble 0xF000.
//   Masks needed: R=(c>>1)&0x000F, G=(c>>2)&0x00F0, B=(c>>3)&0x0F00.
static inline void sgb_apply_if_dirty() {
    if (!s_sgb_active || !s_sgb_dirty) return;
    s_sgb_dirty = false;

    // Inline rgb555→rgba4444: one expression, zero temporaries.
    auto pack = [](const uint16_t c) -> uint16_t {
        return (uint16_t)(((c >> 1) & 0x000Fu)
                        | ((c >> 2) & 0x00F0u)
                        | ((c >> 3) & 0x0F00u)
                        | 0xF000u);
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