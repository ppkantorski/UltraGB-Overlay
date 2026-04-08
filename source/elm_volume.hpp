/********************************************************************************
 * File: elm_volume.hpp
 * VolumeTrackBar
 *
 * Extends TrackBar with two behaviours:
 *
 *  1. Mute visualisation — when volume == 0, draws the speaker glyph
 *     left-half only (via scissor) plus a cross glyph in the right half,
 *     giving a clear muted state without any overdraw artefacts.
 *
 *  2. Icon tap → mute/unmute — tapping the speaker icon fires a callback
 *     (set via setIconTapCallback) so the caller can toggle mute and
 *     preserve the pre-mute volume level.
 *
 *  3. setLabel() — lets update() relabel the slider live when the running
 *     title ID changes without rebuilding the whole list.
 * 
 *  Licensed under GPLv2
 *  Copyright (c) 2026 ppkantorski
 ********************************************************************************/


#pragma once

// Cold UI path — only active when the overlay menu is open.
// Os keeps these lean; no per-frame hot paths exist in this file.
#pragma GCC push_options
#pragma GCC optimize("Os")

#include <tesla.hpp>
#include <functional>
#include <algorithm>

// =============================================================================

// =============================================================================

class VolumeTrackBar final : public tsl::elm::TrackBar {
public:
    using TrackBar::TrackBar;

    // -----------------------------------------------------------------------
    // draw — muted state shows half-speaker + cross instead of full icon.
    // -----------------------------------------------------------------------
    void draw(tsl::gfx::Renderer *renderer) override {
        if (this->m_value > 0 || this->m_icon == nullptr || this->m_icon[0] == '\0') {
            TrackBar::draw(renderer);
            return;
        }

        // --- muted path ---
        constexpr s32 kIconSize  = 30;
        constexpr s32 kIconX     = 42;   // TrackBar hardcodes getX()+42
        constexpr s32 kIconBaseY = 54;   // TrackBar hardcodes getY()+54 as baseline

        const s32 iconX    = this->getX() + kIconX;
        const s32 iconBase = this->getY() + kIconBaseY;

        // Cache speaker-glyph dimensions in plain static floats.
        // Avoids the __cxa_guard_acquire/release overhead that static const auto
        // structured bindings require.  Single-threaded UI path — no races.
        static float s_glyphW = 0.f, s_glyphH = 0.f;
        if (s_glyphW == 0.f) {
            const auto d = renderer->getTextDimensions("\uE13C", false, kIconSize);
            s_glyphW = d.first;
            s_glyphH = d.second;
        }
        const s32 glyphW   = static_cast<s32>(s_glyphW);
        const s32 glyphH   = static_cast<s32>(s_glyphH);
        const s32 glyphTop = iconBase - glyphH;

        // 1. Draw everything EXCEPT the icon by temporarily clearing m_icon.
        const char *savedIcon = this->m_icon;
        this->m_icon = " ";
        TrackBar::draw(renderer);
        this->m_icon = savedIcon;

        // 2. Draw only the LEFT half of the speaker glyph under a scissor so
        //    the right half is geometrically never written — no background rect,
        //    no overdraw artefacts.
        const s32 scissorX = iconX;
        const s32 scissorY = std::max(glyphTop,
                                      static_cast<s32>(ult::activeHeaderHeight));
        const s32 scissorW = glyphW / 2;
        const s32 scissorH = std::min(glyphTop + glyphH,
                                      static_cast<s32>(tsl::cfg::FramebufferHeight) - 73)
                             - scissorY;

        renderer->enableScissoring(scissorX, scissorY, scissorW, scissorH);
        renderer->drawString("\uE13C", false, iconX, iconBase, kIconSize,
                             ((!this->m_focused || !ult::useSelectionText) ? tsl::defaultTextColor : tsl::selectedTextColor));
        renderer->disableScissoring();

        // 3. Cross centred in the right-half region — drawn outside any scissor.
        const s32 rightX    = iconX + glyphW / 2;
        const s32 rightW    = glyphW - glyphW / 2;
        const s32 crossSize = std::max(8, glyphH * 45 / 100);
        // Cache cross-glyph dimensions. crossSize is derived from glyphH which
        // is itself constant, so this is safe to cache with a plain static float.
        static float s_crossW = 0.f, s_crossH = 0.f;
        if (s_crossW == 0.f) {
            const auto cd = renderer->getTextDimensions("\uE14C", false, crossSize);
            s_crossW = cd.first;
            s_crossH = cd.second;
        }
        const s32 crossX    = rightX + (rightW - static_cast<s32>(s_crossW)) / 2;
        const s32 crossY    = (iconBase - glyphH / 2) + static_cast<s32>(s_crossH) / 2 + 2;

        renderer->drawString("\uE14C", false, crossX, crossY, crossSize,
                             ((!this->m_focused || !ult::useSelectionText) ? tsl::defaultTextColor : tsl::selectedTextColor));
    }

