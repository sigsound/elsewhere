// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"
#include "OrganicDsp.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Haze: a synth that sounds remembered rather than played. Warm, slightly detuned
    analog-style voices go through a worn tape and VHS chain: wow and flutter bend the pitch
    (the reverb tail too), Age saturates, lifts the low end and rolls off the highs, Tracking
    throws in the odd glitch and dropout, Chroma smears it wide like an old chorus, Hiss sits
    underneath, Speed slows the whole thing down like a tape played slow, and a dark Haze
    space holds it all. Mod wheel adds warble.

    Voices: two oscillators (saw, square, triangle or a soft wave) detuned from each other,
    a sub, noise, a 12 dB low-pass with its own envelope amount, and an amp envelope.
*/
class HazeDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereHaze";
    static constexpr int maxVoices = 8;
    static constexpr int chunk = 64;

    enum Wave { saw, square, triangle, soft, numWaves };
    static juce::StringArray waveNames()    { return { "Saw", "Square", "Triangle", "Soft" }; }
    static juce::StringArray chromaNames()  { return { "Off", "I", "II", "I+II" }; }

    HazeDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        auto secs = [] (float v) { return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s"; };
        auto cents = [] (float v) { return juce::String (juce::roundToInt (v)) + " ct"; };
        auto pick = [] (juce::StringArray names) { return [names] (float v) { return names[juce::jlimit (0, names.size() - 1, juce::roundToInt (v))]; }; };
        auto octave = [] (float v) { const int o = juce::roundToInt (v); return o == 0 ? juce::String ("0") : (o > 0 ? "+" : "") + juce::String (o) + " oct"; };
        const juce::NormalisableRange<float> pct { 0.0f, 100.0f };

        wave      = addParameter ("wave", "Wave", (float) saw, stepped (0, numWaves - 1), pick (waveNames()));
        detune    = addParameter ("detune", "Detune", 12.0f, { 0.0f, 50.0f }, cents);
        sub       = addParameter ("sub", "Sub", 20.0f, pct, percentText);
        noise     = addParameter ("noise", "Noise", 0.0f, pct, percentText);
        range     = addParameter ("range", "Range", 0.0f, stepped (-2, 1), octave);
        cutoff    = addParameter ("cutoff", "Cutoff", 2400.0f, freqRange (80.0f, 18000.0f), hzText);
        resonance = addParameter ("res", "Resonance", 15.0f, pct, percentText);
        envAmount = addParameter ("envamt", "Env", 30.0f, pct, percentText);
        attack    = addParameter ("attack", "Attack", 0.25f, skewed (0.001f, 6.0f, 0.4f), secs);
        decay     = addParameter ("decay", "Decay", 1.2f, skewed (0.02f, 6.0f, 0.8f), secs);
        sustain   = addParameter ("sustain", "Sustain", 75.0f, pct, percentText);
        release   = addParameter ("release", "Release", 1.4f, skewed (0.02f, 8.0f, 1.2f), secs);
        wow       = addParameter ("wow", "Wow", 35.0f, pct, percentText);
        flutter   = addParameter ("flutter", "Flutter", 20.0f, pct, percentText);
        age       = addParameter ("age", "Age", 40.0f, pct, percentText);
        speed     = addParameter ("speed", "Speed", 92.0f, { 50.0f, 100.0f }, percentText);
        tracking  = addParameter ("tracking", "Tracking", 10.0f, pct, percentText);
        bandwidth = addParameter ("bandwidth", "Bandwidth", 7000.0f, freqRange (1500.0f, 20000.0f), hzText);
        chroma    = addParameter ("chroma", "Chroma", 1.0f, stepped (0, 3), pick (chromaNames()));
        hiss      = addParameter ("hiss", "Hiss", 20.0f, pct, percentText);
        space     = addParameter ("space", "Space", 60.0f, pct, percentText);
        spaceMix  = addParameter ("smix", "Space Mix", 30.0f, pct, percentText);
        drift     = addParameter ("drift", "Drift", 30.0f, pct, percentText);
        volume    = addParameter ("volume", "Volume", -6.0f, { -60.0f, 6.0f }, dbText);

        if (! state.hasProperty ("patch"))
            state.setProperty ("patch", 0, nullptr);
    }

    juce::String getName() const override              { return "Elsewhere Haze"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Haze"; }
    int getDeviceViewWidth() const override            { return 1200; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 8.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }

    //==============================================================================
    // Patches
    struct Patch { const char* name; std::vector<std::pair<const char*, float>> values; };

    static const std::vector<Patch>& patches()
    {
        static const std::vector<Patch> all = {
            { "Saturday Morning", { } },
            { "Pastel Mall",      { { "wave", soft }, { "detune", 8 }, { "sub", 30 }, { "cutoff", 4000 }, { "speed", 85 }, { "chroma", 2 },
                                    { "space", 70 }, { "smix", 45 }, { "release", 2.5f }, { "age", 25 } } },
            { "Rerun",            { { "wave", square }, { "detune", 18 }, { "cutoff", 1500 }, { "res", 25 }, { "envamt", 40 }, { "decay", 0.8f },
                                    { "sustain", 40 }, { "wow", 50 }, { "age", 55 }, { "tracking", 30 } } },
            { "Late Broadcast",   { { "cutoff", 900 }, { "res", 30 }, { "attack", 1.2f }, { "release", 3.0f }, { "wow", 60 }, { "flutter", 30 },
                                    { "age", 70 }, { "hiss", 45 }, { "tracking", 25 }, { "speed", 90 } } },
            { "Station Ident",    { { "wave", triangle }, { "sub", 40 }, { "attack", 0.002f }, { "decay", 0.6f }, { "sustain", 0 }, { "release", 0.8f },
                                    { "envamt", 50 }, { "wow", 20 }, { "space", 45 }, { "volume", 6 } } },
            { "Marine Layer",     { { "wave", soft }, { "detune", 25 }, { "attack", 2.0f }, { "release", 4.0f }, { "cutoff", 1800 }, { "wow", 45 },
                                    { "speed", 80 }, { "space", 85 }, { "smix", 55 }, { "chroma", 3 } } },
            { "Kid Memory",       { { "wave", square }, { "detune", 6 }, { "cutoff", 3000 }, { "decay", 1.2f }, { "sustain", 30 }, { "wow", 40 },
                                    { "flutter", 40 }, { "age", 50 }, { "tracking", 20 } } },
            { "Faded Polaroid",   { { "detune", 30 }, { "cutoff", 1200 }, { "attack", 0.6f }, { "wow", 70 }, { "age", 80 }, { "bandwidth", 3500 },
                                    { "hiss", 60 }, { "tracking", 45 } } },
            { "Sunday Bass",      { { "range", -1 }, { "sub", 60 }, { "cutoff", 600 }, { "res", 20 }, { "envamt", 45 }, { "attack", 0.005f },
                                    { "decay", 0.5f }, { "sustain", 50 }, { "release", 0.4f }, { "wow", 15 }, { "age", 35 }, { "space", 20 }, { "smix", 10 } } },
            { "Ghost Choir",      { { "wave", soft }, { "detune", 35 }, { "noise", 15 }, { "attack", 1.5f }, { "release", 3.5f }, { "cutoff", 2600 },
                                    { "wow", 55 }, { "speed", 88 }, { "chroma", 2 }, { "space", 80 }, { "smix", 50 } } },
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
        edit.getUndoManager().beginNewTransaction ("Load Haze patch");
        for (auto& p : params)
            p->getParameter().setParameter (patchValue (patch, p->getParameter()), juce::sendNotification);
        state.setProperty ("patch", index, &edit.getUndoManager());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", -1); }

    static juce::String patchLabel (int index)
    {
        if (index < 0 || index >= (int) patches().size())
            return "INIT";
        return "H" + juce::String (index + 1).paddedLeft ('0', 3) + " " + patches()[(size_t) index].name;
    }

    bool isPatchModified() const
    {
        const int index = getPatchIndex();
        if (index < 0 || index >= (int) patches().size())
            return true;
        const auto& patch = patches()[(size_t) index];
        for (auto& p : params)
        {
            auto& ap = p->getParameter();
            const float v = patchValue (patch, ap);
            if (std::abs (ap.getCurrentBaseValue() - v) > 1.0e-3f * juce::jmax (1.0f, std::abs (v)))
                return true;
        }
        return false;
    }

    //==============================================================================
    void initialise (const te::PluginInitialisationInfo& info) override
    {
        rate = info.sampleRate;
        for (size_t i = 0; i < voice.size(); ++i)
        {
            voice[i] = Voice();
            voice[i].drift = SynthCore::Drift ((int) i * 19 + 5);
        }
        chorusFx.prepare (rate);
        spaceFx.prepare (rate);
        for (auto& t : tape) t.assign ((size_t) (rate * 0.08) + 8, 0.0f);
        tapePos = 0;
        bandL1.reset(); bandL2.reset(); bandR1.reset(); bandR2.reset(); bumpL.reset(); bumpR.reset(); hissHp.reset();
        wowPhase = flutterPhase = flutter2Phase = 0.0f;
        wowRandom = wowTarget = 0.0f;
        glitchLeft = 0; glitchDepth = 0.0f; glitchSag = 0.0f;
        bend = modWheel = rolling = 0.0f;
        pedal = false;
        noteCounter = 0;
        rng.setSeed (1977);
    }

    void deinitialise() override {}

    void midiPanic() override
    {
        for (auto& v : voice) { v.held = v.sustained = false; v.amp.gate (false); v.filt.gate (false); }
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr || tape[0].empty())
            return;

        juce::ScopedNoDenormals noDenormals;
        auto& buf = *fc.destBuffer;
        buf.clear (fc.bufferStartSample, fc.bufferNumSamples);

        int done = 0;
        if (fc.bufferForMidiMessages != nullptr)
        {
            if (fc.bufferForMidiMessages->isAllNotesOff)
                midiPanic();
            for (auto& m : *fc.bufferForMidiMessages)
            {
                auto pos = juce::jlimit (0, fc.bufferNumSamples, juce::roundToInt (m.getTimeStamp() * rate));
                if (pos > done) { render (buf, fc.bufferStartSample + done, pos - done); done = pos; }
                handleMidi (m);
            }
        }
        if (done < fc.bufferNumSamples)
            render (buf, fc.bufferStartSample + done, fc.bufferNumSamples - done);
    }

    // For the screen
    SynthCore::ScopeBuffer scope;
    std::atomic<float> wowNow { 0.0f };         // the tape's pitch deviation right now, -1..1
    std::atomic<bool> glitching { false };
    std::atomic<double> playSeconds { 0.0 };    // tape counter: time spent playing
    int getActiveVoices() const                 { return activeVoices.load(); }

    Param wave, detune, sub, noise, range, cutoff, resonance, envAmount, attack, decay, sustain, release;
    Param wow, flutter, age, speed, tracking, bandwidth, chroma, hiss, space, spaceMix, drift, volume;

private:
    struct Voice
    {
        int note = -1;
        float vel = 1.0f, pan = 0.0f;
        bool held = false, sustained = false;
        juce::uint32 age = 0;
        std::array<SynthCore::Oscillator, 3> osc;      // two detuned oscillators and the sub
        SynthCore::Svf filter;
        SynthCore::AnalogEnvelope amp, filt;
        SynthCore::Drift drift;
        bool active() const { return amp.isActive(); }
    };

    double rate = 48000.0;
    std::array<Voice, maxVoices> voice;
    SynthCore::Noise noiseGen;
    SynthCore::StereoChorus chorusFx;
    OrganicDsp::Space spaceFx;
    SynthCore::Svf bandL1, bandL2, bandR1, bandR2, bumpL, bumpR, hissHp;
    std::array<std::vector<float>, 2> tape;
    int tapePos = 0;
    float wowPhase = 0.0f, flutterPhase = 0.0f, flutter2Phase = 0.0f, wowRandom = 0.0f, wowTarget = 0.0f;
    int glitchLeft = 0, glitchLength = 1;
    float glitchDepth = 0.0f, glitchSag = 0.0f;
    float bend = 0.0f, modWheel = 0.0f, rolling = 0.0f;
    bool pedal = false;
    juce::uint32 noteCounter = 0;
    juce::Random rng;
    std::atomic<int> activeVoices { 0 };
    std::array<float, chunk> ml {}, mr {};

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())                       noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())                 noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())              bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
        else if (m.isControllerOfType (1))      modWheel = (float) m.getControllerValue() / 127.0f;
        else if (m.isSustainPedalOn())          pedal = true;
        else if (m.isSustainPedalOff())
        {
            pedal = false;
            for (auto& v : voice)
                if (v.sustained && ! v.held) { v.sustained = false; v.amp.gate (false); v.filt.gate (false); }
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            midiPanic();
    }

    void noteOn (int note, float vel)
    {
        Voice* target = nullptr;
        for (auto& v : voice) if (v.active() && v.note == note) { target = &v; break; }
        if (target == nullptr) for (auto& v : voice) if (! v.active()) { target = &v; break; }
        if (target == nullptr)
            for (int pass = 0; pass < 2 && target == nullptr; ++pass)
            {
                juce::uint32 oldest = std::numeric_limits<juce::uint32>::max();
                for (auto& v : voice)
                    if ((pass == 1 || ! v.held) && v.age < oldest) { oldest = v.age; target = &v; }
            }

        auto& v = *target;
        if (! v.active())
        {
            for (auto& o : v.osc) o.reset (noiseGen.white() * 0.5f + 0.5f);
            v.filter.reset();
        }
        v.note = note;
        v.vel = vel;
        v.held = true;
        v.sustained = false;
        v.age = ++noteCounter;
        v.pan = ((noteCounter % 2) == 0 ? 1.0f : -1.0f) * (0.1f + 0.15f * (float) ((noteCounter / 2) % 3) / 2.0f);
        v.amp.gate (true);
        v.filt.gate (true);
    }

    void noteOff (int note)
    {
        for (auto& v : voice)
        {
            if (v.note != note || ! v.held) continue;
            v.held = false;
            if (pedal) v.sustained = true;
            else { v.amp.gate (false); v.filt.gate (false); }
        }
    }

    static float mtof (float note)      { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }

    void render (juce::AudioBuffer<float>& buf, int start, int num)
    {
        for (int done = 0; done < num; )
        {
            const int n = juce::jmin (chunk, num - done);
            renderChunk (buf, start + done, n);
            done += n;
        }
    }

    void renderChunk (juce::AudioBuffer<float>& buf, int start, int n)
    {
        auto* L = buf.getWritePointer (0, start);
        auto* R = buf.getNumChannels() > 1 ? buf.getWritePointer (1, start) : nullptr;
        std::fill (ml.begin(), ml.begin() + n, 0.0f);
        std::fill (mr.begin(), mr.begin() + n, 0.0f);

        const float sr = (float) rate;
        const int w = juce::roundToInt (wave.get());
        const float det = detune.get() / 100.0f;            // semitones (0..0.5)
        const float subLevel = sub.get() / 100.0f, noiseLevel = noise.get() / 100.0f;
        const float slowSemis = 12.0f * std::log2 (juce::jlimit (0.5f, 1.0f, speed.get() / 100.0f));
        const float rangeSemis = 12.0f * (float) juce::roundToInt (range.get());
        const float res = resonance.get() / 100.0f * 0.85f;
        const float envOct = envAmount.get() / 100.0f * 5.0f;
        const float a = attack.get(), d = decay.get(), s = sustain.get() / 100.0f, r = release.get();
        const float driftSemis = drift.get() / 100.0f * 0.18f;
        const float bendSemis = bend * 2.0f;

        int active = 0;
        for (auto& v : voice)
        {
            if (! v.active()) continue;
            ++active;
            const float velGain = 0.55f + 0.45f * v.vel;
            const float driftNow = v.drift.process (driftSemis, sr / (float) n);
            const float pitch = (float) v.note + slowSemis + rangeSemis + bendSemis + driftNow;
            const float dt1 = mtof (pitch - det * 0.5f) / sr, dt2 = mtof (pitch + det * 0.5f) / sr, dtSub = mtof (pitch - 12.0f) / sr;
            const float base = cutoff.get() * std::pow (2.0f, (pitch - 60.0f) / 12.0f * 0.35f) * (0.75f + 0.25f * v.vel);

            for (int i = 0; i < n; ++i)
            {
                const float fenv = v.filt.process (a * 0.6f, d, s * 0.6f, r, sr);
                const float env = v.amp.process (a, d, s, r, sr);
                if ((i & 7) == 0)
                    v.filter.set (juce::jlimit (40.0f, sr * 0.45f, base * std::pow (2.0f, envOct * fenv * v.vel)), res, sr);

                float o1, o2;
                switch (w)
                {
                    case square:   o1 = v.osc[0].process (SynthCore::Wave::square, dt1); o2 = v.osc[1].process (SynthCore::Wave::square, dt2); break;
                    case triangle: o1 = v.osc[0].process (SynthCore::Wave::triangle, dt1); o2 = v.osc[1].process (SynthCore::Wave::triangle, dt2); break;
                    case soft:
                    {
                        auto softAt = [] (SynthCore::Oscillator& o, float dt)
                        {
                            const float p = o.phase * juce::MathConstants<float>::twoPi;
                            const float y = std::sin (p) + 0.22f * std::sin (2.0f * p) + 0.08f * std::sin (3.0f * p);
                            o.advance (dt);
                            return y * 0.85f;
                        };
                        o1 = softAt (v.osc[0], dt1); o2 = softAt (v.osc[1], dt2); break;
                    }
                    default:       o1 = v.osc[0].process (SynthCore::Wave::saw, dt1); o2 = v.osc[1].process (SynthCore::Wave::saw, dt2); break;
                }
                const float subOsc = v.osc[2].process (SynthCore::Wave::sine, dtSub);
                float x = (o1 + o2) * 0.4f + subOsc * subLevel * 0.6f + noiseGen.white() * noiseLevel * 0.25f;
                x = v.filter.lowpass (x) * env * velGain * 0.5f;

                const float pan = v.pan;
                ml[(size_t) i] += x * (1.0f - juce::jmax (0.0f, pan));
                mr[(size_t) i] += x * (1.0f + juce::jmin (0.0f, pan));
            }
        }
        activeVoices = active;

        processTape (n, active > 0);

        const float out = juce::Decibels::decibelsToGain (volume.get(), -60.0f);
        for (int i = 0; i < n; ++i)
        {
            const float l = ml[(size_t) i] * out, rr = mr[(size_t) i] * out;
            scope.push ((l + rr) * 0.5f);
            L[i] += std::abs (l) < 0.8f ? l : std::copysign (0.8f + 0.18f * std::tanh ((std::abs (l) - 0.8f) / 0.18f), l);
            if (R != nullptr)
                R[i] += std::abs (rr) < 0.8f ? rr : std::copysign (0.8f + 0.18f * std::tanh ((std::abs (rr) - 0.8f) / 0.18f), rr);
        }
        if (active > 0)
            playSeconds = playSeconds.load() + (double) n / rate;
    }

    /** Chroma, Age, Bandwidth, wow and flutter, Tracking, Hiss, then the Haze space. */
    void processTape (int n, bool playing)
    {
        const float sr = (float) rate;
        const float rollCoeff = 1.0f / (sr * (playing ? 0.15f : 0.8f));
        chorusFx.process (ml.data(), mr.data(), n, juce::roundToInt (chroma.get()), sr);

        // Age: saturation, a head bump in the lows, highs rolling off; Bandwidth on top
        const float ageAmt = age.get() / 100.0f;
        const float drive = 1.0f + ageAmt * 2.5f;
        const float comp = 1.0f / std::tanh (drive * 0.6f);
        const float bw = juce::jlimit (800.0f, sr * 0.45f, bandwidth.get() * (1.0f - 0.62f * ageAmt));
        bandL1.set (bw, 0.15f, sr); bandL2.set (bw * 1.3f, 0.05f, sr);
        bandR1.set (bw, 0.15f, sr); bandR2.set (bw * 1.3f, 0.05f, sr);
        bumpL.set (95.0f, 0.4f, sr); bumpR.set (95.0f, 0.4f, sr);
        hissHp.set (2500.0f, 0.1f, sr);

        // Wow and flutter: one tape for both channels (the whole mix bends together)
        const float warble = 1.0f + modWheel * 1.6f;
        const float wowMs = wow.get() / 100.0f * 5.5f * warble;
        const float flutterMs = flutter.get() / 100.0f * 0.22f;
        const float trackAmt = tracking.get() / 100.0f;
        const float hissLevel = hiss.get() / 100.0f * 0.022f * (0.6f + 0.6f * ageAmt);
        const int len = (int) tape[0].size();
        const float baseDelay = 0.012f * sr;

        for (int i = 0; i < n; ++i)
        {
            float l = ml[(size_t) i], r = mr[(size_t) i];

            if (ageAmt > 0.001f)
            {
                l = juce::jmap (ageAmt, l, std::tanh (l * drive * 0.6f) * comp * 0.6f);
                r = juce::jmap (ageAmt, r, std::tanh (r * drive * 0.6f) * comp * 0.6f);
                float lo, bp, hp;
                bumpL.process (l, lo, bp, hp); l += bp * 0.35f * ageAmt;
                bumpR.process (r, lo, bp, hp); r += bp * 0.35f * ageAmt;
            }
            l = bandL2.lowpass (bandL1.lowpass (l));
            r = bandR2.lowpass (bandR1.lowpass (r));

            // Tracking glitches: now and then the picture (and sound) sags and drops out
            if (glitchLeft <= 0 && trackAmt > 0.0f && rng.nextFloat() < trackAmt * 0.9f / sr)
            {
                glitchLength = glitchLeft = (int) (sr * (0.06f + 0.22f * rng.nextFloat()));
                glitchDepth = 0.35f + 0.6f * rng.nextFloat() * trackAmt;
            }
            float glitchGain = 1.0f;
            if (glitchLeft > 0)
            {
                const float ph = 1.0f - (float) glitchLeft / (float) glitchLength;
                const float shape = std::sin (ph * juce::MathConstants<float>::pi);
                glitchGain = 1.0f - glitchDepth * shape * 0.8f;
                glitchSag += ((glitchDepth * shape * 0.0025f * sr) - glitchSag) * 0.002f;
                --glitchLeft;
            }
            else
                glitchSag *= 0.999f;

            // Wow: a slow wobble that wanders; flutter: a faster shimmer
            wowPhase += (0.45f + 0.15f * wowRandom) / sr;
            if (wowPhase >= 1.0f) { wowPhase -= 1.0f; wowTarget = rng.nextFloat() * 2.0f - 1.0f; }
            wowRandom += (wowTarget - wowRandom) * (1.0f / (sr * 0.8f));
            flutterPhase += 7.3f / sr;   if (flutterPhase >= 1.0f) flutterPhase -= 1.0f;
            flutter2Phase += 12.9f / sr; if (flutter2Phase >= 1.0f) flutter2Phase -= 1.0f;
            const float wowShape = std::sin (wowPhase * juce::MathConstants<float>::twoPi) * 0.75f + wowRandom * 0.25f;
            const float flut = std::sin (flutterPhase * juce::MathConstants<float>::twoPi) * 0.7f
                             + std::sin (flutter2Phase * juce::MathConstants<float>::twoPi) * 0.3f;
            const float delay = baseDelay + (wowMs * wowShape + flutterMs * flut) * 0.001f * sr + glitchSag;
            if ((i & 15) == 0)
                wowNow = juce::jlimit (-1.0f, 1.0f, wowShape * wow.get() / 100.0f * warble);

            tape[0][(size_t) tapePos] = l;
            tape[1][(size_t) tapePos] = r;
            auto read = [&] (const std::vector<float>& t)
            {
                float rp = (float) tapePos - delay;
                while (rp < 0.0f) rp += (float) len;
                const int i0 = (int) rp % len, i1 = (i0 + 1) % len;
                const float f = rp - std::floor (rp);
                return t[(size_t) i0] + f * (t[(size_t) i1] - t[(size_t) i0]);
            };
            l = read (tape[0]) * glitchGain;
            r = read (tape[1]) * glitchGain;
            if (++tapePos >= len) tapePos = 0;

            // Hiss (and a burst of it in a glitch)
            rolling += ((playing ? 1.0f : 0.0f) - rolling) * rollCoeff;    // the tape only hisses while it's rolling
            const float h = hissHp.highpass (noiseGen.pink()) * (hissLevel + (1.0f - glitchGain) * 0.03f) * rolling;
            ml[(size_t) i] = l + h;
            mr[(size_t) i] = r + h * 0.9f;
        }
        glitching = glitchLeft > 0;

        spaceFx.process (ml.data(), mr.data(), n, space.get() / 100.0f, spaceMix.get() / 100.0f, 1.0f);
    }
};

