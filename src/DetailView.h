// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "ArrangementView.h"
#include "AudioEditor.h"
#include "Presets.h"
#include "devices/DeviceBase.h"
#include "Modulators.h"
#include "MidiLanes.h"
#include "Routing.h"
#include "drums/DrumSectionEditor.h"

namespace elsewhere
{
//==============================================================================
/** One device in the chain: title bar with on/off, name and editor button (and the key it
    listens to, for devices with a sidechain), plus knobs. */
class DevicePanel : public juce::Component, private juce::ValueTree::Listener
{
public:
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<A11y::Handler> (*this, juce::AccessibilityRole::group,
            A11y::Handler::Info { [this] { return plugin != nullptr ? plugin->getName() : juce::String(); },
                                  [this] { return plugin != nullptr && ! plugin->isEnabled() ? juce::String ("off") : juce::String(); }, {}, {} });
    }

    DevicePanel (te::Plugin& p) : plugin (&p)
    {
        power.setClickingTogglesState (true);
        power.setToggleState (p.isEnabled(), juce::dontSendNotification);
        power.setColour (juce::TextButton::buttonOnColourId, Colours::accent);
        power.onClick = [this] { plugin->setEnabled (power.getToggleState()); repaint(); };
        power.setTitle ("Device On");
        addAndMakeVisible (power);

        // Screen readers see a group named after the device (its name is painted), holding its controls
        setFocusContainerType (juce::Component::FocusContainerType::focusContainer);

        editButton.onClick = [this] { plugin->showWindowExplicitly(); };
        editButton.setTitle ("Open Plug-in Window");
        editButton.setTooltip ("Open plugin window");
        addAndMakeVisible (editButton);

        menuButton.onClick = [this] { showMenu(); };
        menuButton.setTitle ("Device Menu");
        addAndMakeVisible (menuButton);

        // Match level: the device's output follows the loudness of its input, so turning it on and
        // off compares its sound, not its volume. Instruments make their own sound, so they don't get it.
        if (! p.isSynth())
        {
            matchButton.setToggleState (p.isGainMatched(), juce::dontSendNotification);
            matchButton.onClick = [this] { setGainMatched (! plugin->isGainMatched()); };
            matchButton.setTooltip ("Match level: keep this device's output as loud as its input, "
                                    "so turning it on and off compares the sound, not the volume");
            addAndMakeVisible (matchButton);
        }

        plugin->state.addListener (this);

        // A key input: which track this device listens to
        if (Routing::canSidechain (p))
        {
            keyButton.onClick = [this] { showKeyMenu(); };
            keyButton.setTitle ("Sidechain");
            keyButton.setTooltip ("Sidechain: let another track's sound drive this device");
            addAndMakeVisible (keyButton);
            updateKey();
        }

        // Elsewhere's own devices bring their full panel into the chain
        if (auto device = dynamic_cast<ElsewhereDevice*> (&p))
        {
            deviceView = device->createDeviceView();
            deviceWidth = device->getDeviceViewWidth();
            addAndMakeVisible (*deviceView);
            return;
        }

        auto isExternal = dynamic_cast<te::ExternalPlugin*> (&p) != nullptr;
        int count = 0;

        for (auto param : p.getAutomatableParameters())
        {
            if (++count > (isExternal ? 8 : 16))
                break;

            auto* k = knobs.add (new ParameterSlider (*param, juce::Slider::RotaryHorizontalVerticalDrag));
            addAndMakeVisible (k);

            auto* l = labels.add (new juce::Label ({}, param->getParameterName()));
            l->setFont (juce::FontOptions (10.0f));
            l->setJustificationType (juce::Justification::centred);
            l->setColour (juce::Label::textColourId, Colours::textDim);
            l->setMinimumHorizontalScale (0.6f);
            addAndMakeVisible (l);
        }
    }

    ~DevicePanel() override     { plugin->state.removeListener (this); }

    std::function<void()> onChanged;

    int getIdealWidth() const
    {
        if (deviceView != nullptr)
            return deviceWidth + 16;

        auto cols = juce::jmax (2, (knobs.size() + 1) / 2);
        return juce::jmax (150, cols * knobW + 12);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::panel);
        g.fillRoundedRectangle (r, 3.0f);

        auto title = r.removeFromTop (22.0f);
        g.setColour (plugin->isEnabled() ? Colours::panelLight : Colours::panelDark);
        g.fillRoundedRectangle (title, 3.0f);

        g.setColour (plugin->isEnabled() ? Colours::text : Colours::textDim);
        g.setFont (juce::FontOptions (12.0f, juce::Font::bold));
        g.drawText (plugin->getName(), title.toNearestInt().withTrimmedLeft (26).withTrimmedRight (52 + (keyButton.isVisible() ? keyButton.getWidth() + 4 : 0)
                                                                                                   + (matchButton.isVisible() ? matchButton.getWidth() + 4 : 0)),
                    juce::Justification::centredLeft, true);

