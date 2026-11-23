// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "TrackHeader.h"
#include "ArrangementView.h"

namespace elsewhere
{
//==============================================================================
/** Launch helpers (adapted from Tracktion's ClipLauncherDemo). */
namespace Launcher
{
    inline void pauseForSyncPointChange (te::Edit& edit)
    {
        if (auto epc = edit.getTransport().getCurrentPlaybackContext())
            epc->blockUntilSyncPointChange();
    }

    inline te::LaunchQuantisation& getLaunchQuantisation (te::Clip& c)
    {
        if (! c.usesGlobalLaunchQuatisation())
            if (auto clipLQ = c.getLaunchQuantisation())
                return *clipLQ;

        return c.edit.getLaunchQuantisation();
    }

    inline te::BeatDuration getLaunchOffset (const te::LaunchQuantisation& lq, te::BeatPosition pos, std::optional<te::BeatRange> loopRange)
    {
        auto quantisedPos = pos;

        for (;;)
        {
            quantisedPos = lq.getNext (quantisedPos);

            if (quantisedPos >= 0_bp)
                break;

            quantisedPos = quantisedPos + 0.001_bd;
        }

        if (! loopRange || quantisedPos < loopRange->getEnd())
            return quantisedPos - pos;

        quantisedPos = lq.getNext (loopRange->getStart());

        if (! loopRange->contains (quantisedPos))
            quantisedPos = loopRange->getStart();

        if (loopRange->contains (pos))
            return (loopRange->getEnd() - pos) + (quantisedPos - loopRange->getStart());

        return quantisedPos - pos;
    }

    inline te::MonotonicBeat getLaunchPosition (te::Edit& e, const te::LaunchQuantisation& lq)
    {
        auto epc = e.getTransport().getCurrentPlaybackContext();

        if (! epc)
            return {};

        auto syncPoint = epc->getSyncPoint();

        if (! syncPoint)
            return {};

        auto& t = e.getTransport();
        auto offset = getLaunchOffset (lq, syncPoint->beat,
                                       t.looping.get() ? std::optional (e.tempoSequence.toBeats (t.getLoopRange())) : std::nullopt);
        return { syncPoint->monotonicBeat.v + offset };
    }

    inline te::MonotonicBeat getLaunchPosition (te::Clip& c)   { return getLaunchPosition (c.edit, getLaunchQuantisation (c)); }

    inline void ensureTransportRunning (te::Edit& edit)
    {
        auto& tc = edit.getTransport();

        if (! tc.isPlaying())
        {
            auto& ts = edit.tempoSequence;
            auto bb = ts.toBarsAndBeats (tc.getPosition());
            tc.setPosition (ts.toTime (te::tempo::BarsAndBeats { .bars = bb.bars }));
            pauseForSyncPointChange (edit);
        }
    }

    inline void stopClip (te::Clip& c)
    {
        if (auto lh = c.getLaunchHandle())
            lh->stop (c.edit.getTransport().isPlaying() ? std::make_optional (getLaunchPosition (c)) : std::nullopt);
    }

    inline void launchClip (te::Clip& c)
    {
        auto& edit = c.edit;
        bool wasPlaying = edit.getTransport().isPlaying();
        ensureTransportRunning (edit);

        if (auto at = dynamic_cast<te::AudioTrack*> (c.getTrack()))
            for (auto slot : at->getClipSlotList().getClipSlots())
                if (auto other = slot->getClip(); other != nullptr && other != &c)
                    stopClip (*other);

        if (auto lh = c.getLaunchHandle())
            lh->play (getLaunchPosition (c));

        if (! wasPlaying)
            edit.getTransport().play (false);
    }

    inline void stopTrack (te::AudioTrack& t)
    {
        for (auto slot : t.getClipSlotList().getClipSlots())
            if (auto c = slot->getClip())
                stopClip (*c);
    }

    inline void stopAll (te::Edit& e)
    {
        for (auto at : te::getAudioTracks (e))
            stopTrack (*at);
    }

    inline void launchScene (te::Edit& edit, int sceneIndex)
    {
        for (auto at : te::getAudioTracks (edit))
        {
            auto slots = at->getClipSlotList().getClipSlots();

            if (auto slot = slots[sceneIndex])
            {
                if (auto c = slot->getClip())
                    launchClip (*c);
                else
                    stopTrack (*at);
            }
        }
    }

    //==============================================================================
    // Shared launching: in a live session, what you launch launches for everyone.
    // Actions are addressed by track + slot/scene position (clip IDs differ per person),
    // and each player launches on their own next quantised beat, so it lands on the same
    // bar musically in both interval and low-latency modes.

    /** Set by the live session while connected; sends an action to the other players. */
    inline std::function<void (const juce::var&)>& broadcaster()
    {
        static std::function<void (const juce::var&)> f;
        return f;
    }

    inline void share (const juce::String& action, te::EditItemID track = {}, int index = -1)
    {
        if (! broadcaster())
            return;

        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("a", action);
        if (track.isValid()) o->setProperty ("track", track.toString());
        if (index >= 0) o->setProperty ("i", index);
        broadcaster() (juce::var (o.get()));
    }

    inline int slotIndexOf (te::Clip& c)
    {
        if (auto at = dynamic_cast<te::AudioTrack*> (c.getTrack()))
        {
            auto slots = at->getClipSlotList().getClipSlots();
            for (int i = 0; i < slots.size(); ++i)
                if (slots[i]->getClip() == &c)
                    return i;
        }
        return -1;
    }

