// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"
#include "Widgets.h"
#include "PluginMenu.h"
#include "Mixing.h"
#include "Routing.h"
#include "Normalise.h"
#include "Takes.h"

namespace elsewhere
{
//==============================================================================
/** Pops up the list of inputs (audio interface channels / MIDI devices) for a track. */
inline void showInputMenu (te::AudioTrack& track, juce::Component* target)
{
    juce::PopupMenu m;
    auto trackRef = te::AudioTrack::Ptr (&track);
    auto current = EngineHelpers::getInputOnTrack (track);

    m.addItem ("No Input", true, current == nullptr, [trackRef] { EngineHelpers::clearInputsFromTrack (*trackRef); });
    m.addSectionHeader ("Audio Inputs");

    for (auto instance : track.edit.getAllInputDevices())
    {
        auto& dev = instance->getInputDevice();

        if (dev.getDeviceType() == te::InputDevice::waveDevice)
            m.addItem (dev.getAliasOrName(), true, instance == current, [trackRef, instance]
            {
                EngineHelpers::clearInputsFromTrack (*trackRef);
                EngineHelpers::assignInputToTrack (*instance, *trackRef);
            });
    }

    m.addSectionHeader ("MIDI Inputs");

    for (auto instance : track.edit.getAllInputDevices())
    {
        auto& dev = instance->getInputDevice();

        if (dev.getDeviceType() == te::InputDevice::physicalMidiDevice
             || dev.getDeviceType() == te::InputDevice::virtualMidiDevice)
            m.addItem (dev.getAliasOrName(), true, instance == current, [trackRef, instance]
            {
                EngineHelpers::clearInputsFromTrack (*trackRef);
                EngineHelpers::assignInputToTrack (*instance, *trackRef);
            });
    }

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

inline juce::String getInputName (te::AudioTrack& track)
{
    if (auto in = EngineHelpers::getInputOnTrack (track))
        return in->getInputDevice().getAliasOrName();

    return "No Input";
}

/** Pops up where a track plays to: Main (the default output), a bus (any other track), a
    hardware output or nowhere (for a track that only keys a sidechain). Tracks in a group play
    through the group, so they don't have the choice; buses that would loop aren't offered. */
inline void showOutputMenu (te::Track& track, juce::Component* target)
{
    auto audioTrack = dynamic_cast<te::AudioTrack*> (&track);

    if (audioTrack == nullptr || track.isPartOfSubmix())
        return;

    auto ref = te::AudioTrack::Ptr (audioTrack);
    auto& out = audioTrack->getOutput();
    auto& dm = track.edit.engine.getDeviceManager();

    juce::PopupMenu m;
    m.addItem ("Main", true, out.usesDefaultAudioOut() && out.getDestinationTrack() == nullptr && ! out.outputsToNone(),
               [ref] { Routing::setOutput (*ref, nullptr); });

    m.addSectionHeader ("Buses");

    for (auto t : te::getAudioTracks (track.edit))
    {
        if (t == audioTrack || Mixing::isReturnTrack (*t))
            continue;

        // Buses first-class; any other track can take one in too, from the submenu
        if (! Routing::isBus (*t))
            continue;

        m.addItem (t->getName(), Routing::canOutputTo (*audioTrack, *t), out.outputsToDestTrack (*t),
                   [ref, dest = te::AudioTrack::Ptr (t)] { Routing::setOutput (*ref, dest.get()); });
    }

    juce::PopupMenu others;

    for (auto t : te::getAudioTracks (track.edit))
        if (t != audioTrack && ! Mixing::isReturnTrack (*t) && ! Routing::isBus (*t))
            others.addItem (t->getName(), Routing::canOutputTo (*audioTrack, *t), out.outputsToDestTrack (*t),
                            [ref, dest = te::AudioTrack::Ptr (t)] { Routing::setOutput (*ref, dest.get()); });

    if (others.getNumItems() > 0)
        m.addSubMenu ("Other Tracks", others);

    m.addItem ("New Bus", [ref]
    {
        ref->edit.getUndoManager().beginNewTransaction ("New bus");

        // One undo for the bus and the route into it
        if (auto bus = Routing::createBus (ref->edit))
            ref->getOutput().setOutputToTrack (bus);
    });

    m.addSectionHeader ("Audio Outputs");

    for (int i = 0; i < dm.getNumWaveOutDevices(); ++i)
    {
        if (auto dev = dm.getWaveOutDevice (i); dev != nullptr && dev->isEnabled())
        {
            auto id = dev->getDeviceID();
            m.addItem (dev->getName(), true, ! out.usesDefaultAudioOut() && out.getOutputDeviceID() == id,
                       [ref, id] { ref->getOutput().setOutputToDeviceID (id); });
        }
    }

    m.addSeparator();
    m.addItem ("No Output", true, out.outputsToNone(), [ref] { Routing::setNoOutput (*ref); });

    m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target));
}

