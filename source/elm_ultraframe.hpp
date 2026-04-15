/********************************************************************************
 * File: elm_ultraframe.hpp
 * Description:
 *   UltraGBOverlayFrame — single-page overlay frame for UltraGB.
 *
 *   Used by RomSelectorGui (ROMs page) and SettingsGui (Settings page).
 *     • Draws the animated "UltraGB" wave title (via draw_ultragb_title)
 *     • Shows a left-page ("ROMs") or right-page ("Settings") footer button
 *       to signal the other page; navigation is handled by swapTo<> in each
 *       Gui's handleInput — no in-place list swapping, no dual list ownership.
 *
 *   Content pointer is OWNING — the frame deletes m_contentElement in its
 *   destructor, exactly like tsl::elm::OverlayFrame does.  This means each
 *   Gui's createUI() can create a list as a plain local, hand it to the frame
 *   via setContent(), and return the frame; Tesla owns the frame via
 *   m_topElement, and everything is freed automatically when swapTo pops the
 *   Gui off the stack (swapTo destroys the old Gui → ~Gui deletes m_topElement
 *   → ~UltraGBOverlayFrame deletes list → ~List deletes all items).
 *   Only one list lives in memory at a time.
 *
 *   NO mutex — Tesla's UI path (draw/layout/requestFocus/onTouch) is
 *   single-threaded.
 *
 * Performance notes:
 *   Static-local caching (s_dimsReady / s_*) avoids three getTextDimensions
 *   calls and their associated temporary string constructions on every frame.
 *   These strings (GAP_1, GAP_2, BACK, OK) never change at runtime so their
 *   pixel widths are permanent once measured.
 *
 *   Instance-level dirty-flag caching (m_footerDirty) avoids rebuilding the
 *   footer string and re-measuring the page-label width every frame; those
 *   values only change when setPageNames() is called.
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
#include <string>
#include "gb_renderer.h"   // draw_wallpaper_direct + all rendering primitives

// draw_ultragb_title is defined in main.cpp with __attribute__((optimize("O3"))).
// Wrap the forward declaration in a matching push/O3/pop so GCC never sees a
// declaration-vs-definition attribute mismatch (-Wattributes), regardless of
// whether this header is included before or after the definition.
#pragma GCC push_options
#pragma GCC optimize("O3")
static s32 draw_ultragb_title(tsl::gfx::Renderer*, s32, s32, u32, bool);
#pragma GCC pop_options

class UltraGBOverlayFrame final : public tsl::elm::Element {
public:
    explicit UltraGBOverlayFrame(std::string pageLeftName  = "",
                                 std::string pageRightName = "")
        : tsl::elm::Element()
        , m_pageLeftName(std::move(pageLeftName))
        , m_pageRightName(std::move(pageRightName))
    {
        ult::activeHeaderHeight = 97;
        ult::loadWallpaperFileWhenSafe();
        m_isItem = false;
        disableSound.store(false, std::memory_order_release);
        // m_footerDirty = true by default — first draw() will populate the cache.
    }

    ~UltraGBOverlayFrame() override { delete m_contentElement; }

    // -----------------------------------------------------------------------
    void __attribute__((optimize("O3"))) draw(tsl::gfx::Renderer* renderer) override {
        // Optimization — skip fillScreen when wallpaper will cover the full frame.
        // draw_wallpaper_direct with no skip parameters writes all 448×720 pixels,
        // making fillScreen's ~645 KB write entirely redundant.  Prime only
        // framebuffer[0] so draw_wallpaper_direct can read bg_r/g/b/a for its
        // blend kernel; every other pixel is written by draw_wallpaper_direct.
        // Fall back to fillScreen when wallpaper will not render this frame.
        {
            const bool wallpaperWillRender =
                !ult::limitedMemory &&
                !ult::wallpaperData.empty() &&
                !ult::refreshWallpaper.load(std::memory_order_acquire) &&
                ult::correctFrameSize;
            if (wallpaperWillRender) {
                *static_cast<tsl::Color*>(renderer->getCurrentFramebuffer()) =
                    a(tsl::defaultBackgroundColor);
            } else {
                renderer->fillScreen(a(tsl::defaultBackgroundColor));
            }
        }
        draw_wallpaper_direct(renderer);

#if USING_WIDGET_DIRECTIVE
        renderer->drawWidget();
#endif

        // --- Animated title — matches IS_LAUNCHER_DIRECTIVE Ultrahand menu exactly:
        //     y=50, offset=6 → baseline 56; fontSize=42 ---
        draw_ultragb_title(renderer, 20, 56, 42, true);

        // --- Version label subtitle — matches Ultrahand main menu: y+25=75, fontSize=15 ---
        // ult::versionLabel carries the full "vX.X.X ▸ loaderTitle" string with the
        // DIVIDER_SYMBOL coloured by textSeparatorColor, exactly as OverlayFrame does.

        static const std::string versionLabel = ult::cleanVersionLabel(APP_VERSION) + " " + ult::DIVIDER_SYMBOL + " " + ult::loaderTitle + " " + ult::cleanVersionLabel(ult::loaderInfo);
        renderer->drawStringWithColoredSections(versionLabel, false,
            tsl::s_dividerSpecialChars, 20, 75, 15,
            tsl::bannerVersionTextColor, tsl::textSeparatorColor);

        // --- Bottom separator ---
        renderer->drawRect(15, tsl::cfg::FramebufferHeight - 73,
                           tsl::cfg::FramebufferWidth - 30, 1,
                           a(tsl::bottomSeparatorColor));

        // -----------------------------------------------------------------------
        // Static footer dimension cache.
        //
        // GAP_1, GAP_2, BACK, OK are runtime constants — their pixel widths
        // never change, so we measure them exactly once across all frames and
        // all instances.  The derived half-gap and button widths are stored
        // alongside so we never recompute them either.
        // -----------------------------------------------------------------------
        static bool  s_dimsReady   = false;
        static float s_gapWidth    = 0.f;
        static float s_halfGap     = 0.f;
        static float s_backWidth   = 0.f;   // backTextWidth + gapWidth
        static float s_selectWidth = 0.f;   // selTextWidth  + gapWidth

        if (!s_dimsReady) {
            s_gapWidth    = renderer->getTextDimensions(ult::GAP_1,                        false, 23).first;
            s_backWidth   = renderer->getTextDimensions("\uE0E1" + ult::GAP_2 + ult::BACK, false, 23).first + s_gapWidth;
            s_selectWidth = renderer->getTextDimensions("\uE0E0" + ult::GAP_2 + ult::OK,   false, 23).first + s_gapWidth;
            s_halfGap     = s_gapWidth * 0.5f;
            s_dimsReady   = true;
        }

        // Sync shared atomics — updateAtomic short-circuits after the first
        // frame once the values stabilise, so this is just 3 atomic loads.
        const auto updateAtomic = [](std::atomic<float>& atom, float val) {
            if (val != atom.load(std::memory_order_acquire))
                atom.store(val, std::memory_order_release);
        };
        updateAtomic(ult::halfGap,     s_halfGap);
        updateAtomic(ult::backWidth,   s_backWidth);
        updateAtomic(ult::selectWidth, s_selectWidth);

        // buttonY is derived from FramebufferHeight which never changes.
        static const float s_buttonY =
            static_cast<float>(tsl::cfg::FramebufferHeight - 73 + 1);
        static constexpr float buttonStartX = 30.f;

        // -----------------------------------------------------------------------
        // Instance-level footer cache.
        //
        // Rebuilt only when setPageNames() is called (or on the very first
        // draw).  Avoids per-frame string concatenation and an additional
        // getTextDimensions call for the page label.
        // -----------------------------------------------------------------------
        if (m_footerDirty) {
            m_hasNextPage = !m_pageLeftName.empty() || !m_pageRightName.empty();

            if (m_hasNextPage) {
                m_cachedPageLabel = !m_pageLeftName.empty()
                    ? ("\uE0ED" + ult::GAP_2 + m_pageLeftName)
                    : ("\uE0EE" + ult::GAP_2 + m_pageRightName);
                // Page label width requires the renderer — measure it once here.
                m_cachedPageLabelWidth =
                    renderer->getTextDimensions(m_cachedPageLabel, false, 23).first + s_gapWidth;

                m_cachedFooterString =
                    "\uE0E1" + ult::GAP_2 + ult::BACK + ult::GAP_1 +
                    "\uE0E0" + ult::GAP_2 + ult::OK   + ult::GAP_1 +
                    m_cachedPageLabel + ult::GAP_1;
            } else {
                m_cachedPageLabel      = {};
                m_cachedPageLabelWidth = 0.f;
                m_cachedFooterString   =
                    "\uE0E1" + ult::GAP_2 + ult::BACK + ult::GAP_1 +
                    "\uE0E0" + ult::GAP_2 + ult::OK   + ult::GAP_1;
            }

            // Sync page-related atomics while we already know the new values.
            ult::hasNextPageButton.store(m_hasNextPage, std::memory_order_release);
            ult::nextPageWidth.store(m_cachedPageLabelWidth, std::memory_order_release);

            m_footerDirty = false;
        }

        // --- Page navigation button ---
        if (m_hasNextPage) {
            // nextPageWidth is already up to date (set in the dirty block above).
            if (ult::touchingNextPage.load(std::memory_order_acquire)) {
                const float nextX = buttonStartX + 2.f - s_halfGap + s_backWidth + 1.f + s_selectWidth;
                renderer->drawRoundedRect(nextX, s_buttonY, m_cachedPageLabelWidth - 2.f, 73.0f, 12.0f,
                                          a(tsl::clickColor));
            }
        }

        // --- Touch highlights ---
        if (ult::touchingBack)
            renderer->drawRoundedRect(buttonStartX + 2.f - s_halfGap, s_buttonY,
                                      s_backWidth - 1.f, 73.0f, 12.0f, a(tsl::clickColor));
        if (ult::touchingSelect.load(std::memory_order_acquire))
            renderer->drawRoundedRect(buttonStartX + 2.f - s_halfGap + s_backWidth + 1.f,
                                      s_buttonY, s_selectWidth - 2.f, 73.0f, 12.0f,
                                      a(tsl::clickColor));

        // --- Footer text ---
        renderer->drawStringWithColoredSections(m_cachedFooterString, false,
            tsl::s_footerSpecialChars, buttonStartX, 693, 23,
            tsl::bottomTextColor, tsl::buttonColor);

        if (!selectIsUsingFocusedColor) {
            static const std::string okOverdraw = "\uE0E0" + ult::GAP_2 + ult::OK + ult::GAP_1;
            renderer->drawStringWithColoredSections(okOverdraw, false, tsl::s_footerSpecialChars,
                buttonStartX + s_backWidth, 693, 23,
                tsl::unfocusedColor, tsl::unfocusedColor);
        }

        // --- Content ---
        if (m_contentElement != nullptr)
            m_contentElement->frame(renderer);

        // --- Edge separator ---
        if (!ult::useRightAlignment)
            renderer->drawRect(447, 0, 448, 720, a(tsl::edgeSeparatorColor));
        else
            renderer->drawRect(0, 0, 1, 720, a(tsl::edgeSeparatorColor));
    }

    // -----------------------------------------------------------------------
    void layout(u16 parentX, u16 parentY, u16 parentWidth, u16 parentHeight) override {
        setBoundaries(parentX, parentY, parentWidth, parentHeight);
        if (m_contentElement != nullptr) {
            m_contentElement->setBoundaries(parentX + 35, parentY + 97,
                                            parentWidth - 85, parentHeight - 73 - 105);
            m_contentElement->invalidate();
        }
    }

    tsl::elm::Element* requestFocus(tsl::elm::Element* oldFocus,
                                    tsl::FocusDirection direction) override {
        return m_contentElement
            ? m_contentElement->requestFocus(oldFocus, direction)
            : nullptr;
    }

    bool onTouch(tsl::elm::TouchEvent event, s32 currX, s32 currY,
                 s32 prevX, s32 prevY, s32 initialX, s32 initialY) override {
        if (!m_contentElement || !m_contentElement->inBounds(currX, currY))
            return false;
        return m_contentElement->onTouch(event, currX, currY,
                                         prevX, prevY, initialX, initialY);
    }

    // -----------------------------------------------------------------------
    /**
     * Set the visible content element — OWNING.
     * The frame takes ownership; the previous content element (if any) is
     * deleted.  In normal usage createUI() calls this exactly once, so the
     * delete-old path is just a safety net.
     */
    void setContent(tsl::elm::Element* content) {
        delete m_contentElement;   // safe on nullptr; drops any previous owner
        m_contentElement = content;
        if (content != nullptr) {
            m_contentElement->setParent(this);
            invalidate();
        }
    }

    /** Update footer page-nav labels.  Empty string = hide that button. */
    void setPageNames(std::string left, std::string right) {
        m_pageLeftName  = std::move(left);
        m_pageRightName = std::move(right);
        m_footerDirty   = true;   // trigger cache rebuild on next draw()
    }

    tsl::elm::Element* getContent() const { return m_contentElement; }

private:
    tsl::elm::Element* m_contentElement = nullptr;  ///< Owning — deleted in destructor.
    std::string        m_pageLeftName;
    std::string        m_pageRightName;

    // -----------------------------------------------------------------------
    // Instance footer cache — rebuilt whenever m_footerDirty is true.
    // -----------------------------------------------------------------------
    bool        m_footerDirty         = true;  ///< True on construction and after setPageNames().
    bool        m_hasNextPage         = false;
    std::string m_cachedPageLabel;              ///< "\uE0ED/<\uE0EE> GAP_2 <name>", or empty.
    std::string m_cachedFooterString;           ///< Full bottom-line string passed to drawStringWithColoredSections.
    float       m_cachedPageLabelWidth = 0.f;   ///< getTextDimensions(m_cachedPageLabel) + gapWidth.
};
#pragma GCC pop_options