//==============================================================================
/**
    The screen: a flat VHS picture. The signal as a waveform, a little chroma fringe either
    side of it, scanlines that slide with the tape's wobble, a band of tracking noise in a
    glitch, and the on-screen display (PLAY, SP, the patch, the tape counter).
*/
class HazeScreen : public juce::Component, private juce::Timer
{
public:
    explicit HazeScreen (HazeDevice& d) : dev (d)   { startTimerHz (30); }

    juce::String patchName;

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 6.0f);

        const float wobble = dev.wowNow.load();
        const auto ink = Theme::isLight() ? Colours::text : juce::Colour (0xffdde4f7);
        auto pic = r.reduced (10.0f, 26.0f);

        // Scanlines, sliding sideways with the wobble
        for (int i = 0; i < 9; ++i)
        {
            const float y = pic.getY() + pic.getHeight() * (float) i / 8.0f;
            const float shift = wobble * 6.0f * std::sin ((float) i * 0.9f + (float) frame * 0.05f);
            g.setColour (Colours::outline.withAlpha (0.55f));
            g.fillRect (juce::Rectangle<float> (pic.getX() + shift, y - 0.5f, pic.getWidth(), 1.0f));
        }

        // The signal, with a chroma fringe
        std::array<float, 256> s {};
        dev.scope.read (s.data(), (int) s.size());
        float peak = 0.0f;
        for (auto v : s) peak = juce::jmax (peak, std::abs (v));
        displayPeak = juce::jmax (peak, displayPeak * 0.97f);           // eases down so quiet tails don't jump
        const float gain = 0.9f / juce::jmax (0.08f, displayPeak);
        auto path = [&] (float dx)
        {
            juce::Path p;
            for (size_t i = 0; i < s.size(); ++i)
            {
                const float x = pic.getX() + pic.getWidth() * (float) i / (float) (s.size() - 1) + dx
                              + wobble * 3.0f * std::sin ((float) i * 0.05f + (float) frame * 0.08f);
                const float y = pic.getCentreY() - juce::jlimit (-1.0f, 1.0f, s[i] * gain) * pic.getHeight() * 0.45f;
                if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
            }
            return p;
        };
        const int chromaMode = juce::roundToInt (dev.chroma.get());
        if (chromaMode > 0)
        {
            const float fringe = 1.0f + (float) chromaMode;
            g.setColour (Colours::clips[4].withAlpha (0.55f));
            g.strokePath (path (-fringe), juce::PathStrokeType (1.4f));
            g.setColour (Colours::clips[6].withAlpha (0.55f));
            g.strokePath (path (fringe), juce::PathStrokeType (1.4f));
        }
        g.setColour (Theme::indicator (Colours::accent));
        g.strokePath (path (0.0f), juce::PathStrokeType (1.6f));

        // Tracking noise in a glitch
        if (dev.glitching.load())
        {
            juce::Random rng ((juce::int64) frame);
            const float bandY = pic.getY() + pic.getHeight() * (0.2f + 0.6f * rng.nextFloat());
            g.setColour (ink.withAlpha (0.6f));
            for (int i = 0; i < 40; ++i)
                g.fillRect (juce::Rectangle<float> (pic.getX() + rng.nextFloat() * pic.getWidth(), bandY + rng.nextFloat() * 14.0f,
                                                    4.0f + rng.nextFloat() * 22.0f, 1.5f));
        }

        // On-screen display
        g.setColour (ink);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 12.0f, juce::Font::bold));
        auto top = r.reduced (10.0f, 6.0f).removeFromTop (16.0f);
        auto bottom = r.reduced (10.0f, 6.0f).removeFromBottom (16.0f);
        const bool playing = dev.getActiveVoices() > 0;
        g.drawText (juce::String::fromUTF8 (playing ? "PLAY \xe2\x96\xb6" : "STOP \xe2\x96\xa0"), top, juce::Justification::centredLeft);
        g.drawText ("SP", top, juce::Justification::centredRight);
        g.setFont (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(), 10.5f, juce::Font::plain));
        const auto secs = (int) dev.playSeconds.load();
        g.drawText (juce::String (secs / 3600) + ":" + juce::String ((secs / 60) % 60).paddedLeft ('0', 2) + ":" + juce::String (secs % 60).paddedLeft ('0', 2),
                    bottom, juce::Justification::centredLeft);
        g.drawText (dev.glitching.load() ? juce::String ("TRACKING") : patchName.toUpperCase(), bottom, juce::Justification::centredRight);
    }

