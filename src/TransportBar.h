// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"
#include "Widgets.h"
#include "Wordmark.h"

namespace elsewhere
{
//==============================================================================
/** Orbit's disc key with a small glyph, used for play/stop/record etc. "On" fills the disc with
    the key's colour (ink, or red for record) and turns the glyph inverse. A glyph colour can be
    given for "off" too (record shows a red dot on a grey disc). */
class IconButton : public juce::Button
{
public:
    IconButton (const juce::String& name, juce::Path p, juce::Colour onCol, std::optional<juce::Colour> glyphOff = {})
        : juce::Button (name), path (std::move (p)), onColour (onCol), offGlyph (glyphOff) {}

    void paintButton (juce::Graphics& g, bool over, bool down) override
    {
        auto bounds = getLocalBounds().toFloat();
        const auto d = juce::jmin (bounds.getWidth(), bounds.getHeight()) - 1.0f;
        auto r = juce::Rectangle<float> (d, d).withCentre (bounds.getCentre());
        auto on = getToggleState();

        g.setColour (on ? onColour : (down ? Theme::lift (Colours::panelLight, 0.15f) : over ? Theme::lift (Colours::panelLight, 0.07f) : Colours::panelLight));
        g.fillEllipse (r);

        if (Theme::moreContrast() && ! on)
        {
            g.setColour (Theme::control());
            g.drawEllipse (r.reduced (0.5f), 1.0f);
        }

        auto iconArea = r.reduced (d * 0.33f);
        g.setColour (on ? (onColour == Colours::record ? juce::Colours::white : Colours::textInverse) : offGlyph.value_or (Colours::text));
        g.fillPath (path, path.getTransformToScaleToFit (iconArea, true));
    }

    static juce::Path play()     { juce::Path p; p.addTriangle (0, 0, 0, 10, 9, 5); return p; }
    static juce::Path stop()     { juce::Path p; p.addRectangle (0, 0, 10, 10); return p; }
    static juce::Path record()   { juce::Path p; p.addEllipse (0, 0, 10, 10); return p; }

    /** A capsule outline, as on Somewhere. */
    static juce::Path loop()
    {
        juce::Path p;
        p.addRoundedRectangle (0.0f, 0.0f, 16.0f, 10.0f, 5.0f);
        juce::Path inner; inner.addRoundedRectangle (1.8f, 1.8f, 12.4f, 6.4f, 3.2f);
        p.addPath (inner);
        p.setUsingNonZeroWinding (false);
        return p;
    }

private:
    juce::Path path;
    juce::Colour onColour;
    std::optional<juce::Colour> offGlyph;
};

//==============================================================================
/** The metronome and its count-in as one pill: the dot toggles the click, the chevron opens
    the count-in choices. Each half fills with ink while it's on, like the other keys. */
class MetronomePill : public juce::Component, public juce::TooltipClient
{
public:
    std::function<void()> onToggleClick, onShowCountIn;

    MetronomePill()
    {
        // Each half is a key of its own for screen readers and the keyboard. They draw nothing
        // and take no clicks: the pill paints both and handles the mouse.
        clickKey.setTitle ("Metronome");
        clickKey.setToggleable (true);
        clickKey.onClick = [this] { if (onToggleClick) onToggleClick(); };
        countInKey.setTitle ("Count-in");
        countInKey.onClick = [this] { if (onShowCountIn) onShowCountIn(); };

        for (auto* k : { &clickKey, &countInKey })
        {
            k->setInterceptsMouseClicks (false, false);
            addAndMakeVisible (*k);
        }
    }

    void resized() override
    {
        clickKey.setBounds (getLocalBounds().withRight (divider()));
        countInKey.setBounds (getChevronArea());
    }

    void setState (bool clickIsOn, bool countInIsOn, const juce::String& countInName)
    {
        if (clickIsOn == clickOn && countInIsOn == countOn && countInName == countName)
            return;

        clickOn = clickIsOn;
        countOn = countInIsOn;
        countName = countInName;
        clickKey.setToggleState (clickOn, juce::dontSendNotification);
        countInKey.setDescription (countOn ? countName : "Off");
        countInKey.setTooltip (countOn ? "Count-in: " + countName : "Count-in before recording");
        repaint();
    }

