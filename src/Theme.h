// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/** A colour theme. The names match the "App Key" labels on the Color Theme boards in the
    Elsewhere Design System Penpot file, so a theme designed there can be copied in here. */
struct Palette
{
    const char* id;
    bool isLight;

    juce::uint32 background, panel, panelLight, panelDark, display, outline, gridLine, gridBar,
                 text, textDim, textInverse,
                 accent, playhead, record, solo, mute, modulation,
                 meterGreen, meterYellow, meterRed,
                 sendPost, sendPre, deviceAmp, deviceCymbal;

    std::array<juce::uint32, 12> clips;   // track / clip palette; drum sections use entries of it
};

namespace Themes
{
    /** Orbit, as on Somewhere: flat greys, ink (the text colour) for whatever is selected or on,
        and red only for recording. Track colours are the only other colour: twelve pigments, the
        same hues in both themes (deepened here, lifted at night), in the order of the stored
        palette (EngineHelpers::trackColourFor). Entries that share a value in one theme share it in
        the other too, so switching theme can remap colours already set on components.

        Contrast (WCAG 2.1 AA): textDim reads at 4.5:1 or more on every surface, and the light
        theme's red carries its inverse-ink label at 4.5:1. Check with Theme::contrast after a change. */
    inline const Palette light {
        "light", true,
        0xffededea, 0xffe2e2de, 0xffd5d5d0, 0xffe6e6e2, 0xffededea, 0xffd5d5d0, 0xffe6e6e2, 0xffc2c2bc,
        0xff111112, 0xff5c5c58, 0xffededea,
        0xff111112, 0xff111112, 0xffc13c32, 0xff111112, 0xff111112, 0xff5c5c58,
        0xff5c5c58, 0xff111112, 0xffc13c32,
        0xff111112, 0xff5c5c58, 0xff957260, 0xffb08a2f,
        { 0xffc26b3d, 0xffb08a2f, 0xff73903f, 0xff3f8f70, 0xff4a84b8, 0xff7c69c2,
          0xffa562a7, 0xffb9667c, 0xff957260, 0xff3a8f96, 0xff7c7e78, 0xff5a6cc4 } };

    inline const Palette dark {
        "dark", false,
        0xff0b0b0c, 0xff141416, 0xff1f1f22, 0xff101012, 0xff0b0b0c, 0xff1f1f22, 0xff101012, 0xff2e2e33,
        0xfff1f1ee, 0xff8a8a90, 0xff0b0b0c,
        0xfff1f1ee, 0xfff1f1ee, 0xffe5483c, 0xfff1f1ee, 0xfff1f1ee, 0xff8a8a90,
        0xff8a8a90, 0xfff1f1ee, 0xffe5483c,
        0xfff1f1ee, 0xff8a8a90, 0xffb5937d, 0xffcfaa55,
        { 0xffd9875a, 0xffcfaa55, 0xff97ae66, 0xff5fae90, 0xff6aa3d3, 0xff9d8bda,
          0xffc385c4, 0xffd58a9e, 0xffb5937d, 0xff5bafb5, 0xff9a9c96, 0xff7d8edb } };

    /** The pigments' names, in palette order, for the colour menus. */
    inline const char* const colourNames[12] { "Ember", "Ochre", "Moss", "Jade", "Sky", "Iris",
                                               "Orchid", "Rose", "Clay", "Teal", "Stone", "Cobalt" };
}

/** The current theme's colours. They change when the theme does (see Theme::apply), so read
    them when painting rather than caching them. */
namespace Colours
{
    inline juce::Colour background, panel, panelLight, panelDark, display, outline, gridLine, gridBar,
                        text, textDim,
                        textInverse,   // text on accent / clip colours
                        accent,        // play / selection
                        playhead, record, solo, mute, modulation,
                        meterGreen, meterYellow, meterRed,
                        sendPost, sendPre, deviceAmp, deviceCymbal,
                        sectionIntro, sectionVerse, sectionChorus, sectionBridge, sectionOutro, sectionBreak;

    inline std::array<juce::Colour, 12> clips;
}

namespace Theme
{
    inline const Palette* current = nullptr;

    inline bool isLight()   { return current != nullptr && current->isLight; }

