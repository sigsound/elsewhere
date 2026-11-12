// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthCore.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Drums' built-in kits, synthesised in code (no sample files to ship).

    Every kit fills the same 16 slots (kick, snare, clap, hats, toms, rim, cowbell, crash, ride,
    shaker, clave, 808, zap), so patterns written for one kit play sensibly on any other; the
    names shown change with the kit (a "Perc" or "Conga" where a tom would be, and so on).

    Each sound can be rendered in a few slightly different variations (round robin), so fast
    repeats don't sound machine-gunned.

    Kit 0, Classic, is the original analog-style kit, synthesised by DrumsDevice itself.
*/
namespace DrumKits
{
    struct Kit
    {
        const char* id;
        const char* name;
        const char* description;
        std::array<const char*, 16> pads;
        bool recorded = false;      // real drums (samples built into the app), rather than synthesised
    };

    inline const std::vector<Kit>& kits()
    {
        static const std::vector<Kit> all = {
            { "classic", "Classic", "The original analog drum machine kit",
              { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Low Tom", "Mid Tom", "High Tom", "Rim", "Cowbell", "Crash", "Ride", "Shaker", "Clave", "808", "Zap" } },
            { "trap", "Trap", "Punchy kick, crisp snare, razor hats and a gliding 808",
              { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Perc Lo", "Perc Mid", "Perc Hi", "Rim", "Hey", "Rev Crash", "Shimmer", "Shaker", "Snap", "808 Glide", "Laser" } },
            { "lofi", "Lo-Fi", "Dusty, warm and crushed: boom-bap through an old sampler",
              { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Low Tom", "Mid Tom", "High Tom", "Knock", "Vinyl", "Crash", "Ride", "Shaker", "Wood", "Sub", "Tape Stop" } },
            { "hyperpop", "Hyperpop", "Distorted, digital and sparkly",
              { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Chip Lo", "Chip Mid", "Chip Hi", "Glitch", "Bell", "Digital Crash", "Zip", "Shaker", "Ping", "Dist 808", "Laser" } },
            { "amapiano", "Amapiano", "Log drum, congas, shakers and a soft deep kick",
              { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Conga Lo", "Conga Mid", "Conga Hi", "Rim", "Agogo", "Crash", "Ride", "Shaker", "Clave", "Log Drum", "Whistle" } },
            { "acoustic", "Acoustic Room", "A modelled acoustic kit in a small room",
              { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Floor Tom", "Mid Tom", "High Tom", "Cross Stick", "Cowbell", "Crash", "Ride", "Shaker", "Clave", "Boom", "Cajon" } },
            { "techno", "Techno", "909-flavoured kit with rumble and metal",
              { "Kick", "Snare", "Clap", "Closed Hat", "Open Hat", "Low Tom", "Mid Tom", "High Tom", "Rim", "Metal", "Crash", "Ride", "Shaker", "Blip", "Rumble", "Noise Hit" } },

            // Recorded kits: real drums, from Karoryfer's CC0 libraries (see resources/kits)
            { "bigrusty", "Big Rusty", "A big, roomy 80s rock kit",
              { "Kick", "Snare", "Snare Edge", "Closed Hat", "Open Hat", "Floor Tom", "Mid Tom", "High Tom", "Rim Click", "Ride Bell", "Crash", "Ride", "Hat Foot", "Hat Perc", "Open Kick", "China" }, true },
            { "brushes", "Brushes", "Brushes on a big kit: jazz, neo-soul, ballads",
              { "Kick", "Brush Snare", "Stir", "Closed Hat", "Open Hat", "Floor Tom", "Mid Tom", "High Tom", "Flutter", "Sizzle", "Crash", "Ride", "Loose Hat", "Release", "Open Kick", "China" }, true },
            { "studiodry", "Studio Dry", "Close-miked and dry: tight R&B, funk and pop",
              { "Kick", "Snare", "Rimshot", "Closed Hat", "Open Hat", "Low Tom", "Mid Tom", "High Tom", "Side Stick", "Wood Hi", "Crash", "Ride", "Hat Foot", "Wood Lo", "Dirty Kick", "Ride Bell" }, true },
            { "unrulyroom", "Unruly Room", "The same kit with the room mics up and a fat snare",
              { "Kick", "Fat Snare", "Snare Edge", "Closed Hat", "Open Hat", "Low Tom", "Mid Tom", "High Tom", "Side Stick", "Wood Hi", "Crash", "Ride", "Hat Foot", "Wood Lo", "Dirty Kick", "Ride Bell" }, true },
            { "seventies", "Seventies", "A warm, early-70s kit",
              { "Kick", "Snare", "Snare Edge", "Closed Hat", "Open Hat", "Low Tom", "Mid Tom", "High Tom", "Side Stick", "Hat Foot", "Loose Hat", "Half Hat", "Tight Hat", "Room Stick", "Room Kick", "Room Snare" }, true },
            { "retrotape", "Retro Tape", "The same kit through a lo-fi retro mic",
              { "Kick", "Snare", "Snare Edge", "Closed Hat", "Open Hat", "Low Tom", "Mid Tom", "High Tom", "Side Stick", "Hat Foot", "Loose Hat", "Half Hat", "Tight Hat", "Room Stick", "Room Kick", "Room Snare" }, true },
        };
        return all;
    }

    inline int indexOf (const juce::String& id)
    {
        for (int i = 0; i < (int) kits().size(); ++i)
            if (id == kits()[(size_t) i].id)
                return i;
        return 0;
    }

    constexpr int numVariations = 3;
    constexpr double rate = 48000.0;

    //==============================================================================
    // A small toolbox for building drum sounds offline
    using Buf = juce::AudioBuffer<float>;

    struct Ctx
    {
        juce::Random rng;
        float pitch = 1.0f;     // variation: slight detune
        float decay = 1.0f;     // variation: slightly longer or shorter
        float noise()           { return rng.nextFloat() * 2.0f - 1.0f; }
    };

    constexpr double twoPi = juce::MathConstants<double>::twoPi;

    inline int samples (double seconds)     { return juce::jmax (1, (int) (seconds * rate)); }

    template <typename Fn>
    Buf mono (double seconds, Fn&& fn)
    {
        Buf b (1, samples (seconds));
        auto* d = b.getWritePointer (0);
        for (int i = 0; i < b.getNumSamples(); ++i)
            d[i] = fn ((double) i / rate);
        return b;
    }

    /** Stereo, with independent noise per side. fn (t, channel). */
    template <typename Fn>
    Buf stereo (double seconds, Fn&& fn)
    {
        Buf b (2, samples (seconds));
        for (int i = 0; i < b.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                b.setSample (ch, i, fn ((double) i / rate, ch));
        return b;
    }

    inline Buf toStereo (const Buf& b)
    {
        if (b.getNumChannels() > 1) return b;
        Buf s (2, b.getNumSamples());
        s.copyFrom (0, 0, b, 0, 0, b.getNumSamples());
        s.copyFrom (1, 0, b, 0, 0, b.getNumSamples());
        return s;
    }

    /** dst += src * gain, starting at offset (seconds); grows dst if needed. */
    inline void mix (Buf& dst, const Buf& src, float gain = 1.0f, double offset = 0.0)
    {
        const int off = samples (offset) - (offset <= 0.0 ? 1 : 0);
        const int need = off + src.getNumSamples();
        const int chans = juce::jmax (dst.getNumChannels(), src.getNumChannels());
        if (need > dst.getNumSamples() || chans > dst.getNumChannels())
        {
            Buf bigger (chans, juce::jmax (need, dst.getNumSamples()));
            bigger.clear();
            for (int ch = 0; ch < chans; ++ch)
                bigger.copyFrom (ch, 0, dst, juce::jmin (ch, dst.getNumChannels() - 1), 0, dst.getNumSamples());
            dst = bigger;
        }
        for (int ch = 0; ch < dst.getNumChannels(); ++ch)
            dst.addFrom (ch, off, src, juce::jmin (ch, src.getNumChannels() - 1), 0, src.getNumSamples(), gain);
    }

    inline void saturate (Buf& b, float drive)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
                d[i] = std::tanh (d[i] * drive) / std::tanh (drive);
        }
    }

    /** Hard clipping, for a digital edge. */
    inline void clip (Buf& b, float drive)
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
                d[i] = juce::jlimit (-1.0f, 1.0f, d[i] * drive);
        }
    }

    /** Fewer bits, and each sample held for a while (a lower sample rate). */
    inline void crush (Buf& b, int bits, int hold)
    {
        const float steps = (float) (1 << (bits - 1));
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            auto* d = b.getWritePointer (ch);
            float held = 0.0f;
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                if (i % hold == 0) held = std::round (d[i] * steps) / steps;
                d[i] = held;
            }
        }
    }

    inline void filter (Buf& b, float hz, float res, int type)   // 0 low, 1 band, 2 high
    {
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
        {
            SynthCore::Svf f;
            f.set (hz, res, (float) rate);
            auto* d = b.getWritePointer (ch);
            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                float l, bp, h;
                f.process (d[i], l, bp, h);
                d[i] = type == 0 ? l : type == 1 ? bp : h;
            }
        }
    }

    inline void lowpass (Buf& b, float hz, float res = 0.1f)   { filter (b, hz, res, 0); }
    inline void highpass (Buf& b, float hz, float res = 0.1f)  { filter (b, hz, res, 2); }
    inline void bandpass (Buf& b, float hz, float res = 0.3f)  { filter (b, hz, res, 1); }

    /** A small stereo room (Schroeder: parallel combs, then allpasses). Grows the buffer for the tail. */
    inline void room (Buf& b, float size, float mixAmount, float damp = 0.4f)
    {
        const double tail = 0.15 + size * 1.2;
        Buf out = toStereo (b);
        const int n = out.getNumSamples() + samples (tail);
        out.setSize (2, n, true, true, false);
        Buf dry; dry.makeCopyOf (out);

        static constexpr int combs[] = { 1116, 1188, 1277, 1356 };
        static constexpr int allpasses[] = { 556, 441 };
        const float fb = 0.70f + size * 0.26f;

        for (int ch = 0; ch < 2; ++ch)
        {
            const int spread = ch == 0 ? 0 : 23;
            std::vector<float> wet ((size_t) n, 0.0f);
            for (auto len : combs)
            {
                const int L = (int) ((len + spread) * (0.45 + size * 0.7));
                std::vector<float> line ((size_t) L, 0.0f);
                float lp = 0.0f;
                int p = 0;
                const float* in = dry.getReadPointer (ch);
                for (int i = 0; i < n; ++i)
                {
                    const float y = line[(size_t) p];
                    lp = y * (1.0f - damp) + lp * damp;
                    line[(size_t) p] = in[i] * 0.015f + lp * fb;
                    wet[(size_t) i] += y;
                    if (++p >= L) p = 0;
                }
            }
            for (auto len : allpasses)
            {
                const int L = len + spread;
                std::vector<float> line ((size_t) L, 0.0f);
                int p = 0;
                for (int i = 0; i < n; ++i)
                {
                    const float buf = line[(size_t) p];
                    const float y = -wet[(size_t) i] + buf;
                    line[(size_t) p] = wet[(size_t) i] + buf * 0.5f;
                    wet[(size_t) i] = y;
                    if (++p >= L) p = 0;
                }
            }
            auto* d = out.getWritePointer (ch);
            for (int i = 0; i < n; ++i)
                d[i] = d[i] + wet[(size_t) i] * mixAmount * 6.0f;
        }
        b = out;
    }

    inline Buf reversed (const Buf& b)
    {
        Buf r (b.getNumChannels(), b.getNumSamples());
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                r.setSample (ch, i, b.getSample (ch, b.getNumSamples() - 1 - i));
        return r;
    }

    /** Fade the end, trim silence after it, normalise to about -1 dBFS. */
    inline void finish (Buf& b)
    {
        // Trim the tail below -70 dB
        int last = b.getNumSamples() - 1;
        const float floor = 0.0003f * juce::jmax (0.0001f, b.getMagnitude (0, b.getNumSamples()));
        while (last > 64)
        {
            bool quiet = true;
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
                if (std::abs (b.getSample (ch, last)) > floor) { quiet = false; break; }
            if (! quiet) break;
            --last;
        }
        b.setSize (b.getNumChannels(), last + 2, true, true, false);

        const int n = b.getNumSamples();
        const int fade = juce::jmin (n, samples (0.006));
        for (int ch = 0; ch < b.getNumChannels(); ++ch)
            for (int i = 0; i < fade; ++i)
                b.setSample (ch, n - fade + i, b.getSample (ch, n - fade + i) * (1.0f - (float) i / (float) fade));

        const float peak = b.getMagnitude (0, n);
        if (peak > 0.0f)
            b.applyGain (0.89f / peak);
    }

    //==============================================================================
    // Building blocks

    /** Kick: a sine that falls from fStart to fEnd, with a click on top. */
    inline Buf kick (Ctx& c, float fEnd, float fStart, float sweep, float decay, double len, float click, float drive)
    {
        double phase = 0.0;
        auto clickNoise = mono (0.01, [&] (double t) { return c.noise() * (float) std::exp (-t * 600.0); });
        highpass (clickNoise, 3000.0f);
        auto body = mono (len * c.decay, [&] (double t)
        {
            const double f = (fEnd + (fStart - fEnd) * std::exp (-t * sweep)) * c.pitch;
            phase += twoPi * f / rate;
            return (float) (std::sin (phase) * std::exp (-t * decay / c.decay) + click * 0.8 * std::exp (-t * 900.0) * std::sin (phase * 7.0));
        });
        mix (body, clickNoise, click);
        saturate (body, drive);
        return body;
    }

    /** Sum of decaying sine partials (drums, shells, bars), each {frequency, amplitude, decay rate}. */
    struct Partial { float f, amp, decay; };

    inline Buf modal (Ctx& c, std::initializer_list<Partial> partials, double len, float pitchDrop = 0.0f, float dropRate = 30.0f)
    {
        std::vector<Partial> p (partials);
        std::vector<double> phase (p.size(), 0.0);
        return mono (len * c.decay, [&] (double t)
        {
            const double bend = 1.0 + pitchDrop * std::exp (-t * dropRate);
            double sum = 0.0;
            for (size_t k = 0; k < p.size(); ++k)
            {
                phase[k] += twoPi * p[k].f * c.pitch * bend / rate;
                sum += std::sin (phase[k]) * p[k].amp * std::exp (-t * p[k].decay / c.decay);
            }
            return (float) sum;
        });
    }

    /** Filtered noise with an attack and an exponential decay; stereo noise if wide. */
    inline Buf noise (Ctx& c, double len, float attack, float decay, bool wide = false)
    {
        auto env = [&] (double t) { return (attack > 0.0f ? (1.0 - std::exp (-t / attack)) : 1.0) * std::exp (-t * decay / c.decay); };
        if (wide)
            return stereo (len, [&] (double t, int) { return (float) (c.noise() * env (t)); });
        return mono (len, [&] (double t) { return (float) (c.noise() * env (t)); });
    }

    /** Several quick noise bursts, then a tail: a hand clap. */
    inline Buf clap (Ctx& c, int bursts, double spacing, float centre, float q, float tail, double len, bool wide)
    {
        auto env = [&] (double t)
        {
            double e = 0.0;
            for (int b = 0; b < bursts; ++b)
            {
                const double start = b * spacing * (1.0 + 0.15 * c.noise());
                if (t >= start) e = juce::jmax (e, std::exp (-(t - start) * 200.0));
            }
            const double tailStart = bursts * spacing;
            if (t >= tailStart) e = juce::jmax (e, 0.85 * std::exp (-(t - tailStart) * tail / c.decay));
            return e;
        };
        Buf b = wide ? stereo (len, [&] (double t, int) { return (float) (c.noise() * env (t)); })
                     : mono (len, [&] (double t) { return (float) (c.noise() * env (t)); });
        bandpass (b, centre * c.pitch, q);
        return b;
    }

    /** Metallic: detuned square waves at inharmonic ratios (hats, cymbals, cowbells). */
    inline Buf metal (Ctx& c, float scale, double len, float decay, std::initializer_list<float> freqs = { 205.3f, 304.4f, 369.6f, 522.7f, 540.0f, 800.0f })
    {
        std::vector<float> f (freqs);
        return mono (len, [&] (double t)
        {
            float sum = 0.0f;
            for (auto fr : f)
                sum += std::fmod (t * fr * scale * c.pitch, 1.0) < 0.5 ? 1.0f : -1.0f;
            return sum / (float) f.size() * (float) std::exp (-t * decay / c.decay);
        });
    }

    /** Two-operator FM: bells, metal, laser zaps. */
    inline Buf fm (Ctx& c, float f, float ratio, float index, double len, float decay, float indexDecay)
    {
        double pc = 0.0, pm = 0.0;
        return mono (len * c.decay, [&] (double t)
        {
            pm += twoPi * f * ratio * c.pitch / rate;
            pc += twoPi * f * c.pitch / rate;
            const double idx = index * std::exp (-t * indexDecay);
            return (float) (std::sin (pc + idx * std::sin (pm)) * std::exp (-t * decay / c.decay));
        });
    }

    /** A long 808 bass, optionally gliding down into the note. */
    inline Buf bass808 (Ctx& c, float f, double len, float glideSemis, float glideTime, float drive, float decay)
    {
        double phase = 0.0;
        auto b = mono (len * c.decay, [&] (double t)
        {
            const double semis = glideSemis * std::exp (-t / juce::jmax (0.001f, glideTime));
            const double fr = f * c.pitch * std::pow (2.0, semis / 12.0) * (1.0 + 0.5 * std::exp (-t * 40.0));
            phase += twoPi * fr / rate;
            return (float) (std::sin (phase) * std::exp (-t * decay / c.decay) * (1.0 - std::exp (-t * 2000.0)));
        });
        saturate (b, drive);
        return b;
    }

    /** A sung "hey": a buzzy source through vowel formants. */
    inline Buf vocal (Ctx& c, float f0, double len)
    {
        double phase = 0.0;
        auto src = mono (len, [&] (double t)
        {
            const double f = f0 * c.pitch * (1.0 + 0.04 * std::exp (-t * 20.0));
            phase += f / rate;
            phase -= std::floor (phase);
            const double env = (1.0 - std::exp (-t * 200.0)) * std::exp (-t * 9.0 / c.decay);
            return (float) ((2.0 * phase - 1.0) * env + c.noise() * 0.15 * env);
        });
        Buf out (1, src.getNumSamples());
        out.clear();
        for (auto [freq, gain] : { std::pair { 530.0f, 1.0f }, std::pair { 1840.0f, 0.6f }, std::pair { 2480.0f, 0.35f } })
        {
            Buf band; band.makeCopyOf (src);
            bandpass (band, freq, 0.85f);
            out.addFrom (0, 0, band, 0, 0, band.getNumSamples(), gain);
        }
        return out;
    }

    /** Shaker: grains of noise in a soft envelope. */
    inline Buf shaker (Ctx& c, double len, float attack, float decay, float hp, bool wide)
    {
        auto b = noise (c, len, attack, decay, wide);
        highpass (b, hp * c.pitch);
        return b;
    }

    /** Vinyl crackle: sparse clicks over a little hiss. */
    inline Buf crackle (Ctx& c, double len, float density)
    {
        auto b = mono (len, [&] (double t)
        {
            float x = c.noise() * 0.04f;
            if (c.rng.nextFloat() < density / (float) rate) x += c.noise() * 0.9f;
            return x * (float) std::exp (-t * 3.0);
        });
        bandpass (b, 2500.0f, 0.2f);
        return b;
    }

    //==============================================================================
    // The kits

    inline Buf trap (Ctx& c, int pad)
    {
        switch (pad)
        {
            case 0: return kick (c, 55.0f, 240.0f, 34.0f, 8.0f, 0.45, 0.9f, 2.4f);
            case 1:
            {
                auto body = modal (c, { { 200.0f, 0.7f, 26.0f }, { 365.0f, 0.4f, 34.0f } }, 0.3, 0.3f, 60.0f);
                auto wires = noise (c, 0.3, 0.0f, 16.0f, true);
                highpass (wires, 1600.0f); lowpass (wires, 12000.0f);
                auto snap = noise (c, 0.02, 0.0f, 250.0f);
                highpass (snap, 4000.0f);
                mix (body, wires, 1.1f); mix (body, snap, 0.8f);
                room (body, 0.15f, 0.08f, 0.5f);
                saturate (body, 1.6f);
                return body;
            }
            case 2: { auto b = clap (c, 4, 0.008, 1400.0f, 0.25f, 12.0f, 0.4, true); room (b, 0.2f, 0.12f); return b; }
            case 3: { auto b = metal (c, 2.4f, 0.07, 95.0f); mix (b, noise (c, 0.07, 0.0f, 110.0f), 0.4f); highpass (b, 8000.0f); return b; }
            case 4: { auto b = metal (c, 2.4f, 0.5, 9.0f); mix (b, noise (c, 0.5, 0.0f, 9.0f), 0.4f); highpass (b, 7000.0f); return toStereo (b); }
            case 5: case 6: case 7:
            {
                const float f = pad == 5 ? 160.0f : pad == 6 ? 240.0f : 360.0f;
                auto b = modal (c, { { f, 1.0f, 18.0f }, { f * 2.7f, 0.25f, 40.0f } }, 0.35, 0.25f, 50.0f);
                mix (b, noise (c, 0.015, 0.0f, 300.0f), 0.3f);
                saturate (b, 1.4f);
                return b;
            }
            case 8:
            {
                auto b = modal (c, { { 1750.0f, 0.6f, 95.0f }, { 520.0f, 1.0f, 70.0f } }, 0.09);
                auto n = noise (c, 0.03, 0.0f, 260.0f); highpass (n, 2500.0f);
                mix (b, n, 0.7f);
                return b;
            }
            case 9: { auto b = vocal (c, 190.0f, 0.3); room (b, 0.25f, 0.15f); return b; }
            case 10:
            {
                auto crash = metal (c, 2.2f, 1.3, 2.4f);
                mix (crash, noise (c, 1.3, 0.0f, 2.4f), 0.7f);
                highpass (crash, 4500.0f);
                auto r = reversed (crash);
                r.applyGainRamp (0, r.getNumSamples() - samples (0.03), 1.0f, 1.0f);
                return toStereo (r);
            }
            case 11: { auto b = fm (c, 2600.0f, 1.41f, 2.2f, 1.4, 3.5f, 6.0f); room (b, 0.4f, 0.2f); return b; }
            case 12: return shaker (c, 0.14, 0.006f, 30.0f, 7000.0f, true);
            case 13:
            {
                auto b = noise (c, 0.05, 0.0f, 180.0f);
                bandpass (b, 2200.0f, 0.4f);
                auto tick = noise (c, 0.004, 0.0f, 1200.0f); highpass (tick, 6000.0f);
                mix (b, tick, 1.0f);
                room (b, 0.15f, 0.2f, 0.3f);
                return b;
            }
            case 14: return bass808 (c, 49.0f, 2.2, 7.0f, 0.06f, 3.2f, 1.6f);
            default:
            {
                double phase = 0.0;
                return mono (0.18, [&] (double t)
                {
                    const double f = (150.0 + 4200.0 * std::exp (-t * 55.0)) * c.pitch;
                    phase += twoPi * f / rate;
                    return (float) (std::sin (phase + 2.0 * std::sin (phase * 0.5)) * std::exp (-t * 16.0));
                });
            }
        }
    }

    inline Buf lofi (Ctx& c, int pad)
    {
        Buf b;
        switch (pad)
        {
            case 0: b = kick (c, 52.0f, 140.0f, 25.0f, 9.0f, 0.5, 0.35f, 1.6f); lowpass (b, 4000.0f); break;
            case 1:
            {
                b = modal (c, { { 180.0f, 0.8f, 24.0f }, { 290.0f, 0.4f, 30.0f } }, 0.4, 0.2f, 40.0f);
                auto wires = noise (c, 0.4, 0.0f, 11.0f);
                bandpass (wires, 3500.0f, 0.15f);
                mix (b, wires, 1.2f);
                room (b, 0.25f, 0.2f, 0.65f);
                lowpass (b, 7000.0f);
                break;
            }
            case 2: b = clap (c, 3, 0.011, 1000.0f, 0.2f, 13.0f, 0.4, false); room (b, 0.3f, 0.2f, 0.6f); lowpass (b, 6000.0f); break;
            case 3: b = metal (c, 1.9f, 0.09, 60.0f); highpass (b, 5000.0f); lowpass (b, 10000.0f); break;
            case 4: b = metal (c, 1.9f, 0.6, 7.0f); highpass (b, 5000.0f); lowpass (b, 9000.0f); break;
            case 5: case 6: case 7:
            {
                const float f = pad == 5 ? 90.0f : pad == 6 ? 130.0f : 180.0f;
                b = modal (c, { { f, 1.0f, 8.0f }, { f * 1.5f, 0.3f, 14.0f }, { f * 2.0f, 0.15f, 20.0f } }, 0.6, 0.3f, 25.0f);
                lowpass (b, 3000.0f);
                break;
            }
            case 8:
            {
                b = modal (c, { { 820.0f, 1.0f, 60.0f }, { 1900.0f, 0.4f, 90.0f } }, 0.12);
                auto n = noise (c, 0.01, 0.0f, 400.0f); bandpass (n, 2000.0f);
                mix (b, n, 0.6f);
                saturate (b, 2.0f);
                break;
            }
            case 9: b = crackle (c, 0.9, 60.0f); break;
            case 10: b = metal (c, 2.0f, 1.6, 2.6f); mix (b, noise (c, 1.6, 0.0f, 2.6f), 0.6f); highpass (b, 3500.0f); lowpass (b, 8000.0f); break;
            case 11: b = metal (c, 2.8f, 1.4, 3.0f); highpass (b, 4000.0f); lowpass (b, 9000.0f); break;
            case 12: b = shaker (c, 0.16, 0.01f, 26.0f, 4500.0f, false); lowpass (b, 9000.0f); break;
            case 13: b = modal (c, { { 1700.0f, 1.0f, 55.0f } }, 0.1); break;
            case 14: b = bass808 (c, 45.0f, 1.0, 0.0f, 0.01f, 1.8f, 3.0f); lowpass (b, 1500.0f); break;
            default:
            {
                double phase = 0.0;
                b = mono (0.5, [&] (double t)
                {
                    const double slow = std::max (0.0, 1.0 - t / 0.45);
                    phase += twoPi * 400.0 * c.pitch * slow * slow / rate;
                    return (float) (std::tanh (std::sin (phase) * 2.0 + std::sin (phase * 2.0) * 0.5) * slow);
                });
                lowpass (b, 3000.0f);
                break;
            }
        }

        // Through an old 12-bit sampler: crushed, warm, with a little dust
        saturate (b, 1.4f);
        crush (b, 12, 2);
        mix (b, crackle (c, (double) b.getNumSamples() / rate, 20.0f), 0.05f);
        lowpass (b, 11000.0f);
        return b;
    }

    inline Buf hyperpop (Ctx& c, int pad)
    {
        switch (pad)
        {
            case 0: { auto b = kick (c, 60.0f, 300.0f, 60.0f, 9.0f, 0.35, 1.0f, 4.0f); clip (b, 2.5f); crush (b, 8, 1); return b; }
            case 1:
            {
                auto body = modal (c, { { 230.0f, 0.8f, 20.0f } }, 0.13, 0.4f, 80.0f);
                auto n = noise (c, 0.13, 0.0f, 3.0f, true);
                highpass (n, 1200.0f);
                mix (body, n, 1.0f);
                clip (body, 3.0f);
                // Gate: hard stop
                const int stop = samples (0.11);
                for (int ch = 0; ch < body.getNumChannels(); ++ch)
                    for (int i = stop; i < body.getNumSamples(); ++i)
                        body.setSample (ch, i, body.getSample (ch, i) * std::max (0.0f, 1.0f - (float) (i - stop) / 200.0f));
                return body;
            }
            case 2:
            {
                auto b = clap (c, 6, 0.006, 1600.0f, 0.3f, 18.0f, 0.3, true);
                Buf fast (b.getNumChannels(), b.getNumSamples() / 2);
                for (int ch = 0; ch < b.getNumChannels(); ++ch)
                    for (int i = 0; i < fast.getNumSamples(); ++i)
                        fast.setSample (ch, i, b.getSample (ch, i * 2));
                mix (b, fast, 0.6f);
                crush (b, 10, 2);
                return b;
            }
            case 3: { auto b = fm (c, 9000.0f, 1.41f, 3.0f, 0.06, 110.0f, 40.0f); highpass (b, 6000.0f); return b; }
            case 4: { auto b = fm (c, 7400.0f, 1.53f, 4.0f, 0.5, 12.0f, 4.0f); mix (b, noise (c, 0.5, 0.0f, 12.0f), 0.3f); highpass (b, 6000.0f); return toStereo (b); }
            case 5: case 6: case 7:
            {
                const float f = pad == 5 ? 220.0f : pad == 6 ? 330.0f : 495.0f;
                double phase = 0.0;
                return mono (0.16, [&] (double t)
                {
                    const double fr = (t < 0.04 ? f * 2.0 : f) * c.pitch;
                    phase += fr / rate; phase -= std::floor (phase);
                    return (float) ((phase < 0.5 ? 0.6 : -0.6) * std::exp (-t * 14.0));
                });
            }
            case 8:
            {
                auto b = mono (0.25, [&] (double t)
                {
                    const double grain = std::fmod (t, 0.031);
                    return (float) (c.noise() * (grain < 0.004 ? 1.0 : 0.0) * std::exp (-t * 8.0));
                });
                bandpass (b, 3000.0f, 0.5f);
                crush (b, 6, 3);
                return b;
            }
            case 9: { auto b = fm (c, 880.0f, 3.5f, 4.0f, 1.2, 4.0f, 3.0f); room (b, 0.35f, 0.18f); return b; }
            case 10: { auto b = noise (c, 1.3, 0.0f, 2.8f, true); highpass (b, 2500.0f); crush (b, 6, 6); return b; }
            case 11:
            {
                auto b = mono (0.3, [&] (double t) { return (float) (c.noise() * (t / 0.3) * (t / 0.3)); });
                double phase = 0.0;
                auto sweep = mono (0.3, [&] (double t) { phase += twoPi * (500.0 + 6000.0 * t / 0.3) / rate; return (float) (std::sin (phase) * t / 0.3); });
                bandpass (b, 5000.0f, 0.4f);
                mix (b, sweep, 0.4f);
                return b;
            }
            case 12: { auto b = shaker (c, 0.12, 0.004f, 35.0f, 6000.0f, true); crush (b, 8, 2); return b; }
            case 13: return modal (c, { { 3000.0f, 1.0f, 30.0f } }, 0.25, 0.08f, 40.0f);
            case 14: { auto b = bass808 (c, 52.0f, 1.6, 12.0f, 0.05f, 7.0f, 2.0f); clip (b, 1.8f); return b; }
            default:
            {
                double phase = 0.0;
                auto b = mono (0.22, [&] (double t)
                {
                    const double f = (120.0 + 7000.0 * std::exp (-t * 70.0)) * c.pitch;
                    phase += twoPi * f / rate;
                    return (float) (std::sin (phase + 4.0 * std::sin (phase * 1.5)) * std::exp (-t * 12.0));
                });
                crush (b, 9, 2);
                return b;
            }
        }
    }

    inline Buf amapiano (Ctx& c, int pad)
    {
        auto conga = [&] (float f, bool slap)
        {
            auto b = modal (c, { { f, 1.0f, 14.0f }, { f * 1.52f, 0.5f, 22.0f }, { f * 1.98f, 0.3f, 30.0f }, { f * 2.44f, 0.15f, 40.0f } }, 0.5, 0.06f, 50.0f);
            auto hit = noise (c, 0.02, 0.0f, slap ? 150.0f : 400.0f);
            bandpass (hit, slap ? 2500.0f : 1200.0f, 0.3f);
            mix (b, hit, slap ? 1.2f : 0.4f);
            room (b, 0.2f, 0.08f);
            return b;
        };

        switch (pad)
        {
            case 0: { auto b = kick (c, 48.0f, 110.0f, 20.0f, 7.0f, 0.6, 0.2f, 1.5f); lowpass (b, 3500.0f); return b; }
            case 1:
            {
                auto b = modal (c, { { 330.0f, 0.7f, 40.0f }, { 520.0f, 0.3f, 50.0f } }, 0.18);
                auto n = noise (c, 0.18, 0.0f, 25.0f); highpass (n, 2000.0f);
                mix (b, n, 0.9f);
                room (b, 0.2f, 0.1f);
                return b;
            }
            case 2: { auto b = clap (c, 3, 0.01, 1200.0f, 0.22f, 15.0f, 0.4, true); room (b, 0.3f, 0.2f); return b; }
            case 3: { auto b = metal (c, 2.3f, 0.06, 100.0f); highpass (b, 8500.0f); return b; }
            case 4: { auto b = metal (c, 2.3f, 0.35, 13.0f); highpass (b, 7500.0f); return b; }
            case 5: return conga (170.0f, false);
            case 6: return conga (230.0f, false);
            case 7: return conga (310.0f, true);
            case 8: { auto b = modal (c, { { 1500.0f, 0.7f, 80.0f }, { 600.0f, 1.0f, 60.0f } }, 0.1); room (b, 0.2f, 0.12f); return b; }
            case 9:
            {
                auto lo = fm (c, 880.0f, 2.5f, 1.5f, 0.35, 10.0f, 15.0f);
                auto hi = fm (c, 1320.0f, 2.5f, 1.5f, 0.35, 12.0f, 15.0f);
                mix (lo, hi, 0.7f);
                return lo;
            }
            case 10: { auto b = metal (c, 2.5f, 2.0, 2.2f); mix (b, noise (c, 2.0, 0.0f, 2.2f), 0.6f); highpass (b, 4500.0f); return toStereo (b); }
            case 11:
            {
                auto b = metal (c, 3.2f, 1.6, 2.8f); highpass (b, 5000.0f);
                mix (b, modal (c, { { 2300.0f, 0.4f, 6.0f } }, 1.6));
                return b;
            }
            case 12: return shaker (c, 0.22, 0.009f, 18.0f, 5500.0f, true);
            case 13: return modal (c, { { 2500.0f, 1.0f, 60.0f } }, 0.09);
            case 14:
            {
                // The log drum: a woody, saturated, pitched bass hit
                double phase = 0.0;
                auto b = mono (0.8 * c.decay, [&] (double t)
                {
                    const double f = 55.0 * c.pitch * (1.0 + 1.1 * std::exp (-t * 45.0));
                    phase += twoPi * f / rate;
                    const double env = std::exp (-t * 4.5 / c.decay);
                    return (float) ((std::sin (phase) + 0.3 * std::sin (phase * 2.0) + 0.12 * std::sin (phase * 3.0)) * env);
                });
                auto knock = noise (c, 0.015, 0.0f, 350.0f);
                bandpass (knock, 900.0f, 0.4f);
                mix (b, knock, 0.5f);
                saturate (b, 2.6f);
                lowpass (b, 2500.0f);
                return b;
            }
            default:
            {
                double phase = 0.0;
                auto b = mono (0.35, [&] (double t)
                {
                    const double f = 2400.0 * c.pitch * (1.0 + 0.02 * std::sin (twoPi * 30.0 * t));
                    phase += twoPi * f / rate;
                    const double env = (1.0 - std::exp (-t * 150.0)) * std::exp (-t * 7.0);
                    return (float) ((std::sin (phase) + c.noise() * 0.12) * env);
                });
                room (b, 0.3f, 0.15f);
                return b;
            }
        }
    }

    inline Buf acoustic (Ctx& c, int pad)
    {
        // A small room around every piece
        auto inRoom = [&] (Buf b, float amount) { room (b, 0.3f, amount, 0.55f); return b; };

        auto cymbal = [&] (double len, float decay, float lowest, int count, float hp)
        {
            std::vector<Partial> parts;
            juce::Random r (77);   // the same cymbal every time; variations come from the noise
            for (int k = 0; k < count; ++k)
                parts.push_back ({ lowest * std::pow (2.0f, r.nextFloat() * 4.5f), 0.4f + r.nextFloat() * 0.6f, decay * (0.6f + r.nextFloat() * 0.8f) });
            std::vector<double> ph (parts.size(), 0.0);
            auto b = mono (len * c.decay, [&] (double t)
            {
                double s = 0.0;
                for (size_t k = 0; k < parts.size(); ++k)
                {
                    ph[k] += twoPi * parts[k].f * c.pitch / rate;
                    s += std::sin (ph[k]) * parts[k].amp * std::exp (-t * parts[k].decay / c.decay);
                }
                return (float) (s / std::sqrt ((double) parts.size()) + c.noise() * 0.35 * std::exp (-t * decay * 1.4 / c.decay));
            });
            highpass (b, hp);
            return b;
        };

        switch (pad)
        {
            case 0:
            {
                auto beater = noise (c, 0.03, 0.0f, 160.0f); lowpass (beater, 3000.0f);
                auto b = modal (c, { { 60.0f, 1.0f, 9.0f }, { 110.0f, 0.5f, 16.0f }, { 172.0f, 0.25f, 24.0f } }, 0.5, 0.25f, 40.0f);
                mix (b, beater, 0.6f);
                saturate (b, 1.3f);
                return inRoom (b, 0.06f);
            }
            case 1:
            {
                auto b = modal (c, { { 185.0f, 1.0f, 18.0f }, { 295.0f, 0.5f, 24.0f }, { 340.0f, 0.4f, 28.0f }, { 420.0f, 0.3f, 32.0f } }, 0.35, 0.12f, 50.0f);
                auto wires = noise (c, 0.35, 0.002f, 10.0f);
                bandpass (wires, 5500.0f, 0.15f);
                mix (b, wires, 1.3f, 0.002);
                auto stick = noise (c, 0.006, 0.0f, 800.0f); highpass (stick, 3000.0f);
                mix (b, stick, 0.5f);
                return inRoom (b, 0.12f);
            }
            case 2: return inRoom (clap (c, 3, 0.012, 1100.0f, 0.2f, 13.0f, 0.4, true), 0.25f);
            case 3:
            {
                auto b = cymbal (0.12, 55.0f, 3000.0f, 14, 6000.0f);
                auto chick = modal (c, { { 420.0f, 0.3f, 80.0f } }, 0.04);
                mix (b, chick, 0.6f);
                return inRoom (b, 0.05f);
            }
            case 4: return inRoom (cymbal (1.2, 3.2f, 2800.0f, 18, 5000.0f), 0.08f);
            case 5: case 6: case 7:
            {
                const float f = pad == 5 ? 82.0f : pad == 6 ? 118.0f : 160.0f;
                auto b = modal (c, { { f, 1.0f, 6.5f }, { f * 1.5f, 0.4f, 10.0f }, { f * 2.0f, 0.25f, 14.0f }, { f * 2.43f, 0.15f, 18.0f } }, 0.9, 0.12f, 18.0f);
                auto stick = noise (c, 0.01, 0.0f, 500.0f); bandpass (stick, 2500.0f);
                mix (b, stick, 0.35f);
                return inRoom (b, 0.12f);
            }
            case 8:
            {
                auto b = modal (c, { { 1600.0f, 0.6f, 70.0f }, { 450.0f, 1.0f, 55.0f }, { 2900.0f, 0.3f, 90.0f } }, 0.12);
                return inRoom (b, 0.15f);
            }
            case 9:
            {
                auto b = modal (c, { { 562.0f, 1.0f, 9.0f }, { 845.0f, 0.7f, 11.0f } }, 0.5);
                auto hit = noise (c, 0.006, 0.0f, 800.0f); highpass (hit, 2000.0f);
                mix (b, hit, 0.4f);
                return inRoom (b, 0.1f);
            }
            case 10: return toStereo (inRoom (cymbal (2.4, 1.6f, 300.0f, 40, 400.0f), 0.08f));
            case 11:
            {
                auto b = cymbal (2.6, 1.4f, 1800.0f, 24, 1500.0f);
                mix (b, modal (c, { { 2300.0f, 0.5f, 3.0f }, { 3450.0f, 0.3f, 4.0f } }, 2.6));
                return inRoom (b, 0.06f);
            }
            case 12:
            {
                auto b = mono (0.2, [&] (double t)
                {
                    const double env = (1.0 - std::exp (-t * 80.0)) * std::exp (-t * 20.0);
                    return (float) ((c.rng.nextFloat() < 0.25f ? c.noise() : 0.0f) * env);
                });
                bandpass (b, 5500.0f, 0.15f);
                return inRoom (b, 0.08f);
            }
            case 13: return inRoom (modal (c, { { 2480.0f, 1.0f, 45.0f }, { 6800.0f, 0.3f, 80.0f } }, 0.12), 0.12f);
            case 14:
            {
                auto b = modal (c, { { 68.0f, 1.0f, 3.2f }, { 104.0f, 0.4f, 6.0f }, { 141.0f, 0.2f, 9.0f } }, 1.4, 0.18f, 15.0f);
                auto beater = noise (c, 0.03, 0.0f, 150.0f); lowpass (beater, 2000.0f);
                mix (b, beater, 0.4f);
                return inRoom (b, 0.12f);
            }
            default:
            {
                auto b = modal (c, { { 110.0f, 1.0f, 14.0f }, { 180.0f, 0.4f, 22.0f } }, 0.4);
                auto wires = noise (c, 0.3, 0.003f, 14.0f); bandpass (wires, 4500.0f, 0.15f);
                mix (b, wires, 0.8f);
                auto slap = noise (c, 0.01, 0.0f, 600.0f); bandpass (slap, 1800.0f);
                mix (b, slap, 0.6f);
                return inRoom (b, 0.12f);
            }
        }
    }

    inline Buf techno (Ctx& c, int pad)
    {
        switch (pad)
        {
            case 0: return kick (c, 50.0f, 320.0f, 45.0f, 6.0f, 0.55, 0.7f, 2.2f);
            case 1:
            {
                auto b = modal (c, { { 180.0f, 0.8f, 25.0f }, { 330.0f, 0.5f, 30.0f } }, 0.35, 0.2f, 60.0f);
                auto n = noise (c, 0.35, 0.0f, 9.0f); highpass (n, 1500.0f);
                mix (b, n, 1.0f);
                return b;
            }
            case 2: { auto b = clap (c, 3, 0.01, 1100.0f, 0.22f, 11.0f, 0.4, true); room (b, 0.35f, 0.18f); return b; }
            case 3: { auto b = metal (c, 2.0f, 0.1, 70.0f); highpass (b, 8000.0f); return b; }
            case 4: { auto b = metal (c, 2.0f, 0.7, 5.0f); highpass (b, 7000.0f); return b; }
            case 5: case 6: case 7:
            {
                const float f = pad == 5 ? 100.0f : pad == 6 ? 150.0f : 200.0f;
                return kick (c, f, f * 2.2f, 30.0f, 7.0f, 0.4, 0.1f, 1.3f);
            }
            case 8: { auto b = modal (c, { { 1720.0f, 0.6f, 90.0f }, { 490.0f, 1.0f, 70.0f } }, 0.08); return b; }
            case 9:
            {
                auto b = fm (c, 340.0f, 1.83f, 6.0f, 0.7, 6.0f, 9.0f);
                saturate (b, 3.0f);
                room (b, 0.4f, 0.2f);
                return b;
            }
            case 10: { auto b = metal (c, 2.3f, 1.8, 2.0f); mix (b, noise (c, 1.8, 0.0f, 2.0f), 0.7f); highpass (b, 4000.0f); return toStereo (b); }
            case 11: { auto b = metal (c, 3.4f, 1.6, 2.4f); highpass (b, 5500.0f); return b; }
            case 12: return shaker (c, 0.12, 0.005f, 32.0f, 6500.0f, false);
            case 13: return modal (c, { { 1200.0f, 1.0f, 40.0f } }, 0.12);
            case 14:
            {
                auto b = kick (c, 48.0f, 200.0f, 35.0f, 5.0f, 0.5, 0.3f, 2.0f);
                room (b, 0.9f, 0.6f, 0.3f);
                lowpass (b, 220.0f, 0.3f);
                saturate (b, 2.0f);
                return b;
            }
            default:
            {
                auto b = noise (c, 0.8, 0.0f, 5.0f, true);
                for (int ch = 0; ch < b.getNumChannels(); ++ch)
                {
                    SynthCore::Svf f;
                    auto* d = b.getWritePointer (ch);
                    for (int i = 0; i < b.getNumSamples(); ++i)
                    {
                        if ((i & 15) == 0)
                            f.set (400.0f + 7600.0f * (float) std::exp (-(double) i / rate * 6.0), 0.8f, (float) rate);
                        float l, bp, h;
                        f.process (d[i], l, bp, h);
                        d[i] = bp;
                    }
                }
                room (b, 0.5f, 0.2f);
                return b;
            }
        }
    }

    /** Renders kit (1..), pad, variation. Kit 0 (Classic) is DrumsDevice's own. */
    inline Buf render (int kit, int pad, int variation)
    {
        Ctx c;
        c.rng.setSeed ((juce::int64) (kit * 1000 + pad * 10 + variation + 1));
        c.pitch = 1.0f + (float) (variation - 1) * 0.012f;
        c.decay = 1.0f + (float) (variation - 1) * 0.05f;

        Buf b;
        switch (kit)
        {
            case 1:  b = trap (c, pad); break;
            case 2:  b = lofi (c, pad); break;
            case 3:  b = hyperpop (c, pad); break;
            case 4:  b = amapiano (c, pad); break;
            case 5:  b = acoustic (c, pad); break;
            default: b = techno (c, pad); break;
        }
        finish (b);
        return b;
    }
}
} // namespace elsewhere
