/********************************************************************************
 * File: gb_audio.h
 * Description:
 *   GB APU → Switch audout bridge — dedicated audio thread design.
 *
 *   Architecture:
 *
 *   RENDER THREAD (draw())
 *   ┌─────────────────────────────────────────────┐
 *   │ gb_run_frame()                              │
 *   │   └─ walnut_cgb calls audio_write(addr,val) │  ← SPSC enqueue only (~5 ns)
 *   │ gb_audio_submit()  [no-op]                  │
 *   │ render pixels                               │
 *   └─────────────────────────────────────────────┘
 *           (fully decoupled — no signals)
 *   AUDIO THREAD (dedicated, priority 0x2C = same as render thread)
 *   ┌─────────────────────────────────────────────┐
 *   │ drain SPSC queue → local GBAPU state        │
 *   │ generate_samples()  — 802 stereo @ 48 kHz   │
 *   │ audoutGetReleasedAudioOutBuffer() [non-blk] │  under m_audioMutex
 *   │ audoutPlayBuffer()                          │  under m_audioMutex
 *   │ svcSleepThread(remaining_frame_ns)          │  drift-corrected 59.73 fps
 *   └─────────────────────────────────────────────┘
 *
 *   Audio quality:
 *   - Proper DAC model: each channel maps its digital [0..15] output to float
 *     [-1..+1] (0.0 when the DAC is powered off). This naturally eliminates
 *     the abrupt DC step when channels are gated on/off, removing the "buzz"
 *     artefact. The real GB has an analog output capacitor for the same reason.
 *   - NO low-pass filter: LP rounds square-wave transitions and makes high-
 *     frequency tones sound muddy. GB audio requires sharp edges.
 *   - DC blocker (high-pass ~8 Hz): removes residual DC via the same one-pole
 *     IIR used by most accurate GB emulators.  HP_R is derived from the Pan
 *     Docs capacitor spec: 0.999958^(4194304/48000/4) ≈ 0.99908 per sample.
 *
 *   Pause / resume (system-combo overlay hide / show):
 *   - gb_audio_pause()  — sets paused flag; audio thread discards SPSC events
 *     and submits silence.  GBAPU state (including hp_ch[]) is preserved.
 *   - gb_audio_resume() — clears paused flag.  Thread transitions naturally to
 *     real audio on the next iteration.  At most one silence frame (16.7 ms)
 *     precedes the first real-audio frame — imperceptible.
 *
 *   X → ROM menu → re-enter game (gb_audio_shutdown + gb_audio_init):
 *     hp_ch[] is saved to s_ctrl.hp_ch when the thread exits (shutdown) and
 *     restored into local.hp_ch at thread startup (init).  This prevents the
 *     ~5 ms HP-filter undershoot transient that caused crackling on re-entry.
 *     regs[] is similarly preserved so the APU restores to its exact last state.
 *
 *   Why audoutAppendAudioOutBuffer + svcSleepThread (not audoutPlayBuffer):
 *     audoutPlayBuffer = audoutAppendAudioOutBuffer + audoutWaitPlayFinish.
 *     The blocking wait means only 1 buffer is ever in the hardware queue.
 *     During the ~1ms gap between buffer completion and the next submission
 *     (generate + mutex overhead) the hardware has nothing to play — this
 *     occurs 60×/sec producing 60Hz amplitude modulation on every tone,
 *     audible as a reverb-like chorus haze on sustained notes.
 *     audoutAppendAudioOutBuffer is non-blocking (μs inside mutex); the
 *     pre-queued silence frames keep the hardware queue 2-3 deep at all
 *     times.  svcSleepThread(GB_FRAME_NS) is the submission clock.
 * 
 *  Licensed under GPLv2
 *  Copyright (c) 2026 ppkantorski
 ********************************************************************************/

#pragma once

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <mutex>
#include <switch.h>
#include "tesla.hpp"   // ult::Audio::m_audioMutex, m_initialized

#pragma GCC optimize("O3")

// Forward-declare the APU callbacks that walnut_cgb.h calls.
// gb_audio.h must be self-contained so it can be included before gb_core.h.
extern "C" {
    uint8_t audio_read(uint8_t addr);
    void    audio_write(uint8_t addr, uint8_t val);
}

extern "C" {
#include "walnut_cgb.h"
}

// ─────────────────────────────────────────────────────────────────────────────
// Output constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr uint32_t GB_CPU_HZ    = 4194304u;
static constexpr uint32_t GB_OUT_RATE  = 48000u;
static constexpr uint32_t GB_FRAME_CYC = 70224u;

// Samples per frame — exact value is 48000×70224/4194304 = 803.6499…
// A fixed ceiling of 804 advances 70254 T-cycles per frame instead of 70224,
// making every note play +0.754 cents sharp — audible on music you know well.
// The audio thread uses a Bresenham accumulator to alternate 803 / 804 samples
// so the long-run average is exactly 48000 Hz (zero pitch error).
// GB_SPF is the maximum size (804) used for DMA buffer sizing only.
static constexpr uint32_t GB_SPF =
    (static_cast<uint64_t>(GB_OUT_RATE) * GB_FRAME_CYC + GB_CPU_HZ - 1u) / GB_CPU_HZ;
static constexpr uint32_t GB_SPF_BASE =
    static_cast<uint32_t>(static_cast<uint64_t>(GB_OUT_RATE) * GB_FRAME_CYC / GB_CPU_HZ); // 803
static constexpr uint32_t GB_SPF_FRAC_INC =
    static_cast<uint32_t>((static_cast<uint64_t>(GB_OUT_RATE) * GB_FRAME_CYC) % GB_CPU_HZ);

static constexpr uint32_t GB_DMA_DATA  = GB_SPF * 2u * sizeof(int16_t);
static constexpr uint32_t GB_DMA_ALIGN = 0x1000u;
static constexpr uint32_t GB_DMA_CAP   =
    (GB_DMA_DATA + GB_DMA_ALIGN - 1u) & ~(GB_DMA_ALIGN - 1u);

static constexpr int GB_SEQ_CYC = 8192;  // frame sequencer: 512 Hz

static constexpr uint8_t GB_DUTY[4][8] = {
    {0,0,0,0,0,0,0,1},{1,0,0,0,0,0,0,1},{1,0,0,0,1,1,1,1},{0,1,1,1,1,1,1,0}
};
static constexpr uint16_t GB_NDIV[8] = {8,16,32,48,64,80,96,112};

// DC-blocker decay coefficient from Pan Docs capacitor spec:
//   charge factor = 0.999958 per T-cycle (the 4.194 MHz GB clock)
//   per output sample @ 48 kHz: 0.999958 ^ (4194304/48000) = 0.99634
//
// IMPORTANT: the exponent must NOT be divided by 4.  Pan Docs defines the
// factor per T-cycle (one 4 MHz clock tick), not per machine cycle (4 T).
// The old value (0.99908) used /4, giving a 22ms time constant.  The
// correct 5.7ms constant settles between volume envelope steps (~7ms min),
// eliminating the periodic dip/hum on sustained notes caused by the DC
// tracker permanently lagging behind the changing envelope DC level.
static constexpr float HP_R = 0.99634f;
static constexpr float HP_C = 1.0f - HP_R;  // complement; avoids recomputing each sample

// ─────────────────────────────────────────────────────────────────────────────
// GBAPU — full APU state, owned exclusively by the audio thread.
// ─────────────────────────────────────────────────────────────────────────────
struct GBAPU {
    struct Ch1 {
        int timer=0,duty_pos=0; uint8_t duty=0;
        int vol=0; uint8_t vol_init=0; bool vol_add=false;
        int vol_timer=0,vol_period=0; uint16_t period=0;
        int len_timer=0; bool len_en=false,enabled=false,dac_on=false;
        int sw_timer=0,sw_period=0,sw_shift=0; bool sw_negate=false; uint16_t sw_shadow=0;
    } ch1;
    struct Ch2 {
        int timer=0,duty_pos=0; uint8_t duty=0;
        int vol=0; uint8_t vol_init=0; bool vol_add=false;
        int vol_timer=0,vol_period=0; uint16_t period=0;
        int len_timer=0; bool len_en=false,enabled=false,dac_on=false;
    } ch2;
    struct Ch3 {
        int timer=0,pos=0; uint8_t vol_shift=0; uint16_t period=0;
        int len_timer=0; bool len_en=false,enabled=false,dac_on=false;
        uint8_t ram[16]={};
    } ch3;
    struct Ch4 {
        int timer=0,vol=0; uint8_t vol_init=0; bool vol_add=false;
        int vol_timer=0,vol_period=0;
        uint8_t clock_shift=0,div_code=0; bool width7=false; uint16_t lfsr=0x7FFF;
        int len_timer=0; bool len_en=false,enabled=false,dac_on=false;
    } ch4;
    uint8_t nr50=0x77,nr51=0xFF; bool power=false;
    int seq_cycles=0,seq_step=0; uint32_t cycle_frac=0;

