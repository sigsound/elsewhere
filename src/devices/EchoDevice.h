// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"
#include "Dsp.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Tape Echo: a loop of tape passing one record head and three playback heads.

    The sound is written to the tape as it passes the record head, and each playback head hears
    it once that piece of tape has travelled to it. The echo time is therefore set by the tape's
    speed, not by a clock: turning Repeat Rate makes the motor speed up or slow down, and every
    echo already on the tape bends in pitch while it does. The second and third heads sit two
    and three times as far from the record head as the first.

    What the heads play goes back to the record head (Intensity), so each repeat has been through
    the tape once more than the one before: a little duller, a little more squashed, and, on a
    worn machine, a little different every time:

      - Wow and Flutter: slow and fast wavering of the tape speed
      - Wear: patches of tape that play quietly and dull, heads that lose contact for a moment,
        the motor stumbling, and the join in the loop coming round. All of it by chance, so no
        two repeats decay the same way
      - Noise: tape hiss, recorded with everything else and repeated with it

    Turned up far enough, Intensity makes the machine feed on itself; the tape can only hold so
    much, which keeps it from running away.
*/
class EchoDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereEcho";
    static constexpr int numHeads = 3;

    /** Tape in the loop, measured in gaps between the record head and the first playback head. */
    static constexpr double loopLength = 40.0;

    static juce::StringArray syncNames()    { return { "Free", "1/16", "1/8T", "1/8", "1/8D", "1/4" }; }
    static double syncBeats (int index)
    {
        static const double b[] = { 0.0, 0.25, 1.0 / 3.0, 0.5, 0.75, 1.0 };
        return b[juce::jlimit (0, 5, index)];
    }

    EchoDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        auto shelfText = [] (float v) { return (v > 0.05f ? "+" : "") + juce::String (v, 1) + " dB"; };

        time      = addParameter ("time", "Repeat Rate", 180.0f, skewed (40.0f, 600.0f, 200.0f), msText);
        sync      = addParameter ("sync", "Sync", 0.0f, stepped (0, 5),
                                  [] (float v) { return syncNames()[juce::jlimit (0, 5, juce::roundToInt (v))]; });
        intensity = addParameter ("intensity", "Intensity", 40.0f, { 0.0f, 100.0f }, percentText);
        echo      = addParameter ("echo", "Echo", 60.0f, { 0.0f, 100.0f }, percentText);
        dry       = addParameter ("dry", "Dry", 1.0f, stepped (0, 1), onOffText);
        drive     = addParameter ("drive", "Drive", 15.0f, { 0.0f, 100.0f }, percentText);
        bass      = addParameter ("bass", "Bass", 0.0f, { -12.0f, 12.0f }, shelfText);
        treble    = addParameter ("treble", "Treble", 0.0f, { -12.0f, 12.0f }, shelfText);
        wow       = addParameter ("wow", "Wow", 25.0f, { 0.0f, 100.0f }, percentText);
        flutter   = addParameter ("flutter", "Flutter", 20.0f, { 0.0f, 100.0f }, percentText);
        wear      = addParameter ("wear", "Wear", 30.0f, { 0.0f, 100.0f }, percentText);
        noise     = addParameter ("noise", "Noise", 0.0f, { 0.0f, 100.0f }, percentText);
        spread    = addParameter ("spread", "Spread", 50.0f, { 0.0f, 100.0f }, percentText);

        for (int i = 0; i < numHeads; ++i)
            heads[(size_t) i] = addParameter ("head" + juce::String (i + 1), "Head " + juce::String (i + 1),
                                              i == 0 ? 1.0f : 0.0f, stepped (0, 1), onOffText);
    }

    juce::String getName() const override              { return "Elsewhere Tape Echo"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Tape Echo"; }
    int getDeviceViewWidth() const override            { return 880; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    bool noTail() override                              { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }

    double getTailLength() const override
    {
        // How long the repeats take to fall 60 dB; near and past self-oscillation, just "long"
        const double loop = loopGain (intensity.get());
        const double longest = 3.0 * (juce::roundToInt (sync.get()) > 0 ? 1.0 : time.get() * 0.001);
        if (loop >= 0.9)
            return 30.0;
        return juce::jlimit (1.0, 30.0, longest * (1.0 + 6.9 / -std::log (juce::jmax (0.01, loop))));
    }

    //==============================================================================
    // Patches
    struct Patch { const char* name; std::vector<std::pair<const char*, float>> values; };

    static const std::vector<Patch>& patches()
    {
        static const std::vector<Patch> all = {
            { "Fresh Tape",   { } },
            { "Slapback",     { { "time", 95 }, { "intensity", 12 }, { "echo", 70 }, { "wow", 10 }, { "flutter", 15 }, { "wear", 10 } } },
            { "Three Heads",  { { "time", 150 }, { "head2", 1 }, { "head3", 1 }, { "intensity", 35 }, { "spread", 80 } } },
            { "Worn Loop",    { { "time", 240 }, { "head1", 0 }, { "head2", 1 }, { "intensity", 55 }, { "wow", 55 }, { "flutter", 45 },
                                { "wear", 85 }, { "noise", 30 }, { "treble", -3 } } },
            { "Slow Motor",   { { "time", 330 }, { "intensity", 50 }, { "wow", 90 }, { "flutter", 10 }, { "wear", 50 }, { "bass", 2 } } },
            { "In Time",      { { "sync", 3 }, { "head2", 1 }, { "intensity", 45 }, { "wow", 15 }, { "wear", 20 }, { "spread", 70 } } },
            { "Dub Chamber",  { { "sync", 4 }, { "intensity", 68 }, { "drive", 45 }, { "bass", -4 }, { "treble", -5 }, { "wear", 40 } } },
            { "Runaway",      { { "time", 210 }, { "head3", 1 }, { "intensity", 88 }, { "drive", 35 }, { "wear", 45 }, { "noise", 15 },
                                { "echo", 45 } } },
        };
        return all;
    }

    float patchValue (const Patch& patch, te::AutomatableParameter& ap) const
    {
        for (auto& [id, value] : patch.values)
            if (ap.paramID == id)
                return value;
        return ap.getDefaultValue().value_or (ap.getCurrentBaseValue());
    }

    void loadPatch (int index)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);
        const auto& patch = patches()[(size_t) index];
        edit.getUndoManager().beginNewTransaction ("Load Tape Echo patch");
        for (auto& p : params)
            p->getParameter().setParameter (patchValue (patch, p->getParameter()), juce::sendNotification);
        state.setProperty ("patch", index, &edit.getUndoManager());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", 0); }

    juce::String getPatchLabel() const
    {
        const int index = getPatchIndex();
        if (index < 0 || index >= (int) patches().size())
            return "Patches";

        const auto& patch = patches()[(size_t) index];
        for (auto& p : params)
            if (std::abs (p->getParameter().getCurrentBaseValue() - patchValue (patch, p->getParameter())) > 1.0e-3f)
                return juce::String (patch.name) + " *";
        return patch.name;
    }

    //==============================================================================
    void initialise (const te::PluginInitialisationInfo& info) override
    {
        // (The engine can do this while audio is still running through the device)
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        rate = info.sampleRate;
        tempoPosition = std::make_unique<te::tempo::Sequence::Position> (te::createPosition (edit.tempoSequence));

        // Room for the third head at the slowest speed, with the motor stumbling
        size = juce::nextPowerOfTwo ((int) (rate * 4.0));
        mask = (juce::int64) size - 1;
        tape.assign ((size_t) size, 0.0f);
        travel.assign ((size_t) size, 0.0);
        renderedUpTo = -1.0;
        startAfresh();
    }

    /** A blank tape, and the machine as it is every time it is switched on. */
    void startAfresh()
    {
        std::fill (tape.begin(), tape.end(), 0.0f);

        for (int h = 0; h < numHeads; ++h)
        {
            head[(size_t) h] = {};
            head[(size_t) h].gain = heads[(size_t) h].get() > 0.5f ? 1.0f : 0.0f;
        }

        runAt (1.0 / (juce::jlimit (0.03, 1.0, time.get() * 0.001) * rate));
        upToSpeed = false;

        rng.setSeed (0x7a9e5eed);   // (the same wear every time, so a mixdown can be repeated)
        bump.set (Biquad::Type::bell, 95.0, 0.9, 1.5, rate);
        bump.reset();
        bassShelf.reset(); trebleShelf.reset();
        shelfBass = shelfTreble = 1000.0f;   // (forces the shelves to be set on the first block)
        feedback = 0.0f;
        wowPhase = wowPhase2 = flutterPhase = flutterPhase2 = 0.0;
        wowDrift = wowDriftTarget = flutterNoise = 0.0f;
        wowHold = tick = 0;
        patch = {}; slip = {}; splice = {};
        dullState = hissState = 0.0f;
        lastLoop = 0;
        echoGain.setTimeMs (20.0f, rate);       echoGain.reset (echo.get() / 100.0f);
        intensityGain.setTimeMs (20.0f, rate);  intensityGain.reset ((float) loopGain (intensity.get()));
        dryGain.setTimeMs (10.0f, rate);        dryGain.reset (dry.get() > 0.5f ? 1.0f : 0.0f);
    }

    /** Sets the tape going at a speed (head gaps per sample) as if it always had been. */
    void runAt (double newSpeed)
    {
        speed = newSpeed;
        uiGap = (float) (1.0 / (speed * rate));
        count = size;
        position = 0.0;
        for (juce::int64 j = 1; j <= size; ++j)
            travel[(size_t) (j & mask)] = -(double) (size - j) * speed;

        for (int h = 0; h < numHeads; ++h)
            head[(size_t) h].index = count - (juce::int64) std::ceil ((double) (h + 1) / speed) - 1;
    }

    void reset() override
    {
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        if (! tape.empty())
            startAfresh();
    }

    void deinitialise() override
    {
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        tempoPosition.reset();
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked())
            return;     // being set up again for a new rate or buffer size

        if (fc.destBuffer == nullptr || tape.empty())
            return;

        juce::ScopedNoDenormals noDenormals;
        auto& buf = *fc.destBuffer;
        const int numCh = juce::jmin (2, buf.getNumChannels());
        if (numCh == 0) return;

        // A mixdown starts with a blank tape and the machine as it always starts, so mixing
        // the same song down twice gives the same result
        if (fc.isRendering && std::abs (fc.editTime.getStart().inSeconds() - renderedUpTo) > 1.0e-6)
            startAfresh();
        renderedUpTo = fc.isRendering ? fc.editTime.getEnd().inSeconds() : -1.0;

        float* left = buf.getWritePointer (0, fc.bufferStartSample);
        float* right = numCh > 1 ? buf.getWritePointer (1, fc.bufferStartSample) : nullptr;
        const int num = fc.bufferNumSamples;
        const float fs = (float) rate;

        // How long the tape takes to get from the record head to the first playback head
        double gapSeconds = time.get() * 0.001;
        if (const int s = juce::roundToInt (sync.get()); s > 0 && tempoPosition != nullptr)
        {
            tempoPosition->set (fc.editTime.getStart());
            gapSeconds = syncBeats (s) * 60.0 / juce::jmax (20.0, tempoPosition->getTempo());
        }
        gapSeconds = juce::jlimit (0.03, 1.0, gapSeconds);
        const double speedTarget = 1.0 / (gapSeconds * rate);

        // (A blank tape starts at the right speed, synced or not, rather than gliding to it)
        if (! upToSpeed)
        {
            upToSpeed = true;
            if (speedTarget != speed)
                runAt (speedTarget);
        }
        const double inertia = 1.0 - std::exp (-1.0 / (0.12 * rate));   // the motor takes a moment to change speed
        const double slowest = (double) numHeads / (double) (size - 64);

        const float wearAmt = wear.get() / 100.0f;
        const float wowAmt = wow.get() / 100.0f * 0.012f;
        const float flutterAmt = flutter.get() / 100.0f * 0.0035f;
        const float hissAmt = juce::square (noise.get() / 100.0f) * 0.004f;
        const float driveGain = juce::Decibels::decibelsToGain (drive.get() * 0.24f);
        const float driveComp = std::pow (driveGain, -0.75f);
        const float lean = 0.1f * wearAmt;   // worn tape squashes one side of the wave more than the other

        echoGain.target = echo.get() / 100.0f;
        intensityGain.target = (float) loopGain (intensity.get());
        dryGain.target = dry.get() > 0.5f ? 1.0f : 0.0f;

        // Slower tape and older tape both lose treble
        const float brightness = 5200.0f * std::pow ((float) (0.16 / (1.0 / (speed * rate))), 0.4f) * (1.0f - 0.45f * wearAmt);
        const float lpA = std::exp (-juce::MathConstants<float>::twoPi * juce::jlimit (1200.0f, 9000.0f, brightness) / fs);
        const float hpA = std::exp (-juce::MathConstants<float>::twoPi * 70.0f / fs);
        const float dullA = std::exp (-juce::MathConstants<float>::twoPi * 900.0f / fs);
        const float gainA = std::exp (-1.0f / (0.01f * fs));
        const float contactA = std::exp (-1.0f / (0.08f * fs));

        if (bass.get() != shelfBass || treble.get() != shelfTreble)
        {
            shelfBass = bass.get(); shelfTreble = treble.get();
            bassShelf.set (Biquad::Type::lowShelf, 180.0, 0.7, shelfBass, rate);
            trebleShelf.set (Biquad::Type::highShelf, 3000.0, 0.7, shelfTreble, rate);
        }

        // Where each head sits across the stereo picture: 1 in the middle, 2 left, 3 right
        const float sp = spread.get() / 100.0f;
        const float pan[numHeads] = { 0.0f, -sp, sp };
        float panL[numHeads], panR[numHeads], headOn[numHeads];
        for (int h = 0; h < numHeads; ++h)
        {
            const float a = (pan[h] + 1.0f) * juce::MathConstants<float>::pi * 0.25f;
            panL[h] = std::cos (a) * 1.41421f;
            panR[h] = std::sin (a) * 1.41421f;
            headOn[h] = heads[(size_t) h].get() > 0.5f ? 1.0f : 0.0f;
        }

        const double twoPi = juce::MathConstants<double>::twoPi;
        float peak[numHeads] = {}, deepest = 0.0f;

        for (int i = 0; i < num; ++i)
        {
            // Things that happen now and then, checked a few hundred times a second
            if (--tick <= 0)
            {
                tick = controlInterval;
                everySoOften (wearAmt, (float) controlInterval / fs);
            }

            // Tape speed: the motor, plus everything that makes it waver
            wowPhase += twoPi * 0.55 * (1.0 + 0.5 * wowDrift) / rate;
            wowPhase2 += twoPi * 1.49 / rate;
            flutterPhase += twoPi * 8.3 * (1.0 + 0.1 * wowDrift) / rate;
            flutterPhase2 += twoPi * 13.7 / rate;
            if (wowPhase > twoPi) wowPhase -= twoPi;
            if (wowPhase2 > twoPi) wowPhase2 -= twoPi;
            if (flutterPhase > twoPi) flutterPhase -= twoPi;
            if (flutterPhase2 > twoPi) flutterPhase2 -= twoPi;

            flutterNoise += 0.004f * ((rng.nextFloat() * 2.0f - 1.0f) - flutterNoise);
            const float waver = wowAmt * (float) (std::sin (wowPhase) + 0.3 * std::sin (wowPhase2)) * 0.77f
                              + flutterAmt * ((float) (std::sin (flutterPhase) + 0.5 * std::sin (flutterPhase2)) * 0.5f + flutterNoise * 6.0f);

            speed += (speedTarget - speed) * inertia;
            const double moved = juce::jmax (slowest, speed * (1.0 + (double) waver - (double) slip.next() - (double) splice.next() * 0.02));

            // Record: what comes in, plus what the heads are playing, plus hiss
            const float in = right != nullptr ? 0.5f * (left[i] + right[i]) : left[i];
            float hiss = 0.0f;
            if (hissAmt > 0.0f)
            {
                const float white = rng.nextFloat() * 2.0f - 1.0f;
                hiss = (white - 0.6f * hissState) * hissAmt;
                hissState = white;
            }

            float rec = in * driveGain + feedback * intensityGain.next() + hiss;
            rec = bump.process (rec, 0);
            rec = ceiling * std::tanh ((rec + lean * rec * rec) / ceiling);

            // A weak patch of tape takes the signal down quietly and dully
            dullState = rec + dullA * (dullState - rec);
            const float weak = juce::jmin (1.0f, patch.next() + splice.value * 0.8f * wearAmt);
            rec = (rec + (dullState - rec) * weak * 0.6f) * (1.0f - weak);
            deepest = juce::jmax (deepest, weak);

            ++count;
            position += moved;
            travel[(size_t) (count & mask)] = position;
            tape[(size_t) (count & mask)] = rec;

            // Playback: each head hears the piece of tape that is passing it now
            float wetL = 0.0f, wetR = 0.0f, sum = 0.0f, active = 0.0f;

            for (int h = 0; h < numHeads; ++h)
            {
                auto& hd = head[(size_t) h];
                hd.gain = headOn[h] + gainA * (hd.gain - headOn[h]);
                hd.contact = hd.contactTarget + contactA * (hd.contact - hd.contactTarget);

                float y = readHead (hd, position - (double) (h + 1));
                hd.lp1 = y + lpA * (hd.lp1 - y);
                hd.lp2 = hd.lp1 + lpA * (hd.lp2 - hd.lp1);
                hd.hp = hd.lp2 + hpA * (hd.hp - hd.lp2);
                y = (hd.lp2 - hd.hp) * hd.contact * hd.gain;

                sum += y;
                active += hd.gain;
                wetL += y * panL[h];
                wetR += y * panR[h];
                peak[h] = juce::jmax (peak[h], std::abs (y));
            }

            // More heads mean more going back to the tape, as on the real thing, but not three
            // times as much: the same Intensity should still die away
            const float share = juce::jmax (1.0f, active);
            feedback = sum / std::pow (share, 0.75f);

            const float level = echoGain.next() * driveComp / std::sqrt (share);
            wetL = trebleShelf.process (bassShelf.process (wetL * level, 0), 0);
            wetR = trebleShelf.process (bassShelf.process (wetR * level, 1), 1);
            const float d = dryGain.next();

            if (right != nullptr)
            {
                left[i] = left[i] * d + wetL;
                right[i] = right[i] * d + wetR;
            }
            else
            {
                left[i] = left[i] * d + 0.5f * (wetL + wetR);
            }
        }

        // The splice comes round once per trip of the loop
        if (const auto loops = (juce::int64) std::floor (position / loopLength); loops != lastLoop)
        {
            lastLoop = loops;
            splice.start (0.012f * fs, 1.0f);
        }

        for (int h = 0; h < numHeads; ++h)
            uiHeadLevel[(size_t) h] = juce::jmax (peak[h], uiHeadLevel[(size_t) h].load() * 0.7f);
        uiWeak = juce::jmax (deepest, uiWeak.load() * 0.8f);
        uiTape = position;
        uiGap = (float) (1.0 / (speed * rate));
    }

    //==============================================================================
    // For the picture of the machine
    double getTapePosition() const          { return uiTape.load(); }
    float getHeadLevel (int h) const        { return uiHeadLevel[(size_t) h].load(); }
    float getWeakPatch() const              { return uiWeak.load(); }

    /** The time the tape takes from the record head to Head 1, at the speed it is going now. */
    float getGapSeconds() const             { return uiGap.load(); }

    Param time, sync, intensity, echo, dry, drive, bass, treble, wow, flutter, wear, noise, spread;
    std::array<Param, numHeads> heads;

