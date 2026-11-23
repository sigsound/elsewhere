// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "TrackHeader.h"
#include "devices/DeviceBase.h"

namespace elsewhere
{
//==============================================================================
/**
    A full channel strip, top to bottom: input and output, inserts (the track's devices and
    plug-ins), sends, pan, the fader with its meter and level, record / solo / mute, and the
    track's name. For audio and MIDI tracks, groups, returns and the master.
*/
class MixerStrip : public juce::Component, private juce::Timer
{
public:
    static constexpr int width = 96, insertRowH = 19, sendRowH = 26;

    MixerStrip (EditViewState& evs, te::Track& t) : viewState (evs), track (&t)
    {
        audioTrack = dynamic_cast<te::AudioTrack*> (&t);
        folderTrack = dynamic_cast<te::FolderTrack*> (&t);
        isMaster = t.isMasterTrack();
        isReturn = Mixing::isReturnTrack (t);

        // Screen readers see each strip as a group named after its track, holding its controls
        setFocusContainerType (juce::Component::FocusContainerType::focusContainer);

        te::VolumeAndPanPlugin* vp = nullptr;
        te::LevelMeterPlugin* lm = nullptr;

        if (isMaster)
        {
            vp = t.edit.getMasterVolumePlugin().get();

            for (auto p : t.edit.getMasterPluginList())
                if (auto m = dynamic_cast<te::LevelMeterPlugin*> (p))
                    lm = m;
        }
        else
        {
            vp = audioTrack != nullptr ? audioTrack->getVolumePlugin() : folderTrack != nullptr ? folderTrack->getVolumePlugin() : nullptr;
            lm = Mixing::findMeter (t);
        }

        if (vp != nullptr)
        {
            volumePlugin = vp;
            volume = std::make_unique<ParameterSlider> (*vp->volParam, juce::Slider::LinearVertical);
            pan = std::make_unique<ParameterSlider> (*vp->panParam, juce::Slider::RotaryHorizontalVerticalDrag);
            volume->setTooltip ("Volume. Double-click for 0 dB");
            pan->setTooltip ("Pan. Double-click to centre");
            addAndMakeVisible (*volume);
            addAndMakeVisible (*pan);
        }

        if (lm != nullptr)
        {
            meter = std::make_unique<LevelMeter> (lm->measurer);
            addAndMakeVisible (*meter);
        }

        level.setJustificationType (juce::Justification::centred);
        level.setFont (juce::FontOptions (11.0f));
        level.setColour (juce::Label::textColourId, Colours::text);
        level.setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        level.setColour (juce::Label::textColourId, Colours::textDim);
        level.setDescription ("Fader level");
        addAndMakeVisible (level);

        // Input (recording tracks) and output
        if (audioTrack != nullptr && ! isReturn)
        {
            inputButton.onClick = [this] { showInputMenu (*audioTrack, &inputButton); };
            inputButton.setTooltip ("Input");
            inputButton.setDescription ("Input");
            styleRoutingButton (inputButton);
            addAndMakeVisible (inputButton);

            armButton.onClick = [this]
            {
                if (! EngineHelpers::trackHasInput (*audioTrack))
                    return showInputMenu (*audioTrack, &inputButton);

                EngineHelpers::armTrack (*audioTrack, ! EngineHelpers::isTrackArmed (*audioTrack));
                refresh();
            };
            addAndMakeVisible (armButton);
        }

        outputButton.onClick = [this] { showOutputMenu (*track, &outputButton); };
        outputButton.setTooltip ("Output");
        outputButton.setDescription ("Output");
        styleRoutingButton (outputButton);
        addAndMakeVisible (outputButton);

        if (! isMaster)
        {
            muteButton.onClick = [this] { track->setMute (! track->isMuted (false)); refresh(); };
            soloButton.onClick = [this] { Mixing::clickSolo (*track); refresh(); };
            addAndMakeVisible (muteButton);
            addAndMakeVisible (soloButton);
        }

        // Inserts
        for (auto p : EngineHelpers::getUserPlugins (t))
        {
            auto* b = insertButtons.add (new juce::TextButton (p->getName()));
            b->setColour (juce::TextButton::buttonColourId, Colours::panelLight);
            b->setTooltip (p->getName() + ". Click to open, right-click for more");
            b->onClick = [this, ref = te::Plugin::Ptr (p)]
            {
                if (juce::ModifierKeys::currentModifiers.isPopupMenu())
                    return;

                openPlugin (*ref);
            };
            b->addMouseListener (this, false);
            plugins.add (p);
            addAndMakeVisible (b);
        }

        addInsert.setTooltip ("Add a device or plug-in");
        addInsert.setTitle ("Add Device");
        addInsert.setColour (juce::TextButton::buttonColourId, Colours::panelDark);
        addInsert.setColour (juce::TextButton::textColourOffId, Colours::textDim);
        addInsert.onClick = [this] { PluginMenu::showForTrack (*track, -1, &addInsert, [this] (te::Plugin& p) { openPlugin (p); }); };
        addAndMakeVisible (addInsert);

        moreInserts.setColour (juce::TextButton::buttonColourId, Colours::panelDark);
        moreInserts.setColour (juce::TextButton::textColourOffId, Colours::textDim);
        moreInserts.onClick = [this] { showDevices(); };
        addChildComponent (moreInserts);

        // Sends, one knob per return
        for (auto s : Mixing::getSends (t))
            addAndMakeVisible (sendKnobs.add (new SendKnob (t, *s)));

        refresh();
        startTimerHz (5);
    }

