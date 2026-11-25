// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"

namespace elsewhere
{
//==============================================================================
/**
    Base for MIDI effects (the arpeggiator here, the step sequencer in StepsDevice.h): they sit before an instrument,
    rewrite the MIDI going into it, and leave audio alone.

    Provides a tempo-synced clock (locked to the song's beats while playing, free-running
    at the song tempo while stopped, so you can still play live) and note-off scheduling
    across blocks.
*/
class MidiEffectDevice : public ElsewhereDevice
{
public:
    MidiEffectDevice (te::PluginCreationInfo info) : ElsewhereDevice (info) {}

    bool takesMidiInput() override                      { return true; }
    bool isSynth() override                             { return false; }
    bool producesAudioWhenNoAudioInput() override       { return false; }

    static juce::StringArray rateNames()    { return { "1/4", "1/8", "1/8T", "1/16", "1/16T", "1/32" }; }
    static double rateInBeats (float v)
    {
        static const double r[] = { 1.0, 0.5, 1.0 / 3.0, 0.25, 1.0 / 6.0, 0.125 };
        return r[juce::jlimit (0, 5, juce::roundToInt (v))];
    }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        // (The engine can do this while audio is still running through the device)
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        rate = info.sampleRate;
        tempoPosition = std::make_unique<te::tempo::Sequence::Position> (te::createPosition (edit.tempoSequence));
        out.reserve (512);
        sounding.clear();
        sounding.reserve (128);
        freeBeat = 0.0;
        wasPlaying = false;
        onReset();
    }

    void deinitialise() override    { const juce::SpinLock::ScopedLockType dspGuard (dspLock); tempoPosition.reset(); }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked())
            return;     // being set up again for a new rate or buffer size

        if (fc.bufferForMidiMessages == nullptr || tempoPosition == nullptr)
            return;

        auto& midi = *fc.bufferForMidiMessages;
        const int n = fc.bufferNumSamples;

        // Clock for this block
        Block blk;
        blk.numSamples = n;
        blk.playing = fc.isPlaying;
        tempoPosition->set (fc.editTime.getStart());
        blk.beatsPerSample = tempoPosition->getTempo() / 60.0 / rate;

        if (fc.isPlaying)
        {
            blk.startBeat = tempoPosition->getBeats().inBeats();
        }
        else
        {
            blk.startBeat = freeBeat;
            freeBeat += blk.beatsPerSample * n;
        }

        out.clear();
        rendering = fc.isRendering;

        // Starting/stopping the transport, or an all-notes-off: silence what we're playing
        const bool transportChanged = fc.isPlaying != wasPlaying;
        wasPlaying = fc.isPlaying;

        if (midi.isAllNotesOff || transportChanged)
        {
            allNotesOff (0);
            onReset();
        }
        else if (fc.isPlaying && std::abs (blk.startBeat - expectedNextBeat) > 1.0e-6)
        {
            // The playhead jumped (play from the start, loop, click in the timeline): restart the pattern
            onReset();
        }

        expectedNextBeat = blk.endBeat();

        process (midi, blk);

        // Note-offs due in this block (sample positions are relative to this block's start)
        for (int i = (int) sounding.size(); --i >= 0;)
        {
            auto& s = sounding[(size_t) i];
            if (s.samplesLeft < n)
            {
                emit (juce::MidiMessage::noteOff (s.channel, s.note), (int) s.samplesLeft);
                sounding.erase (sounding.begin() + i);
            }
            else
            {
                s.samplesLeft -= n;
            }
        }

        const bool allOff = midi.isAllNotesOff;
        midi.clear();
        midi.isAllNotesOff = allOff;
        midi.mergeFromAndClear (out);
        midi.sortByTimestamp();
    }

    /** Current step (for the UI's playhead), -1 when idle. */
    int getCurrentStep() const      { return currentStep.load(); }