private:
    static constexpr int controlInterval = 32;

    /** The most the tape can hold (full scale is 1): what a runaway echo levels off at. */
    static constexpr float ceiling = 0.7f;

    /** Feedback gain for an Intensity setting: the machine starts feeding on itself at about 80 %. */
    static double loopGain (float intensityPercent)     { return intensityPercent / 100.0 * 1.25; }

    /** A passing event: rises and falls once over its length. */
    struct Swell
    {
        float phase = 1.0f, step = 0.0f, depth = 0.0f, value = 0.0f;

        bool busy() const                               { return phase < 1.0f; }
        void start (float lengthSamples, float d)       { phase = 0.0f; step = 1.0f / juce::jmax (1.0f, lengthSamples); depth = d; }

        float next() noexcept
        {
            if (phase >= 1.0f)
                return value = 0.0f;

            const float s = std::sin (juce::MathConstants<float>::pi * phase);
            phase += step;
            return value = depth * s * s;
        }
    };

    struct Head
    {
        juce::int64 index = 0;      // the sample at or just before the piece of tape under the head
        float lp1 = 0.0f, lp2 = 0.0f, hp = 0.0f;
        float gain = 0.0f;          // on/off, without a click
        float contact = 1.0f, contactTarget = 1.0f;     // how well the tape is touching the head
        int contactHold = 0;
    };

    double rate = 44100.0, renderedUpTo = -1.0;
    std::unique_ptr<te::tempo::Sequence::Position> tempoPosition;

    // The tape: what was recorded each sample, and how far the tape had travelled by then
    // (in head gaps). A head plays the sample recorded when the tape was one, two or three
    // gaps back from where it is now.
    std::vector<float> tape;
    std::vector<double> travel;
    int size = 0;
    juce::int64 mask = 0, count = 0, lastLoop = 0;
    double position = 0.0, speed = 0.0;
    bool upToSpeed = false;

    std::array<Head, numHeads> head;
    Biquad bump, bassShelf, trebleShelf;
    Smoothed echoGain, intensityGain, dryGain;
    float shelfBass = 1000.0f, shelfTreble = 1000.0f;
    float feedback = 0.0f, dullState = 0.0f, hissState = 0.0f;

    juce::Random rng;
    double wowPhase = 0.0, wowPhase2 = 0.0, flutterPhase = 0.0, flutterPhase2 = 0.0;
    float wowDrift = 0.0f, wowDriftTarget = 0.0f, flutterNoise = 0.0f;
    int wowHold = 0, tick = 0;
    Swell patch, slip, splice;

    std::atomic<double> uiTape { 0.0 };
    std::array<std::atomic<float>, numHeads> uiHeadLevel {};
    std::atomic<float> uiWeak { 0.0f }, uiGap { 0.18f };

    /** The chance events of an old machine. `dt` is the time since the last call, in seconds. */
    void everySoOften (float wearAmt, float dt)
    {
        const float fs = (float) rate;

        // The wow never quite keeps the same pace
        if (--wowHold <= 0)
        {
            wowHold = (int) ((0.3f + 0.6f * rng.nextFloat()) / dt);
            wowDriftTarget = rng.nextFloat() * 2.0f - 1.0f;
        }
        wowDrift += (wowDriftTarget - wowDrift) * 0.02f;

        // Each head's contact with the tape comes and goes, so repeats don't die away evenly
        for (auto& hd : head)
        {
            if (--hd.contactHold <= 0)
            {
                hd.contactHold = (int) ((0.15f + 0.7f * rng.nextFloat()) / dt);
                hd.contactTarget = 1.0f - wearAmt * 0.4f * juce::square (rng.nextFloat());
            }
        }

        if (wearAmt <= 0.001f)
            return;

        // A weak patch of tape: usually a blink, now and then a long fade
        if (! patch.busy() && rng.nextFloat() < juce::square (wearAmt) * 1.6f * dt)
        {
            if (rng.nextFloat() < 0.15f)
                patch.start ((0.4f + 0.8f * rng.nextFloat()) * fs, wearAmt * (0.2f + 0.4f * rng.nextFloat()));
            else
                patch.start ((0.02f + 0.2f * juce::square (rng.nextFloat())) * fs, wearAmt * (0.35f + 0.65f * rng.nextFloat()));
        }

        // The motor stumbles and the pitch dips with it
        if (! slip.busy() && rng.nextFloat() < juce::square (wearAmt) * 0.3f * dt)
            slip.start ((0.08f + 0.27f * rng.nextFloat()) * fs, wearAmt * (0.02f + 0.06f * rng.nextFloat()));
    }

    /** What the tape holds at `target` (a distance along the tape, in head gaps). */
    float readHead (Head& hd, double target) noexcept
    {
        // Follow the tape forwards to the two samples either side of that spot
        hd.index = juce::jmax (hd.index, count - (juce::int64) size + 4);

        for (int steps = 0; steps < 64 && hd.index + 1 < count && travel[(size_t) ((hd.index + 1) & mask)] <= target; ++steps)
            ++hd.index;

        const double x0 = travel[(size_t) (hd.index & mask)], x1 = travel[(size_t) ((hd.index + 1) & mask)];
        const float t = x1 > x0 ? (float) juce::jlimit (0.0, 1.0, (target - x0) / (x1 - x0)) : 0.0f;

        const float a = tape[(size_t) ((hd.index - 1) & mask)];
        const float b = tape[(size_t) (hd.index & mask)];
        const float c = tape[(size_t) ((hd.index + 1) & mask)];
        const float d = tape[(size_t) (juce::jmin (hd.index + 2, count) & mask)];

        // Cubic (Hermite) between the four samples around it
        const float c1 = 0.5f * (c - a);
        const float c2 = a - 2.5f * b + 2.0f * c - 0.5f * d;
        const float c3 = 0.5f * (d - a) + 1.5f * (b - c);
        return ((c3 * t + c2) * t + c1) * t + b;
    }
};

