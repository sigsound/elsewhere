// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"
#include <map>

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Modular: a rack of modules joined by patch cables.

    The patch lives in the plugin's state (a PATCH tree of MODULE and CABLE children), so
    it's saved with the project, undoable, and synced to collaborators like everything else.
    Module knobs are properties of their MODULE; four Macro parameters bring automation,
    modulation and MIDI mapping into the patch through the Macros module.

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

    inline const Spec* findSpec (const juce::String& type)
    {
        for (auto& s : catalogue())
            if (type == s.type)
                return &s;
        return nullptr;
    }

    inline int portIndex (const std::vector<Port>& ports, const juce::String& id)
    {
        for (size_t i = 0; i < ports.size(); ++i)
            if (id == ports[i].id)
                return (int) i;
        return -1;
    }

    inline juce::String paramProperty (const ParamSpec& p)     { return juce::String ("p_") + p.id; }

    inline juce::StringArray scaleNames()   { return { "Chromatic", "Major", "Minor", "Major Pent", "Minor Pent", "Dorian", "Whole Tone" }; }

    inline juce::String formatValue (const ParamSpec& p, float v)
    {
        const juce::String unit (p.unit);
        if (juce::String (p.id) == "exp")   return v > 0.5f ? "Exponential" : "Linear";
        if (juce::String (p.id) == "scale") return scaleNames()[juce::jlimit (0, scaleNames().size() - 1, juce::roundToInt (v))];
        if (juce::String (p.id) == "root")  return juce::MidiMessage::getMidiNoteName (juce::roundToInt (v), true, false, 4);
        if (unit == "Hz")   return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " kHz" : juce::String (v, v < 10.0f ? 2 : 0) + " Hz";
        if (unit == "s")    return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s";
        if (unit == "ms")   return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " s" : juce::String (juce::roundToInt (v)) + " ms";
        if (unit == "st" || unit == "oct") return (v > 0.0f ? "+" : "") + juce::String (p.kind == ParamSpec::stepped ? juce::String (juce::roundToInt (v)) : juce::String (v, 2)) + " " + unit;
        if (unit == "ct")   return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct";
        if (unit == "V")    return (v > 0.0f ? "+" : "") + juce::String (v, 2) + " V";
        if (unit == "%")    return juce::String (juce::roundToInt (v)) + " %";
        return juce::String (v, 2);
    }

    inline juce::NormalisableRange<double> rangeFor (const ParamSpec& p)
    {
        juce::NormalisableRange<double> r (p.min, p.max, p.kind == ParamSpec::stepped ? 1.0 : 0.0);
        if (p.kind == ParamSpec::log)
            r.setSkewForCentre (p.min > 0.0f ? std::sqrt ((double) p.min * p.max) : p.max * 0.1);
        return r;
    }

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
            const float dt = juce::jlimit (1.0e-6f, 0.45f, hz / c.sr);
            if (in[4] > 0.5f && lastSync[(size_t) v] <= 0.5f) o.phase = 0.0f;
            lastSync[(size_t) v] = in[4];
            const float width = juce::jlimit (0.05f, 0.95f, param (3) / 100.0f + in[3] * 0.45f);
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
        NoiseDsp() { for (size_t i = 0; i < gen.size(); ++i) gen[i].rng.setSeed ((juce::int64) i * 977 + 31); }
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
                const float res = juce::jlimit (0.0f, 1.0f, param (1) / 100.0f + in[3]);
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
                const float cv = juce::jlimit (0.0f, 1.0f, in[1]);
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
            const float delay = juce::jlimit (1.0f, (float) len - 2.0f, param (0) * 0.001f * c.sr * std::pow (2.0f, in[1]));
            auto& p = pos[(size_t) v];
            float rp = (float) p - delay; if (rp < 0.0f) rp += (float) len;
            const int i0 = (int) rp, i1 = (i0 + 1) % len;
            const float frac = rp - (float) i0;
            const float wet = l[(size_t) i0] + frac * (l[(size_t) i1] - l[(size_t) i0]);
            l[(size_t) p] = std::tanh (in[0] + wet * param (1) / 100.0f);
            p = (p + 1) % len;
            const float mix = param (2) / 100.0f;
            out[(size_t) v][0] = in[0] * (1.0f - mix) + wet * mix;
        }
    };

    /** West Coast wavefolding: a sine folder, so it folds smoothly (and the oversampling keeps it clean). */
    struct FoldDsp : Dsp
    {
        void process (int v, const float* in, const bool*, Context&) override
        {
            const float amount = juce::jlimit (0.0f, 1.0f, param (0) / 100.0f + in[1] * 0.5f);
            const float gain = 1.0f + 9.0f * amount * amount + 1.5f * amount;
            const float bias = param (1) / 100.0f * 0.8f;
            out[(size_t) v][0] = (std::sin (juce::MathConstants<float>::halfPi * (in[0] * gain + bias)) - std::sin (juce::MathConstants<float>::halfPi * bias)) * 0.8f;
        }
    };

    /** A Buchla-style low-pass gate: a vactrol (quick to open, slow to close) that opens a filter
        and an amplifier together. Strike it with a gate for the classic pluck. */
    struct LpgDsp : Dsp
    {
        std::array<float, maxVoices> vactrol {}, lastStrike {};
        std::array<SynthCore::Svf, maxVoices> svf;
        std::array<int, maxVoices> counter {};
        void process (int v, const float* in, const bool* patched, Context& c) override
        {
            auto& vac = vactrol[(size_t) v];
            float target = patched[1] ? juce::jlimit (0.0f, 1.0f, in[1]) : 0.0f;
            if (in[2] > 0.5f && lastStrike[(size_t) v] <= 0.5f) vac = 1.0f;     // struck: opens at once, then rings closed
            lastStrike[(size_t) v] = in[2];
            if (! patched[1] && ! patched[2]) target = 1.0f;
            const float riseT = 0.004f, fallT = param (1) * (0.4f + 0.6f * vac);    // closes slower as it gets darker, like a vactrol
            const float t = target > vac ? riseT : fallT;
            vac += (target - vac) * (1.0f - std::exp (-1.0f / (juce::jmax (0.0005f, t) * c.sr)));

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
            static const std::vector<std::vector<int>> scales = {
                { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 }, { 0, 2, 4, 5, 7, 9, 11 }, { 0, 2, 3, 5, 7, 8, 10 },
                { 0, 2, 4, 7, 9 }, { 0, 3, 5, 7, 10 }, { 0, 2, 3, 5, 7, 9, 10 }, { 0, 2, 4, 6, 8, 10 } };
            const auto& sc = scales[(size_t) juce::jlimit (0, 6, juce::roundToInt (param (0)))];
            const int root = juce::roundToInt (param (1));
            const float semis = in[0] * 12.0f - (float) root;
            const float octave = std::floor (semis / 12.0f);
            const float within = semis - octave * 12.0f;
            float best = 0.0f, bestDist = 99.0f;
            for (int d : sc)
                for (int wrap : { -12, 0, 12 })
                {
                    const float cand = (float) (d + wrap);
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
        std::array<juce::Random, maxVoices> rng;
        RandomDsp() { for (size_t i = 0; i < rng.size(); ++i) rng[i].setSeed ((juce::int64) i * 7919 + 13); }
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
            const float u = sm <= 0.001f ? 1.0f : juce::jmin (1.0f, p / sm);
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

    inline std::unique_ptr<Dsp> createDsp (const juce::String& type)
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
    struct FactoryPatch { const char* name; int voices; std::vector<PatchModule> modules; std::vector<PatchCable> cables; };

    inline const std::vector<FactoryPatch>& factoryPatches()
    {
        static const std::vector<FactoryPatch> all = {
            { "Basic Voice", 4,
              { { "midi", {} }, { "vco", {} }, { "filter", { { "cutoff", 600 }, { "res", 30 }, { "cvamt", 60 } } }, { "adsr", { { "d", 0.4f }, { "s", 50 } } },
                { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 1, "saw", 2, "in" }, { 0, "gate", 3, "gate" }, { 3, "env", 2, "cv" }, { 2, "lp24", 4, "in" },
                { 3, "env", 4, "cv" }, { 4, "out", 5, "l" } } },

            { "Dual VCO Bass", 1,
              { { "midi", { { "glide", 0.04f } } }, { "vco", { { "octave", -1 } } }, { "vco", { { "octave", -2 }, { "fine", 6 } } }, { "mixer", {} },
                { "filter", { { "cutoff", 300 }, { "res", 35 }, { "cvamt", 70 }, { "drive", 40 } } }, { "adsr", { { "d", 0.25f }, { "s", 20 }, { "r", 0.15f } } },
                { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 0, "pitch", 2, "pitch" }, { 1, "saw", 3, "in1" }, { 2, "square", 3, "in2" }, { 3, "out", 4, "in" },
                { 0, "gate", 5, "gate" }, { 5, "env", 4, "cv" }, { 4, "lp24", 6, "in" }, { 5, "env", 6, "cv" }, { 6, "out", 7, "l" } } },

            { "Wobble", 1,
              { { "midi", {} }, { "vco", { { "octave", -1 } } }, { "lfo", { { "rate", 3.0f } } }, { "filter", { { "cutoff", 350 }, { "res", 55 }, { "cvamt", 45 } } },
                { "adsr", { { "s", 100 } } }, { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 1, "saw", 3, "in" }, { 2, "sine", 3, "cv" }, { 0, "gate", 4, "gate" }, { 3, "lp24", 5, "in" },
                { 4, "env", 5, "cv" }, { 5, "out", 6, "l" } } },

            { "S&H Bleeps", 1,
              { { "midi", {} }, { "noise", {} }, { "lfo", { { "rate", 8.0f } } }, { "sh", {} }, { "vco", {} }, { "filter", { { "cutoff", 2500 }, { "res", 40 } } },
                { "adsr", { { "s", 100 } } }, { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 4, "pitch" }, { 1, "white", 3, "in" }, { 2, "square", 3, "trig" }, { 3, "out", 4, "pitch2" }, { 4, "square", 5, "in" },
                { 0, "gate", 6, "gate" }, { 5, "lp12", 7, "in" }, { 6, "env", 7, "cv" }, { 7, "out", 8, "l" } } },

            { "Sequenced Pluck", 1,
              { { "midi", {} }, { "seq", { { "rate", 7.0f }, { "len", 40 } } }, { "vco", {} }, { "filter", { { "cutoff", 400 }, { "res", 45 }, { "cvamt", 80 } } },
                { "adsr", { { "a", 0.002f }, { "d", 0.18f }, { "s", 0 }, { "r", 0.1f } } }, { "vca", {} }, { "vca", {} }, { "output", {} } },
              { { 0, "pitch", 2, "pitch" }, { 1, "pitch", 2, "pitch2" }, { 2, "saw", 3, "in" }, { 1, "gate", 4, "gate" }, { 4, "env", 3, "cv" },
                { 3, "lp24", 5, "in" }, { 4, "env", 5, "cv" }, { 5, "out", 6, "in" }, { 0, "gate", 6, "cv" }, { 6, "out", 7, "l" } } },

            { "FM Bell", 4,
              { { "midi", {} }, { "vco", { { "fm", 45 } } }, { "vco", { { "octave", 1 }, { "tune", 2 } } },
                { "adsr", { { "a", 0.002f }, { "d", 2.0f }, { "s", 0 }, { "r", 1.5f } } }, { "vca", { { "exp", 1 } } }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 0, "pitch", 2, "pitch" }, { 2, "sine", 1, "fm" }, { 1, "sine", 4, "in" }, { 0, "gate", 3, "gate" },
                { 3, "env", 4, "cv" }, { 4, "out", 5, "l" } } },

            { "Delay Drone", 2,
              { { "midi", {} }, { "vco", {} }, { "vco", { { "fine", 9 }, { "pw", 30 } } }, { "mixer", {} }, { "lfo", { { "rate", 0.2f } } },
                { "filter", { { "cutoff", 900 }, { "res", 45 }, { "cvamt", 30 } } }, { "adsr", { { "a", 1.5f }, { "s", 80 }, { "r", 2.5f } } },
                { "vca", {} }, { "delay", { { "time", 450 }, { "fb", 60 }, { "mix", 40 } } }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 0, "pitch", 2, "pitch" }, { 1, "saw", 3, "in1" }, { 2, "square", 3, "in2" }, { 3, "out", 5, "in" },
                { 4, "tri", 5, "cv" }, { 0, "gate", 6, "gate" }, { 5, "lp24", 7, "in" }, { 6, "env", 7, "cv" }, { 7, "out", 8, "in" }, { 8, "out", 9, "l" } } },

            { "West Coast Pluck", 4,
              { { "midi", {} }, { "vco", {} }, { "fold", { { "fold", 40 }, { "sym", 15 } } }, { "lpg", { { "resp", 70 }, { "decay", 0.45f } } },
                { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 1, "tri", 2, "in" }, { 0, "vel", 2, "cv" }, { 2, "out", 3, "in" }, { 0, "gate", 3, "strike" },
                { 3, "out", 4, "l" } } },

            { "Generative Garden", 1,
              { { "midi", {} }, { "random", { { "rate", 2.5f }, { "smooth", 0 } } }, { "quant", { { "scale", 4 }, { "root", 9 } } },
                { "vco", {} }, { "vco", { { "octave", 1 }, { "fine", 6 } } }, { "mixer", { { "l1", 70 }, { "l2", 35 } } },
                { "lpg", { { "resp", 65 }, { "decay", 0.9f } } }, { "delay", { { "time", 620 }, { "fb", 62 }, { "mix", 45 } } },
                { "adsr", { { "a", 1.2f }, { "s", 100 }, { "r", 3.0f } } }, { "vca", {} }, { "output", {} } },
              { { 1, "step", 2, "in" }, { 2, "out", 3, "pitch" }, { 2, "out", 4, "pitch" }, { 0, "pitch", 3, "pitch2" }, { 0, "pitch", 4, "pitch2" },
                { 3, "tri", 5, "in1" }, { 4, "sine", 5, "in2" }, { 5, "out", 6, "in" }, { 2, "trig", 6, "strike" }, { 6, "out", 7, "in" },
                { 0, "gate", 8, "gate" }, { 7, "out", 9, "in" }, { 8, "env", 9, "cv" }, { 9, "out", 10, "l" } } },

            { "Ring Bells", 4,
              { { "midi", {} }, { "vco", {} }, { "vco", { { "octave", 1 }, { "tune", 7 } } }, { "ring", { { "mix", 85 } } },
                { "adsr", { { "a", 0.002f }, { "d", 1.8f }, { "s", 0 }, { "r", 1.5f } } }, { "vca", { { "exp", 1 } } },
                { "delay", { { "time", 380 }, { "fb", 45 }, { "mix", 30 } } }, { "output", {} } },
              { { 0, "pitch", 1, "pitch" }, { 0, "pitch", 2, "pitch" }, { 1, "sine", 3, "a" }, { 2, "sine", 3, "b" }, { 0, "gate", 4, "gate" },
                { 3, "out", 5, "in" }, { 4, "env", 5, "cv" }, { 5, "out", 6, "in" }, { 6, "out", 7, "l" } } },

            { "Clock Garden", 1,
              { { "midi", {} }, { "lfo", { { "rate", 6.0f } } }, { "clkdiv", {} }, { "seq", {} }, { "quant", { { "scale", 3 }, { "root", 2 } } },
                { "vco", {} }, { "lpg", { { "resp", 60 }, { "decay", 0.3f } } }, { "delay", { { "time", 333 }, { "fb", 50 }, { "mix", 35 } } },
                { "output", {} }, { "adsr", { { "a", 0.05f }, { "s", 100 }, { "r", 1.5f } } }, { "vca", {} } },
              { { 1, "square", 2, "clock" }, { 2, "d2", 3, "clock" }, { 3, "pitch", 4, "in" }, { 4, "out", 5, "pitch" }, { 0, "pitch", 5, "pitch2" },
                { 5, "saw", 6, "in" }, { 2, "d3", 6, "strike" }, { 6, "out", 7, "in" }, { 0, "gate", 9, "gate" }, { 7, "out", 10, "in" },
                { 9, "env", 10, "cv" }, { 10, "out", 8, "l" } } },
        };
        return all;
    }

    inline const juce::Identifier patchId { "PATCH" }, moduleId { "MODULE" }, cableId { "CABLE" };
}