    te::Track* getTrack() const     { return track.get(); }
    int getNumInserts() const       { return plugins.size(); }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<A11y::Handler> (*this, juce::AccessibilityRole::group,
            A11y::Handler::Info { [this] { return TrackSpeech::name (*track); }, [this] { return TrackSpeech::kind (*track); }, {}, {} },
            juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this]
            {
                if (! isMaster)
                    viewState.focusTrack (track.get());
            }));
    }

    /** All the strips line up: the mixer gives each one the same number of insert and send rows. */
    void setRows (int inserts, int sends)
    {
        insertRows = inserts;
        sendRows = sends;
        resized();
    }

    std::function<void()> onShowDevices;

    void paint (juce::Graphics& g) override
    {
        bool focused = viewState.focusedTrack == track.get();
        auto c = getColour();

        // Orbit: each strip is a soft rounded panel on the background, raised when focused
        g.fillAll (Colours::background);
        g.setColour (focused ? Theme::raisedSurface() : Colours::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat().reduced (3.0f, 0.0f).withTrimmedTop (8.0f).withTrimmedBottom (2.0f), 16.0f);

        if (volume != nullptr)
        {
            if (isMaster || isReturn)
                volume->removeColour (juce::Slider::trackColourId);
            else
                volume->setColour (juce::Slider::trackColourId, c);
        }

        // The name at the bottom, after the track's colour disc
        auto name = getLocalBounds().removeFromBottom (nameH).reduced (8, 3);
        if (! isMaster && ! isReturn)
        {
            g.setColour (c);
            g.fillEllipse (name.removeFromLeft (12).toFloat().withSizeKeepingCentre (folderTrack != nullptr ? 12.0f : 10.0f, folderTrack != nullptr ? 12.0f : 10.0f));
            name.removeFromLeft (5);
        }
        g.setColour (isMaster || isReturn ? Colours::textDim : Colours::text);
        g.setFont (juce::FontOptions (12.0f, folderTrack != nullptr ? juce::Font::bold : juce::Font::plain));
        g.drawFittedText (getDisplayName(), name, juce::Justification::centredLeft, 2, 1.0f);   // never squeezed: two lines, then "..."

        // Section labels
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.5f));

        if (! insertsLabel.isEmpty())
            g.drawText ("inserts", insertsLabel, juce::Justification::centredLeft);

        if (! sendsLabel.isEmpty() && ! sendKnobs.isEmpty())
            g.drawText ("sends", sendsLabel, juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds().reduced (10, 0);
        r.removeFromTop (16);

        // Routing
        if (inputButton.isVisible())
            inputButton.setBounds (r.removeFromTop (18));
        else
            r.removeFromTop (18);

        r.removeFromTop (3);
        outputButton.setBounds (r.removeFromTop (18));
        r.removeFromTop (6);

        // Inserts: as many as fit in the rows, then "N more", then +
        insertsLabel = r.removeFromTop (12);
        auto shown = juce::jmin (plugins.size(), insertRows - 1);
        bool overflow = plugins.size() > insertRows - 1;

        if (overflow)
            shown = insertRows - 2;

        for (int i = 0; i < insertButtons.size(); ++i)
        {
            insertButtons[i]->setVisible (i < shown);

            if (i < shown)
                insertButtons[i]->setBounds (r.removeFromTop (insertRowH).reduced (0, 1));
        }

        moreInserts.setVisible (overflow);

        if (overflow)
        {
            moreInserts.setButtonText ("+" + juce::String (plugins.size() - shown) + " more");
            moreInserts.setBounds (r.removeFromTop (insertRowH).reduced (0, 1));
        }

        addInsert.setBounds (r.removeFromTop (insertRowH).reduced (0, 1));
        r.removeFromTop ((insertRows - 1 - shown - (overflow ? 1 : 0)) * insertRowH);
        r.removeFromTop (6);

        // Sends: two to a row
        sendsLabel = sendRows > 0 ? r.removeFromTop (12) : juce::Rectangle<int>();
        auto sendsArea = r.removeFromTop (sendRows * sendRowH);

        for (int i = 0; i < sendKnobs.size(); ++i)
        {
            auto row = sendsArea.withHeight (sendRowH).translated (0, (i / 2) * sendRowH);
            auto cellW = row.getWidth() / 2;
            sendKnobs[i]->setBounds (row.withWidth (cellW).translated ((i % 2) * cellW, 0).reduced (1));
            sendKnobs[i]->setVisible (i / 2 < sendRows);
        }

        r.removeFromTop (4);

        if (pan != nullptr)
            pan->setBounds (r.removeFromTop (34).withSizeKeepingCentre (34, 34));

        r.removeFromTop (4);

        // Bottom up: name, buttons, level, then the fader fills the rest
        r.removeFromBottom (nameH);
        auto buttons = r.removeFromBottom (20);
        r.removeFromBottom (4);
        level.setBounds (r.removeFromBottom (18));
        r.removeFromBottom (4);

        // Three discs: arm, solo, mute
        auto bw = buttons.getWidth() / 3;
        auto disc = [] (juce::Rectangle<int> b) { return b.withSizeKeepingCentre (20, 20); };
        if (armButton.isVisible()) armButton.setBounds (disc (buttons.removeFromLeft (bw)));
        else buttons.removeFromLeft (bw);
        soloButton.setBounds (disc (buttons.removeFromLeft (bw)));
        muteButton.setBounds (disc (buttons));

        auto fader = r;

        if (meter != nullptr)
            meter->setBounds (fader.removeFromRight (16).withSizeKeepingCentre (9, fader.getHeight() - 8));

        if (volume != nullptr)
            volume->setBounds (fader.withSizeKeepingCentre (juce::jmin (fader.getWidth(), 44), fader.getHeight()));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Right-click on an insert: its menu
        for (int i = 0; i < insertButtons.size(); ++i)
            if (e.eventComponent == insertButtons[i] && e.mods.isPopupMenu())
                return showInsertMenu (*plugins[i]);

        if (e.eventComponent != this)
            return;

        if (! isMaster)
        {
            viewState.focusTrack (track.get());
            viewState.selectionManager.selectOnly (track.get());
        }

        if (e.mods.isPopupMenu() && e.y > getHeight() - nameH && ! isMaster)
            showTrackMenu (*track, viewState, [this] { rename(); });
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.eventComponent == this && e.y > getHeight() - nameH && ! isMaster)
            rename();
    }