    /** Every palette colour, for remapping colours already set on components when the theme changes. */
    inline std::vector<juce::Colour*> allColours()
    {
        using namespace Colours;
        std::vector<juce::Colour*> v { &background, &panel, &panelLight, &panelDark, &display, &outline, &gridLine, &gridBar,
                                       &text, &textDim, &textInverse, &accent, &playhead, &record, &solo, &mute, &modulation,
                                       &meterGreen, &meterYellow, &meterRed, &sendPost, &sendPre, &deviceAmp, &deviceCymbal };
        for (auto& c : clips)
            v.push_back (&c);
        return v;
    }

    inline void apply (const Palette& p)
    {
        using namespace Colours;
        current = &p;
        background = juce::Colour (p.background);   panel = juce::Colour (p.panel);
        panelLight = juce::Colour (p.panelLight);   panelDark = juce::Colour (p.panelDark);
        display = juce::Colour (p.display);         outline = juce::Colour (p.outline);
        gridLine = juce::Colour (p.gridLine);       gridBar = juce::Colour (p.gridBar);
        text = juce::Colour (p.text);               textDim = juce::Colour (p.textDim);
        textInverse = juce::Colour (p.textInverse);
        accent = juce::Colour (p.accent);           playhead = juce::Colour (p.playhead);
        record = juce::Colour (p.record);           solo = juce::Colour (p.solo);
        mute = juce::Colour (p.mute);               modulation = juce::Colour (p.modulation);
        meterGreen = juce::Colour (p.meterGreen);   meterYellow = juce::Colour (p.meterYellow);
        meterRed = juce::Colour (p.meterRed);
        sendPost = juce::Colour (p.sendPost);       sendPre = juce::Colour (p.sendPre);
        deviceAmp = juce::Colour (p.deviceAmp);     deviceCymbal = juce::Colour (p.deviceCymbal);

        for (size_t i = 0; i < clips.size(); ++i)
            clips[i] = juce::Colour (p.clips[i]);

        sectionIntro = clips[4];  sectionVerse = clips[2];  sectionChorus = clips[0];
        sectionBridge = clips[5]; sectionOutro = clips[3];  sectionBreak = clips[6];
    }

    /** Track and clip colours are saved in the project as the original (classic) palette values,
        so collaborators on other themes see the same choice. This shows them in the current theme. */
    inline juce::Colour forTrack (juce::Colour stored)
    {
        for (int i = 0; i < 12; ++i)
            if (stored.getARGB() == EngineHelpers::trackColourFor (i).getARGB())
                return Colours::clips[(size_t) i];

        return stored;
    }

    /** A highlight: lighter on dark themes, a little darker on light ones. */
    inline juce::Colour lift (juce::Colour c, float amount)
    {
        return isLight() ? c.darker (amount * 0.6f) : c.brighter (amount);
    }

    /** The selected track's (or focused strip's) panel. Lighter than the knobs and keys on it
        (panelLight), so they still show: paler than the page in the light theme, lifted at night. */
    inline juce::Colour raisedSurface()
    {
        return isLight() ? Colours::panel.interpolatedWith (juce::Colours::white, 0.7f)
                         : Colours::panelLight.interpolatedWith (Colours::text, 0.07f);
    }

    /** Waveforms and notes drawn on top of a clip colour. */
    inline juce::Colour contentOn (juce::Colour clipColour)
    {
        return isLight() ? clipColour.darker (0.9f) : clipColour.brighter (0.4f);
    }

    //==============================================================================
    // Orbit's "tint": a clip is a quiet wash of its track's colour, and what's in it (the
    // waveform, notes or hits) carries the full colour.

    /** A clip's body: the track colour washed over the background. */
    inline juce::Colour tint (juce::Colour c)           { return c.withAlpha (isLight() ? 0.24f : 0.17f); }

    /** A clip's name, in its track colour but readable on the tint. */
    inline juce::Colour tintLabel (juce::Colour c)      { return isLight() ? c.interpolatedWith (juce::Colours::black, 0.3f) : c.interpolatedWith (juce::Colours::white, 0.15f); }