    juce::Rectangle<int> getChevronArea() const     { return getLocalBounds().withTrimmedLeft (divider()); }

    void paint (juce::Graphics& g) override
    {
        auto b = getLocalBounds().toFloat().reduced (0.5f);
        const float radius = b.getHeight() * 0.5f, split = (float) divider();

        juce::Path pill;
        pill.addRoundedRectangle (b, radius);
        g.saveState();
        g.reduceClipRegion (pill);

        auto half = [&] (Part p) { return p == Part::click ? b.withRight (split) : b.withLeft (split); };

        for (auto p : { Part::click, Part::countIn })
        {
            const bool on = p == Part::click ? clickOn : countOn;
            auto base = Colours::panelLight;

            if (! on && p == hover)
                base = Theme::lift (base, p == pressed ? 0.15f : 0.07f);

            g.setColour (on ? Colours::accent : base);
            g.fillRect (half (p));
        }

        g.restoreState();

        // A short rule between the two, as in the other split controls (hidden against ink)
        if (! clickOn && ! countOn)
        {
            g.setColour (Colours::textDim.withAlpha (0.3f));
            g.fillRect (juce::Rectangle<float> (1.5f, b.getHeight() * 0.4f).withCentre ({ split, b.getCentreY() }));
        }

        const float h = b.getHeight();
        auto glyph = [] (bool on) { return on ? Colours::textInverse : Colours::text; };

        // Click: the same small dot as Somewhere's click key
        auto dotArea = half (Part::click).withTrimmedLeft (h * 0.12f);
        g.setColour (glyph (clickOn));
        g.fillEllipse (juce::Rectangle<float> (h * 0.3f, h * 0.3f).withCentre (dotArea.getCentre()));

        // Count-in: the same small chevron as the combo boxes
        auto c = half (Part::countIn).withTrimmedRight (h * 0.12f).getCentre();
        juce::Path chevron;
        chevron.startNewSubPath (c.x - 3.5f, c.y - 1.75f);
        chevron.lineTo (c.x, c.y + 1.75f);
        chevron.lineTo (c.x + 3.5f, c.y - 1.75f);
        g.setColour (glyph (countOn));
        g.strokePath (chevron, juce::PathStrokeType (1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    juce::String getTooltip() override
    {
        if (hover == Part::countIn)
            return countOn ? "Count-in: " + countName : "Count-in before recording";

        return clickOn ? "Metronome: on" : "Metronome";
    }

    void mouseMove (const juce::MouseEvent& e) override     { setHover (partAt (e.getPosition())); }
    void mouseEnter (const juce::MouseEvent& e) override    { setHover (partAt (e.getPosition())); }
    void mouseExit (const juce::MouseEvent&) override       { setHover (Part::none); }
    void mouseDown (const juce::MouseEvent& e) override     { pressed = partAt (e.getPosition()); repaint(); }

    void mouseUp (const juce::MouseEvent& e) override
    {
        auto p = pressed;
        pressed = Part::none;
        repaint();

        if (p != partAt (e.getPosition()))
            return;

        if (p == Part::click && onToggleClick)          onToggleClick();
        else if (p == Part::countIn && onShowCountIn)   onShowCountIn();
    }

private:
    struct Key : public juce::Button
    {
        Key() : juce::Button ({}) {}
        void paintButton (juce::Graphics&, bool, bool) override {}
    };

    Key clickKey, countInKey;

    enum class Part { none, click, countIn };
    Part hover = Part::none, pressed = Part::none;
    bool clickOn = false, countOn = false;
    juce::String countName;

    int divider() const     { return juce::roundToInt (getWidth() * 0.54f); }

    Part partAt (juce::Point<int> p) const
    {
        if (! getLocalBounds().contains (p))
            return Part::none;

        return p.x < divider() ? Part::click : Part::countIn;
    }

    void setHover (Part p)
    {
        if (p != hover)
        {
            hover = p;
            repaint();
        }
    }
};

//==============================================================================
class TransportBar : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    TransportBar (EditViewState& evs) : viewState (evs), edit (evs.edit), transport (evs.edit.getTransport())
    {
        tempo.onValueChange = [this] (double bpm)
        {
            edit.getUndoManager().beginNewTransaction ("Tempo");
            edit.tempoSequence.getTempo (0)->setBpm (bpm);
        };
        addAndMakeVisible (tempo);

        tapButton.onClick = [this] { tap(); };
        tapButton.setTitle ("Tap Tempo");
        tapButton.setTooltip ("Tap tempo: click in time to set the tempo");
        addAndMakeVisible (tapButton);
        tempo.accessibleName = "Tempo";
        tempo.unit = "BPM";
        tempo.setTooltip ("Tempo. Drag up or down, or use the arrow keys; double-click or Return to type it");

        // Orbit: the tempo in big light digits, the time signature quietly beside it
        tempo.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        tempo.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        tempo.setFont (juce::FontOptions (24.0f, juce::Font::plain).withStyle ("Light"));
        tempo.setJustificationType (juce::Justification::centredRight);

        timeSig.setEditable (false, true, false);
        timeSig.setJustificationType (juce::Justification::centredLeft);
        timeSig.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        timeSig.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        timeSig.setColour (juce::Label::textColourId, Colours::textDim);
        timeSig.setFont (juce::FontOptions (11.0f));
        timeSig.setTooltip ("Time signature. Double-click to change it");
        timeSig.onTextChange = [this]
        {
            auto s = timeSig.getText();
            if (s.containsChar ('/'))
                edit.tempoSequence.getTimeSig (0)->setStringTimeSig (s);
            refresh();
        };
        addAndMakeVisible (timeSig);

        metronome.onToggleClick = [this] { edit.clickTrackEnabled = ! edit.clickTrackEnabled.get(); refresh(); };
        metronome.onShowCountIn = [this] { showCountInMenu(); };
        addAndMakeVisible (metronome);

        playButton.onClick = [this]
        {
            if (transport.isPlaying())
                transport.stop (false, false);
            else
                transport.play (false);
        };
        stopButton.onClick = [this] { EngineHelpers::stopOrReturnToZero (edit); };
        recordButton.onClick = [this] { EngineHelpers::toggleRecord (edit); };
        loopButton.onClick = [this] { EngineHelpers::setLooping (edit, ! transport.looping.get()); refresh(); };
        loopButton.setTooltip ("Loop (Cmd+L)");
        playButton.setTooltip ("Play (Space)");
        stopButton.setTooltip ("Stop; press again to go back to the start");
        recordButton.setTooltip ("Record (F9)");

        for (auto* b : { &playButton, &recordButton, &loopButton })
            b->setToggleable (true);

        for (auto* b : { &playButton, &stopButton, &recordButton, &loopButton })
            addAndMakeVisible (*b);

        automationArm.setTooltip ("Automation Arm: record knob and fader moves while playing");
        automationArm.setTitle ("Automation Arm");
        automationArm.setColour (juce::TextButton::buttonOnColourId, Colours::record);
        automationArm.onClick = [this]
        {
            auto& arm = edit.getAutomationRecordManager();
            arm.setWritingAutomation (! arm.isWritingAutomation());
            refresh();
        };
        addAndMakeVisible (automationArm);

        midiLearn.setTooltip ("MIDI Map Mode (Cmd+M): click a knob or fader, then move a control on your MIDI controller");
        midiLearn.setTitle ("MIDI Map Mode");
        midiLearn.setColour (juce::TextButton::buttonOnColourId, Colours::solo);
        midiLearn.onClick = [this]
        {
            auto& learn = edit.engine.getMidiLearnState();
            learn.setActive (! learn.isActive());
            refresh();
        };
        addAndMakeVisible (midiLearn);

        punchButton.setTooltip ("Punch: record only between the brace's ends on the ruler. Right-click for pre-roll");
        punchButton.setTitle ("Punch Recording");
        punchButton.setColour (juce::TextButton::buttonOnColourId, Colours::record);
        punchButton.onClick = [this]
        {
            if (juce::ModifierKeys::currentModifiers.isPopupMenu())
                return showPreRollMenu();

            EngineHelpers::setPunching (edit, ! EngineHelpers::isPunching (edit));
            refresh();
        };
        addAndMakeVisible (punchButton);


        liveButton.setTooltip ("Play together live over the internet (Cmd+J)");
        liveButton.setTitle ("Live Session");
        liveButton.setColour (juce::TextButton::buttonOnColourId, Colours::record);
        liveButton.onClick = [this] { if (onLiveClicked) onLiveClicked(); };
        addAndMakeVisible (liveButton);

        backToArrangement.setTitle ("Back to Arrangement");
        backToArrangement.setTooltip ("Some tracks are playing Session clips. Click to play the Arrangement on all tracks again.");
        backToArrangement.setColour (juce::TextButton::buttonColourId, Colours::accent);
        backToArrangement.setColour (juce::TextButton::textColourOffId, Colours::textInverse);
        backToArrangement.onClick = [this]
        {
            for (auto t : te::getAudioTracks (edit))
                EngineHelpers::backToArrangement (*t);
        };
        addChildComponent (backToArrangement);

        backToAlbum.setColour (juce::TextButton::buttonColourId, Colours::accent);
        backToAlbum.setColour (juce::TextButton::textColourOffId, Colours::textInverse);
        backToAlbum.onClick = [this] { if (onBackToAlbum) onBackToAlbum(); };
        addChildComponent (backToAlbum);

        position.setJustificationType (juce::Justification::centredLeft);
        position.setFont (juce::FontOptions (18.0f, juce::Font::plain).withStyle ("Light"));
        position.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        position.setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        addAndMakeVisible (position);
        position.setTooltip ("Playhead position: bars, beats and time");

        quantBox.addItemList ({ "None", "1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars" }, 1);
        quantBox.setTooltip ("Launch quantisation for Session clips");
        quantBox.setTitle ("Launch Quantisation");
        quantBox.onChange = [this] { applyLaunchQuantisation(); };
        quantBox.setSelectedId (6, juce::dontSendNotification);
        applyLaunchQuantisation();
        addAndMakeVisible (quantBox);

        gridBox.addItemList ({ "Adaptive", "1/16", "1/8", "1/4", "1/2", "1 Bar", "Off" }, 1);
        gridBox.setTooltip ("Arrangement snap grid");
        gridBox.setTitle ("Snap Grid");
        gridBox.setSelectedId (4, juce::dontSendNotification);
        gridBox.onChange = [this] { applyGrid(); };
        addAndMakeVisible (gridBox);

        for (int i = 0; i < 4; ++i)
            editModeBox.addItem (EditViewState::getEditModeName ((EditViewState::EditMode) i), i + 1);

        editModeBox.setTooltip ("Edit mode: Shuffle (F1) closes clips up, Slip (F2) moves freely, "
                                "Spot (F3) asks for an exact time, Grid (F4) snaps");
        editModeBox.setSelectedId ((int) viewState.editMode + 1, juce::dontSendNotification);
        editModeBox.setTitle ("Edit Mode");
        editModeBox.onChange = [this]
        {
            if (onEditMode)
                onEditMode ((EditViewState::EditMode) (editModeBox.getSelectedId() - 1));
        };
        addAndMakeVisible (editModeBox);

        viewToggle.onClick = [this] { if (onToggleView) onToggleView(); };
        viewToggle.setTooltip ("Show the mixer (Cmd+=)");
        viewToggle.setTitle ("Mixer");
        viewToggle.setColour (juce::TextButton::buttonOnColourId, Colours::accent);
        viewToggle.setColour (juce::TextButton::textColourOnId, Colours::background);
        addAndMakeVisible (viewToggle);


        cpu.setJustificationType (juce::Justification::centredRight);
        cpu.setColour (juce::Label::textColourId, Colours::textDim);
        cpu.setFont (juce::FontOptions (11.0f));
        cpu.setTitle ("Processor load and dropouts");
        addAndMakeVisible (cpu);

        status.setJustificationType (juce::Justification::centredRight);
        status.setColour (juce::Label::textColourId, Colours::textDim);
        status.setFont (juce::FontOptions (11.0f));
        addAndMakeVisible (status);

        // Who's in the project, for screen readers (the chips are painted)
        collaboratorInfo.setInterceptsMouseClicks (false, false);
        addChildComponent (collaboratorInfo);

        for (auto* b : { &automationArm, &midiLearn, &liveButton, &punchButton, &viewToggle })
            b->setToggleable (true);

        setTitle ("Transport");
        setFocusContainerType (juce::Component::FocusContainerType::keyboardFocusContainer);

        transport.addChangeListener (this);
        refresh();
        startTimerHz (20);
    }

    ~TransportBar() override { transport.removeChangeListener (this); }

    std::function<void()> onToggleView, onLiveClicked;
    std::function<void (EditViewState::EditMode)> onEditMode;

    /** Launch quantisation is only for the Session grid. */
    void setHasSession (bool b)
    {
        if (quantBox.isVisible() != b)
        {
            quantBox.setVisible (b);
            resized();
        }
    }


    void setLiveActive (bool active)    { liveButton.setToggleState (active, juce::dontSendNotification); }

    void setMixerShowing (bool b)                   { viewToggle.setToggleState (b, juce::dontSendNotification); }
    void setStatusText (const juce::String& s)      { status.setText (s, juce::dontSendNotification); }
    juce::String getStatusText() const              { return status.getText(); }

    /** A song opened from a mastering project shows a button back to it; empty hides it. */
    void setBackToAlbum (const juce::String& albumName)
    {
        if (albumName == backToAlbumName)
            return;

        backToAlbumName = albumName;
        auto label = juce::String::fromUTF8 ("\xe2\x86\xa9 Back to ") + albumName;
        backToAlbum.setButtonText (label);
        backToAlbum.setTitle ("Back to " + albumName);
        backToAlbum.setTooltip ("Go back to the mastering project " + albumName + ". If you saved changes to this song, the album plays them once its render is updated.");
        updateSideButtons();
    }

    std::function<void()> onBackToAlbum;

    /** Coloured name chips for collaborators who are in the project right now. */
    void setCollaborators (std::vector<std::pair<juce::String, juce::Colour>> people)
    {
        if (people != collaborators)
        {
            collaborators = std::move (people);
            updateSideButtons();

            juce::StringArray names;
            for (auto& p : collaborators)
                names.add (p.first);

            collaboratorInfo.text = "In the project now: " + names.joinIntoString (", ");
            collaboratorInfo.setVisible (! collaborators.empty());
            repaint();
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::background);

        // The wordmark, in ink
        Wordmark::draw (g, wordmark.toFloat().withSizeKeepingCentre ((float) wordmark.getWidth(), wordmarkHeight), Colours::text,
                        juce::RectanglePlacement::xLeft | juce::RectanglePlacement::yMid);

        // Collaborator chips replace the status text while someone else is in the project
        auto area = status.getBounds();
        int x = area.getRight();

        for (auto it = collaborators.rbegin(); it != collaborators.rend(); ++it)
        {
            auto w = juce::jmin (100, juce::GlyphArrangement::getStringWidthInt (juce::FontOptions (11.0f), it->first) + 24);
            x -= w + 4;

            if (x < area.getX())
                break;

            auto chip = juce::Rectangle<int> (x, area.getCentreY() - 10, w, 20).toFloat();
            g.setColour (it->second.withAlpha (0.2f));
            g.fillRoundedRectangle (chip, 10.0f);
            g.setColour (it->second);
            g.fillEllipse (chip.withWidth (12.0f).withHeight (12.0f).translated (5.0f, 4.0f));
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (it->first, chip.withTrimmedLeft (21.0f).toNearestInt(), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 9);
        const int h = r.getHeight();
        const int key = h, smallH = 26;
        auto small = [smallH] (juce::Rectangle<int> b) { return b.withSizeKeepingCentre (b.getWidth(), smallH); };

        wordmark = r.removeFromLeft (juce::roundToInt (wordmarkHeight * Wordmark::aspect) + 20);
        tempo.setBounds (r.removeFromLeft (84));
        r.removeFromLeft (4);
        timeSig.setBounds (r.removeFromLeft (34));
        tapButton.setBounds (small (r.removeFromLeft (40)));
        r.removeFromLeft (6);
        metronome.setBounds (small (r.removeFromLeft (smallH * 2 + 4)));
        r.removeFromLeft (10);

        viewToggle.setBounds (small (r.removeFromRight (76)));
        r.removeFromRight (8);
        cpu.setBounds (r.removeFromRight (dropouts > 0 ? 150 : 64));

        // The transport gets its room first; the status (or Back to Arrangement, which shows
        // in its place) has what's left, up to 180
        const int centreW = quantBox.isVisible() ? 800 : 710;
        auto side = r.removeFromRight (juce::jlimit (0, 180, r.getWidth() - centreW));
        status.setBounds (side.getWidth() >= 70 ? side : juce::Rectangle<int>());
        collaboratorInfo.setBounds (side);
        backToArrangement.setBounds (side.withSizeKeepingCentre (juce::jmin (side.getWidth(), 160), h));
        auto albumSide = side.withTrimmedLeft (8);
        backToAlbum.setBounds (albumSide.withSizeKeepingCentre (juce::jmin (albumSide.getWidth(), backToAlbum.getBestWidthForHeight (26) + 8), 26));

        auto centre = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), centreW), h);

        loopButton.setBounds (centre.removeFromLeft (key));
        centre.removeFromLeft (8);
        playButton.setBounds (centre.removeFromLeft (key));
        centre.removeFromLeft (8);
        stopButton.setBounds (centre.removeFromLeft (key));
        centre.removeFromLeft (8);
        recordButton.setBounds (centre.removeFromLeft (key));
        centre.removeFromLeft (12);
        position.setBounds (centre.removeFromLeft (170));
        centre.removeFromLeft (4);

        punchButton.setBounds (small (centre.removeFromLeft (46)));
        centre.removeFromLeft (12);
        automationArm.setBounds (small (centre.removeFromLeft (44)));
        centre.removeFromLeft (4);
        midiLearn.setBounds (small (centre.removeFromLeft (44)));
        centre.removeFromLeft (4);
        liveButton.setBounds (small (centre.removeFromLeft (40)));
        centre.removeFromLeft (12);
        editModeBox.setBounds (small (centre.removeFromLeft (80)));
        centre.removeFromLeft (4);
        gridBox.setBounds (small (centre.removeFromLeft (80)));
        centre.removeFromLeft (4);
        quantBox.setBounds (small (centre.removeFromLeft (80)));
    }

private:
    EditViewState& viewState;
    te::Edit& edit;
    te::TransportControl& transport;