private:
    static constexpr int nameH = 34;

    EditViewState& viewState;
    te::Track::Ptr track;
    te::AudioTrack* audioTrack = nullptr;
    te::FolderTrack* folderTrack = nullptr;
    bool isMaster = false, isReturn = false;
    te::VolumeAndPanPlugin::Ptr volumePlugin;

    juce::TextButton inputButton, outputButton, addInsert { "+" }, moreInserts;
    juce::OwnedArray<juce::TextButton> insertButtons;
    juce::Array<te::Plugin*> plugins;
    juce::OwnedArray<SendKnob> sendKnobs;
    std::unique_ptr<ParameterSlider> volume, pan;
    std::unique_ptr<LevelMeter> meter;
    juce::Label level;
    SmallToggle muteButton { "m", Colours::mute, "Mute" }, soloButton { "s", Colours::solo, "Solo" },
                armButton { juce::String::fromUTF8 ("\xe2\x97\x8f"), Colours::record, "Record Arm" };
    int insertRows = 5, sendRows = 1;
    juce::Rectangle<int> insertsLabel, sendsLabel;

    static void styleRoutingButton (juce::TextButton& b)
    {
        b.setColour (juce::TextButton::buttonColourId, Colours::panelDark);
        b.setColour (juce::TextButton::textColourOffId, Colours::textDim);
    }

    juce::Colour getColour() const
    {
        if (isMaster)
            return Colours::accent;

        return Theme::ofTrack (*track);
    }

    juce::String getDisplayName() const
    {
        if (isMaster)
            return "Main";

        return track->getName();
    }

    void openPlugin (te::Plugin& p)
    {
        // Elsewhere's own devices show in the panel below; plug-ins open their window
        if (dynamic_cast<ElsewhereDevice*> (&p) != nullptr)
            showDevices();
        else
            p.showWindowExplicitly();
    }

    void showDevices()
    {
        if (! isMaster)
            viewState.focusTrack (track.get());

        viewState.detailMode = EditViewState::DetailMode::devices;
        viewState.sendChangeMessage();

        if (onShowDevices)
            onShowDevices();
    }

    void showInsertMenu (te::Plugin& p)
    {
        auto ref = te::Plugin::Ptr (&p);
        juce::PopupMenu m;
        m.addItem ("Open", [this, ref] { openPlugin (*ref); });
        m.addItem (p.isEnabled() ? "Bypass" : "Turn On", [ref] { ref->setEnabled (! ref->isEnabled()); });
        m.addSeparator();
        m.addItem ("Remove", [ref, &edit = track->edit]
        {
            edit.getUndoManager().beginNewTransaction ("Remove " + ref->getName());
            ref->deleteFromParent();
        });
        m.showMenuAsync ({});
    }

    void rename()
    {
        auto* aw = Theme::newAlertWindow ("Rename Track", {});
        Theme::addTextField (*aw, "name", track->getName());
        aw->addButton ("OK", 1, juce::KeyPress (juce::KeyPress::returnKey));
        aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
        aw->enterModalState (true, juce::ModalCallbackFunction::create ([aw, t = track] (int res)
        {
            if (res == 1)
                t->setName (aw->getTextEditorContents ("name"));
        }), true);
    }

    void refresh()
    {
        muteButton.setToggleState (track->isMuted (false), juce::dontSendNotification);
        soloButton.setToggleState (track->isSolo (false), juce::dontSendNotification);

        if (audioTrack != nullptr && ! isReturn)
        {
            armButton.setToggleState (EngineHelpers::isTrackArmed (*audioTrack), juce::dontSendNotification);
            inputButton.setButtonText (getInputName (*audioTrack));
        }

        outputButton.setButtonText (getOutputName (*track));

        // A bypassed insert is dimmed (still readable) and struck through
        for (int i = 0; i < insertButtons.size(); ++i)
        {
            auto* b = insertButtons[i];
            const bool on = plugins[i]->isEnabled();
            b->setColour (juce::TextButton::textColourOffId, on ? Colours::text : Colours::textDim);

            if ((bool) b->getProperties()["struckThrough"] == on)
            {
                b->getProperties().set ("struckThrough", ! on);
                b->setDescription (on ? juce::String() : juce::String ("bypassed"));
                b->repaint();
            }
        }

        if (volumePlugin != nullptr)
        {
            auto db = volumePlugin->getVolumeDb();
            level.setText (db <= -96.0f ? juce::String::fromUTF8 ("-\xe2\x88\x9e") : juce::String (db, 1) + " dB", juce::dontSendNotification);
        }

        repaint();
    }

    void timerCallback() override { refresh(); }
};