    // -----------------------------------------------------------------------
    // onTouch — tapping the speaker icon fires m_iconTapCallback.
    // The icon is drawn at getX()+42, baseline getY()+54, fontSize 30.
    // A generous hit area is used so the tap is easy to land.
    // -----------------------------------------------------------------------
    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY,
                 s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
        if (m_iconTapCallback) {
            const bool inIcon =
                (initialX >= this->getX() + 30 && initialX <= this->getX() + 75 &&
                 initialY >= this->getY() + 22  && initialY <= this->getY() + 65);
            if (inIcon) {
                if (event == tsl::elm::TouchEvent::Touch) {
                    touchInSliderBounds = true;
                    triggerNavigationFeedback();
                } else if (event == tsl::elm::TouchEvent::Release) {
                    touchInSliderBounds = false;
                    const bool stillInIcon =
                        (currX >= this->getX() + 20 && currX <= this->getX() + 85 &&
                         currY >= this->getY() + 12  && currY <= this->getY() + 75);
                    if (stillInIcon) {
                        m_iconTapCallback();
                        tsl::shiftItemFocus(this);
                        triggerNavigationFeedback();
                    }
                }
                return true; // consume so the slider doesn't also react
            }
        }
        return TrackBar::onTouch(event, currX, currY, prevX, prevY, initialX, initialY);
    }

    // -----------------------------------------------------------------------
    // setIconTapCallback — called when the speaker icon is tapped.
    // -----------------------------------------------------------------------
    void setIconTapCallback(std::function<void()> cb) {
        m_iconTapCallback = std::move(cb);
    }

    // setLabel — lets update() relabel the slider live when the running
    // title ID changes without rebuilding the whole settings list.
    // Takes by value so callers passing a temporary get a move, not a copy.
    void setLabel(std::string label) { m_label = std::move(label); }

    // matchesJumpCriteria — allows jumpToItem() to locate this slider by its
    // label.  TrackBar inherits Element's stub (always false) rather than
    // ListItem's text-matching override, so without this, jumpToItem("Game Boy")
    // and jumpToItem("Active Title") silently skip the sliders every time.
    bool matchesJumpCriteria(const std::string& jumpText,
                             const std::string& /*jumpValue*/,
                             bool exactMatch) const override {
        if (jumpText.empty()) return false;
        return exactMatch ? (m_label == jumpText)
                          : (m_label.find(jumpText) != std::string::npos);
    }

private:
    std::function<void()> m_iconTapCallback;
};


// =============================================================================
// AudioBalanceTrackBar — per-game GB-audio balance trim (−150% … 0% … +150%).
//
// Internal m_value is the direct offset from m_minValue (= 0 at far left,
// 300 at far right), so each key/touch step is exactly 1 percentage point.
//
//   m_value   0  →  display "−150%"  (2^−3  = 0.125× gain)
//   m_value 150  →  display    "0%"  (2^ 0  = 1.000× gain, neutral)
//   m_value 300  →  display "+150%"  (2^+3  = 8.000× gain)
//
//   dispVal = −150 + m_value        (minValue=−150, maxValue=+150)
//
// KEY_Y while focused → reset to m_value 150 (0% balance); handled in
// GameSettingsGui::handleInput().
// =============================================================================
class AudioBalanceTrackBar final : public tsl::elm::TrackBar {
public:
    static constexpr int kMinBalance = -150;
    static constexpr int kMaxBalance =  150;

    AudioBalanceTrackBar(const char icon[3], bool usingStepTrackbar,
                         bool usingNamedStepTrackbar, bool useV2Style,
                         const std::string& label, const std::string& units,
                         bool unlockedTrackbar)
        : TrackBar(icon, usingStepTrackbar, usingNamedStepTrackbar,
                   useV2Style, label, units, unlockedTrackbar)
    {
        // m_minValue / m_maxValue are protected TrackBar members (s16).
        // The slider span becomes (300), giving 301 positions — one per 1%.
        m_minValue = static_cast<s16>(kMinBalance);
        m_maxValue = static_cast<s16>(kMaxBalance);
        // "+" prefix for positive values is automatic when m_minValue < 0.
    }

    // -----------------------------------------------------------------------
    // Convert between the raw m_value (0–300) and the balance domain (−150…+150).
    //   sliderToBalance(0)   = −150
    //   sliderToBalance(150) =    0
    //   sliderToBalance(300) = +150
    // -----------------------------------------------------------------------
    static int16_t sliderToBalance(u16 raw) {
        return static_cast<int16_t>(kMinBalance + static_cast<int>(raw));
    }
    static u16 balanceToSlider(int16_t b) {
        const int clamped = std::clamp(static_cast<int>(b), kMinBalance, kMaxBalance);
        return static_cast<u16>(clamped - kMinBalance);
    }
};

#pragma GCC pop_options