        if (knobs.isEmpty() && deviceView == nullptr)
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (11.0f));
            g.drawFittedText ("Click the window button to open this plugin's editor",
                              getLocalBounds().withTrimmedTop (22).reduced (8), juce::Justification::centred, 3);
        }

        g.setColour (Colours::outline);
        g.drawRoundedRectangle (getLocalBounds().toFloat().reduced (0.5f), 3.0f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto title = r.removeFromTop (22).reduced (3);
        power.setBounds (title.removeFromLeft (16));
        menuButton.setBounds (title.removeFromRight (20));
        title.removeFromRight (2);
        editButton.setBounds (title.removeFromRight (24));

        if (matchButton.isVisible())
        {
            title.removeFromRight (4);
            matchButton.setBounds (title.removeFromRight (juce::jmin (44, title.getWidth() / 3)));
        }

        if (keyButton.isVisible())
        {
            title.removeFromRight (4);
            auto w = juce::jlimit (34, 120, keyButton.getBestWidthForHeight (title.getHeight()));
            keyButton.setBounds (title.removeFromRight (juce::jmin (w, title.getWidth() / 2)));
        }

        r.reduce (6, 4);

        if (deviceView != nullptr)
        {
            deviceView->setBounds (r.reduced (2, 2));
            return;
        }

        auto rowH = r.getHeight() / 2;

        for (int i = 0; i < knobs.size(); ++i)
        {
            auto col = i / 2, row = i % 2;
            auto cell = juce::Rectangle<int> (r.getX() + col * knobW, r.getY() + row * rowH, knobW, rowH);
            labels[i]->setBounds (cell.removeFromBottom (14));
            knobs[i]->setBounds (cell.reduced (4, 0));
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (e.mods.isPopupMenu())
            showMenu();
        else
            onTitle = e.y < titleH;
    }

    // Drag a device by its title bar to move it along the chain
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (! onTitle || (! dragging && e.getDistanceFromDragStart() < 6))
            return;

        dragging = true;
        if (onDrag) onDrag (*this, e);
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging && onDrop)
            onDrop (*this);

        onTitle = dragging = false;
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (e.y < titleH ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    std::function<void (DevicePanel&, const juce::MouseEvent&)> onDrag;
    std::function<void (DevicePanel&)> onDrop;
    te::Plugin& getPlugin()     { return *plugin; }

private:
    static constexpr int knobW = 58, titleH = 22;
    bool onTitle = false, dragging = false;
    te::Plugin::Ptr plugin;
    SmallToggle keyButton { "key", Colours::text };
    SmallToggle matchButton { "match", Colours::text, "Match Level" };
    juce::TextButton power, editButton { juce::String::fromUTF8 ("\xe2\xa7\x89") }, menuButton { juce::String::fromUTF8 ("\xe2\x8b\xaf") };
    juce::OwnedArray<ParameterSlider> knobs;
    juce::OwnedArray<juce::Label> labels;
    std::unique_ptr<juce::Component> deviceView;
    int deviceWidth = 0;

    void updateKey()
    {
        auto source = Routing::getSidechainSource (*plugin);
        auto missing = Routing::hasMissingSidechain (*plugin);
        keyButton.setButtonText (source != nullptr ? juce::String::fromUTF8 ("key \xc2\xb7 ") + source->getName()
                                 : missing ? juce::String ("key missing") : juce::String ("key"));
        keyButton.setToggleState (source != nullptr || missing, juce::dontSendNotification);
        keyButton.setTooltip (source != nullptr ? plugin->getName() + " listens to " + source->getName()
                                                : "Sidechain: let another track's sound drive this device");
        resized();
        repaint();
    }

    void showKeyMenu()
    {
        juce::PopupMenu m;
        te::Plugin::Ptr p = plugin;
        auto current = Routing::getSidechainSource (*p);

        m.addItem ("None", true, current == nullptr && ! Routing::hasMissingSidechain (*p), [p] { Routing::setSidechain (*p, nullptr); });
        m.addSectionHeader ("Listen to");

        for (auto t : te::getAudioTracks (p->edit))
        {
            if (t == p->getOwnerTrack())
                continue;

            // A track that already hears this one would make a loop
            juce::PopupMenu::Item item (t->getName());
            item.setEnabled (Routing::canSidechainFrom (*p, *t));
            item.setTicked (t == current);
            item.setAction ([p, ref = te::AudioTrack::Ptr (t)] { Routing::setSidechain (*p, ref.get()); });
            m.addItem (std::move (item));
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&keyButton));
    }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        if (v == plugin->state && id == te::IDs::sidechainSourceID)
            updateKey();

        if (v == plugin->state && id == juce::Identifier ("gainMatch"))
            matchButton.setToggleState (plugin->isGainMatched(), juce::dontSendNotification);
    }

    void setGainMatched (bool b)
    {
        plugin->edit.getUndoManager().beginNewTransaction (b ? "Match Level" : "Stop Matching Level");
        plugin->setGainMatched (b);
    }

    void showMenu()
    {
        juce::PopupMenu m;
        auto p = plugin;
        auto* list = p->getOwnerList();

        m.addItem ("Open Editor", [p] { p->showWindowExplicitly(); });

        if (! p->isSynth())
            m.addItem ("Match Level", true, p->isGainMatched(), [this] { setGainMatched (! plugin->isGainMatched()); });

        m.addSeparator();

        m.addItem ("Save Preset...", [p]
        {
            auto* aw = Theme::newAlertWindow ("Save Preset", "Name this " + p->getName() + " preset:");
            Theme::addTextField (*aw, "name", "My " + p->getName());
            aw->addButton ("Save", 1, juce::KeyPress (juce::KeyPress::returnKey));
            aw->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
            aw->enterModalState (true, juce::ModalCallbackFunction::create ([p, aw] (int r)
            {
                if (r == 1 && aw->getTextEditorContents ("name").trim().isNotEmpty())
                    Library::savePreset (*p, aw->getTextEditorContents ("name").trim());
            }), true);
        });

        juce::PopupMenu presets;
        for (auto f : Library::getPresets (*p))
            presets.addItem (f.getFileNameWithoutExtension(), [p, f, this]
            {
                if (Library::loadPreset (*p, f) && onChanged)
                    onChanged();
            });
        if (presets.getNumItems() == 0)
            presets.addItem ("No presets saved yet", false, false, [] {});
        m.addSubMenu ("Load Preset", presets);
        m.addItem ("Show Presets in " + fileManagerName(), [p]
        {
            auto folder = Library::getPresetFolder (*p);
            folder.createDirectory();
            folder.revealToUser();
        });
        m.addSeparator();

        if (list != nullptr)
        {
            auto index = list->indexOf (p.get());
            m.addItem ("Move Left", index > 0, false, [p, list, index, this]
            {
                p->removeFromParent();
                list->insertPlugin (p, index - 1, nullptr);
                if (onChanged) onChanged();
            });
            m.addItem ("Move Right", true, false, [p, list, index, this]
            {
                p->removeFromParent();
                list->insertPlugin (p, index + 1, nullptr);
                if (onChanged) onChanged();
            });
        }

        m.addSeparator();
        m.addItem ("Delete", [p] { p->deleteFromParent(); });
        m.showMenuAsync ({});
    }
};

//==============================================================================
/** The device chain for the focused track. */
class DeviceChainView : public juce::Component,
                        private juce::ChangeListener,
                        private juce::ValueTree::Listener,
                        private juce::AsyncUpdater
{
public:
    DeviceChainView (EditViewState& evs) : viewState (evs)
    {
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (false, true);
        viewport.setScrollBarThickness (8);

        addButton.onClick = [this]
        {
            if (auto t = viewState.focusedTrack.get())
                PluginMenu::showForTrack (*t, -1, &addButton, [] (te::Plugin& p)
                {
                    // Elsewhere devices show inline in the chain; plug-ins open their window
                    if (dynamic_cast<ElsewhereDevice*> (&p) == nullptr)
                        p.showWindowExplicitly();
                });
        };
        content.addAndMakeVisible (addButton);

        viewState.addChangeListener (this);
        rebuild();
    }

    ~DeviceChainView() override
    {
        viewState.removeChangeListener (this);
        watch (nullptr);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panelDark);

        if (watchedTrack == nullptr)
        {
            g.setColour (Colours::textDim);
            g.drawText ("Select a track to see its devices", getLocalBounds(), juce::Justification::centred);
        }
        else if (panels.isEmpty() && lfoPanels.isEmpty())
        {
            g.setColour (Colours::textDim);
            g.drawText ("Drop an instrument or effect here: click + to add a device (built-in, VST3 or AU)",
                        getLocalBounds().withTrimmedLeft (60), juce::Justification::centred);
        }
    }

    void resized() override
    {
        viewport.setBounds (getLocalBounds());
        layout();
    }

