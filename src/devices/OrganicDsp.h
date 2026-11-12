// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthCore.h"
#include "../SoundLibrary.h"

namespace elsewhere
{
//==============================================================================
/**
    The building blocks of Elsewhere Organic: a grain cloud that plays a recorded source
    (transparently at Texture 0, as an evolving cloud as Texture rises), a tuned modal
    "body" the sound excites, a small FDN space, and the built-in sources that play when no
    sound pack is installed.
*/
namespace OrganicDsp
{
    using SoundLibrary::Source;
    using SoundLibrary::Zone;

    inline float hann (float p) noexcept
    {
        static const auto table = []
        {
            std::array<float, 1025> t {};
            for (size_t i = 0; i < t.size(); ++i)
                t[i] = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / 1024.0f);
            return t;
        }();
        const float x = juce::jlimit (0.0f, 1.0f, p) * 1024.0f;
        const int i = juce::jmin (1023, (int) x);
        const float f = x - (float) i;
        return table[(size_t) i] + (table[(size_t) i + 1] - table[(size_t) i]) * f;
    }

    /** 4-point Hermite read of one channel at a fractional position. */
    inline float readAt (const float* d, int n, double pos) noexcept
    {
        const int i = (int) pos;
        if (i < 1 || i >= n - 2)
            return (i >= 0 && i < n) ? d[i] : 0.0f;
        const float f = (float) (pos - (double) i);
        const float xm1 = d[i - 1], x0 = d[i], x1 = d[i + 1], x2 = d[i + 2];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * f + c2) * f + c1) * f + x0;
    }

    //==============================================================================
    /** How a cloud plays: derived from the Texture control (and Life). */
    struct CloudSettings
    {
        float size = 0.08f;         // grain length, seconds
        float density = 2.0f;       // grains overlapping at once
        float spray = 0.0f;         // random offset of each grain's start, seconds
        float scan = 1.0f;          // how fast the play position moves (1 = natural)
        float scatter = 0.0f;       // random pitch per grain, semitones
        float reverse = 0.0f;       // chance a grain plays backwards
        float spread = 0.0f;        // random pan per grain, 0..1
        bool freeze = false;        // hold struck and plucked sounds too (they become a shimmer)

        static CloudSettings forTexture (float t, float life)
        {
            t = juce::jlimit (0.0f, 1.0f, t);
            CloudSettings s;
            s.size = 0.08f + 0.30f * t * t + 0.06f * t;
            s.density = 2.0f + 4.0f * t;
            s.spray = 0.32f * t * t;
            s.scan = juce::jmap (std::pow (t, 0.6f), 1.0f, 0.06f);
            s.scatter = 0.12f * t * t + 0.05f * life * t;
            s.reverse = 0.3f * juce::jmax (0.0f, t - 0.45f) / 0.55f;
            s.spread = 0.85f * t;
            s.freeze = t > 0.55f;
            return s;
        }
    };

    struct Grain
    {
        bool active = false;
        const Zone* z[2] {};
        double pos[2] {}, inc[2] {};
        double offsetR = 0.0;       // the right channel reads slightly apart: real width, not just panning
        float mix = 0.0f, phase = 0.0f, phaseInc = 0.0f, gainL = 1.0f, gainR = 1.0f, weight = 1.0f;
    };

    /** A cloud of grains reading one source for one voice. */
    struct Cloud
    {
        static constexpr int maxGrains = 10;
        std::array<Grain, maxGrains> grains;
        double pos = 0.0;           // play position, seconds into the source
        double timer = 0.0;         // samples until the next grain
        bool finished = true, holding = false, opened = false;
        float holdDir = 1.0f;
        juce::Random rng;

        /** Most recent grain positions, for the display (0..1 of the source). */
        float lastX = 0.0f;

        void start (double seconds, int seed)
        {
            for (auto& g : grains) g.active = false;
            pos = seconds;
            timer = 0.0;
            finished = false;
            holding = false;
            opened = false;
            holdDir = 1.0f;
            rng.setSeed (seed);
        }

        bool isSilent() const
        {
            if (! finished) return false;
            for (auto& g : grains) if (g.active) return false;
            return true;
        }

        /** Renders n samples into L/R (added). pitch: the note being played (MIDI, with bend,
            tune and drift). mix: 0 = the soft layer's zone, 1 = the loud one's. */
        void render (float* L, float* R, int n, const Source& src, const Zone* soft, const Zone* loud, float mix,
                     float pitch, double devRate, const CloudSettings& s, float gain)
        {
            if (soft == nullptr || (finished && isSilent()))
                return;
            if (loud == nullptr) loud = soft;

            const double rootRatio = std::pow (2.0, (pitch - (float) soft->root) / 12.0);
            const double secondsPerSample = rootRatio / devRate;
            const double lenSec = soft->audio.getNumSamples() / src.rate;
            const double hop = juce::jmax (8.0, s.size * devRate / s.density);
            const float norm = 2.0f / juce::jmax (2.0f, s.density);

            for (int i = 0; i < n; ++i)
            {
                // ---- New grains
                if (! finished && timer <= 0.0)
                {
                    if (! opened)
                    {
                        // The note's first grain starts fully open, so the attack is intact
                        spawn (src, soft, loud, mix, pitch, devRate, s, 0.5f, juce::jmax (1.0f, s.density * 0.5f));
                        opened = true;
                    }
                    spawn (src, soft, loud, mix, pitch, devRate, s, 0.0f, 1.0f);
                    const float jitter = 1.0f + (rng.nextFloat() * 2.0f - 1.0f) * 0.25f * juce::jmin (1.0f, s.spray * 4.0f);
                    timer += hop * jitter;
                }
                timer -= 1.0;

                // ---- Move the play position
                if (! finished)
                {
                    if (src.sustains() || s.freeze)
                    {
                        // Sustaining sounds hold in their steady part; frozen plucks just after the attack
                        const double holdEnd = src.sustains() ? juce::jmax (0.2, lenSec * 0.86 - s.size * rootRatio)
                                                              : juce::jmin (lenSec * 0.5, 0.18 + 0.5 * (double) s.size);
                        const double holdStart = src.sustains() ? juce::jmax (lenSec * 0.38, holdEnd - 2.5) : juce::jmin (holdEnd * 0.5, 0.06);
                        if (! holding)
                        {
                            pos += s.scan * secondsPerSample;
                            if (pos >= holdEnd) holding = true;
                        }
                        else
                        {
                            // Wander back and forth through the steady part: an endless sustain
                            pos += holdDir * juce::jmax (0.18, (double) s.scan) * secondsPerSample;
                            if (pos > holdEnd)   { pos = holdEnd; holdDir = -1.0f; }
                            if (pos < holdStart) { pos = holdStart; holdDir = 1.0f; }
                        }
                    }
                    else
                    {
                        pos += s.scan * secondsPerSample;
                        if (pos >= lenSec) finished = true;
                    }
                }

                // ---- Play the grains
                float l = 0.0f, r = 0.0f;
                for (auto& g : grains)
                {
                    if (! g.active) continue;
                    const float w = hann (g.phase) * g.weight;
                    float sl = 0.0f, sr = 0.0f;
                    for (int k = 0; k < 2; ++k)
                    {
                        const float zw = k == 0 ? 1.0f - g.mix : g.mix;
                        if (zw <= 0.001f || g.z[k] == nullptr) continue;
                        const auto& a = g.z[k]->audio;
                        const int len = a.getNumSamples();
                        sl += zw * readAt (a.getReadPointer (0), len, g.pos[k]);
                        sr += zw * readAt (a.getReadPointer (1), len, g.pos[k] + g.offsetR);
                        g.pos[k] += g.inc[k];
                    }
                    l += sl * w * g.gainL;
                    r += sr * w * g.gainR;
                    g.phase += g.phaseInc;
                    if (g.phase >= 1.0f || g.pos[0] < 0.0 || g.pos[0] >= g.z[0]->audio.getNumSamples())
                        g.active = false;
                }
                L[i] += l * norm * gain;
                R[i] += r * norm * gain;
            }

            lastX = (float) juce::jlimit (0.0, 1.0, pos / juce::jmax (0.01, lenSec));
        }

    private:
        void spawn (const Source& src, const Zone* soft, const Zone* loud, float mix, float pitch, double devRate,
                    const CloudSettings& s, float startPhase, float weight)
        {
            Grain* g = nullptr;
            for (auto& c : grains) if (! c.active) { g = &c; break; }
            if (g == nullptr)
            {
                g = &grains[0];
                for (auto& c : grains) if (c.phase > g->phase) g = &c;
            }

            const float scatter = s.scatter * (rng.nextFloat() * 2.0f - 1.0f);
            const bool reverse = s.reverse > 0.0f && rng.nextFloat() < s.reverse;
            const double start = juce::jmax (0.0, pos + (double) s.spray * (rng.nextFloat() * 2.0 - 1.0) * (startPhase > 0.0f ? 0.0 : 1.0));
            const double lenOut = s.size * devRate;

            g->active = true;
            g->mix = mix;
            g->weight = weight;
            g->phase = startPhase;
            g->phaseInc = (float) (1.0 / juce::jmax (16.0, lenOut));
            const float pan = s.spread * (rng.nextFloat() * 2.0f - 1.0f);
            g->gainL = std::sqrt (0.5f * (1.0f - pan)) * juce::MathConstants<float>::sqrt2;
            g->gainR = std::sqrt (0.5f * (1.0f + pan)) * juce::MathConstants<float>::sqrt2;
            g->offsetR = (double) s.spread * (rng.nextFloat() * 2.0 - 1.0) * 0.006 * src.rate;

            const Zone* zs[2] = { soft, loud };
            for (int k = 0; k < 2; ++k)
            {
                g->z[k] = zs[k];
                const double ratio = std::pow (2.0, (pitch + scatter - (float) zs[k]->root) / 12.0) * src.rate / devRate;
                const double remaining = lenOut * (1.0 - startPhase);
                double p = start * src.rate;
                if (reverse) p += ratio * remaining;
                g->pos[k] = p;
                g->inc[k] = reverse ? -ratio : ratio;
            }
        }
    };

    //==============================================================================
    /** A tuned body the sound rings in: eight resonant modes at the note's partials. */
    struct Body
    {
        enum Type { string, tube, bowl, glass, wood, numTypes };
        static juce::StringArray names()    { return { "String", "Tube", "Bowl", "Glass", "Wood" }; }

        static constexpr int modes = 8;
        std::array<float, modes> y1 {}, y2 {}, g {}, c1 {}, c2 {}, pl {}, pr {};
        float noiseGain = 1.0f;     // scale for broadband excitation (air), so it rings as loudly as a tone would

        void reset()    { y1.fill (0.0f); y2.fill (0.0f); }

        void set (float f0, int type, float decay, float bright, double rate)
        {
            static const float ratios[numTypes][modes] = {
                { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f },
                { 1.0f, 3.0f, 5.0f, 7.0f, 9.0f, 11.0f, 13.0f, 15.0f },
                { 1.0f, 2.76f, 5.40f, 8.93f, 13.34f, 18.64f, 24.81f, 31.87f },
                { 1.0f, 2.32f, 4.25f, 6.63f, 9.38f, 12.53f, 16.07f, 20.01f },
                { 1.0f, 3.93f, 9.0f, 15.9f, 24.6f, 35.0f, 47.0f, 60.8f },
            };
            static const float decayScale[numTypes] = { 1.0f, 0.8f, 1.6f, 1.3f, 0.22f };
            type = juce::jlimit (0, numTypes - 1, type);
            const float r0 = std::exp (-6.9078f / (juce::jmax (0.02f, decay * decayScale[type]) * (float) rate));
            noiseGain = std::sqrt (2.0f / juce::jmax (1.0e-7f, 1.0f - r0)) * 0.12f;

            for (int k = 0; k < modes; ++k)
            {
                const float f = f0 * ratios[type][k];
                if (f >= (float) rate * 0.45f) { g[(size_t) k] = 0.0f; c1[(size_t) k] = c2[(size_t) k] = 0.0f; continue; }
                const float t60 = juce::jmax (0.02f, decay * decayScale[type] / std::sqrt (1.0f + 0.6f * (float) k));
                const float rr = std::exp (-6.9078f / (t60 * (float) rate));
                const float w = juce::MathConstants<float>::twoPi * f / (float) rate;
                c1[(size_t) k] = 2.0f * rr * std::cos (w);
                c2[(size_t) k] = -rr * rr;
                const float tilt = std::pow ((float) (k + 1), -1.2f + 1.1f * bright);
                g[(size_t) k] = (1.0f - rr) * 2.0f * juce::jmax (0.05f, std::sin (w)) * tilt;
                const float pan = (k % 2 == 0 ? -1.0f : 1.0f) * juce::jmin (0.7f, 0.12f * (float) k);
                pl[(size_t) k] = 1.0f - juce::jmax (0.0f, pan);
                pr[(size_t) k] = 1.0f + juce::jmin (0.0f, pan);
            }
        }

        /** tone: the sound (rings at unity where it matches a mode); noise: broadband excitation. */
        void process (float tone, float noise, float& l, float& r) noexcept
        {
            const float in = tone + noise * noiseGain;
            float sl = 0.0f, sr = 0.0f;
            for (size_t k = 0; k < (size_t) modes; ++k)
            {
                const float y = g[k] * in + c1[k] * y1[k] + c2[k] * y2[k];
                y2[k] = y1[k]; y1[k] = y;
                sl += y * pl[k]; sr += y * pr[k];
            }
            l = sl; r = sr;
        }
    };

    //==============================================================================
    /** A lush little space: 8-line feedback delay network with gentle modulation. */
    struct Space
    {
        static constexpr int lines = 8;
        std::array<std::vector<float>, lines> buf;
        std::array<int, lines> pos {}, len {};
        std::array<float, lines> lp {};
        std::vector<float> pre;
        int prePos = 0;
        double rate = 48000.0;
        float phase = 0.0f;

        void prepare (double sr)
        {
            rate = sr;
            for (auto& b : buf) b.assign ((size_t) (sr * 0.25) + 8, 0.0f);
            pre.assign ((size_t) (sr * 0.03) + 4, 0.0f);
            pos.fill (0); lp.fill (0.0f); prePos = 0; phase = 0.0f;
        }

        void process (float* L, float* R, int n, float size, float mix, float width) noexcept
        {
            if (mix <= 0.0005f || buf[0].empty())
                return;

            static const float ms[lines] = { 29.7f, 37.1f, 41.1f, 43.7f, 53.3f, 59.9f, 67.7f, 73.1f };
            const float scale = 0.6f + 1.9f * size;
            const float t60 = 0.8f + 9.0f * size * size;
            const float damp = 0.35f + 0.4f * size;
            for (int k = 0; k < lines; ++k)
                len[(size_t) k] = juce::jlimit (8, (int) buf[(size_t) k].size() - 4, (int) (ms[k] * scale * 0.001 * rate));

            const float wet = std::sin (mix * juce::MathConstants<float>::halfPi), dry = std::cos (mix * juce::MathConstants<float>::halfPi);
            const float phInc = (float) (0.37 / rate);

            for (int i = 0; i < n; ++i)
            {
                // Pre-delay
                const float in = (L[i] + R[i]) * 0.5f;
                const int preLen = (int) pre.size() - 2;
                const float delayed = pre[(size_t) ((prePos + 1) % preLen)];
                pre[(size_t) prePos] = in;
                prePos = (prePos + 1) % preLen;

                phase += phInc; if (phase > 1.0f) phase -= 1.0f;
                const float mod = std::sin (phase * juce::MathConstants<float>::twoPi);

                std::array<float, lines> x {};
                float sum = 0.0f;
                for (size_t k = 0; k < (size_t) lines; ++k)
                {
                    const int extra = (k == 1 || k == 6) ? (int) ((k == 1 ? mod : -mod) * 12.0f) : 0;
                    const int sz = (int) buf[k].size();
                    int rp = pos[k] - len[k] - extra; while (rp < 0) rp += sz;
                    float v = buf[k][(size_t) (rp % sz)];
                    lp[k] += (v - lp[k]) * (1.0f - damp * 0.8f);
                    v = lp[k];
                    const float gfb = std::pow (10.0f, -3.0f * (float) len[k] / (float) rate / t60);
                    x[k] = v * gfb;
                    sum += x[k];
                }
                const float h = sum * (2.0f / (float) lines);
                float outL = 0.0f, outR = 0.0f;
                for (size_t k = 0; k < (size_t) lines; ++k)
                {
                    const float fb = x[k] - h;
                    buf[k][(size_t) pos[k]] = fb + delayed * ((k % 2 == 0) ? 0.35f : -0.35f);
                    pos[k] = (pos[k] + 1) % (int) buf[k].size();
                    if (k % 2 == 0) outL += x[k]; else outR += x[k];
                }
                const float mid = (outL + outR) * 0.5f, side = (outL - outR) * 0.5f * width;
                L[i] = L[i] * dry + (mid + side) * wet * 0.55f;
                R[i] = R[i] * dry + (mid - side) * wet * 0.55f;
            }
        }
    };

    //==============================================================================
    /** Sources built into Elsewhere Organic (so it plays without any pack installed). */
    namespace Builtins
    {
        struct Info { const char* id; const char* name; const char* family; };
        inline const std::vector<Info>& list()
        {
            static const std::vector<Info> all = {
                { "bow",   "Air Bow",      "Bowed" },
                { "choir", "Breath Choir", "Breath" },
                { "glass", "Glass",        "Struck" },
                { "wind",  "Wind",         "Texture" },
            };
            return all;
        }

        /** A substitute for a pack source that isn't installed. */
        inline juce::String fallbackFor (const juce::String& family)
        {
            if (family == "Breath" || family == "Keys") return "builtin/choir";
            if (family == "Plucked" || family == "Struck") return "builtin/glass";
            if (family == "Texture") return "builtin/wind";
            return "builtin/bow";
        }

        /** Additive single cycle with a formant envelope. */
        inline std::vector<float> cycle (float f0, std::initializer_list<std::pair<float, float>> formants, float brightness, float rate)
        {
            std::vector<float> t (2048, 0.0f);
            const int harmonics = juce::jmin (80, (int) (rate * 0.45f / f0));
            for (int k = 1; k <= harmonics; ++k)
            {
                const float f = f0 * (float) k;
                float env = 0.08f;
                for (auto [fc, gain] : formants)
                {
                    const float d = std::log2 (f / fc);
                    env += gain * std::exp (-d * d * 6.0f);
                }
                const float a = env / std::pow ((float) k, 1.25f - 0.55f * brightness) * std::exp (-f / (2500.0f + 9000.0f * brightness));
                const float ph = (float) k * 1.618f;
                for (size_t i = 0; i < t.size(); ++i)
                    t[i] += a * std::sin (juce::MathConstants<float>::twoPi * (float) k * (float) i / 2048.0f + ph);
            }
            float peak = 1.0e-6f;
            for (auto v : t) peak = juce::jmax (peak, std::abs (v));
            for (auto& v : t) v /= peak;
            return t;
        }

        inline void fadeEdges (juce::AudioBuffer<float>& b, double rate, float in, float out)
        {
            const int n = b.getNumSamples();
            const int a = (int) (in * rate), z = (int) (out * rate);
            for (int ch = 0; ch < 2; ++ch)
            {
                auto* d = b.getWritePointer (ch);
                for (int i = 0; i < a && i < n; ++i) d[i] *= (float) i / (float) a;
                for (int i = 0; i < z && i < n; ++i) d[n - 1 - i] *= (float) i / (float) z;
            }
        }

        inline SoundLibrary::SourcePtr make (const juce::String& id)
        {
            const double rate = 48000.0;
            const int len = (int) (rate * 6.0);
            auto src = std::make_shared<Source>();
            src->key = "builtin/" + id;
            src->rate = rate;
            for (auto& i : list())
                if (id == i.id) { src->name = i.name; src->family = i.family; }
            if (src->name.isEmpty())
                return {};

            juce::Random rng (id.hashCode());

            if (id == "bow" || id == "choir")
            {
                const bool choir = id == "choir";
                for (int root = 36; root <= 84; root += 12)
                    for (int layer = 0; layer < 2; ++layer)
                    {
                        const float f0 = 440.0f * std::pow (2.0f, ((float) root - 69.0f) / 12.0f);
                        auto table = choir
                            ? (layer == 0 ? cycle (f0, { { 300.0f, 1.0f }, { 870.0f, 0.5f }, { 2240.0f, 0.15f } }, 0.1f, (float) rate)
                                          : cycle (f0, { { 700.0f, 1.0f }, { 1220.0f, 0.8f }, { 2600.0f, 0.4f } }, 0.6f, (float) rate))
                            : cycle (f0, { { 280.0f, 0.9f }, { 1050.0f, 0.6f }, { 2700.0f, 0.5f + 0.4f * (float) layer } }, layer == 0 ? 0.25f : 0.85f, (float) rate);

                        Zone z;
                        z.root = root; z.layer = layer;
                        z.audio.setSize (2, len);
                        SynthCore::Svf breathL, breathR;
                        breathL.set (choir ? 2400.0f : 3200.0f, 0.3f, (float) rate);
                        breathR.set (choir ? 2600.0f : 3500.0f, 0.3f, (float) rate);
                        for (int ch = 0; ch < 2; ++ch)
                        {
                            auto* d = z.audio.getWritePointer (ch);
                            double ph = rng.nextDouble();
                            const float detune = ch == 0 ? -0.04f : 0.04f;
                            float noiseLp = 0.0f;
                            for (int i = 0; i < len; ++i)
                            {
                                const float t = (float) i / (float) rate;
                                const float vib = juce::jmin (1.0f, juce::jmax (0.0f, t - 0.45f) * 1.5f)
                                                  * 0.09f * std::sin (juce::MathConstants<float>::twoPi * (5.1f + 0.2f * (float) ch) * t);
                                const double inc = f0 * std::pow (2.0f, (vib + detune) / 12.0f) / rate;
                                ph += inc; ph -= std::floor (ph);
                                const float x = (float) (ph * 2048.0);
                                const int xi = (int) x;
                                const float tone = table[(size_t) xi] + (table[(size_t) ((xi + 1) & 2047)] - table[(size_t) xi]) * (x - (float) xi);
                                const float w = rng.nextFloat() * 2.0f - 1.0f;
                                noiseLp += (w - noiseLp) * 0.5f;
                                const float breath = (ch == 0 ? breathL : breathR).lowpass (noiseLp) * (0.5f + 0.5f * tone);
                                const float swellIn = juce::jmin (1.0f, t / (choir ? 0.18f : 0.09f));
                                d[i] = (tone * 0.32f + breath * (choir ? 0.22f : 0.12f) * (1.2f - 0.4f * (float) layer)) * swellIn * (0.55f + 0.45f * (float) layer);
                            }
                        }
                        fadeEdges (z.audio, rate, 0.002f, 0.3f);
                        src->zones.push_back (std::move (z));
                    }
            }
            else if (id == "glass")
            {
                static const float partials[] = { 1.0f, 2.32f, 4.25f, 6.63f, 9.38f, 12.53f };
                for (int root = 48; root <= 96; root += 12)
                {
                    const float f0 = 440.0f * std::pow (2.0f, ((float) root - 69.0f) / 12.0f);
                    Zone z;
                    z.root = root;
                    z.audio.setSize (2, len);
                    z.audio.clear();
                    for (int k = 0; k < 6; ++k)
                    {
                        const float f = f0 * partials[k];
                        if (f > rate * 0.45) break;
                        const float amp = 0.35f / (1.0f + (float) k * 0.9f);
                        const float decay = 4.5f / (1.0f + (float) k * 0.8f) * (1.0f - 0.004f * (float) (root - 48));
                        const float ph = rng.nextFloat() * 6.28f;
                        for (int ch = 0; ch < 2; ++ch)
                        {
                            auto* d = z.audio.getWritePointer (ch);
                            const float beat = 1.0f + (ch == 0 ? -0.00002f : 0.00002f) * (float) (k + 1);
                            const double w = juce::MathConstants<double>::twoPi * f * beat / rate;
                            for (int i = 0; i < len; ++i)
                                d[i] += amp * std::exp (-(float) i / (float) rate / decay) * (float) std::sin (w * i + ph);
                        }
                    }
                    fadeEdges (z.audio, rate, 0.0008f, 0.3f);
                    src->zones.push_back (std::move (z));
                }
            }
            else    // wind
            {
                src->pitched = false;
                Zone z;
                z.root = 60;
                z.audio.setSize (2, len);
                for (int ch = 0; ch < 2; ++ch)
                {
                    SynthCore::Svf a, b2;
                    SynthCore::Noise noise;
                    noise.rng.setSeed (77 + ch);
                    auto* d = z.audio.getWritePointer (ch);
                    for (int i = 0; i < len; ++i)
                    {
                        const float t = (float) i / (float) rate;
                        if ((i & 31) == 0)
                        {
                            a.set (500.0f * std::pow (2.0f, 1.5f * std::sin (t * 0.7f + (float) ch)), 0.75f, (float) rate);
                            b2.set (1800.0f * std::pow (2.0f, 1.0f * std::sin (t * 0.43f + 2.0f + (float) ch)), 0.6f, (float) rate);
                        }
                        float l1, b1, h1, l2, bb, h2;
                        const float x = noise.pink();
                        a.process (x, l1, b1, h1);
                        b2.process (x, l2, bb, h2);
                        d[i] = (b1 * 0.9f + bb * 0.5f) * (0.7f + 0.3f * std::sin (t * 1.3f + (float) ch));
                    }
                }
                fadeEdges (z.audio, rate, 0.3f, 0.6f);
                src->zones.push_back (std::move (z));
            }

            src->finish();
            return src;
        }
    }
}

} // namespace elsewhere