    // What the user does (these also tell the other players)
    inline void userLaunchClip (te::Clip& c)
    {
        launchClip (c);
        if (auto t = c.getTrack()) share ("launch", t->itemID, slotIndexOf (c));
    }

    inline void userStopClip (te::Clip& c)
    {
        stopClip (c);
        if (auto t = c.getTrack()) share ("stopTrack", t->itemID);
    }

    inline void userStopTrack (te::AudioTrack& t)   { stopTrack (t); share ("stopTrack", t.itemID); }
    inline void userLaunchScene (te::Edit& e, int i) { launchScene (e, i); share ("scene", {}, i); }
    inline void userStopAll (te::Edit& e)           { stopAll (e); share ("stopAll"); }

    /** Applies an action from another player. Returns a description (for logs), or empty if ignored. */
    inline juce::String applyShared (te::Edit& edit, const juce::var& v)
    {
        const auto action = v["a"].toString();
        const auto trackID = te::EditItemID::fromVar (v["track"]);
        const int index = v.hasProperty ("i") ? (int) v["i"] : -1;

        te::AudioTrack* track = nullptr;
        if (trackID.isValid())
            for (auto at : te::getAudioTracks (edit))
                if (at->itemID == trackID) track = at;

        if (action == "launch" && track != nullptr)
            if (auto slot = track->getClipSlotList().getClipSlots()[index])
                if (auto c = slot->getClip())
                {
                    launchClip (*c);
                    return "launch " + track->getName() + " slot " + juce::String (index + 1);
                }

        if (action == "stopTrack" && track != nullptr)    { stopTrack (*track); return "stop " + track->getName(); }
        if (action == "scene" && index >= 0)              { launchScene (edit, index); return "scene " + juce::String (index + 1); }
        if (action == "stopAll")                          { stopAll (edit); return "stop all"; }
        return {};
    }

    //==============================================================================
    // Recording into a clip slot (Session view)
    struct SlotRecording
    {
        te::EditItemID slotID, trackID;
        bool trackWasArmed = false;
    };

    inline std::optional<SlotRecording>& activeSlotRecording()
    {
        static std::optional<SlotRecording> rec;
        return rec;
    }

    inline bool isRecordingSlot (const te::ClipSlot& slot)
    {
        auto& rec = activeSlotRecording();
        return rec && rec->slotID == slot.itemID;
    }

    /** Starts recording the track's input into an empty slot. Returns false if the track has no input. */
    inline bool recordIntoSlot (te::AudioTrack& track, te::ClipSlot& slot)
    {
        auto& edit = track.edit;
        auto input = EngineHelpers::getInputOnTrack (track);

        if (input == nullptr || activeSlotRecording())
            return false;

        SlotRecording rec { slot.itemID, track.itemID, input->isRecordingEnabled (track.itemID) };

        // Record into the slot instead of the arrangement lane
        input->setRecordingEnabled (track.itemID, false);

        if (! input->setTarget (slot.itemID, false, nullptr))
            return false;

        input->setRecordingEnabled (slot.itemID, true);
        activeSlotRecording() = rec;

        // Let the playback graph pick up the new destination before recording starts
        juce::Timer::callAfterDelay (60, [&edit]
        {
            auto& transport = edit.getTransport();

            if (! transport.isPlaying())
                ensureTransportRunning (edit);

            transport.record (false, true);    // records Session launches and automation even with nothing armed
        });

        return true;
    }

    /** Stops the slot recording, turns the take into a bar-length loop and starts it playing. */
    inline void stopSlotRecording (te::Edit& edit)
    {
        auto rec = activeSlotRecording();

        if (! rec)
            return;

        edit.getTransport().stopRecording (false);
        activeSlotRecording().reset();

        juce::Timer::callAfterDelay (150, [&edit, rec]
        {
            auto track = dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, rec->trackID));
            auto slot = te::findClipSlotForID (edit, rec->slotID);

            if (track != nullptr)
            {
                if (auto input = EngineHelpers::getInputOnTrack (*track))
                {
                    [[maybe_unused]] auto r = input->removeTarget (rec->slotID, nullptr);
                    input->setRecordingEnabled (track->itemID, rec->trackWasArmed);
                }
            }

            if (slot == nullptr)
                return;

            if (auto clip = slot->getClip())
            {
                // Round the take to whole bars so it loops in time
                auto& ts = edit.tempoSequence;
                auto beatsPerBar = ts.getTimeSig (0)->numerator.get() * 4.0 / ts.getTimeSig (0)->denominator.get();
                auto lenBeats = clip->getLengthInBeats().inBeats();
                auto bars = juce::jmax (1.0, std::round (lenBeats / beatsPerBar));
                clip->setLoopRangeBeats ({ te::BeatPosition(), te::BeatDuration::fromBeats (bars * beatsPerBar) });
                launchClip (*clip);
            }
        });
    }

    inline bool isPlayingOrQueued (te::Clip& c, bool& isQueued)
    {
        isQueued = false;

        if (auto lh = c.getLaunchHandle())
        {
            if (auto q = lh->getQueuedStatus())
            {
                isQueued = *q == te::LaunchHandle::QueueState::playQueued;
                if (isQueued) return false;
            }

            return lh->getPlayingStatus() == te::LaunchHandle::PlayState::playing;
        }

        return false;
    }
}

//==============================================================================
/** The fader fills in the track's colour, as on Somewhere. */
inline void tintFader (juce::Slider* fader, juce::Colour c)
{
    if (fader != nullptr && (! fader->isColourSpecified (juce::Slider::trackColourId) || fader->findColour (juce::Slider::trackColourId) != c))
        fader->setColour (juce::Slider::trackColourId, c);
}