inline juce::String getOutputName (te::Track& track)
{
    if (track.isMasterTrack())
    {
        if (auto dev = track.edit.engine.getDeviceManager().getDefaultWaveOutDevice())
            return dev->getName();

        return "Output";
    }

    if (auto parent = dynamic_cast<te::FolderTrack*> (track.getParentTrack()); parent != nullptr && track.isPartOfSubmix())
        return juce::String::fromUTF8 ("\xe2\x86\x92 ") + parent->getName();

    if (auto at = dynamic_cast<te::AudioTrack*> (&track))
    {
        if (auto routed = Routing::describeOutput (*at); routed.isNotEmpty())
            return routed;

        if (! at->getOutput().usesDefaultAudioOut())
            return at->getOutput().getOutputName();
    }

    return "Main";
}

//==============================================================================
/** Renders a track (clips + devices) to a WAV and puts it on a new audio track below, muting the original. */
inline void bounceTrack (te::AudioTrack& track, te::Edit& edit, EditViewState& evs)
{
    auto length = edit.getLength();

    if (length <= 0s)
        return;

    auto folder = edit.editFileRetriever ? edit.editFileRetriever().getParentDirectory().getChildFile ("Samples/Bounced")
                                         : juce::File::getSpecialLocation (juce::File::tempDirectory);
    folder.createDirectory();
    auto file = folder.getNonexistentChildFile (juce::File::createLegalFileName (track.getName() + " (bounce)"), ".wav", false);

    juce::BigInteger tracksToDo;
    tracksToDo.setBit (te::getAllTracks (edit).indexOf (&track));

    edit.getTransport().stop (false, true);

    auto renderedOk = te::Renderer::renderToFile ("Bouncing " + track.getName(), file, edit, { 0s, length }, tracksToDo, true, false, {}, true);

    if (! renderedOk)
    {
        juce::AlertWindow::showMessageBoxAsync (juce::MessageBoxIconType::WarningIcon, "Bounce failed", "Couldn't render " + track.getName() + ".");
        return;
    }

    edit.getUndoManager().beginNewTransaction ("Bounce track");
    auto newTrack = edit.insertNewAudioTrack (te::TrackInsertPoint (track.getParentTrack(), &track), nullptr);

    if (newTrack == nullptr)
        return;

    newTrack->setName (track.getName() + " (bounce)");
    newTrack->setColour (track.getColour());
    newTrack->getClipSlotList().ensureNumberOfSlots (edit.getSceneList().getScenes().size());
    te::AudioFile af (edit.engine, file);

    if (auto clip = newTrack->insertWaveClip (newTrack->getName(), file, { { 0s, te::TimeDuration::fromSeconds (af.getLength()) }, {} }, false))
    {
        clip->setAutoTempo (false);
        clip->setFadeIn ({});
        clip->setFadeOut ({});
    }

    // The bounce includes the original track's devices and fader
    Mixing::ensureSends (*newTrack);
    track.setMute (true);
    evs.focusTrack (newTrack.get());
}

//==============================================================================
/** Who a colour choice belongs to. A group has one colour for all of it, and its tracks show tones
    of it (see Theme::ofTrack), so choosing a colour on any of them colours the whole group. */
inline te::Track& colourOwner (te::Track& track)
{
    auto* owner = &track;
    while (auto group = owner->getParentFolderTrack())
        owner = group;
    return *owner;
}

/** The twelve track colours, ticking the current one. Item IDs 1-12. */
inline juce::PopupMenu colourMenu (te::Track& track)
{
    juce::PopupMenu colours;
    auto current = colourOwner (track).getColour();

    for (int i = 0; i < 12; ++i)
    {
        auto stored = EngineHelpers::trackColourFor (i);
        colours.addColouredItem (i + 1, Themes::colourNames[i], Theme::forTrack (stored), true, current == stored, nullptr);
    }

    return colours;
}

