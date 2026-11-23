// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Theme.h"
#include "platform/SystemAccessibility.h"

namespace elsewhere::A11y
{
//==============================================================================
/** The system's accessibility settings, read once a second by the app (see refresh) so that
    painting code can check them cheaply. */
struct Settings
{
    bool reduceMotion = false, moreContrast = false, screenReader = false;

    bool operator== (const Settings& o) const
    {
        return reduceMotion == o.reduceMotion && moreContrast == o.moreContrast && screenReader == o.screenReader;
    }
};

inline Settings& settings()     { static Settings s; return s; }

/** Reads the system settings again. Returns true if any of them changed. */
inline bool refresh()
{
    const Settings now { systemPrefersReducedMotion(), systemPrefersMoreContrast(), systemScreenReaderRunning() };
    const bool changed = ! (now == settings());
    settings() = now;
    return changed;
}

/** Decorative motion (flashing, pulsing) should hold still. Meters and the playhead still move. */
inline bool reduceMotion()      { return settings().reduceMotion; }

/** A screen reader is running: Tab then always moves between controls. */
inline bool screenReader()      { return settings().screenReader; }

/** Says something through the screen reader, if one is running (otherwise does nothing). */
inline void announce (const juce::String& text,
                      juce::AccessibilityHandler::AnnouncementPriority priority = juce::AccessibilityHandler::AnnouncementPriority::medium)
{
    if (text.isNotEmpty())
        juce::AccessibilityHandler::postAnnouncement (text, priority);
}

/** True while someone is moving between controls with the keyboard (F6, Tab), so focus is shown.
    A mouse click ends it. */
inline bool& keyboardNavigating()   { static bool b = false; return b; }

/** Sets a component's accessible name and, optionally, its description. */
inline void label (juce::Component& c, const juce::String& title, const juce::String& description = {})
{
    if (c.getTitle() != title)
        c.setTitle (title);

    if (c.getDescription() != description)
        c.setDescription (description);
}

//==============================================================================
/** An accessibility handler for a custom-painted component: its name, description, value and
    state come from functions, so they're always current. */
class Handler : public juce::AccessibilityHandler
{
public:
    struct Info
    {
        std::function<juce::String()> title, description, value;
        std::function<juce::AccessibleState (juce::AccessibleState)> state;
    };

    Handler (juce::Component& c, juce::AccessibilityRole role, Info i, juce::AccessibilityActions actions = {})
        : juce::AccessibilityHandler (c, role, std::move (actions), makeInterfaces (i)), info (std::move (i)) {}

    juce::String getTitle() const override          { return info.title ? info.title() : juce::AccessibilityHandler::getTitle(); }
    juce::String getDescription() const override    { return info.description ? info.description() : juce::AccessibilityHandler::getDescription(); }

    juce::AccessibleState getCurrentState() const override
    {
        auto s = juce::AccessibilityHandler::getCurrentState();
        return info.state ? info.state (s) : s;
    }

private:
    struct TextValue : public juce::AccessibilityTextValueInterface
    {
        explicit TextValue (std::function<juce::String()> f) : get (std::move (f)) {}
        bool isReadOnly() const override                        { return true; }
        juce::String getCurrentValueAsString() const override   { return get(); }
        void setValueAsString (const juce::String&) override    {}
        std::function<juce::String()> get;
    };

    static Interfaces makeInterfaces (const Info& i)
    {
        if (! i.value)
            return {};

        return Interfaces { std::unique_ptr<juce::AccessibilityValueInterface> (new TextValue (i.value)) };
    }