    DragNumberBox tempo { 20.0, 999.0, 2 };
    juce::TextButton tapButton { "tap" };
    juce::Rectangle<int> wordmark;
    static constexpr float wordmarkHeight = 12.5f;
    juce::Label timeSig;
    MetronomePill metronome;
    IconButton playButton { "Play", IconButton::play(), Colours::accent },
               stopButton { "Stop", IconButton::stop(), Colours::accent },
               recordButton { "Record", IconButton::record(), Colours::record, Colours::record },
               loopButton { "Loop", IconButton::loop(), Colours::accent };
    juce::Label position, cpu, status;
    juce::ComboBox quantBox, gridBox, editModeBox;
    juce::TextButton viewToggle { "mixer" }, automationArm { "auto" }, midiLearn { "midi" }, liveButton { "live" },
                     punchButton { "punch" };
    juce::TextButton backToArrangement { juce::String::fromUTF8 ("\xe2\x86\xa9 Back to Arrangement") };
    juce::TextButton backToAlbum;
    juce::String backToAlbumName;

    /** One thing has the space beside the transport: Back to Arrangement, then Back to Album, then the status. */
    void updateSideButtons()
    {
        backToAlbum.setVisible (backToAlbumName.isNotEmpty() && ! backToArrangement.isVisible());
        status.setVisible (collaborators.empty() && ! backToArrangement.isVisible() && ! backToAlbum.isVisible());
    }
    juce::Array<double> tapTimes;
    std::vector<std::pair<juce::String, juce::Colour>> collaborators;