//==============================================================================
/** The machine: the tape loop going round, the heads lighting as they play. */
class EchoScreen : public juce::Component, private juce::Timer
{
public:
    EchoScreen (EchoDevice& d) : dev (d)    { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 4.0f);

        // The loop: along the heads at the bottom, round and back over the top
        auto loop = r.reduced (16.0f, 20.0f).withTrimmedTop (4.0f);
        juce::Path path;
        path.addRoundedRectangle (loop, loop.getHeight() * 0.5f);
        const float weak = dev.getWeakPatch();
        const auto tapeColour = Colours::clips[6].interpolatedWith (Colours::textDim, 0.3f);
        g.setColour (tapeColour.withAlpha (0.55f));
        g.strokePath (path, juce::PathStrokeType (5.0f));

        // Marks on the tape, so you can see it move (and waver)
        const float length = path.getLength();
        const double pos = dev.getTapePosition();
        const float gap = loop.getWidth() * 0.19f;
        const int marks = juce::jmax (8, (int) (length / (gap * 0.5f)));
        const float phase = (float) (pos * 2.0 - std::floor (pos * 2.0));
        g.setColour (Colours::display.withAlpha (0.8f));
        for (int i = 0; i < marks; ++i)
        {
            auto p = path.getPointAlongPath (length * ((float) i + phase) / (float) marks);
            g.fillEllipse (p.x - 1.0f, p.y - 1.0f, 2.0f, 2.0f);
        }

