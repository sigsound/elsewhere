// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"

namespace elsewhere
{
//==============================================================================
/**
    Base for MIDI effects (arpeggiator, step sequencer): they sit before an instrument,
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
/**
    Elsewhere Steps: a note sequencer of up to 32 steps.

    Each step is on or off, and has a value in each lane:

      - Pitch (semitones from the root, kept to the Scale) and Velocity
      - Length: how long the note lasts, as a share of the Gate. Past 100 % it runs into the
        next step, which a synth with glide plays as a slide
      - Chance: how likely the step is to play
      - Repeat: plays the note several times within the step
      - Nudge: moves the step earlier or later
      - Every: plays only on some trips round the pattern (the 1st of 2, the 3rd of 4 ...)

    A lane can have its own number of steps, so (say) five pitches go round under a pattern of
    sixteen and the two only line up again after eighty. The pattern can run forwards, backwards,
    back and forth, or in a random order.

    It plays with the transport, or (Follow Keys) only while you hold a key, transposed to it.

    Which step plays when is worked out from where the song is, not counted, so a pattern sounds
    the same from wherever you start. Chance and Random are rolled per step and per time through,
    and repeat exactly on a mixdown.
*/
class StepsDevice : public MidiEffectDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereSteps";
    static constexpr int numSteps = 32;

    enum Lane { pitchLane, velocityLane, lengthLane, chanceLane, repeatLane, nudgeLane, everyLane, numLanes };
    static juce::StringArray laneNames()        { return { "Pitch", "Velocity", "Length", "Chance", "Repeat", "Nudge", "Every" }; }

    enum Direction { forward, backward, bounce, randomOrder, numDirections };
    static juce::StringArray directionNames()   { return { "Forward", "Back", "Bounce", "Random" }; }

    /** Scales, as the semitones above the root that belong to them. */
    struct Scale { const char* name; std::vector<int> notes; };

    static const std::vector<Scale>& scales()
    {
        static const std::vector<Scale> all = {
            { "Any Note",         { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 } },
            { "Major",            { 0, 2, 4, 5, 7, 9, 11 } },
            { "Minor",            { 0, 2, 3, 5, 7, 8, 10 } },
            { "Dorian",           { 0, 2, 3, 5, 7, 9, 10 } },
            { "Phrygian",         { 0, 1, 3, 5, 7, 8, 10 } },
            { "Lydian",           { 0, 2, 4, 6, 7, 9, 11 } },
            { "Mixolydian",       { 0, 2, 4, 5, 7, 9, 10 } },
            { "Harmonic Minor",   { 0, 2, 3, 5, 7, 8, 11 } },
            { "Major Pentatonic", { 0, 2, 4, 7, 9 } },
            { "Minor Pentatonic", { 0, 3, 5, 7, 10 } },
            { "Blues",            { 0, 3, 5, 6, 7, 10 } },
            { "Whole Tone",       { 0, 2, 4, 6, 8, 10 } },
        };
        return all;
    }

    /** Moves a pitch (semitones from the root) down to the nearest note of a scale. */
    static int snapToScale (int semitones, int scaleIndex)
    {
        const auto& notes = scales()[(size_t) juce::jlimit (0, (int) scales().size() - 1, scaleIndex)].notes;
        const int octave = (int) std::floor (semitones / 12.0), within = semitones - octave * 12;
        int best = 0;
        for (int n : notes)
            if (n <= within)
                best = n;
        return octave * 12 + best;
    }

    static juce::StringArray everyNames()
    {
        return { "Always", "1st of 2", "2nd of 2", "1st of 3", "2nd of 3", "3rd of 3", "1st of 4", "2nd of 4", "3rd of 4", "4th of 4" };
    }

    /** Whether an Every setting lets a step play on this trip round the pattern (counted from 0). */
    static bool everyAllows (int setting, long long trip)
    {
        if (setting <= 0) return true;
        const int of = setting <= 2 ? 2 : setting <= 5 ? 3 : 4;
        const int which = setting - (of == 2 ? 1 : of == 3 ? 3 : 6);
        return trip % of == which;
    }

    struct Step
    {
        Param on, pitch, velocity, length, chance, repeat, nudge, every;

        Param& lane (int l)
        {
            switch (l)
            {
                case velocityLane:  return velocity;
                case lengthLane:    return length;
                case chanceLane:    return chance;
                case repeatLane:    return repeat;
                case nudgeLane:     return nudge;
                case everyLane:     return every;
                default:            return pitch;
            }
        }
    };