    Info info;
};

//==============================================================================
/** Draws a ring around whatever has keyboard focus while someone is moving through the controls
    with the keyboard. Add it on top of a window's content; it takes no clicks and screen readers
    skip it. */
class FocusRing : public juce::Component,
                  private juce::FocusChangeListener,
                  private juce::Timer
{
public:
    FocusRing()
    {
        setInterceptsMouseClicks (false, false);
        setAlwaysOnTop (true);
        auto& desktop = juce::Desktop::getInstance();
        desktop.addFocusChangeListener (this);
        desktop.addGlobalMouseListener (this);
        startTimerHz (10);   // follows the control when its view scrolls or relays out
    }

    ~FocusRing() override
    {
        auto& desktop = juce::Desktop::getInstance();
        desktop.removeGlobalMouseListener (this);
        desktop.removeFocusChangeListener (this);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return createIgnoredAccessibilityHandler (*this);
    }

    void paint (juce::Graphics& g) override
    {
        if (area.isEmpty())
            return;

        // Ink, which reads at well over 3:1 on every surface, with a background-coloured halo so
        // it stays clear of a filled key
        const auto corner = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f;
        g.setColour (Colours::background);
        g.drawRoundedRectangle (area.reduced (2.0f), juce::jmax (0.0f, corner - 2.0f), 1.5f);
        g.setColour (Colours::accent);
        g.drawRoundedRectangle (area, corner, 2.0f);
    }

    // From the global mouse listener: a click means the mouse is in charge again
    void mouseDown (const juce::MouseEvent&) override
    {
        keyboardNavigating() = false;
        update();
    }

private:
    juce::Rectangle<float> area;

    void globalFocusChanged (juce::Component*) override     { update(); }
    void timerCallback() override                           { if (keyboardNavigating() || ! area.isEmpty()) update(); }

    void update()
    {
        juce::Rectangle<float> r;
        auto* parent = getParentComponent();
        auto* focused = juce::Component::getCurrentlyFocusedComponent();

        if (keyboardNavigating() && parent != nullptr && focused != nullptr && focused != parent
             && parent->isParentOf (focused) && focused->isShowing())
        {
            // Only the part of it that shows (it may be scrolled partly out of a view)
            auto bounds = getLocalArea (focused, focused->getLocalBounds());

            for (auto* p = focused->getParentComponent(); p != nullptr && p != parent; p = p->getParentComponent())
                bounds = bounds.getIntersection (getLocalArea (p, p->getLocalBounds()));

            if (! bounds.isEmpty())
                r = bounds.toFloat().expanded (2.5f);
        }

        if (r != area)
        {
            repaint (area.getSmallestIntegerContainer().expanded (2));
            area = r;
            repaint (area.getSmallestIntegerContainer().expanded (2));
        }
    }
};

//==============================================================================
/** JUCE's scroll views take keyboard focus, but they have nothing to say or do for someone
    moving through the controls, so F6 and Tab pass over them. */
inline bool isScrollView (juce::Component* c)     { return dynamic_cast<juce::Viewport*> (c) != nullptr; }

/** Moves keyboard focus into the next (or previous) area of the window that has controls: the
    transport, the tracks, the mixer, the device panel and so on. Returns false if none has any. */
inline bool moveToNextRegion (const std::vector<juce::Component*>& regions, bool forwards)
{
    std::vector<juce::Component*> shown;

    for (auto* r : regions)
        if (r != nullptr && r->isShowing())
            shown.push_back (r);

    if (shown.empty())
        return false;

    auto* focused = juce::Component::getCurrentlyFocusedComponent();
    int current = -1;

    for (int i = 0; i < (int) shown.size(); ++i)
        if (focused != nullptr && (shown[(size_t) i] == focused || shown[(size_t) i]->isParentOf (focused)))
            current = i;

    const int n = (int) shown.size();

    for (int step = 1; step <= n; ++step)
    {
        const int index = current < 0 ? (forwards ? step - 1 : n - step)
                                      : ((current + (forwards ? step : -step)) % n + n) % n;
        auto* region = shown[(size_t) index];
        auto all = juce::KeyboardFocusTraverser().getAllComponents (region);

        all.erase (std::remove_if (all.begin(), all.end(), [] (juce::Component* c) { return ! c->isShowing() || isScrollView (c); }), all.end());

        if (! all.empty())
        {
            keyboardNavigating() = true;
            all.front()->grabKeyboardFocus();
            announce (region->getTitle(), juce::AccessibilityHandler::AnnouncementPriority::low);
            return true;
        }
    }

    return false;
}

//==============================================================================
/** Tab and Shift-Tab move between controls once someone is navigating with the keyboard (or a
    screen reader is running); otherwise Tab keeps its shortcut. Escape goes back to the main
    view, where the single-key shortcuts work. Add it to the window after the command manager's
    key mappings, so it's asked first. */
class KeyboardNavigation : public juce::KeyListener
{
public:
    std::function<juce::Component*()> getHome;          // the main view, which has focus when no control does
    std::function<bool()> isEscapeTaken;                // e.g. Esc is leaving a mapping mode instead

    bool keyPressed (const juce::KeyPress& k, juce::Component*) override
    {
        auto* home = getHome ? getHome() : nullptr;
        auto* focused = juce::Component::getCurrentlyFocusedComponent();

        if (home == nullptr || focused == nullptr || focused == home || ! home->isParentOf (focused))
            return false;

        const auto mods = k.getModifiers();

        if (k.isKeyCode (juce::KeyPress::tabKey) && ! (mods.isCommandDown() || mods.isAltDown() || mods.isCtrlDown())
             && (keyboardNavigating() || screenReader()))
        {
            keyboardNavigating() = true;
            focused->moveKeyboardFocusToSibling (! mods.isShiftDown());

            for (int i = 0; i < 4; ++i)
                if (auto* now = juce::Component::getCurrentlyFocusedComponent(); now != nullptr && isScrollView (now))
                    now->moveKeyboardFocusToSibling (! mods.isShiftDown());

            return true;
        }

        if (k == juce::KeyPress::escapeKey && keyboardNavigating() && ! (isEscapeTaken && isEscapeTaken()))
        {
            keyboardNavigating() = false;
            home->grabKeyboardFocus();
            return true;
        }

        return false;
    }
};

} // namespace elsewhere::A11y