//==============================================================================
class ModularDevice : public ElsewhereDevice,
                      private juce::Timer
{
public:
    static constexpr const char* xmlTypeName = "elsewhereModular";

    ModularDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        voices = addParameter ("voices", "Voices", 1.0f, stepped (1, Modular::maxVoices), [] (float v) { return juce::String (juce::roundToInt (v)); });
        volume = addParameter ("volume", "Volume", -6.0f, { -60.0f, 6.0f }, dbText);
        for (int i = 0; i < 4; ++i)
            macros[(size_t) i] = addParameter ("macro" + juce::String (i + 1), "Macro " + juce::String (i + 1), 0.5f, { 0.0f, 1.0f }, percentFromUnit);

        if (! state.getChildWithName (Modular::patchId).isValid() || state.getChildWithName (Modular::patchId).getNumChildren() == 0)
            buildPatch (Modular::factoryPatches()[0], nullptr);

        state.addListener (&watcher);
        rebuild();
        startTimer (500);
    }

    ~ModularDevice() override
    {
        stopTimer();
        state.removeListener (&watcher);
        delete program.exchange (nullptr);
    }

    juce::String getName() const override              { return "Elsewhere Modular"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Modular"; }
    int getDeviceViewWidth() const override            { return 1400; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 4.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }

    juce::ValueTree getPatch()                          { return state.getOrCreateChildWithName (Modular::patchId, nullptr); }
    juce::UndoManager* getUndo()                        { return &edit.getUndoManager(); }

    /** Presets copy the patch too, not just the parameters. */
    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        ElsewhereDevice::restorePluginStateFromValueTree (v);
        auto incoming = v.getChildWithName (Modular::patchId);
        if (incoming.isValid())
            getPatch().copyPropertiesAndChildrenFrom (incoming, getUndo());
    }

    //==============================================================================
    // Editing the patch (message thread; all undoable)
    int nextModuleId()
    {
        auto patch = getPatch();
        int id = 1;
        for (auto m : patch)
            if (m.hasType (Modular::moduleId))
                id = juce::jmax (id, (int) m["id"] + 1);
        return id;
    }

    juce::ValueTree addModule (const juce::String& type, juce::UndoManager* um)
    {
        auto* spec = Modular::findSpec (type);
        if (spec == nullptr) return {};

        // New modules go at the end of the rack, just before Output
        auto list = modulesInOrder();
        int insertAt = (int) list.size();
        if (! list.empty() && list.back()["type"].toString() == "output")
            insertAt = (int) list.size() - 1;

        juce::ValueTree m (Modular::moduleId);
        m.setProperty ("id", nextModuleId(), nullptr);
        m.setProperty ("type", type, nullptr);
        m.setProperty ("pos", (int) list.size(), nullptr);
        for (auto& p : spec->params)
            m.setProperty (Modular::paramProperty (p), p.def, nullptr);
        getPatch().appendChild (m, um);
        moveModule ((int) m["id"], insertAt, um);
        return m;
    }

    void removeModule (int id, juce::UndoManager* um)
    {
        auto patch = getPatch();
        for (int i = patch.getNumChildren(); --i >= 0;)
        {
            auto c = patch.getChild (i);
            if ((c.hasType (Modular::moduleId) && (int) c["id"] == id)
                || (c.hasType (Modular::cableId) && ((int) c["from"] == id || (int) c["to"] == id)))
                patch.removeChild (i, um);
        }
        normalisePositions (um);
    }

    void connect (int fromId, const juce::String& out, int toId, const juce::String& in, juce::UndoManager* um)
    {
        disconnectInput (toId, in, um);
        juce::ValueTree c (Modular::cableId);
        c.setProperty ("from", fromId, nullptr);
        c.setProperty ("out", out, nullptr);
        c.setProperty ("to", toId, nullptr);
        c.setProperty ("in", in, nullptr);
        getPatch().appendChild (c, um);
    }

    void disconnectInput (int toId, const juce::String& in, juce::UndoManager* um)
    {
        auto patch = getPatch();
        for (int i = patch.getNumChildren(); --i >= 0;)
        {
            auto c = patch.getChild (i);
            if (c.hasType (Modular::cableId) && (int) c["to"] == toId && c["in"].toString() == in)
                patch.removeChild (i, um);
        }
    }

    /** Modules in rack order. */
    std::vector<juce::ValueTree> modulesInOrder()
    {
        std::vector<juce::ValueTree> list;
        for (auto m : getPatch())
            if (m.hasType (Modular::moduleId))
                list.push_back (m);
        std::stable_sort (list.begin(), list.end(), [] (const juce::ValueTree& a, const juce::ValueTree& b) { return (int) a["pos"] < (int) b["pos"]; });
        return list;
    }

    void normalisePositions (juce::UndoManager* um)
    {
        int i = 0;
        for (auto& m : modulesInOrder())
            if ((int) m["pos"] != i)
                m.setProperty ("pos", i++, um);
            else
                ++i;
    }

    /** Moves a module to a new place in the rack. */
    void moveModule (int id, int newIndex, juce::UndoManager* um)
    {
        auto list = modulesInOrder();
        auto it = std::find_if (list.begin(), list.end(), [id] (const juce::ValueTree& m) { return (int) m["id"] == id; });
        if (it == list.end()) return;
        auto m = *it;
        list.erase (it);
        newIndex = juce::jlimit (0, (int) list.size(), newIndex);
        list.insert (list.begin() + newIndex, m);
        for (int i = 0; i < (int) list.size(); ++i)
            if ((int) list[(size_t) i]["pos"] != i)
                list[(size_t) i].setProperty ("pos", i, um);
    }

    //==============================================================================
    static const std::vector<Modular::FactoryPatch>& patches()     { return Modular::factoryPatches(); }

    void loadPatch (int index)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);
        edit.getUndoManager().beginNewTransaction ("Load Modular patch");
        buildPatch (patches()[(size_t) index], getUndo());
        voices.param().setParameter ((float) patches()[(size_t) index].voices, juce::sendNotification);
        state.setProperty ("patch", index, getUndo());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", -1); }

    //==============================================================================
    void initialise (const te::PluginInitialisationInfo& info) override
    {
        rate = (float) info.sampleRate;
        // The patch runs at twice the sample rate, so FM, sync, wavefolding and the filter's
        // saturation stay clean of aliasing
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversampler->initProcessing ((size_t) maxChunk);
        work.setSize (2, maxChunk);
        work.clear();
        for (auto& [id, d] : dsps)
            d->prepare (rate * 2.0f);
        held.clear();
        for (auto& v : voiceState) v = {};
        noteCounter = 0;
    }

    void midiPanic() override
    {
        held.clear();
        for (auto& v : voiceState) { v.gate = 0.0f; v.held = false; }
    }

    double getLatencySeconds() override     { return oversampler != nullptr ? oversampler->getLatencyInSamples() / rate : 0.0; }

    /** Listens to one output jack (voice 1) for the hover scope; -1 to stop. */
    void setProbe (int moduleId, int port)  { probePort = port; probeModule = moduleId; }
    SynthCore::ScopeBuffer probeScope;

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr || oversampler == nullptr)
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

    SynthCore::ScopeBuffer scope;
    float getSamplesPerCycle() const    { return samplesPerCycle.load(); }

    Param voices, volume;
    std::array<Param, 4> macros;

