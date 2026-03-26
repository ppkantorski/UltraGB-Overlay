/********************************************************************************
 * File: elm_ultraframe.hpp
 * Description:
 *   UltraGBOverlayFrame — single-page overlay frame for UltraGB.
 *
 *   Used by RomSelectorGui (ROMs page) and SettingsGui (Settings page).
 *     • Draws the animated "UltraGB" wave title (via draw_ultraboy_title)
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
 ********************************************************************************/

#pragma once

#include <tesla.hpp>
#include <string>
#include <cmath>

// draw_ultraboy_title is defined in main.cpp (static, visible here because
// this header is included after the definition).
// Forward-declare the signature so the compiler is happy if included earlier.
static s32 draw_ultraboy_title(tsl::gfx::Renderer*, s32, s32, u32);

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
    }

    ~UltraGBOverlayFrame() override { delete m_contentElement; }

    // -----------------------------------------------------------------------
    void draw(tsl::gfx::Renderer* renderer) override {
        renderer->fillScreen(a(tsl::defaultBackgroundColor));
        renderer->drawWallpaper();

#if USING_WIDGET_DIRECTIVE
        renderer->drawWidget();
#endif

        // --- Animated title ---
        draw_ultraboy_title(renderer, 20, 67, 50);

        // --- Bottom separator ---
        renderer->drawRect(15, tsl::cfg::FramebufferHeight - 73,
                           tsl::cfg::FramebufferWidth - 30, 1,
                           a(tsl::bottomSeparatorColor));

        // --- Footer width calculations (mirrors SysTuneOverlayFrame) ---
        const auto updateAtomic = [](std::atomic<float>& atom, float val) {
            if (val != atom.load(std::memory_order_acquire))
                atom.store(val, std::memory_order_release);
        };

        const float gapWidth      = renderer->getTextDimensions(ult::GAP_1,                        false, 23).first;
        const float backTextWidth = renderer->getTextDimensions("\uE0E1" + ult::GAP_2 + ult::BACK, false, 23).first;
        const float selTextWidth  = renderer->getTextDimensions("\uE0E0" + ult::GAP_2 + ult::OK,   false, 23).first;

        const float _halfGap     = gapWidth * 0.5f;
        const float _backWidth   = backTextWidth + gapWidth;
        const float _selectWidth = selTextWidth  + gapWidth;

        updateAtomic(ult::halfGap,     _halfGap);
        updateAtomic(ult::backWidth,   _backWidth);
        updateAtomic(ult::selectWidth, _selectWidth);

        static constexpr float buttonStartX = 30.f;
        const float buttonY = static_cast<float>(tsl::cfg::FramebufferHeight - 73 + 1);

        // --- Page navigation button ---
        const bool hasNextPage = !m_pageLeftName.empty() || !m_pageRightName.empty();
        if (hasNextPage != ult::hasNextPageButton.load(std::memory_order_acquire))
            ult::hasNextPageButton.store(hasNextPage, std::memory_order_release);

        if (hasNextPage) {
            const std::string pageLabel = !m_pageLeftName.empty()
                ? ("\uE0ED" + ult::GAP_2 + m_pageLeftName)
                : ("\uE0EE" + ult::GAP_2 + m_pageRightName);

            const float _nextPageWidth =
                renderer->getTextDimensions(pageLabel, false, 23).first + gapWidth;
            updateAtomic(ult::nextPageWidth, _nextPageWidth);

            if (ult::touchingNextPage.load(std::memory_order_acquire)) {
                const float nextX = buttonStartX + 2.f - _halfGap + _backWidth + 1.f + _selectWidth;
                renderer->drawRoundedRect(nextX, buttonY, _nextPageWidth - 2.f, 73.0f, 12.0f,
                                          a(tsl::clickColor));
            }
        } else {
            ult::nextPageWidth.store(0.0f, std::memory_order_release);
        }

        // --- Touch highlights ---
        if (ult::touchingBack)
            renderer->drawRoundedRect(buttonStartX + 2.f - _halfGap, buttonY,
                                      _backWidth - 1.f, 73.0f, 12.0f, a(tsl::clickColor));
        if (ult::touchingSelect.load(std::memory_order_acquire))
            renderer->drawRoundedRect(buttonStartX + 2.f - _halfGap + _backWidth + 1.f,
                                      buttonY, _selectWidth - 2.f, 73.0f, 12.0f,
                                      a(tsl::clickColor));

        // --- Footer text ---
        const std::string currentBottomLine =
            "\uE0E1" + ult::GAP_2 + ult::BACK  + ult::GAP_1 +
            "\uE0E0" + ult::GAP_2 + ult::OK    + ult::GAP_1 +
            (!m_pageLeftName.empty()  ? "\uE0ED" + ult::GAP_2 + m_pageLeftName  + ult::GAP_1 :
             !m_pageRightName.empty() ? "\uE0EE" + ult::GAP_2 + m_pageRightName + ult::GAP_1 : "");

        renderer->drawStringWithColoredSections(currentBottomLine, false,
            tsl::s_footerSpecialChars, buttonStartX, 693, 23,
            tsl::bottomTextColor, tsl::buttonColor);

        if (!usingUnfocusedColor) {
            static const std::string okOverdraw = "\uE0E0" + ult::GAP_2 + ult::OK + ult::GAP_1;
            renderer->drawStringWithColoredSections(okOverdraw, false, tsl::s_footerSpecialChars,
                buttonStartX + _backWidth, 693, 23,
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
    }

    tsl::elm::Element* getContent() const { return m_contentElement; }

private:
    tsl::elm::Element* m_contentElement = nullptr;  ///< Owning — deleted in destructor.
    std::string        m_pageLeftName;
    std::string        m_pageRightName;
};