    StepsDevice (te::PluginCreationInfo info) : MidiEffectDevice (info)
    {
        auto signedText = [] (float v) { auto s = juce::roundToInt (v); return (s > 0 ? "+" : "") + juce::String (s); };
        auto wholeText = [] (float v) { return juce::String (juce::roundToInt (v)); };

        rateP  = addParameter ("rate", "Rate", 3.0f, stepped (0, 5), [] (float v) { return rateNames()[juce::jlimit (0, 5, juce::roundToInt (v))]; });
        length = addParameter ("length", "Steps", 16.0f, stepped (1, numSteps), wholeText);
        swing  = addParameter ("swing", "Swing", 0.0f, { 0.0f, 75.0f }, percentText);
        gate   = addParameter ("gate", "Gate", 50.0f, { 5.0f, 200.0f }, percentText);
        root   = addParameter ("root", "Root", 48.0f, stepped (24, 96), [] (float v) { return juce::MidiMessage::getMidiNoteName (juce::roundToInt (v), true, true, 3); });
        follow = addParameter ("follow", "Follow Keys", 0.0f, stepped (0, 1), onOffText);
        direction = addParameter ("direction", "Direction", (float) forward, stepped (0, numDirections - 1),
                                  [] (float v) { return directionNames()[juce::jlimit (0, numDirections - 1, juce::roundToInt (v))]; });
        scale  = addParameter ("scale", "Scale", 0.0f, stepped (0, (float) scales().size() - 1.0f),
                               [] (float v) { return juce::String (scales()[(size_t) juce::jlimit (0, (int) scales().size() - 1, juce::roundToInt (v))].name); });

        // A simple starting pattern: root on the beats, a fifth and an octave in between
        static const int defaultOn[16]    = { 1,0,1,0, 1,0,1,1, 1,0,1,0, 1,0,1,1 };
        static const int defaultPitch[16] = { 0,0,7,0, 12,0,7,10, 0,0,7,0, 12,0,15,12 };

        for (int i = 0; i < numSteps; ++i)
        {
            auto id = "s" + juce::String (i + 1);
            auto nm = "Step " + juce::String (i + 1) + " ";
            auto& s = steps[(size_t) i];
            s.on       = addParameter (id + "on", nm + "On", i < 16 ? (float) defaultOn[i] : 0.0f, stepped (0, 1), onOffText);
            s.pitch    = addParameter (id + "pitch", nm + "Pitch", i < 16 ? (float) defaultPitch[i] : 0.0f, stepped (-24, 24), signedText);
            s.velocity = addParameter (id + "vel", nm + "Velocity", i % 4 == 0 ? 110.0f : 85.0f, stepped (1, 127), wholeText);
            s.length   = addParameter (id + "len", nm + "Length", 100.0f, skewed (10.0f, 400.0f, 100.0f), percentText);
            s.chance   = addParameter (id + "chance", nm + "Chance", 100.0f, stepped (0, 100), percentText);
            s.repeat   = addParameter (id + "rep", nm + "Repeat", 1.0f, stepped (1, 8), [] (float v) { return juce::String (juce::roundToInt (v)) + "x"; });
            s.nudge    = addParameter (id + "nudge", nm + "Nudge", 0.0f, stepped (-50, 50), [signedText] (float v) { return signedText (v) + " %"; });
            s.every    = addParameter (id + "every", nm + "Every", 0.0f, stepped (0, 9), [] (float v) { return everyNames()[juce::jlimit (0, 9, juce::roundToInt (v))]; });
        }

        static const char* laneIds[numLanes] = { "pitchsteps", "velsteps", "lensteps", "chancesteps", "repsteps", "nudgesteps", "everysteps" };
        for (int l = 0; l < numLanes; ++l)
            laneSteps[(size_t) l] = addParameter (laneIds[l], laneNames()[l] + " Steps", 0.0f, stepped (0, numSteps),
                                                  [] (float v) { return v < 0.5f ? juce::String ("Pattern") : juce::String (juce::roundToInt (v)); });
    }