inline void applyColourChoice (te::Track& track, int result)
{
    if (result < 1 || result > 12)
        return;

    auto& owner = colourOwner (track);
    owner.edit.getUndoManager().beginNewTransaction (owner.isFolderTrack() ? "Group colour" : "Track colour");
    owner.setColour (EngineHelpers::trackColourFor (result - 1));
}

/** The colour menu on its own, e.g. from the colour disc on a track's header. */
inline void showColourMenu (te::Track& track, juce::Component* target)
{
    colourMenu (track).showMenuAsync (juce::PopupMenu::Options().withTargetComponent (target),
                                      [ref = te::Track::Ptr (&track)] (int result) { applyColourChoice (*ref, result); });
}

/** Menu of per-track actions, shared by the arrangement headers and session strips. */
inline void showTrackMenu (te::Track& track, EditViewState& evs, std::function<void()> onRename)
{
    juce::PopupMenu m;
    auto trackRef = te::Track::Ptr (&track);
    auto& edit = track.edit;

    m.addItem ("Rename", [onRename] { if (onRename) onRename(); });
    m.addSubMenu (track.getParentFolderTrack() != nullptr || track.isFolderTrack() ? "Group Colour" : "Colour", colourMenu (track));

    if (dynamic_cast<te::AudioTrack*> (&track) != nullptr || dynamic_cast<te::FolderTrack*> (&track) != nullptr)
        m.addItem ("Add Device...", [trackRef] { PluginMenu::showForTrack (*trackRef, -1); });

    if (auto at = dynamic_cast<te::AudioTrack*> (&track); at != nullptr && ! Mixing::isReturnTrack (track))
    {
        auto atRef = te::AudioTrack::Ptr (at);
        bool frozen = at->isFrozen (te::Track::individualFreeze);

        m.addSeparator();
        m.addItem (frozen ? "Unfreeze Track" : "Freeze Track", [atRef, frozen]
        {
            atRef->setFrozen (! frozen, te::Track::individualFreeze);
        });
        m.addItem ("Bounce to New Audio Track", [atRef, &edit, &evs] { bounceTrack (*atRef, edit, evs); });

        // All its clips together by default, so the balance between them stays
        m.addItem ("Normalize Track...", ! Normalise::audioClipsOf (*at).isEmpty(), false, [atRef]
        {
            Normalise::showForClips (Normalise::audioClipsOf (*atRef), Normalise::Process::together);
        });
    }

    m.addSeparator();

    auto selectedTracks = evs.selectionManager.getItemsOfType<te::Track>();
    if (! selectedTracks.contains (&track))
        selectedTracks.add (&track);

    m.addItem ("Group Tracks  (Cmd+G)", [selectedTracks, &edit, &evs]
    {
        edit.getUndoManager().beginNewTransaction ("Group tracks");
        if (auto g = Mixing::groupTracks (edit, selectedTracks))
            evs.focusTrack (g);
    });

    if (auto ft = dynamic_cast<te::FolderTrack*> (&track))
        m.addItem ("Ungroup", [ft, &edit, &evs]
        {
            edit.getUndoManager().beginNewTransaction ("Ungroup");
            evs.selectionManager.deselectAll();
            evs.focusTrack (nullptr);
            Mixing::ungroup (edit, *ft);
        });

    m.addSeparator();
    m.addItem ("Delete Track", [trackRef, &edit, &evs]
    {
        evs.selectionManager.deselectAll();
        edit.deleteTrack (trackRef.get());
    });

    m.showMenuAsync ({}, [trackRef] (int result) { applyColourChoice (*trackRef, result); });
}

//==============================================================================
/** How a track is described to screen readers. */
namespace TrackSpeech
{
    inline juce::String name (const te::Track& t)
    {
        return t.isMasterTrack() ? juce::String ("Main") : t.getName();
    }

    /** The name of the track's colour from the colour menu ("Ember"), if it has one of those. */
    inline juce::String colourName (const te::Track& t)
    {
        const auto c = t.getColour();

        for (int i = 0; i < 12; ++i)
            if (c.getARGB() == EngineHelpers::trackColourFor (i).getARGB())
                return Themes::colourNames[i];

        return {};
    }