private:
    EditViewState& viewState;
    juce::Viewport viewport;
    juce::Component content;
    juce::OwnedArray<DevicePanel> panels;
    juce::OwnedArray<LfoPanel> lfoPanels;
    juce::TextButton addButton { "+" };
    te::Track::Ptr watchedTrack;
    juce::ValueTree watchedPlugins;    // where its devices live: the track itself, or the edit's master list for the master track

    void watch (te::Track* t)
    {
        if (watchedTrack != nullptr)
            watchedTrack->state.removeListener (this);
        watchedPlugins.removeListener (this);

        watchedTrack = t;
        watchedPlugins = t != nullptr ? t->pluginList.state : juce::ValueTree();

        if (watchedTrack != nullptr)
            watchedTrack->state.addListener (this);
        if (watchedPlugins != (watchedTrack != nullptr ? watchedTrack->state : juce::ValueTree()))
            watchedPlugins.addListener (this);
    }

    void rebuild()
    {
        panels.clear();
        lfoPanels.clear();
        auto t = viewState.focusedTrack.get();

        if (watchedTrack.get() != t)
            watch (t);

        if (t != nullptr)
        {
            for (auto p : EngineHelpers::getUserPlugins (*t))
            {
                auto* panel = panels.add (new DevicePanel (*p));
                panel->onChanged = [this] { triggerAsyncUpdate(); };
                panel->onDrag = [this] (DevicePanel& dragged, const juce::MouseEvent& e) { dragOver (dragged, e); };
                panel->onDrop = [this] (DevicePanel& dragged) { drop (dragged); };
                content.addAndMakeVisible (panel);
            }

            // Modulators come after the devices they move
            if (auto ml = t->getModifierList())
                for (auto lfo : te::getModifiersOfType<te::LFOModifier> (*ml))
                    content.addAndMakeVisible (lfoPanels.add (new LfoPanel (*t, *lfo)));
        }

        addButton.setVisible (t != nullptr);
        layout();
        repaint();
    }

    void layout()
    {
        auto h = viewport.getMaximumVisibleHeight();
        int x = 8;

        for (auto* p : panels)
        {
            p->setBounds (x, 6, p->getIdealWidth(), h - 12);
            x += p->getIdealWidth() + 6;
        }

        for (auto* p : lfoPanels)
        {
            p->setBounds (x, 6, LfoPanel::idealWidth, h - 12);
            x += LfoPanel::idealWidth + 6;
        }

        addButton.setBounds (x, 6, 40, h - 12);
        content.setSize (juce::jmax (viewport.getMaximumVisibleWidth(), x + 60), h);
    }

    void handleAsyncUpdate() override { rebuild(); }

    //==============================================================================
    // Dragging a device along the chain
    struct DropMarker : public juce::Component
    {
        DropMarker()    { setInterceptsMouseClicks (false, false); }
        void paint (juce::Graphics& g) override     { g.setColour (Colours::accent); g.fillRoundedRectangle (getLocalBounds().toFloat(), 1.5f); }
    };

    DropMarker dropMarker;
    int dropSlot = -1;      // the gap the dragged device would go into: 0 is before the first

    void dragOver (DevicePanel& dragged, const juce::MouseEvent& e)
    {
        const int x = e.getEventRelativeTo (&content).x;
        dropSlot = panels.size();

        for (int i = 0; i < panels.size(); ++i)
            if (x < panels[i]->getBounds().getCentreX())
            {
                dropSlot = i;
                break;
            }

        const int at = dropSlot < panels.size() ? panels[dropSlot]->getX() - 5 : panels.getLast()->getRight() + 1;
        content.addAndMakeVisible (dropMarker);
        dropMarker.setBounds (at, 6, 4, content.getHeight() - 12);
        dropMarker.toFront (false);
        dragged.setAlpha (0.55f);

        // Near either end of what's showing, bring more of the chain into view
        const auto inView = e.getEventRelativeTo (&viewport);
        viewport.autoScroll (inView.x, viewport.getHeight() / 2, 40, 14);
    }

    void drop (DevicePanel& dragged)
    {
        dragged.setAlpha (1.0f);
        content.removeChildComponent (&dropMarker);
        const int from = panels.indexOf (&dragged), slot = dropSlot;
        dropSlot = -1;

        // (the gaps either side of a device are where it already is)
        if (from < 0 || slot < 0 || slot == from || slot == from + 1)
            return;

        te::Plugin::Ptr p = &dragged.getPlugin();
        auto* list = p->getOwnerList();
        if (list == nullptr)
            return;

        // Where that gap is in the track's own list, which also holds its fader and meter
        int index = slot < panels.size() ? list->indexOf (&panels[slot]->getPlugin())
                                         : list->indexOf (&panels.getLast()->getPlugin()) + 1;
        if (list->indexOf (p.get()) < index)
            --index;    // taking it out first moves everything after it down one

        p->edit.getUndoManager().beginNewTransaction ("Move Device");
        p->removeFromParent();
        list->insertPlugin (p, index, nullptr);
        triggerAsyncUpdate();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override
    {
        if (viewState.focusedTrack.get() != watchedTrack.get())
            triggerAsyncUpdate();
    }

    static bool isPluginTree (const juce::ValueTree& v) { return v.hasType (te::IDs::PLUGIN); }
    static bool isModifierTree (const juce::ValueTree& v) { return te::ModifierList::isModifier (v.getType()) || v.hasType (te::IDs::MODIFIERS); }

    void valueTreeChildAdded (juce::ValueTree& p, juce::ValueTree& c) override
    {
        if ((isPluginTree (c) && p == watchedPlugins) || isModifierTree (c)) triggerAsyncUpdate();
    }

    void valueTreeChildRemoved (juce::ValueTree& p, juce::ValueTree& c, int) override
    {
        if ((isPluginTree (c) && p == watchedPlugins) || isModifierTree (c)) triggerAsyncUpdate();
    }

    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override
    {
        if (p == watchedPlugins) triggerAsyncUpdate();
    }
};

//==============================================================================
/** Piano-roll editor for a MIDI clip. */
class PianoRoll : public juce::Component, private juce::ValueTree::Listener, private juce::Timer
{
public:
    static constexpr int keyboardW = 56;

