// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthCore.h"
#include "SpaceDsp.h"

#include <cstring>
#include <string>
#include <utility>

namespace elsewhere
{
//==============================================================================
/**
    The sound of Elsewhere Haze, on its own: voices, the tape and VHS chain and the space, with
    nothing of JUCE beyond jlimit, jmin, jmax and MathConstants, so it builds anywhere. The
    Haze device plays through it, and so can anything else that wants to sound the same.

    Call prepare() first, then noteOn and noteOff as notes come, and render() for each block.
    None of them allocates or locks. The randomness (drift, noise, tape glitches) starts from
    fixed seeds on every prepare(), so the same notes always render the same audio.
*/
class HazeEngine
{
public:
    static constexpr int maxVoices = 8;
    static constexpr int chunk = 64;

    enum Wave { saw, square, triangle, soft, numWaves };

    /** Every control, in the units the device shows (percent, seconds, Hz, dB). */
    struct Params
    {
        float wave = saw, detune = 12.0f, sub = 20.0f, noise = 0.0f, range = 0.0f;
        float cutoff = 2400.0f, resonance = 15.0f, envAmount = 30.0f;
        float attack = 0.25f, decay = 1.2f, sustain = 75.0f, release = 1.4f;
        float wow = 35.0f, flutter = 20.0f, age = 40.0f, speed = 92.0f, tracking = 10.0f, bandwidth = 7000.0f;
        float chroma = 1.0f, hiss = 20.0f, space = 60.0f, spaceMix = 30.0f, drift = 30.0f, volume = -6.0f;

        /** Each control by the ID the device saves it under, so patches and parameters line up. */
        float* find (const char* id) noexcept
        {
            for (auto& [name, value] : table())
                if (std::strcmp (name, id) == 0)
                    return &(this->*value);
            return nullptr;
        }

        static const std::vector<std::pair<const char*, float Params::*>>& table()
        {
            static const std::vector<std::pair<const char*, float Params::*>> all = {
                { "wave", &Params::wave }, { "detune", &Params::detune }, { "sub", &Params::sub }, { "noise", &Params::noise },
                { "range", &Params::range }, { "cutoff", &Params::cutoff }, { "res", &Params::resonance }, { "envamt", &Params::envAmount },
                { "attack", &Params::attack }, { "decay", &Params::decay }, { "sustain", &Params::sustain }, { "release", &Params::release },
                { "wow", &Params::wow }, { "flutter", &Params::flutter }, { "age", &Params::age }, { "speed", &Params::speed },
                { "tracking", &Params::tracking }, { "bandwidth", &Params::bandwidth }, { "chroma", &Params::chroma }, { "hiss", &Params::hiss },
                { "space", &Params::space }, { "smix", &Params::spaceMix }, { "drift", &Params::drift }, { "volume", &Params::volume },
            };
            return all;
        }
    };

    Params params;

    //==============================================================================
    // Patches: each sets the controls it names and leaves the rest at their defaults
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

    static int patchCount() noexcept                { return (int) patches().size(); }
    static const char* patchName (int i) noexcept   { return i >= 0 && i < patchCount() ? patches()[(size_t) i].name : ""; }

    /** A control's value in a patch: the patch's own, or the default. */
    static float patchValue (int i, const char* id)
    {
        if (i >= 0 && i < patchCount())
            for (auto& [name, value] : patches()[(size_t) i].values)
                if (std::strcmp (name, id) == 0)
                    return value;

        Params defaults;
        auto* v = defaults.find (id);
        return v != nullptr ? *v : 0.0f;
    }

    /** Sets every control to the patch (not on the audio thread while it renders). */
    void setPatch (int i)
    {
        for (auto& [id, member] : Params::table())
            params.*member = patchValue (i, id);
    }

    //==============================================================================
    void prepare (double sampleRate, int /*maxBlock*/ = 0)
    {
        rate = sampleRate;
        for (size_t i = 0; i < voice.size(); ++i)
        {
            voice[i] = Voice();
            voice[i].drift = SynthCore::Drift ((int) i * 19 + 5);
        }
        noiseGen = SynthCore::Noise();
        chorusFx = SynthCore::StereoChorus();
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
        playSeconds = 0.0;
    }

    bool isPrepared() const noexcept    { return ! tape[0].empty(); }