protected:
    struct Block
    {
        double startBeat = 0.0, beatsPerSample = 0.0;
        int numSamples = 0;
        bool playing = false;

        double endBeat() const              { return startBeat + beatsPerSample * numSamples; }
        int sampleAt (double beat) const    { return juce::jlimit (0, numSamples - 1, (int) std::lround ((beat - startBeat) / beatsPerSample)); }
    };

    /** Implemented by each effect: read the incoming MIDI, emit() the result. */
    virtual void process (const te::MidiMessageArray& in, const Block&) = 0;

    /** Called on transport start/stop or all-notes-off. */
    virtual void onReset() {}

    void emit (const juce::MidiMessage& m, int sampleOffset)
    {
        out.addMidiMessage (m, juce::jmax (0, sampleOffset) / rate, {});
    }

    /** Plays a note and schedules its note-off. */
    void playNote (int channel, int note, float velocity, int sampleOffset, double lengthSamples)
    {
        note = juce::jlimit (0, 127, note);

        // If that note is already sounding, end it first
        for (int i = (int) sounding.size(); --i >= 0;)
            if (sounding[(size_t) i].note == note)
            {
                emit (juce::MidiMessage::noteOff (sounding[(size_t) i].channel, note), sampleOffset);
                sounding.erase (sounding.begin() + i);
            }

        auto on = juce::MidiMessage::noteOn (channel, note, juce::jlimit (0.01f, 1.0f, velocity));
        out.addMidiMessage (on, (juce::jmax (0, sampleOffset) + 0.01) / rate, {});   // just after a note-off at the same time
        sounding.push_back ({ note, channel, juce::jmax ((double) sampleOffset + 1.0, (double) sampleOffset + lengthSamples) });
    }

    void allNotesOff (int sampleOffset)
    {
        for (auto& s : sounding)
            emit (juce::MidiMessage::noteOff (s.channel, s.note), sampleOffset);
        sounding.clear();
    }

    /** Swing: every second step is pushed later, up to a third of a step. */
    static double swingOffset (long long step, double stepBeats, float swingPercent)
    {
        return (step & 1) != 0 ? stepBeats * (double) swingPercent / 100.0 * (2.0 / 3.0) : 0.0;
    }

    double rate = 44100.0;
    double freeBeat = 0.0;
    bool rendering = false;     // this block is part of a mixdown
    std::atomic<int> currentStep { -1 };

    struct Sounding { int note, channel; double samplesLeft; };
    std::vector<Sounding> sounding;

private:
    std::unique_ptr<te::tempo::Sequence::Position> tempoPosition;
    te::MidiMessageArray out;
    bool wasPlaying = false;
    double expectedNextBeat = -1.0;
};