    /** A step of a group's colour for one of its tracks: k from -1 to 1. Lighter or deeper, in the
        direction that keeps it readable on this theme. */
    inline juce::Colour tone (juce::Colour c, float k)
    {
        if (k == 0.0f)
            return c;

        if (isLight())
            return k > 0.0f ? c.interpolatedWith (juce::Colours::black, 0.3f * k) : c.interpolatedWith (Colours::background, -0.3f * k);

        return k > 0.0f ? c.interpolatedWith (juce::Colours::white, 0.28f * k) : c.interpolatedWith (Colours::background, -0.32f * k);
    }

    /** The colour a track shows in. A group has one colour, chosen for the whole group, and its
        tracks show tones of it from the first (one way) to the last (the other), so they read as
        one family and still tell apart. Other tracks show their own colour. */
    inline juce::Colour ofTrack (const te::Track& t)
    {
        if (t.isMasterTrack())
            return Colours::textDim;

        if (auto group = t.getParentFolderTrack())
        {
            juce::Array<te::Track*> members;
            for (auto m : group->getAllSubTracks (false))
                if (m->isAudioTrack() || m->isFolderTrack())
                    members.add (m);

            const auto n = members.size(), i = members.indexOf (const_cast<te::Track*> (&t));
            const auto k = n > 1 && i >= 0 ? 0.6f - 1.2f * (float) i / (float) (n - 1) : 0.0f;
            return tone (ofTrack (*group), k);
        }

        auto c = t.getColour();
        return c.isTransparent() ? Colours::textDim : forTrack (c);
    }

    //==============================================================================
    // Accessibility: WCAG 2.1 contrast. Text needs 4.5:1 and the graphics that carry meaning
    // (value arcs, fader levels, meters, hash marks, control outlines) 3:1, against every
    // surface they sit on, so colours are checked against the theme's least contrasting one.

    inline float relativeLuminance (juce::Colour c)
    {
        auto channel = [] (juce::uint8 v)
        {
            const float x = (float) v / 255.0f;
            return x <= 0.03928f ? x / 12.92f : std::pow ((x + 0.055f) / 1.055f, 2.4f);
        };
        return 0.2126f * channel (c.getRed()) + 0.7152f * channel (c.getGreen()) + 0.0722f * channel (c.getBlue());
    }

    inline float contrast (juce::Colour a, juce::Colour b)
    {
        const float la = relativeLuminance (a), lb = relativeLuminance (b);
        return (juce::jmax (la, lb) + 0.05f) / (juce::jmin (la, lb) + 0.05f);
    }

    /** The colour, darkened (light theme) or lifted (dark theme) just enough to reach the ratio. */
    inline juce::Colour ensureContrast (juce::Colour c, juce::Colour against, float ratio)
    {
        for (int i = 0; i < 60 && contrast (c, against) < ratio; ++i)
            c = isLight() ? c.darker (0.05f) : c.brighter (0.05f);
        return c;
    }

    /** The surface controls sit on that gives the least contrast in this theme. */
    inline juce::Colour hardestSurface()                { return isLight() ? Colours::panelDark : Colours::panelLight; }

    /** A colour carrying meaning as a mark or level (3:1). */
    inline juce::Colour indicator (juce::Colour c)      { return ensureContrast (c, hardestSurface(), 3.0f); }

    /** A colour used for text (4.5:1). */
    inline juce::Colour readable (juce::Colour c)       { return ensureContrast (c, hardestSurface(), 4.5f); }

    /** Hash marks, control tracks and outlines: the grid colour, made visible enough to read. */
    inline juce::Colour control()                       { return indicator (Colours::gridBar); }

    /** A text field's fill: set off from the panels and dialogs it sits in (paler by day, deeper
        at night). Its outline (fieldOutline()) is what makes it findable. */
    inline juce::Colour field()                         { return isLight() ? Colours::panel.interpolatedWith (juce::Colours::white, 0.8f) : Colours::background; }

    /** A text field's outline: 3:1 against any surface a field sits on, including panelLight
        (the darkest by day, the lightest at night). */
    inline juce::Colour fieldOutline()                  { return ensureContrast (Colours::gridBar, Colours::panelLight, 3.0f); }

    /** Text or a glyph on a filled colour (a disc, a chip): whichever of black or white reads better. */
    inline juce::Colour inkOn (juce::Colour fill)
    {
        return contrast (juce::Colours::white, fill) >= contrast (juce::Colour (0xff111112), fill) ? juce::Colours::white : juce::Colour (0xff111112);
    }