    /** What kind of track it is and its colour: "group, Ember". */
    inline juce::String kind (const te::Track& t)
    {
        juce::StringArray parts;
        parts.add (t.isMasterTrack() ? "main output" : t.isFolderTrack() ? "group" : Mixing::isReturnTrack (t) ? "return track" : "track");

        if (! t.isMasterTrack())
            parts.add (colourName (t));

        parts.removeEmptyStrings();
        return parts.joinIntoString (", ");
    }

    /** What's switched on: "muted, soloed, armed". */
    inline juce::String states (te::Track& t)
    {
        juce::StringArray parts;

        if (t.isMuted (false))  parts.add ("muted");
        if (t.isSolo (false))   parts.add ("soloed");

        if (auto at = dynamic_cast<te::AudioTrack*> (&t))
            if (EngineHelpers::isTrackArmed (*at))
                parts.add ("armed");

        return parts.joinIntoString (", ");
    }

    /** All of it, for announcing a track when it's selected from the keyboard. */
    inline juce::String describe (te::Track& t)
    {
        juce::StringArray parts { name (t), kind (t), states (t) };
        parts.removeEmptyStrings();
        return parts.joinIntoString (", ");
    }
}

//==============================================================================
/** A small send knob: the return's letter, blue when post-fader, yellow when pre-fader.
    Right-click to switch between pre- and post-fader. */
class SendKnob : public juce::Component
{
public:
    SendKnob (te::Track& t, te::AuxSendPlugin& s)
        : track (&t), send (&s), knob (*s.gain, juce::Slider::RotaryHorizontalVerticalDrag)
    {
        knob.addMouseListener (this, false);
        addAndMakeVisible (knob);
        setFocusContainerType (juce::Component::FocusContainerType::focusContainer);
        update();
    }

    void update()
    {
        const bool pre = Mixing::isPreFader (*track, *send);
        knob.setColour (juce::Slider::rotarySliderFillColourId, pre ? Colours::mute : Colours::solo);
        auto tip = "Send to Return " + Mixing::busLetter (send->getBusNumber()) + (pre ? " (pre-fader)" : " (post-fader)")
                   + ". Right-click to switch.";
        knob.setTooltip (tip);
        knob.setTitle ("Send to Return " + Mixing::busLetter (send->getBusNumber()));
        knob.setDescription (pre ? "pre-fader" : "post-fader");
        repaint();
    }

    void paint (juce::Graphics& g) override
    {
        auto letter = getLocalBounds().removeFromLeft (8);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText (Mixing::busLetter (send->getBusNumber()), letter, juce::Justification::centred);

        // Pre-fader sends underline their letter, so the difference isn't only the knob's colour
        if (Mixing::isPreFader (*track, *send))
            g.fillRect (juce::Rectangle<float> (6.0f, 1.0f).withCentre ({ (float) letter.getCentreX(), (float) letter.getCentreY() + 6.0f }));
    }

    void resized() override     { knob.setBounds (getLocalBounds().withTrimmedLeft (8).expanded (0, 2)); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        if (! e.mods.isPopupMenu())
            return;

        const bool pre = Mixing::isPreFader (*track, *send);
        juce::PopupMenu m;
        m.addItem ("Post-fader (follows the track's fader)", true, ! pre, [this] { Mixing::setPreFader (*track, *send, false); });
        m.addItem ("Pre-fader (independent of the fader)", true, pre, [this] { Mixing::setPreFader (*track, *send, true); });
        m.showMenuAsync ({});
    }

    te::AuxSendPlugin* getSend() const  { return send.get(); }

private:
    te::Track::Ptr track;
    juce::ReferenceCountedObjectPtr<te::AuxSendPlugin> send;
    ParameterSlider knob;
};