    juce::String getName() const override              { return "Elsewhere Steps"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Steps"; }
    int getDeviceViewWidth() const override            { return 940; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    std::unique_ptr<juce::Component> createWindowView() override;
    juce::Point<int> getWindowViewSize() const override    { return { 1120, 640 }; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        turn = 0;   // (so a mixdown rolls the same dice every time)
        MidiEffectDevice::initialise (info);
    }

    //==============================================================================
    int patternSteps() const        { return juce::jlimit (1, numSteps, juce::roundToInt (length.get())); }

    /** How many steps a lane goes round in: its own count, or the pattern's. */
    int stepsInLane (int lane) const
    {
        const int own = juce::roundToInt (laneSteps[(size_t) lane].get());
        return own > 0 ? juce::jmin (own, numSteps) : patternSteps();
    }

    /** Which step is under the playhead at step count `k`, for the on/off row (lane -1) or a lane. */
    int indexFor (int lane, long long k) const
    {
        const int len = lane < 0 ? patternSteps() : stepsInLane (lane);
        const auto count = (long long) len;

        switch ((Direction) juce::jlimit (0, numDirections - 1, juce::roundToInt (direction.get())))
        {
            case backward:      return (int) (count - 1 - k % count);
            case bounce:
            {
                if (len < 2) return 0;
                const auto there = k % (2 * count - 2);
                return (int) (there < count ? there : 2 * count - 2 - there);
            }
            case randomOrder:   return (int) (roll (k, 0x51ed) % (juce::uint32) len);
            case forward: case numDirections: break;
        }
        return (int) (k % count);
    }

    /** The step count the pattern is at (for the playheads), -1 when idle. */
    long long getCurrentCount() const       { return currentCount.load(); }

    //==============================================================================
    // Things to do to a pattern (from the Tools menu)
    void randomiseLane (int lane)
    {
        juce::Random r;
        edit.getUndoManager().beginNewTransaction ("Randomise " + laneNames()[lane]);

        for (int i = 0; i < stepsInLane (lane); ++i)
        {
            float v = 0.0f;
            switch (lane)
            {
                case pitchLane:     v = (float) snapToScale (r.nextInt ({ -12, 16 }), juce::roundToInt (scale.get())); break;
                case velocityLane:  v = (float) r.nextInt ({ 45, 128 }); break;
                case lengthLane:    { static const float l[] = { 50, 100, 100, 100, 150, 200 }; v = l[r.nextInt (6)]; break; }
                case chanceLane:    v = r.nextFloat() < 0.5f ? 100.0f : (float) r.nextInt ({ 30, 100 }); break;
                case repeatLane:    v = r.nextFloat() < 0.75f ? 1.0f : (float) r.nextInt ({ 2, 5 }); break;
                case nudgeLane:     v = (float) r.nextInt ({ -15, 16 }); break;
                case everyLane:     v = r.nextFloat() < 0.7f ? 0.0f : (float) r.nextInt ({ 1, 10 }); break;
                default: break;
            }
            steps[(size_t) i].lane (lane).param().setParameter (v, juce::sendNotification);
        }
    }

    void randomiseSteps()
    {
        juce::Random r;
        edit.getUndoManager().beginNewTransaction ("Randomise Steps");
        for (int i = 0; i < patternSteps(); ++i)
            steps[(size_t) i].on.param().setParameter (i == 0 || r.nextFloat() < 0.55f ? 1.0f : 0.0f, juce::sendNotification);
    }

    void resetLane (int lane)
    {
        edit.getUndoManager().beginNewTransaction ("Reset " + laneNames()[lane]);
        for (auto& s : steps)
        {
            auto& p = s.lane (lane).param();
            p.setParameter (p.getDefaultValue().value_or (p.getCurrentBaseValue()), juce::sendNotification);
        }
    }

    /** Moves the whole pattern (every lane) one step later (+1) or earlier (-1), round the end. */
    void shift (int delta)
    {
        const int len = patternSteps();
        edit.getUndoManager().beginNewTransaction ("Shift Pattern");

        for (int l = -1; l < numLanes; ++l)
        {
            std::vector<float> was;
            for (int i = 0; i < len; ++i)
                was.push_back ((l < 0 ? steps[(size_t) i].on : steps[(size_t) i].lane (l)).param().getCurrentBaseValue());
            for (int i = 0; i < len; ++i)
                (l < 0 ? steps[(size_t) i].on : steps[(size_t) i].lane (l)).param().setParameter (was[(size_t) (((i - delta) % len + len) % len)], juce::sendNotification);
        }
    }

    /** Back to the pattern a new Step Sequencer starts with. */
    void loadDefaults()
    {
        edit.getUndoManager().beginNewTransaction ("Start Over");
        for (auto& p : params)
            p->getParameter().setParameter (p->getParameter().getDefaultValue().value_or (p->getParameter().getCurrentBaseValue()), juce::sendNotification);
    }

    bool canDouble() const      { return patternSteps() * 2 <= numSteps; }

    /** Twice as many steps, the second half a copy of the first, ready to be changed. */
    void doubleLength()
    {
        const int len = patternSteps();
        if (! canDouble()) return;
        edit.getUndoManager().beginNewTransaction ("Double Pattern");

        for (int i = 0; i < len; ++i)
        {
            auto& from = steps[(size_t) i];
            auto& to = steps[(size_t) (i + len)];
            to.on.param().setParameter (from.on.param().getCurrentBaseValue(), juce::sendNotification);
            for (int l = 0; l < numLanes; ++l)
                to.lane (l).param().setParameter (from.lane (l).param().getCurrentBaseValue(), juce::sendNotification);
        }
        length.param().setParameter ((float) (len * 2), juce::sendNotification);
    }

    std::array<Step, numSteps> steps;
    std::array<Param, numLanes> laneSteps;
    Param rateP, length, swing, gate, root, follow, direction, scale;

private:
    std::array<int, 128> keysDown {};
    int transposeNote = -1;
    double freeOrigin = -1.0;           // stopped, following keys: the beat the first key went down on
    juce::uint32 turn = 0;              // changes each time the pattern starts over, so chance differs each time round a loop
    std::atomic<long long> currentCount { -1 };

    void onReset() override
    {
        freeOrigin = -1.0;
        currentStep = -1;
        currentCount = -1;
        turn = rendering ? 1 : turn + 1;    // (a mixdown always rolls the same dice)
    }

    /** A dice roll that is always the same for the same step count, use and time through. */
    juce::uint32 roll (long long k, juce::uint32 use) const
    {
        auto x = (juce::uint64) k * 0x9E3779B97F4A7C15ull + (juce::uint64) (turn * 7919u + use) * 0xBF58476D1CE4E5B9ull;
        x ^= x >> 31; x *= 0x94D049BB133111EBull; x ^= x >> 29;
        return (juce::uint32) (x >> 16);
    }

    void process (const te::MidiMessageArray& in, const Block& blk) override
    {
        const bool followKeys = follow.get() > 0.5f;
        int firstKeyOffset = -1;

        for (auto& m : in)
        {
            const int offset = juce::roundToInt (m.getTimeStamp() * rate);

            if (followKeys && m.isNoteOn())
            {
                if (transposeNote < 0) firstKeyOffset = offset;
                keysDown[(size_t) m.getNoteNumber()] = 1;
                transposeNote = m.getNoteNumber();
            }
            else if (followKeys && m.isNoteOff())
            {
                keysDown[(size_t) m.getNoteNumber()] = 0;
                if (m.getNoteNumber() == transposeNote)
                {
                    transposeNote = -1;
                    for (int k = 127; k >= 0; --k) if (keysDown[(size_t) k] != 0) { transposeNote = k; break; }
                }
            }
            else
            {
                emit (m, offset);   // without Follow Keys, what you play passes through on top of the pattern
            }
        }

        if (followKeys && transposeNote < 0)
        {
            onReset();
            return;
        }

        // The grid the steps sit on: the song's beats, or (stopped, following keys) beats counted from the first key
        double origin = 0.0;

        if (! blk.playing)
        {
            if (! followKeys)
            {
                currentStep = -1;
                currentCount = -1;
                return;
            }

            if (freeOrigin < 0.0)
                freeOrigin = blk.startBeat + juce::jmax (0, firstKeyOffset) * blk.beatsPerSample;
            origin = freeOrigin;
        }

        const int len = patternSteps();
        const double stepBeats = rateInBeats (rateP.get());
        const double start = blk.startBeat - origin, end = blk.endBeat() - origin;
        const double gateShare = gate.get() / 100.0;
        const int rootNote = followKeys ? transposeNote : juce::roundToInt (root.get());
        const int scaleIndex = juce::roundToInt (scale.get());
        const float swingAmount = swing.get();
        const auto now = (long long) std::floor (start / stepBeats);

        if (now >= 0)
        {
            currentCount = now;
            currentStep = indexFor (-1, now);
        }

        // A step can sound up to half a step early (Nudge) and, with Swing, Nudge and Repeat, well
        // into the steps after it: look at the ones either side of this block
        for (auto k = juce::jmax (0LL, now - 2); ((double) k - 0.5) * stepBeats < end; ++k)
        {
            if (steps[(size_t) indexFor (-1, k)].on.get() < 0.5f)
                continue;

            if (! everyAllows (juce::roundToInt (steps[(size_t) indexFor (everyLane, k)].every.get()), k / len))
                continue;

            const float chance = steps[(size_t) indexFor (chanceLane, k)].chance.get();
            if (chance < 99.5f && (float) (roll (k, 0xc4a9) % 10000u) >= chance * 100.0f)
                continue;

            const double nudge = steps[(size_t) indexFor (nudgeLane, k)].nudge.get() / 100.0;
            const double at = juce::jmax (0.0, ((double) k + nudge) * stepBeats + swingOffset (k, stepBeats, swingAmount));
            const int repeats = juce::jlimit (1, 8, juce::roundToInt (steps[(size_t) indexFor (repeatLane, k)].repeat.get()));
            const double each = stepBeats / repeats;
            const double share = gateShare * steps[(size_t) indexFor (lengthLane, k)].length.get() / 100.0;
            const double lengthSamples = each * (repeats > 1 ? juce::jmin (0.95, share) : share) / blk.beatsPerSample;

            const int note = rootNote + snapToScale (juce::roundToInt (steps[(size_t) indexFor (pitchLane, k)].pitch.get()), scaleIndex);
            const float velocity = steps[(size_t) indexFor (velocityLane, k)].velocity.get() / 127.0f;

            for (int j = 0; j < repeats; ++j)
            {
                const double hit = at + j * each;
                if (hit >= start - 1.0e-9 && hit < end - 1.0e-9)
                    playNote (1, note, velocity, blk.sampleAt (hit + origin), lengthSamples);
            }
        }
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

//==============================================================================
/** The step grid: a row of on/off buttons, and under it the bars of one lane (drag to set them). */
class StepGrid : public juce::Component, private juce::Timer
{
public:
    StepGrid (StepsDevice& d) : dev (d)
    {
        setTitle ("Steps");
        startTimerHz (30);
    }

    void setLane (int l)        { lane = l; repaint(); }
    int getLane() const         { return lane; }

    /** Without the row of on/off buttons: for a lane stacked under another grid that has them. */
    void setShowsOnRow (bool shouldShow)    { showOnRow = shouldShow; repaint(); }

    /** Keeps its columns in line with grids showing the other lanes. */
    void setLinedUpWithAllLanes (bool b)    { allLanes = b; repaint(); }

    /** 16 across until something needs more. */
    int columns() const
    {
        if (dev.patternSteps() > 16 || dev.stepsInLane (lane) > 16)
            return 32;

        if (allLanes)
            for (int l = 0; l < StepsDevice::numLanes; ++l)
                if (dev.stepsInLane (l) > 16)
                    return 32;

        return 16;
    }

    void paint (juce::Graphics& g) override
    {
        const int cols = columns();
        const int len = dev.patternSteps(), laneLen = dev.stepsInLane (lane);
        const auto count = dev.getCurrentCount();
        const int playing = count >= 0 ? dev.indexFor (-1, count) : -1;
        const int lanePlaying = count >= 0 ? dev.indexFor (lane, count) : -1;
        const auto colour = laneColour (lane);
        g.setFont (juce::FontOptions (cols > 16 ? 8.5f : 9.5f));

        for (int i = 0; i < cols; ++i)
        {
            auto& s = dev.steps[(size_t) i];
            const bool on = s.on.get() > 0.5f;
            auto col = cell (i);

            // On/off
            if (showOnRow)
            {
                auto onBox = col.withHeight (onH).reduced (1.5f);
                g.setColour ((on ? Colours::accent : Colours::panelLight).withMultipliedAlpha (i < len ? 1.0f : 0.3f));
                g.fillRoundedRectangle (onBox, 3.0f);
                if (i == playing)
                {
                    g.setColour (Colours::text);
                    g.drawRoundedRectangle (onBox, 3.0f, 2.0f);
                }
                if (i % 4 == 0)
                {
                    g.setColour (on && i < len ? Colours::textInverse : Colours::textDim);
                    g.drawText (juce::String (i + 1), onBox.toNearestInt(), juce::Justification::centred);
                }
            }

            // The lane's bar: from the middle for values that go both ways, otherwise from the bottom
            const float alpha = i < laneLen ? 1.0f : 0.3f;
            auto box = laneBox (i);
            g.setColour (i == lanePlaying ? Colours::display.interpolatedWith (Colours::text, 0.14f) : Colours::display);
            g.fillRect (box);

            auto& p = s.lane (lane).param();
            const auto range = p.getValueRange();
            const float v = p.getCurrentValue();
            const float n = p.valueRange.convertTo0to1 (juce::jlimit (range.getStart(), range.getEnd(), v));
            const bool sounds = on || laneLen != len;    // (a lane with its own count isn't tied to this column's step)
            g.setColour (colour.withAlpha ((sounds ? 0.85f : 0.3f) * alpha));

            if (range.getStart() < 0.0f)
            {
                const float mid = box.getCentreY(), y = box.getBottom() - n * box.getHeight();
                g.fillRect (juce::Rectangle<float>::leftTopRightBottom (box.getX(), juce::jmin (mid, y), box.getRight(), juce::jmax (mid, y) + 1.0f));
            }
            else
            {
                g.fillRect (box.withTop (box.getBottom() - juce::jmax (1.0f, n * box.getHeight())));
            }

            if (auto text = cellText (lane, v); text.isNotEmpty())
            {
                g.setColour (Colours::text.withAlpha (alpha));
                g.drawText (text, box.toNearestInt().removeFromBottom (12), juce::Justification::centred);
            }
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        auto i = stepAt (e.position.x);
        if (i < 0) return;

        onRow = showOnRow && e.position.y < onH + 2.0f;

        if (onRow)
        {
            auto& on = dev.steps[(size_t) i].on.param();
            paintValue = on.getCurrentValue() > 0.5f ? 0.0f : 1.0f;
            on.setParameter (paintValue, juce::sendNotification);
        }
        else
        {
            setFromMouse (i, e.position.y);
        }
        repaint();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto i = stepAt (e.position.x);
        if (i < 0) return;

        // Dragging across steps paints the same on/off value, or sets bars as you go
        if (onRow)
            dev.steps[(size_t) i].on.param().setParameter (paintValue, juce::sendNotification);
        else
            setFromMouse (i, e.position.y);
        repaint();
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto i = stepAt (e.position.x);
        if (i < 0 || onRow) return;
        auto& p = dev.steps[(size_t) i].lane (lane).param();
        p.setParameter (p.getDefaultValue().value_or (p.getCurrentBaseValue()), juce::sendNotification);
        repaint();
    }

    static juce::Colour laneColour (int l)
    {
        static const int which[StepsDevice::numLanes] = { -1, 0, 2, 4, 5, 6, 3 };
        return which[l] < 0 ? Colours::solo : Colours::clips[(size_t) which[l]];
    }

private:
    StepsDevice& dev;
    static constexpr float onH = 20.0f;
    int lane = StepsDevice::pitchLane;
    bool onRow = false, showOnRow = true, allLanes = false;
    float paintValue = 1.0f;
    long long lastCount = -2;
    int lastCols = 0, sinceRepaint = 0;

    juce::Rectangle<float> cell (int i) const
    {
        auto w = (float) getWidth() / (float) columns();
        return { w * (float) i, 0.0f, w, (float) getHeight() };
    }

    juce::Rectangle<float> laneBox (int i) const     { return cell (i).withTrimmedTop (showOnRow ? onH + 4.0f : 0.0f).reduced (1.5f, 0.0f); }

    int stepAt (float x) const
    {
        auto i = (int) (x / ((float) getWidth() / (float) columns()));
        return i >= 0 && i < columns() ? i : -1;
    }

    /** The few characters shown in a cell: nothing where the value is the ordinary one. */
    static juce::String cellText (int lane, float v)
    {
        const int n = juce::roundToInt (v);

        switch (lane)
        {
            case StepsDevice::pitchLane:
            case StepsDevice::nudgeLane:    return n == 0 ? juce::String() : (n > 0 ? "+" : "") + juce::String (n);
            case StepsDevice::velocityLane: return juce::String (n);
            case StepsDevice::lengthLane:   return n == 100 ? juce::String() : juce::String (n);
            case StepsDevice::chanceLane:   return n >= 100 ? juce::String() : juce::String (n);
            case StepsDevice::repeatLane:   return n <= 1 ? juce::String() : juce::String (n) + "x";
            case StepsDevice::everyLane:
            {
                if (n <= 0) return {};
                const int of = n <= 2 ? 2 : n <= 5 ? 3 : 4;
                return juce::String (n - (of == 2 ? 0 : of == 3 ? 2 : 5)) + ":" + juce::String (of);
            }
            default: break;
        }
        return {};
    }

    void setFromMouse (int i, float y)
    {
        auto box = laneBox (i);
        auto& p = dev.steps[(size_t) i].lane (lane).param();
        const float n = juce::jlimit (0.0f, 1.0f, (box.getBottom() - y) / box.getHeight());
        p.setParameter (std::round (p.valueRange.convertFrom0to1 (n)), juce::sendNotification);
    }

    void timerCallback() override
    {
        // (also follows changes made from another view of the same device)
        const auto c = dev.getCurrentCount();
        const int cols = columns();
        if (c != lastCount || cols != lastCols || ++sinceRepaint >= 6) { lastCount = c; lastCols = cols; sinceRepaint = 0; repaint(); }
    }
};

/** Chooses which lane the grid shows. */
class LaneTabs : public juce::Component
{
public:
    std::function<void (int)> onChange;
    int current = 0;

    LaneTabs()      { setTitle ("Lane"); }

    void paint (juce::Graphics& g) override
    {
        const auto names = StepsDevice::laneNames();
        const float w = (float) getWidth() / (float) names.size();

        for (int i = 0; i < names.size(); ++i)
        {
            auto cell = juce::Rectangle<float> (w * (float) i, 0.0f, w, (float) getHeight()).reduced (1.0f);
            const bool on = i == current;
            g.setColour (on ? StepGrid::laneColour (i) : Colours::panelDark);
            g.fillRoundedRectangle (cell, 3.0f);
            g.setColour (on ? Colours::textInverse : Colours::textDim);
            g.setFont (juce::FontOptions (11.0f, on ? juce::Font::bold : juce::Font::plain));
            g.drawFittedText (names[i], cell.toNearestInt().reduced (2), juce::Justification::centred, 1);
        }
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        current = juce::jlimit (0, StepsDevice::numLanes - 1, e.x * StepsDevice::numLanes / juce::jmax (1, getWidth()));
        repaint();
        if (onChange) onChange (current);
    }
};

/** The sequencer's menus, shared by its panel in the chain and its window. */
namespace StepsMenus
{
    inline void set (te::AutomatableParameter& p, float v)
    {
        p.parameterChangeGestureBegin();
        p.setParameter (v, juce::sendNotification);
        p.parameterChangeGestureEnd();
    }

    inline void showScale (StepsDevice& dev, juce::Component& target)
    {
        juce::PopupMenu m;
        const int current = juce::roundToInt (dev.scale.get());
        for (int i = 0; i < (int) StepsDevice::scales().size(); ++i)
            m.addItem (i + 1, StepsDevice::scales()[(size_t) i].name, true, i == current);
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target), [ref = te::Plugin::Ptr (&dev)] (int result)
        {
            if (result > 0)
                set (static_cast<StepsDevice*> (ref.get())->scale.param(), (float) (result - 1));
        });
    }

    inline void showLaneSteps (StepsDevice& dev, int lane, juce::Component& target)
    {
        juce::PopupMenu m;
        const int current = juce::roundToInt (dev.laneSteps[(size_t) lane].get());
        m.addItem (1, "Same as the Pattern", true, current == 0);
        m.addSeparator();
        for (int i = 1; i <= StepsDevice::numSteps; ++i)
        {
            if (i == 17) m.addColumnBreak();
            m.addItem (i + 1, juce::String (i) + (i == 1 ? " step" : " steps"), true, current == i);
        }
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target), [ref = te::Plugin::Ptr (&dev), lane] (int result)
        {
            if (result > 0)
                set (static_cast<StepsDevice*> (ref.get())->laneSteps[(size_t) lane].param(), (float) (result - 1));
        });
    }

    /** Things to do to one lane (lane >= 0), to the whole pattern (wholePattern), or both. */
    inline void showTools (StepsDevice& dev, int lane, bool wholePattern, juce::Component& target)
    {
        juce::PopupMenu m;

        if (lane >= 0)
        {
            const auto name = StepsDevice::laneNames()[lane];
            m.addItem (1, "Randomise " + name);
            m.addItem (6, "Reset " + name);
        }

        if (wholePattern)
        {
            if (lane >= 0) m.addSeparator();
            m.addItem (2, "Randomise Which Steps Play");
            m.addSeparator();
            m.addItem (3, "Shift Pattern Left");
            m.addItem (4, "Shift Pattern Right");
            m.addItem (5, "Double Pattern", dev.canDouble());
            m.addSeparator();
            m.addItem (7, "Start Over");
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&target), [ref = te::Plugin::Ptr (&dev), lane] (int result)
        {
            auto& d = *static_cast<StepsDevice*> (ref.get());
            switch (result)
            {
                case 1: d.randomiseLane (lane); break;
                case 2: d.randomiseSteps(); break;
                case 3: d.shift (-1); break;
                case 4: d.shift (1); break;
                case 5: d.doubleLength(); break;
                case 6: d.resetLane (lane); break;
                case 7: d.loadDefaults(); break;
                default: break;
            }
        });
    }