private:
    struct VoiceState { int note = -1; float pitchV = 0.0f, gate = 0.0f, vel = 0.0f; bool held = false; juce::uint32 age = 0; };

    float rate = 48000.0f;
    static constexpr int maxChunk = 256;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> work;
    std::atomic<int> probeModule { -1 }, probePort { -1 };
    std::map<int, std::unique_ptr<Modular::Dsp>> dsps;
    std::atomic<Modular::Program*> program { nullptr };
    std::vector<std::pair<double, std::unique_ptr<Modular::Program>>> oldPrograms;
    std::vector<std::pair<double, std::unique_ptr<Modular::Dsp>>> oldDsps;
    std::array<VoiceState, Modular::maxVoices> voiceState;
    std::vector<int> held;
    juce::uint32 noteCounter = 0;
    float bend = 0.0f, modWheel = 0.0f;
    std::atomic<float> samplesPerCycle { 200.0f };

    static juce::String percentFromUnit (float v)  { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; }

    void buildPatch (const Modular::FactoryPatch& fp, juce::UndoManager* um)
    {
        auto patch = state.getOrCreateChildWithName (Modular::patchId, um);
        patch.removeAllChildren (um);

        for (int i = 0; i < (int) fp.modules.size(); ++i)
        {
            const auto& pm = fp.modules[(size_t) i];
            auto* spec = Modular::findSpec (pm.type);
            juce::ValueTree m (Modular::moduleId);
            m.setProperty ("id", i + 1, nullptr);
            m.setProperty ("type", pm.type, nullptr);
            m.setProperty ("pos", i, nullptr);
            for (auto& p : spec->params)
            {
                float v = p.def;
                for (auto& [id, value] : pm.params)
                    if (juce::String (id) == p.id) v = value;
                m.setProperty (Modular::paramProperty (p), v, nullptr);
            }
            patch.appendChild (m, um);
        }

        for (auto& c : fp.cables)
        {
            juce::ValueTree cable (Modular::cableId);
            cable.setProperty ("from", c.from + 1, nullptr);
            cable.setProperty ("out", c.out, nullptr);
            cable.setProperty ("to", c.to + 1, nullptr);
            cable.setProperty ("in", c.in, nullptr);
            patch.appendChild (cable, um);
        }
    }

    //==============================================================================
    // Patch -> program (message thread). te::Plugin is already a ValueTree listener, so the
    // patch is watched by a small helper object instead.
    struct Watcher : juce::ValueTree::Listener
    {
        ModularDevice& owner;
        explicit Watcher (ModularDevice& o) : owner (o) {}
        void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& p) override        { owner.patchPropertyChanged (t, p); }
        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&) override                 { if (owner.isPatchTree (parent)) owner.rebuild(); }
        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override          { if (owner.isPatchTree (parent)) owner.rebuild(); }
        void valueTreeChildOrderChanged (juce::ValueTree& parent, int, int) override                  { if (owner.isPatchTree (parent)) owner.rebuild(); }
        void valueTreeRedirected (juce::ValueTree&) override                                          { owner.rebuild(); }
    };
    Watcher watcher { *this };

    void patchPropertyChanged (juce::ValueTree& t, const juce::Identifier& prop)
    {
        if (t.hasType (Modular::moduleId) && prop.toString().startsWith ("p_"))
        {
            setParamFromTree (t, prop);
            return;
        }
        if (t.hasType (Modular::moduleId) || t.hasType (Modular::cableId))
            rebuild();
    }

    bool isPatchTree (const juce::ValueTree& t) const   { return t.hasType (Modular::patchId) || t == state; }

    void setParamFromTree (const juce::ValueTree& m, const juce::Identifier& prop)
    {
        auto it = dsps.find ((int) m["id"]);
        auto* spec = Modular::findSpec (m["type"].toString());
        if (it == dsps.end() || spec == nullptr) return;
        for (size_t i = 0; i < spec->params.size(); ++i)
            if (prop.toString() == Modular::paramProperty (spec->params[i]))
                it->second->params[i].store ((float) m[prop], std::memory_order_relaxed);
    }

    void rebuild()
    {
        auto modules = modulesInOrder();

        // Modules: keep the processing state of the ones that are still there
        std::map<int, std::unique_ptr<Modular::Dsp>> next;
        for (auto& m : modules)
        {
            const int id = (int) m["id"];
            auto it = dsps.find (id);
            if (it != dsps.end())
                next[id] = std::move (it->second);
            else if (auto d = Modular::createDsp (m["type"].toString()))
            {
                d->prepare (rate * 2.0f);
                next[id] = std::move (d);
            }

            if (auto* spec = Modular::findSpec (m["type"].toString()))
                if (auto& d = next[id])
                    for (size_t i = 0; i < spec->params.size(); ++i)
                        d->params[i].store ((float) m.getProperty (Modular::paramProperty (spec->params[i]), spec->params[i].def));
        }

        const double now = juce::Time::getMillisecondCounterHiRes();
        for (auto& [id, d] : dsps)
            if (d != nullptr)
                oldDsps.push_back ({ now, std::move (d) });
        dsps = std::move (next);

        // Processing order: sources before the modules they feed (cycles fall back to rack order,
        // reading the previous sample)
        std::map<int, std::vector<int>> feeds;
        std::map<int, int> incoming;
        std::vector<juce::ValueTree> cables;
        for (auto c : getPatch())
            if (c.hasType (Modular::cableId) && dsps.count ((int) c["from"]) && dsps.count ((int) c["to"]))
                cables.push_back (c);
        for (auto& m : modules) incoming[(int) m["id"]] = 0;
        for (auto& c : cables)
        {
            feeds[(int) c["from"]].push_back ((int) c["to"]);
            ++incoming[(int) c["to"]];
        }

        std::vector<int> order;
        std::vector<bool> placed (modules.size(), false);
        while (order.size() < modules.size())
        {
            bool progressed = false;
            for (size_t i = 0; i < modules.size(); ++i)
            {
                const int id = (int) modules[i]["id"];
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
                        const int id = (int) modules[i]["id"];
                        placed[i] = true; order.push_back (id);
                        for (auto to : feeds[id]) --incoming[to];
                        break;
                    }
        }

        auto prog = std::make_unique<Modular::Program>();
        std::map<int, int> nodeIndex;
        for (auto id : order)
            if (dsps[id] != nullptr)
                nodeIndex[id] = (int) nodeIndex.size();

        for (auto id : order)
        {
            if (dsps[id] == nullptr) continue;
            Modular::Program::Node n;
            n.dsp = dsps[id].get();
            n.moduleId = id;
            const juce::ValueTree m = *std::find_if (modules.begin(), modules.end(), [id] (const juce::ValueTree& t) { return (int) t["id"] == id; });
            auto* spec = Modular::findSpec (m["type"].toString());
            n.numIns = (int) spec->ins.size();
            n.srcNode.fill (-1);
            n.srcPort.fill (0);
            for (auto& c : cables)
            {
                if ((int) c["to"] != id) continue;
                const int in = Modular::portIndex (spec->ins, c["in"].toString());
                auto* fromSpec = Modular::findSpec (std::find_if (modules.begin(), modules.end(), [&] (const juce::ValueTree& t) { return t["id"] == c["from"]; })->getProperty ("type").toString());
                const int out = fromSpec != nullptr ? Modular::portIndex (fromSpec->outs, c["out"].toString()) : -1;
                if (in < 0 || out < 0) continue;
                n.srcNode[(size_t) in] = nodeIndex[(int) c["from"]];
                n.srcPort[(size_t) in] = out;
                n.patched[(size_t) in] = true;
            }
            prog->nodes.push_back (n);
        }

        if (auto* old = program.exchange (prog.release()))
            oldPrograms.push_back ({ now, std::unique_ptr<Modular::Program> (old) });
    }

    /** Programs and modules that have been replaced are freed a little later, once the audio
        thread can't still be using them. */
    void timerCallback() override
    {
        const double cutoff = juce::Time::getMillisecondCounterHiRes() - 1000.0;
        oldPrograms.erase (std::remove_if (oldPrograms.begin(), oldPrograms.end(), [cutoff] (auto& p) { return p.first < cutoff; }), oldPrograms.end());
        oldDsps.erase (std::remove_if (oldDsps.begin(), oldDsps.end(), [cutoff] (auto& p) { return p.first < cutoff; }), oldDsps.end());
    }

    //==============================================================================
    int numVoices() const   { return juce::jlimit (1, Modular::maxVoices, juce::roundToInt (voices.get())); }

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())                       noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())                 noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())              bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f / 12.0f;
        else if (m.isControllerOfType (1))      modWheel = (float) m.getControllerValue() / 127.0f;
        else if (m.isAllNotesOff() || m.isAllSoundOff()) midiPanic();
    }

    void noteOn (int note, float vel)
    {
        const int n = numVoices();
        if (n == 1)
        {
            // Mono: legato, last note priority (the gate stays up while keys are held)
            held.erase (std::remove (held.begin(), held.end(), note), held.end());
            held.push_back (note);
            auto& v = voiceState[0];
            v.note = note; v.pitchV = (float) (note - 60) / 12.0f; v.gate = 1.0f; v.held = true;
            if (held.size() == 1) v.vel = vel;
            return;
        }

        VoiceState* target = nullptr;
        for (int i = 0; i < n && target == nullptr; ++i)
            if (voiceState[(size_t) i].note == note) target = &voiceState[(size_t) i];
        for (int i = 0; i < n && target == nullptr; ++i)
            if (! voiceState[(size_t) i].held) target = &voiceState[(size_t) i];
        if (target == nullptr)
        {
            juce::uint32 oldest = std::numeric_limits<juce::uint32>::max();
            for (int i = 0; i < n; ++i)
                if (voiceState[(size_t) i].age < oldest) { oldest = voiceState[(size_t) i].age; target = &voiceState[(size_t) i]; }
        }

        target->note = note; target->pitchV = (float) (note - 60) / 12.0f; target->vel = vel; target->held = true; target->age = ++noteCounter;
        target->gate = 0.0f;   // a one-sample dip so envelopes retrigger on a stolen voice
        pendingGate[(size_t) (target - voiceState.data())] = true;
    }

    void noteOff (int note)
    {
        if (numVoices() == 1)
        {
            held.erase (std::remove (held.begin(), held.end(), note), held.end());
            auto& v = voiceState[0];
            if (held.empty()) { v.gate = 0.0f; v.held = false; }
            else { v.note = held.back(); v.pitchV = (float) (v.note - 60) / 12.0f; }
            return;
        }

        for (auto& v : voiceState)
            if (v.note == note && v.held) { v.held = false; v.gate = 0.0f; }
    }

    std::array<bool, Modular::maxVoices> pendingGate {};

    void render (juce::AudioBuffer<float>& buf, int start, int num)
    {
        while (num > 0)
        {
            const int n = juce::jmin (num, maxChunk);
            renderChunk (buf, start, n);
            start += n;
            num -= n;
        }
    }

    void renderChunk (juce::AudioBuffer<float>& buf, int start, int num)
    {
        auto* prog = program.load (std::memory_order_acquire);
        if (prog == nullptr) return;

        const int n = numVoices();
        std::array<float, Modular::maxVoices> pitch {}, gate {}, vel {};
        const std::array<float, 4> macroValues { macros[0].get(), macros[1].get(), macros[2].get(), macros[3].get() };

        Modular::Context ctx;
        ctx.sr = rate * 2.0f;
        ctx.pitch = pitch.data(); ctx.gate = gate.data(); ctx.vel = vel.data();
        ctx.mod = modWheel;
        ctx.macros = macroValues.data();

        // The hover scope's jack, if any
        int probeNode = -1;
        const int pm = probeModule.load(), pp = probePort.load();
        if (pm >= 0)
            for (size_t i = 0; i < prog->nodes.size(); ++i)
                if (prog->nodes[i].moduleId == pm) probeNode = (int) i;

        auto inBlock = juce::dsp::AudioBlock<float> (work).getSubBlock (0, (size_t) num);
        auto up = oversampler->processSamplesUp (inBlock);
        float* OL = up.getChannelPointer (0);
        float* OR = up.getChannelPointer (1);
        const int osN = (int) up.getNumSamples();
        float in[Modular::maxPorts];

        for (int s = 0; s < osN; ++s)
        {
            for (int v = 0; v < n; ++v)
            {
                auto& vs = voiceState[(size_t) v];
                pitch[(size_t) v] = vs.pitchV + bend;
                gate[(size_t) v] = vs.gate;
                vel[(size_t) v] = vs.vel;
                if (pendingGate[(size_t) v]) { pendingGate[(size_t) v] = false; vs.gate = 1.0f; }
            }

            ctx.accL = ctx.accR = 0.0f;
            for (int v = 0; v < n; ++v)
                for (auto& node : prog->nodes)
                {
                    for (int p = 0; p < node.numIns; ++p)
                        in[p] = node.srcNode[(size_t) p] >= 0 ? prog->nodes[(size_t) node.srcNode[(size_t) p]].dsp->out[(size_t) v][(size_t) node.srcPort[(size_t) p]] : 0.0f;
                    node.dsp->process (v, in, node.patched.data(), ctx);
                }

            OL[s] = ctx.accL;
            OR[s] = ctx.accR;
            if (probeNode >= 0 && (s & 1) == 0 && pp >= 0 && pp < Modular::maxPorts)
                probeScope.push (prog->nodes[(size_t) probeNode].dsp->out[0][(size_t) pp]);
        }

        auto outBlock = juce::dsp::AudioBlock<float> (work).getSubBlock (0, (size_t) num);
        oversampler->processSamplesDown (outBlock);

        const float outGain = juce::Decibels::decibelsToGain (volume.get(), -60.0f);
        const bool stereo = buf.getNumChannels() > 1;
        float* L = buf.getWritePointer (0, start);
        float* R = stereo ? buf.getWritePointer (1, start) : nullptr;
        const float* wl = work.getReadPointer (0);
        const float* wr = work.getReadPointer (1);
        for (int s = 0; s < num; ++s)
        {
            const float l = std::tanh (wl[s] * outGain);
            const float r = std::tanh (wr[s] * outGain);
            if (stereo) { L[s] = l; R[s] = r; } else L[s] = (l + r) * 0.5f;
            scope.push ((l + r) * 0.5f);
        }
        work.clear();

        samplesPerCycle = rate / Modular::voctToHz (voiceState[0].pitchV);
    }
};