    PianoRoll (EditViewState& evs, te::MidiClip& c) : viewState (evs), clip (&c)
    {
        clip->state.addListener (this);
        auto range = clip->getSequence().getNoteNumberRange();
        auto centreNote = clip->getSequence().getNumNotes() > 0 ? (range.getStart() + range.getEnd()) / 2 : 60;
        scrollY = juce::jmax (0.0f, (float) (127 - centreNote) * keyH - 120.0f);
        setWantsKeyboardFocus (true);
        startTimerHz (30);

        MidiLanes::Host host;
        host.beatToX = [this] (double b) { return beatToX (b); };
        host.xToBeat = [this] (float x) { return xToBeat (x); };
        host.snap = [this] (double b) { return snap (b, false); };
        host.getGrid = [this] { return getGrid(); };
        host.getSelection = [this] { return selectedNotes; };
        host.getColour = [this] { return getClipColour (*clip); };
        host.wheel = [this] (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
        {
            // Scrolling over the lane moves through time; up and down belongs to the notes
            if (e.mods.isCommandDown() || e.mods.isAltDown() || e.mods.isShiftDown() || std::abs (w.deltaX) > std::abs (w.deltaY))
                mouseWheelMove (e.getEventRelativeTo (this), w);
        };
        host.keyboardW = keyboardW;

        lane = std::make_unique<MidiLanes::Lane> (c, std::move (host));
        lane->onOpenChanged = [this] { resized(); };
        addAndMakeVisible (*lane);
    }

    ~PianoRoll() override { clip->state.removeListener (this); }

    te::MidiClip& getClip() { return *clip; }
    MidiLanes::Lane& getLane() { return *lane; }
    void setSelectedNotes (juce::Array<te::MidiNote*> notes) { selectedNotes = std::move (notes); repaint(); }

    void resized() override
    {
        lane->setBounds (getLocalBounds().removeFromBottom (lane->isOpen() ? MidiLanes::Lane::openHeight : MidiLanes::Lane::closedHeight));
        scrollY = juce::jlimit (0.0f, juce::jmax (0.0f, 128.0f * keyH - (float) getNotesHeight()), scrollY);
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds();
        auto keys = r.removeFromLeft (keyboardW);
        g.fillAll (Colours::background);

        // Rows
        for (int n = 0; n < 128; ++n)
        {
            auto y = noteToY (n);
            if (y > (float) getHeight() || y + keyH < 0.0f) continue;
            bool black = juce::MidiMessage::isMidiNoteBlack (n);
            g.setColour (black ? Colours::panelDark : Theme::lift (Colours::background, 0.03f));
            g.fillRect (juce::Rectangle<float> ((float) r.getX(), y, (float) r.getWidth(), keyH));

            if (n % 12 == 0)
            {
                g.setColour (Colours::gridBar);
                g.drawHorizontalLine (juce::roundToInt (y + keyH), (float) r.getX(), (float) r.getRight());
            }
        }

        // Clip region
        auto [regionStart, regionEnd] = getRegion();
        g.setColour (Colours::accent.withAlpha (0.05f));
        g.fillRect (juce::Rectangle<float> (beatToX (regionStart), 0.0f, beatToX (regionEnd) - beatToX (regionStart), (float) getHeight()));

        // Grid
        auto beatsPerBar = viewState.getBeatsPerBar();
        auto grid = getGrid();
        for (double b = std::floor (scrollBeat / grid) * grid; beatToX (b) < (float) getWidth(); b += grid)
        {
            bool isBar = std::abs (std::fmod (b, beatsPerBar)) < 1e-6;
            bool isBeat = std::abs (std::fmod (b, 1.0)) < 1e-6;
            g.setColour (isBar ? Theme::lift (Colours::gridBar, 0.2f) : isBeat ? Colours::gridBar : Colours::gridLine);
            g.drawVerticalLine (juce::roundToInt (beatToX (b)), 0.0f, (float) getHeight());
        }

        // Region edges
        g.setColour (Colours::accent.withAlpha (0.6f));
        g.drawVerticalLine (juce::roundToInt (beatToX (regionStart)), 0.0f, (float) getHeight());
        g.drawVerticalLine (juce::roundToInt (beatToX (regionEnd)), 0.0f, (float) getHeight());

        // Notes
        auto colour = getClipColour (*clip);

        for (auto n : clip->getSequence().getNotes())
        {
            auto nr = getNoteBounds (*n);
            if (nr.getRight() < (float) keyboardW || nr.getX() > (float) getWidth()) continue;
            bool sel = selectedNotes.contains (n);
            auto velAlpha = 0.45f + 0.55f * (float) n->getVelocity() / 127.0f;
            g.setColour ((sel ? colour.brighter (0.5f) : colour).withAlpha (velAlpha));
            g.fillRect (nr);
            g.setColour (sel ? Colours::text : Colours::panelDark);
            g.drawRect (nr, 1.0f);
        }

        // Playhead inside the clip
        if (auto pos = getPlayheadBeat())
        {
            g.setColour (Colours::playhead.withAlpha (0.8f));
            g.drawVerticalLine (juce::roundToInt (beatToX (*pos)), 0.0f, (float) getHeight());
        }

        // Keyboard
        g.reduceClipRegion (keys);
        for (int n = 0; n < 128; ++n)
        {
            auto y = noteToY (n);
            if (y > (float) getHeight() || y + keyH < 0.0f) continue;
            bool black = juce::MidiMessage::isMidiNoteBlack (n);
            auto keyR = juce::Rectangle<float> (0.0f, y, (float) keyboardW, keyH);
            g.setColour (black ? juce::Colour (0xff202124) : juce::Colour (0xffd9d9d9));
            g.fillRect (black ? keyR.withWidth (keyboardW * 0.65f) : keyR);
            g.setColour (Colours::outline);
            g.drawRect (keyR, 0.5f);

            if (n % 12 == 0)
            {
                g.setColour (juce::Colours::black);
                g.setFont (juce::FontOptions (9.5f));
                g.drawText (juce::MidiMessage::getMidiNoteName (n, true, true, 3), keyR.toNearestInt().withTrimmedRight (3),
                            juce::Justification::centredRight);
            }
        }
    }

    //==============================================================================
    void mouseDown (const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();

        if (e.x < keyboardW)
        {
            previewNote (yToNote ((float) e.y));
            return;
        }

        auto* um = &clip->edit.getUndoManager();
        um->beginNewTransaction ("Edit notes");

        if (auto n = getNoteAt (e.position))
        {
            if (e.mods.isShiftDown())
            {
                if (selectedNotes.contains (n)) selectedNotes.removeFirstMatchingValue (n);
                else selectedNotes.add (n);
            }
            else if (! selectedNotes.contains (n))
            {
                selectedNotes = { n };
            }

            auto nr = getNoteBounds (*n);
            dragMode = (e.position.x > nr.getRight() - 5) ? DragMode::resize : DragMode::move;
            captureDragStart();
            previewNote (n->getNoteNumber());
        }
        else
        {
            // Draw a new note, one grid step long
            auto beat = snap (xToBeat ((float) e.x), true);
            auto noteNum = yToNote ((float) e.y);
            auto len = lastNoteLength > 0.0 ? lastNoteLength : getGrid();

            if (auto newNote = clip->getSequence().addNote (noteNum, te::BeatPosition::fromBeats (beat), te::BeatDuration::fromBeats (len), 100, 0, um))
            {
                selectedNotes = { newNote };
                dragMode = DragMode::resize;
                captureDragStart();
                previewNote (noteNum);
            }
        }

        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragMode == DragMode::none)
            return;

        auto* um = &clip->edit.getUndoManager();
        auto deltaBeats = e.getDistanceFromDragStartX() / pixelsPerBeat;
        auto deltaNotes = -juce::roundToInt ((float) e.getDistanceFromDragStartY() / keyH);
        auto bypass = e.mods.isCommandDown();

        for (int i = 0; i < selectedNotes.size(); ++i)
        {
            auto* n = selectedNotes[i];
            auto& start = dragStarts[(size_t) i];

            if (dragMode == DragMode::move)
            {
                auto newStart = juce::jmax (0.0, snapDelta (start.start, deltaBeats, bypass));
                n->setStartAndLength (te::BeatPosition::fromBeats (newStart), te::BeatDuration::fromBeats (start.length), um);
                auto newNote = juce::jlimit (0, 127, start.note + deltaNotes);

                if (newNote != n->getNoteNumber())
                {
                    n->setNoteNumber (newNote, um);
                    if (i == 0) previewNote (newNote);
                }
            }
            else
            {
                auto end = snapDelta (start.start + start.length, deltaBeats, bypass);
                auto newLen = juce::jmax (1.0 / 64.0, end - start.start);
                n->setStartAndLength (te::BeatPosition::fromBeats (start.start), te::BeatDuration::fromBeats (newLen), um);
                lastNoteLength = newLen;
            }
        }

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragMode = DragMode::none;
        stopPreview();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (auto n = getNoteAt (e.position))
        {
            selectedNotes.removeFirstMatchingValue (n);
            clip->getSequence().removeNote (*n, &clip->edit.getUndoManager());
            repaint();
        }
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        if (e.mods.isCommandDown() || e.mods.isAltDown())
        {
            auto anchor = xToBeat ((float) e.x);
            pixelsPerBeat = juce::jlimit (8.0, 600.0, pixelsPerBeat * std::pow (1.15, (double) w.deltaY * 8.0));
            scrollBeat = juce::jmax (0.0, anchor - (e.x - keyboardW) / pixelsPerBeat);
        }
        else if (std::abs (w.deltaX) > std::abs (w.deltaY) || e.mods.isShiftDown())
        {
            auto d = std::abs (w.deltaX) > 0.0f ? w.deltaX : w.deltaY;
            scrollBeat = juce::jmax (0.0, scrollBeat - d * 300.0 / pixelsPerBeat);
        }
        else
        {
            scrollY = juce::jlimit (0.0f, juce::jmax (0.0f, 128.0f * keyH - (float) getNotesHeight()), scrollY - w.deltaY * 300.0f);
        }

        repaint();
    }