    inline juce::String laneStepsText (StepsDevice& dev, int lane)
    {
        const int own = juce::roundToInt (dev.laneSteps[(size_t) lane].get());
        return own > 0 ? juce::String (own) + (own == 1 ? " step" : " steps") : juce::String ("Pattern");
    }
}

class StepsView : public juce::Component, private juce::Timer
{
public:
    StepsView (StepsDevice& d)
        : dev (d), grid (d),
          rateChoice (d.rateP.param(), MidiEffectDevice::rateNames(), Colours::solo),
          directionChoice (d.direction.param(), StepsDevice::directionNames()),
          length (d.length.param()), swing (d.swing.param()), gate (d.gate.param()), root (d.root.param()),
          follow (d.follow.param(), "Follow Keys")
    {
        for (juce::Component* c : { (juce::Component*) &grid, (juce::Component*) &tabs, (juce::Component*) &rateChoice, (juce::Component*) &directionChoice,
                                    (juce::Component*) &length, (juce::Component*) &swing, (juce::Component*) &gate, (juce::Component*) &root,
                                    (juce::Component*) &follow, (juce::Component*) &scaleButton, (juce::Component*) &laneStepsButton, (juce::Component*) &toolsButton })
            addAndMakeVisible (c);

        rateChoice.setTitle ("Rate");
        directionChoice.setTitle ("Direction");
        follow.setTitle ("Follow Keys");
        scaleButton.setTooltip ("Keeps every pitch to the notes of a scale, counted from the Root");
        laneStepsButton.setTooltip ("How many steps this lane goes round in. Give it a different number from the pattern and the two drift apart, then meet again.");
        toolsButton.setTooltip ("Randomise, shift, reset or double the pattern");

        tabs.onChange = [this] (int l) { grid.setLane (l); timerCallback(); };
        scaleButton.onClick = [this] { showScaleMenu(); };
        laneStepsButton.onClick = [this] { showLaneStepsMenu(); };
        toolsButton.onClick = [this] { showToolsMenu(); };
        timerCallback();
        startTimerHz (5);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto side = r.removeFromLeft (216);
        rateChoice.setBounds (side.removeFromTop (22));
        side.removeFromTop (3);
        directionChoice.setBounds (side.removeFromTop (20));
        side.removeFromTop (3);
        auto bottom = side.removeFromBottom (22);
        follow.setBounds (bottom.removeFromRight (84));
        bottom.removeFromRight (4);
        scaleButton.setBounds (bottom);
        side.removeFromBottom (3);
        const int w = side.getWidth() / 4;
        for (auto* k : { &length, &root, &swing, &gate })
            k->setBounds (side.removeFromLeft (w));

        r.removeFromLeft (8);
        auto top = r.removeFromTop (20);
        toolsButton.setBounds (top.removeFromRight (58));
        top.removeFromRight (4);
        laneStepsButton.setBounds (top.removeFromRight (138));
        top.removeFromRight (6);
        tabs.setBounds (top);
        r.removeFromTop (4);
        grid.setBounds (r);
    }

private:
    StepsDevice& dev;
    StepGrid grid;
    LaneTabs tabs;
    DeviceUI::Choice rateChoice, directionChoice;
    DeviceUI::Knob length, swing, gate, root;
    DeviceUI::Toggle follow;
    juce::TextButton scaleButton, laneStepsButton, toolsButton { "Tools" };

