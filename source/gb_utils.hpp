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
                                   u32 skip_grp_end) {
    // ── Same entry guards as Renderer::drawWallpaper() ──────────────────────
    if (!ult::expandedMemory || ult::refreshWallpaper.load(std::memory_order_acquire)) return;

    ult::inPlot.store(true, std::memory_order_release);

    if (!ult::wallpaperData.empty() &&
        !ult::refreshWallpaper.load(std::memory_order_acquire) &&
        ult::correctFrameSize)
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
                               + (kH - skip_row_end) * kGW;
        const u32 target       = (total_work + numThreads - 1u) / numThreads;

        // thread_starts[t] / thread_starts[t+1] are the row range for thread t.
        // Stack array — numThreads is always small (≤ 4 on Switch).
        u32 thread_starts[9] = {};  // [0..numThreads], max 8 threads + sentinel
        thread_starts[0] = 0u;
        {
            u32 cum = 0u, ti = 1u;
            for (u32 y = 0u; y < kH && ti < numThreads; ++y) {
                cum += (y >= skip_row_start && y < skip_row_end) ? mid_row_work : kGW;
                if (cum >= ti * target) thread_starts[ti++] = y + 1u;
            }
            thread_starts[numThreads] = kH;
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
                        const u8* const rs    = src_base + y * (kW * 2u);
                        for (u32 g = 0u; g < kGW; ++g) do_group(yPart, rs, g);
                    }

                    const u32 mid_start = std::max(rowStart, skip_row_start);
                    const u32 mid_end   = std::min(rowEnd,   skip_row_end);
                    for (u32 y = mid_start; y < mid_end; ++y) {
                        const u32       yPart = s_yParts[y];
                        const u8* const rs    = src_base + y * (kW * 2u);
                        for (u32 g = 0u;           g < skip_grp_start; ++g) do_group(yPart, rs, g);
                        for (u32 g = skip_grp_end; g < kGW;            ++g) do_group(yPart, rs, g);
                    }

                    const u32 bot_start = std::max(rowStart, skip_row_end);
                    for (u32 y = bot_start; y < rowEnd; ++y) {
                        const u32       yPart = s_yParts[y];
                        const u8* const rs    = src_base + y * (kW * 2u);
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