    bool keyPressed (const juce::KeyPress& k) override
    {
        auto* um = &clip->edit.getUndoManager();

        if (k == juce::KeyPress::deleteKey || k == juce::KeyPress::backspaceKey)
        {
            um->beginNewTransaction ("Delete notes");
            for (auto n : selectedNotes)
                clip->getSequence().removeNote (*n, um);
            selectedNotes.clear();
            repaint();
            return true;
        }

        if (k == juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0))
        {
            selectedNotes = clip->getSequence().getNotes();
            repaint();
            return true;
        }

        if (k == juce::KeyPress ('q'))
        {
            quantise();
            return true;
        }

        if (k == juce::KeyPress::upKey || k == juce::KeyPress::downKey)
        {
            auto delta = (k == juce::KeyPress::upKey ? 1 : -1) * (k.getModifiers().isShiftDown() ? 12 : 1);
            um->beginNewTransaction ("Transpose");
            for (auto n : selectedNotes)
                n->setNoteNumber (juce::jlimit (0, 127, n->getNoteNumber() + delta), um);
            repaint();
            return true;
        }

        return false;
    }

    /** Quantises selected notes (or all notes if none are selected) to the grid. */
    void quantise()
    {
        auto* um = &clip->edit.getUndoManager();
        um->beginNewTransaction ("Quantise");
        auto notes = selectedNotes.isEmpty() ? clip->getSequence().getNotes() : selectedNotes;
        auto grid = getGrid();

        for (auto n : notes)
        {
            auto s = std::round (n->getStartBeat().inBeats() / grid) * grid;
            n->setStartAndLength (te::BeatPosition::fromBeats (s), n->getLengthBeats(), um);
        }

        repaint();
    }

    void zoomToClip()
    {
        auto [s, e] = getRegion();
        auto width = juce::jmax (100, getWidth() - keyboardW - 20);
        pixelsPerBeat = juce::jlimit (8.0, 600.0, width / juce::jmax (1.0, e - s));
        scrollBeat = s;
        repaint();
    }

    double gridOverride = 0.25;

private:
    enum class DragMode { none, move, resize };
    struct NoteStart { double start, length; int note; };

    EditViewState& viewState;
    te::MidiClip::Ptr clip;
    double pixelsPerBeat = 60.0, scrollBeat = 0.0, lastNoteLength = 0.0;
    float scrollY = 0.0f;
    static constexpr float keyH = 12.0f;
    juce::Array<te::MidiNote*> selectedNotes;
    std::vector<NoteStart> dragStarts;
    DragMode dragMode = DragMode::none;
    int previewingNote = -1;
    std::unique_ptr<MidiLanes::Lane> lane;

    int getNotesHeight() const      { return lane != nullptr ? lane->getY() : getHeight(); }
    double getGrid() const          { return gridOverride > 0.0 ? gridOverride : 0.25; }
    float beatToX (double b) const  { return keyboardW + (float) ((b - scrollBeat) * pixelsPerBeat); }
    double xToBeat (float x) const  { return scrollBeat + (x - keyboardW) / pixelsPerBeat; }
    float noteToY (int n) const     { return (float) (127 - n) * keyH - scrollY; }
    int yToNote (float y) const     { return juce::jlimit (0, 127, 127 - (int) std::floor ((y + scrollY) / keyH)); }

    double snap (double beat, bool floorIt) const
    {
        auto g = getGrid();
        return floorIt ? std::floor (beat / g) * g : std::round (beat / g) * g;
    }

    double snapDelta (double start, double delta, bool bypass) const
    {
        if (bypass) return start + delta;
        return start + std::round (delta / getGrid()) * getGrid();
    }

    std::pair<double, double> getRegion() const
    {
        if (clip->isLooping())
        {
            auto s = clip->getLoopStartBeats().inBeats();
            return { s, s + clip->getLoopLengthBeats().inBeats() };
        }

        auto s = clip->getOffsetInBeats().inBeats();
        return { s, s + clip->getLengthInBeats().inBeats() };
    }

    std::optional<double> getPlayheadBeat() const
    {
        auto& transport = clip->edit.getTransport();

        if (clip->getClipSlot() != nullptr)
        {
            if (auto lh = clip->getLaunchHandle())
                if (lh->getPlayingStatus() == te::LaunchHandle::PlayState::playing)
                    if (auto played = lh->getPlayedRange())
                    {
                        auto [s, e] = getRegion();
                        auto len = e - s;
                        if (len > 0.0)
                            return s + std::fmod (played->getLength().inBeats(), len);
                    }

            return std::nullopt;
        }

        auto pos = transport.getPosition();

        if (! clip->getEditTimeRange().contains (pos))
            return std::nullopt;

        auto beat = clip->getContentBeatAtTime (pos).inBeats();

        if (clip->isLooping())
        {
            auto [s, e] = getRegion();
            auto len = e - s;
            if (len > 0.0)
                beat = s + std::fmod (beat - s + len * 1000.0, len);
        }

        return beat;
    }

    juce::Rectangle<float> getNoteBounds (te::MidiNote& n) const
    {
        auto x1 = beatToX (n.getStartBeat().inBeats());
        auto x2 = beatToX (n.getEndBeat().inBeats());
        return { x1, noteToY (n.getNoteNumber()) + 0.5f, juce::jmax (3.0f, x2 - x1), keyH - 1.0f };
    }

    te::MidiNote* getNoteAt (juce::Point<float> p) const
    {
        auto notes = clip->getSequence().getNotes();

        for (int i = notes.size(); --i >= 0;)
            if (getNoteBounds (*notes[i]).contains (p))
                return notes[i];

        return nullptr;
    }

    void captureDragStart()
    {
        dragStarts.clear();

        for (auto n : selectedNotes)
            dragStarts.push_back ({ n->getStartBeat().inBeats(), n->getLengthBeats().inBeats(), n->getNoteNumber() });
    }

    void previewNote (int note)
    {
        stopPreview();
        if (auto at = dynamic_cast<te::AudioTrack*> (clip->getTrack()))
        {
            at->injectLiveMidiMessage (juce::MidiMessage::noteOn (1, note, (juce::uint8) 100), {});
            previewingNote = note;
        }
    }

    void stopPreview()
    {
        if (previewingNote >= 0)
            if (auto at = dynamic_cast<te::AudioTrack*> (clip->getTrack()))
                at->injectLiveMidiMessage (juce::MidiMessage::noteOff (1, previewingNote), {});

        previewingNote = -1;
    }

    void timerCallback() override
    {
        if (clip->edit.getTransport().isPlaying() || clip->getClipSlot() != nullptr)
            repaint();
    }

    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override
    {
        // Notes might have been removed by undo: drop stale selection pointers
        auto notes = clip->getSequence().getNotes();
        selectedNotes.removeIf ([&notes] (te::MidiNote* n) { return ! notes.contains (n); });
        repaint();
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override       { repaint(); }
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { repaint(); }
};