//==============================================================================
/** Flat jacks, knobs and modules for the rack. */
namespace ModularUI
{
    class Rack;

    /** A knob bound to a module property (undoable), drawn with the flat dot knob. */
    class ModKnob : public juce::Slider
    {
    public:
        std::function<void (const juce::String&, const juce::String&, float)> onTouch;

        ModKnob (juce::ValueTree module, const Modular::ParamSpec& p, juce::UndoManager* um, juce::Colour colour)
            : juce::Slider (juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox), spec (p), undo (um)
        {
            setNormalisableRange (Modular::rangeFor (p));
            setDoubleClickReturnValue (true, p.def);
            setColour (juce::Slider::rotarySliderFillColourId, colour);
            getValueObject().referTo (module.getPropertyAsValue (Modular::paramProperty (p), um));
            onDragStart = [this] { if (undo != nullptr) undo->beginNewTransaction ("Modular knob"); report(); };
            onValueChange = [this] { report(); };
        }

        void report()
        {
            if (onTouch)
                onTouch (spec.name, Modular::formatValue (spec, (float) getValue()), (float) valueToProportionOfLength (getValue()));
        }

    private:
        Modular::ParamSpec spec;
        juce::UndoManager* undo;
    };

    struct JackRef
    {
        int moduleId = -1;
        bool isInput = true;
        int index = -1;
        bool operator== (const JackRef& o) const { return moduleId == o.moduleId && isInput == o.isInput && index == o.index; }
    };