    /** Paints nothing: it tells screen readers what the collaborator chips show. */
    struct InfoText : public juce::Component
    {
        juce::String text;

        std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
        {
            return std::make_unique<A11y::Handler> (*this, juce::AccessibilityRole::staticText,
                                                    A11y::Handler::Info { [this] { return text; }, {}, {}, {} });
        }
    } collaboratorInfo;

    // What the transport was doing last time, so a change can be announced
    bool wasPlaying = false, wasRecording = false;
    int dropouts = 0, xrunsAtStart = 0;
    bool countingDropouts = false, saidDropout = false;

    void tap()
    {
        auto now = juce::Time::getMillisecondCounterHiRes();

        if (! tapTimes.isEmpty() && now - tapTimes.getLast() > 2000.0)
            tapTimes.clear();

        tapTimes.add (now);

        if (tapTimes.size() > 8)
            tapTimes.remove (0);

        if (tapTimes.size() >= 3)
        {
            auto avg = (tapTimes.getLast() - tapTimes.getFirst()) / (tapTimes.size() - 1);
            tempo.setValue (60000.0 / avg, true);
        }
    }

    juce::String countInName() const
    {
        using C = te::Edit::CountIn;

        switch (edit.getCountInMode())
        {
            case C::oneBeat:    return "1 Beat";
            case C::twoBeat:    return "2 Beats";
            case C::oneBar:     return "1 Bar";
            case C::twoBar:     return "2 Bars";
            default:            return "Off";
        }
    }