//==============================================================================
/**
    Elsewhere Arp: plays held notes one at a time in a pattern, locked to the song tempo.
*/
class ArpDevice : public MidiEffectDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereArp";
    enum Mode { up, down, upDown, converge, played, random, chord, numModes };
    static juce::StringArray modeNames()    { return { "Up", "Down", "Up/Down", "Converge", "As Played", "Random", "Chord" }; }

    ArpDevice (te::PluginCreationInfo info) : MidiEffectDevice (info)
    {
        mode     = addParameter ("mode", "Pattern", (float) up, stepped (0, numModes - 1),
                                 [] (float v) { return modeNames()[juce::jlimit (0, numModes - 1, juce::roundToInt (v))]; });
        rateP    = addParameter ("rate", "Rate", 3.0f, stepped (0, 5), [] (float v) { return rateNames()[juce::jlimit (0, 5, juce::roundToInt (v))]; });
        octaves  = addParameter ("octaves", "Octaves", 1.0f, stepped (1, 4), [] (float v) { return juce::String (juce::roundToInt (v)); });
        gate     = addParameter ("gate", "Gate", 60.0f, { 5.0f, 200.0f }, percentText);
        swing    = addParameter ("swing", "Swing", 0.0f, { 0.0f, 75.0f }, percentText);
        velocity = addParameter ("velocity", "Velocity", 0.0f, stepped (0, 127),
                                 [] (float v) { return v < 0.5f ? juce::String ("As Played") : juce::String (juce::roundToInt (v)); });
        latch    = addParameter ("latch", "Latch", 0.0f, stepped (0, 1), onOffText);
    }

    juce::String getName() const override              { return "Elsewhere Arp"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Arp"; }
    int getDeviceViewWidth() const override            { return 560; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    /** The notes the arp is currently cycling through (for the UI). */
    std::vector<int> getHeldNotes() const
    {
        const juce::SpinLock::ScopedLockType sl (uiLock);
        return uiNotes;
    }

    int getLastPlayedNote() const   { return lastPlayed.load(); }

    Param mode, rateP, octaves, gate, swing, velocity, latch;

protected:
    struct Held { int note, channel; float velocity; };
    std::vector<Held> held;            // in the order played
    long long stepCounter = 0;
    double nextFreeStepBeat = -1.0;    // free-running clock: next step time
    juce::Random rng;
    mutable juce::SpinLock uiLock;
    std::vector<int> uiNotes;
    std::atomic<int> lastPlayed { -1 };
    std::array<int, 128> physicallyDown {};

    void onReset() override
    {
        stepCounter = 0;
        nextFreeStepBeat = -1.0;
        currentStep = -1;
    }

    void process (const te::MidiMessageArray& in, const Block& blk) override
    {
        const bool latched = latch.get() > 0.5f;
        int firstNoteOffset = -1;

        for (auto& m : in)
        {
            const int offset = juce::roundToInt (m.getTimeStamp() * rate);

            if (m.isNoteOn())
            {
                // With latch: the first key after everything was released starts a new chord
                if (latched && ! anyKeyDown())
                    held.clear();

                physicallyDown[(size_t) m.getNoteNumber()] = 1;
                auto it = std::find_if (held.begin(), held.end(), [&] (auto& h) { return h.note == m.getNoteNumber(); });
                if (it == held.end())
                    held.push_back ({ m.getNoteNumber(), m.getChannel(), m.getFloatVelocity() });

                if (firstNoteOffset < 0 && held.size() == 1)
                    firstNoteOffset = offset;
            }
            else if (m.isNoteOff())
            {
                physicallyDown[(size_t) m.getNoteNumber()] = 0;
                if (! latched)
                    held.erase (std::remove_if (held.begin(), held.end(), [&] (auto& h) { return h.note == m.getNoteNumber(); }), held.end());
            }
            else
            {
                emit (m, offset);   // pitch bend, CCs etc. pass straight through
            }
        }

        if (! latched && ! anyKeyDown())
            held.clear();

        {
            const juce::SpinLock::ScopedTryLockType sl (uiLock);
            if (sl.isLocked())
            {
                uiNotes.clear();
                for (auto& h : held) uiNotes.push_back (h.note);
            }
        }

        if (held.empty())
        {
            onReset();
            return;
        }

        const double stepBeats = rateInBeats (rateP.get());
        const double stepSamples = stepBeats / blk.beatsPerSample;
        const double gateSamples = stepSamples * gate.get() / 100.0;

        if (blk.playing)
        {
            // Locked to the song's grid
            // Start one step back: a swung step can land after the start of its grid slot
            auto k = (long long) std::floor (blk.startBeat / stepBeats) - 1;
            for (;; ++k)
            {
                const double beat = (double) k * stepBeats + swingOffset (k, stepBeats, swing.get());
                if (beat >= blk.endBeat() - 1.0e-9) break;
                if (beat < blk.startBeat - 1.0e-9) continue;
                step (blk.sampleAt (beat), gateSamples);
            }
        }
        else
        {
            // Free running: the first note starts the pattern immediately
            if (nextFreeStepBeat < 0.0)
                nextFreeStepBeat = blk.startBeat + juce::jmax (0, firstNoteOffset) * blk.beatsPerSample;

            while (nextFreeStepBeat < blk.endBeat())
            {
                step (blk.sampleAt (nextFreeStepBeat), gateSamples);
                nextFreeStepBeat += stepBeats + swingOffset (stepCounter, stepBeats, swing.get()) - swingOffset (stepCounter - 1, stepBeats, swing.get());
            }
        }
    }

    bool anyKeyDown() const
    {
        for (auto d : physicallyDown) if (d != 0) return true;
        return false;
    }

    void step (int sampleOffset, double gateSamples)
    {
        auto pattern = buildPattern();
        if (pattern.empty())
            return;

        const auto m = (Mode) juce::jlimit (0, numModes - 1, juce::roundToInt (mode.get()));
        const int fixedVel = juce::roundToInt (velocity.get());
        auto velFor = [&] (const Held& h) { return fixedVel > 0 ? (float) fixedVel / 127.0f : h.velocity; };

        if (m == chord)
        {
            for (auto& h : held)
                for (int o = 0; o < juce::roundToInt (octaves.get()); ++o)
                    playNote (h.channel, h.note + 12 * o, velFor (h), sampleOffset, gateSamples);
            lastPlayed = held.front().note;
        }
        else
        {
            const auto& h = m == random ? pattern[(size_t) rng.nextInt ((int) pattern.size())]
                                        : pattern[(size_t) (stepCounter % (long long) pattern.size())];
            playNote (h.channel, h.note, velFor (h), sampleOffset, gateSamples);
            lastPlayed = h.note;
        }

        currentStep = (int) (stepCounter % 64);
        ++stepCounter;
    }

    std::vector<Held> buildPattern() const
    {
        const auto m = (Mode) juce::jlimit (0, numModes - 1, juce::roundToInt (mode.get()));
        const int oct = juce::jlimit (1, 4, juce::roundToInt (octaves.get()));

        std::vector<Held> base = held;
        if (m != played)
            std::sort (base.begin(), base.end(), [] (auto& a, auto& b) { return a.note < b.note; });

        std::vector<Held> p;
        for (int o = 0; o < oct; ++o)
            for (auto h : base) { h.note += 12 * o; p.push_back (h); }

        switch (m)
        {
            case down:
                std::reverse (p.begin(), p.end());
                break;
            case upDown:
                if (p.size() > 2)
                    for (int i = (int) p.size() - 2; i >= 1; --i) p.push_back (p[(size_t) i]);
                break;
            case converge:
            {
                std::vector<Held> c;
                for (size_t lo = 0, hi = p.size(); lo < hi;)
                {
                    c.push_back (p[lo++]);
                    if (lo < hi) c.push_back (p[--hi]);
                }
                p = c;
                break;
            }
            case up: case played: case random: case chord: case numModes:
                break;
        }

        return p;
    }
};

//==============================================================================
class ArpView : public juce::Component, private juce::Timer
{
public:
    ArpView (ArpDevice& d)
        : arp (d),
          modeChoice (d.mode.param(), ArpDevice::modeNames()),
          rateChoice (d.rateP.param(), MidiEffectDevice::rateNames(), Colours::solo),
          octaves (d.octaves.param()), gate (d.gate.param()), swing (d.swing.param()), velocity (d.velocity.param()),
          latch (d.latch.param(), "Latch")
    {
        for (juce::Component* c : { (juce::Component*) &modeChoice, (juce::Component*) &rateChoice, (juce::Component*) &octaves,
                                    (juce::Component*) &gate, (juce::Component*) &swing, (juce::Component*) &velocity, (juce::Component*) &latch })
            addAndMakeVisible (c);
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        // Mini keyboard strip showing held notes and the one playing now
        auto r = keysArea.toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 3.0f);

        const int lo = 36, hi = 96;
        const float w = r.getWidth() / (float) (hi - lo);
        for (int n = lo; n < hi; ++n)
        {
            auto key = juce::Rectangle<float> (r.getX() + (float) (n - lo) * w, r.getY() + 2, w - 1.0f, r.getHeight() - 4);
            bool black = juce::MidiMessage::isMidiNoteBlack (n);
            bool isHeld = std::find (notes.begin(), notes.end(), n) != notes.end();
            g.setColour (n == playingNote && flash > 0.0f ? Colours::accent
                         : isHeld ? Colours::accent.withAlpha (0.45f)
                         : black ? Colours::panelDark : Colours::panelLight);
            g.fillRect (key);
        }

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (notes.empty() ? juce::String ("Hold some notes (put an instrument after the Arp)") : juce::String (notes.size()) + " notes held",
                    keysArea.translated (0, keysArea.getHeight() + 2).withHeight (14), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        modeChoice.setBounds (r.removeFromTop (24));
        r.removeFromTop (4);
        rateChoice.setBounds (r.removeFromTop (22).removeFromLeft (300));
        r.removeFromTop (6);
        keysArea = r.removeFromBottom (40).withTrimmedBottom (16);
        auto knobs = r;
        auto w = 70;
        for (auto* k : { &octaves, &gate, &swing, &velocity })
            k->setBounds (knobs.removeFromLeft (w));
        latch.setBounds (knobs.removeFromLeft (80).withSizeKeepingCentre (64, 24));
    }

private:
    ArpDevice& arp;
    DeviceUI::Choice modeChoice, rateChoice;
    DeviceUI::Knob octaves, gate, swing, velocity;
    DeviceUI::Toggle latch;
    juce::Rectangle<int> keysArea;
    std::vector<int> notes;
    int playingNote = -1, lastStep = -1;
    float flash = 0.0f;

    void timerCallback() override
    {
        notes = arp.getHeldNotes();
        auto s = arp.getCurrentStep();
        if (s != lastStep && s >= 0) { lastStep = s; flash = 1.0f; playingNote = arp.getLastPlayedNote(); }
        flash = juce::jmax (0.0f, flash - 0.15f);
        repaint (keysArea.expanded (0, 16));
    }
};

inline std::unique_ptr<juce::Component> ArpDevice::createDeviceView()      { return std::make_unique<ArpView> (*this); }

} // namespace elsewhere