    // Per-channel DC-blocker (applied to each channel before NR51 mix).
    // Keeping HP per-channel means a DC step on one channel (e.g. an arpeggio
    // channel triggering a new note) cannot bleed into the other channels'
    // waveforms — the combined-mix HP caused audible amplitude distortion.
    // Preserved across pause/resume to avoid clicks.
    float hp_ch[4]={};

    // Pop-prevention: shadow the previous dac_on / enabled state per channel.
    //
    // When dac_on or enabled changes between consecutive output samples the raw
    // pre-HP channel output 'd' can jump by up to 2.0:
    //   • dac_on false→true:  d jumps  0.0 → -1.0        (Δ = -1.0)
    //   • dac_on true→false:  d jumps  playing → 0.0
    //   • enabled true→false: d jumps  playing → -1.0
    //   • enabled false→true: d jumps -1.0 → new note level
    //
    // The HP filter, having been tracking the old DC level in hp_ch, converts
    // that full step into an exponential transient (≈ HP_R × Δ in the very
    // first sample, decaying over 5.7 ms) — heard as an audible pop/click on
    // music transitions, scene changes, and channel gating.
    //
    // Fix: when either flag changes, snap hp_ch[i] to the current d value so
    // d_out = d - hp_ch = 0 at the transition sample.  The HP filter resumes
    // normally from the new level with no transient.
    //
    // Normal note play (both flags constant) is completely unaffected.
    // Pokémon Yellow voice: NR30=0→0x80→trigger all fire within one sample's
    // event batch, so the state ends the same as it began — no snap fires.
    bool prev_dac_on[4]  = {};
    bool prev_enabled[4] = {};
};

// Software master volume scalar (0.0 = silent, 1.0 = full).
// Written by gb_audio_set_volume() from the UI thread; read every sample in
// the mix loop.  relaxed ordering is fine — a one-frame lag on a slider drag
// is imperceptible.
static std::atomic<float> s_master_gain{1.0f};

static inline int ch1_p(const GBAPU& a){return (2048-a.ch1.period)*4;}
static inline int ch2_p(const GBAPU& a){return (2048-a.ch2.period)*4;}
static inline int ch3_p(const GBAPU& a){return (2048-a.ch3.period)*2;}
static inline int ch4_p(const GBAPU& a){return GB_NDIV[a.ch4.div_code]<<a.ch4.clock_shift;}