//==============================================================================
/** Waveform of an audio clip's source in warped time, with editable warp markers. */
class SampleEditor : public juce::Component, private juce::Timer
{
public:
    SampleEditor (te::AudioClipBase& c) : clip (&c)
    {
        // Always show the original recording, not the engine's warped/rendered copy
        thumbnail = std::make_unique<te::SmartThumbnail> (c.edit.engine, te::AudioFile (c.edit.engine, c.getOriginalFile()), *this, &c.edit);
        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::background);
        auto r = getLocalBounds().toFloat();
        auto markersArea = r.removeFromTop (16.0f);
        g.setColour (Colours::panel);
        g.fillRect (markersArea);

        auto colour = getClipColour (*clip).brighter (0.3f);
        auto& wtm = clip->getWarpTimeManager();
        bool warping = clip->getWarpTime();

        if (thumbnail->getTotalLength() <= 0.0)
        {
            g.setColour (Colours::textDim);
            g.drawText ("Loading waveform...", getLocalBounds(), juce::Justification::centred);
            return;
        }

        // Clip region (what actually plays) shaded behind the waveform
        auto offset = clip->getPosition().getOffset().inSeconds();
        auto len = clip->getPosition().getLength().inSeconds();
        g.setColour (Colours::accent.withAlpha (0.07f));
        g.fillRect (juce::Rectangle<float> (timeToX (offset), r.getY(), timeToX (offset + len) - timeToX (offset), r.getHeight()));

        g.setColour (colour);

        if (warping)
        {
            // Draw each segment between markers stretched to its warped position
            auto markers = getSortedMarkers();

            for (size_t i = 0; i + 1 < markers.size(); ++i)
            {
                auto x1 = timeToX (markers[i].warpTime.inSeconds());
                auto x2 = timeToX (markers[i + 1].warpTime.inSeconds());

                if (x2 - x1 < 1.0f)
                    continue;

                thumbnail->drawChannels (g, juce::Rectangle<float> (x1, r.getY(), x2 - x1, r.getHeight()).toNearestInt(),
                                         { markers[i].sourceTime, markers[i + 1].sourceTime }, 1.0f);
            }
        }
        else
        {
            thumbnail->drawChannels (g, r.toNearestInt(), { 0s, te::TimePosition::fromSeconds (thumbnail->getTotalLength()) }, 1.0f);
        }

        // Transients (helpful for placing markers)
        auto [ready, transients] = wtm.getTransientTimes();

        if (ready)
        {
            g.setColour (Colours::textDim.withAlpha (0.35f));

            for (auto t : transients)
            {
                auto x = timeToX (warping ? wtm.sourceTimeToWarpTime (t).inSeconds() : t.inSeconds());
                g.drawVerticalLine (juce::roundToInt (x), markersArea.getBottom(), markersArea.getBottom() + 6.0f);
            }
        }

        // Warp markers
        if (warping)
        {
            auto markers = wtm.getMarkers();

            for (int i = 0; i < markers.size(); ++i)
            {
                auto x = timeToX (markers[i]->warpTime.inSeconds());
                bool active = i == dragIndex;
                g.setColour (active ? Colours::text : Colours::mute);
                g.drawVerticalLine (juce::roundToInt (x), markersArea.getY(), (float) getHeight());

                juce::Path handle;
                handle.addTriangle (x - 5.0f, markersArea.getY(), x + 5.0f, markersArea.getY(), x, markersArea.getBottom());
                g.fillPath (handle);
            }
        }

        // Playhead
        if (auto pos = getPlayheadSourceTime())
        {
            g.setColour (Colours::playhead);
            g.drawVerticalLine (juce::roundToInt (timeToX (*pos)), 0.0f, (float) getHeight());
        }

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawText (warping ? "Double-click to add a warp marker, drag handles to stretch, right-click a marker to delete it"
                            : "Turn on Warp Markers to stretch the audio in time",
                    getLocalBounds().removeFromBottom (18).reduced (6, 0), juce::Justification::centredLeft);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! clip->getWarpTime())
            return;

        auto& wtm = clip->getWarpTimeManager();
        dragIndex = findMarkerNear ((float) e.x);

        if (dragIndex >= 0 && e.mods.isPopupMenu())
        {
            clip->edit.getUndoManager().beginNewTransaction ("Delete warp marker");
            wtm.removeMarker (dragIndex);
            dragIndex = -1;
            repaint();
            return;
        }

        if (dragIndex >= 0)
            clip->edit.getUndoManager().beginNewTransaction ("Move warp marker");
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragIndex < 0)
            return;

        auto t = te::TimePosition::fromSeconds (juce::jmax (0.0, xToTime ((float) e.x)));
        dragIndex = findMarkerIndexAfterMove (clip->getWarpTimeManager().moveMarker (dragIndex, t));
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        dragIndex = -1;
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (! clip->getWarpTime() || findMarkerNear ((float) e.x) >= 0)
            return;

        auto& wtm = clip->getWarpTimeManager();
        auto warpT = te::TimePosition::fromSeconds (xToTime ((float) e.x));
        clip->edit.getUndoManager().beginNewTransaction ("Add warp marker");
        wtm.insertMarker (te::WarpMarker (wtm.warpTimeToSourceTime (warpT), warpT));
        repaint();
    }

private:
    te::AudioClipBase::Ptr clip;
    std::unique_ptr<te::SmartThumbnail> thumbnail;
    int dragIndex = -1;

    double getDisplayLength() const
    {
        auto len = thumbnail->getTotalLength();

        if (clip->getWarpTime())
            len = juce::jmax (len, clip->getWarpTimeManager().getWarpEndMarkerTime().inSeconds());

        return juce::jmax (0.001, len);
    }

    float timeToX (double t) const   { return (float) (t / getDisplayLength()) * (float) getWidth(); }
    double xToTime (float x) const   { return (double) x / juce::jmax (1, getWidth()) * getDisplayLength(); }

    std::vector<te::WarpMarker> getSortedMarkers() const
    {
        std::vector<te::WarpMarker> result;

        for (auto m : clip->getWarpTimeManager().getMarkers())
            result.push_back (*m);

        std::sort (result.begin(), result.end(), [] (auto& a, auto& b) { return a.warpTime < b.warpTime; });
        return result;
    }

    int findMarkerNear (float x) const
    {
        auto markers = clip->getWarpTimeManager().getMarkers();

        for (int i = 0; i < markers.size(); ++i)
            if (std::abs (timeToX (markers[i]->warpTime.inSeconds()) - x) < 6.0f)
                return i;

        return -1;
    }

    int findMarkerIndexAfterMove (te::TimePosition newTime) const
    {
        auto markers = clip->getWarpTimeManager().getMarkers();

        for (int i = 0; i < markers.size(); ++i)
            if (markers[i]->warpTime == newTime)
                return i;

        return dragIndex;
    }

    std::optional<double> getPlayheadSourceTime() const
    {
        auto pos = clip->edit.getTransport().getPosition();

        if (clip->getClipSlot() != nullptr || ! clip->getEditTimeRange().contains (pos))
            return std::nullopt;

        return (pos - clip->getPosition().getStart()).inSeconds() + clip->getPosition().getOffset().inSeconds();
    }

    void timerCallback() override
    {
        if (clip->edit.getTransport().isPlaying() || thumbnail->isGeneratingProxy() || ! thumbnail->isFullyLoaded())
            repaint();
    }
};

