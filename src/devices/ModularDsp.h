// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthCore.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace elsewhere
{
//==============================================================================
/**
    The modules of Elsewhere Modular, in plain C++: what each module is (its knobs and jacks),
    what it does to the signal, the compiled patch (Program) and the factory patches.

    Signals are plain numbers: audio is about +-1; pitch is volts per octave with 0 V = C4;
    gates are 0 or 1; envelopes run 0..1; LFOs +-1.

    The whole patch runs once per voice (1-8 voices), so patches are polyphonic.
*/
namespace Modular
{
    constexpr int maxVoices = 8, maxPorts = 8, maxParams = 12;

    enum class Signal { audio, cv, gate };

    struct Port     { const char* id; const char* name; Signal signal; };

    struct ParamSpec
    {
        const char* id; const char* name;
        float min, max, def;
        enum Kind { linear, log, stepped } kind = linear;
        const char* unit = "";
    };

    struct Spec
    {
        const char* type; const char* name;
        int columns;
        std::vector<ParamSpec> params;
        std::vector<Port> ins, outs;
        bool fixed = false;     // MIDI In and Output: always there, can't be removed
    };

    inline const std::vector<Spec>& catalogue()
    {
        using P = ParamSpec;
        static const std::vector<Spec> all = {
            { "midi", "MIDI In", 2, { P { "glide", "Glide", 0.0f, 2.0f, 0.0f, P::log, "s" } },
              {}, { { "pitch", "Pitch", Signal::cv }, { "gate", "Gate", Signal::gate }, { "vel", "Velocity", Signal::cv }, { "mod", "Mod", Signal::cv } }, true },

            { "vco", "VCO", 4,
              { P { "octave", "Octave", -3.0f, 3.0f, 0.0f, P::stepped, "oct" }, P { "tune", "Tune", -12.0f, 12.0f, 0.0f, P::linear, "st" },
                P { "fine", "Fine", -50.0f, 50.0f, 0.0f, P::linear, "ct" }, P { "pw", "Width", 5.0f, 95.0f, 50.0f, P::linear, "%" },
                P { "fm", "FM", 0.0f, 100.0f, 0.0f, P::linear, "%" } },
              { { "pitch", "Pitch", Signal::cv }, { "pitch2", "Pitch 2", Signal::cv }, { "fm", "FM", Signal::audio }, { "pw", "PW", Signal::cv }, { "sync", "Sync", Signal::gate } },
              { { "sine", "Sine", Signal::audio }, { "tri", "Tri", Signal::audio }, { "saw", "Saw", Signal::audio }, { "square", "Square", Signal::audio } } },

            { "lfo", "LFO", 2, { P { "rate", "Rate", 0.05f, 30.0f, 2.0f, P::log, "Hz" } },
              { { "rate", "Rate", Signal::cv }, { "reset", "Reset", Signal::gate } },
              { { "sine", "Sine", Signal::cv }, { "tri", "Tri", Signal::cv }, { "saw", "Saw", Signal::cv }, { "square", "Square", Signal::cv } } },

            { "noise", "Noise", 2, {}, {}, { { "white", "White", Signal::audio }, { "pink", "Pink", Signal::audio } } },

            { "filter", "Filter", 4,
              { P { "cutoff", "Cutoff", 20.0f, 20000.0f, 1000.0f, P::log, "Hz" }, P { "res", "Resonance", 0.0f, 100.0f, 20.0f, P::linear, "%" },
                P { "cvamt", "CV Amount", -100.0f, 100.0f, 50.0f, P::linear, "%" }, P { "drive", "Drive", 0.0f, 100.0f, 0.0f, P::linear, "%" } },
              { { "in", "In", Signal::audio }, { "voct", "V/Oct", Signal::cv }, { "cv", "CV", Signal::cv }, { "res", "Res", Signal::cv } },
              { { "lp24", "LP 24", Signal::audio }, { "lp12", "LP 12", Signal::audio }, { "bp", "BP", Signal::audio }, { "hp", "HP", Signal::audio } } },

            { "vca", "VCA", 2,
              { P { "level", "Level", 0.0f, 100.0f, 100.0f, P::linear, "%" }, P { "exp", "Response", 0.0f, 1.0f, 0.0f, P::stepped, "" } },
              { { "in", "In", Signal::audio }, { "cv", "CV", Signal::cv } }, { { "out", "Out", Signal::audio } } },

            { "adsr", "ADSR", 2,
              { P { "a", "Attack", 0.001f, 10.0f, 0.01f, P::log, "s" }, P { "d", "Decay", 0.001f, 10.0f, 0.3f, P::log, "s" },
                P { "s", "Sustain", 0.0f, 100.0f, 60.0f, P::linear, "%" }, P { "r", "Release", 0.001f, 10.0f, 0.3f, P::log, "s" } },
              { { "gate", "Gate", Signal::gate } }, { { "env", "Env", Signal::cv }, { "inv", "Inv", Signal::cv } } },

            { "mixer", "Mixer", 4,
              { P { "l1", "Level 1", 0.0f, 100.0f, 70.0f }, P { "l2", "Level 2", 0.0f, 100.0f, 70.0f }, P { "l3", "Level 3", 0.0f, 100.0f, 70.0f }, P { "l4", "Level 4", 0.0f, 100.0f, 70.0f } },
              { { "in1", "In 1", Signal::audio }, { "in2", "In 2", Signal::audio }, { "in3", "In 3", Signal::audio }, { "in4", "In 4", Signal::audio } },
              { { "out", "Out", Signal::audio } } },

            { "util", "Utility", 2,
              { P { "scale", "Scale", -100.0f, 100.0f, 100.0f, P::linear, "%" }, P { "offset", "Offset", -2.0f, 2.0f, 0.0f, P::linear, "V" } },
              { { "in", "In", Signal::cv } }, { { "out", "Out", Signal::cv } } },

            { "sh", "S&H", 2, { P { "slew", "Slew", 0.0f, 1.0f, 0.0f, P::log, "s" } },
              { { "in", "In", Signal::cv }, { "trig", "Trig", Signal::gate } }, { { "out", "Out", Signal::cv } } },

            { "seq", "Sequencer", 4,
              { P { "s1", "Step 1", -12.0f, 12.0f, 0.0f, P::stepped, "st" }, P { "s2", "Step 2", -12.0f, 12.0f, 3.0f, P::stepped, "st" },
                P { "s3", "Step 3", -12.0f, 12.0f, 7.0f, P::stepped, "st" }, P { "s4", "Step 4", -12.0f, 12.0f, 10.0f, P::stepped, "st" },
                P { "s5", "Step 5", -12.0f, 12.0f, 12.0f, P::stepped, "st" }, P { "s6", "Step 6", -12.0f, 12.0f, 7.0f, P::stepped, "st" },
                P { "s7", "Step 7", -12.0f, 12.0f, 5.0f, P::stepped, "st" }, P { "s8", "Step 8", -12.0f, 12.0f, 3.0f, P::stepped, "st" },
                P { "len", "Gate", 5.0f, 100.0f, 50.0f, P::linear, "%" }, P { "rate", "Rate", 0.5f, 30.0f, 6.0f, P::log, "Hz" } },
              { { "clock", "Clock", Signal::gate }, { "reset", "Reset", Signal::gate } },
              { { "pitch", "Pitch", Signal::cv }, { "gate", "Gate", Signal::gate } } },

            { "delay", "Delay", 3,
              { P { "time", "Time", 1.0f, 2000.0f, 300.0f, P::log, "ms" }, P { "fb", "Feedback", 0.0f, 95.0f, 40.0f, P::linear, "%" },
                P { "mix", "Mix", 0.0f, 100.0f, 35.0f, P::linear, "%" } },
              { { "in", "In", Signal::audio }, { "time", "Time", Signal::cv } }, { { "out", "Out", Signal::audio } } },

            { "fold", "Wavefolder", 2,
              { P { "fold", "Fold", 0.0f, 100.0f, 30.0f, P::linear, "%" }, P { "sym", "Symmetry", -100.0f, 100.0f, 0.0f, P::linear, "%" } },
              { { "in", "In", Signal::audio }, { "cv", "Fold CV", Signal::cv } }, { { "out", "Out", Signal::audio } } },

            { "lpg", "Low-Pass Gate", 2,
              { P { "resp", "Response", 0.0f, 100.0f, 60.0f, P::linear, "%" }, P { "decay", "Decay", 0.02f, 3.0f, 0.35f, P::log, "s" } },
              { { "in", "In", Signal::audio }, { "cv", "CV", Signal::cv }, { "strike", "Strike", Signal::gate } }, { { "out", "Out", Signal::audio } } },

            { "ring", "Ring Mod", 2, { P { "mix", "Mix", 0.0f, 100.0f, 100.0f, P::linear, "%" } },
              { { "a", "A", Signal::audio }, { "b", "B", Signal::audio } }, { { "out", "Out", Signal::audio } } },

            { "quant", "Quantizer", 2,
              { P { "scale", "Scale", 0.0f, 6.0f, 1.0f, P::stepped, "" }, P { "root", "Root", 0.0f, 11.0f, 0.0f, P::stepped, "" } },
              { { "in", "In", Signal::cv } }, { { "out", "Out", Signal::cv }, { "trig", "Trig", Signal::gate } } },

            { "slew", "Slew", 2,
              { P { "rise", "Rise", 0.0f, 2.0f, 0.1f, P::log, "s" }, P { "fall", "Fall", 0.0f, 2.0f, 0.1f, P::log, "s" } },
              { { "in", "In", Signal::cv } }, { { "out", "Out", Signal::cv } } },

            { "clkdiv", "Clock Divider", 2, {},
              { { "clock", "Clock", Signal::gate }, { "reset", "Reset", Signal::gate } },
              { { "d2", "/2", Signal::gate }, { "d3", "/3", Signal::gate }, { "d4", "/4", Signal::gate }, { "d8", "/8", Signal::gate } } },

            { "random", "Random", 2,
              { P { "rate", "Rate", 0.05f, 30.0f, 1.0f, P::log, "Hz" }, P { "smooth", "Smooth", 0.0f, 100.0f, 50.0f, P::linear, "%" } },
              { { "rate", "Rate", Signal::cv } }, { { "smooth", "Smooth", Signal::cv }, { "step", "Stepped", Signal::cv }, { "trig", "Trig", Signal::gate } } },

            { "macros", "Macros", 2, {}, {},
              { { "m1", "Macro 1", Signal::cv }, { "m2", "Macro 2", Signal::cv }, { "m3", "Macro 3", Signal::cv }, { "m4", "Macro 4", Signal::cv } } },

            { "output", "Output", 2, { P { "level", "Level", 0.0f, 100.0f, 70.0f, P::linear, "%" } },
              { { "l", "Left", Signal::audio }, { "r", "Right", Signal::audio } }, {}, true },
        };
        return all;
    }

    inline const Spec* findSpec (const char* type)
    {
        for (auto& s : catalogue())
            if (std::strcmp (type, s.type) == 0)
                return &s;
        return nullptr;
    }

    inline const Spec* findSpec (const std::string& type)     { return findSpec (type.c_str()); }

    inline int portIndex (const std::vector<Port>& ports, const char* id)
    {
        for (size_t i = 0; i < ports.size(); ++i)
            if (std::strcmp (id, ports[i].id) == 0)
                return (int) i;
        return -1;
    }

    inline int portIndex (const std::vector<Port>& ports, const std::string& id)   { return portIndex (ports, id.c_str()); }

    /** Rounds to the nearest whole number, halves to even. */
    inline int roundToInt (float v) noexcept     { return (int) std::nearbyint ((double) v); }


    //==============================================================================
    /** What every module can see while it runs. */
    struct Context
    {
        float sr = 48000.0f;
        const float* pitch = nullptr;   // per voice, V/oct (0 = C4), including bend
        const float* gate = nullptr;    // per voice, 0 or 1
        const float* vel = nullptr;     // per voice, 0..1
        float mod = 0.0f;               // mod wheel
        const float* macros = nullptr;  // four values, 0..1
        float accL = 0.0f, accR = 0.0f; // summed by Output modules
    };

    /** A module's processing: parameters (set from the message thread), outputs and state per voice. */
    struct Dsp
    {
        virtual ~Dsp() = default;
        virtual void prepare (float) {}
        virtual void process (int voice, const float* in, const bool* patched, Context& c) = 0;

        float param (int i) const noexcept      { return params[(size_t) i].load (std::memory_order_relaxed); }

        std::array<std::atomic<float>, maxParams> params {};
        std::array<std::array<float, maxPorts>, maxVoices> out {};
    };

    inline float voctToHz (float v)     { return 261.6256f * std::pow (2.0f, v); }

    struct MidiDsp : Dsp
    {
        std::array<float, maxVoices> slewed {};
        void process (int v, const float*, const bool*, Context& c) override
        {
            const float g = param (0);
            const float coeff = g <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (g * 0.35f * c.sr));
            slewed[(size_t) v] += (c.pitch[v] - slewed[(size_t) v]) * coeff;
            auto& o = out[(size_t) v];
            o[0] = slewed[(size_t) v]; o[1] = c.gate[v]; o[2] = c.vel[v]; o[3] = c.mod;
        }
    };

    struct VcoDsp : Dsp
    {
        std::array<SynthCore::Oscillator, maxVoices> osc;
        std::array<float, maxVoices> lastSync {};
        void process (int v, const float* in, const bool*, Context& c) override
        {
            auto& o = osc[(size_t) v];
            const float semis = param (0) * 12.0f + param (1) + param (2) / 100.0f;
            const float hz = voctToHz (semis / 12.0f + in[0] + in[1] + in[2] * param (4) / 100.0f * 2.0f);
            const float dt = dspmath::jlimit (1.0e-6f, 0.45f, hz / c.sr);
            if (in[4] > 0.5f && lastSync[(size_t) v] <= 0.5f) o.phase = 0.0f;
            lastSync[(size_t) v] = in[4];
            const float width = dspmath::jlimit (0.05f, 0.95f, param (3) / 100.0f + in[3] * 0.45f);
            auto& out_ = out[(size_t) v];
            out_[0] = std::sin (o.phase * SynthCore::twoPi);
            out_[1] = o.triAt (dt);
            out_[2] = o.sawAt (dt);
            out_[3] = o.pulseAt (width, dt);
            o.advance (dt);
        }
    };

    struct LfoDsp : Dsp
    {
        std::array<float, maxVoices> phase {}, lastReset {};
        void process (int v, const float* in, const bool*, Context& c) override
        {
            auto& p = phase[(size_t) v];
            if (in[1] > 0.5f && lastReset[(size_t) v] <= 0.5f) p = 0.0f;
            lastReset[(size_t) v] = in[1];
            p += param (0) * std::pow (2.0f, in[0] * 4.0f) / c.sr;
            p -= std::floor (p);
            auto& o = out[(size_t) v];
            o[0] = std::sin (p * SynthCore::twoPi);
            o[1] = 4.0f * std::abs (p - 0.5f) - 1.0f;
            o[2] = 2.0f * p - 1.0f;
            o[3] = p < 0.5f ? 1.0f : -1.0f;
        }
    };

    struct NoiseDsp : Dsp
    {
        std::array<SynthCore::Noise, maxVoices> gen;
        NoiseDsp() { for (size_t i = 0; i < gen.size(); ++i) gen[i].rng.setSeed ((std::int64_t) i * 977 + 31); }
        void process (int v, const float*, const bool*, Context&) override
        {
            out[(size_t) v][0] = gen[(size_t) v].white() * 0.7f;
            out[(size_t) v][1] = gen[(size_t) v].pink();
        }
    };

    struct FilterDsp : Dsp
    {
        std::array<SynthCore::MoogLadder, maxVoices> ladder;
        std::array<SynthCore::Svf, maxVoices> svf;
        std::array<int, maxVoices> counter {};
        void process (int v, const float* in, const bool*, Context& c) override
        {
            auto& l = ladder[(size_t) v];
            auto& s = svf[(size_t) v];
            if ((counter[(size_t) v]++ & 3) == 0)
            {
                const float oct = in[1] + in[2] * param (2) / 100.0f * 5.0f;
                const float fc = param (0) * std::pow (2.0f, oct);
                const float res = dspmath::jlimit (0.0f, 1.0f, param (1) / 100.0f + in[3]);
                l.set (fc, std::pow (res, 1.3f), c.sr);
                s.set (fc, res * 0.95f, c.sr);
            }
            const float x = in[0] * (0.6f + param (3) / 100.0f * 2.0f);
            auto& o = out[(size_t) v];
            o[0] = l.process (x);
            float lp, bp, hp;
            s.process (in[0], lp, bp, hp);
            o[1] = lp; o[2] = bp; o[3] = hp;
        }
    };

    struct VcaDsp : Dsp
    {
        void process (int v, const float* in, const bool* patched, Context&) override
        {
            float g = param (0) / 100.0f;
            if (patched[1])
            {
                const float cv = dspmath::jlimit (0.0f, 1.0f, in[1]);
                g *= param (1) > 0.5f ? cv * cv : cv;
            }
            out[(size_t) v][0] = in[0] * g;
        }
    };

    struct AdsrDsp : Dsp
    {
        std::array<SynthCore::AnalogEnvelope, maxVoices> env;
        std::array<float, maxVoices> lastGate {};
        void process (int v, const float* in, const bool*, Context& c) override
        {
            auto& e = env[(size_t) v];
            const bool high = in[0] > 0.5f, wasHigh = lastGate[(size_t) v] > 0.5f;
            if (high && ! wasHigh) e.gate (true);
            if (! high && wasHigh) e.gate (false);
            lastGate[(size_t) v] = in[0];
            const float value = e.process (param (0), param (1), param (2) / 100.0f, param (3), c.sr);
            out[(size_t) v][0] = value;
            out[(size_t) v][1] = -value;
        }
    };

    struct MixerDsp : Dsp
    {
        void process (int v, const float* in, const bool*, Context&) override
        {
            float s = 0.0f;
            for (int i = 0; i < 4; ++i) s += in[i] * param (i) / 100.0f;
            out[(size_t) v][0] = s;
        }
    };

    struct UtilDsp : Dsp
    {
        void process (int v, const float* in, const bool*, Context&) override
        {
            out[(size_t) v][0] = in[0] * param (0) / 100.0f + param (1);
        }
    };

    struct ShDsp : Dsp
    {
        std::array<float, maxVoices> held {}, lastTrig {}, slewed {};
        void process (int v, const float* in, const bool*, Context& c) override
        {
            if (in[1] > 0.5f && lastTrig[(size_t) v] <= 0.5f) held[(size_t) v] = in[0];
            lastTrig[(size_t) v] = in[1];
            const float s = param (0);
            const float coeff = s <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (s * 0.35f * c.sr));
            slewed[(size_t) v] += (held[(size_t) v] - slewed[(size_t) v]) * coeff;
            out[(size_t) v][0] = slewed[(size_t) v];
        }
    };

    struct SeqDsp : Dsp
    {
        std::array<int, maxVoices> step {};
        std::array<float, maxVoices> phase {}, lastClock {}, lastReset {};
        void process (int v, const float* in, const bool* patched, Context& c) override
        {
            auto& st = step[(size_t) v];
            if (in[1] > 0.5f && lastReset[(size_t) v] <= 0.5f) { st = 0; phase[(size_t) v] = 0.0f; }
            lastReset[(size_t) v] = in[1];

            float gate;
            if (patched[0])
            {
                if (in[0] > 0.5f && lastClock[(size_t) v] <= 0.5f) st = (st + 1) & 7;
                lastClock[(size_t) v] = in[0];
                gate = in[0] > 0.5f ? 1.0f : 0.0f;
            }
            else
            {
                auto& p = phase[(size_t) v];
                p += param (9) / c.sr;
                if (p >= 1.0f) { p -= 1.0f; st = (st + 1) & 7; }
                gate = p < param (8) / 100.0f ? 1.0f : 0.0f;
            }

            out[(size_t) v][0] = std::round (param (st)) / 12.0f;
            out[(size_t) v][1] = gate;
        }
    };

    struct DelayDsp : Dsp
    {
        std::array<std::vector<float>, maxVoices> line;
        std::array<int, maxVoices> pos {};
        void prepare (float sr) override
        {
            for (auto& l : line) l.assign ((size_t) (sr * 2.2f) + 4, 0.0f);
            pos.fill (0);
        }
        void process (int v, const float* in, const bool*, Context& c) override
        {
            auto& l = line[(size_t) v];
            if (l.empty()) { out[(size_t) v][0] = in[0]; return; }
            const int len = (int) l.size();
            const float delay = dspmath::jlimit (1.0f, (float) len - 2.0f, param (0) * 0.001f * c.sr * std::pow (2.0f, in[1]));
            auto& p = pos[(size_t) v];
            float rp = (float) p - delay; if (rp < 0.0f) rp += (float) len;
            if (rp >= (float) len) rp = 0.0f;   // (a tiny negative plus len rounds up to len itself)
            const int i0 = (int) rp, i1 = (i0 + 1) % len;
            const float frac = rp - (float) i0;
            const float wet = l[(size_t) i0] + frac * (l[(size_t) i1] - l[(size_t) i0]);
            l[(size_t) p] = std::tanh (in[0] + wet * param (1) / 100.0f);
            p = (p + 1) % len;
            const float mix = param (2) / 100.0f;
            out[(size_t) v][0] = in[0] * (1.0f - mix) + wet * mix;
        }
    };

    /** Wavefolding through a sine, so it folds smoothly (and the oversampling keeps it clean). */
    struct FoldDsp : Dsp
    {
        void process (int v, const float* in, const bool*, Context&) override
        {
            const float amount = dspmath::jlimit (0.0f, 1.0f, param (0) / 100.0f + in[1] * 0.5f);
            const float gain = 1.0f + 9.0f * amount * amount + 1.5f * amount;
            const float bias = param (1) / 100.0f * 0.8f;
            out[(size_t) v][0] = (std::sin (dspmath::MathConstants<float>::halfPi * (in[0] * gain + bias)) - std::sin (dspmath::MathConstants<float>::halfPi * bias)) * 0.8f;
        }
    };

    /** A low-pass gate: a vactrol (quick to open, slow to close) that opens a filter and an
        amplifier together. Strike it with a gate and it plucks. */
    struct LpgDsp : Dsp
    {
        std::array<float, maxVoices> vactrol {}, lastStrike {};
        std::array<SynthCore::Svf, maxVoices> svf;
        std::array<int, maxVoices> counter {};
        void process (int v, const float* in, const bool* patched, Context& c) override
        {
            auto& vac = vactrol[(size_t) v];
            float target = patched[1] ? dspmath::jlimit (0.0f, 1.0f, in[1]) : 0.0f;
            if (in[2] > 0.5f && lastStrike[(size_t) v] <= 0.5f) vac = 1.0f;     // struck: opens at once, then rings closed
            lastStrike[(size_t) v] = in[2];
            if (! patched[1] && ! patched[2]) target = 1.0f;
            const float riseT = 0.004f, fallT = param (1) * (0.4f + 0.6f * vac);    // closes slower as it gets darker, like a vactrol
            const float t = target > vac ? riseT : fallT;
            vac += (target - vac) * (1.0f - std::exp (-1.0f / (dspmath::jmax (0.0005f, t) * c.sr)));

            if ((counter[(size_t) v]++ & 3) == 0)
                svf[(size_t) v].set (30.0f * std::pow (2.0f, vac * 9.5f), 0.25f, c.sr);
            const float resp = param (0) / 100.0f;
            const float filtered = svf[(size_t) v].lowpass (in[0]);
            out[(size_t) v][0] = in[0] * vac * (1.0f - resp) + filtered * std::sqrt (vac) * resp;
        }
    };

    struct RingDsp : Dsp
    {
        void process (int v, const float* in, const bool*, Context&) override
        {
            const float mix = param (0) / 100.0f;
            out[(size_t) v][0] = in[0] * (1.0f - mix) + in[0] * in[1] * mix * 1.4f;
        }
    };

    /** Rounds a pitch (V/oct) to the nearest note of a scale; Trig fires when the note changes. */
    struct QuantDsp : Dsp
    {
        std::array<float, maxVoices> last {};
        std::array<int, maxVoices> trigLeft {};
        void process (int v, const float* in, const bool*, Context& c) override
        {
            static constexpr int scales[7][12] = {
                { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 },
                { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 }, { 0, 2, 3, 5, 7, 9, 10 }, { 0, 2, 4, 6, 8, 10 } };
            static constexpr int sizes[7] = { 12, 7, 7, 5, 5, 7, 6 };
            const int scale = dspmath::jlimit (0, 6, roundToInt (param (0)));
            const int root = roundToInt (param (1));
            const float semis = in[0] * 12.0f - (float) root;
            const float octave = std::floor (semis / 12.0f);
            const float within = semis - octave * 12.0f;
            float best = 0.0f, bestDist = 99.0f;
            for (int k = 0; k < sizes[scale]; ++k)
                for (int wrap : { -12, 0, 12 })
                {
                    const float cand = (float) (scales[scale][k] + wrap);
                    if (std::abs (cand - within) < bestDist) { bestDist = std::abs (cand - within); best = cand; }
                }
            const float q = (octave * 12.0f + best + (float) root) / 12.0f;
            if (std::abs (q - last[(size_t) v]) > 1.0e-4f) trigLeft[(size_t) v] = (int) (0.005f * c.sr);
            last[(size_t) v] = q;
            out[(size_t) v][0] = q;
            out[(size_t) v][1] = trigLeft[(size_t) v]-- > 0 ? 1.0f : 0.0f;
        }
    };

    struct SlewDsp : Dsp
    {
        std::array<float, maxVoices> value {};
        void process (int v, const float* in, const bool*, Context& c) override
        {
            auto& y = value[(size_t) v];
            const float t = in[0] > y ? param (0) : param (1);
            y += (in[0] - y) * (t <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (t * c.sr)));
            out[(size_t) v][0] = y;
        }
    };

    /** Passes every 2nd, 3rd, 4th and 8th clock pulse. */
    struct ClockDivDsp : Dsp
    {
        std::array<int, maxVoices> count {};
        std::array<float, maxVoices> lastClock {}, lastReset {};
        void process (int v, const float* in, const bool*, Context&) override
        {
            auto& n = count[(size_t) v];
            if (in[1] > 0.5f && lastReset[(size_t) v] <= 0.5f) n = 0;
            lastReset[(size_t) v] = in[1];
            if (in[0] > 0.5f && lastClock[(size_t) v] <= 0.5f) ++n;
            lastClock[(size_t) v] = in[0];
            const bool high = in[0] > 0.5f;
            static constexpr int div[] = { 2, 3, 4, 8 };
            for (int k = 0; k < 4; ++k)
                out[(size_t) v][(size_t) k] = high && ((n - 1) % div[k]) == 0 ? 1.0f : 0.0f;
        }
    };

    /** Random voltages: a new value Rate times a second, stepped and smoothly joined. */
    struct RandomDsp : Dsp
    {
        std::array<float, maxVoices> phase {}, prev {}, cur {};
        std::array<int, maxVoices> trigLeft {};
        std::array<SynthCore::Random, maxVoices> rng;
        RandomDsp() { for (size_t i = 0; i < rng.size(); ++i) rng[i].setSeed ((std::int64_t) i * 7919 + 13); }
        void process (int v, const float* in, const bool*, Context& c) override
        {
            auto& p = phase[(size_t) v];
            p += param (0) * std::pow (2.0f, in[0] * 4.0f) / c.sr;
            if (p >= 1.0f)
            {
                p -= std::floor (p);
                prev[(size_t) v] = cur[(size_t) v];
                cur[(size_t) v] = rng[(size_t) v].nextFloat() * 2.0f - 1.0f;
                trigLeft[(size_t) v] = (int) (0.005f * c.sr);
            }
            // Smooth: how much of each period is spent gliding to the new value (0 = a clean step)
            const float sm = param (1) / 100.0f;
            const float u = sm <= 0.001f ? 1.0f : dspmath::jmin (1.0f, p / sm);
            const float t = u * u * (3.0f - 2.0f * u);
            auto& o = out[(size_t) v];
            o[0] = prev[(size_t) v] + (cur[(size_t) v] - prev[(size_t) v]) * t;
            o[1] = cur[(size_t) v];
            o[2] = trigLeft[(size_t) v]-- > 0 ? 1.0f : 0.0f;
        }
    };

    struct MacrosDsp : Dsp
    {
        void process (int v, const float*, const bool*, Context& c) override
        {
            for (int i = 0; i < 4; ++i) out[(size_t) v][(size_t) i] = c.macros[i];
        }
    };

    struct OutputDsp : Dsp
    {
        void process (int, const float* in, const bool* patched, Context& c) override
        {
            const float g = param (0) / 100.0f;
            c.accL += in[0] * g;
            c.accR += (patched[1] ? in[1] : in[0]) * g;
        }
    };

    inline std::unique_ptr<Dsp> createDsp (const std::string& type)
    {
        if (type == "midi")     return std::make_unique<MidiDsp>();
        if (type == "vco")      return std::make_unique<VcoDsp>();
        if (type == "lfo")      return std::make_unique<LfoDsp>();
        if (type == "noise")    return std::make_unique<NoiseDsp>();
        if (type == "filter")   return std::make_unique<FilterDsp>();
        if (type == "vca")      return std::make_unique<VcaDsp>();
        if (type == "adsr")     return std::make_unique<AdsrDsp>();
        if (type == "mixer")    return std::make_unique<MixerDsp>();
        if (type == "util")     return std::make_unique<UtilDsp>();
        if (type == "sh")       return std::make_unique<ShDsp>();
        if (type == "seq")      return std::make_unique<SeqDsp>();
        if (type == "delay")    return std::make_unique<DelayDsp>();
        if (type == "fold")     return std::make_unique<FoldDsp>();
        if (type == "lpg")      return std::make_unique<LpgDsp>();
        if (type == "ring")     return std::make_unique<RingDsp>();
        if (type == "quant")    return std::make_unique<QuantDsp>();
        if (type == "slew")     return std::make_unique<SlewDsp>();
        if (type == "clkdiv")   return std::make_unique<ClockDivDsp>();
        if (type == "random")   return std::make_unique<RandomDsp>();
        if (type == "macros")   return std::make_unique<MacrosDsp>();
        if (type == "output")   return std::make_unique<OutputDsp>();
        return {};
    }

    //==============================================================================
    /** The patch, compiled for the audio thread: modules in processing order, inputs resolved. */
    struct Program
    {
        struct Node
        {
            Dsp* dsp = nullptr;
            int moduleId = -1;
            int numIns = 0;
            std::array<int, maxPorts> srcNode {}, srcPort {};
            std::array<bool, maxPorts> patched {};
        };
        std::vector<Node> nodes;
    };

    //==============================================================================
    // Factory patches
    struct PatchModule { const char* type; std::vector<std::pair<const char*, float>> params; };
    struct PatchCable  { int from; const char* out; int to; const char* in; };
    /** A macro as a patch uses it: its name, and the input it moves (module index and jack).
        Each macro goes through a Utility that makes it 0 at the macro's middle (50 %), so a patch
        sounds the same as without it until the macro is moved: `scale` is that Utility's Scale.
        `copies` > 1 sums that many copies in a Mixer first, for a wider range. */
    struct PatchMacro { const char* name; int to; const char* in; float scale; int copies = 1; };

    struct FactoryPatch
    {
        const char* name; int voices; std::vector<PatchModule> modules; std::vector<PatchCable> cables;
        std::vector<PatchMacro> macros;     // up to four: Macro 1, 2, 3, 4
    };

    inline const std::vector<FactoryPatch>& factoryPatches()
    {
        static const std::vector<FactoryPatch> all = {
            { "Basic Voice", 4,
              { { "midi", {} }, { "vco", {} }, { "filter", { { "cutoff", 600 }, { "res", 30 }, { "cvamt", 60 } } }, { "adsr", { { "d", 0.4f }, { "s", 50 } } },
                { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 1, "saw", 2, "in" }, { 0, "gate", 3, "gate" }, { 3, "env", 2, "cv" }, { 2, "lp24", 4, "in" },
                { 3, "env", 4, "cv" }, { 4, "out", 5, "l" } },
              { { "Cutoff", 2, "voct", 100, 4 }, { "Resonance", 2, "res", 60 } } },

            { "Dual VCO Bass", 1,
              { { "midi", { { "glide", 0.04f } } }, { "vco", { { "octave", -1 } } }, { "vco", { { "octave", -2 }, { "fine", 6 } } }, { "mixer", {} },
                { "filter", { { "cutoff", 300 }, { "res", 35 }, { "cvamt", 70 }, { "drive", 40 } } }, { "adsr", { { "d", 0.25f }, { "s", 20 }, { "r", 0.15f } } },
                { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 0, "pitch", 2, "pitch" }, { 1, "saw", 3, "in1" }, { 2, "square", 3, "in2" }, { 3, "out", 4, "in" },
                { 0, "gate", 5, "gate" }, { 5, "env", 4, "cv" }, { 4, "lp24", 6, "in" }, { 5, "env", 6, "cv" }, { 6, "out", 7, "l" } },
              { { "Cutoff", 4, "voct", 100, 4 }, { "Resonance", 4, "res", 70 }, { "Width", 2, "pw", 100 }, { "Detune", 2, "pitch2", 10 } } },

            { "Wobble", 1,
              { { "midi", {} }, { "vco", { { "octave", -1 } } }, { "lfo", { { "rate", 3.0f } } }, { "filter", { { "cutoff", 350 }, { "res", 55 }, { "cvamt", 45 } } },
                { "adsr", { { "s", 100 } } }, { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 1, "saw", 3, "in" }, { 2, "sine", 3, "cv" }, { 0, "gate", 4, "gate" }, { 3, "lp24", 5, "in" },
                { 4, "env", 5, "cv" }, { 5, "out", 6, "l" } },
              { { "Cutoff", 3, "voct", 100, 4 }, { "Resonance", 3, "res", 80 }, { "Rate", 2, "rate", 100 } } },

            { "S&H Bleeps", 1,
              { { "midi", {} }, { "noise", {} }, { "lfo", { { "rate", 8.0f } } }, { "sh", {} }, { "vco", {} }, { "filter", { { "cutoff", 2500 }, { "res", 40 } } },
                { "adsr", { { "s", 100 } } }, { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 4, "pitch" }, { 1, "white", 3, "in" }, { 2, "square", 3, "trig" }, { 3, "out", 4, "pitch2" }, { 4, "square", 5, "in" },
                { 0, "gate", 6, "gate" }, { 5, "lp12", 7, "in" }, { 6, "env", 7, "cv" }, { 7, "out", 8, "l" } },
              { { "Cutoff", 5, "voct", 100, 4 }, { "Resonance", 5, "res", 80 }, { "Rate", 2, "rate", 100 }, { "Width", 4, "pw", 100 } } },

            { "Sequenced Pluck", 1,
              { { "midi", {} }, { "seq", { { "rate", 7.0f }, { "len", 40 } } }, { "vco", {} }, { "filter", { { "cutoff", 400 }, { "res", 45 }, { "cvamt", 80 } } },
                { "adsr", { { "a", 0.002f }, { "d", 0.18f }, { "s", 0 }, { "r", 0.1f } } }, { "vca", {} }, { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 2, "pitch" }, { 1, "pitch", 2, "pitch2" }, { 2, "saw", 3, "in" }, { 1, "gate", 4, "gate" }, { 4, "env", 3, "cv" },
                { 3, "lp24", 5, "in" }, { 4, "env", 5, "cv" }, { 5, "out", 6, "in" }, { 0, "gate", 6, "cv" }, { 6, "out", 7, "l" } },
              { { "Cutoff", 3, "voct", 100, 4 }, { "Resonance", 3, "res", 90 } } },

            { "FM Bell", 4,
              { { "midi", {} }, { "vco", { { "fm", 45 } } }, { "vco", { { "octave", 1 }, { "tune", 2 } } },
                { "adsr", { { "a", 0.002f }, { "d", 2.0f }, { "s", 0 }, { "r", 1.5f } } }, { "vca", { { "exp", 1 } } }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 0, "pitch", 2, "pitch" }, { 2, "sine", 1, "fm" }, { 1, "sine", 4, "in" }, { 0, "gate", 3, "gate" },
                { 3, "env", 4, "cv" }, { 4, "out", 5, "l" } },
              { { "Ratio", 2, "pitch2", 100 } } },

            { "Delay Drone", 2,
              { { "midi", {} }, { "vco", {} }, { "vco", { { "fine", 9 }, { "pw", 30 } } }, { "mixer", {} }, { "lfo", { { "rate", 0.2f } } },
                { "filter", { { "cutoff", 900 }, { "res", 45 }, { "cvamt", 30 } } }, { "adsr", { { "a", 1.5f }, { "s", 80 }, { "r", 2.5f } } },
                { "vca", {} }, { "delay", { { "time", 450 }, { "fb", 60 }, { "mix", 40 } } }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 0, "pitch", 2, "pitch" }, { 1, "saw", 3, "in1" }, { 2, "square", 3, "in2" }, { 3, "out", 5, "in" },
                { 4, "tri", 5, "cv" }, { 0, "gate", 6, "gate" }, { 5, "lp24", 7, "in" }, { 6, "env", 7, "cv" }, { 7, "out", 8, "in" }, { 8, "out", 9, "l" } },
              { { "Cutoff", 5, "voct", 100, 4 }, { "Resonance", 5, "res", 90 }, { "Motion", 4, "rate", 100 }, { "Width", 2, "pw", 100 } } },

            { "West Coast Pluck", 4,
              { { "midi", {} }, { "vco", {} }, { "fold", { { "fold", 40 }, { "sym", 15 } } }, { "lpg", { { "resp", 70 }, { "decay", 0.45f } } },
                { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 1, "tri", 2, "in" }, { 0, "vel", 2, "cv" }, { 2, "out", 3, "in" }, { 0, "gate", 3, "strike" },
                { 3, "out", 4, "l" } },
              { } },

            { "Generative Garden", 1,
              { { "midi", {} }, { "random", { { "rate", 2.5f }, { "smooth", 0 } } }, { "quant", { { "scale", 4 }, { "root", 9 } } },
                { "vco", {} }, { "vco", { { "octave", 1 }, { "fine", 6 } } }, { "mixer", { { "l1", 70 }, { "l2", 35 } } },
                { "lpg", { { "resp", 65 }, { "decay", 0.9f } } }, { "delay", { { "time", 620 }, { "fb", 62 }, { "mix", 45 } } },
                { "adsr", { { "a", 1.2f }, { "s", 100 }, { "r", 3.0f } } }, { "vca", {} }, { "output", {} } },
              { { 1, "step", 2, "in" }, { 2, "out", 3, "pitch" }, { 2, "out", 4, "pitch" }, { 0, "pitch", 3, "pitch2" }, { 0, "pitch", 4, "pitch2" },
                { 3, "tri", 5, "in1" }, { 4, "sine", 5, "in2" }, { 5, "out", 6, "in" }, { 2, "trig", 6, "strike" }, { 6, "out", 7, "in" },
                { 0, "gate", 8, "gate" }, { 7, "out", 9, "in" }, { 8, "env", 9, "cv" }, { 9, "out", 10, "l" } },
              { { "Rate", 1, "rate", 100 }, { "Time", 7, "time", 100 } } },

            { "Ring Bells", 4,
              { { "midi", {} }, { "vco", {} }, { "vco", { { "octave", 1 }, { "tune", 7 } } }, { "ring", { { "mix", 85 } } },
                { "adsr", { { "a", 0.002f }, { "d", 1.8f }, { "s", 0 }, { "r", 1.5f } } }, { "vca", { { "exp", 1 } } },
                { "delay", { { "time", 380 }, { "fb", 45 }, { "mix", 30 } } }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 0, "pitch", 2, "pitch" }, { 1, "sine", 3, "a" }, { 2, "sine", 3, "b" }, { 0, "gate", 4, "gate" },
                { 3, "out", 5, "in" }, { 4, "env", 5, "cv" }, { 5, "out", 6, "in" }, { 6, "out", 7, "l" } },
              { { "Ratio", 2, "pitch2", 100 }, { "Time", 6, "time", 100 } } },

            { "Clock Garden", 1,
              { { "midi", {} }, { "lfo", { { "rate", 6.0f } } }, { "clkdiv", {} }, { "seq", {} }, { "quant", { { "scale", 3 }, { "root", 2 } } },
                { "vco", {} }, { "lpg", { { "resp", 60 }, { "decay", 0.3f } } }, { "delay", { { "time", 333 }, { "fb", 50 }, { "mix", 35 } } },
                { "output", {} }, { "adsr", { { "a", 0.05f }, { "s", 100 }, { "r", 1.5f } } }, { "vca", {} } },
              { { 1, "square", 2, "clock" }, { 2, "d2", 3, "clock" }, { 3, "pitch", 4, "in" }, { 4, "out", 5, "pitch" }, { 0, "pitch", 5, "pitch2" },
                { 5, "saw", 6, "in" }, { 2, "d3", 6, "strike" }, { 6, "out", 7, "in" }, { 0, "gate", 9, "gate" }, { 7, "out", 10, "in" },
                { 9, "env", 10, "cv" }, { 10, "out", 8, "l" } },
              { { "Tempo", 1, "rate", 100 }, { "Time", 7, "time", 100 } } },
        };
        return all;
    }

    /** What `name` is called in a factory patch's macro `macro` (0..3), or "" if the patch doesn't use it. */
    inline const char* macroName (const FactoryPatch& fp, int macro)
    {
        return macro >= 0 && macro < (int) fp.macros.size() ? fp.macros[(size_t) macro].name : "";
    }

    /**
        A factory patch with its macros patched in: a Macros module, a Utility for each macro
        (and a Mixer where a macro has copies), placed just before the Output module, and the
        cables to the inputs the macros move. Module indices after them move up to make room.
    */
    inline FactoryPatch withMacros (const FactoryPatch& fp)
    {
        if (fp.macros.empty())
            return fp;

        int outputAt = (int) fp.modules.size();
        for (int i = 0; i < (int) fp.modules.size(); ++i)
            if (std::strcmp (fp.modules[(size_t) i].type, "output") == 0) { outputAt = i; break; }

        std::vector<PatchModule> added;
        std::vector<PatchCable> cables;
        auto addModule = [&] (PatchModule m) { added.push_back (std::move (m)); return outputAt + (int) added.size() - 1; };

        const int macros = addModule ({ "macros", {} });
        static const char* outs[] = { "m1", "m2", "m3", "m4" };
        static const char* mixIns[] = { "in1", "in2", "in3", "in4" };
        static const char* mixLevels[] = { "l1", "l2", "l3", "l4" };
        std::vector<std::pair<int, const PatchMacro*>> targets;

        for (int i = 0; i < (int) fp.macros.size() && i < 4; ++i)
        {
            const auto& m = fp.macros[(size_t) i];
            // Exactly 0 when the macro is at 0.5: the Utility gives 0.5 * scale / 100 + offset
            const float offset = -((0.5f * m.scale) / 100.0f);
            const int util = addModule ({ "util", { { "scale", m.scale }, { "offset", offset } } });
            cables.push_back ({ macros, outs[i], util, "in" });

            int source = util;
            if (m.copies > 1)
            {
                PatchModule mixer { "mixer", {} };
                for (int k = 0; k < 4; ++k)
                    mixer.params.push_back ({ mixLevels[k], k < m.copies ? 100.0f : 0.0f });
                source = addModule (mixer);
                for (int k = 0; k < m.copies && k < 4; ++k)
                    cables.push_back ({ util, "out", source, mixIns[k] });
            }
            targets.push_back ({ source, &m });
        }

        const int shift = (int) added.size();
        auto moved = [&] (int index) { return index >= outputAt ? index + shift : index; };

        FactoryPatch result { fp.name, fp.voices, {}, {}, fp.macros };
        for (int i = 0; i < outputAt; ++i)                          result.modules.push_back (fp.modules[(size_t) i]);
        for (auto& m : added)                                       result.modules.push_back (m);
        for (int i = outputAt; i < (int) fp.modules.size(); ++i)   result.modules.push_back (fp.modules[(size_t) i]);

        for (auto& c : fp.cables)
            result.cables.push_back ({ moved (c.from), c.out, moved (c.to), c.in });
        for (auto& c : cables)
            result.cables.push_back (c);
        for (auto& [source, m] : targets)
            result.cables.push_back ({ source, "out", moved (m->to), m->in });
        return result;
    }

    //==============================================================================
    /** A patch as plain data, whoever keeps it: modules in rack order, each with its knobs in
        the order its Spec lists them, and the cables. */
    struct PatchDesc
    {
        struct Module { int id; std::string type; std::vector<float> params; };
        struct Cable  { int from; std::string out; int to; std::string in; };
        std::vector<Module> modules;
        std::vector<Cable> cables;
    };

    /** A factory patch (macros patched in) as a PatchDesc: modules get IDs 1, 2, 3... in order. */
    inline PatchDesc describe (const FactoryPatch& patch)
    {
        const auto fp = withMacros (patch);
        PatchDesc d;
        for (int i = 0; i < (int) fp.modules.size(); ++i)
        {
            const auto& pm = fp.modules[(size_t) i];
            PatchDesc::Module m { i + 1, pm.type, {} };
            if (auto* spec = findSpec (pm.type))
                for (auto& p : spec->params)
                {
                    float v = p.def;
                    for (auto& [id, value] : pm.params)
                        if (std::strcmp (id, p.id) == 0) v = value;
                    m.params.push_back (v);
                }
            d.modules.push_back (std::move (m));
        }
        for (auto& c : fp.cables)
            d.cables.push_back ({ c.from + 1, c.out, c.to + 1, c.in });
        return d;
    }

    //==============================================================================
    /**
        A running patch: the modules' processing, kept by module ID, and the Program that runs
        them. build() compiles a PatchDesc (allocating; not on the audio thread), keeping the
        processing state of the modules that are still there, and swaps the new Program in
        without stopping the audio. What it replaces is freed by release() once the audio
        thread can no longer be using it.
    */
    class Patch
    {
    public:
        Patch() = default;
        ~Patch()    { delete live.exchange (nullptr); }

        /** The rate the modules run at (twice the output rate). */
        void setRate (float processingRate)     { rate = processingRate; }

        void build (const PatchDesc& desc)
        {
            const auto& modules = desc.modules;

            // Modules: keep the processing state of the ones that are still there
            std::map<int, Entry> next;
            for (auto& m : modules)
            {
                auto it = dsps.find (m.id);
                if (it != dsps.end() && it->second.type == m.type)
                    next[m.id] = std::move (it->second);
                else if (auto d = createDsp (m.type))
                {
                    d->prepare (rate);
                    next[m.id] = { m.type, std::move (d) };
                }

                if (auto* spec = findSpec (m.type))
                    if (auto& d = next[m.id].dsp)
                        for (size_t i = 0; i < spec->params.size(); ++i)
                            d->params[i].store (i < m.params.size() ? m.params[i] : spec->params[i].def);
            }

            Retired gone;
            for (auto& [id, e] : dsps)
                if (e.dsp != nullptr)
                    gone.dsps.push_back (std::move (e.dsp));
            dsps = std::move (next);

            // Processing order: sources before the modules they feed (cycles fall back to rack order,
            // reading the previous sample)
            std::map<int, std::vector<int>> feeds;
            std::map<int, int> incoming;
            std::vector<const PatchDesc::Cable*> cables;
            for (auto& c : desc.cables)
                if (dsps.count (c.from) && dsps.count (c.to))
                    cables.push_back (&c);
            for (auto& m : modules) incoming[m.id] = 0;
            for (auto* c : cables)
            {
                feeds[c->from].push_back (c->to);
                ++incoming[c->to];
            }

            std::vector<int> order;
            std::vector<bool> placed (modules.size(), false);
            while (order.size() < modules.size())
            {
                bool progressed = false;
                for (size_t i = 0; i < modules.size(); ++i)
                {
                    const int id = modules[i].id;
                    if (! placed[i] && incoming[id] == 0)
                    {
                        placed[i] = true; order.push_back (id); progressed = true;
                        for (auto to : feeds[id]) --incoming[to];
                    }
                }
                if (! progressed)
                    for (size_t i = 0; i < modules.size(); ++i)
                        if (! placed[i])
                        {
                            const int id = modules[i].id;
                            placed[i] = true; order.push_back (id);
                            for (auto to : feeds[id]) --incoming[to];
                            break;
                        }
            }

            auto dspFor = [this] (int id) -> Dsp* { auto it = dsps.find (id); return it != dsps.end() ? it->second.dsp.get() : nullptr; };
            auto moduleFor = [&] (int id) -> const PatchDesc::Module* {
                for (auto& m : modules) if (m.id == id) return &m;
                return nullptr;
            };

            auto prog = std::make_unique<Program>();
            std::map<int, int> nodeIndex;
            for (auto id : order)
                if (dspFor (id) != nullptr)
                    nodeIndex[id] = (int) nodeIndex.size();

            for (auto id : order)
            {
                if (dspFor (id) == nullptr) continue;
                Program::Node n;
                n.dsp = dspFor (id);
                n.moduleId = id;
                auto* spec = findSpec (moduleFor (id)->type);
                n.numIns = (int) spec->ins.size();
                n.srcNode.fill (-1);
                n.srcPort.fill (0);
                for (auto* c : cables)
                {
                    if (c->to != id) continue;
                    const int in = portIndex (spec->ins, c->in);
                    auto* from = moduleFor (c->from);
                    auto* fromSpec = from != nullptr ? findSpec (from->type) : nullptr;
                    const int out = fromSpec != nullptr ? portIndex (fromSpec->outs, c->out) : -1;
                    if (in < 0 || out < 0) continue;
                    n.srcNode[(size_t) in] = nodeIndex[c->from];
                    n.srcPort[(size_t) in] = out;
                    n.patched[(size_t) in] = true;
                }
                prog->nodes.push_back (n);
            }

            gone.program.reset (live.exchange (prog.release()));
            gone.stamp = renderEnd.load();
            retired.push_back (std::move (gone));
            release();
        }

        /** Frees replaced programs and modules once the audio thread is done with them. */
        void release()
        {
            retired.erase (std::remove_if (retired.begin(), retired.end(), [this] (const Retired& r)
                                           { return renderEnd.load() > r.stamp || ! rendering.load(); }),
                           retired.end());
        }

        /**
            Starts every module afresh at a new rate, with its knobs as they are (not while
            the audio thread is using the patch). Afterwards the same notes render the same.
        */
        void reset (float processingRate)
        {
            rate = processingRate;
            std::map<Dsp*, Dsp*> fresh;
            for (auto& [id, e] : dsps)
            {
                if (e.dsp == nullptr) continue;
                auto d = createDsp (e.type);
                for (size_t i = 0; i < d->params.size(); ++i)
                    d->params[i].store (e.dsp->params[i].load());
                d->prepare (rate);
                fresh[e.dsp.get()] = d.get();
                e.dsp = std::move (d);
            }
            if (auto* p = live.load())
                for (auto& n : p->nodes)
                    n.dsp = fresh[n.dsp];
            retired.clear();
        }

        /** A module's processing by module ID, for setting its knobs (nullptr if there's none). */
        Dsp* module (int id) const
        {
            auto it = dsps.find (id);
            return it != dsps.end() ? it->second.dsp.get() : nullptr;
        }

        // The audio thread: begin() gives the Program to run (or nullptr), end() when it's done with it
        const Program* begin() noexcept     { rendering.store (true); return live.load(); }
        void end() noexcept                 { renderEnd.fetch_add (1); rendering.store (false); }

    private:
        struct Entry { std::string type; std::unique_ptr<Dsp> dsp; };
        struct Retired { std::unique_ptr<Program> program; std::vector<std::unique_ptr<Dsp>> dsps; std::uint64_t stamp = 0; };

        float rate = 96000.0f;
        std::map<int, Entry> dsps;
        std::atomic<Program*> live { nullptr };
        std::vector<Retired> retired;
        std::atomic<bool> rendering { false };
        std::atomic<std::uint64_t> renderEnd { 0 };
    };
} // namespace Modular
} // namespace elsewhere