//==============================================================================
class SessionSlot : public juce::Component,
                    public juce::FileDragAndDropTarget,
                    public juce::DragAndDropTarget,
                    private juce::Timer,
                    private juce::ChangeListener
{
public:
    SessionSlot (EditViewState& evs, te::AudioTrack& t, te::ClipSlot& s) : viewState (evs), track (&t), slot (&s)
    {
        viewState.selectionManager.addChangeListener (this);
        startTimerHz (20);
    }

    ~SessionSlot() override { viewState.selectionManager.removeChangeListener (this); }

    std::function<void (const juce::File&, te::ClipSlot&)> onFileDropped;

    void paint (juce::Graphics& g) override
    {
        // Orbit: rounded slots; a clip is a wash of its track's colour with an ink play disc
        auto r = getLocalBounds().toFloat().reduced (2.0f, 1.5f);
        const float round = juce::jmin (10.0f, r.getHeight() * 0.5f);
        auto clip = slot->getClip();

        if (clip == nullptr)
        {
            bool recording = Launcher::isRecordingSlot (*slot);
            g.setColour (recording ? Colours::record.withAlpha (A11y::reduceMotion() ? 0.3f : flash ? 0.35f : 0.2f)
                                   : isDragOver ? Colours::accent.withAlpha (0.2f) : Colours::panel);
            g.fillRoundedRectangle (r, round);

            auto button = r.removeFromLeft (r.getHeight()).withSizeKeepingCentre (8.0f, 8.0f);

            if (recording || EngineHelpers::isTrackArmed (*track))
            {
                // Armed track: empty slots become record buttons
                g.setColour (Colours::record);
                g.fillEllipse (button);

                if (recording)
                {
                    g.setColour (Colours::text);
                    g.setFont (juce::FontOptions (11.0f));
                    g.drawText ("recording", r.toNearestInt(), juce::Justification::centredLeft);
                }
            }
            else
            {
                // Empty-slot stop button
                g.setColour (Theme::control());
                g.fillRoundedRectangle (button.withSizeKeepingCentre (6.0f, 6.0f), 1.0f);
            }
            return;
        }

        bool queued = false;
        bool playing = Launcher::isPlayingOrQueued (*clip, queued);
        bool selected = viewState.selectionManager.isSelected (clip);
        auto colour = getClipColour (*clip);

        g.setColour (Colours::background);
        g.fillRoundedRectangle (r, round);
        g.setColour (selected ? colour.withAlpha (0.4f) : Theme::tint (colour));
        g.fillRoundedRectangle (r, round);

        // Launch key: a disc, ink while playing, flashing while queued. With Reduce Motion a queued
        // clip holds still instead: an ink ring round the disc, waiting for its bar
        auto disc = r.removeFromLeft (r.getHeight()).withSizeKeepingCentre (r.getHeight() - 6.0f, r.getHeight() - 6.0f);
        const bool steady = A11y::reduceMotion();
        const bool lit = playing || (queued && ! steady && flash);
        g.setColour (lit ? Colours::text : Colours::background.withAlpha (0.5f));
        g.fillEllipse (disc);

        if (queued && steady)
        {
            g.setColour (Colours::text);
            g.drawEllipse (disc.reduced (0.75f), 1.5f);
        }
        auto tri = disc.reduced (disc.getWidth() * 0.3f).translated (disc.getWidth() * 0.04f, 0.0f);
        juce::Path p;
        p.addTriangle (tri.getX(), tri.getY(), tri.getX(), tri.getBottom(), tri.getRight(), tri.getCentreY());
        g.setColour (lit ? Colours::textInverse : queued && steady ? Colours::text : Theme::tintLabel (colour));
        g.fillPath (p);

        g.setColour (Theme::tintLabel (colour));
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (clip->getName(), r.toNearestInt().withTrimmedLeft (2).withTrimmedRight (4), juce::Justification::centredLeft, true);

        // Progress for playing clips: a thin line in the track's colour
        if (playing)
        {
            if (auto lh = clip->getLaunchHandle())
            {
                if (auto played = lh->getPlayedRange())
                {
                    auto len = clip->isLooping() ? clip->getLoopLengthBeats().inBeats() : clip->getLengthInBeats().inBeats();

                    if (len > 0.0)
                    {
                        auto elapsed = played->getLength().inBeats();
                        auto prop = (float) std::fmod (elapsed, len) / (float) len;
                        auto full = getLocalBounds().toFloat().reduced (2.0f + round, 1.5f);
                        g.setColour (colour);
                        g.fillRoundedRectangle (full.removeFromBottom (2.0f).withWidth (full.getWidth() * prop), 1.0f);
                    }
                }
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto clip = slot->getClip();
        viewState.focusTrack (track.get());

        if (e.mods.isPopupMenu())
        {
            showMenu();
            return;
        }

        bool onButton = e.x < getHeight();

        if (clip == nullptr)
        {
            if (Launcher::isRecordingSlot (*slot))
                Launcher::stopSlotRecording (track->edit);
            else if (onButton && EngineHelpers::isTrackArmed (*track))
                Launcher::recordIntoSlot (*track, *slot);
            else if (onButton)
                Launcher::userStopTrack (*track);

            viewState.selectionManager.selectOnly (track.get());
            repaint();
            return;
        }

        viewState.selectionManager.selectOnly (clip);

        if (onButton)
            Launcher::userLaunchClip (*clip);
    }

    /** What clicking the slot's key does: launch its clip, or (empty) record into it or stop the track. */
    void pressLaunchKey()
    {
        viewState.focusTrack (track.get());

        if (auto clip = slot->getClip())
        {
            viewState.selectionManager.selectOnly (clip);
            Launcher::userLaunchClip (*clip);
            return;
        }

        if (Launcher::isRecordingSlot (*slot))
            Launcher::stopSlotRecording (track->edit);
        else if (EngineHelpers::isTrackArmed (*track))
            Launcher::recordIntoSlot (*track, *slot);
        else
            Launcher::userStopTrack (*track);

        viewState.selectionManager.selectOnly (track.get());
        repaint();
    }

    /** Screen readers: the clip's name (or "Empty slot") and what it's doing; pressing it does
        what clicking its key does. */
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<A11y::Handler> (*this, juce::AccessibilityRole::button,
            A11y::Handler::Info {
                [this] { auto c = slot->getClip(); return c != nullptr ? c->getName() : juce::String ("Empty slot"); },
                [this] { return TrackSpeech::name (*track); },
                [this] () -> juce::String
                {
                    if (auto c = slot->getClip())
                    {
                        bool queued = false;
                        const bool playing = Launcher::isPlayingOrQueued (*c, queued);
                        return queued ? "queued" : playing ? "playing" : "stopped";
                    }

                    if (Launcher::isRecordingSlot (*slot))          return "recording";
                    if (EngineHelpers::isTrackArmed (*track))       return "press to record";
                    return "press to stop the track";
                },
                {} },
            juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [this] { pressLaunchKey(); }));
    }

    void mouseDoubleClick (const juce::MouseEvent&) override
    {
        if (slot->getClip() == nullptr)
            createMidiClip();

        viewState.detailMode = EditViewState::DetailMode::clip;
        viewState.sendChangeMessage();
    }

    /** Creates an empty one-bar looping MIDI clip in this slot. */
    void createMidiClip()
    {
        {
            auto& ts = track->edit.tempoSequence;
            auto bar = ts.getTimeSig (0)->numerator.get() * 4.0 / ts.getTimeSig (0)->denominator.get();
            track->edit.getUndoManager().beginNewTransaction ("Create MIDI clip");

            if (auto mc = te::insertMIDIClip (*slot, "MIDI Clip", ts.toTime ({ 0_bp, te::BeatDuration::fromBeats (bar) })))
            {
                mc->setLoopRangeBeats ({ 0_bp, te::BeatDuration::fromBeats (bar) });
                viewState.selectionManager.selectOnly (mc.get());
            }
        }
    }

    // Files from Finder
    bool isInterestedInFileDrag (const juce::StringArray& files) override
    {
        for (auto& f : files)
            if (juce::File (f).hasFileExtension (te::soundFileAndMidiExtensions))
                return true;
        return false;
    }

    void fileDragEnter (const juce::StringArray&, int, int) override   { isDragOver = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override              { isDragOver = false; repaint(); }

    void filesDropped (const juce::StringArray& files, int, int) override
    {
        isDragOver = false;
        if (onFileDropped && ! files.isEmpty())
            onFileDropped (juce::File (files[0]), *slot);
        repaint();
    }

    // Files from the browser
    bool isInterestedInDragSource (const SourceDetails& d) override    { return getDraggedFile (d).existsAsFile(); }
    void itemDragEnter (const SourceDetails&) override                 { isDragOver = true; repaint(); }
    void itemDragExit (const SourceDetails&) override                  { isDragOver = false; repaint(); }

    void itemDropped (const SourceDetails& d) override
    {
        isDragOver = false;
        if (onFileDropped)
            onFileDropped (getDraggedFile (d), *slot);
        repaint();
    }

private:
    EditViewState& viewState;
    te::AudioTrack::Ptr track;
    te::ClipSlot* slot;
    bool isDragOver = false, flash = false, wasArmed = false;
    int frame = 0;

    void timerCallback() override
    {
        if (++frame % 5 == 0)
            flash = ! flash;

        if (slot->getClip() != nullptr || Launcher::isRecordingSlot (*slot) || EngineHelpers::isTrackArmed (*track) != wasArmed)
        {
            wasArmed = EngineHelpers::isTrackArmed (*track);
            repaint();
        }
    }

    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }

    void showMenu()
    {
        juce::PopupMenu m;
        auto clip = slot->getClip();
        auto* evs = &viewState;

        if (clip != nullptr)
        {
            auto clipRef = te::Clip::Ptr (clip);
            m.addItem ("Launch", [clipRef] { Launcher::userLaunchClip (*clipRef); });
            m.addItem ("Stop", [clipRef] { Launcher::userStopClip (*clipRef); });
            m.addSeparator();

            m.addItem ("Copy to Arrangement at Playhead", [clipRef]
            {
                auto& edit = clipRef->edit;

                if (auto at = dynamic_cast<te::AudioTrack*> (clipRef->getTrack()))
                {
                    auto state = clipRef->state.createCopy();
                    te::EditItemID::remapIDs (state, nullptr, edit);
                    auto pos = edit.getTransport().getPosition();

                    if (auto newClip = at->insertClipWithState (state))
                    {
                        newClip->setStart (pos, false, true);

                        // Session clips loop forever; in the arrangement give it one loop's length
                        if (newClip->isLooping())
                            newClip->setLength (edit.tempoSequence.toTime (newClip->getStartBeat() + newClip->getLoopLengthBeats()) - pos, false);
                    }
                }
            });

            m.addSeparator();
            m.addItem ("Delete", [clipRef, evs] { evs->selectionManager.deselectAll(); clipRef->removeFromParent(); });
        }
        else
        {
            m.addItem ("Insert MIDI Clip", [this] { createMidiClip(); });
        }

        m.showMenuAsync ({});
    }
};