// ─────────────────────────────────────────────────────────────────────────────
// apply_reg — replay one register write into a GBAPU instance.
// ─────────────────────────────────────────────────────────────────────────────
static void apply_reg(GBAPU& a, uint8_t addr, uint8_t val) {
    if (addr>=0x30&&addr<=0x3F){a.ch3.ram[addr-0x30]=val;return;}
    if (!a.power&&addr!=0x26) return;
    switch(addr){
    case 0x10: a.ch1.sw_period=(val>>4)&7;a.ch1.sw_negate=(val>>3)&1;a.ch1.sw_shift=val&7;break;
    case 0x11: a.ch1.duty=(val>>6)&3;a.ch1.len_timer=64-(val&0x3F);break;
    case 0x12: a.ch1.vol_init=(val>>4)&0xF;a.ch1.vol_add=(val>>3)&1;a.ch1.vol_period=val&7;
               a.ch1.dac_on=(val&0xF8)!=0;if(!a.ch1.dac_on)a.ch1.enabled=false;break;
    case 0x13: a.ch1.period=(a.ch1.period&0x700u)|val;break;
    case 0x14: a.ch1.period=(a.ch1.period&0xFFu)|(static_cast<uint16_t>(val&7)<<8);
               a.ch1.len_en=(val>>6)&1;
               if(val&0x80){a.ch1.enabled=a.ch1.dac_on;a.ch1.vol=a.ch1.vol_init;
                   a.ch1.vol_timer=a.ch1.vol_period;
                   a.ch1.timer=ch1_p(a)>0?ch1_p(a):1;
                   // NOTE: duty_pos is intentionally NOT reset here.  Real GB
                   // hardware does not reset the duty-cycle position on trigger —
                   // only the period timer and envelope are reloaded.  Resetting
                   // duty_pos caused a phase discontinuity (click) on retriggered
                   // notes that doesn't exist on hardware.
                   if(a.ch1.len_timer==0)a.ch1.len_timer=64;
                   a.ch1.sw_shadow=a.ch1.period;
                   a.ch1.sw_timer=a.ch1.sw_period?a.ch1.sw_period:8;}break;
    case 0x16: a.ch2.duty=(val>>6)&3;a.ch2.len_timer=64-(val&0x3F);break;
    case 0x17: a.ch2.vol_init=(val>>4)&0xF;a.ch2.vol_add=(val>>3)&1;a.ch2.vol_period=val&7;
               a.ch2.dac_on=(val&0xF8)!=0;if(!a.ch2.dac_on)a.ch2.enabled=false;break;
    case 0x18: a.ch2.period=(a.ch2.period&0x700u)|val;break;
    case 0x19: a.ch2.period=(a.ch2.period&0xFFu)|(static_cast<uint16_t>(val&7)<<8);
               a.ch2.len_en=(val>>6)&1;
               if(val&0x80){a.ch2.enabled=a.ch2.dac_on;a.ch2.vol=a.ch2.vol_init;
                   a.ch2.vol_timer=a.ch2.vol_period;
                   a.ch2.timer=ch2_p(a)>0?ch2_p(a):1;
                   // duty_pos not reset — see CH1 trigger comment above.
                   if(a.ch2.len_timer==0)a.ch2.len_timer=64;}break;
    case 0x1A: a.ch3.dac_on=(val&0x80)!=0;if(!a.ch3.dac_on)a.ch3.enabled=false;break;
    case 0x1B: a.ch3.len_timer=256-val;break;
    case 0x1C: a.ch3.vol_shift=(val>>5)&3;break;
    case 0x1D: a.ch3.period=(a.ch3.period&0x700u)|val;break;
    case 0x1E: a.ch3.period=(a.ch3.period&0xFFu)|(static_cast<uint16_t>(val&7)<<8);
               a.ch3.len_en=(val>>6)&1;
               if(val&0x80){a.ch3.enabled=a.ch3.dac_on;
                   a.ch3.timer=ch3_p(a)>0?ch3_p(a):1;a.ch3.pos=0;
                   if(a.ch3.len_timer==0)a.ch3.len_timer=256;}break;
    case 0x20: a.ch4.len_timer=64-(val&0x3F);break;
    case 0x21: a.ch4.vol_init=(val>>4)&0xF;a.ch4.vol_add=(val>>3)&1;a.ch4.vol_period=val&7;
               a.ch4.dac_on=(val&0xF8)!=0;if(!a.ch4.dac_on)a.ch4.enabled=false;break;
    case 0x22: a.ch4.clock_shift=val>>4;a.ch4.width7=(val>>3)&1;a.ch4.div_code=val&7;break;
    case 0x23: a.ch4.len_en=(val>>6)&1;
               if(val&0x80){a.ch4.enabled=a.ch4.dac_on;a.ch4.vol=a.ch4.vol_init;
                   a.ch4.vol_timer=a.ch4.vol_period;
                   a.ch4.timer=ch4_p(a)>0?ch4_p(a):1;a.ch4.lfsr=0x7FFF;
                   if(a.ch4.len_timer==0)a.ch4.len_timer=64;}break;
    case 0x24: a.nr50=val;break;
    case 0x25: a.nr51=val;break;
    case 0x26:
        a.power=(val&0x80)!=0;
        if(!a.power){
            const auto ram=a.ch3.ram;
            const int sc=a.seq_cycles,ss=a.seq_step;const uint32_t cf=a.cycle_frac;
            float hc[4]; memcpy(hc,a.hp_ch,sizeof(hc));
            a.ch1={};a.ch2={};a.ch4={};a.ch3={};memcpy(a.ch3.ram,ram,16);
            a.nr50=0;a.nr51=0;a.seq_cycles=sc;a.seq_step=ss;a.cycle_frac=cf;
            memcpy(a.hp_ch,hc,sizeof(hc));}
        break;
    default:break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// SPSC event queue
//
// EVT_CAP = 4096: Pokémon Yellow's Pikachu voice engine fires a timer IRQ at
// ~8 kHz.  Each interrupt writes NR30=0 (DAC off), 16 wave RAM bytes, NR30=0x80
// (DAC on), NR33, NR34 (trigger) — ~20 events.  At 8 kHz that is ~134 IRQs ×
// 20 = ~2 680 events per frame.  The old cap of 512 caused queue overflow and
// silently dropped the critical NR30=0x80 / NR34 writes, leaving CH3's DAC in
// the OFF state every frame → complete silence on the voice channel.
// ─────────────────────────────────────────────────────────────────────────────
struct RegEvent { uint8_t addr, val; uint16_t cycle; };  // cycle: T-cycles since frame start (0..70223)

// Pointer to the live gb_s struct, set once via gb_audio_set_gb_ptr().
// audio_write() reads hram_io[IO_LY]*456 + counter.lcd_count to get the exact
// T-cycle offset within the current frame — zero overhead, perfectly accurate,
// and independent of wall-clock time (which fails because the emulator runs
// faster than real-time: a 16.75 ms game frame may complete in ~2 ms, so all
// events get near-zero delta_ns and pile up at sample 0, breaking Pikachu).
static const gb_s* s_gb_ptr = nullptr;

// gb_audio_set_gb_ptr — call once after gb_s is initialised, before the first
// gb_run_one_frame.  The pointer remains valid for the entire emulation session.
static inline void gb_audio_set_gb_ptr(const gb_s* p) { s_gb_ptr = p; }

// gb_audio_mark_frame_start — now a no-op; kept so existing call sites in
// gb_core.h compile without modification.  Cycle offsets are derived from the
// live gb_s counters in audio_write() instead of a wall-clock snapshot.
static inline void gb_audio_mark_frame_start() {}
// SPSC ring — must be power-of-2 for the bitmask drain.
// One frame worst-case (Pokémon Yellow Pikachu voice): ~134 IRQs × 20 events
// ≈ 2,680 events.  4096 holds a full frame plus ~1,400 slots of headroom.
// At 60 fps / 59.73 fps the render thread is at most ~1 frame ahead of the
// audio thread in steady state, so 4096 is sufficient.  Saves 8 KB vs 8192.
static constexpr uint32_t EVT_CAP  = 4096u;
static constexpr uint32_t EVT_MASK = EVT_CAP - 1u;

// Per-frame local copy — does NOT need to be power-of-2 (flat array, not ring).
// Bounded by one sentinel's worth of events; 3072 > 2,680 with comfortable
// margin.  Saves 10 KB vs a second EVT_CAP-sized buffer.
static constexpr uint32_t LOC_CAP  = 3072u;

// Flat event buffer owned exclusively by the audio thread.  The audio thread
// copies its frame's events here before passing them to generate_samples().
// File-scope (not stack) because the audio thread stack is only 0x1000 bytes.
static RegEvent s_local_evts[LOC_CAP];

// ─────────────────────────────────────────────────────────────────────────────
// ─────────────────────────────────────────────────────────────────────────────
// generate_samples — writes n_samp stereo int16 samples into dst,
// distributing the supplied register events at their true T-cycle position
// within the frame.
//
// Event distribution:
//   Each RegEvent carries a `cycle` field (T-cycles since frame start, 0..70223)
//   stamped by audio_write() via LY*456+lcd_count.  generate_samples converts
//   this to an output sample index:
//       due_sample = floor(event.cycle * n_samp / GB_FRAME_CYC)
//   This correctly places:
//     • Pokémon Yellow Pikachu voice: ~134 wave-table clusters fired at 8 kHz
//       IRQ intervals → evenly spaced cycles → same ~6-sample windows as before.
//     • Tamagotchi / short SFX: NR21-NR24 all written within ~100 T-cycles at
//       frame start → all map to sample 0-1 → trigger fires immediately rather
//       than being delayed to sample 602, so a tight length counter still
//       produces the full audible note.
//   The old index-based formula (j * n_samp / n_evts) was equivalent only when
//   events were perfectly evenly spaced — true for Pikachu's voice, wrong for
//   any game whose sound effects are clustered at the start of a frame.
//
// Output pipeline per sample:
//   1. Frame sequencer tick (length / sweep / volume envelope, integer)
//   2. Per-channel time-integration over the output sample period:
//        For CH1/CH2 (square): count how many of the `cyc` CPU cycles the
//        duty output was HIGH.  Output = (high_cycles / cyc) × vol.
//        For CH3 (wave):       accumulate (sample_nibble × cycles_held) / cyc.
//        For CH4 (noise LFSR): count high cycles for current LFSR state.
//      This is a box-filter / integrate-and-dump approach.  It completely
//      eliminates aliasing from high-frequency oscillators — without it,
//      a channel cycling 20× per output sample produces a scratchy alias
//      tone instead of the correct (near-silence) high-frequency note.
//   3. DAC model: averaged digital value [0..15] → float [-1..+1].
//      DAC-off channels contribute 0.0.  This removes gate-on/off DC steps.
//   4. NR51 panning + NR50 master volume mix (float).
//   5. DC blocker (HP_R capacitor), no low-pass filter.
// ─────────────────────────────────────────────────────────────────────────────
static void generate_samples(GBAPU& a, int16_t* dst, uint32_t n_samp,
                             const RegEvent* evts = nullptr, uint32_t n_evts = 0) {
    static constexpr uint32_t STEP =
        static_cast<uint32_t>((static_cast<uint64_t>(GB_CPU_HZ)<<16)/GB_OUT_RATE);

    static constexpr float DAC_SCALE = 1.0f / 7.5f;  // [0..15] → [-1..+1]
    static constexpr float MIX_SCALE = 900.0f;        // ~88% of int16 range

    uint32_t evt_idx = 0;   // next event to apply

    // Hoist master gain: one atomic load per frame-call, not per sample.
    // A one-frame lag on a slider drag is imperceptible.
    const float mg = s_master_gain.load(std::memory_order_relaxed);

    // Precompute sample-index due values using a 32-bit fixed-point scale factor
    // instead of a 64-bit divide per event.  cycle is 0..70223 (16-bit), n_samp
    // is 803 or 804, GB_FRAME_CYC is 70224.  The scale = (n_samp << 16) /
    // GB_FRAME_CYC fits in 32 bits (804<<16 = 52,691,968 < 2^26), so
    // cycle * scale fits in 32 bits with no overflow (70223 * 52691968 >> 16
    // < 70223 * 804 = 56,459,292 < 2^26).  Result is identical to the 64-bit
    // path for all valid inputs; 64-bit divide was ~10x more expensive on ARM.
    static uint32_t due_arr[LOC_CAP];
    {
        const uint32_t scale = (n_samp << 16) / GB_FRAME_CYC;
        for (uint32_t k = 0; k < n_evts; ++k)
            due_arr[k] = (static_cast<uint32_t>(evts[k].cycle) * scale) >> 16;
    }

    // Hoist NR50 master volume out of the per-sample loop.
    // NR50 changes at most once per frame (rare mid-frame writes), so we track
    // the last-seen nr50 value and only recompute lv/rv when it changes.
    // This eliminates 803 pairs of shift+mask+cast+float-add operations per frame
    // in the common case where NR50 is constant (the vast majority of frames).
    uint8_t cached_nr50 = a.nr50;
    float lv = static_cast<float>(((cached_nr50 >> 4) & 7) + 1);
    float rv = static_cast<float>(( cached_nr50        & 7) + 1);

    // Hoist NR51 panning out of the per-sample loop.
    // NR51 also rarely changes mid-frame, so cache it exactly like NR50.
    uint8_t cached_nr51 = a.nr51;

    for (uint32_t i=0; i<n_samp; ++i) {
        // ── Cycle-accurate event distribution ────────────────────────────────
        // Apply every event whose T-cycle timestamp maps to sample index <= i.
        // event.cycle is 0..GB_FRAME_CYC-1; due = cycle * n_samp / GB_FRAME_CYC.
        // n_evts==0 is safe: the while guard ensures the body never executes.
        while (evt_idx < n_evts && due_arr[evt_idx] <= i) {
            apply_reg(a, evts[evt_idx].addr, evts[evt_idx].val);
            ++evt_idx;
        }

        // NR50 master volume — update cached floats only when NR50 actually changed
        // (covers the rare mid-frame NR50 write injected by the event loop above).
        if (__builtin_expect(a.nr50 != cached_nr50, 0)) {
            cached_nr50 = a.nr50;
            lv = static_cast<float>(((cached_nr50 >> 4) & 7) + 1);
            rv = static_cast<float>(( cached_nr50        & 7) + 1);
        }

        // NR51 panning — update cached byte only when NR51 actually changed
        // (covers the rare mid-frame NR51 write injected by the event loop above).
        if (__builtin_expect(a.nr51 != cached_nr51, 0)) {
            cached_nr51 = a.nr51;
        }

        a.cycle_frac+=STEP;
        const int cyc=static_cast<int>(a.cycle_frac>>16);
        a.cycle_frac&=0xFFFFu;
        const float rcyc = 1.0f / static_cast<float>(cyc);

        // ── Frame sequencer ───────────────────────────────────────────────────
        a.seq_cycles+=cyc;
        while(a.seq_cycles>=GB_SEQ_CYC){
            a.seq_cycles-=GB_SEQ_CYC;
            const int s=a.seq_step;
            if((s&1)==0){
                auto tl=[](bool&en,bool le,int& lt){if(le&&lt>0&&--lt==0)en=false;};
                tl(a.ch1.enabled,a.ch1.len_en,a.ch1.len_timer);
                tl(a.ch2.enabled,a.ch2.len_en,a.ch2.len_timer);
                tl(a.ch3.enabled,a.ch3.len_en,a.ch3.len_timer);
                tl(a.ch4.enabled,a.ch4.len_en,a.ch4.len_timer);
            }
            if(s==2||s==6){
                if(a.ch1.sw_period>0&&--a.ch1.sw_timer<=0){
                    a.ch1.sw_timer=a.ch1.sw_period;
                    if(a.ch1.sw_shift>0){
                        const int d=a.ch1.sw_shadow>>a.ch1.sw_shift;
                        const int np=a.ch1.sw_negate?a.ch1.sw_shadow-d:a.ch1.sw_shadow+d;
                        if(np>2047)a.ch1.enabled=false;
                        else a.ch1.sw_shadow=a.ch1.period=static_cast<uint16_t>(np);
                    }
                }
            }
            if(s==7){
                auto te=[](int&v,bool add,int&vt,int vp){
                    if(vp>0&&--vt<=0){vt=vp;if(add&&v<15)++v;else if(!add&&v>0)--v;}};
                te(a.ch1.vol,a.ch1.vol_add,a.ch1.vol_timer,a.ch1.vol_period);
                te(a.ch2.vol,a.ch2.vol_add,a.ch2.vol_timer,a.ch2.vol_period);
                te(a.ch4.vol,a.ch4.vol_add,a.ch4.vol_timer,a.ch4.vol_period);
            }
            a.seq_step=(a.seq_step+1)&7;
        }

        // ── CH1 — integrate square wave over cyc cycles ───────────────────────
        // Walk the duty/timer forward cycle-by-cycle, accumulating the number
        // of cycles the output was HIGH.  avg = hi_cycles / cyc × vol.
        float d1;
        {
            int rem=cyc, t=a.ch1.timer, dp=a.ch1.duty_pos, hi=0;
            if(t>rem){
                // Fast path: no duty-position crossing this sample
                if(a.ch1.enabled&&GB_DUTY[a.ch1.duty][dp]) hi=rem;
                t-=rem;
            } else {
                while(rem>0){
                    if(t<=0){dp=(dp+1)&7;const int p=ch1_p(a);t+=p>0?p:1;}
                    const int step=rem<t?rem:t;
                    if(a.ch1.enabled&&GB_DUTY[a.ch1.duty][dp]) hi+=step;
                    rem-=step; t-=step;
                }
            }
            a.ch1.timer=t; a.ch1.duty_pos=dp;
            const float avg=a.ch1.enabled
                ?static_cast<float>(hi)*rcyc*static_cast<float>(a.ch1.vol):0.0f;
            d1=a.ch1.dac_on?avg*DAC_SCALE-1.0f:0.0f;
        }
        if (a.ch1.dac_on != a.prev_dac_on[0] || a.ch1.enabled != a.prev_enabled[0]) {
            a.hp_ch[0] = d1;  // absorb DC step — no transient pop
            a.prev_dac_on[0]  = a.ch1.dac_on;
            a.prev_enabled[0] = a.ch1.enabled;
        }
        { const float _h=HP_R*a.hp_ch[0]+HP_C*d1; d1-=_h; a.hp_ch[0]=_h; }

        // ── CH2 — integrate square wave ───────────────────────────────────────
        float d2;
        {
            int rem=cyc, t=a.ch2.timer, dp=a.ch2.duty_pos, hi=0;
            if(t>rem){
                // Fast path: no duty-position crossing this sample
                if(a.ch2.enabled&&GB_DUTY[a.ch2.duty][dp]) hi=rem;
                t-=rem;
            } else {
                while(rem>0){
                    if(t<=0){dp=(dp+1)&7;const int p=ch2_p(a);t+=p>0?p:1;}
                    const int step=rem<t?rem:t;
                    if(a.ch2.enabled&&GB_DUTY[a.ch2.duty][dp]) hi+=step;
                    rem-=step; t-=step;
                }
            }
            a.ch2.timer=t; a.ch2.duty_pos=dp;
            const float avg=a.ch2.enabled
                ?static_cast<float>(hi)*rcyc*static_cast<float>(a.ch2.vol):0.0f;
            d2=a.ch2.dac_on?avg*DAC_SCALE-1.0f:0.0f;
        }
        if (a.ch2.dac_on != a.prev_dac_on[1] || a.ch2.enabled != a.prev_enabled[1]) {
            a.hp_ch[1] = d2;  // absorb DC step — no transient pop
            a.prev_dac_on[1]  = a.ch2.dac_on;
            a.prev_enabled[1] = a.ch2.enabled;
        }
        { const float _h=HP_R*a.hp_ch[1]+HP_C*d2; d2-=_h; a.hp_ch[1]=_h; }

        // ── CH3 — integrate wave table ────────────────────────────────────────
        // Accumulate (nibble_value × cycles_at_that_position) / cyc.
        float d3;
        {
            int rem=cyc, t=a.ch3.timer, pos=a.ch3.pos;
            float acc=0.0f;
            if(t>rem){
                // Fast path: wave-table position does not advance this sample
                if(a.ch3.enabled&&a.ch3.vol_shift>0){
                    const uint8_t b=a.ch3.ram[pos>>1];
                    const int nib=((pos&1)?(b&0xFu):(b>>4))>>(a.ch3.vol_shift-1);
                    acc=static_cast<float>(nib)*static_cast<float>(rem);
                }
                t-=rem;
            } else {
                while(rem>0){
                    if(t<=0){pos=(pos+1)&31;const int p=ch3_p(a);t+=p>0?p:1;}
                    const int step=rem<t?rem:t;
                    if(a.ch3.enabled&&a.ch3.vol_shift>0){
                        const uint8_t b=a.ch3.ram[pos>>1];
                        const int nib=((pos&1)?(b&0xFu):(b>>4))>>(a.ch3.vol_shift-1);
                        acc+=static_cast<float>(nib)*static_cast<float>(step);
                    }
                    rem-=step; t-=step;
                }
            }
            a.ch3.timer=t; a.ch3.pos=pos;
            d3=a.ch3.dac_on?acc*rcyc*DAC_SCALE-1.0f:0.0f;
        }
        if (a.ch3.dac_on != a.prev_dac_on[2] || a.ch3.enabled != a.prev_enabled[2]) {
            a.hp_ch[2] = d3;  // absorb DC step — no transient pop
            a.prev_dac_on[2]  = a.ch3.dac_on;
            a.prev_enabled[2] = a.ch3.enabled;
        }
        { const float _h=HP_R*a.hp_ch[2]+HP_C*d3; d3-=_h; a.hp_ch[2]=_h; }

        // ── CH4 — integrate LFSR noise ────────────────────────────────────────
        // The LFSR state is valid for `t` cycles; when t expires the LFSR shifts.
        // Accumulate high cycles for the current LFSR bit between each shift.
        float d4;
        {
            int rem=cyc, t=a.ch4.timer, hi=0;
            uint16_t lfsr=a.ch4.lfsr;
            if(t>rem){
                // Fast path: LFSR does not shift this sample
                if(a.ch4.enabled&&(~lfsr&1u)) hi=rem;
                t-=rem;
            } else {
                while(rem>0){
                    if(t<=0){
                        const int p=ch4_p(a);t+=p>0?p:1;
                        const uint16_t xb=(lfsr^(lfsr>>1))&1u;
                        lfsr=(lfsr>>1)|(xb<<14);
                        if(a.ch4.width7)lfsr=(lfsr&~(1u<<6))|(xb<<6);
                    }
                    const int step=rem<t?rem:t;
                    if(a.ch4.enabled&&(~lfsr&1u)) hi+=step;
                    rem-=step; t-=step;
                }
            }
            a.ch4.timer=t; a.ch4.lfsr=lfsr;
            const float avg=a.ch4.enabled
                ?static_cast<float>(hi)*rcyc*static_cast<float>(a.ch4.vol):0.0f;
            d4=a.ch4.dac_on?avg*DAC_SCALE-1.0f:0.0f;
        }
        if (a.ch4.dac_on != a.prev_dac_on[3] || a.ch4.enabled != a.prev_enabled[3]) {
            a.hp_ch[3] = d4;  // absorb DC step — no transient pop
            a.prev_dac_on[3]  = a.ch4.dac_on;
            a.prev_enabled[3] = a.ch4.enabled;
        }
        { const float _h=HP_R*a.hp_ch[3]+HP_C*d4; d4-=_h; a.hp_ch[3]=_h; }

        // ── NR51 panning + NR50 volume ────────────────────────────────────────
        float ls=0.0f,rs=0.0f;
        const uint8_t nr51 = cached_nr51;
        if(nr51&0x10u){ls+=d1;} if(nr51&0x01u){rs+=d1;}
        if(nr51&0x20u){ls+=d2;} if(nr51&0x02u){rs+=d2;}
        if(nr51&0x40u){ls+=d3;} if(nr51&0x04u){rs+=d3;}
        if(nr51&0x80u){ls+=d4;} if(nr51&0x08u){rs+=d4;}
        ls*=lv; rs*=rv;

        // ── Scale, clamp, write ──────────────────────────────────────────────
        // DC removed per-channel above; no combined-mix HP needed.
        const float sl=ls*MIX_SCALE*mg, sr=rs*MIX_SCALE*mg;
        *dst++=sl> 32767.f? 32767:sl<-32767.f?-32767:static_cast<int16_t>(sl);
        *dst++=sr> 32767.f? 32767:sr<-32767.f?-32767:static_cast<int16_t>(sr);
    }

    // Drain any events that integer division mapped past the last sample
    // (only occurs when n_evts > n_samp, i.e. extremely event-dense frames).
    while (evt_idx < n_evts) {
        apply_reg(a, evts[evt_idx].addr, evts[evt_idx].val);
        ++evt_idx;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Shared control block
// ─────────────────────────────────────────────────────────────────────────────
static struct GACtrl {
    RegEvent              events[EVT_CAP]{};
    std::atomic<uint32_t> evt_w{0};
    std::atomic<uint32_t> evt_r{0};

    // Live register mirror: index = addr - 0x10, covers NR10–NR52 + wave RAM.
    // Used by audio_read() to return correct values to peanut_gb.
    uint8_t regs[64]{};

    // HP capacitor state, one value per channel.  Persisted across
    // shutdown/init so the filter resumes at its settled value rather than
    // starting from 0.0 (which produces a ~5 ms undershoot transient heard
    // as a click or crackle on every re-entry into a game).
    float hp_ch[4]{};

    int16_t*       dma[4]  = {};
    AudioOutBuffer ab[4]   = {};
    uint8_t        cur     = 0;

    std::atomic<bool> thread_run{false};
    Thread            thread_handle{};
    bool              own_session = false;
    bool              ready       = false;

    // paused — thread submits silence; GBAPU state is fully preserved.
    std::atomic<bool> paused{false};

    // resync — set by gb_audio_request_resync() when the operation mode changes
    // (handheld ↔ docked).  The audio thread handles it inline on its next
    // iteration: flush, stop, start, re-prequeue two silence frames, reset
    // drift clock.  Zero blocking; zero overhead in the steady-state path
    // (one relaxed atomic load per audio frame, ~1 ns).
    std::atomic<bool> resync{false};

    // Full GBAPU snapshot written by the audio thread on exit (gb_audio_shutdown).
    // Restored on the next thread startup (gb_audio_init) so all runtime state
    // (channel enabled flags, timers, duty_pos, vol, seq_step, lfsr, ...) is
    // exact -- register replay alone cannot do this because apply_reg() never sets
    // ch.enabled, so every channel stays silent after a save-state resume.
    GBAPU snapshot{};
    bool  snapshot_valid{false};

    // Channel-active bits (NR52 bits 3-0) written by the audio thread after
    // every generate_samples() call and read by audio_read(0x26) on the
    // emulation thread.  Without this, NR52 always returns 0 in the channel
    // bits because ch.enabled lives only inside the audio thread's local GBAPU.
    // Games that poll NR52 to confirm a trigger (e.g. Oracle of Seasons/Ages
    // logo-sequence jingle) would spin forever on a white screen.
    std::atomic<uint8_t> nr52_ch_bits{0};
} s_ctrl;

// ─────────────────────────────────────────────────────────────────────────────
// audio_write / audio_read — called by peanut_gb on the render thread.
// ─────────────────────────────────────────────────────────────────────────────
extern "C" {

void audio_write(const uint8_t addr, const uint8_t val) {
    if (addr>=0x10&&addr<=0x4F)
        s_ctrl.regs[addr-0x10]=val;

    // Derive the T-cycle offset within the current frame directly from the
    // emulator's own counters: LY (0..153) * 456 + lcd_count gives the exact
    // T-cycle position.  This is zero-overhead and cycle-accurate.
    // Wall-clock time cannot be used because the emulator runs faster than
    // real-time (a 16.75 ms game frame may finish in ~2 ms), which would cause
    // all events to cluster at sample 0 and destroy the Pikachu voice.
    uint16_t cycle = 0;
    if (s_gb_ptr) {
        const uint32_t ly  = s_gb_ptr->hram_io[IO_LY];
        const uint32_t lc  = s_gb_ptr->counter.lcd_count;
        const uint32_t raw = ly * 456u + lc;
        cycle = (uint16_t)(raw < GB_FRAME_CYC ? raw : GB_FRAME_CYC - 1u);
    }

    const uint32_t w    = s_ctrl.evt_w.load(std::memory_order_relaxed);
    const uint32_t next = (w+1u)&EVT_MASK;
    if (next!=s_ctrl.evt_r.load(std::memory_order_acquire)){
        s_ctrl.events[w]={addr,val,cycle};
        s_ctrl.evt_w.store(next,std::memory_order_release);
    }
}

uint8_t audio_read(const uint8_t addr) {
    if (addr>=0x30&&addr<=0x3F) return s_ctrl.regs[addr-0x10];
    if (addr<0x10||addr>0x4F)   return 0xFFu;
    // NR52 (0xFF26): bits 7 (power) and 6-4 (always 1) come from regs[];
    // bits 3-0 (channel active) must come from the audio thread's live state.
    // regs[0x16] only tracks what the game wrote to NR52, not the runtime
    // ch.enabled flags — so without this special case every channel always
    // reads as inactive, causing games that poll NR52 after triggering a
    // channel (e.g. Oracle of Seasons/Ages) to spin forever.
    if (addr == 0x26u)
        return (s_ctrl.regs[0x16] & 0x80u) | 0x70u |
               s_ctrl.nr52_ch_bits.load(std::memory_order_acquire);
    static constexpr uint8_t MASK[0x17]={
        0x80,0x3F,0x00,0xFF,0xBF,
        0xFF,0x3F,0x00,0xFF,0xBF,
        0x7F,0xFF,0x9F,0xFF,0xBF,
        0xFF,0xFF,0x00,0x00,0xBF,
        0x00,0x00,0x70,
    };
    const uint8_t off=addr-0x10u;
    return s_ctrl.regs[off]|(off<sizeof(MASK)?MASK[off]:0u);
}

}  // extern "C"

// ─────────────────────────────────────────────────────────────────────────────
// Audio thread
// ─────────────────────────────────────────────────────────────────────────────
static constexpr int64_t GB_FRAME_NS =
    (int64_t)GB_FRAME_CYC*1'000'000'000LL/(int64_t)GB_CPU_HZ;

static void gb_audio_thread_fn(void*) {
    GBAPU local{};
    local.ch4.lfsr=0x7FFF;

    if (s_ctrl.snapshot_valid) {
        // Restore full GBAPU state from the previous session.  This is the only
        // way to correctly resume mid-game audio: the snapshot contains all the
        // runtime computed fields (ch.enabled, timer, duty_pos, current vol,
        // seq_step, lfsr, ...) that apply_reg() cannot reconstruct because it
        // deliberately ignores trigger bits.  Without ch.enabled=true the channels
        // produce silence even when all registers are correctly configured.
        local = s_ctrl.snapshot;
    } else {
        // Fresh start: restore HP capacitor state then replay saved registers.
        // On a cold boot s_ctrl.hp_ch and s_ctrl.regs are all zeros -- correct.
        // NR52 must be applied first so subsequent channel writes are not dropped
        // by the apply_reg() power gate.
        memcpy(local.hp_ch, s_ctrl.hp_ch, sizeof(local.hp_ch));

        apply_reg(local,0x26,s_ctrl.regs[0x16]);          // NR52 -- power on first
        apply_reg(local,0x24,s_ctrl.regs[0x14]);          // NR50 master vol
        apply_reg(local,0x25,s_ctrl.regs[0x15]);          // NR51 panning
        apply_reg(local,0x10,s_ctrl.regs[0x00]);          // CH1
        apply_reg(local,0x11,s_ctrl.regs[0x01]);
        apply_reg(local,0x12,s_ctrl.regs[0x02]);
        apply_reg(local,0x13,s_ctrl.regs[0x03]);
        apply_reg(local,0x14,s_ctrl.regs[0x04]&0x7F);    // no retrigger
        apply_reg(local,0x16,s_ctrl.regs[0x06]);          // CH2
        apply_reg(local,0x17,s_ctrl.regs[0x07]);
        apply_reg(local,0x18,s_ctrl.regs[0x08]);
        apply_reg(local,0x19,s_ctrl.regs[0x09]&0x7F);
        apply_reg(local,0x1A,s_ctrl.regs[0x0A]);          // CH3
        apply_reg(local,0x1B,s_ctrl.regs[0x0B]);
        apply_reg(local,0x1C,s_ctrl.regs[0x0C]);
        apply_reg(local,0x1D,s_ctrl.regs[0x0D]);
        apply_reg(local,0x1E,s_ctrl.regs[0x0E]&0x7F);
        apply_reg(local,0x20,s_ctrl.regs[0x10]);          // CH4
        apply_reg(local,0x21,s_ctrl.regs[0x11]);
        apply_reg(local,0x22,s_ctrl.regs[0x12]);
        apply_reg(local,0x23,s_ctrl.regs[0x13]&0x7F);
        for(uint8_t r=0x30;r<=0x3F;++r)                  // wave RAM
            apply_reg(local,r,s_ctrl.regs[r-0x10]);
    }

    // Bresenham: accumulates GB_SPF_FRAC_INC each frame; flips 803→804 when
    // it overflows GB_CPU_HZ, giving exactly 48000 samples/sec on average.
    uint32_t spf_acc = 0;

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC,&ts);
    int64_t next_ns=(int64_t)ts.tv_sec*1'000'000'000LL+ts.tv_nsec+GB_FRAME_NS;

    while(s_ctrl.thread_run.load(std::memory_order_relaxed)){

        // ── Bresenham sample count for this frame ─────────────────────────────
        spf_acc += GB_SPF_FRAC_INC;
        const uint32_t n_samp = GB_SPF_BASE + (spf_acc >= GB_CPU_HZ ? 1u : 0u);
        if (spf_acc >= GB_CPU_HZ) spf_acc -= GB_CPU_HZ;
        const uint32_t n_data = n_samp * 2u * static_cast<uint32_t>(sizeof(int16_t));

        if(s_ctrl.paused.load(std::memory_order_acquire)){
            // ── Paused path ───────────────────────────────────────────────────
            // Drain SPSC so stale events don't pile up.  Submit silence.
            // GBAPU state (including hp_ch) is preserved in local — no reset.
            s_ctrl.evt_r.store(
                s_ctrl.evt_w.load(std::memory_order_acquire),
                std::memory_order_release);
            memset(s_ctrl.dma[s_ctrl.cur],0,n_data);
            s_ctrl.nr52_ch_bits.store(0, std::memory_order_release);

        } else {
            // ── Active path ───────────────────────────────────────────────────
            // Collect this frame's events into s_local_evts (flat array owned
            // by the audio thread), stopping at the frame-end sentinel (0xFF).
            // Then hand them to generate_samples for proportional distribution.
            uint32_t n_evts = 0;
            {
                uint32_t r = s_ctrl.evt_r.load(std::memory_order_relaxed);
                const uint32_t w = s_ctrl.evt_w.load(std::memory_order_acquire);
                while (r != w) {
                    const RegEvent ev = s_ctrl.events[r];
                    r = (r + 1u) & EVT_MASK;
                    if (ev.addr == 0xFF) break;              // sentinel: end of frame
                    if (n_evts < LOC_CAP) s_local_evts[n_evts++] = ev; // guard overflow
                }
                s_ctrl.evt_r.store(r, std::memory_order_release);
            }

            generate_samples(local, s_ctrl.dma[s_ctrl.cur], n_samp,
                             s_local_evts, n_evts);

            // Publish the channel-active state so audio_read(0x26) can return
            // correct NR52 bits 3-0 to the emulation thread.
            s_ctrl.nr52_ch_bits.store(
                (local.ch1.enabled ? 0x01u : 0u) |
                (local.ch2.enabled ? 0x02u : 0u) |
                (local.ch3.enabled ? 0x04u : 0u) |
                (local.ch4.enabled ? 0x08u : 0u),
                std::memory_order_release);
        }

        // ── Submit ────────────────────────────────────────────────────────────
        {
            std::lock_guard<std::mutex> lk(ult::Audio::m_audioMutex);
            AudioOutBuffer* rel=nullptr; u32 cnt=0;
            audoutGetReleasedAudioOutBuffer(&rel,&cnt);  // clean up finished bufs

            AudioOutBuffer& ab=s_ctrl.ab[s_ctrl.cur];
            ab.buffer     =s_ctrl.dma[s_ctrl.cur];
            ab.buffer_size=GB_DMA_CAP;
            ab.data_size  =n_data;
            ab.data_offset=0;
            ab.next       =nullptr;
            audoutAppendAudioOutBuffer(&ab);
        }
        s_ctrl.cur=(s_ctrl.cur+1u)&3u;

        // ── Operation-mode resync (handheld ↔ docked) ────────────────────────
        // When the audio output device changes the kernel silently invalidates
        // all queued DMA buffers, desynchronising our ring.  Flush+stop+start
        // resets the hardware queue; re-queuing two silence frames restores the
        // 2-frame headroom.  GBAPU state in `local` (including hp_ch) is fully
        // preserved — no pops, no stale channel state.
        if (__builtin_expect(s_ctrl.resync.load(std::memory_order_acquire), 0)) {
            {
                std::lock_guard<std::mutex> lk(ult::Audio::m_audioMutex);
                bool _dummy = false;
                audoutFlushAudioOutBuffers(&_dummy);
                audoutStopAudioOut();
                audoutStartAudioOut();
                for (int _i = 0; _i < 2; ++_i) {
                    const uint8_t slot = (s_ctrl.cur + _i) & 3u;
                    memset(s_ctrl.dma[slot], 0, GB_DMA_CAP);
                    s_ctrl.ab[slot] = {};
                    s_ctrl.ab[slot].buffer      = s_ctrl.dma[slot];
                    s_ctrl.ab[slot].buffer_size = GB_DMA_CAP;
                    s_ctrl.ab[slot].data_size   = GB_DMA_DATA;
                    s_ctrl.ab[slot].data_offset = 0;
                    s_ctrl.ab[slot].next        = nullptr;
                    audoutAppendAudioOutBuffer(&s_ctrl.ab[slot]);
                }
                s_ctrl.cur = (s_ctrl.cur + 2u) & 3u;
            }
            // Re-anchor the drift clock so we don't spin catch-up frames.
            clock_gettime(CLOCK_MONOTONIC, &ts);
            next_ns = (int64_t)ts.tv_sec * 1'000'000'000LL + ts.tv_nsec + GB_FRAME_NS;
            s_ctrl.resync.store(false, std::memory_order_release);
        }

        // ── Sleep for remainder of frame period ───────────────────────────────
        // Skip sleep when thread_run is false so the thread exits immediately
        // rather than waiting up to 16 ms.  threadWaitForExit then returns in
        // < 1 ms, keeping gb_audio_shutdown() safe to call from handleInput.
        clock_gettime(CLOCK_MONOTONIC,&ts);
        const int64_t now_ns=(int64_t)ts.tv_sec*1'000'000'000LL+ts.tv_nsec;
        const int64_t sleep_ns=next_ns-now_ns;
        if(sleep_ns>0 && s_ctrl.thread_run.load(std::memory_order_relaxed))
            svcSleepThread(sleep_ns);
        next_ns+=GB_FRAME_NS;
        // If we've fallen behind real time by more than one frame (scheduler
        // jitter, mutex contention, generate_samples overrun), snap next_ns
        // forward to now+1 frame.  Without this clamp the thread spins through
        // back-to-back iterations with sleep_ns<=0, submitting frames faster
        // than the hardware plays them and building up unbounded queue latency.
        if(next_ns < now_ns)
            next_ns = now_ns + GB_FRAME_NS;
    }

    // Save full GBAPU state so the next thread startup can restore it exactly.
    // This covers both the pause/resume path (X -> ROM menu -> re-enter same game)
    // and the save-state path (shutdown -> save_state snapshots s_ctrl.snapshot).
    // hp_ch is included inside the snapshot struct, so the separate s_ctrl.hp_ch
    // copy is kept only for the legacy register-replay path (snapshot_valid=false).
    s_ctrl.snapshot       = local;
    s_ctrl.snapshot_valid = true;
    memcpy(s_ctrl.hp_ch, local.hp_ch, sizeof(local.hp_ch));
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_set_volume — set master output volume (0–100).
// Thread-safe: written from UI thread, read in audio thread via relaxed atomic.
static void gb_audio_set_volume(u8 percent) {
    s_master_gain.store(std::clamp(float(percent), 0.f, 100.f) / 100.f,
                        std::memory_order_relaxed);
}

// gb_audio_get_volume — read back the current volume (0–100).
static u8 gb_audio_get_volume() {
    return static_cast<u8>(s_master_gain.load(std::memory_order_relaxed) * 100.f + 0.5f);
}

// gb_audio_init — call after gb_init_lcd(), before gb_reset().
// ─────────────────────────────────────────────────────────────────────────────
static bool gb_audio_init(gb_s*) {
    if(s_ctrl.ready) return true;

    s_ctrl.evt_w.store(0,std::memory_order_relaxed);
    s_ctrl.evt_r.store(0,std::memory_order_relaxed);
    // Do NOT zero s_ctrl.regs or s_ctrl.hp_ch — they hold state from the
    // previous session and are restored by the thread on startup.
    s_ctrl.cur=0;
    s_ctrl.paused.store(false,std::memory_order_relaxed);

    // If DMA buffers were pre-allocated by gb_audio_preinit_dma(), reuse them
    // directly — do NOT free and re-allocate.  Pre-allocation happens in
    // gb_load_rom immediately after gb_unload_rom(), while the heap is clean
    // (before the large ROM malloc).  aligned_alloc(0x1000) needs a contiguous
    // free block of ~8191 bytes; after a ~2 MB ROM is malloc'd on a 4 MB heap
    // that space may not exist, causing a spurious "Not enough memory" failure
    // even though total free bytes are sufficient.  Skipping re-allocation here
    // when the buffers are already valid fixes that race.
    for(int i=0;i<4;++i){
        if(!s_ctrl.dma[i]){
            s_ctrl.dma[i]=static_cast<int16_t*>(aligned_alloc(GB_DMA_ALIGN,GB_DMA_CAP));
            if(!s_ctrl.dma[i]){
                for(int j=0;j<i;++j){free(s_ctrl.dma[j]);s_ctrl.dma[j]=nullptr;}
                return false;
            }
        }
        memset(s_ctrl.dma[i],0,GB_DMA_CAP);
    }

    if(!ult::Audio::m_initialized){
        if(R_FAILED(audoutInitialize())){
            for(int i=0;i<4;++i){free(s_ctrl.dma[i]);s_ctrl.dma[i]=nullptr;}
            return false;
        }
        s_ctrl.own_session=true;
    }

    // Stop then restart audout for exclusive, clean stream ownership.
    // When ultrahand owns the session its audio thread may still be active.
    // Stop flushes all their queued buffers so our silence pre-roll is the
    // first thing the hardware plays — no stale frames, no de-sync.
    audoutStopAudioOut();
    if(R_FAILED(audoutStartAudioOut())){
        if(s_ctrl.own_session){audoutExit();s_ctrl.own_session=false;}
        else{audoutStartAudioOut();}  // best-effort restore if Start failed
        for(int i=0;i<4;++i){free(s_ctrl.dma[i]);s_ctrl.dma[i]=nullptr;}
        return false;
    }

    // Pre-queue two silence frames for ~33 ms of headroom before the audio
    // thread generates its first real frame.
    s_ctrl.cur=2;
    {
        std::lock_guard<std::mutex> lk(ult::Audio::m_audioMutex);
        for(int i=0;i<2;++i){
            s_ctrl.ab[i]={};
            s_ctrl.ab[i].buffer     =s_ctrl.dma[i];
            s_ctrl.ab[i].buffer_size=GB_DMA_CAP;
            s_ctrl.ab[i].data_size  =GB_DMA_DATA;
            audoutAppendAudioOutBuffer(&s_ctrl.ab[i]);
        }
    }
    s_ctrl.thread_run.store(true,std::memory_order_relaxed);
    if(R_FAILED(threadCreate(&s_ctrl.thread_handle,
                              gb_audio_thread_fn,nullptr,
                              nullptr,0x1000,0x2B,-2)))
    {
        s_ctrl.thread_run.store(false);
        audoutStopAudioOut();
        if(s_ctrl.own_session){audoutExit();s_ctrl.own_session=false;}
        else{audoutStartAudioOut();}  // restore ultrahand's stream on failure
        for(int i=0;i<4;++i){free(s_ctrl.dma[i]);s_ctrl.dma[i]=nullptr;}
        return false;
    }
    threadStart(&s_ctrl.thread_handle);
    s_ctrl.ready=true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_pause — call from Overlay::onHide().
// ─────────────────────────────────────────────────────────────────────────────
static void gb_audio_pause() {
    s_ctrl.paused.store(true,std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_resume — call from Overlay::onShow() after a system-combo hide.
//
// Simply clears the paused flag.  The thread transitions from submitting
// silence to generating real audio on the very next iteration.  At most one
// silence frame (16.7 ms) was submitted ahead of the first real-audio frame —
// imperceptible.  hp_ch[] is preserved in the running thread, so there is no
// transient.  No flush or re-queue is needed.
// ─────────────────────────────────────────────────────────────────────────────
static void gb_audio_resume() {
    s_ctrl.paused.store(false, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_request_resync — call when operation mode changes (handheld ↔ docked).
//
// Sets a flag; the audio thread handles it asynchronously on its next iteration
// (≤16.7 ms).  Non-blocking; safe to call from draw() or handleInput().
// No-op when the audio thread is not running.
// ─────────────────────────────────────────────────────────────────────────────
static void gb_audio_request_resync() {
    if (s_ctrl.ready)
        s_ctrl.resync.store(true, std::memory_order_release);
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_shutdown
// ─────────────────────────────────────────────────────────────────────────────
static void gb_audio_shutdown() {
    if(!s_ctrl.ready) return;
    s_ctrl.ready=false;

    // Signal the thread to stop.  The thread skips its sleep when thread_run
    // is false, so threadWaitForExit returns in < 1 ms.  The thread writes
    // local.hp_ch back to s_ctrl.hp_ch just before returning.
    s_ctrl.thread_run.store(false,std::memory_order_release);
    threadWaitForExit(&s_ctrl.thread_handle);
    threadClose(&s_ctrl.thread_handle);

    // Under m_audioMutex so no concurrent playSound() can interfere.
    //
    // 1. audoutFlushAudioOutBuffers — releases all queued-but-not-yet-
    //    playing buffers from the kernel queue.
    // 2. audoutStopAudioOut — synchronous IPC call: hardware DMA is fully
    //    stopped when it returns.  free() is safe immediately after.
    //    DO NOT use audoutWaitPlayFinish: it only dequeues ONE buffer per
    //    call; with 2-4 buffers in the ring it returns while the hardware
    //    is still DMA-reading the next one — free() then crashes.
    //    This Flush+Stop pattern matches ultrahand's own Audio::exit().
    // 3. audoutStartAudioOut (non-own_session) — restores ultrahand's stream
    //    inside the mutex so triggerExitFeedback → playSound() immediately
    //    finds a live stream when it next acquires the mutex.
    {
        std::lock_guard<std::mutex> lk(ult::Audio::m_audioMutex);
        bool dummy=false;
        audoutFlushAudioOutBuffers(&dummy);
        audoutStopAudioOut();

        if(s_ctrl.own_session){
            audoutExit();
            s_ctrl.own_session=false;
        } else {
            audoutStartAudioOut();
        }
    }

    // DMA buffers are intentionally kept live across shutdown/init cycles.
    // Freeing and re-allocating 4× aligned_alloc(4096) on every game switch
    // creates heap holes that resist coalescing, degrading the 4MB heap over
    // successive loads until malloc(romSz) silently fails inside a Tesla 'new'.
    // Since the overlay always needs these buffers, keeping them avoids that
    // fragmentation at zero practical cost.  ab[] ring metadata is cleared so
    // gb_audio_init's pre-queue re-builds AudioOutBuffer descriptors cleanly.
    for(int i=0;i<4;++i){ s_ctrl.ab[i]={}; }
    // Buffers are released once via gb_audio_free_dma() in exitServices.
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_preinit_dma — allocate DMA buffers while the heap is clean.
//
// Call this in gb_load_rom AFTER gb_unload_rom() and BEFORE malloc(romSz).
//
// Why this exists:
//   aligned_alloc(0x1000, 0x1000) internally requests a free block of
//   ~8191 bytes so dlmalloc can guarantee 4096-byte alignment.  On a 4 MB
//   heap, after a ~2 MB ROM buffer has been malloc'd, the largest remaining
//   contiguous free region may be smaller than 8191 bytes even though total
//   free memory is sufficient.  Calling this before the ROM malloc ensures
//   the four DMA buffers land in clean, unfragmented space.
//
//   gb_audio_init() checks each dma[i] for non-null before allocating, so
//   if the buffers are already present here it simply memsets them to zero
//   and skips re-allocation entirely.
//
//   No-op if all four buffers are already allocated (e.g. isLive fast-resume
//   path where gb_audio_shutdown was never called for this session).
// ─────────────────────────────────────────────────────────────────────────────
static bool gb_audio_preinit_dma() {
    for(int i=0;i<4;++i){
        if(!s_ctrl.dma[i]){
            s_ctrl.dma[i]=static_cast<int16_t*>(aligned_alloc(GB_DMA_ALIGN,GB_DMA_CAP));
            if(!s_ctrl.dma[i]){
                // Partial allocation — roll back and report failure.
                for(int j=0;j<i;++j){free(s_ctrl.dma[j]);s_ctrl.dma[j]=nullptr;}
                return false;
            }
            memset(s_ctrl.dma[i],0,GB_DMA_CAP);
        }
    }
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_free_dma — release DMA buffers on overlay exit.
//
// DMA buffers are kept live across all game sessions (see gb_audio_shutdown)
// to eliminate aligned_alloc/free fragmentation on the 4MB heap.  This is
// the one place they are explicitly freed: from Overlay::exitServices(), after
// gb_unload_rom() has stopped the audio thread and called audoutStopAudioOut.
// ─────────────────────────────────────────────────────────────────────────────
static void gb_audio_free_dma() {
    for(int i=0;i<4;++i){free(s_ctrl.dma[i]);s_ctrl.dma[i]=nullptr;s_ctrl.ab[i]={};}
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_reset_regs -- reset APU state for a new-game / cold-boot load.
//
// Called when no saved APU state is available.  Clears the register shadow and
// invalidates the GBAPU snapshot so the audio thread does a fresh register
// replay rather than restoring stale state from a different game.
//
// hp_ch[] is intentionally NOT touched: resetting it causes a ~5ms undershoot
// transient (click) on the first generated frame.  The new game's audio
// settles the DC-blocker naturally within a couple of frames.
//
// regs[0x16] (NR52) is primed to 0x80 (APU power ON) so that apply_reg()
// accepts channel writes immediately.  For a cold boot the game will overwrite
// it during its own init sequence (no effect).  For a v1 save-state resume the
// game never re-runs init, so without this prime apply_reg() drops all channel
// writes (power=false gate) -> permanent silence.
// ─────────────────────────────────────────────────────────────────────────────
static void gb_audio_reset_regs() {
    memset(s_ctrl.regs, 0, sizeof(s_ctrl.regs));
    s_ctrl.regs[0x16]       = 0x80;   // NR52: APU power ON
    s_ctrl.snapshot_valid   = false;  // force register-replay path on next init
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_save_state / gb_audio_restore_state
//
// Persist and restore the complete GBAPU snapshot that lives outside gb_s.
// Must be called while the audio thread is NOT running (after shutdown / before
// init).  gb_audio_shutdown() causes the thread to write s_ctrl.snapshot before
// exiting, so the snapshot is fully settled when save_state() calls save here.
// ─────────────────────────────────────────────────────────────────────────────
static void gb_audio_save_state(GBAPU* dst) {
    *dst = s_ctrl.snapshot;  // snapshot_valid guaranteed true after shutdown
}

static void gb_audio_restore_state(const GBAPU* src) {
    s_ctrl.snapshot       = *src;
    s_ctrl.snapshot_valid = true;
    // Keep regs[] consistent with the snapshot for audio_read() callers.
    // Extract the handful of registers peanut_gb reads back (NR52, NR50, NR51,
    // wave RAM).  Channel registers are write-only from the game's perspective
    // so their mirror values don't matter for correctness.
    memset(s_ctrl.regs, 0, sizeof(s_ctrl.regs));
    s_ctrl.regs[0x16] = src->power ? 0x80u : 0x00u;     // NR52
    s_ctrl.regs[0x14] = src->nr50;                       // NR50
    s_ctrl.regs[0x15] = src->nr51;                       // NR51
    memcpy(&s_ctrl.regs[0x20], src->ch3.ram, 16);        // wave RAM (regs[0x20..0x2F])
}

// ─────────────────────────────────────────────────────────────────────────────
// gb_audio_submit — enqueue a frame-end sentinel after each gb_run_frame().
//
// addr=0xFF is never a real APU register offset (APU only uses 0x10–0x3F).
// The drain loop stops at this marker, so the audio thread consumes exactly
// one render frame's events per audio frame — preventing multi-frame batching
// when the render thread (vsync ≈ 60fps) briefly runs ahead of the audio
// thread (≈59.73fps).
// ─────────────────────────────────────────────────────────────────────────────
static void gb_audio_submit() {
    if (!s_ctrl.ready) return;
    const uint32_t w    = s_ctrl.evt_w.load(std::memory_order_relaxed);
    const uint32_t next = (w + 1u) & EVT_MASK;
    if (next != s_ctrl.evt_r.load(std::memory_order_acquire)) {
        s_ctrl.events[w] = {0xFF, 0, 0};
        s_ctrl.evt_w.store(next, std::memory_order_release);
    }
}