    //==============================================================================
    void noteOn (int note, float velocity)
    {
        Voice* target = nullptr;
        for (auto& v : voice) if (v.active() && v.note == note) { target = &v; break; }
        if (target == nullptr) for (auto& v : voice) if (! v.active()) { target = &v; break; }
        if (target == nullptr)
            for (int pass = 0; pass < 2 && target == nullptr; ++pass)
            {
                std::uint32_t oldest = std::numeric_limits<std::uint32_t>::max();
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
        v.vel = velocity;
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

    /** Every note let go (they ring out through their release). */
    void allNotesOff()
    {
        for (auto& v : voice) { v.held = v.sustained = false; v.amp.gate (false); v.filt.gate (false); }
    }

    void setPitchBend (float minusOneToOne)     { bend = minusOneToOne; }    // ±2 semitones
    void setModWheel (float zeroToOne)          { modWheel = zeroToOne; }    // adds warble

    void setSustainPedal (bool down)
    {
        pedal = down;
        if (! down)
            for (auto& v : voice)
                if (v.sustained && ! v.held) { v.sustained = false; v.amp.gate (false); v.filt.gate (false); }
    }

    //==============================================================================
    /** Renders n samples, adding them to L and R. */
    void render (float* L, float* R, int num)
    {
        if (! isPrepared())
            return;

        for (int done = 0; done < num; )
        {
            const int n = std::min (chunk, num - done);
            renderChunk (L + done, R + done, n);
            done += n;
        }
    }

    // What's going on, for drawing
    SynthCore::ScopeBuffer scope;
    std::atomic<float> wowNow { 0.0f };         // the tape's pitch deviation right now, -1..1
    std::atomic<bool> glitching { false };
    std::atomic<double> playSeconds { 0.0 };    // tape counter: time spent playing
    std::atomic<int> activeVoices { 0 };

private:
    struct Voice
    {
        int note = -1;
        float vel = 1.0f, pan = 0.0f;
        bool held = false, sustained = false;
        std::uint32_t age = 0;
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
    std::uint32_t noteCounter = 0;
    SynthCore::Random rng { 1977 };
    std::array<float, chunk> ml {}, mr {};

    static float mtof (float note)      { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }
    static int whole (float v)          { return (int) std::lround (v); }

    void renderChunk (float* L, float* R, int n)
    {
        std::fill (ml.begin(), ml.begin() + n, 0.0f);
        std::fill (mr.begin(), mr.begin() + n, 0.0f);

        const auto& p = params;
        const float sr = (float) rate;
        const int w = whole (p.wave);
        const float det = p.detune / 100.0f;                // semitones (0..0.5)
        const float subLevel = p.sub / 100.0f, noiseLevel = p.noise / 100.0f;
        const float slowSemis = 12.0f * std::log2 (dspmath::jlimit (0.5f, 1.0f, p.speed / 100.0f));
        const float rangeSemis = 12.0f * (float) whole (p.range);
        const float res = p.resonance / 100.0f * 0.85f;
        const float envOct = p.envAmount / 100.0f * 5.0f;
        const float a = p.attack, d = p.decay, s = p.sustain / 100.0f, r = p.release;
        const float driftSemis = p.drift / 100.0f * 0.18f;
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
            const float base = p.cutoff * std::pow (2.0f, (pitch - 60.0f) / 12.0f * 0.35f) * (0.75f + 0.25f * v.vel);

            for (int i = 0; i < n; ++i)
            {
                const float fenv = v.filt.process (a * 0.6f, d, s * 0.6f, r, sr);
                const float env = v.amp.process (a, d, s, r, sr);
                if ((i & 7) == 0)
                    v.filter.set (dspmath::jlimit (40.0f, sr * 0.45f, base * std::pow (2.0f, envOct * fenv * v.vel)), res, sr);

                float o1, o2;
                switch (w)
                {
                    case square:   o1 = v.osc[0].process (SynthCore::Wave::square, dt1); o2 = v.osc[1].process (SynthCore::Wave::square, dt2); break;
                    case triangle: o1 = v.osc[0].process (SynthCore::Wave::triangle, dt1); o2 = v.osc[1].process (SynthCore::Wave::triangle, dt2); break;
                    case soft:
                    {
                        auto softAt = [] (SynthCore::Oscillator& o, float dt)
                        {
                            const float ph = o.phase * dspmath::MathConstants<float>::twoPi;
                            const float y = std::sin (ph) + 0.22f * std::sin (2.0f * ph) + 0.08f * std::sin (3.0f * ph);
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
                ml[(size_t) i] += x * (1.0f - dspmath::jmax (0.0f, pan));
                mr[(size_t) i] += x * (1.0f + dspmath::jmin (0.0f, pan));
            }
        }
        activeVoices = active;

        processTape (n, active > 0);

        const float out = p.volume > -60.0f ? std::pow (10.0f, p.volume * 0.05f) : 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float l = ml[(size_t) i] * out, rr = mr[(size_t) i] * out;
            scope.push ((l + rr) * 0.5f);
            L[i] += std::abs (l) < 0.8f ? l : std::copysign (0.8f + 0.18f * std::tanh ((std::abs (l) - 0.8f) / 0.18f), l);
            R[i] += std::abs (rr) < 0.8f ? rr : std::copysign (0.8f + 0.18f * std::tanh ((std::abs (rr) - 0.8f) / 0.18f), rr);
        }
        if (active > 0)
            playSeconds = playSeconds.load() + (double) n / rate;
    }

    /** Chroma, Age, Bandwidth, wow and flutter, Tracking, Hiss, then the Haze space. */
    void processTape (int n, bool playing)
    {
        const auto& p = params;
        const float sr = (float) rate;
        const float rollCoeff = 1.0f / (sr * (playing ? 0.15f : 0.8f));
        chorusFx.process (ml.data(), mr.data(), n, whole (p.chroma), sr);

        // Age: saturation, a head bump in the lows, highs rolling off; Bandwidth on top
        const float ageAmt = p.age / 100.0f;
        const float drive = 1.0f + ageAmt * 2.5f;
        const float comp = 1.0f / std::tanh (drive * 0.6f);
        const float bw = dspmath::jlimit (800.0f, sr * 0.45f, p.bandwidth * (1.0f - 0.62f * ageAmt));
        bandL1.set (bw, 0.15f, sr); bandL2.set (bw * 1.3f, 0.05f, sr);
        bandR1.set (bw, 0.15f, sr); bandR2.set (bw * 1.3f, 0.05f, sr);
        bumpL.set (95.0f, 0.4f, sr); bumpR.set (95.0f, 0.4f, sr);
        hissHp.set (2500.0f, 0.1f, sr);

        // Wow and flutter: one tape for both channels (the whole mix bends together)
        const float warble = 1.0f + modWheel * 1.6f;
        const float wowMs = p.wow / 100.0f * 5.5f * warble;
        const float flutterMs = p.flutter / 100.0f * 0.22f;
        const float trackAmt = p.tracking / 100.0f;
        const float hissLevel = p.hiss / 100.0f * 0.022f * (0.6f + 0.6f * ageAmt);
        const int len = (int) tape[0].size();
        const float baseDelay = 0.012f * sr;

        for (int i = 0; i < n; ++i)
        {
            float l = ml[(size_t) i], r = mr[(size_t) i];

            if (ageAmt > 0.001f)
            {
                l = l + ageAmt * (std::tanh (l * drive * 0.6f) * comp * 0.6f - l);
                r = r + ageAmt * (std::tanh (r * drive * 0.6f) * comp * 0.6f - r);
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
                const float shape = std::sin (ph * dspmath::MathConstants<float>::pi);
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
            const float wowShape = std::sin (wowPhase * dspmath::MathConstants<float>::twoPi) * 0.75f + wowRandom * 0.25f;
            const float flut = std::sin (flutterPhase * dspmath::MathConstants<float>::twoPi) * 0.7f
                             + std::sin (flutter2Phase * dspmath::MathConstants<float>::twoPi) * 0.3f;
            const float delay = baseDelay + (wowMs * wowShape + flutterMs * flut) * 0.001f * sr + glitchSag;
            if ((i & 15) == 0)
                wowNow = dspmath::jlimit (-1.0f, 1.0f, wowShape * p.wow / 100.0f * warble);

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

        spaceFx.process (ml.data(), mr.data(), n, p.space / 100.0f, p.spaceMix / 100.0f, 1.0f);
    }
};

} // namespace elsewhere