//==============================================================================
/**
    The Mix view: a channel strip for every track, group and return, in the arrangement's
    order, with the master always at the right.
*/
class MixerView : public juce::Component,
                  private juce::ValueTree::Listener,
                  private juce::AsyncUpdater,
                  private juce::ChangeListener
{
public:
    MixerView (EditViewState& evs) : viewState (evs)
    {
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (false, true);
        viewport.setScrollBarThickness (10);

        viewState.edit.state.addListener (this);
        viewState.addChangeListener (this);
        rebuild();
    }

    ~MixerView() override
    {
        viewState.edit.state.removeListener (this);
        viewState.removeChangeListener (this);
    }

    /** Called when an insert that shows in the panel below is opened. */
    std::function<void()> onShowDevices;

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::background);

        if (strips.isEmpty())
        {
            g.setColour (Colours::textDim);
            g.drawText ("No tracks yet", getLocalBounds().withTrimmedRight (MixerStrip::width), juce::Justification::centred);
        }
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        juce::ignoreUnused (g);
    }

    void resized() override
    {
        auto r = getLocalBounds();

        if (master != nullptr)
        {
            master->setBounds (r.removeFromRight (MixerStrip::width).withTrimmedBottom (viewport.getScrollBarThickness()));
            r.removeFromRight (3);
        }

        viewport.setBounds (r);
        layout();
    }