        // Heads along the bottom run: record, then the three that play
        const float y = loop.getBottom();
        const float x0 = loop.getCentreX() + gap * 1.5f;
        static const char* names[] = { "REC", "1", "2", "3" };
        juce::StringArray times;

        for (int i = 0; i < 4; ++i)
        {
            auto box = juce::Rectangle<float> (14.0f, 14.0f).withCentre ({ x0 - gap * (float) i, y });
            const bool on = i == 0 || dev.heads[(size_t) (i - 1)].get() > 0.5f;
            const float level = i == 0 ? 0.0f : juce::jlimit (0.0f, 1.0f, dev.getHeadLevel (i - 1) * 3.0f);
            auto colour = i == 0 ? Colours::meterRed : Colours::accent;
            g.setColour (Colours::display);
            g.fillRoundedRectangle (box.expanded (2.0f), 3.0f);
            g.setColour (on ? colour.withAlpha (i == 0 ? 0.9f - 0.6f * weak : 0.35f + 0.65f * level) : Colours::panelLight);
            g.fillRoundedRectangle (box, 2.0f);
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
            g.drawText (names[i], box.translated (0.0f, 15.0f).expanded (8.0f, 0.0f), juce::Justification::centred);

            if (i > 0 && on)
                times.add (juce::String (juce::roundToInt (dev.getGapSeconds() * 1000.0f * (float) i)));
        }