    inline const bool initialised = [] { apply (Themes::dark); return true; }();

    /** Adds a text field to a dialog, inset so its text clears the field's rounded ends.
        Use instead of AlertWindow::addTextEditor. */
    inline juce::TextEditor* addTextField (juce::AlertWindow& aw, const juce::String& name, const juce::String& text,
                                           const juce::String& label = {})
    {
        aw.addTextEditor (name, text, label);
        auto* ed = aw.getTextEditor (name);
        ed->setIndents (10, ed->getTopIndent());
        return ed;
    }

    /** A dialog with see-through corners, so its rounded panel shows: JUCE makes windows opaque. */
    inline juce::AlertWindow* newAlertWindow (const juce::String& title, const juce::String& message)
    {
        auto* aw = new juce::AlertWindow (title, message, juce::MessageBoxIconType::NoIcon);
        aw->setOpaque (false);
        return aw;
    }
}

//==============================================================================
class ElsewhereLookAndFeel : public juce::LookAndFeel_V4
{
public:
    ElsewhereLookAndFeel()     { refreshColours(); }

    /** Re-reads the current theme's colours. */
    void refreshColours()
    {
        auto scheme = juce::LookAndFeel_V4::ColourScheme {
            Colours::background,   // windowBackground
            Colours::panel,        // widgetBackground
            Colours::panelDark,    // menuBackground
            Colours::outline,      // outline
            Colours::text,         // defaultText
            Colours::panelLight,   // defaultFill
            Colours::textInverse,  // highlightedText
            Colours::accent,       // highlightedFill
            Colours::text          // menuText
        };
        setColourScheme (scheme);

        setColour (juce::ResizableWindow::backgroundColourId, Colours::background);
        setColour (juce::TextButton::buttonColourId, Colours::panelLight);
        setColour (juce::TextButton::buttonOnColourId, Colours::accent);
        setColour (juce::TextButton::textColourOffId, Colours::text);
        setColour (juce::TextButton::textColourOnId, Colours::textInverse);
        setColour (juce::Slider::thumbColourId, Colours::text);
        setColour (juce::Slider::trackColourId, Colours::accent);
        setColour (juce::Slider::backgroundColourId, Colours::panelDark);
        setColour (juce::Slider::rotarySliderFillColourId, Colours::accent);
        setColour (juce::Slider::rotarySliderOutlineColourId, Colours::gridBar);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Label::textColourId, Colours::text);
        setColour (juce::ComboBox::backgroundColourId, Colours::panelLight);
        setColour (juce::PopupMenu::backgroundColourId, Colours::panel.withAlpha (0.99f));  // so its window can have round corners
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Colours::accent);
        setColour (juce::PopupMenu::highlightedTextColourId, Colours::textInverse);
        setColour (juce::PopupMenu::textColourId, Colours::text);
        setColour (juce::ComboBox::textColourId, Colours::text);
        setColour (juce::ComboBox::arrowColourId, Colours::textDim);
        setColour (juce::TextEditor::textColourId, Colours::text);
        setColour (juce::TextEditor::highlightColourId, Colours::accent.withAlpha (0.35f));
        setColour (juce::ToggleButton::textColourId, Colours::text);
        setColour (juce::ToggleButton::tickColourId, Colours::accent);
        setColour (juce::ToggleButton::tickDisabledColourId, Colours::textDim);
        setColour (juce::Slider::textBoxTextColourId, Colours::text);
        setColour (juce::AlertWindow::backgroundColourId, Colours::panel);
        setColour (juce::AlertWindow::textColourId, Colours::text);
        setColour (juce::AlertWindow::outlineColourId, Colours::outline);
        setColour (juce::TooltipWindow::backgroundColourId, juce::Colours::transparentBlack);  // drawTooltip fills a rounded panelLight
        setColour (juce::TooltipWindow::textColourId, Colours::text);
        setColour (juce::TooltipWindow::outlineColourId, juce::Colours::transparentBlack);
        setColour (juce::TreeView::linesColourId, Colours::textDim);
        setColour (juce::ScrollBar::thumbColourId, Colours::gridBar);
        setColour (juce::TreeView::backgroundColourId, Colours::panel);
        setColour (juce::TreeView::selectedItemBackgroundColourId, Colours::accent.withAlpha (0.35f));
        setColour (juce::DirectoryContentsDisplayComponent::highlightColourId, Colours::accent.withAlpha (0.35f));
        setColour (juce::DirectoryContentsDisplayComponent::textColourId, Colours::text);
        // Text fields: a fill set off from the surface, an outline at 3:1 so the field can be
        // found, and a heavier ink outline once it has focus
        setColour (juce::TextEditor::backgroundColourId, Theme::field());
        setColour (juce::TextEditor::outlineColourId, Theme::fieldOutline());
        setColour (juce::TextEditor::focusedOutlineColourId, Colours::accent);
        // AlertWindow::addTextEditor gives its fields the ComboBox outline colour (our combo
        // boxes draw none), so this is the outline of every text field in a dialog
        setColour (juce::ComboBox::outlineColourId, Theme::fieldOutline());
        setColour (juce::ListBox::backgroundColourId, Colours::panel);
        setColour (juce::TabbedButtonBar::tabOutlineColourId, Colours::outline);
        setColour (juce::CaretComponent::caretColourId, Colours::accent);
    }

    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override
    {
        return juce::FontOptions (juce::jmin (13.0f, (float) buttonHeight * 0.6f));
    }

    void drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour& backgroundColour,
                               bool isMouseOver, bool isButtonDown) override
    {
        auto r = b.getLocalBounds().toFloat().reduced (0.5f);
        auto c = backgroundColour;

        auto shade = [] (juce::Colour col, float amount) { return Theme::isLight() ? col.darker (amount * 0.6f) : col.brighter (amount); };

        if (isButtonDown)       c = shade (c, 0.15f);
        else if (isMouseOver)   c = shade (c, 0.07f);

        // Orbit: flat, no outline, fully rounded, so a square key is a disc and a wide one a capsule
        g.setColour (c);
        g.fillRoundedRectangle (r, juce::jmin (r.getWidth(), r.getHeight()) * 0.5f);
    }

    /** Keys small enough to be discs (m, s, a number) show their text centred, without the usual
        side indents that would cut it short. */
    void drawButtonText (juce::Graphics& g, juce::TextButton& b, bool isMouseOver, bool isButtonDown) override
    {
        if (b.getWidth() > b.getHeight() * 3 / 2)
            return juce::LookAndFeel_V4::drawButtonText (g, b, isMouseOver, isButtonDown);

        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.setColour (b.findColour (b.getToggleState() ? juce::TextButton::textColourOnId : juce::TextButton::textColourOffId)
                      .withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.5f));
        g.drawText (b.getButtonText(), b.getLocalBounds(), juce::Justification::centred, false);
    }

    /** Knob body and dot, as on Somewhere's Orbit design: a raised disc with an ink dot. */
    static juce::Colour knobBody()      { return Colours::panelLight; }
    static juce::Colour knobPointer()   { return Colours::text; }

    void drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height, float pos,
                           float startAngle, float endAngle, juce::Slider& s) override
    {
        juce::ignoreUnused (s);

        // Orbit's knob: a disc with a dot orbiting inside its rim, from -135 to 135 degrees
        auto bounds = juce::Rectangle<int> (x, y, width, height).toFloat().reduced (1.0f);
        const auto size = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const auto centre = bounds.getCentre();
        const auto radius = size * 0.5f;

        g.setColour (knobBody());
        g.fillEllipse (juce::Rectangle<float> (size, size).withCentre (centre));

        const auto angle = startAngle + pos * (endAngle - startAngle);
        const auto dot = juce::jlimit (3.0f, 7.0f, size * 0.13f);
        const auto orbit = radius - juce::jmax (dot * 0.5f + 2.5f, size * 0.17f);
        g.setColour (knobPointer());
        g.fillEllipse (juce::Rectangle<float> (dot, dot).withCentre (centre + juce::Point<float> (std::sin (angle), -std::cos (angle)) * orbit));
    }

    //==============================================================================
    /** Vertical faders, as on Somewhere's Orbit design: a capsule washed in the track's colour,
        filled up to the level, with an ink disc for a thumb. */
    static constexpr float faderWidth = 22.0f;

    int getSliderThumbRadius (juce::Slider& s) override
    {
        if (s.getSliderStyle() == juce::Slider::LinearVertical)
            return (int) (faderWidth * 0.5f);
        if (isThinFader (s))
            return 5;
        return juce::LookAndFeel_V4::getSliderThumbRadius (s);
    }

    /** Horizontal faders without a text box (e.g. a track header's volume): a thin line and a dot. */
    static bool isThinFader (juce::Slider& s)
    {
        const auto style = s.getSliderStyle();
        return (style == juce::Slider::LinearBar || style == juce::Slider::LinearHorizontal) && s.getTextBoxPosition() == juce::Slider::NoTextBox;
    }

    void drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height, float sliderPos, float minPos, float maxPos,
                           juce::Slider::SliderStyle style, juce::Slider& s) override
    {
        juce::ignoreUnused (minPos, maxPos);
        auto r = juce::Rectangle<int> (x, y, width, height).toFloat();
        const bool coloured = s.isColourSpecified (juce::Slider::trackColourId);
        const auto colour = s.findColour (juce::Slider::trackColourId);

        if (style == juce::Slider::LinearVertical)
        {
            // The thumb's centre travels from top to bottom; the capsule runs past both ends by its radius
            const float w = juce::jmin (faderWidth, r.getWidth()), cx = r.getCentreX();
            auto capsule = juce::Rectangle<float> (cx - w * 0.5f, r.getY() - w * 0.5f, w, r.getHeight() + w);
            g.setColour (coloured ? Theme::tint (colour) : Colours::gridBar);
            g.fillRoundedRectangle (capsule, w * 0.5f);
            g.setColour (coloured ? colour.withAlpha (0.5f) : Colours::textDim.withAlpha (0.5f));
            g.fillRoundedRectangle (capsule.withTop (sliderPos - w * 0.5f), w * 0.5f);
            g.setColour (Colours::text);
            g.fillEllipse (juce::Rectangle<float> (w, w).withCentre ({ cx, sliderPos }));
            return;
        }

        if (isThinFader (s))
        {
            const float cy = r.getCentreY();
            auto line = juce::Rectangle<float> (r.getX(), cy - 2.0f, r.getWidth(), 4.0f);
            g.setColour (Colours::gridBar);
            g.fillRoundedRectangle (line, 2.0f);
            g.setColour (Colours::textDim);
            g.fillRoundedRectangle (line.withRight (sliderPos), 2.0f);
            g.setColour (Colours::text);
            g.fillEllipse (juce::Rectangle<float> (10.0f, 10.0f).withCentre ({ juce::jlimit (r.getX() + 5.0f, r.getRight() - 5.0f, sliderPos), cy }));
            return;
        }

        if (style == juce::Slider::LinearBar)
        {
            // A capsule filled up to the value, with its text on top
            g.setColour (Colours::panelLight);
            g.fillRoundedRectangle (r, r.getHeight() * 0.5f);
            g.setColour (coloured ? colour.withAlpha (0.55f) : Colours::gridBar);
            g.fillRoundedRectangle (r.withRight (juce::jmax (r.getX() + r.getHeight(), sliderPos)), r.getHeight() * 0.5f);
            return;
        }

        juce::LookAndFeel_V4::drawLinearSlider (g, x, y, width, height, sliderPos, minPos, maxPos, style, s);
    }

    void drawComboBox (juce::Graphics& g, int width, int height, bool, int, int, int, int, juce::ComboBox& box) override
    {
        auto r = juce::Rectangle<int> (width, height).toFloat();
        g.setColour (box.findColour (juce::ComboBox::backgroundColourId));
        g.fillRoundedRectangle (r, r.getHeight() * 0.5f);

        // A small chevron
        const float cx = r.getRight() - r.getHeight() * 0.55f, cy = r.getCentreY();
        juce::Path p;
        p.startNewSubPath (cx - 3.5f, cy - 1.75f);
        p.lineTo (cx, cy + 1.75f);
        p.lineTo (cx + 3.5f, cy - 1.75f);
        g.setColour (box.findColour (juce::ComboBox::arrowColourId));
        g.strokePath (p, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void positionComboBoxText (juce::ComboBox& box, juce::Label& label) override
    {
        label.setBounds (juce::jmax (8, box.getHeight() / 3), 1, box.getWidth() - box.getHeight() - 4, box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
    }

    //==============================================================================
    // Menus, dialogs, tooltips and text fields are rounded like everything else. Menus and dialogs
    // are windows of their own, which JUCE makes opaque; theirs are made see-through (a menu's
    // background colour is a hair off opaque, a dialog is set so) and the shadow follows the shape.
    static constexpr float windowCorner = 12.0f;

    void drawPopupMenuBackground (juce::Graphics& g, int width, int height) override
    {
        g.setColour (Colours::panel);
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), windowCorner);
    }

    int getPopupMenuBorderSize() override      { return 6; }

    void drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                            bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                            const juce::String& shortcutKeyText, const juce::Drawable* icon, const juce::Colour* textColour) override
    {
        // The highlight is a rounded bar inside the menu's padding, not a block across it
        if (isHighlighted && isActive && ! isSeparator)
        {
            g.setColour (findColour (juce::PopupMenu::highlightedBackgroundColourId));
            g.fillRoundedRectangle (area.toFloat(), 7.0f);

            auto ink = findColour (juce::PopupMenu::highlightedTextColourId);
            return juce::LookAndFeel_V4::drawPopupMenuItem (g, area, false, true, false, isTicked, hasSubMenu, text, shortcutKeyText, icon, &ink);
        }

        juce::LookAndFeel_V4::drawPopupMenuItem (g, area, isSeparator, isActive, isHighlighted, isTicked, hasSubMenu, text, shortcutKeyText, icon, textColour);
    }

    juce::AlertWindow* createAlertWindow (const juce::String& title, const juce::String& message, const juce::String& button1,
                                          const juce::String& button2, const juce::String& button3, juce::MessageBoxIconType iconType,
                                          int numButtons, juce::Component* associatedComponent) override
    {
        auto* aw = juce::LookAndFeel_V4::createAlertWindow (title, message, button1, button2, button3, iconType, numButtons, associatedComponent);
        aw->setOpaque (false);
        return aw;
    }

    /** A rounded panel with the title and message; no icon, the title says what it is. */
    void drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea, juce::TextLayout& textLayout) override
    {
        g.setColour (alert.findColour (juce::AlertWindow::backgroundColourId));
        g.fillRoundedRectangle (alert.getLocalBounds().toFloat(), windowCorner);
        textLayout.draw (g, textArea.toFloat());
    }

    void drawTooltip (juce::Graphics& g, const juce::String& text, int width, int height) override
    {
        g.setColour (Colours::panelLight);
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), juce::jmin (8.0f, (float) height * 0.5f));
        juce::LookAndFeel_V4::drawTooltip (g, text, width, height);
    }

    void fillTextEditorBackground (juce::Graphics& g, int width, int height, juce::TextEditor& ed) override
    {
        g.setColour (ed.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (juce::Rectangle<int> (width, height).toFloat(), juce::jmin (8.0f, (float) height * 0.5f));
    }

    void drawTextEditorOutline (juce::Graphics& g, int width, int height, juce::TextEditor& ed) override
    {
        if (! ed.isEnabled())
            return;

        auto focused = ed.hasKeyboardFocus (true) && ! ed.isReadOnly();
        auto c = ed.findColour (focused ? juce::TextEditor::focusedOutlineColourId : juce::TextEditor::outlineColourId);

        if (c.isTransparent())
            return;

        const float thickness = focused ? 2.0f : 1.0f;
        g.setColour (c);
        g.drawRoundedRectangle (juce::Rectangle<int> (width, height).toFloat().reduced (thickness * 0.5f),
                                juce::jmin (8.0f, (float) height * 0.5f), thickness);
    }

    void drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int width, int height,
                        bool isVertical, int thumbStart, int thumbSize, bool isMouseOver, bool isDown) override
    {
        auto thumb = isVertical ? juce::Rectangle<int> (x, thumbStart, width, thumbSize)
                                : juce::Rectangle<int> (thumbStart, y, thumbSize, height);
        auto thumbColour = Theme::isLight() ? Colours::gridBar.darker (isDown ? 0.3f : isMouseOver ? 0.15f : 0.0f)
                                            : Colours::gridBar.brighter (isDown ? 0.5f : isMouseOver ? 0.3f : 0.15f);
        g.setColour (thumbColour);
        g.fillRoundedRectangle (thumb.reduced (2).toFloat(), 3.0f);
    }
};

} // namespace elsewhere