//==============================================================================
/** A column in the Session view: track name, clip slots, then a compact mixer strip. */
class SessionTrackColumn : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    SessionTrackColumn (EditViewState& evs, te::AudioTrack& t, int numScenes) : viewState (evs), track (&t)
    {
        track->getClipSlotList().ensureNumberOfSlots (numScenes);

        for (auto s : track->getClipSlotList().getClipSlots())
        {
            auto* slot = slots.add (new SessionSlot (evs, t, *s));
            slot->onFileDropped = [this] (const juce::File& f, te::ClipSlot& cs) { if (onFileDropped) onFileDropped (f, cs); };
            addAndMakeVisible (slot);
        }

        stopButton.onClick = [this] { Launcher::userStopTrack (*track); };
        stopButton.setTitle ("Stop Track");
        addAndMakeVisible (stopButton);

        for (auto* b : { &armButton, &soloButton, &muteButton })
            addAndMakeVisible (*b);

        muteButton.onClick = [this] { track->setMute (! track->isMuted (false)); refresh(); };
        soloButton.onClick = [this] { Mixing::clickSolo (*track); refresh(); };
        armButton.onClick = [this]
        {
            if (! EngineHelpers::trackHasInput (*track))
                return showInputMenu (*track, &inputButton);

            EngineHelpers::armTrack (*track, ! EngineHelpers::isTrackArmed (*track));
            refresh();
        };

        inputButton.onClick = [this] { showInputMenu (*track, &inputButton); };
        inputButton.setColour (juce::TextButton::buttonColourId, Colours::panelDark);
        inputButton.setDescription ("Input");
        addAndMakeVisible (inputButton);

        volume = std::make_unique<ParameterSlider> (*t.getVolumePlugin()->volParam, juce::Slider::LinearVertical);
        addAndMakeVisible (*volume);
        pan = std::make_unique<ParameterSlider> (*t.getVolumePlugin()->panParam, juce::Slider::RotaryHorizontalVerticalDrag);
        addAndMakeVisible (*pan);

        if (auto lm = t.getLevelMeterPlugin())
        {
            meter = std::make_unique<LevelMeter> (lm->measurer);
            addAndMakeVisible (*meter);
        }

        // Send knobs, one per return track
        for (auto p : t.pluginList)
        {
            if (auto send = dynamic_cast<te::AuxSendPlugin*> (p))
            {
                auto* k = sendKnobs.add (new ParameterSlider (*send->gain, juce::Slider::RotaryHorizontalVerticalDrag));
                k->setColour (juce::Slider::rotarySliderFillColourId, Colours::solo);
                addAndMakeVisible (k);
                auto* l = sendLabels.add (new juce::Label ({}, Mixing::busLetter (send->getBusNumber())));
                l->setFont (juce::FontOptions (10.0f));
                l->setJustificationType (juce::Justification::centred);
                l->setColour (juce::Label::textColourId, Colours::textDim);
                addAndMakeVisible (l);
            }
        }

        if (Mixing::isReturnTrack (t))
        {
            armButton.setVisible (false);
            inputButton.setVisible (false);
        }

        viewState.addChangeListener (this);
        refresh();
        startTimerHz (4);
    }

    ~SessionTrackColumn() override { viewState.removeChangeListener (this); }

    std::function<void (const juce::File&, te::ClipSlot&)> onFileDropped;

    static constexpr int headerH = 30, slotH = 28, mixerH = 230;

    void paint (juce::Graphics& g) override
    {
        bool focused = viewState.focusedTrack == track.get();
        auto c = Theme::ofTrack (*track);
        tintFader (volume.get(), c);
        paintOrbitColumn (g, *this, *track, c, focused, false);
    }

    /** A column's header (colour disc and name, a group's colour as a bar across the top of its
        tracks' columns) and its mixer as a soft panel, as in the Mixer. */
    static void paintOrbitColumn (juce::Graphics& g, juce::Component& column, te::Track& t, juce::Colour c, bool focused, bool isGroup)
    {
        g.fillAll (Colours::background);
        auto header = column.getLocalBounds().removeFromTop (headerH);

        if (auto* group = isGroup ? &t : t.getParentFolderTrack())
        {
            g.setColour (Theme::ofTrack (*group));
            g.fillRoundedRectangle (header.toFloat().removeFromTop (4.0f).withTrimmedLeft (1.0f).withTrimmedRight (1.0f), 2.0f);
        }

        auto label = header.withTrimmedTop (4).reduced (8, 0);
        g.setColour (c);
        g.fillEllipse (label.removeFromLeft (12).toFloat().withSizeKeepingCentre (isGroup ? 12.0f : 10.0f, isGroup ? 12.0f : 10.0f));
        label.removeFromLeft (5);
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (12.5f, isGroup ? juce::Font::bold : juce::Font::plain));
        g.drawText (t.getName(), label, juce::Justification::centredLeft, true);

        g.setColour (focused ? Theme::raisedSurface() : Colours::panel);
        g.fillRoundedRectangle (column.getLocalBounds().removeFromBottom (mixerH).toFloat().reduced (3.0f, 2.0f), 14.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (headerH);
        auto mixer = r.removeFromBottom (mixerH).reduced (9, 8);

        for (auto* s : slots)
            s->setBounds (r.removeFromTop (slotH));

        stopButton.setBounds (r.removeFromTop (slotH).withSizeKeepingCentre (22, 22));

        inputButton.setBounds (mixer.removeFromTop (18));
        mixer.removeFromTop (4);
        auto buttons = mixer.removeFromTop (20);
        auto bw = buttons.getWidth() / 3;
        auto disc = [] (juce::Rectangle<int> b) { return b.withSizeKeepingCentre (20, 20); };
        armButton.setBounds (disc (buttons.removeFromLeft (bw)));
        soloButton.setBounds (disc (buttons.removeFromLeft (bw)));
        muteButton.setBounds (disc (buttons));
        mixer.removeFromTop (4);

        // Sends: up to 4 knobs per row
        auto sendsArea = mixer.removeFromTop (38);
        auto knobW = sendsArea.getWidth() / 4;
        for (int i = 0; i < sendKnobs.size() && i < 4; ++i)
        {
            auto cell = sendsArea.removeFromLeft (knobW);
            sendLabels[i]->setBounds (cell.removeFromBottom (12));
            sendKnobs[i]->setBounds (cell);
        }

        pan->setBounds (mixer.removeFromTop (30).withSizeKeepingCentre (30, 30));
        mixer.removeFromTop (4);

        auto faderArea = mixer;

        if (meter != nullptr)
            meter->setBounds (faderArea.removeFromRight (10));

        volume->setBounds (faderArea.withSizeKeepingCentre (juce::jmin (faderArea.getWidth(), 40), faderArea.getHeight()));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        viewState.focusTrack (track.get());
        viewState.selectionManager.selectOnly (track.get());

        if (e.mods.isPopupMenu() && e.y < headerH)
            showTrackMenu (*track, viewState, [this] { rename(); });
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (e.y < headerH)
            rename();
    }