    inline juce::Colour moduleColour (const juce::String& type);

    /** Every jack and cable shows its signal: audio orange, CV violet, gates gold. */
    inline juce::Colour signalColour (Modular::Signal s)
    {
        return Theme::indicator (s == Modular::Signal::audio ? Colours::accent : s == Modular::Signal::gate ? Colours::solo : Colours::clips[5]);
    }

    /** One module: title bar, knobs, input jacks, then output jacks on a darker plate. */
    class ModulePanel : public juce::Component
    {
    public:
        static constexpr int colW = 44, knobH = 42, jackH = 30, titleH = 18;

        ModulePanel (Rack& r, juce::ValueTree m, juce::UndoManager* um);

        static int widthFor (const Modular::Spec& s)    { return s.columns * colW + 8; }

        int getModuleId() const         { return (int) tree["id"]; }
        const Modular::Spec& getSpec() const { return *spec; }

        /** Centre of a jack, in this panel's coordinates. */
        juce::Point<float> jackCentre (bool isInput, int index) const
        {
            auto r = jackBounds (isInput, index);
            return { r.getCentreX(), r.getY() + 9.0f };
        }

        juce::Rectangle<float> jackBounds (bool isInput, int index) const
        {
            const int cols = spec->columns;
            const int row = index / cols, col = index % cols;
            const float y = (float) (isInput ? inputsTop : outputsTop) + (float) (row * jackH);
            return { 4.0f + (float) (col * colW), y, (float) colW, (float) jackH };
        }