//==============================================================================
class TrackHeader : public juce::Component,
                    private juce::ChangeListener,
                    private juce::ValueTree::Listener,
                    private juce::Timer
{
public:
    static constexpr int indentPerLevel = 12;

    TrackHeader (EditViewState& evs, te::Track& t) : viewState (evs), track (&t)
    {
        name.setFont (juce::FontOptions (13.0f));
        name.setMinimumHorizontalScale (1.0f);     // long names end in "..." rather than squeezing
        name.setInterceptsMouseClicks (false, false);
        name.setEditable (false, true, false);
        name.onTextChange = [this] { track->setName (name.getText()); };
        addAndMakeVisible (name);

        // Screen readers see each header as a group named after its track, holding its controls
        setFocusContainerType (juce::Component::FocusContainerType::focusContainer);

        audioTrack = dynamic_cast<te::AudioTrack*> (&t);
        folderTrack = dynamic_cast<te::FolderTrack*> (&t);
        bool hasMixer = audioTrack != nullptr || folderTrack != nullptr;

        if (hasMixer)
        {
            addAndMakeVisible (soloButton);
            addAndMakeVisible (muteButton);

            muteButton.onClick = [this] { track->setMute (! track->isMuted (false)); refresh(); };
            soloButton.onClick = [this] { Mixing::clickSolo (*track); refresh(); };

            if (auto vp = Mixing::findVolume (t))
            {
                volume = std::make_unique<ParameterSlider> (*vp->volParam, juce::Slider::LinearBar);
                pan = std::make_unique<ParameterSlider> (*vp->panParam, juce::Slider::RotaryHorizontalVerticalDrag);
            }

            if (auto lm = Mixing::findMeter (t))
                meter = std::make_unique<LevelMeter> (lm->measurer);
        }

        if (audioTrack != nullptr && ! Mixing::isReturnTrack (t))
        {
            addAndMakeVisible (armButton);
            armButton.onClick = [this]
            {
                if (! EngineHelpers::trackHasInput (*audioTrack))
                {
                    showInputMenu (*audioTrack, &inputButton);
                    return;
                }

                EngineHelpers::armTrack (*audioTrack, ! EngineHelpers::isTrackArmed (*audioTrack));
                refresh();
            };

            inputButton.onClick = [this] { showInputMenu (*audioTrack, &inputButton); };
            inputButton.setColour (juce::TextButton::buttonColourId, Colours::panelDark);
            addAndMakeVisible (inputButton);
        }

        // The output shows in the wide headers (Record & Mix)
        if (audioTrack != nullptr || folderTrack != nullptr)
        {
            outputButton.onClick = [this] { showOutputMenu (*track, &outputButton); };
            outputButton.setColour (juce::TextButton::buttonColourId, Colours::panelDark);
            outputButton.setTooltip ("Output");
            addChildComponent (outputButton);
        }

        if (t.isMasterTrack())
        {
            if (auto vp = t.edit.getMasterVolumePlugin())
            {
                volume = std::make_unique<ParameterSlider> (*vp->volParam, juce::Slider::LinearBar);
                pan = std::make_unique<ParameterSlider> (*vp->panParam, juce::Slider::RotaryHorizontalVerticalDrag);
            }

            if (auto lm = getMasterMeterPlugin (t.edit))
                meter = std::make_unique<LevelMeter> (lm->measurer);
        }

        if (volume != nullptr)
        {
            volume->setColour (juce::Slider::trackColourId, Colours::textDim.withAlpha (0.55f));
            addAndMakeVisible (*volume);
            addAndMakeVisible (*pan);
        }

        if (meter != nullptr)
            addAndMakeVisible (*meter);

        if (folderTrack != nullptr)
        {
            collapseButton.setToggleable (true);
            collapseButton.onClick = [this]
            {
                auto& set = viewState.collapsedGroups;
                if (set.count (track->itemID)) set.erase (track->itemID); else set.insert (track->itemID);
                viewState.sendChangeMessage();
            };
            addAndMakeVisible (collapseButton);
        }

        if (audioTrack != nullptr)
        {
            arrangementButton.setTitle ("Back to Arrangement");
            arrangementButton.setTooltip ("This track is playing Session clips, so its Arrangement clips are silent. "
                                          "Click to go back to the Arrangement.");
            arrangementButton.setColour (juce::TextButton::buttonColourId, Colours::accent);
            arrangementButton.setColour (juce::TextButton::textColourOffId, Colours::textInverse);
            arrangementButton.onClick = [this] { EngineHelpers::backToArrangement (*audioTrack); refresh(); };
            addChildComponent (arrangementButton);
        }

        rebuildSends();

        automationButton.setTooltip ("Show automation lane");
        automationButton.onClick = [this] { if (onToggleAutomation) onToggleAutomation(); };
        addAndMakeVisible (automationButton);

        // Shows only once the track has takes, with how many
        takesButton.onClick = [this]
        {
            auto& set = viewState.takeLanes;
            if (set.count (track->itemID)) set.erase (track->itemID); else set.insert (track->itemID);
            viewState.sendChangeMessage();
        };
        addChildComponent (takesButton);

        track->state.addListener (this);
        track->edit.state.getChildWithName (te::IDs::INPUTDEVICES).addListener (this);
        viewState.selectionManager.addChangeListener (this);
        refresh();
        startTimerHz (4);
    }

    ~TrackHeader() override
    {
        viewState.selectionManager.removeChangeListener (this);

        if (track != nullptr)
            track->state.removeListener (this);
    }

    te::Track* getTrack() const     { return track.get(); }

    std::function<void()> onToggleAutomation;

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<A11y::Handler> (*this, juce::AccessibilityRole::group,
            A11y::Handler::Info {
                [this] { return TrackSpeech::name (*track); },
                [this] { return TrackSpeech::kind (*track); },
                {},
                [this] (juce::AccessibleState s)
                {
                    s = s.withSelectable();
                    return viewState.selectionManager.isSelected (track.get()) ? s.withSelected() : s;
                } },
            juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this]
            {
                viewState.selectionManager.selectOnly (track.get());
                viewState.focusTrack (track.get());
            }));
    }

    void paint (juce::Graphics& g) override
    {
        bool focused = viewState.focusedTrack == track.get();
        bool selected = viewState.selectionManager.isSelected (track.get());

        // Orbit: no panels or lines; the focused track sits on a soft rounded panel
        g.fillAll (Colours::background);

        if (focused || selected)
        {
            g.setColour (selected ? Theme::raisedSurface() : Colours::panel);
            g.fillRoundedRectangle (getLocalBounds().toFloat().withTrimmedLeft ((float) getIndent()).reduced (4.0f, 2.0f), 10.0f);
        }

        // The track's colour as a disc; click it to change it (a group's tracks share the group's)
        if (! track->isMasterTrack())
        {
            g.setColour (getTrackColour());
            g.fillEllipse (colourDisc.toFloat().withSizeKeepingCentre (folderTrack != nullptr ? 13.0f : 11.0f, folderTrack != nullptr ? 13.0f : 11.0f));
        }

        if (Mixing::isReturnTrack (*track))
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (9.0f));
            g.drawText ("return", getLocalBounds().removeFromBottom (14).withTrimmedLeft (getIndent() + 14), juce::Justification::centredLeft);
        }
    }

    /** Collaborators working on this track: coloured initials next to the name. */
    void paintOverChildren (juce::Graphics& g) override
    {
        auto x = (float) (arrangementButton.isVisible() ? arrangementButton.getX() : automationButton.getX()) - 4.0f;

        for (auto& c : viewState.collaborators)
        {
            if (c.focusedTrack != track->itemID)
                continue;

            x -= 15.0f;
            auto dot = juce::Rectangle<float> (x, (float) automationButton.getY() + 2.0f, 14.0f, 14.0f);
            g.setColour (c.colour);
            g.fillEllipse (dot);
            g.setColour (Colours::textInverse);
            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.drawText (c.name.substring (0, 1).toUpperCase(), dot.toNearestInt(), juce::Justification::centred);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds().withTrimmedLeft (getIndent() + 10).withTrimmedRight (6).reduced (0, 6);

        if (meter != nullptr)
            meter->setBounds (r.removeFromRight (5).reduced (0, 2));

        r.removeFromRight (6);
        auto top = r.removeFromTop (20);

        if (folderTrack != nullptr)
            collapseButton.setBounds (top.removeFromLeft (18).reduced (1));

        colourDisc = top.removeFromLeft (20);

        for (auto* b : { &muteButton, &soloButton, &armButton })
            if (b->isVisible())
                b->setBounds (top.removeFromRight (20).reduced (1));

        automationButton.setBounds (top.removeFromRight (20).reduced (1));

        if (takesButton.isVisible())
            takesButton.setBounds (top.removeFromRight (20).reduced (1));

        if (arrangementButton.isVisible())
            arrangementButton.setBounds (top.removeFromRight (22).reduced (1));
        top.removeFromRight (4);
        name.setBounds (top);

        // Each row shows only once it fits at full size (padding, name, fader, input), so dragging a
        // track shorter drops rows instead of squashing them
        auto showMixer = getHeight() >= 6 + 20 + 3 + 18 + 6;

        if (volume != nullptr) volume->setVisible (showMixer);
        if (! showMixer)
            for (auto* s : sends)
                s->setVisible (false);
        if (pan != nullptr)    pan->setVisible (showMixer);

        // Wide headers have the input and output side by side
        auto wide = getWidth() >= 260;
        auto showRouting = showMixer && getHeight() >= 6 + 20 + 3 + 18 + 3 + 16 + 6;
        inputButton.setVisible (showRouting && audioTrack != nullptr && ! Mixing::isReturnTrack (*track));
        outputButton.setVisible (showRouting && wide && (audioTrack != nullptr || folderTrack != nullptr));

        if (showMixer)
        {
            r.removeFromTop (3);
            auto row = r.removeFromTop (18);

            if (pan != nullptr)
                pan->setBounds (row.removeFromRight (22).expanded (0, 2));

            row.removeFromRight (4);

            // Send knobs between the fader and pan, as many as fit
            for (auto* s : sends)
                s->setVisible (false);

            for (auto* s : sends)
            {
                if (row.getWidth() < 26 + 56)
                    break;
                s->setBounds (row.removeFromRight (26));
                s->setVisible (true);
            }

            if (! sends.isEmpty())
                row.removeFromRight (4);

            if (volume != nullptr)
                volume->setBounds (row);

            r.removeFromTop (3);

            auto routing = r.removeFromTop (16);

            if (outputButton.isVisible())
            {
                auto half = (routing.getWidth() - 4) / 2;
                outputButton.setBounds (routing.removeFromRight (half));
                routing.removeFromRight (4);
            }

            if (inputButton.isVisible())
                inputButton.setBounds (routing);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        beginResizeIfAtEdge (e);

        if (resizing)
            return;

        if (colourDisc.contains (e.getPosition()) && ! track->isMasterTrack() && ! e.mods.isPopupMenu())
        {
            showColourMenu (*track, this);
            return;
        }

        // Cmd/shift-click builds a multi-selection (e.g. to group tracks)
        if (e.mods.isCommandDown() || e.mods.isShiftDown())
            viewState.selectionManager.select (track.get(), true);
        else if (! (e.mods.isPopupMenu() && viewState.selectionManager.isSelected (track.get())))
            viewState.selectionManager.selectOnly (track.get());

        viewState.focusTrack (track.get());

        if (e.mods.isPopupMenu() && ! track->isMasterTrack())
            showTrackMenu (*track, viewState, [this] { name.showEditor(); });
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (! track->isMasterTrack())
            name.showEditor();
    }

    /** Drag the bottom edge to resize the track height. */
    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (resizing)
        {
            viewState.trackHeights[track->itemID] = juce::jmax (EditViewState::minTrackHeight, dragStartHeight + e.getDistanceFromDragStartY());
            viewState.sendChangeMessage();
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        setMouseCursor (e.y > getHeight() - 5 ? juce::MouseCursor::UpDownResizeCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseUp (const juce::MouseEvent&) override    { resizing = false; }

private:
    EditViewState& viewState;
    te::Track::Ptr track;
    te::AudioTrack* audioTrack = nullptr;
    te::FolderTrack* folderTrack = nullptr;

    juce::Label name;
    juce::Rectangle<int> colourDisc;
    SmallToggle muteButton { "m", Colours::mute, "Mute" }, soloButton { "s", Colours::solo, "Solo" },
                armButton { juce::String::fromUTF8 ("\xe2\x97\x8f"), Colours::record, "Record Arm" },
                automationButton { "a", Colours::accent, "Automation Lane" }, takesButton { "", Colours::accent, "Take Lanes" };
    juce::TextButton arrangementButton { juce::String::fromUTF8 ("\xe2\x86\xa9") };   // back to arrangement
    juce::TextButton inputButton, outputButton;
    juce::ShapeButton collapseButton { "collapse", Colours::textDim, Colours::text, Colours::text };
    std::unique_ptr<ParameterSlider> volume, pan;
    std::unique_ptr<LevelMeter> meter;
    juce::OwnedArray<SendKnob> sends;
    bool sendsPending = false;

    /** One knob per return track (sends are created automatically for each return). */
    void rebuildSends()
    {
        sendsPending = false;
        sends.clear();

        if (Mixing::isReturnTrack (*track) || track->isMasterTrack() || ! (track->isAudioTrack() || track->isFolderTrack()))
            return;

        // Sends are listed right-to-left from the pan knob, so add them in reverse
        auto list = Mixing::getSends (*track);
        for (int i = list.size(); --i >= 0;)
            addChildComponent (sends.add (new SendKnob (*track, *list[i])));

        resized();
    }

    void scheduleSendRebuild()
    {
        if (sendsPending)
            return;

        sendsPending = true;
        juce::MessageManager::callAsync ([sp = SafePointer<TrackHeader> (this)] { if (sp != nullptr && sp->sendsPending) sp->rebuildSends(); });
    }

    void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
    {
        if (parent == track->state && child.hasType (te::IDs::PLUGIN)) scheduleSendRebuild();
    }

    void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
    {
        if (parent == track->state && child.hasType (te::IDs::PLUGIN)) scheduleSendRebuild();
    }

    void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override
    {
        if (parent == track->state) scheduleSendRebuild();
    }
    bool resizing = false;
    int dragStartHeight = 0;

    int getIndent() const       { return juce::jmax (0, track->getTrackDepth()) * indentPerLevel; }

    void beginResizeIfAtEdge (const juce::MouseEvent& e)
    {
        resizing = e.y > getHeight() - 5;
        dragStartHeight = getHeight();
    }

    static te::LevelMeterPlugin* getMasterMeterPlugin (te::Edit& edit)
    {
        for (auto p : edit.getMasterPluginList())
            if (auto lm = dynamic_cast<te::LevelMeterPlugin*> (p))
                return lm;

        return nullptr;
    }

    juce::Colour getTrackColour() const
    {
        return Theme::ofTrack (*track);
    }

    void refresh()
    {
        muteButton.setToggleState (track->isMuted (false), juce::dontSendNotification);
        soloButton.setToggleState (track->isSolo (false), juce::dontSendNotification);

        if (audioTrack != nullptr)
        {
            armButton.setToggleState (EngineHelpers::isTrackArmed (*audioTrack), juce::dontSendNotification);
            inputButton.setButtonText ("In: " + getInputName (*audioTrack));
        }

        if (outputButton.isVisible())
            outputButton.setButtonText ("Out: " + getOutputName (*track));

        if (folderTrack != nullptr)
        {
            // A chevron: pointing right when the group is closed, down when it's open
            juce::Path chevron;
            if (viewState.collapsedGroups.count (track->itemID))
                chevron.startNewSubPath (0.0f, 0.0f), chevron.lineTo (4.0f, 4.0f), chevron.lineTo (0.0f, 8.0f);
            else
                chevron.startNewSubPath (0.0f, 0.0f), chevron.lineTo (4.0f, 4.0f), chevron.lineTo (8.0f, 0.0f);
            juce::Path outline;
            juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded).createStrokedPath (outline, chevron);
            collapseButton.setShape (outline, false, true, false);
            collapseButton.setBorderSize (juce::BorderSize<int> (5));

            const bool open = viewState.collapsedGroups.count (track->itemID) == 0;
            collapseButton.setTitle ("Show Group's Tracks");
            collapseButton.setToggleState (open, juce::dontSendNotification);
        }

        automationButton.setToggleState (viewState.automationLanes.count (track->itemID) > 0, juce::dontSendNotification);

        if (audioTrack != nullptr)
        {
            auto numTakes = Takes::getNumTakes (*audioTrack);
            takesButton.setButtonText (juce::String (numTakes));
            takesButton.setTooltip (juce::String (numTakes) + (numTakes == 1 ? " take" : " takes") + ": show the take lanes to comp them");
            takesButton.setToggleState (viewState.takeLanes.count (track->itemID) > 0, juce::dontSendNotification);

            if (takesButton.isVisible() != (numTakes > 0))
            {
                takesButton.setVisible (numTakes > 0);
                resized();
            }
        }

        if (audioTrack != nullptr && arrangementButton.isVisible() != EngineHelpers::isPlayingSessionClips (*audioTrack))
        {
            arrangementButton.setVisible (EngineHelpers::isPlayingSessionClips (*audioTrack));
            resized();
        }

        if (! name.isBeingEdited())
            name.setText (track->getName(), juce::dontSendNotification);

        repaint();
    }

    void timerCallback() override                 { refresh(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override
    {
        juce::MessageManager::callAsync ([sp = SafePointer<TrackHeader> (this)] { if (sp) sp->refresh(); });
    }
};

} // namespace elsewhere