//==============================================================================
/** Audio clip view: gain, transpose, warp settings, and the waveform: a selection editor
    normally, the warp marker editor with Warp Markers on (and for Session clips). */
class AudioClipPanel : public juce::Component, private juce::Timer
{
public:
    AudioClipPanel (te::AudioClipBase& c, EditViewState& evs) : clip (&c), editor (c), selectionEditor (evs, c)
    {
        auto setup = [this] (juce::Slider& s, juce::Label& l, const juce::String& name, double min, double max, double step, double value)
        {
            s.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
            s.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 60, 16);
            s.setRange (min, max, step);
            s.setValue (value, juce::dontSendNotification);
            l.setText (name, juce::dontSendNotification);
            l.setJustificationType (juce::Justification::centred);
            l.setColour (juce::Label::textColourId, Colours::textDim);
            l.setFont (juce::FontOptions (11.0f));
            addAndMakeVisible (s);
            addAndMakeVisible (l);
        };

        setup (gain, gainLabel, "Gain (dB)", -48.0, 24.0, 0.1, clip->getGainDB());
        gain.onValueChange = [this] { clip->setGainDB ((float) gain.getValue()); };

        setup (pitch, pitchLabel, "Transpose", -24.0, 24.0, 1.0, clip->getPitchChange());
        pitch.onValueChange = [this] { clip->setPitchChange ((float) pitch.getValue()); };

        auto toggle = [this] (juce::ToggleButton& b, const juce::String& text, bool state, std::function<void()> fn)
        {
            b.setButtonText (text);
            b.setToggleState (state, juce::dontSendNotification);
            b.onClick = std::move (fn);
            addAndMakeVisible (b);
        };

        toggle (warp, "Follow tempo", clip->getAutoTempo(), [this] { clip->setAutoTempo (warp.getToggleState()); editor.repaint(); });
        toggle (warpMarkers, "Warp markers", clip->getWarpTime(), [this]
        {
            clip->setWarpTime (warpMarkers.getToggleState());
            showEditor();
        });
        toggle (reverse, "Reverse", clip->getIsReversed(), [this] { clip->setIsReversed (reverse.getToggleState()); });
        toggle (loop, "Loop", clip->isLooping(), [this]
        {
            if (loop.getToggleState())
                clip->setLoopRangeBeats ({ te::BeatPosition() + clip->getOffsetInBeats(), clip->getLengthInBeats() });
            else
                clip->disableLooping();
        });

        // The selection (or the whole clip, from the warp editor)
        normalize.onClick = [this]
        {
            if (selectionEditor.isVisible() && selectionEditor.hasSelection())
                selectionEditor.normalizeSelection();
            else
                Normalise::showForClips ({ clip.get() });
        };
        addAndMakeVisible (normalize);

        addChildComponent (editor);
        addChildComponent (selectionEditor);
        showEditor();
        startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panelDark);
        auto left = getLocalBounds().removeFromLeft (controlsW);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        g.drawText (clip->getName(), left.removeFromTop (24).reduced (10, 0), juce::Justification::centredLeft, true);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.5f));
        auto af = te::AudioFile (clip->edit.engine, clip->getOriginalFile());
        g.drawText (af.getFile().getFileName() + "  " + juce::String (af.getSampleRate() / 1000.0, 1) + " kHz",
                    left.removeFromTop (14).reduced (10, 0), juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto left = r.removeFromLeft (controlsW).withTrimmedTop (42).reduced (8, 4);
        editor.setBounds (r.reduced (4));
        selectionEditor.setBounds (r.reduced (4));

        auto knobs = left.removeFromTop (juce::jmin (80, left.getHeight() / 2));
        auto col = knobs.removeFromLeft (knobs.getWidth() / 2);
        gainLabel.setBounds (col.removeFromTop (14));
        gain.setBounds (col);
        pitchLabel.setBounds (knobs.removeFromTop (14));
        pitch.setBounds (knobs);

        left.removeFromTop (4);
        auto rowH = juce::jlimit (16, 22, (left.getHeight() - 4) / 5);   // four toggles, a gap, Normalize
        for (auto* b : { &warp, &warpMarkers, &reverse, &loop })
            b->setBounds (left.removeFromTop (rowH));

        left.removeFromTop (4);
        normalize.setBounds (left.removeFromTop (rowH).withWidth (96));
    }

private:
    static constexpr int controlsW = 200;
    te::AudioClipBase::Ptr clip;
    SampleEditor editor;
    AudioSelectionEditor selectionEditor;
    juce::Slider gain, pitch;
    juce::Label gainLabel, pitchLabel;
    juce::ToggleButton warp, warpMarkers, reverse, loop;
    juce::TextButton normalize { "Normalize..." };

    void showEditor()
    {
        auto warpEditor = clip->getWarpTime() || clip->getClipSlot() != nullptr;
        editor.setVisible (warpEditor);
        selectionEditor.setVisible (! warpEditor);
        (warpEditor ? (juce::Component&) editor : selectionEditor).repaint();
    }

    void timerCallback() override
    {
        if (! gain.isMouseButtonDown() && std::abs (gain.getValue() - clip->getGainDB()) > 0.01)
            gain.setValue (clip->getGainDB(), juce::dontSendNotification);
    }
};

//==============================================================================
/** Bottom panel: shows either the clip editor or the device chain. */
class DetailView : public juce::Component, private juce::ChangeListener
{
public:
    DetailView (EditViewState& evs) : viewState (evs), devices (evs)
    {
        addAndMakeVisible (devices);

        for (auto* b : { &clipTab, &devicesTab, &commentsTab, &liveTab })
        {
            b->setClickingTogglesState (false);
            addAndMakeVisible (*b);
        }

        lookAndFeelChanged();

        clipTab.onClick = [this] { viewState.detailMode = EditViewState::DetailMode::clip; viewState.sendChangeMessage(); };
        devicesTab.onClick = [this] { viewState.detailMode = EditViewState::DetailMode::devices; viewState.sendChangeMessage(); };
        commentsTab.onClick = [this] { viewState.detailMode = EditViewState::DetailMode::comments; viewState.sendChangeMessage(); };
        liveTab.onClick = [this] { viewState.detailMode = EditViewState::DetailMode::live; viewState.sendChangeMessage(); };

        gridBox.addItemList ({ "1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar" }, 1);
        gridBox.setSelectedId (2, juce::dontSendNotification);
        gridBox.onChange = [this] { updateGrid(); };
        addChildComponent (gridBox);

        quantiseButton.onClick = [this] { if (pianoRoll) pianoRoll->quantise(); };
        addChildComponent (quantiseButton);

        viewState.addChangeListener (this);
        viewState.selectionManager.addChangeListener (this);
        refresh();
    }