        std::optional<JackRef> jackAt (juce::Point<float> p) const
        {
            for (int i = 0; i < (int) spec->ins.size(); ++i)
                if (jackBounds (true, i).contains (p)) return JackRef { getModuleId(), true, i };
            for (int i = 0; i < (int) spec->outs.size(); ++i)
                if (jackBounds (false, i).contains (p)) return JackRef { getModuleId(), false, i };
            return {};
        }

        void paint (juce::Graphics& g) override;
        void resized() override;
        void mouseDown (const juce::MouseEvent& e) override;
        void mouseDrag (const juce::MouseEvent& e) override;
        void mouseUp (const juce::MouseEvent& e) override;
        void mouseMove (const juce::MouseEvent& e) override;
        void mouseExit (const juce::MouseEvent& e) override;

    private:
        Rack& rack;
        juce::ValueTree tree;
        const Modular::Spec* spec;
        std::vector<std::unique_ptr<ModKnob>> knobs;
        int inputsTop = 0, outputsTop = 0;
        bool draggingTitle = false;
    };

    //==============================================================================
    /** The row of modules, with the cables drawn over them. */
    class Rack : public juce::Component, public Reflow::Sections, private juce::ValueTree::Listener, private juce::Timer
    {
    public:
        /** Each module is a section: a wrapped device window starts a new row between modules. */
        std::vector<juce::Rectangle<int>> getLayoutSections() const override
        {
            std::vector<juce::Rectangle<int>> out;
            for (auto& p : panels) out.push_back (p->getBounds());
            out.push_back (addButton.getBounds());
            return out;
        }

        std::function<void (const juce::String&, const juce::String&, float)> onTouch;

        Rack (ModularDevice& d) : device (d), patch (d.getPatch())
        {
            addButton.onClick = [this] { showAddMenu(); };
            addAndMakeVisible (addButton);
            patch.addListener (this);
            rebuild();
        }

        ~Rack() override
        {
            patch.removeListener (this);
            device.setProbe (-1, -1);
        }

        // While a cable is being dragged: where it comes from and the jack it would plug into
        std::optional<JackRef> dragSource() const   { return dragFrom; }
        std::optional<JackRef> dropTarget() const   { return target; }

        /** The output jack under the mouse (or none): it gets a live scope. */
        void hoverJack (std::optional<JackRef> jack)
        {
            if (jack && jack->isInput) jack.reset();
            if (jack.has_value() == hovered.has_value() && (! jack || (jack->moduleId == hovered->moduleId && jack->index == hovered->index)))
                return;
            hovered = jack;
            device.setProbe (jack ? jack->moduleId : -1, jack ? jack->index : -1);
            if (jack) startTimerHz (30); else stopTimer();
            repaint();
        }

        ModularDevice& getDevice()      { return device; }

        int idealWidth() const
        {
            int w = 8;
            for (auto& p : panels) w += p->getWidth() + 6;
            return w + 70;
        }

        void resized() override
        {
            int x = 4;
            for (auto& p : panels)
            {
                p->setBounds (x, 0, ModulePanel::widthFor (p->getSpec()), getHeight());
                x += p->getWidth() + 6;
            }
            addButton.setBounds (x, getHeight() / 2 - 30, 56, 60);
        }

        void paint (juce::Graphics&) override {}

        void paintOverChildren (juce::Graphics& g) override
        {
            int index = 0;
            for (auto c : patch)
            {
                if (! c.hasType (Modular::cableId)) continue;
                auto from = jackPosition ({ (int) c["from"], false, outIndex (c) });
                auto to = jackPosition ({ (int) c["to"], true, inIndex (c) });
                if (from && to)
                    drawCable (g, *from, *to, signalColour (signalOf ({ (int) c["from"], false, outIndex (c) })), (float) getHeight());
                ++index;
            }

            if (dragFrom)
                if (auto p = jackPosition (*dragFrom))
                    drawCable (g, *p, target ? *jackPosition (*target) : dragPoint, signalColour (signalOf (*dragFrom)), (float) getHeight());

            drawProbe (g);
        }

        // Jack dragging, called by the panels
        void beginCable (JackRef jack)
        {
            if (jack.isInput)
            {
                // Pick up the cable already in this input, from its other end
                for (auto c : patch)
                    if (c.hasType (Modular::cableId) && (int) c["to"] == jack.moduleId && inIndex (c) == jack.index)
                    {
                        dragFrom = JackRef { (int) c["from"], false, outIndex (c) };
                        device.getUndo()->beginNewTransaction ("Patch cable");
                        device.disconnectInput (jack.moduleId, c["in"].toString(), device.getUndo());
                        return;
                    }
            }
            dragFrom = jack;
            device.getUndo()->beginNewTransaction ("Patch cable");
        }

        void dragCable (juce::Point<float> rackPos)
        {
            dragPoint = rackPos;
            hoverJack ({});
            auto t = jackAt (rackPos);
            if (t && dragFrom && t->isInput == dragFrom->isInput) t.reset();
            const bool changed = t.has_value() != target.has_value() || (t && (t->moduleId != target->moduleId || t->index != target->index || t->isInput != target->isInput));
            target = t;
            if (changed) for (auto& p : panels) p->repaint();
            repaint();
        }

        void endCable (juce::Point<float> rackPos)
        {
            if (dragFrom)
                if (auto target = jackAt (rackPos))
                    if (target->isInput != dragFrom->isInput)
                    {
                        const auto out = dragFrom->isInput ? *target : *dragFrom;
                        const auto in = dragFrom->isInput ? *dragFrom : *target;
                        auto* fromPanel = panelFor (out.moduleId);
                        auto* toPanel = panelFor (in.moduleId);
                        if (fromPanel != nullptr && toPanel != nullptr)
                            device.connect (out.moduleId, fromPanel->getSpec().outs[(size_t) out.index].id,
                                            in.moduleId, toPanel->getSpec().ins[(size_t) in.index].id, device.getUndo());
                    }

            dragFrom.reset();
            target.reset();
            for (auto& p : panels) p->repaint();
            repaint();
        }

        void moveModule (int id, float rackX)
        {
            int index = 0;
            for (auto& p : panels)
                if (rackX > (float) p->getBounds().getCentreX()) ++index;
            device.getUndo()->beginNewTransaction ("Move module");
            auto list = device.modulesInOrder();
            auto it = std::find_if (list.begin(), list.end(), [id] (const juce::ValueTree& m) { return (int) m["id"] == id; });
            const int current = (int) (it - list.begin());
            device.moveModule (id, index > current ? index - 1 : index, device.getUndo());
        }

        void touched (const juce::String& n, const juce::String& v, float p)   { if (onTouch) onTouch (n, v, p); }

    private:
        ModularDevice& device;
        juce::ValueTree patch;
        std::vector<std::unique_ptr<ModulePanel>> panels;
        juce::TextButton addButton { "+" };
        std::optional<JackRef> dragFrom, target, hovered;
        juce::Point<float> dragPoint;

        void timerCallback() override   { repaint(); }

        Modular::Signal signalOf (JackRef j) const
        {
            auto* p = panelFor (j.moduleId);
            if (p == nullptr || j.index < 0) return Modular::Signal::audio;
            const auto& ports = j.isInput ? p->getSpec().ins : p->getSpec().outs;
            return j.index < (int) ports.size() ? ports[(size_t) j.index].signal : Modular::Signal::audio;
        }