        // When the echoes come, for the heads that are on
        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (15.0f, juce::Font::bold));
        g.drawText (times.isEmpty() ? juce::String ("No heads on") : times.joinIntoString (juce::String::fromUTF8 (" \xc2\xb7 ")) + " ms",
                    loop.toNearestInt().reduced (10, 0), juce::Justification::centred);
    }

private:
    EchoDevice& dev;
    void timerCallback() override   { repaint(); }
};

class EchoView : public juce::Component, private juce::Timer
{
public:
    EchoView (EchoDevice& d)
        : dev (d), screen (d),
          head1 (d.heads[0].param(), "Head 1"), head2 (d.heads[1].param(), "Head 2"), head3 (d.heads[2].param(), "Head 3"),
          syncChoice (d.sync.param(), EchoDevice::syncNames(), Colours::solo),
          dryToggle (d.dry.param(), "Dry"),
          time (d.time.param()), intensity (d.intensity.param()), echo (d.echo.param()),
          drive (d.drive.param(), {}, Colours::clips[4]), bass (d.bass.param(), {}, Colours::clips[4]), treble (d.treble.param(), {}, Colours::clips[4]),
          wow (d.wow.param(), {}, Colours::clips[6]), flutter (d.flutter.param(), {}, Colours::clips[6]),
          wear (d.wear.param(), {}, Colours::clips[6]), noise (d.noise.param(), {}, Colours::clips[6]),
          spread (d.spread.param(), {}, Colours::clips[3])
    {
        addAndMakeVisible (screen);
        for (juce::Component* c : { (juce::Component*) &head1, (juce::Component*) &head2, (juce::Component*) &head3,
                                    (juce::Component*) &syncChoice, (juce::Component*) &dryToggle, (juce::Component*) &patchButton })
            addAndMakeVisible (c);
        for (auto* k : row1()) addAndMakeVisible (k);
        for (auto* k : row2()) addAndMakeVisible (k);

        head1.setTitle ("Head 1"); head2.setTitle ("Head 2"); head3.setTitle ("Head 3");
        syncChoice.setTitle ("Sync");
        dryToggle.setTitle ("Dry");
        patchButton.onClick = [this] { showPatchMenu(); };
        timerCallback();
        startTimerHz (5);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        screen.setBounds (r.removeFromLeft (230));
        r.removeFromLeft (10);

        auto top = r.removeFromTop (22);
        head1.setBounds (top.removeFromLeft (58)); top.removeFromLeft (2);
        head2.setBounds (top.removeFromLeft (58)); top.removeFromLeft (2);
        head3.setBounds (top.removeFromLeft (58)); top.removeFromLeft (10);
        patchButton.setBounds (top.removeFromRight (120)); top.removeFromRight (10);
        dryToggle.setBounds (top.removeFromRight (44)); top.removeFromRight (10);
        syncChoice.setBounds (top);
        r.removeFromTop (6);

        auto upper = r.removeFromTop (r.getHeight() / 2);
        layoutRow (row1(), upper);
        layoutRow (row2(), r);
    }

private:
    EchoDevice& dev;
    EchoScreen screen;
    DeviceUI::Toggle head1, head2, head3;
    DeviceUI::Choice syncChoice;
    DeviceUI::Toggle dryToggle;
    DeviceUI::Knob time, intensity, echo, drive, bass, treble, wow, flutter, wear, noise, spread;
    juce::TextButton patchButton { "Patches" };

    std::vector<DeviceUI::Knob*> row1()     { return { &time, &intensity, &echo, &drive, &bass, &treble }; }
    std::vector<DeviceUI::Knob*> row2()     { return { &wow, &flutter, &wear, &noise, &spread }; }

    static void layoutRow (std::vector<DeviceUI::Knob*> knobs, juce::Rectangle<int> r)
    {
        const int w = r.getWidth() / 6;
        for (auto* k : knobs)
            k->setBounds (r.removeFromLeft (w));
    }

    void showPatchMenu()
    {
        juce::PopupMenu m;
        for (int i = 0; i < (int) EchoDevice::patches().size(); ++i)
            m.addItem (i + 1, EchoDevice::patches()[(size_t) i].name, true, i == dev.getPatchIndex());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchButton), [safe = juce::Component::SafePointer<EchoView> (this)] (int result)
        {
            if (safe != nullptr && result > 0)
                safe->dev.loadPatch (result - 1);
        });
    }

    void timerCallback() override   { patchButton.setButtonText (dev.getPatchLabel()); }
};

inline std::unique_ptr<juce::Component> EchoDevice::createDeviceView()     { return std::make_unique<EchoView> (*this); }

} // namespace elsewhere