    ~DetailView() override
    {
        viewState.selectionManager.removeChangeListener (this);
        viewState.removeChangeListener (this);
    }

    void lookAndFeelChanged() override
    {
        for (auto* b : { &clipTab, &devicesTab, &commentsTab, &liveTab })
        {
            b->setColour (juce::TextButton::buttonOnColourId, Theme::lift (Colours::panelLight, 0.2f));
            b->setColour (juce::TextButton::textColourOnId, Colours::text);
        }
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::panelDark);
        g.setColour (Colours::panel);
        g.fillRect (getLocalBounds().removeFromTop (tabH));
        g.setColour (Colours::outline);
        g.drawHorizontalLine (0, 0.0f, (float) getWidth());

        if (viewState.detailMode == EditViewState::DetailMode::clip && pianoRoll == nullptr && audioPanel == nullptr)
        {
            g.setColour (Colours::textDim);
            g.drawText ("Select a clip to edit it. Double-click an empty lane or session slot to create a MIDI clip.",
                        getLocalBounds().withTrimmedTop (tabH), juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto tabs = r.removeFromTop (tabH).reduced (4, 3);
        clipTab.setBounds (tabs.removeFromLeft (70));
        tabs.removeFromLeft (2);
        devicesTab.setBounds (tabs.removeFromLeft (70));
        tabs.removeFromLeft (2);
        commentsTab.setBounds (tabs.removeFromLeft (84));
        tabs.removeFromLeft (2);
        liveTab.setBounds (tabs.removeFromLeft (60));

        quantiseButton.setBounds (tabs.removeFromRight (80));
        tabs.removeFromRight (4);
        gridBox.setBounds (tabs.removeFromRight (80));

        devices.setBounds (r);
        if (commentsPanel) commentsPanel->setBounds (r);
        if (livePanel) livePanel->setBounds (r);
        if (pianoRoll)  pianoRoll->setBounds (r);
        if (audioPanel) audioPanel->setBounds (r);
        if (drumEditor) drumEditor->setBounds (r);
    }

    void setCommentsPanel (std::unique_ptr<juce::Component> panel)
    {
        commentsPanel = std::move (panel);
        addChildComponent (*commentsPanel);
        refresh();
        resized();
    }

    void setLivePanel (std::unique_ptr<juce::Component> panel)
    {
        livePanel = std::move (panel);
        addChildComponent (*livePanel);
        refresh();
        resized();
    }

    void focusPianoRoll()
    {
        if (pianoRoll && pianoRoll->isShowing())
            pianoRoll->grabKeyboardFocus();
    }

private:
    static constexpr int tabH = 26;
    EditViewState& viewState;
    DeviceChainView devices;
    std::unique_ptr<PianoRoll> pianoRoll;
    std::unique_ptr<AudioClipPanel> audioPanel;
    std::unique_ptr<DrumSectionEditor> drumEditor;
    juce::TextButton clipTab { "Clip" }, devicesTab { "Devices" }, commentsTab { "Comments" }, liveTab { "Live" }, quantiseButton { "Quantise" };
    std::unique_ptr<juce::Component> commentsPanel, livePanel;
    juce::ComboBox gridBox;
    te::SafeSelectable<te::Clip> shownClip;

    void updateGrid()
    {
        static const double grids[] = { 0.125, 0.25, 0.5, 1.0, 2.0, 4.0 };
        if (pianoRoll)
            pianoRoll->gridOverride = grids[juce::jlimit (0, 5, gridBox.getSelectedId() - 1)];
        if (pianoRoll)
            pianoRoll->repaint();
    }

    void refresh()
    {
        auto isClipMode = viewState.detailMode == EditViewState::DetailMode::clip;
        auto isComments = viewState.detailMode == EditViewState::DetailMode::comments;
        clipTab.setToggleState (isClipMode, juce::dontSendNotification);
        devicesTab.setToggleState (viewState.detailMode == EditViewState::DetailMode::devices, juce::dontSendNotification);
        commentsTab.setToggleState (isComments, juce::dontSendNotification);

        if (commentsPanel)
            commentsPanel->setVisible (isComments);

        auto isLive = viewState.detailMode == EditViewState::DetailMode::live;
        liveTab.setToggleState (isLive, juce::dontSendNotification);

        if (livePanel)
            livePanel->setVisible (isLive);

        auto clip = viewState.getSelectedClip();

        if (clip == nullptr && shownClip.get() != nullptr)
            clip = shownClip.get();   // keep showing the last clip when a track gets selected

        // A drum section converted to MIDI (or the reverse) needs the other editor
        const bool wantsDrumEditor = clip != nullptr && DrumTrack::isSection (*clip);
        const bool editorKindChanged = clip != nullptr && dynamic_cast<te::MidiClip*> (clip) != nullptr
                                       && wantsDrumEditor != (drumEditor != nullptr);

        if (clip != shownClip.get() || editorKindChanged || (clip != nullptr && pianoRoll == nullptr && audioPanel == nullptr && drumEditor == nullptr))
        {
            pianoRoll.reset();
            audioPanel.reset();
            drumEditor.reset();
            shownClip = clip;

            if (auto sectionClip = dynamic_cast<te::MidiClip*> (clip); sectionClip != nullptr && wantsDrumEditor)
            {
                drumEditor = std::make_unique<DrumSectionEditor> (*sectionClip);
                drumEditor->onConverted = [this] { juce::MessageManager::callAsync ([sp = SafePointer<DetailView> (this)] { if (sp) sp->refresh(); }); };
                addChildComponent (*drumEditor);
            }
            else if (auto mc = dynamic_cast<te::MidiClip*> (clip))
            {
                pianoRoll = std::make_unique<PianoRoll> (viewState, *mc);
                addChildComponent (*pianoRoll);
                updateGrid();
            }
            else if (auto ac = dynamic_cast<te::AudioClipBase*> (clip))
            {
                audioPanel = std::make_unique<AudioClipPanel> (*ac, viewState);
                addChildComponent (*audioPanel);
            }

            resized();

            if (pianoRoll)
                juce::MessageManager::callAsync ([sp = SafePointer<DetailView> (this)] { if (sp && sp->pianoRoll) sp->pianoRoll->zoomToClip(); });
        }

        devices.setVisible (viewState.detailMode == EditViewState::DetailMode::devices);
        if (pianoRoll)  pianoRoll->setVisible (isClipMode);
        if (audioPanel) audioPanel->setVisible (isClipMode);
        if (drumEditor) drumEditor->setVisible (isClipMode);
        gridBox.setVisible (isClipMode && pianoRoll != nullptr);
        quantiseButton.setVisible (isClipMode && pianoRoll != nullptr);
        repaint();
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { refresh(); }
};

} // namespace elsewhere