private:
    HazeDevice& dev;
    juce::uint32 frame = 0;
    float displayPeak = 0.0f;

    void timerCallback() override
    {
        ++frame;
        repaint();
    }
};

//==============================================================================
class HazeView : public SynthPanel::PanelBase, private juce::Timer
{
public:
    HazeView (HazeDevice& d) : dev (d), screen (d)
    {
        addAndMakeVisible (screen);
        for (auto* b : { &prevButton, &nextButton, &patchButton })
            addAndMakeVisible (b);
        prevButton.onClick = [this] { step (-1); };
        prevButton.setTitle ("Previous Patch");
        nextButton.onClick = [this] { step (1); };
        nextButton.setTitle ("Next Patch");
        patchButton.onClick = [this] { showPatchMenu(); };

        const auto cTone = Colours::accent, cFilt = Colours::clips[4], cEnv = Colours::clips[2], cTape = Colours::clips[6],
                   cVhs = Colours::clips[5], cHaze = Colours::clips[3];
        waveSel = selector (d.wave, HazeDevice::waveNames(), cTone);
        toneKnobs = { knob (d.detune, "Detune", cTone), knob (d.sub, "Sub", cTone), knob (d.noise, "Noise", cTone), knob (d.range, "Range", cTone) };
        filterKnobs = { knob (d.cutoff, "Cutoff", cFilt), knob (d.resonance, "Resonance", cFilt), knob (d.envAmount, "Env", cFilt) };
        envKnobs = { knob (d.attack, "Attack", cEnv), knob (d.decay, "Decay", cEnv), knob (d.sustain, "Sustain", cEnv), knob (d.release, "Release", cEnv) };
        tapeKnobs = { knob (d.wow, "Wow", cTape), knob (d.flutter, "Flutter", cTape), knob (d.age, "Age", cTape), knob (d.speed, "Speed", cTape) };
        chromaSel = selector (d.chroma, HazeDevice::chromaNames(), cVhs);
        vhsKnobs = { knob (d.tracking, "Tracking", cVhs), knob (d.bandwidth, "Bandwidth", cVhs), knob (d.hiss, "Hiss", cVhs) };
        hazeKnobs = { knob (d.space, "Space", cHaze), knob (d.spaceMix, "Mix", cHaze), knob (d.drift, "Drift", cHaze), knob (d.volume, "Volume", Colours::accent) };

        timerCallback();
        startTimerHz (5);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, toneArea, "Tone", Colours::accent);
        SynthPanel::paintSection (g, filterArea, "Filter", Colours::clips[4]);
        SynthPanel::paintSection (g, envArea, "Envelope", Colours::clips[2]);
        SynthPanel::paintSection (g, tapeArea, "Tape", Colours::clips[6]);
        SynthPanel::paintSection (g, vhsArea, "VHS", Colours::clips[5]);
        SynthPanel::paintSection (g, hazeArea, "Haze", Colours::clips[3]);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto display = r.removeFromLeft (260);
        auto nav = display.removeFromBottom (24);
        display.removeFromBottom (4);
        screen.setBounds (display);
        prevButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        nextButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        patchButton.setBounds (nav);
        r.removeFromLeft (6);