    void showScaleMenu()        { StepsMenus::showScale (dev, scaleButton); }
    void showLaneStepsMenu()    { StepsMenus::showLaneSteps (dev, grid.getLane(), laneStepsButton); }
    void showToolsMenu()        { StepsMenus::showTools (dev, grid.getLane(), true, toolsButton); }

    void timerCallback() override
    {
        scaleButton.setButtonText ("Scale: " + dev.scale.param().getCurrentValueAsString());
        const int lane = grid.getLane();
        const int own = juce::roundToInt (dev.laneSteps[(size_t) lane].get());
        laneStepsButton.setButtonText (StepsDevice::laneNames()[lane] + (own > 0 ? " steps: " + juce::String (own) : juce::String (" steps: Pattern")));
        tabs.current = lane;
    }
};

//==============================================================================
/** The sequencer in its own window: every lane at once, one under another, on one grid. */
class StepsWindowView : public juce::Component, private juce::Timer
{
public:
    StepsWindowView (StepsDevice& d)
        : dev (d),
          rateChoice (d.rateP.param(), MidiEffectDevice::rateNames(), Colours::solo),
          directionChoice (d.direction.param(), StepsDevice::directionNames()),
          length (d.length.param()), swing (d.swing.param()), gate (d.gate.param()), root (d.root.param()),
          follow (d.follow.param(), "Follow Keys")
    {
        for (juce::Component* c : { (juce::Component*) &rateChoice, (juce::Component*) &directionChoice, (juce::Component*) &length,
                                    (juce::Component*) &swing, (juce::Component*) &gate, (juce::Component*) &root, (juce::Component*) &follow,
                                    (juce::Component*) &scaleButton, (juce::Component*) &patternButton })
            addAndMakeVisible (c);

        rateChoice.setTitle ("Rate");
        directionChoice.setTitle ("Direction");
        follow.setTitle ("Follow Keys");
        scaleButton.setTooltip ("Keeps every pitch to the notes of a scale, counted from the Root");
        patternButton.setTooltip ("Randomise which steps play, shift or double the pattern, or start over");
        scaleButton.onClick = [this] { StepsMenus::showScale (dev, scaleButton); };
        patternButton.onClick = [this] { StepsMenus::showTools (dev, -1, true, patternButton); };

        for (int l = 0; l < StepsDevice::numLanes; ++l)
        {
            auto* row = rows.add (new Row (d));
            row->grid.setLane (l);
            row->grid.setShowsOnRow (l == 0);
            row->grid.setLinedUpWithAllLanes (true);
            row->grid.setTitle (StepsDevice::laneNames()[l]);
            row->steps.setTitle (StepsDevice::laneNames()[l] + " steps");
            row->steps.setTooltip ("How many steps this lane goes round in. Give it a different number from the pattern and the two drift apart, then meet again.");
            row->steps.onClick = [this, l] { StepsMenus::showLaneSteps (dev, l, rows[l]->steps); };
            row->tools.setTitle (StepsDevice::laneNames()[l] + " tools");
            row->tools.setTooltip ("Randomise or reset this lane");
            row->tools.onClick = [this, l] { StepsMenus::showTools (dev, l, false, rows[l]->tools); };
            addAndMakeVisible (row->grid);
            addAndMakeVisible (row->steps);
            addAndMakeVisible (row->tools);
        }

        timerCallback();
        startTimerHz (5);
    }