private:
    EditViewState& viewState;
    te::AudioTrack::Ptr track;
    juce::OwnedArray<SessionSlot> slots;
    juce::TextButton stopButton { juce::String::fromUTF8 ("\xe2\x96\xa0") };
    SmallToggle muteButton { "m", Colours::mute, "Mute" }, soloButton { "s", Colours::solo, "Solo" },
                armButton { juce::String::fromUTF8 ("\xe2\x97\x8f"), Colours::record, "Record Arm" };
    juce::TextButton inputButton;
    std::unique_ptr<ParameterSlider> volume, pan;
    std::unique_ptr<LevelMeter> meter;
    juce::OwnedArray<ParameterSlider> sendKnobs;
    juce::OwnedArray<juce::Label> sendLabels;

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
        armButton.setToggleState (EngineHelpers::isTrackArmed (*track), juce::dontSendNotification);
        inputButton.setButtonText (getInputName (*track));
        repaint();
    }

    void timerCallback() override { refresh(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
};

//==============================================================================
/** A group (submix) track in the Session view: name and mixer, no clip slots. */
class SessionGroupColumn : public juce::Component, private juce::ChangeListener, private juce::Timer
{
public:
    SessionGroupColumn (EditViewState& evs, te::FolderTrack& t) : viewState (evs), track (&t)
    {
        for (auto* b : { &soloButton, &muteButton })
            addAndMakeVisible (*b);

        muteButton.onClick = [this] { track->setMute (! track->isMuted (false)); refresh(); };
        soloButton.onClick = [this] { Mixing::clickSolo (*track); refresh(); };

        if (auto vp = t.getVolumePlugin())
        {
            volume = std::make_unique<ParameterSlider> (*vp->volParam, juce::Slider::LinearVertical);
            pan = std::make_unique<ParameterSlider> (*vp->panParam, juce::Slider::RotaryHorizontalVerticalDrag);
            addAndMakeVisible (*volume);
            addAndMakeVisible (*pan);
        }

        if (auto lm = Mixing::findMeter (t))
        {
            meter = std::make_unique<LevelMeter> (lm->measurer);
            addAndMakeVisible (*meter);
        }

        for (auto p : t.pluginList)
        {
            if (auto send = dynamic_cast<te::AuxSendPlugin*> (p))
            {
                auto* k = sendKnobs.add (new ParameterSlider (*send->gain, juce::Slider::RotaryHorizontalVerticalDrag));
                k->setColour (juce::Slider::rotarySliderFillColourId, Colours::solo);
                addAndMakeVisible (k);
                auto* l = sendLabels.add (new juce::Label ({}, Mixing::busLetter (send->getBusNumber())));
                l->setFont (juce::FontOptions (10.0f));
                l->setJustificationType (juce::Justification::centred);
                l->setColour (juce::Label::textColourId, Colours::textDim);
                addAndMakeVisible (l);
            }
        }

        viewState.addChangeListener (this);
        refresh();
        startTimerHz (4);
    }

    ~SessionGroupColumn() override { viewState.removeChangeListener (this); }

    void paint (juce::Graphics& g) override
    {
        bool focused = viewState.focusedTrack == track.get();
        auto c = Theme::ofTrack (*track);
        tintFader (volume.get(), c);
        SessionTrackColumn::paintOrbitColumn (g, *this, *track, c, focused, true);

        // Each scene's row: what the group plays there, a dot per track with a clip, in its tone
        juce::Array<te::AudioTrack*> members;
        for (auto child : track->getAllSubTracks (true))
            if (auto at = dynamic_cast<te::AudioTrack*> (child))
                members.add (at);

        auto rows = getLocalBounds().withTrimmedTop (SessionTrackColumn::headerH).withTrimmedBottom (SessionTrackColumn::mixerH);
        const int numScenes = track->edit.getSceneList().getScenes().size();

        for (int i = 0; i < numScenes; ++i)
        {
            auto row = rows.removeFromTop (SessionTrackColumn::slotH).toFloat().reduced (2.0f, 1.5f);
            juce::Array<juce::Colour> playing;

            for (auto m : members)
                if (auto s = m->getClipSlotList().getClipSlots()[i]; s != nullptr && s->getClip() != nullptr)
                    playing.add (Theme::ofTrack (*m));

            g.setColour (playing.isEmpty() ? Colours::panel : Theme::tint (c));
            g.fillRoundedRectangle (row, juce::jmin (10.0f, row.getHeight() * 0.5f));

            auto x = row.getX() + 10.0f;
            for (auto pc : playing)
            {
                g.setColour (pc);
                g.fillEllipse (juce::Rectangle<float> (8.0f, 8.0f).withCentre ({ x + 4.0f, row.getCentreY() }));
                x += 12.0f;
            }
        }
    }

    void resized() override
    {
        auto mixer = getLocalBounds().removeFromBottom (SessionTrackColumn::mixerH).reduced (9, 8);
        mixer.removeFromTop (22);
        auto buttons = mixer.removeFromTop (20);
        auto bw = buttons.getWidth() / 3;
        buttons.removeFromLeft (bw);
        soloButton.setBounds (buttons.removeFromLeft (bw).withSizeKeepingCentre (20, 20));
        muteButton.setBounds (buttons.withSizeKeepingCentre (20, 20));
        mixer.removeFromTop (4);

        auto sendsArea = mixer.removeFromTop (38);
        auto knobW = sendsArea.getWidth() / 4;
        for (int i = 0; i < sendKnobs.size() && i < 4; ++i)
        {
            auto cell = sendsArea.removeFromLeft (knobW);
            sendLabels[i]->setBounds (cell.removeFromBottom (12));
            sendKnobs[i]->setBounds (cell);
        }

        if (pan != nullptr)
            pan->setBounds (mixer.removeFromTop (30).withSizeKeepingCentre (30, 30));

        mixer.removeFromTop (4);

        if (meter != nullptr)
            meter->setBounds (mixer.removeFromRight (10));

        if (volume != nullptr)
            volume->setBounds (mixer.withSizeKeepingCentre (juce::jmin (mixer.getWidth(), 40), mixer.getHeight()));
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        viewState.focusTrack (track.get());
        viewState.selectionManager.selectOnly (track.get());

        if (e.mods.isPopupMenu() && e.y < SessionTrackColumn::headerH)
            showTrackMenu (*track, viewState, {});
    }

private:
    EditViewState& viewState;
    te::FolderTrack::Ptr track;
    SmallToggle muteButton { "m", Colours::mute, "Mute" }, soloButton { "s", Colours::solo, "Solo" };
    std::unique_ptr<ParameterSlider> volume, pan;
    std::unique_ptr<LevelMeter> meter;
    juce::OwnedArray<ParameterSlider> sendKnobs;
    juce::OwnedArray<juce::Label> sendLabels;

    void refresh()
    {
        muteButton.setToggleState (track->isMuted (false), juce::dontSendNotification);
        soloButton.setToggleState (track->isSolo (false), juce::dontSendNotification);
        repaint();
    }

    void timerCallback() override { refresh(); }
    void changeListenerCallback (juce::ChangeBroadcaster*) override { repaint(); }
};

//==============================================================================
class SceneColumn : public juce::Component
{
public:
    SceneColumn (te::Edit& e) : edit (e)
    {
        auto numScenes = edit.getSceneList().getScenes().size();

        for (int i = 0; i < numScenes; ++i)
        {
            // Scenes are numbered discs, as on Somewhere
            auto* b = buttons.add (new juce::TextButton (getSceneName (i)));
            b->setColour (juce::TextButton::buttonColourId, Colours::panelLight);
            b->setTooltip ("Launch scene " + getSceneName (i));
            b->onClick = [this, i] { Launcher::userLaunchScene (edit, i); };
            addAndMakeVisible (b);
        }

        stopAll.onClick = [this] { Launcher::userStopAll (edit); };
        addAndMakeVisible (stopAll);

        addScene.onClick = [this]
        {
            auto& sl = edit.getSceneList();
            sl.ensureNumberOfScenes (sl.getScenes().size() + 1);
        };
        addAndMakeVisible (addScene);
    }

    void paint (juce::Graphics& g) override
    {
        g.fillAll (Colours::background);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText ("scenes", getLocalBounds().removeFromTop (SessionTrackColumn::headerH).withTrimmedTop (4), juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        r.removeFromTop (SessionTrackColumn::headerH);

        for (auto* b : buttons)
        {
            auto row = r.removeFromTop (SessionTrackColumn::slotH);
            b->setBounds (row.withSizeKeepingCentre (row.getHeight() - 2, row.getHeight() - 2));
        }

        stopAll.setBounds (r.removeFromTop (SessionTrackColumn::slotH).reduced (8, 2));
        r.removeFromTop (6);
        addScene.setBounds (r.removeFromTop (SessionTrackColumn::slotH).withSizeKeepingCentre (26, 26));
    }

private:
    te::Edit& edit;
    juce::OwnedArray<juce::TextButton> buttons;
    juce::TextButton stopAll { "stop all" }, addScene { "+" };

    juce::String getSceneName (int i)
    {
        if (auto s = edit.getSceneList().getScenes()[i])
            if (s->name.get().isNotEmpty())
                return s->name.get();

        return juce::String (i + 1);
    }
};

//==============================================================================
class SessionView : public juce::Component,
                    private juce::ValueTree::Listener,
                    private juce::AsyncUpdater
{
public:
    static constexpr int columnWidth = 124, sceneWidth = 110;

    SessionView (EditViewState& evs) : viewState (evs)
    {
        addAndMakeVisible (viewport);
        viewport.setViewedComponent (&content, false);
        viewport.setScrollBarsShown (true, true);
        viewport.setScrollBarThickness (10);

        // The scenes stay at the right, scrolling up and down with the grid
        addChildComponent (sceneViewport);
        sceneViewport.setScrollBarsShown (false, false);
        sceneViewport.setInterceptsMouseClicks (true, true);
        viewport.onScrolled = [this] (int y) { sceneViewport.setViewPosition (0, y); };

        if (viewState.edit.getSceneList().getScenes().isEmpty())
            viewState.edit.getSceneList().ensureNumberOfScenes (8);

        viewState.edit.state.addListener (this);
        rebuild();
    }

    ~SessionView() override
    {
        viewState.edit.state.removeListener (this);
        sceneViewport.setViewedComponent (nullptr, false);
    }

    std::function<void (const juce::File&, te::ClipSlot&)> onFileDropped;

    void paint (juce::Graphics& g) override { g.fillAll (Colours::background); }

    void resized() override
    {
        auto r = getLocalBounds();

        if (showScenes)
            sceneViewport.setBounds (r.removeFromRight (sceneWidth).withTrimmedBottom (viewport.getScrollBarThickness()));

        viewport.setBounds (r);
        layout();
    }

    /** The scene launch column, on the right (View > Show Scenes). */
    void setShowScenes (bool b)
    {
        if (b == showScenes)
            return;

        showScenes = b;
        sceneViewport.setVisible (b);
        resized();
    }

private:
    struct ScrollWatchingViewport : public juce::Viewport
    {
        std::function<void (int)> onScrolled;
        void visibleAreaChanged (const juce::Rectangle<int>& area) override   { if (onScrolled) onScrolled (area.getY()); }
    };

    EditViewState& viewState;
    ScrollWatchingViewport viewport;
    juce::Viewport sceneViewport;
    bool showScenes = false;
    juce::Component content;
    juce::OwnedArray<juce::Component> columns;
    std::unique_ptr<SceneColumn> scenes;

    void rebuild()
    {
        columns.clear();
        auto numScenes = viewState.edit.getSceneList().getScenes().size();

        // Same order as the arrangement: groups appear right before their tracks
        for (auto t : te::getAllTracks (viewState.edit))
        {
            if (EditViewState::isTrackHidden (*t))
                continue;

            if (auto at = dynamic_cast<te::AudioTrack*> (t))
            {
                auto* col = new SessionTrackColumn (viewState, *at, numScenes);
                col->onFileDropped = [this] (const juce::File& f, te::ClipSlot& s) { if (onFileDropped) onFileDropped (f, s); };
                columns.add (col);
                content.addAndMakeVisible (col);
            }
            else if (auto ft = dynamic_cast<te::FolderTrack*> (t))
            {
                auto* col = new SessionGroupColumn (viewState, *ft);
                columns.add (col);
                content.addAndMakeVisible (col);
            }
        }

        sceneViewport.setViewedComponent (nullptr, false);
        scenes = std::make_unique<SceneColumn> (viewState.edit);
        sceneViewport.setViewedComponent (scenes.get(), false);
        layout();
    }

    void layout()
    {
        auto numScenes = viewState.edit.getSceneList().getScenes().size();
        auto h = juce::jmax (viewport.getMaximumVisibleHeight(),
                             SessionTrackColumn::headerH + (numScenes + 1) * SessionTrackColumn::slotH + SessionTrackColumn::mixerH + 10);
        int x = 0;

        for (auto* c : columns)
        {
            c->setBounds (x, 0, columnWidth, h);
            x += columnWidth;
        }

        content.setSize (juce::jmax (viewport.getMaximumVisibleWidth(), x), h);
        scenes->setBounds (0, 0, sceneWidth, h);
        sceneViewport.setViewPosition (0, viewport.getViewPositionY());
    }

    void handleAsyncUpdate() override { rebuild(); }

    static bool isStructural (const juce::ValueTree& c)
    {
        return te::TrackList::isTrack (c) || c.hasType (te::IDs::SCENE) || c.hasType (te::IDs::CLIPSLOT)
                || (c.hasType (te::IDs::PLUGIN) && c[te::IDs::type].toString() == te::AuxSendPlugin::xmlTypeName)
                || (te::Clip::isClipState (c) && c.getParent().hasType (te::IDs::CLIPSLOT));
    }

    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree& c) override            { if (isStructural (c)) triggerAsyncUpdate(); }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree& c, int) override     { if (isStructural (c)) triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged (juce::ValueTree& p, int, int) override             { if (p.hasType (te::IDs::EDIT)) triggerAsyncUpdate(); }

    void valueTreePropertyChanged (juce::ValueTree& v, const juce::Identifier& id) override
    {
        if (te::TrackList::isTrack (v) && (id == te::IDs::name || id == te::IDs::colour || id == EditViewState::hiddenProperty))
            triggerAsyncUpdate();
    }
};

} // namespace elsewhere
