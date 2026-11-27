// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DspMath.h"
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace elsewhere
{
//==============================================================================
/**
    Building blocks shared by Elsewhere's synths (Mono, Poly, Modular, Haze): plain C++ DSP
    that needs nothing of JUCE beyond jlimit, jmin, jmax and MathConstants, so it also builds
    outside it.
*/
//==============================================================================
/**
    One control of an instrument engine, as a screen draws it: a knob, a switch or a menu.

    Each engine lists its controls in controls(), in the order its device panel shows them,
    by the same IDs as its Params::table() (and its device's parameters). step 0 means
    continuous, step 1 whole numbers; choices is non-empty for a menu (the value is the index
    of the choice). unit is what the value is in: "%", "s", "ms", "Hz", "dB", "st", "ct",
    "oct", "x" or "".
*/
struct SynthControl
{
    const char* id;
    const char* name;
    const char* section;
    float min, max, defaultValue;
    float step;
    bool logarithmic;
    const char* unit;
    std::vector<const char*> choices;
};

namespace SynthCore
{
    /** A repeatable random sequence: the same numbers from the same seed as juce::Random. */
    struct Random
    {
        std::int64_t seed = 1;

        Random (std::int64_t s = 1) noexcept : seed (s) {}
        void setSeed (std::int64_t s) noexcept     { seed = s; }

        int nextInt() noexcept
        {
            seed = (std::int64_t) ((((std::uint64_t) seed) * 0x5deece66dULL + 11) & 0xffffffffffffULL);
            return (int) (seed >> 16);
        }

        float nextFloat() noexcept
        {
            const auto result = (float) (std::uint32_t) nextInt() / ((float) std::numeric_limits<std::uint32_t>::max() + 1.0f);
            return std::min (result, 1.0f - std::numeric_limits<float>::epsilon());
        }
    };

    constexpr float twoPi = 6.283185307179586f;

    /** PolyBLEP: removes the step discontinuity's aliasing (t = phase, dt = phase increment). */
    inline float polyBlep (float t, float dt) noexcept
    {
        if (t < dt)         { t /= dt; return t + t - t * t - 1.0f; }
        if (t > 1.0f - dt)  { t = (t - 1.0f) / dt; return t * t + t + t + 1.0f; }
        return 0.0f;
    }

    /** PolyBLAMP: the same for slope changes (triangle corners). */
    inline float polyBlamp (float t, float dt) noexcept
    {
        if (t < dt)         { t = t / dt - 1.0f; return -1.0f / 3.0f * t * t * t; }
        if (t > 1.0f - dt)  { t = (t - 1.0f) / dt + 1.0f; return 1.0f / 3.0f * t * t * t; }
        return 0.0f;
    }

    //==============================================================================
    /** Classic analog-style waveforms, band-limited. */
    enum class Wave { triangle, shark, saw, reverseSaw, square, widePulse, narrowPulse, sine };

    struct Oscillator
    {
        float phase = 0.0f;

        void reset (float p = 0.0f)     { phase = p; }

        /** One sample. `dt` = frequency / sampleRate. */
        float process (Wave w, float dt) noexcept
        {
            dt = dspmath::jlimit (1.0e-7f, 0.45f, dt);
            const float t = phase;
            float y = 0.0f;

            auto saw = [&] { return 2.0f * t - 1.0f - polyBlep (t, dt); };
            auto pulse = [&] (float width)
            {
                float t2 = t + (1.0f - width); if (t2 >= 1.0f) t2 -= 1.0f;
                return (t < width ? 1.0f : -1.0f) + polyBlep (t, dt) - polyBlep (t2, dt) + (1.0f - 2.0f * width);
            };
            auto tri = [&]
            {
                float v = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
                float t2 = t + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
                v += 4.0f * dt * (polyBlamp (t, dt) - polyBlamp (t2, dt));
                return v;
            };

            switch (w)
            {
                case Wave::triangle:    y = tri(); break;
                case Wave::shark:       y = 0.55f * tri() + 0.55f * saw(); break;   // "tri-saw": rounded ramp
                case Wave::saw:         y = saw(); break;
                case Wave::reverseSaw:  y = -saw(); break;
                case Wave::square:      y = pulse (0.5f); break;
                case Wave::widePulse:   y = pulse (0.3f); break;
                case Wave::narrowPulse: y = pulse (0.12f); break;
                case Wave::sine:        y = std::sin (t * twoPi); break;
            }

            phase += dt;
            if (phase >= 1.0f) phase -= 1.0f;
            return y;
        }

        /** Pulse with any width (0..1), band-limited and DC-free. */
        float pulse (float width, float dt) noexcept
        {
            dt = dspmath::jlimit (1.0e-7f, 0.45f, dt);
            width = dspmath::jlimit (0.03f, 0.97f, width);
            const float t = phase;
            float t2 = t + (1.0f - width); if (t2 >= 1.0f) t2 -= 1.0f;
            const float y = (t < width ? 1.0f : -1.0f) + polyBlep (t, dt) - polyBlep (t2, dt) + (1.0f - 2.0f * width);
            phase += dt;
            if (phase >= 1.0f) phase -= 1.0f;
            return y;
        }

        // Reading waveforms at the current phase without moving on (several shapes from one
        // phase, like a DCO), then advance() once per sample
        float sawAt (float dt) const noexcept       { return 2.0f * phase - 1.0f - polyBlep (phase, dspmath::jlimit (1.0e-7f, 0.45f, dt)); }

        float pulseAt (float width, float dt) const noexcept
        {
            dt = dspmath::jlimit (1.0e-7f, 0.45f, dt);
            width = dspmath::jlimit (0.03f, 0.97f, width);
            float t2 = phase + (1.0f - width); if (t2 >= 1.0f) t2 -= 1.0f;
            return (phase < width ? 1.0f : -1.0f) + polyBlep (phase, dt) - polyBlep (t2, dt) + (1.0f - 2.0f * width);
        }

        float triAt (float dt) const noexcept
        {
            dt = dspmath::jlimit (1.0e-7f, 0.45f, dt);
            float v = phase < 0.5f ? 4.0f * phase - 1.0f : 3.0f - 4.0f * phase;
            float t2 = phase + 0.5f; if (t2 >= 1.0f) t2 -= 1.0f;
            return v + 4.0f * dt * (polyBlamp (phase, dt) - polyBlamp (t2, dt));
        }

        /** Moves on one sample; returns true if the phase wrapped. */
        bool advance (float dt) noexcept
        {
            phase += dspmath::jlimit (1.0e-7f, 0.45f, dt);
            if (phase >= 1.0f) { phase -= 1.0f; return true; }
            return false;
        }

        /** True on the sample the phase wrapped (for hard sync). */
        bool wrapped (float dt) const noexcept   { return phase < dt; }
    };

    //==============================================================================
    /**
        Transistor ladder low-pass (24 dB/oct), after Huovilainen's non-linear model:
        four one-pole stages with tanh saturation and resonance feedback that self-oscillates
        near the top of its range. Run it oversampled (2x or more).
    */
    struct Ladder
    {
        std::array<float, 4> y {}, ty {};
        float lastOut = 0.0f;
        float g = 0.1f, k = 0.0f, comp = 1.0f;
        int poles = 4;      // 4: 24 dB transistor ladder; 3: 18 dB, closer to the diode ladder of acid basslines

        void reset()    { y.fill (0.0f); ty.fill (0.0f); lastOut = 0.0f; }

        /** cutoff in Hz, resonance 0..1 (1 = self-oscillation), at the processing rate. */
        void set (float cutoff, float resonance, float rate) noexcept
        {
            const float fc = dspmath::jlimit (10.0f, rate * 0.42f, cutoff);
            // Frequency warping correction so the cutoff stays in tune up high
            const float x = fc / rate;
            g = 1.0f - std::exp (-twoPi * x * (1.0f + 0.9f * x));
            // Self-oscillation needs a loop gain of 4 with four poles, 8 with three
            k = (poles == 3 ? 8.4f : 4.7f) * dspmath::jlimit (0.0f, 1.05f, resonance);
            comp = 1.0f + 0.45f * k;   // gives back some of the bass that resonance takes (not all: that's the sound)
        }

        float process (float in) noexcept
        {
            // Half-sample-delayed feedback (average of this and last output) keeps the tuning right
            const auto& outPole = y[(size_t) (poles - 1)];
            const float fb = k * 0.5f * (outPole + lastOut);
            lastOut = outPole;
            const float u = fastTanh (in * comp - fb);

            y[0] += g * (u - ty[0]);        ty[0] = fastTanh (y[0]);
            y[1] += g * (ty[0] - ty[1]);    ty[1] = fastTanh (y[1]);
            y[2] += g * (ty[1] - ty[2]);    ty[2] = fastTanh (y[2]);
            y[3] += g * (ty[2] - ty[3]);    ty[3] = fastTanh (y[3]);
            return y[(size_t) (poles - 1)];
        }

        static float fastTanh (float x) noexcept
        {
            // Pade approximation, accurate to ~1e-3 and smooth beyond +-3
            if (x > 3.0f) return 1.0f;
            if (x < -3.0f) return -1.0f;
            const float x2 = x * x;
            return x * (27.0f + x2) / (27.0f + 9.0f * x2);
        }
    };

    //==============================================================================
    /**
        The classic 70s transistor ladder (24 dB/oct), solved zero-delay-feedback with a tanh
        in the input and in every stage (after Mystran's cheap non-linear ZDF method: each
        stage's saturation is linearised around its current state, then the feedback loop is
        solved exactly). Stays in tune up to self-oscillation, saturates like the real filter
        when driven, and loses some (not all) of its bass as the resonance rises.
    */
    struct MoogLadder
    {
        std::array<float, 4> s {};
        float zi = 0.0f, f = 0.1f, r = 0.0f, comp = 1.0f;

        void reset()    { s.fill (0.0f); zi = 0.0f; }

        /** cutoff in Hz, resonance 0..1 (self-oscillates from about 0.96), at the processing rate. */
        void set (float cutoff, float resonance, float rate) noexcept
        {
            const float fc = dspmath::jlimit (10.0f, rate * 0.45f, cutoff);
            f = std::tan (dspmath::MathConstants<float>::pi * fc / rate);
            r = 4.25f * dspmath::jlimit (0.0f, 1.03f, resonance);
            comp = 1.0f + 0.55f * r;
        }

        float process (float in) noexcept
        {
            in *= comp;
            const float ih = 0.5f * (in + zi);      // half-sample input delay: keeps the stages in step
            zi = in;

            // Non-linear gains of the input and of each stage, around their current state
            const float t0 = tanhXdX (ih - r * s[3]);
            const float t1 = tanhXdX (s[0]), t2 = tanhXdX (s[1]), t3 = tanhXdX (s[2]), t4 = tanhXdX (s[3]);

            const float g0 = 1.0f / (1.0f + f * t1), g1 = 1.0f / (1.0f + f * t2);
            const float g2 = 1.0f / (1.0f + f * t3), g3 = 1.0f / (1.0f + f * t4);
            const float f3 = f * t3 * g3, f2 = f * t2 * g2 * f3, f1 = f * t1 * g1 * f2, f0 = f * t0 * g0 * f1;

            // Solve the feedback loop, then each stage
            const float y3 = (g3 * s[3] + f3 * g2 * s[2] + f2 * g1 * s[1] + f1 * g0 * s[0] + f0 * ih) / (1.0f + r * f0);
            const float xx = t0 * (ih - r * y3);
            const float y0 = t1 * g0 * (s[0] + f * xx);
            const float y1 = t2 * g1 * (s[1] + f * y0);
            const float y2 = t3 * g2 * (s[2] + f * y1);

            s[0] += 2.0f * f * (xx - y0);
            s[1] += 2.0f * f * (y0 - y1);
            s[2] += 2.0f * f * (y1 - y2);
            s[3] += 2.0f * f * (y2 - t4 * y3);
            return y3;
        }

        /** tanh(x) / x, a rational approximation good to well past the saturation range. */
        static float tanhXdX (float x) noexcept
        {
            const float a = x * x;
            if (a > 16.0f)                      // hard driven: the exact curve, so it keeps saturating
                return std::tanh (x) / x;
            return ((a + 105.0f) * a + 945.0f) / ((15.0f * a + 420.0f) * a + 945.0f);
        }
    };

    /** One-pole DC blocker / AC coupling (the slight droop of analog squares). */
    struct DcBlock
    {
        float x1 = 0.0f, y1 = 0.0f, R = 0.999f;
        void set (float hz, float rate) noexcept    { R = std::exp (-dspmath::MathConstants<float>::twoPi * hz / rate); }
        void reset()                                { x1 = y1 = 0.0f; }
        float process (float x) noexcept            { const float y = x - x1 + R * y1; x1 = x; y1 = y; return y; }
    };

    //==============================================================================
    /** Zero-delay-feedback state-variable filter (Zavalishin): 12 dB low, band and high pass. */
    struct Svf
    {
        float ic1 = 0.0f, ic2 = 0.0f;
        float g = 0.0f, k = 1.0f, a1 = 0.0f, a2 = 0.0f, a3 = 0.0f;

        void reset()    { ic1 = ic2 = 0.0f; }

        /** resonance 0..1 (near 1 rings strongly). */
        void set (float cutoff, float resonance, float rate) noexcept
        {
            g = std::tan (dspmath::MathConstants<float>::pi * dspmath::jlimit (10.0f, rate * 0.45f, cutoff) / rate);
            k = 2.0f - 1.94f * dspmath::jlimit (0.0f, 1.0f, resonance);
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        void process (float v0, float& lp, float& bp, float& hp) noexcept
        {
            const float v3 = v0 - ic2;
            const float v1 = a1 * ic1 + a2 * v3;
            const float v2 = ic2 + a2 * ic1 + a3 * v3;
            ic1 = 2.0f * v1 - ic1;
            ic2 = 2.0f * v2 - ic2;
            lp = v2; bp = v1; hp = v0 - k * v1 - v2;
        }

        float lowpass (float x) noexcept    { float l, b, h; process (x, l, b, h); return l; }
        float highpass (float x) noexcept   { float l, b, h; process (x, l, b, h); return h; }
    };

    //==============================================================================
    /**
        Stereo bucket-brigade-style chorus, after the classic 80s poly chorus: one delay line
        swept by a triangle LFO, read in opposite directions for left and right, darkened like
        a BBD. Modes: 1 (slow, gentle), 2 (faster, deeper), 3 (both: a fast shimmer).
    */
    struct StereoChorus
    {
        std::vector<float> line;
        int pos = 0;
        float phase = 0.0f, lpL = 0.0f, lpR = 0.0f;

        void prepare (double rate)  { line.assign ((size_t) (rate * 0.03) + 4, 0.0f); pos = 0; phase = 0.0f; lpL = lpR = 0.0f; }

        void process (float* left, float* right, int n, int mode, float rate) noexcept
        {
            if (mode <= 0 || line.empty())
                return;

            const float lfoHz = mode == 1 ? 0.513f : mode == 2 ? 0.863f : 9.75f;
            const float depthMs = mode == 1 ? 1.85f : mode == 2 ? 2.2f : 0.35f;
            const float centreMs = mode == 3 ? 3.3f : 3.6f;
            const float inc = lfoHz / rate;
            const float lp = std::exp (-twoPi * 9000.0f / rate);
            const int len = (int) line.size();

            auto read = [&] (float delaySamples)
            {
                float rp = (float) pos - delaySamples;
                while (rp < 0.0f) rp += (float) len;
                const int i0 = (int) rp % len;
                const int i1 = (i0 + 1) % len;
                const float f = rp - std::floor (rp);
                return line[(size_t) i0] + f * (line[(size_t) i1] - line[(size_t) i0]);
            };

            for (int i = 0; i < n; ++i)
            {
                const float mono = (left[i] + right[i]) * 0.5f;
                line[(size_t) pos] = mono;

                phase += inc;
                if (phase >= 1.0f) phase -= 1.0f;
                const float tri = 4.0f * std::abs (phase - 0.5f) - 1.0f;

                const float dL = (centreMs + depthMs * tri) * 0.001f * rate;
                const float dR = (centreMs - depthMs * tri) * 0.001f * rate;
                lpL = lp * lpL + (1.0f - lp) * read (dL);
                lpR = lp * lpR + (1.0f - lp) * read (dR);

                left[i] = left[i] * 0.72f + lpL * 0.72f;
                right[i] = right[i] * 0.72f + lpR * 0.72f;

                if (++pos >= len) pos = 0;
            }
        }
    };

    //==============================================================================
    /**
        Analog-style contour: attack charges towards a target above 1 (the curve of a
        capacitor charging), decay and release fall exponentially.
        Times are the 0 -> ~63% / 1 -> ~37% style "time to mostly get there", in seconds.
    */
    struct AnalogEnvelope
    {
        enum class Stage { idle, attack, decay, release };
        Stage stage = Stage::idle;
        float value = 0.0f;

        void gate (bool on)                 { stage = on ? Stage::attack : (stage == Stage::idle ? Stage::idle : Stage::release); }
        bool isActive() const noexcept      { return stage != Stage::idle; }
        void reset()                        { stage = Stage::idle; value = 0.0f; }

        float process (float attack, float decay, float sustain, float release, float rate) noexcept
        {
            auto coeff = [rate] (float t) { return 1.0f - std::exp (-1.0f / (dspmath::jmax (0.0005f, t) * rate)); };

            switch (stage)
            {
                case Stage::attack:
                    value += (1.3f - value) * coeff (attack * 0.85f);
                    if (value >= 1.0f) { value = 1.0f; stage = Stage::decay; }
                    break;
                case Stage::decay:
                    value += (sustain - value) * coeff (decay * 0.3f);
                    break;
                case Stage::release:
                    value += (0.0f - value) * coeff (release * 0.3f);
                    if (value < 1.0e-4f) { value = 0.0f; stage = Stage::idle; }
                    break;
                case Stage::idle:
                    break;
            }
            return value;
        }
    };

    //==============================================================================
    /** Slow random pitch wander (analog drift), in semitones. Call once per sample. */
    struct Drift
    {
        Random rng;
        float value = 0.0f, target = 0.0f, smooth = 0.0f;
        int counter = 0;

        explicit Drift (int seed = 1) : rng (seed) {}

        float process (float amountSemis, float rate) noexcept
        {
            if (--counter <= 0)
            {
                counter = (int) (rate * 0.08f);
                target = (rng.nextFloat() * 2.0f - 1.0f);
            }
            smooth += (target - smooth) * (1.0f / (rate * 0.25f));
            value = smooth * amountSemis;
            return value;
        }
    };

    /** White or pink noise (pink: Paul Kellet's economy filter). */
    struct Noise
    {
        Random rng { 1234 };
        float b0 = 0, b1 = 0, b2 = 0;

        float white() noexcept      { return rng.nextFloat() * 2.0f - 1.0f; }

        float pink() noexcept
        {
            const float w = white();
            b0 = 0.99765f * b0 + w * 0.0990460f;
            b1 = 0.96300f * b1 + w * 0.2965164f;
            b2 = 0.57000f * b2 + w * 1.0526913f;
            return (b0 + b1 + b2 + w * 0.1848f) * 0.22f;
        }
    };

    //==============================================================================
    /** Held keys for a mono synth, with note priority (like a real monosynth's keyboard). */
    struct NoteStack
    {
        enum class Priority { last, low, high };
        std::vector<int> held;

        NoteStack()     { held.reserve (128); }

        void press (int note)       { release (note); held.push_back (note); }
        void release (int note)     { held.erase (std::remove (held.begin(), held.end(), note), held.end()); }
        void clear()                { held.clear(); }
        bool empty() const          { return held.empty(); }

        int current (Priority p) const
        {
            if (held.empty()) return -1;
            switch (p)
            {
                case Priority::low:     return *std::min_element (held.begin(), held.end());
                case Priority::high:    return *std::max_element (held.begin(), held.end());
                case Priority::last:
                default:                return held.back();
            }
        }
    };

    /** A lock-free ring of recent output samples, for an oscilloscope on the UI thread. */
    struct ScopeBuffer
    {
        static constexpr int size = 4096;
        std::array<float, size> data {};
        std::atomic<int> writePos { 0 };

        void push (float v) noexcept
        {
            auto p = writePos.load (std::memory_order_relaxed);
            data[(size_t) p] = v;
            writePos.store ((p + 1) & (size - 1), std::memory_order_release);
        }

        /** Copies the latest `n` samples (oldest first). */
        void read (float* dest, int n) const noexcept
        {
            auto p = writePos.load (std::memory_order_acquire);
            for (int i = 0; i < n; ++i)
                dest[i] = data[(size_t) ((p - n + i) & (size - 1))];
        }
    };
}
} // namespace elsewhere