        /** A little scope over the hovered output jack. */
        void drawProbe (juce::Graphics& g)
        {
            if (! hovered || dragFrom) return;
            auto pos = jackPosition (*hovered);
            auto* panel = panelFor (hovered->moduleId);
            if (! pos || panel == nullptr) return;
            const auto& port = panel->getSpec().outs[(size_t) hovered->index];
            auto box = juce::Rectangle<float> (112.0f, 52.0f).withCentre ({ pos->x, pos->y - 44.0f });
            box = box.constrainedWithin (getLocalBounds().toFloat().reduced (2.0f));
            g.setColour (Colours::display);
            g.fillRoundedRectangle (box, 5.0f);
            g.setColour (Colours::outline);
            g.drawRoundedRectangle (box, 5.0f, 1.0f);

            // About 16 ms (a couple of cycles of a bass note), drawn at every 4th sample
            std::array<float, 768> raw {};
            device.probeScope.read (raw.data(), (int) raw.size());
            std::array<float, 192> data {};
            for (size_t i = 0; i < data.size(); ++i) data[i] = raw[i * 4];
            float lo = 1.0e9f, hi = -1.0e9f;
            for (auto v : data) { lo = juce::jmin (lo, v); hi = juce::jmax (hi, v); }
            const float span = juce::jmax (0.05f, hi - lo);
            auto plot = box.reduced (6.0f, 6.0f).withTrimmedTop (10.0f);
            juce::Path path;
            for (size_t i = 0; i < data.size(); ++i)
            {
                const float x = plot.getX() + plot.getWidth() * (float) i / (float) (data.size() - 1);
                const float y = plot.getBottom() - (data[i] - lo) / span * plot.getHeight();
                if (i == 0) path.startNewSubPath (x, y); else path.lineTo (x, y);
            }
            g.setColour (signalColour (port.signal));
            g.strokePath (path, juce::PathStrokeType (1.4f));
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (8.5f, juce::Font::bold));
            g.drawText (juce::String (port.name).toUpperCase() + "   " + juce::String (lo, 2) + " .. " + juce::String (hi, 2),
                        box.reduced (6.0f, 3.0f).removeFromTop (10.0f), juce::Justification::centredLeft);
        }

        ModulePanel* panelFor (int id) const
        {
            for (auto& p : panels)
                if (p->getModuleId() == id) return p.get();
            return nullptr;
        }

        int outIndex (const juce::ValueTree& c) const
        {
            auto* p = panelFor ((int) c["from"]);
            return p != nullptr ? Modular::portIndex (p->getSpec().outs, c["out"].toString()) : -1;
        }

        int inIndex (const juce::ValueTree& c) const
        {
            auto* p = panelFor ((int) c["to"]);
            return p != nullptr ? Modular::portIndex (p->getSpec().ins, c["in"].toString()) : -1;
        }

        std::optional<juce::Point<float>> jackPosition (JackRef j) const
        {
            auto* p = panelFor (j.moduleId);
            if (p == nullptr || j.index < 0) return {};
            return p->jackCentre (j.isInput, j.index) + p->getPosition().toFloat();
        }

        std::optional<JackRef> jackAt (juce::Point<float> rackPos) const
        {
            for (auto& p : panels)
                if (p->getBounds().toFloat().contains (rackPos))
                    return p->jackAt (rackPos - p->getPosition().toFloat());
            return {};
        }

        static void drawCable (juce::Graphics& g, juce::Point<float> a, juce::Point<float> b, juce::Colour colour, float height)
        {
            // Hangs a little with distance, but never below the bottom of the rack
            const float room = height - juce::jmax (a.y, b.y) - 6.0f;
            const float sag = juce::jlimit (6.0f, juce::jmax (6.0f, room * 1.3f), 18.0f + a.getDistanceFrom (b) * 0.22f);
            juce::Path p;
            p.startNewSubPath (a);
            p.cubicTo (a.translated (0.0f, sag), b.translated (0.0f, sag), b);
            g.setColour (colour.withAlpha (0.9f));
            g.strokePath (p, juce::PathStrokeType (3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (a));
            g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (b));
        }

        void showAddMenu()
        {
            juce::PopupMenu m;
            int i = 1;
            for (auto& s : Modular::catalogue())
            {
                if (! s.fixed) m.addItem (i, s.name);
                ++i;
            }
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&addButton), [safe = juce::Component::SafePointer<Rack> (this)] (int result)
            {
                if (safe == nullptr || result <= 0) return;
                safe->device.getUndo()->beginNewTransaction ("Add module");
                safe->device.addModule (Modular::catalogue()[(size_t) result - 1].type, safe->device.getUndo());
            });
        }

        void rebuild()
        {
            panels.clear();
            for (auto& m : device.modulesInOrder())
                if (Modular::findSpec (m["type"].toString()) != nullptr)
                {
                    panels.push_back (std::make_unique<ModulePanel> (*this, m, device.getUndo()));
                    addAndMakeVisible (*panels.back());
                }
            setSize (idealWidth(), juce::jmax (getHeight(), 100));
            resized();
            repaint();
        }

        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree& child) override
        {
            if (parent == patch) { if (child.hasType (Modular::moduleId)) rebuild(); else repaint(); }
        }

        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree& child, int) override
        {
            if (parent == patch) { if (child.hasType (Modular::moduleId)) rebuild(); else repaint(); }
        }

        void valueTreePropertyChanged (juce::ValueTree& t, const juce::Identifier& prop) override
        {
            if (t.hasType (Modular::moduleId) && prop.toString() == "pos") { rebuild(); return; }
            if (t.hasType (Modular::cableId)) repaint();
        }

        void valueTreeRedirected (juce::ValueTree&) override  { rebuild(); }
    };

    //==============================================================================
    inline juce::Colour moduleColour (const juce::String& type)
    {
        if (type == "vco" || type == "noise")                  return Colours::accent;
        if (type == "filter")                                   return Colours::solo;
        if (type == "vca" || type == "adsr" || type == "mixer") return Colours::clips[2];
        if (type == "lfo" || type == "sh" || type == "seq")     return Colours::clips[6];
        if (type == "delay")                                    return Colours::clips[5];
        if (type == "fold" || type == "ring")                   return Colours::accent;
        if (type == "lpg")                                      return Colours::clips[2];
        if (type == "quant" || type == "slew" || type == "clkdiv" || type == "random") return Colours::clips[6];
        return Colours::textDim;
    }

    inline ModulePanel::ModulePanel (Rack& r, juce::ValueTree m, juce::UndoManager* um)
        : rack (r), tree (m), spec (Modular::findSpec (m["type"].toString()))
    {
        for (auto& p : spec->params)
        {
            auto k = std::make_unique<ModKnob> (tree, p, um, moduleColour (spec->type));
            k->onTouch = [this] (const juce::String& n, const juce::String& v, float pr) { rack.touched (n, v, pr); };
            addAndMakeVisible (*k);
            knobs.push_back (std::move (k));
        }
    }

    inline void ModulePanel::resized()
    {
        const int cols = spec->columns;
        int y = titleH + 2;
        for (size_t i = 0; i < knobs.size(); ++i)
        {
            const int row = (int) i / cols, col = (int) i % cols;
            knobs[i]->setBounds (4 + col * colW + 6, y + row * knobH, colW - 12, knobH - 12);
        }
        const int knobRows = ((int) knobs.size() + cols - 1) / cols;
        y += knobRows * knobH;
        inputsTop = y;
        y += (((int) spec->ins.size() + cols - 1) / cols) * jackH;
        outputsTop = juce::jmax (y + 2, getHeight() - (((int) spec->outs.size() + cols - 1) / cols) * jackH - 4);
    }

    inline void ModulePanel::paint (juce::Graphics& g)
    {
        auto r = getLocalBounds().toFloat();
        const auto colour = moduleColour (spec->type);
        g.setColour (Colours::panelDark.withAlpha (0.55f));
        g.fillRoundedRectangle (r, 4.0f);

        // Title
        auto title = getLocalBounds().removeFromTop (titleH).reduced (6, 0);
        g.setColour (colour);
        g.fillRect (title.removeFromLeft (3).withSizeKeepingCentre (3, 8));
        title.removeFromLeft (5);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (juce::String (spec->name).toUpperCase(), title, juce::Justification::centredLeft);

        // Knob names
        g.setFont (juce::FontOptions (9.0f));
        for (size_t i = 0; i < knobs.size(); ++i)
            g.drawFittedText (spec->params[i].name, knobs[i]->getBounds().translated (0, knobs[i]->getHeight()).withHeight (11).expanded (5, 0),
                              juce::Justification::centred, 1, 0.7f);

        // Outputs sit on a darker plate
        if (! spec->outs.empty())
        {
            auto plate = juce::Rectangle<float> (2.0f, (float) outputsTop - 2.0f, r.getWidth() - 4.0f, r.getBottom() - (float) outputsTop);
            g.setColour (Colours::panelDark);
            g.fillRoundedRectangle (plate, 3.0f);
        }

        auto drawJack = [&] (bool isInput, int i, const Modular::Port& port)
        {
            auto c = jackCentre (isInput, i);
            const auto ring = juce::Rectangle<float> (13.0f, 13.0f).withCentre (c);
            const auto colour = signalColour (port.signal);
            // While a cable is being dragged, every jack it can plug into lights up
            if (auto src = rack.dragSource(); src && src->isInput != isInput)
            {
                const auto t = rack.dropTarget();
                const bool isTarget = t && t->moduleId == getModuleId() && t->isInput == isInput && t->index == i;
                g.setColour (colour.withAlpha (isTarget ? 0.55f : 0.22f));
                g.fillEllipse (ring.expanded (isTarget ? 6.0f : 4.0f));
            }
            g.setColour (isInput ? Colours::panel : Colours::panelLight);
            g.fillEllipse (ring);
            g.setColour (colour);
            g.drawEllipse (ring.reduced (1.0f), 2.0f);
            g.setColour (Colours::outline);
            g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (c));
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (8.5f));
            g.drawFittedText (port.name, juce::Rectangle<float> (c.x - colW * 0.5f, c.y + 7.0f, (float) colW, 10.0f).toNearestInt(), juce::Justification::centred, 1, 0.6f);
        };

        for (int i = 0; i < (int) spec->ins.size(); ++i)  drawJack (true, i, spec->ins[(size_t) i]);
        for (int i = 0; i < (int) spec->outs.size(); ++i) drawJack (false, i, spec->outs[(size_t) i]);
    }

    inline void ModulePanel::mouseDown (const juce::MouseEvent& e)
    {
        if (auto jack = jackAt (e.position))
        {
            rack.beginCable (*jack);
            rack.dragCable (e.position + getPosition().toFloat());
            return;
        }

        if (e.y < titleH)
        {
            if (e.mods.isPopupMenu())
            {
                if (spec->fixed) return;
                juce::PopupMenu m;
                m.addItem (1, "Delete " + juce::String (spec->name));
                m.showMenuAsync (juce::PopupMenu::Options(), [safe = juce::Component::SafePointer<ModulePanel> (this)] (int result)
                {
                    if (safe == nullptr || result != 1) return;
                    auto& dev = safe->rack.getDevice();
                    dev.getUndo()->beginNewTransaction ("Delete module");
                    dev.removeModule (safe->getModuleId(), dev.getUndo());
                });
                return;
            }
            draggingTitle = true;
        }
    }

    inline void ModulePanel::mouseDrag (const juce::MouseEvent& e)
    {
        if (draggingTitle)
        {
            setAlpha (0.6f);
            return;
        }
        rack.dragCable (e.position + getPosition().toFloat());
    }

    inline void ModulePanel::mouseMove (const juce::MouseEvent& e)     { rack.hoverJack (jackAt (e.position)); }
    inline void ModulePanel::mouseExit (const juce::MouseEvent&)       { rack.hoverJack ({}); }

    inline void ModulePanel::mouseUp (const juce::MouseEvent& e)
    {
        if (draggingTitle)
        {
            draggingTitle = false;
            setAlpha (1.0f);
            if (e.getDistanceFromDragStart() > 10)
                rack.moveModule (getModuleId(), e.position.x + (float) getX());
            return;
        }
        rack.endCable (e.position + getPosition().toFloat());
    }
}