private:
    EditViewState& viewState;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<MixerStrip> strips;
    std::unique_ptr<MixerStrip> master;
    te::Track::Ptr lastFocused;
    std::set<te::EditItemID> lastCollapsed;

    void rebuild()
    {
        auto scroll = viewport.getViewPositionX();
        strips.clear();
        master.reset();

        for (auto t : te::getAllTracks (viewState.edit))
        {
            if (t->isMasterTrack() || ! (t->isAudioTrack() || t->isFolderTrack()) || EditViewState::isTrackHidden (*t)
                 || viewState.isHiddenByCollapsedGroup (*t))
                continue;

            auto* s = strips.add (new MixerStrip (viewState, *t));
            s->onShowDevices = [this] { if (onShowDevices) onShowDevices(); };
            content.addAndMakeVisible (s);
        }

        if (auto mt = viewState.edit.getMasterTrack())
        {
            master = std::make_unique<MixerStrip> (viewState, *mt);
            master->onShowDevices = [this] { if (onShowDevices) onShowDevices(); };
            addAndMakeVisible (*master);
        }

        // Every strip gets the same rows, so the faders line up
        int inserts = master != nullptr ? master->getNumInserts() : 0;
        for (auto* s : strips)
            inserts = juce::jmax (inserts, s->getNumInserts());

        auto rows = juce::jlimit (4, 8, inserts + 1);
        auto buses = Mixing::getReturnBuses (viewState.edit).size();
        auto sendRows = (buses + 1) / 2;

        for (auto* s : strips)
            s->setRows (rows, sendRows);

        if (master != nullptr)
            master->setRows (rows, sendRows);

        resized();
        viewport.setViewPosition (scroll, 0);
    }

    void layout()
    {
        auto h = viewport.getMaximumVisibleHeight();
        int x = 0;

        for (auto* s : strips)
        {
            s->setBounds (x, 0, MixerStrip::width, h);
            x += MixerStrip::width;
        }

        content.setSize (juce::jmax (x, viewport.getMaximumVisibleWidth()), h);
    }

    /** Keeps the focused track's strip in view. */
    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        // A closed group is one strip
        if (viewState.collapsedGroups != lastCollapsed)
        {
            lastCollapsed = viewState.collapsedGroups;
            triggerAsyncUpdate();
        }

        auto focused = viewState.focusedTrack.get();

        if (focused == lastFocused.get())
            return repaint();

        lastFocused = focused;

        for (auto* s : strips)
        {
            if (s->getTrack() == focused)
            {
                auto view = viewport.getViewArea();

                if (s->getX() < view.getX() || s->getRight() > view.getRight())
                    viewport.setViewPosition (juce::jmax (0, s->getX() - MixerStrip::width), 0);
            }

            s->repaint();
        }
    }

    void handleAsyncUpdate() override { rebuild(); }

    static bool isStructural (const juce::ValueTree& c)
    {
        return te::TrackList::isTrack (c) || c.hasType (te::IDs::PLUGIN);
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override            { if (isStructural (c)) triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override     { if (isStructural (c)) triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override             { if (p.hasType (te::IDs::EDIT) || p.hasType (te::IDs::FOLDERTRACK)) triggerAsyncUpdate(); }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        if ((te::TrackList::isTrack (v) && (id == te::IDs::name || id == te::IDs::colour || id == EditViewState::hiddenProperty))
             || (v.hasType (te::IDs::PLUGIN) && id == te::IDs::name))
            triggerAsyncUpdate();
    }
};

} // namespace elsewhere