        auto cols = columns (r, { 176, 128, 128, 128, 176, 156 }, 6);
        toneArea = cols[0]; filterArea = cols[1]; envArea = cols[2]; tapeArea = cols[3]; vhsArea = cols[4]; hazeArea = cols[5];

        auto grid = [] (juce::Rectangle<int> c, std::vector<SynthPanel::Knob*>& k)
        {
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { k[0], k[1] }, 2);
            if (k.size() > 3) row (c, { k[2], k[3] }, 2);
            else row (c, { k[2] }, 2);
        };
        {
            auto c = content (toneArea);
            waveSel->setBounds (c.removeFromTop (18)); c.removeFromTop (3);
            grid (c, toneKnobs);
        }
        grid (content (filterArea), filterKnobs);
        grid (content (envArea), envKnobs);
        grid (content (tapeArea), tapeKnobs);
        {
            auto c = content (vhsArea);
            chromaSel->setBounds (c.removeFromTop (18)); c.removeFromTop (6);
            row (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (c.getHeight(), 66)), { vhsKnobs[0], vhsKnobs[1], vhsKnobs[2] }, 3);
        }
        grid (content (hazeArea), hazeKnobs);
    }

private:
    HazeDevice& dev;
    HazeScreen screen;
    juce::TextButton prevButton { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextButton { juce::String::fromUTF8 ("\xe2\x96\xb6") }, patchButton { "Patches" };
    SynthPanel::Selector *waveSel = nullptr, *chromaSel = nullptr;
    std::vector<SynthPanel::Knob*> toneKnobs, filterKnobs, envKnobs, tapeKnobs, vhsKnobs, hazeKnobs;
    juce::Rectangle<int> toneArea, filterArea, envArea, tapeArea, vhsArea, hazeArea;

    void step (int delta)
    {
        const int count = (int) HazeDevice::patches().size();
        const int index = dev.getPatchIndex();
        dev.loadPatch (index < 0 ? (delta > 0 ? 0 : count - 1) : (index + delta + count) % count);
    }

    void showPatchMenu()
    {
        juce::PopupMenu m;
        for (int i = 0; i < (int) HazeDevice::patches().size(); ++i)
            m.addItem (i + 1, HazeDevice::patchLabel (i), true, i == dev.getPatchIndex());
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchButton), [safe = juce::Component::SafePointer<HazeView> (this)] (int result)
        {
            if (safe != nullptr && result > 0)
                safe->dev.loadPatch (result - 1);
        });
    }

    void timerCallback() override
    {
        const int index = dev.getPatchIndex();
        auto name = HazeDevice::patchLabel (index);
        if (index >= 0 && dev.isPatchModified())
            name << " *";
        screen.patchName = name;
    }
};

inline std::unique_ptr<juce::Component> HazeDevice::createDeviceView()     { return std::make_unique<HazeView> (*this); }

} // namespace elsewhere