    void paint (juce::Graphics& g) override
    {
        for (int l = 0; l < rows.size(); ++l)
        {
            auto label = rows[l]->label;
            g.setColour (StepGrid::laneColour (l));
            g.fillRoundedRectangle (label.removeFromLeft (4).toFloat(), 2.0f);
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
            g.drawText (StepsDevice::laneNames()[l], label.withTrimmedLeft (8).removeFromTop (20), juce::Justification::centredLeft);
        }
    }

    void resized() override
    {
        auto r = getLocalBounds();

        // Across the top: how it plays
        auto top = r.removeFromTop (86);
        auto choices = top.removeFromLeft (300);
        rateChoice.setBounds (choices.removeFromTop (24));
        choices.removeFromTop (6);
        directionChoice.setBounds (choices.removeFromTop (24));
        top.removeFromLeft (16);
        for (auto* k : { &length, &root, &swing, &gate })
            k->setBounds (top.removeFromLeft (76));
        top.removeFromLeft (16);
        auto buttons = top.removeFromLeft (190);
        scaleButton.setBounds (buttons.removeFromTop (24));
        buttons.removeFromTop (6);
        follow.setBounds (buttons.removeFromTop (24).removeFromLeft (92));
        patternButton.setBounds (follow.getBounds().translated (98, 0));
        r.removeFromTop (8);

        // The lanes: Pitch gets the most room
        const int gap = 6;
        const int shares = 3 + (rows.size() - 1);
        const int unit = (r.getHeight() - 24 - gap * (rows.size() - 1)) / shares;

        for (int l = 0; l < rows.size(); ++l)
        {
            auto area = r.removeFromTop (l == 0 ? unit * 3 + 24 : unit);
            r.removeFromTop (gap);
            auto label = area.removeFromLeft (124);
            area.removeFromLeft (8);
            rows[l]->grid.setBounds (area);

            if (l == 0) label.removeFromTop (24);   // (level with the bars, under the on/off row)
            rows[l]->label = label;
            auto controls = label.withTrimmedLeft (8).withTrimmedTop (22).removeFromTop (20);
            rows[l]->tools.setBounds (controls.removeFromRight (26));
            controls.removeFromRight (4);
            rows[l]->steps.setBounds (controls);
        }
    }

private:
    struct Row
    {
        Row (StepsDevice& d) : grid (d) {}
        StepGrid grid;
        juce::TextButton steps, tools { juce::String::fromUTF8 ("\xe2\x8b\xaf") };
        juce::Rectangle<int> label;
    };

    StepsDevice& dev;
    DeviceUI::Choice rateChoice, directionChoice;
    DeviceUI::Knob length, swing, gate, root;
    DeviceUI::Toggle follow;
    juce::TextButton scaleButton, patternButton { "Pattern" };
    juce::OwnedArray<Row> rows;

    void timerCallback() override
    {
        scaleButton.setButtonText ("Scale: " + dev.scale.param().getCurrentValueAsString());
        for (int l = 0; l < rows.size(); ++l)
            rows[l]->steps.setButtonText (StepsMenus::laneStepsText (dev, l));
    }
};

inline std::unique_ptr<juce::Component> ArpDevice::createDeviceView()      { return std::make_unique<ArpView> (*this); }
inline std::unique_ptr<juce::Component> StepsDevice::createDeviceView()    { return std::make_unique<StepsView> (*this); }
inline std::unique_ptr<juce::Component> StepsDevice::createWindowView()    { return std::make_unique<StepsWindowView> (*this); }

} // namespace elsewhere