    void showCountInMenu()
    {
        using C = te::Edit::CountIn;
        juce::PopupMenu m;
        m.addSectionHeader ("Count-in");

        for (auto [mode, name] : { std::pair { C::none, "Off" }, { C::oneBeat, "1 Beat" }, { C::twoBeat, "2 Beats" },
                                   { C::oneBar, "1 Bar" }, { C::twoBar, "2 Bars" } })
            m.addItem (name, true, edit.getCountInMode() == mode, [this, mode] { edit.setCountInMode (mode); refresh(); });

        m.showMenuAsync (juce::PopupMenu::Options().withTargetScreenArea (metronome.getChevronArea() + metronome.getScreenPosition()));
    }

    void showPreRollMenu()
    {
        juce::PopupMenu m;
        m.addSectionHeader ("Pre-roll before the punch-in");

        for (int bars : { 0, 1, 2 })
            m.addItem (bars == 0 ? "None" : bars == 1 ? "1 Bar" : "2 Bars", true, EngineHelpers::getPreRollBars (edit) == bars,
                       [this, bars] { EngineHelpers::setPreRollBars (edit, bars); });

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&punchButton));
    }

    /** During the count-in: the beats left before recording starts, otherwise 0. */
    int getCountInBeatsLeft() const
    {
        if (! transport.isRecording() || edit.getNumCountInBeats() == 0)
            return 0;

        auto& ts = edit.tempoSequence;
        auto left = ts.toBeats (EngineHelpers::recordStartTime()) - ts.toBeats (transport.getPosition());
        return left.inBeats() > 0.0 ? (int) std::ceil (left.inBeats()) : 0;
    }

    void applyLaunchQuantisation()
    {
        static const te::LaunchQType types[] = { te::LaunchQType::none, te::LaunchQType::sixteenth, te::LaunchQType::eighth,
                                                 te::LaunchQType::quarter, te::LaunchQType::half, te::LaunchQType::bar,
                                                 te::LaunchQType::twoBars, te::LaunchQType::fourBars };
        edit.getLaunchQuantisation().type = types[juce::jlimit (0, 7, quantBox.getSelectedId() - 1)];
    }

    void applyGrid()
    {
        static const double grids[] = { 0.0, 0.25, 0.5, 1.0, 2.0, -1.0, 0.0 };
        auto id = gridBox.getSelectedId();
        viewState.snapEnabled = id != 7;
        auto g = grids[juce::jlimit (0, 6, id - 1)];
        viewState.gridBeats = g < 0.0 ? viewState.getBeatsPerBar() : g;
        viewState.sendChangeMessage();
    }

    void refresh()
    {
        // Screen readers hear the transport start and stop, however it happened (a key, a
        // collaborator, a controller)
        if (transport.isRecording() != wasRecording || transport.isPlaying() != wasPlaying)
        {
            A11y::announce (transport.isRecording() ? (wasRecording ? "" : "Recording")
                                                    : transport.isPlaying() ? (wasRecording ? "Recording stopped, playing" : "Playing")
                                                                            : (wasRecording ? "Recording stopped" : "Stopped"));
            wasPlaying = transport.isPlaying();
            wasRecording = transport.isRecording();
        }

        playButton.setToggleState (transport.isPlaying(), juce::dontSendNotification);
        recordButton.setToggleState (transport.isRecording(), juce::dontSendNotification);
        loopButton.setToggleState (transport.looping.get(), juce::dontSendNotification);
        punchButton.setToggleState (EngineHelpers::isPunching (edit), juce::dontSendNotification);

        editModeBox.setSelectedId ((int) viewState.editMode + 1, juce::dontSendNotification);
        metronome.setState (edit.clickTrackEnabled.get(), edit.getCountInMode() != te::Edit::CountIn::none, countInName());
        automationArm.setToggleState (edit.getAutomationRecordManager().isWritingAutomation(), juce::dontSendNotification);
        midiLearn.setToggleState (edit.engine.getMidiLearnState().isActive(), juce::dontSendNotification);
        backToArrangement.setVisible (EngineHelpers::anyTrackPlayingSessionClips (edit));
        updateSideButtons();

        if (! tempo.isBeingEdited() && ! tempo.isMouseButtonDown())
            tempo.setValue (edit.tempoSequence.getTempo (0)->getBpm(), false);

        if (! timeSig.isBeingEdited())
            timeSig.setText (edit.tempoSequence.getTimeSig (0)->getStringTimeSig(), juce::dontSendNotification);
    }

    void timerCallback() override
    {
        // The count-in counts down in the position display, in red, as it does on a tape machine's remote
        if (auto left = getCountInBeatsLeft(); left > 0)
        {
            position.setText ("count in   " + juce::String (left), juce::dontSendNotification);
            position.setColour (juce::Label::textColourId, Theme::readable (Colours::record));
        }
        else
        {
            auto pos = transport.getPosition();
            position.setText (EngineHelpers::formatBarsBeats (edit, pos) + "   " + EngineHelpers::formatTime (pos), juce::dontSendNotification);
            position.setColour (juce::Label::textColourId, Colours::text);
        }

        // Dropouts: times the interface was kept waiting for its audio, counted from when the
        // transport last started, so a take that has a click in it says so (and keeps saying so
        // after it stops, until the next one starts)
        auto xruns = edit.engine.getDeviceManager().deviceManager.getXRunCount();
        const bool running = transport.isPlaying() || transport.isRecording();

        if (running && ! countingDropouts)
            xrunsAtStart = xruns;

        if (running)
        {
            auto now = juce::jmax (0, xruns - xrunsAtStart);

            if (now > dropouts && transport.isRecording() && ! saidDropout)
            {
                A11y::announce ("Dropout while recording");
                saidDropout = true;
            }

            if ((now > 0) != (dropouts > 0))
            {
                dropouts = now;
                resized();
            }

            dropouts = now;
        }

        if (running != countingDropouts && running)
            saidDropout = false;

        countingDropouts = running;

        auto load = edit.engine.getDeviceManager().getCpuUsage();
        auto loadText = "CPU " + juce::String (juce::roundToInt (load * 100.0f)) + "%";
        cpu.setText (dropouts > 0 ? juce::String (dropouts) + (dropouts == 1 ? " dropout   " : " dropouts   ") + loadText : loadText,
                     juce::dontSendNotification);
        cpu.setColour (juce::Label::textColourId, load > 0.8f || dropouts > 0 ? Theme::readable (Colours::record) : Colours::textDim);
        cpu.setTooltip (dropouts > 0 ? "The audio interface was kept waiting " + juce::String (dropouts) + (dropouts == 1 ? " time" : " times")
                                         + " since playback last started: each can be a click. If it happens while recording, "
                                           "raise the buffer size in Audio Settings or freeze heavy tracks."
                                     : juce::String ("Processor load of the audio engine"));

        refresh();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
};

} // namespace elsewhere