//==============================================================================
/** Modular: display and patches on the left, the rack (scrolling sideways) on the right. */
class ModularView : public SynthPanel::PanelBase, private juce::Timer
{
public:
    ModularView (ModularDevice& d) : dev (d), rack (d)
    {
        oled.readScope = [this] (float* dst, int n) { dev.scope.read (dst, n); };
        oled.getSamplesPerCycle = [this] { return dev.getSamplesPerCycle(); };
        oled.header = "MODULAR";
        addAndMakeVisible (oled);
        onTouch = [this] (te::AutomatableParameter& p) { oled.showParameter (p); };
        rack.onTouch = [this] (const juce::String& n, const juce::String& v, float p) { oled.showValue (n, v, p); };

        for (auto* b : { &prevButton, &nextButton, &patchButton })
            addAndMakeVisible (b);
        prevButton.onClick = [this] { step (-1); };
        prevButton.setTitle ("Previous Patch");
        nextButton.onClick = [this] { step (1); };
        nextButton.setTitle ("Next Patch");
        patchButton.onClick = [this] { showPatchMenu(); };

        const auto c = Colours::clips[5];
        controls = { knob (d.voices, "Voices", c), knob (d.volume, "Volume", Colours::accent),
                     knob (d.macros[0], "Macro 1", Colours::clips[6]), knob (d.macros[1], "Macro 2", Colours::clips[6]),
                     knob (d.macros[2], "Macro 3", Colours::clips[6]), knob (d.macros[3], "Macro 4", Colours::clips[6]) };

        viewport.setViewedComponent (&rack, false);
        viewport.setScrollBarsShown (false, true);
        addAndMakeVisible (viewport);

        updatePatchName();
        startTimerHz (5);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, controlArea, "Patch", Colours::clips[5]);
    }

    /** In its own window the whole rack is laid out, so it can wrap module by module. */
    int getNaturalWidth() const override    { return 200 + 6 + 110 + 6 + rack.idealWidth(); }

    void resized() override
    {
        auto r = getLocalBounds();
        auto display = r.removeFromLeft (200);
        auto nav = display.removeFromBottom (24);
        display.removeFromBottom (4);
        oled.setBounds (display);
        prevButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        nextButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        patchButton.setBounds (nav);
        r.removeFromLeft (6);

        controlArea = r.removeFromLeft (110);
        r.removeFromLeft (6);
        auto c = content (controlArea);
        const int rowH = c.getHeight() / 3;
        for (int i = 0; i < 3; ++i)
            row (c.removeFromTop (rowH), { controls[(size_t) i * 2], controls[(size_t) i * 2 + 1] }, 2);

        viewport.setBounds (r);
        rack.setSize (juce::jmax (rack.idealWidth(), r.getWidth()), r.getHeight() - viewport.getScrollBarThickness());
        sections = { controlArea };
    }

private:
    ModularDevice& dev;
    SynthPanel::Oled oled;
    ModularUI::Rack rack;
    juce::Viewport viewport;
    juce::TextButton prevButton { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextButton { juce::String::fromUTF8 ("\xe2\x96\xb6") }, patchButton { "Patches" };
    std::vector<SynthPanel::Knob*> controls;
    juce::Rectangle<int> controlArea;
    int lastRackWidth = 0;

    void step (int delta)
    {
        const int count = (int) ModularDevice::patches().size();
        const int current = dev.getPatchIndex();
        dev.loadPatch (((current < 0 ? (delta > 0 ? -1 : 0) : current) + delta + count) % count);
        updatePatchName();
    }

    void showPatchMenu()
    {
        juce::PopupMenu m;
        for (int i = 0; i < (int) ModularDevice::patches().size(); ++i)
            m.addItem (i + 1, ModularDevice::patches()[(size_t) i].name, true, i == dev.getPatchIndex());

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchButton), [safe = juce::Component::SafePointer<ModularView> (this)] (int result)
        {
            if (safe != nullptr && result > 0)
            {
                safe->dev.loadPatch (result - 1);
                safe->updatePatchName();
            }
        });
    }

    void updatePatchName()
    {
        const int index = dev.getPatchIndex();
        oled.patchName = index >= 0 && index < (int) ModularDevice::patches().size()
                           ? juce::String ("M") + juce::String (index + 1).paddedLeft ('0', 3) + " " + ModularDevice::patches()[(size_t) index].name
                           : juce::String ("Custom patch");
    }

    void timerCallback() override
    {
        updatePatchName();
        if (rack.idealWidth() != lastRackWidth)
        {
            lastRackWidth = rack.idealWidth();
            resized();
        }
    }
};

inline std::unique_ptr<juce::Component> ModularDevice::createDeviceView()      { return std::make_unique<ModularView> (*this); }

} // namespace elsewhere
