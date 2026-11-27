// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthCore.h"
#include "SpaceDsp.h"
#include "EngineControl.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace elsewhere
{
//==============================================================================
/**
    The sound of Elsewhere Organic, on its own: two layers of recorded sound played through
    grain clouds, a tuned resonant body, expression, warmth and the space. Plain C++ with no
    JUCE, so it builds anywhere. The Organic device plays through it, and so can anything
    else that wants to sound the same.

    The engine never loads files. It plays Sources: zones of audio the caller owns, handed
    over with setSource(). It can also build its four built-in sources (Air Bow, Breath
    Choir, Glass, Wind) into storage of its own, so it plays with no sample pack at all.

    Call prepare() first, then noteOn and noteOff as notes come, and render() for each block.
    None of those allocates or locks. Everything random starts from fixed seeds on every
    prepare(), so the same notes always render the same audio. Changing a value in params
    between renders takes effect in the next render.
*/
class OrganicEngine
{
public:
    static constexpr int maxVoices = 12;
    static constexpr int chunk = 64;
    static constexpr int numLayers = 2;

    //==============================================================================
    // Sources

    /** One recording: a stereo pair of channels at the source's rate, for a root note, a
        dynamic layer (0 the softest) and a take (round robin). */
    struct Zone
    {
        int root = 60, layer = 0, take = 0;
        const float* data[2] {};    // left and right (the same pointer twice for mono); caller-owned
        int length = 0;             // samples per channel
    };

    /**
        A playable sound: an array of zones and what the grain player needs to know about them.
        Nothing here is owned: the zones and their audio belong to whoever made the Source.
    */
    struct Source
    {
        const Zone* zones = nullptr;
        int numZones = 0;
        double rate = 48000.0;      // the zones' sample rate
        int layers = 1, takes = 1;  // dynamic layers and round-robin takes (finish() works them out)
        float gain = 1.0f;          // loudness normalisation across sources (finish() works it out)
        bool sustains = true;       // holds while the key is down (bowed, blown, textures); false rings out (plucked, struck)
        bool pitched = true;

        /** The zone to play for a note, in a dynamic layer, for a take. */
        const Zone* pick (int note, int layer, int take) const noexcept
        {
            const Zone* best = nullptr;
            int bestScore = std::numeric_limits<int>::max();
            for (int i = 0; i < numZones; ++i)
            {
                // Prefer the right layer and take; then the closest root, slightly preferring
                // roots below the note (shifting a sample up sounds more natural than down)
                const auto& z = zones[i];
                int score = std::abs (z.root - note) * 4 + (z.root > note ? 1 : 0);
                if (z.layer != layer) score += 2000 + std::abs (z.layer - layer) * 100;
                if (z.take != take % dspmath::jmax (1, takes)) score += 500;
                if (score < bestScore) { bestScore = score; best = &z; }
            }
            return best;
        }

        double lengthSeconds() const noexcept
        {
            int n = 0;
            for (int i = 0; i < numZones; ++i) n = dspmath::jmax (n, zones[i].length);
            return n / rate;
        }

        /** Works out layers, takes and gain from the zones (RMS over the first two seconds of
            the loudest layer). Doesn't allocate. */
        void finish() noexcept
        {
            layers = takes = 1;
            for (int i = 0; i < numZones; ++i)
            {
                layers = dspmath::jmax (layers, zones[i].layer + 1);
                takes = dspmath::jmax (takes, zones[i].take + 1);
            }

            double sum = 0.0; int count = 0;
            for (int i = 0; i < numZones; ++i)
            {
                const auto& z = zones[i];
                if (z.layer != layers - 1) continue;
                const int n = dspmath::jmin (z.length, (int) (rate * 2.0));
                if (n <= 0) continue;
                sum += rmsOf (z.data[0], n) + rmsOf (z.data[1], n);
                count += 2;
            }
            const auto rms = count > 0 ? (float) (sum / count) : 0.1f;
            gain = dspmath::jlimit (0.5f, 24.0f, 0.36f / dspmath::jmax (1.0e-4f, rms));
        }

        static float rmsOf (const float* d, int n) noexcept
        {
            double sum = 0.0;
            for (int i = 0; i < n; ++i) { auto s = d[i]; sum += s * s; }
            return static_cast<float> (std::sqrt (sum / n));
        }
    };

    /**
        Plays a source on a layer (0 is A, 1 is B). The engine keeps a pointer to `source`: it,
        its zones and their audio must stay valid until another setSource() has replaced it on
        that layer and the engine has moved off it (voices still on the old source fade out
        over 30 ms, at the next render; keeping the old one a second or more is plenty).
        Doesn't allocate or lock, so it may be called while the engine renders.
    */
    void setSource (int layer, const Source& source) noexcept
    {
        if (layer >= 0 && layer < numLayers)
            live[(size_t) layer].store (&source);
    }

    void clearSource (int layer) noexcept
    {
        if (layer >= 0 && layer < numLayers)
            live[(size_t) layer].store (nullptr);
    }

    const Source* getSource (int layer) const noexcept
    {
        return layer >= 0 && layer < numLayers ? live[(size_t) layer].load() : nullptr;
    }

    //==============================================================================
    // Built-in sources: synthesized, so the engine plays with no pack installed

    struct BuiltinInfo { const char* id; const char* name; const char* family; };

    static const std::vector<BuiltinInfo>& builtins()
    {
        static const std::vector<BuiltinInfo> all = {
            { "bow",   "Air Bow",      "Bowed" },
            { "choir", "Breath Choir", "Breath" },
            { "glass", "Glass",        "Struck" },
            { "wind",  "Wind",         "Texture" },
        };
        return all;
    }

    /** The built-in source (its ID) that stands in for a sound of a family. */
    static const char* fallbackFor (const char* family)
    {
        auto is = [family] (const char* f) { return std::strcmp (family, f) == 0; };
        if (is ("Breath") || is ("Keys")) return "choir";
        if (is ("Plucked") || is ("Struck")) return "glass";
        if (is ("Texture")) return "wind";
        return "bow";
    }

    /** A built-in source with its audio. */
    struct BuiltSource
    {
        std::string id, name, family;
        std::vector<std::array<std::vector<float>, 2>> audio;   // per zone, left and right
        std::vector<Zone> zones;
        Source source;
    };

    /** Synthesizes a built-in source by ID ("bow", "choir", "glass" or "wind"), or nullptr.
        Allocates (about 23 MB for the bowed and breathed ones): call it off the audio thread. */
    static std::unique_ptr<BuiltSource> buildSource (const char* id)
    {
        const double rate = 48000.0;
        const int len = (int) (rate * 6.0);
        const BuiltinInfo* info = nullptr;
        for (auto& b : builtins())
            if (std::strcmp (b.id, id) == 0) info = &b;
        if (info == nullptr)
            return {};

        auto src = std::make_unique<BuiltSource>();
        src->id = info->id; src->name = info->name; src->family = info->family;
        const std::string sid = id;
        SynthCore::Random rng ((std::int64_t) seedFor (id));
        bool pitched = true;

        auto addZone = [&] (int root, int layer)
        {
            Zone z;
            z.root = root; z.layer = layer;
            z.length = len;
            src->zones.push_back (z);
            src->audio.push_back ({ std::vector<float> ((size_t) len, 0.0f), std::vector<float> ((size_t) len, 0.0f) });
            return src->audio.size() - 1;
        };

        if (sid == "bow" || sid == "choir")
        {
            const bool choir = sid == "choir";
            for (int root = 36; root <= 84; root += 12)
                for (int layer = 0; layer < 2; ++layer)
                {
                    const float f0 = 440.0f * std::pow (2.0f, ((float) root - 69.0f) / 12.0f);
                    auto table = choir
                        ? (layer == 0 ? cycle (f0, { { 300.0f, 1.0f }, { 870.0f, 0.5f }, { 2240.0f, 0.15f } }, 0.1f, (float) rate)
                                      : cycle (f0, { { 700.0f, 1.0f }, { 1220.0f, 0.8f }, { 2600.0f, 0.4f } }, 0.6f, (float) rate))
                        : cycle (f0, { { 280.0f, 0.9f }, { 1050.0f, 0.6f }, { 2700.0f, 0.5f + 0.4f * (float) layer } }, layer == 0 ? 0.25f : 0.85f, (float) rate);

                    auto& audio = src->audio[addZone (root, layer)];
                    SynthCore::Svf breathL, breathR;
                    breathL.set (choir ? 2400.0f : 3200.0f, 0.3f, (float) rate);
                    breathR.set (choir ? 2600.0f : 3500.0f, 0.3f, (float) rate);
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto* d = audio[(size_t) ch].data();
                        double ph = nextDouble (rng);
                        const float detune = ch == 0 ? -0.04f : 0.04f;
                        float noiseLp = 0.0f;
                        for (int i = 0; i < len; ++i)
                        {
                            const float t = (float) i / (float) rate;
                            const float vib = dspmath::jmin (1.0f, dspmath::jmax (0.0f, t - 0.45f) * 1.5f)
                                              * 0.09f * std::sin (dspmath::MathConstants<float>::twoPi * (5.1f + 0.2f * (float) ch) * t);
                            const double inc = f0 * std::pow (2.0f, (vib + detune) / 12.0f) / rate;
                            ph += inc; ph -= std::floor (ph);
                            const float x = (float) (ph * 2048.0);
                            const int xi = (int) x;
                            const float tone = table[(size_t) xi] + (table[(size_t) ((xi + 1) & 2047)] - table[(size_t) xi]) * (x - (float) xi);
                            const float w = rng.nextFloat() * 2.0f - 1.0f;
                            noiseLp += (w - noiseLp) * 0.5f;
                            const float breath = (ch == 0 ? breathL : breathR).lowpass (noiseLp) * (0.5f + 0.5f * tone);
                            const float swellIn = dspmath::jmin (1.0f, t / (choir ? 0.18f : 0.09f));
                            d[i] = (tone * 0.32f + breath * (choir ? 0.22f : 0.12f) * (1.2f - 0.4f * (float) layer)) * swellIn * (0.55f + 0.45f * (float) layer);
                        }
                    }
                    fadeEdges (audio, rate, 0.002f, 0.3f);
                }
        }
        else if (sid == "glass")
        {
            static const float partials[] = { 1.0f, 2.32f, 4.25f, 6.63f, 9.38f, 12.53f };
            for (int root = 48; root <= 96; root += 12)
            {
                const float f0 = 440.0f * std::pow (2.0f, ((float) root - 69.0f) / 12.0f);
                auto& audio = src->audio[addZone (root, 0)];
                for (int k = 0; k < 6; ++k)
                {
                    const float f = f0 * partials[k];
                    if (f > rate * 0.45) break;
                    const float amp = 0.35f / (1.0f + (float) k * 0.9f);
                    const float decay = 4.5f / (1.0f + (float) k * 0.8f) * (1.0f - 0.004f * (float) (root - 48));
                    const float ph = rng.nextFloat() * 6.28f;
                    for (int ch = 0; ch < 2; ++ch)
                    {
                        auto* d = audio[(size_t) ch].data();
                        const float beat = 1.0f + (ch == 0 ? -0.00002f : 0.00002f) * (float) (k + 1);
                        const double w = dspmath::MathConstants<double>::twoPi * f * beat / rate;
                        for (int i = 0; i < len; ++i)
                            d[i] += amp * std::exp (-(float) i / (float) rate / decay) * (float) std::sin (w * i + ph);
                    }
                }
                fadeEdges (audio, rate, 0.0008f, 0.3f);
            }
        }
        else    // wind
        {
            pitched = false;
            auto& audio = src->audio[addZone (60, 0)];
            for (int ch = 0; ch < 2; ++ch)
            {
                SynthCore::Svf a, b2;
                SynthCore::Noise noise;
                noise.rng.setSeed (77 + ch);
                auto* d = audio[(size_t) ch].data();
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
            fadeEdges (audio, rate, 0.3f, 0.6f);
        }

        // The zones point at the audio only once it has stopped moving
        for (size_t i = 0; i < src->zones.size(); ++i)
        {
            src->zones[i].data[0] = src->audio[i][0].data();
            src->zones[i].data[1] = src->audio[i][1].data();
        }
        auto& s = src->source;
        s.zones = src->zones.data();
        s.numZones = (int) src->zones.size();
        s.rate = rate;
        s.pitched = pitched;
        s.sustains = isSustaining (info->family);
        s.finish();
        return src;
    }

    /** Builds a built-in source once into the engine's own storage and returns it (nullptr
        for an unknown ID). Allocates the first time: call it off the audio thread. */
    const Source* builtin (const char* id)
    {
        const auto& list = builtins();
        for (size_t i = 0; i < list.size() && i < builtStore.size(); ++i)
            if (std::strcmp (list[i].id, id) == 0)
            {
                if (builtStore[i] == nullptr)
                    builtStore[i] = buildSource (id);
                return builtStore[i] != nullptr ? &builtStore[i]->source : nullptr;
            }
        return nullptr;
    }

    /** Plays a built-in source on a layer (builds it first if needed: off the audio thread). */
    bool setBuiltinSource (int layer, const char* id)
    {
        if (auto* s = builtin (id)) { setSource (layer, *s); return true; }
        return false;
    }

    //==============================================================================
    // Source keys, as patches and saved state name them ("builtin/<id>" or "<pack>/<id>")

    /** The family of a source key: Bowed, Breath, Plucked, Struck, Keys or Texture. */
    static const char* familyOf (const char* key)
    {
        if (std::strncmp (key, "builtin/", 8) == 0)
        {
            for (auto& b : builtins())
                if (std::strcmp (key + 8, b.id) == 0)
                    return b.family;
            return "Bowed";
        }
        for (auto& s : packSources())
            if (std::strcmp (idOf (key), s.id) == 0)
                return s.family;
        return "Bowed";
    }

    /** The name a source key's sound goes by. */
    static const char* nameOf (const char* key)
    {
        for (auto& s : packSources())
            if (std::strcmp (idOf (key), s.id) == 0)
                return s.name;
        return key;
    }

    /** The built-in source that plays for a key: its own for "builtin/<id>", else a stand-in of the same family. */
    static const char* builtinFor (const char* key)
    {
        if (std::strncmp (key, "builtin/", 8) == 0)
            for (auto& b : builtins())
                if (std::strcmp (key + 8, b.id) == 0)
                    return b.id;
        return fallbackFor (familyOf (key));
    }

    static bool isBuiltinKey (const char* key)      { return std::strncmp (key, "builtin/", 8) == 0; }

    /** Sustaining families hold while the key is down; plucked and struck ones ring out. */
    static bool isSustaining (const char* family)   { return std::strcmp (family, "Plucked") != 0 && std::strcmp (family, "Struck") != 0; }

    /** The Organic Sounds pack's sources (the files are not part of the engine). */
    struct PackSourceInfo { const char* id; const char* name; const char* family; };
    static const std::vector<PackSourceInfo>& packSources()
    {
        static const std::vector<PackSourceInfo> all = {
            { "cello", "Cello Ensemble", "Bowed" }, { "violins", "Violin Ensemble", "Bowed" }, { "solovln", "Solo Violin", "Bowed" },
            { "bass", "Contrabass", "Bowed" }, { "flute", "Flute", "Breath" }, { "clarinet", "Clarinet", "Breath" },
            { "bassoon", "Bassoon", "Breath" }, { "horn", "French Horn", "Breath" }, { "harp", "Harp", "Plucked" },
            { "bpizz", "Contrabass Pizz", "Plucked" }, { "vpizz", "Violin Pizz", "Plucked" }, { "marimba", "Marimba", "Struck" },
            { "glock", "Glockenspiel", "Struck" }, { "bells", "Nepalese Bells", "Texture" }, { "cymbal", "Bowed Cymbal", "Texture" },
            { "gong", "Gong Scrape", "Texture" },
            // Organic Sounds 2
            { "violas", "Viola Ensemble", "Bowed" }, { "vlntrem", "Violin Tremolo", "Bowed" }, { "celtrem", "Cello Tremolo", "Bowed" },
            { "oboe", "Oboe", "Breath" }, { "mhorn", "Muted Horn", "Breath" }, { "trumpet", "Trumpet", "Breath" },
            { "trombone", "Trombone", "Breath" }, { "tuba", "Tuba", "Breath" }, { "piano", "Upright Piano", "Struck" },
            { "tubular", "Tubular Bells", "Struck" }, { "organ", "Pipe Organ", "Keys" }, { "timproll", "Timpani Roll", "Texture" },
            { "belltree", "Bell Tree", "Texture" }, { "cymswell", "Cymbal Swell", "Texture" }, { "longbells", "Bell Drone", "Texture" },
        };
        return all;
    }

    //==============================================================================
    /** Every control, in the units the device shows (percent, semitones, Hz, seconds, dB). */
    struct Params
    {
        float aTexture = 10.0f, aPosition = 0.0f, aTune = 0.0f, aLevel = 80.0f;
        float bTexture = 55.0f, bPosition = 20.0f, bTune = 0.0f, bLevel = 80.0f;
        float morph = 30.0f, tide = 0.08f, tideDepth = 25.0f;
        float body = 0.0f, resonance = 25.0f, bodyDecay = 2.5f, air = 15.0f;
        float swell = 100.0f, dynamics = 60.0f, bloom = 0.8f;
        float bright = 9000.0f, warmth = 20.0f, attack = 0.06f, release = 1.8f;
        float life = 35.0f, space = 45.0f, spaceMix = 28.0f, width = 70.0f, volume = -6.0f;

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
                { "atex", &Params::aTexture }, { "apos", &Params::aPosition }, { "atune", &Params::aTune }, { "alevel", &Params::aLevel },
                { "btex", &Params::bTexture }, { "bpos", &Params::bPosition }, { "btune", &Params::bTune }, { "blevel", &Params::bLevel },
                { "morph", &Params::morph }, { "tide", &Params::tide }, { "tdepth", &Params::tideDepth },
                { "body", &Params::body }, { "res", &Params::resonance }, { "bdecay", &Params::bodyDecay }, { "air", &Params::air },
                { "swell", &Params::swell }, { "dyn", &Params::dynamics }, { "bloom", &Params::bloom },
                { "bright", &Params::bright }, { "warmth", &Params::warmth }, { "attack", &Params::attack }, { "release", &Params::release },
                { "life", &Params::life }, { "space", &Params::space }, { "smix", &Params::spaceMix }, { "width", &Params::width },
                { "volume", &Params::volume },
            };
            return all;
        }
    };

    Params params;

    //==============================================================================
    using Control = EngineControl;

    /** Every control, in the order the device panel shows them. */
    static const std::vector<SynthControl>& controls()
    {
        static const std::vector<SynthControl> all = {
            { "atex",   "Texture",   "Layer A",    0, 100, 10, 0, false, "%", {} },
            { "apos",   "Position",  "Layer A",    0, 100, 0, 0, false, "%", {} },
            { "atune",  "Tune",      "Layer A",    -24, 24, 0, 1, false, "st", {} },
            { "alevel", "Level",     "Layer A",    0, 100, 80, 0, false, "%", {} },
            { "morph",  "Morph",     "Morph",      0, 100, 30, 0, false, "%", {} },
            { "tide",   "Tide",      "Morph",      0.01f, 2, 0.08f, 0, true, "Hz", {} },
            { "tdepth", "Depth",     "Morph",      0, 100, 25, 0, false, "%", {} },
            { "btex",   "Texture",   "Layer B",    0, 100, 55, 0, false, "%", {} },
            { "bpos",   "Position",  "Layer B",    0, 100, 20, 0, false, "%", {} },
            { "btune",  "Tune",      "Layer B",    -24, 24, 0, 1, false, "st", {} },
            { "blevel", "Level",     "Layer B",    0, 100, 80, 0, false, "%", {} },
            { "body",   "Body",      "Body",       0, 4, 0, 1, false, "", { "String", "Tube", "Bowl", "Glass", "Wood" } },
            { "res",    "Resonance", "Body",       0, 100, 25, 0, false, "%", {} },
            { "bdecay", "Decay",     "Body",       0.1f, 10, 2.5f, 0, true, "s", {} },
            { "air",    "Air",       "Body",       0, 100, 15, 0, false, "%", {} },
            { "swell",  "Swell",     "Expression", 0, 100, 100, 0, false, "%", {} },
            { "dyn",    "Dynamics",  "Expression", 0, 100, 60, 0, false, "%", {} },
            { "bloom",  "Bloom",     "Expression", 0, 6, 0.8f, 0, false, "s", {} },
            { "bright", "Bright",    "Tone",       200, 20000, 9000, 0, true, "Hz", {} },
            { "warmth", "Warmth",    "Tone",       0, 100, 20, 0, false, "%", {} },
            { "attack", "Attack",    "Tone",       0.001f, 8, 0.06f, 0, true, "s", {} },
            { "release","Release",   "Tone",       0.02f, 12, 1.8f, 0, true, "s", {} },
            { "space",  "Space",     "Space",      0, 100, 45, 0, false, "%", {} },
            { "smix",   "Mix",       "Space",      0, 100, 28, 0, false, "%", {} },
            { "width",  "Width",     "Space",      0, 100, 70, 0, false, "%", {} },
            { "life",   "Life",      "Space",      0, 100, 35, 0, false, "%", {} },
            { "volume", "Volume",    "Space",      -60, 6, -6, 0, false, "dB", {} },
        };
        return all;
    }

    //==============================================================================
    // Patches: two source keys, and the controls each sets (the rest stay at their defaults)
    struct Patch { const char* name; const char* a; const char* b; std::vector<std::pair<const char*, float>> values; };

    static const std::vector<Patch>& patches()
    {
        static const std::vector<Patch> all = {
            { "Horizon Strings",    "organic/cello", "organic/violins",
              { { "atex", 12 }, { "btex", 45 }, { "bpos", 30 }, { "morph", 40 }, { "tdepth", 30 }, { "res", 20 }, { "bloom", 1.2f },
                { "space", 55 }, { "smix", 30 }, { "attack", 0.12f }, { "release", 2.2f } } },
            { "Glass Breath",       "organic/flute", "organic/glock",
              { { "atex", 25 }, { "btex", 80 }, { "bpos", 0 }, { "morph", 35 }, { "body", 3 }, { "res", 45 }, { "bdecay", 4.0f }, { "air", 30 },
                { "space", 60 }, { "smix", 38 } } },
            { "Wooden Air",         "organic/clarinet", "organic/marimba",
              { { "atex", 8 }, { "btex", 30 }, { "bpos", 0 }, { "morph", 25 }, { "body", 4 }, { "res", 35 }, { "bloom", 0.3f }, { "attack", 0.02f } } },
            { "Iron Cathedral",     "organic/horn", "organic/cymbal",
              { { "atex", 15 }, { "btex", 70 }, { "morph", 45 }, { "tdepth", 40 }, { "body", 2 }, { "res", 30 }, { "bdecay", 5.0f },
                { "space", 85 }, { "smix", 45 }, { "release", 4.0f } } },
            { "Harp Rain",          "organic/harp", "organic/bells",
              { { "atex", 35 }, { "btex", 65 }, { "bpos", 0 }, { "morph", 30 }, { "res", 30 }, { "space", 65 }, { "smix", 40 },
                { "attack", 0.002f }, { "release", 3.0f } } },
            { "Deep Current",       "organic/bass", "organic/bassoon",
              { { "atex", 5 }, { "btex", 40 }, { "morph", 35 }, { "dyn", 85 }, { "bloom", 1.5f }, { "bright", 4000 }, { "warmth", 40 },
                { "space", 35 }, { "smix", 20 } } },
            { "Paper Moon",         "organic/solovln", "organic/vpizz",
              { { "atex", 60 }, { "btex", 20 }, { "bpos", 0 }, { "morph", 50 }, { "tide", 0.15f }, { "tdepth", 50 }, { "body", 3 }, { "res", 20 },
                { "space", 60 }, { "smix", 35 }, { "volume", -2 } } },
            { "Bowl Garden",        "organic/glock", "organic/gong",
              { { "atex", 50 }, { "btex", 85 }, { "morph", 40 }, { "body", 2 }, { "res", 55 }, { "bdecay", 6.0f }, { "air", 20 },
                { "space", 75 }, { "smix", 45 }, { "attack", 0.8f }, { "release", 5.0f } } },
            { "Frozen Lake",        "organic/violins", "organic/cymbal",
              { { "atex", 95 }, { "apos", 40 }, { "btex", 90 }, { "morph", 30 }, { "space", 90 }, { "smix", 50 }, { "attack", 1.5f },
                { "release", 6.0f }, { "bloom", 2.5f }, { "volume", -2 } } },
            { "Pizz Constellation", "organic/bpizz", "organic/vpizz",
              { { "atex", 70 }, { "btex", 90 }, { "bpos", 0 }, { "morph", 50 }, { "tide", 0.3f }, { "tdepth", 60 }, { "body", 3 }, { "res", 25 },
                { "space", 70 }, { "smix", 42 }, { "attack", 0.002f }, { "release", 2.5f }, { "volume", 3 } } },
            // Organic Sounds 2: the orchestra, keys and bells
            { "Adagio Strings",     "organic/violins", "organic/violas",
              { { "atex", 8 }, { "btex", 15 }, { "morph", 45 }, { "tdepth", 20 }, { "res", 10 }, { "bloom", 1.5f }, { "space", 60 }, { "smix", 32 },
                { "attack", 0.35f }, { "release", 2.5f }, { "life", 25 } } },
            { "Brass Choir",        "organic/horn", "organic/trombone",
              { { "atex", 10 }, { "btex", 15 }, { "morph", 45 }, { "tdepth", 15 }, { "res", 0 }, { "dyn", 80 }, { "bloom", 0.8f }, { "space", 55 },
                { "smix", 28 }, { "attack", 0.1f }, { "release", 1.6f } } },
            { "Trumpet Fanfare",    "organic/trumpet", "organic/horn",
              { { "atex", 0 }, { "btex", 5 }, { "morph", 40 }, { "tdepth", 0 }, { "res", 0 }, { "dyn", 75 }, { "bloom", 0.0f }, { "space", 45 },
                { "smix", 25 }, { "attack", 0.03f }, { "release", 1.2f } } },
            { "Low Brass",          "organic/tuba", "organic/trombone",
              { { "atex", 5 }, { "btex", 10 }, { "morph", 50 }, { "tdepth", 0 }, { "res", 0 }, { "dyn", 85 }, { "bloom", 0.5f }, { "space", 40 },
                { "smix", 22 }, { "attack", 0.06f }, { "release", 1.4f } } },
            { "Muted Brass Night",  "organic/mhorn", "organic/cello",
              { { "atex", 10 }, { "btex", 20 }, { "morph", 35 }, { "res", 0 }, { "bright", 5000 }, { "space", 60 }, { "smix", 34 },
                { "attack", 0.15f }, { "release", 2.2f } } },
            { "Oboe Pastorale",     "organic/oboe", "organic/harp",
              { { "atex", 0 }, { "btex", 35 }, { "blevel", 60 }, { "morph", 40 }, { "tdepth", 10 }, { "body", 1 }, { "res", 10 }, { "space", 50 },
                { "smix", 30 }, { "attack", 0.05f }, { "release", 1.6f } } },
            { "Tremolo Suspense",   "organic/vlntrem", "organic/celtrem",
              { { "atex", 10 }, { "btex", 10 }, { "morph", 50 }, { "tdepth", 30 }, { "res", 0 }, { "dyn", 70 }, { "bloom", 1.0f }, { "space", 55 },
                { "smix", 30 }, { "attack", 0.2f }, { "release", 2.0f } } },
            { "Timpani Swell",      "organic/celtrem", "organic/timproll",
              { { "atex", 10 }, { "btex", 20 }, { "morph", 45 }, { "res", 0 }, { "dyn", 90 }, { "bloom", 2.0f }, { "space", 65 }, { "smix", 35 },
                { "attack", 0.6f }, { "release", 3.0f } } },
            { "Nocturne Piano",     "organic/piano", "organic/violins",
              { { "atex", 0 }, { "btex", 40 }, { "blevel", 45 }, { "morph", 30 }, { "tdepth", 15 }, { "res", 0 }, { "dyn", 70 }, { "bloom", 0.0f },
                { "space", 50 }, { "smix", 28 }, { "attack", 0.002f }, { "release", 1.8f }, { "life", 15 }, { "volume", 2 } } },
            { "Cathedral Organ",    "organic/organ", "organic/cymswell",
              { { "atex", 5 }, { "btex", 70 }, { "blevel", 35 }, { "morph", 25 }, { "tdepth", 10 }, { "res", 0 }, { "dyn", 40 }, { "space", 85 },
                { "smix", 45 }, { "attack", 0.08f }, { "release", 3.5f }, { "volume", -14 } } },
            { "Tubular Hall",       "organic/tubular", "organic/longbells",
              { { "atex", 0 }, { "btex", 60 }, { "blevel", 50 }, { "morph", 30 }, { "res", 15 }, { "body", 2 }, { "bdecay", 5.0f }, { "space", 90 },
                { "smix", 50 }, { "attack", 0.002f }, { "release", 6.0f } } },
            { "Bell Tree Sparkle",  "organic/glock", "organic/belltree",
              { { "atex", 30 }, { "btex", 50 }, { "bpos", 0 }, { "morph", 45 }, { "body", 3 }, { "res", 20 }, { "space", 70 }, { "smix", 40 },
                { "attack", 0.002f }, { "release", 3.0f } } },
            { "Breathing Choir",    "builtin/choir", "organic/flute",
              { { "atex", 20 }, { "btex", 70 }, { "morph", 35 }, { "air", 35 }, { "body", 1 }, { "res", 25 }, { "bloom", 1.0f } } },
            { "Air Organ",          "builtin/bow", "builtin/choir",
              { { "atex", 20 }, { "btex", 50 }, { "morph", 40 }, { "body", 1 }, { "res", 30 }, { "space", 50 } } },
            { "Wind Glass",         "builtin/wind", "builtin/glass",
              { { "atex", 60 }, { "btex", 40 }, { "bpos", 0 }, { "morph", 55 }, { "body", 3 }, { "res", 40 }, { "bdecay", 4.0f }, { "space", 70 },
                { "smix", 40 }, { "attack", 0.4f }, { "release", 3.5f } } },
        };
        return all;
    }

    static int patchCount() noexcept                { return (int) patches().size(); }
    static const char* patchName (int i) noexcept   { return i >= 0 && i < patchCount() ? patches()[(size_t) i].name : ""; }

    /** A patch's source key for a layer (0 is A, 1 is B). */
    static const char* patchSource (int i, int layer) noexcept
    {
        if (i < 0 || i >= patchCount()) return "";
        return layer == 0 ? patches()[(size_t) i].a : patches()[(size_t) i].b;
    }

    /** True when a patch plays only built-in sources (no sample pack needed). */
    static bool patchIsBuiltin (int i) noexcept
    {
        return i >= 0 && i < patchCount() && isBuiltinKey (patchSource (i, 0)) && isBuiltinKey (patchSource (i, 1));
    }

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

    /** Sets every control to the patch (not on the audio thread while it renders). Sources
        are the caller's to set: setSource() with the patch's keys, or useBuiltinsFor(). */
    void setPatch (int i)
    {
        for (auto& [id, member] : Params::table())
            params.*member = patchValue (i, id);
    }

    /** Plays a patch's sources from the built-in ones: its own built-ins, and for a pack
        source a built-in stand-in of the same family. Builds them if needed: off the audio thread. */
    void useBuiltinsFor (int patch)
    {
        for (int layer = 0; layer < numLayers; ++layer)
            setBuiltinSource (layer, builtinFor (patchSource (patch, layer)));
    }

    //==============================================================================
    OrganicEngine()
    {
        for (auto& l : live) l.store (nullptr);
    }

    /** Sets the rate and starts everything afresh (sources stay as they are). */
    void prepare (double sampleRate, int /*maxBlock*/ = 0)
    {
        rate = sampleRate;
        space.prepare (rate);
        for (size_t i = 0; i < voice.size(); ++i)
        {
            voice[i] = Voice();
            voice[i].drift = SynthCore::Drift ((int) i * 13 + 7);
            voice[i].flutter = SynthCore::Drift ((int) i * 17 + 3);
            voiceView[i] = VoiceView();
        }
        noise = SynthCore::Noise();
        noteCounter = 0;
        bend = ctrl = tidePhase = 0.0f;
        morphSmooth = -1.0f;        // the first render starts it at Morph
        ctrlActive = pedal = false;
        expressionSource = 0;
        liveSwell = 0.0f;
        activeVoices = 0;
        prepared = true;
    }

    bool isPrepared() const noexcept    { return prepared; }

    //==============================================================================
    void noteOn (int note, float vel)
    {
        Voice* target = nullptr;
        for (auto& v : voice)
            if (v.active() && v.note == note) { target = &v; break; }
        if (target == nullptr)
            for (auto& v : voice)
                if (! v.active()) { target = &v; break; }
        if (target == nullptr)
        {
            for (int pass = 0; pass < 2 && target == nullptr; ++pass)
            {
                std::uint32_t oldest = std::numeric_limits<std::uint32_t>::max();
                for (auto& v : voice)
                    if ((pass == 1 || ! v.held) && v.age < oldest) { oldest = v.age; target = &v; }
            }
        }

        auto& v = *target;
        const bool wasSilent = ! v.active();
        v.note = note;
        v.vel = vel;
        v.press = 0.0f;
        v.held = true;
        v.sustained = false;
        v.killing = false;
        v.kill = 1.0f;
        v.age = ++noteCounter;
        v.take = (int) (noteCounter % 4);
        v.bloomT = 0.0f;
        v.pan = ((noteCounter % 2) == 0 ? 1.0f : -1.0f) * (0.08f + 0.22f * (float) ((noteCounter / 2) % 3) / 2.0f);
        v.tideOffset = (float) (noteCounter % 7) / 7.0f;
        if (wasSilent)
        {
            v.sw = swellTarget (v);
            v.body.reset();
            v.toneL.reset(); v.toneR.reset(); v.airBp.reset();
        }

        const float startA = params.aPosition / 100.0f, startB = params.bPosition / 100.0f;
        for (int k = 0; k < 2; ++k)
        {
            v.src[(size_t) k] = live[(size_t) k].load();
            const auto len = v.src[(size_t) k] != nullptr ? v.src[(size_t) k]->lengthSeconds() : 1.0;
            v.cloud[(size_t) k].start ((k == 0 ? startA : startB) * len * 0.85, (int) noteCounter * 31 + k);
        }
        v.amp.gate (true);
    }

    void noteOff (int note)
    {
        for (auto& v : voice)
        {
            if (v.note != note || ! v.held) continue;
            v.held = false;
            if (pedal) v.sustained = true;
            else v.amp.gate (false);
        }
    }

    /** Every note let go (they ring out through their release). */
    void allNotesOff()
    {
        for (auto& v : voice) { v.held = v.sustained = false; v.amp.gate (false); }
    }

    void setPitchBend (float minusOneToOne)         { bend = minusOneToOne; }      // ±2 semitones

    // Expression: whichever of these moved last drives the swell (0..1 each)
    enum ExpressionSource { velocity, modWheel, breath, expression, pressure };
    static const char* expressionName (int source)
    {
        static const char* names[] = { "Velocity", "Mod Wheel", "Breath", "Expression", "Pressure" };
        return names[dspmath::jlimit (0, 4, source)];
    }

    void setModWheel (float zeroToOne)              { setCtrl (zeroToOne, modWheel); }     // CC 1
    void setBreath (float zeroToOne)                { setCtrl (zeroToOne, breath); }       // CC 2
    void setExpression (float zeroToOne)            { setCtrl (zeroToOne, expression); }   // CC 11
    void setChannelPressure (float zeroToOne)       { setCtrl (zeroToOne, pressure); }

    /** Polyphonic aftertouch: lifts one held note's swell. */
    void setNotePressure (int note, float zeroToOne)
    {
        for (auto& v : voice)
            if (v.note == note && v.held)
                v.press = zeroToOne;
    }

    void setSustainPedal (bool down)
    {
        pedal = down;
        if (! down)
            for (auto& v : voice)
                if (v.sustained && ! v.held) { v.sustained = false; v.amp.gate (false); }
    }

    //==============================================================================
    /** Renders n samples, adding them to L and R. */
    void render (float* L, float* R, int num)
    {
        if (! prepared)
            return;

        for (int done = 0; done < num; )
        {
            const int n = std::min (chunk, num - done);
            renderChunk (L + done, R + done, n);
            done += n;
        }
    }

    //==============================================================================
    // What's going on, for drawing
    struct VoiceView { float x[2] {}, amp = 0.0f; bool active = false; };    // x: play position in each layer's source, 0..1
    std::array<VoiceView, maxVoices> voiceView;
    std::atomic<float> liveSwell { 0.0f };
    std::atomic<int> expressionSource { 0 };
    std::atomic<float> morphNow { 0.3f };
    std::atomic<int> activeVoices { 0 };

    //==============================================================================
    // The building blocks

    static float hann (float p) noexcept
    {
        static const auto table = []
        {
            std::array<float, 1025> t {};
            for (size_t i = 0; i < t.size(); ++i)
                t[i] = 0.5f - 0.5f * std::cos (dspmath::MathConstants<float>::twoPi * (float) i / 1024.0f);
            return t;
        }();
        const float x = dspmath::jlimit (0.0f, 1.0f, p) * 1024.0f;
        const int i = dspmath::jmin (1023, (int) x);
        const float f = x - (float) i;
        return table[(size_t) i] + (table[(size_t) i + 1] - table[(size_t) i]) * f;
    }

    /** 4-point Hermite read of one channel at a fractional position. */
    static float readAt (const float* d, int n, double pos) noexcept
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
            t = dspmath::jlimit (0.0f, 1.0f, t);
            CloudSettings s;
            s.size = 0.08f + 0.30f * t * t + 0.06f * t;
            s.density = 2.0f + 4.0f * t;
            s.spray = 0.32f * t * t;
            s.scan = map01 (std::pow (t, 0.6f), 1.0f, 0.06f);
            s.scatter = 0.12f * t * t + 0.05f * life * t;
            s.reverse = 0.3f * dspmath::jmax (0.0f, t - 0.45f) / 0.55f;
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
        SynthCore::Random rng;

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
            const double lenSec = soft->length / src.rate;
            const double hop = dspmath::jmax (8.0, s.size * devRate / s.density);
            const float norm = 2.0f / dspmath::jmax (2.0f, s.density);

            for (int i = 0; i < n; ++i)
            {
                // ---- New grains
                if (! finished && timer <= 0.0)
                {
                    if (! opened)
                    {
                        // The note's first grain starts fully open, so the attack is intact
                        spawn (src, soft, loud, mix, pitch, devRate, s, 0.5f, dspmath::jmax (1.0f, s.density * 0.5f));
                        opened = true;
                    }
                    spawn (src, soft, loud, mix, pitch, devRate, s, 0.0f, 1.0f);
                    const float jitter = 1.0f + (rng.nextFloat() * 2.0f - 1.0f) * 0.25f * dspmath::jmin (1.0f, s.spray * 4.0f);
                    timer += hop * jitter;
                }
                timer -= 1.0;

                // ---- Move the play position
                if (! finished)
                {
                    if (src.sustains || s.freeze)
                    {
                        // Sustaining sounds hold in their steady part; frozen plucks just after the attack
                        const double holdEnd = src.sustains ? dspmath::jmax (0.2, lenSec * 0.86 - s.size * rootRatio)
                                                            : dspmath::jmin (lenSec * 0.5, 0.18 + 0.5 * (double) s.size);
                        const double holdStart = src.sustains ? dspmath::jmax (lenSec * 0.38, holdEnd - 2.5) : dspmath::jmin (holdEnd * 0.5, 0.06);
                        if (! holding)
                        {
                            pos += s.scan * secondsPerSample;
                            if (pos >= holdEnd) holding = true;
                        }
                        else
                        {
                            // Wander back and forth through the steady part: an endless sustain
                            pos += holdDir * dspmath::jmax (0.18, (double) s.scan) * secondsPerSample;
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
                        const auto& a = *g.z[k];
                        const int len = a.length;
                        sl += zw * readAt (a.data[0], len, g.pos[k]);
                        sr += zw * readAt (a.data[1], len, g.pos[k] + g.offsetR);
                        g.pos[k] += g.inc[k];
                    }
                    l += sl * w * g.gainL;
                    r += sr * w * g.gainR;
                    g.phase += g.phaseInc;
                    if (g.phase >= 1.0f || g.pos[0] < 0.0 || g.pos[0] >= g.z[0]->length)
                        g.active = false;
                }
                L[i] += l * norm * gain;
                R[i] += r * norm * gain;
            }

            lastX = (float) dspmath::jlimit (0.0, 1.0, pos / dspmath::jmax (0.01, lenSec));
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
            const double start = dspmath::jmax (0.0, pos + (double) s.spray * (rng.nextFloat() * 2.0 - 1.0) * (startPhase > 0.0f ? 0.0 : 1.0));
            const double lenOut = s.size * devRate;

            g->active = true;
            g->mix = mix;
            g->weight = weight;
            g->phase = startPhase;
            g->phaseInc = (float) (1.0 / dspmath::jmax (16.0, lenOut));
            const float pan = s.spread * (rng.nextFloat() * 2.0f - 1.0f);
            g->gainL = std::sqrt (0.5f * (1.0f - pan)) * sqrt2;
            g->gainR = std::sqrt (0.5f * (1.0f + pan)) * sqrt2;
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

    /** A tuned body the sound rings in: eight resonant modes at the note's partials. */
    struct Body
    {
        enum Type { string, tube, bowl, glass, wood, numTypes };
        static const char* name (int type)
        {
            static const char* names[] = { "String", "Tube", "Bowl", "Glass", "Wood" };
            return names[dspmath::jlimit (0, numTypes - 1, type)];
        }

        static constexpr int modes = 8;
        std::array<float, modes> y1 {}, y2 {}, g {}, c1 {}, c2 {}, pl {}, pr {};
        float noiseGain = 1.0f;     // scale for broadband excitation (air), so it rings as loudly as a tone would

        void reset()    { y1.fill (0.0f); y2.fill (0.0f); }

        void set (float f0, int type, float decay, float bright, double sampleRate)
        {
            static const float ratios[numTypes][modes] = {
                { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f },
                { 1.0f, 3.0f, 5.0f, 7.0f, 9.0f, 11.0f, 13.0f, 15.0f },
                { 1.0f, 2.76f, 5.40f, 8.93f, 13.34f, 18.64f, 24.81f, 31.87f },
                { 1.0f, 2.32f, 4.25f, 6.63f, 9.38f, 12.53f, 16.07f, 20.01f },
                { 1.0f, 3.93f, 9.0f, 15.9f, 24.6f, 35.0f, 47.0f, 60.8f },
            };
            static const float decayScale[numTypes] = { 1.0f, 0.8f, 1.6f, 1.3f, 0.22f };
            type = dspmath::jlimit (0, numTypes - 1, type);
            const float r0 = std::exp (-6.9078f / (dspmath::jmax (0.02f, decay * decayScale[type]) * (float) sampleRate));
            noiseGain = std::sqrt (2.0f / dspmath::jmax (1.0e-7f, 1.0f - r0)) * 0.12f;

            for (int k = 0; k < modes; ++k)
            {
                const float f = f0 * ratios[type][k];
                if (f >= (float) sampleRate * 0.45f) { g[(size_t) k] = 0.0f; c1[(size_t) k] = c2[(size_t) k] = 0.0f; continue; }
                const float t60 = dspmath::jmax (0.02f, decay * decayScale[type] / std::sqrt (1.0f + 0.6f * (float) k));
                const float rr = std::exp (-6.9078f / (t60 * (float) sampleRate));
                const float w = dspmath::MathConstants<float>::twoPi * f / (float) sampleRate;
                c1[(size_t) k] = 2.0f * rr * std::cos (w);
                c2[(size_t) k] = -rr * rr;
                const float tilt = std::pow ((float) (k + 1), -1.2f + 1.1f * bright);
                g[(size_t) k] = (1.0f - rr) * 2.0f * dspmath::jmax (0.05f, std::sin (w)) * tilt;
                const float pan = (k % 2 == 0 ? -1.0f : 1.0f) * dspmath::jmin (0.7f, 0.12f * (float) k);
                pl[(size_t) k] = 1.0f - dspmath::jmax (0.0f, pan);
                pr[(size_t) k] = 1.0f + dspmath::jmin (0.0f, pan);
            }
        }

        /** tone: the sound (rings at unity where it matches a mode); noise: broadband excitation. */
        void process (float tone, float excite, float& l, float& r) noexcept
        {
            const float in = tone + excite * noiseGain;
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

private:
    static constexpr float sqrt2 = static_cast<float> (1.4142135623730950488L);

    struct Voice
    {
        int note = -1, take = 0;
        float vel = 1.0f, press = 0.0f, sw = 0.0f, bloomT = 0.0f, pan = 0.0f, tideOffset = 0.0f, kill = 1.0f;
        bool held = false, sustained = false, killing = false;
        std::uint32_t age = 0;
        std::array<Cloud, 2> cloud;
        std::array<const Source*, 2> src {};
        Body body;
        SynthCore::Svf toneL, toneR, airBp;
        SynthCore::AnalogEnvelope amp;
        SynthCore::Drift drift, flutter;
        bool active() const { return amp.isActive(); }
    };

    double rate = 48000.0;
    bool prepared = false;
    std::array<Voice, maxVoices> voice;
    std::array<std::atomic<const Source*>, numLayers> live;
    std::array<std::unique_ptr<BuiltSource>, 4> builtStore;
    OrganicDsp::Space space;
    SynthCore::Noise noise;
    std::uint32_t noteCounter = 0;
    float bend = 0.0f, ctrl = 0.0f, tidePhase = 0.0f, morphSmooth = -1.0f;
    bool ctrlActive = false, pedal = false;
    std::array<float, chunk> tl {}, tr {}, bl {}, br {}, ml {}, mr {};

    static float map01 (float t, float a, float b) noexcept     { return a + t * (b - a); }
    static int roundToInt (float v) noexcept                    { return (int) std::nearbyint (v); }   // halves to even
    static float mtof (float note)                              { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }
    static float dbToGain (float db, float floorDb = -100.0f)   { return db > floorDb ? std::pow (10.0f, db * 0.05f) : 0.0f; }

    static double nextDouble (SynthCore::Random& r) noexcept
    {
        return static_cast<std::uint32_t> (r.nextInt()) / (std::numeric_limits<std::uint32_t>::max() + 1.0);
    }

    /** The seed a built-in source starts its randomness from: a 31-multiplier hash of its ID. */
    static int seedFor (const char* id) noexcept
    {
        std::uint32_t h = 0;
        for (auto* c = reinterpret_cast<const unsigned char*> (id); *c != 0; ++c)
            h = 31u * h + (std::uint32_t) *c;
        return (int) h;
    }

    static const char* idOf (const char* key)
    {
        auto* slash = std::strchr (key, '/');
        return slash != nullptr ? slash + 1 : key;
    }

    /** Additive single cycle with a formant envelope. */
    static std::vector<float> cycle (float f0, std::initializer_list<std::pair<float, float>> formants, float brightness, float rate)
    {
        std::vector<float> t (2048, 0.0f);
        const int harmonics = dspmath::jmin (80, (int) (rate * 0.45f / f0));
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
                t[i] += a * std::sin (dspmath::MathConstants<float>::twoPi * (float) k * (float) i / 2048.0f + ph);
        }
        float peak = 1.0e-6f;
        for (auto v : t) peak = dspmath::jmax (peak, std::abs (v));
        for (auto& v : t) v /= peak;
        return t;
    }

    static void fadeEdges (std::array<std::vector<float>, 2>& b, double rate, float in, float out)
    {
        const int n = (int) b[0].size();
        const int a = (int) (in * rate), z = (int) (out * rate);
        for (int ch = 0; ch < 2; ++ch)
        {
            auto* d = b[(size_t) ch].data();
            for (int i = 0; i < a && i < n; ++i) d[i] *= (float) i / (float) a;
            for (int i = 0; i < z && i < n; ++i) d[n - 1 - i] *= (float) i / (float) z;
        }
    }

    void setCtrl (float value, int source)
    {
        ctrl = value;
        ctrlActive = true;
        expressionSource = source;
    }

    float swellTarget (const Voice& v) const
    {
        const float velCurve = 0.25f + 0.75f * std::pow (v.vel, 0.8f);
        float s = ctrlActive ? map01 (0.8f, velCurve, ctrl) : velCurve;
        s = dspmath::jmax (s, v.press);
        return dspmath::jlimit (0.0f, 1.0f, s * params.swell / 100.0f);
    }

    void renderChunk (float* L, float* R, int n)
    {
        std::fill (ml.begin(), ml.begin() + n, 0.0f);
        std::fill (mr.begin(), mr.begin() + n, 0.0f);

        const auto& p = params;
        const float sr = (float) rate;
        const float lifeAmt = p.life / 100.0f;
        const auto setA = CloudSettings::forTexture (p.aTexture / 100.0f, lifeAmt);
        const auto setB = CloudSettings::forTexture (p.bTexture / 100.0f, lifeAmt);
        const float levelA = std::pow (p.aLevel / 100.0f, 1.5f), levelB = std::pow (p.bLevel / 100.0f, 1.5f);
        const float res = p.resonance / 100.0f, airAmt = p.air / 100.0f;
        const float dyn = p.dynamics / 100.0f;
        const float bloomTime = p.bloom;
        const float bendSemis = bend * 2.0f;
        const float wid = p.width / 100.0f;
        const int type = roundToInt (p.body);
        const float decay = p.bodyDecay;
        const float att = p.attack, rel = p.release;
        const float cutoff = p.bright;
        const float swCoeff = 1.0f - std::exp (-(float) n / (0.03f * sr));

        // Tide: the morph sways slowly (each voice at its own point of the swell)
        tidePhase += p.tide * (float) n / sr;
        tidePhase -= std::floor (tidePhase);
        if (morphSmooth < 0.0f) morphSmooth = p.morph / 100.0f;
        morphSmooth += (p.morph / 100.0f - morphSmooth) * swCoeff;
        morphNow = morphSmooth;

        const auto* liveA = live[0].load();
        const auto* liveB = live[1].load();
        float maxSwell = 0.0f;
        int active = 0;

        for (size_t vi = 0; vi < voice.size(); ++vi)
        {
            auto& v = voice[vi];
            auto& view = voiceView[vi];
            if (! v.active())
            {
                view.active = false;
                continue;
            }
            ++active;

            // A source changed under the voice: fade it out quickly
            if (v.src[0] != liveA || v.src[1] != liveB)
                v.killing = true;

            // Expression
            v.bloomT += (float) n / sr;
            const float bloomShape = bloomTime <= 0.001f ? 1.0f : 1.0f - 0.65f * std::exp (-v.bloomT / (bloomTime * 0.4f));
            v.sw += (swellTarget (v) * bloomShape - v.sw) * swCoeff;
            const float s = v.sw;
            maxSwell = dspmath::jmax (maxSwell, s);
            const float levelDb = (s - 1.0f) * 30.0f * dyn;
            const float expGain = dbToGain (levelDb);
            const float brightOct = (s - 0.75f) * 3.0f * dyn;
            const float layerMix = dspmath::jlimit (0.0f, 1.0f, (0.75f + (s - 0.75f) * dyn - 0.2f) / 0.7f);

            const float driftSemis = v.drift.process (0.12f * lifeAmt, sr / (float) n) ;
            const float pitch = (float) v.note + bendSemis + driftSemis;
            const float flutter = 1.0f + v.flutter.process (0.12f * lifeAmt, sr / (float) n);

            const float m = dspmath::jlimit (0.0f, 1.0f, morphSmooth + p.tideDepth / 100.0f * 0.5f
                                                         * std::sin (dspmath::MathConstants<float>::twoPi * (tidePhase + v.tideOffset)));
            const float gA = std::cos (m * dspmath::MathConstants<float>::halfPi) * levelA;
            const float gB = std::sin (m * dspmath::MathConstants<float>::halfPi) * levelB;

            std::fill (tl.begin(), tl.begin() + n, 0.0f); std::fill (tr.begin(), tr.begin() + n, 0.0f);
            std::fill (bl.begin(), bl.begin() + n, 0.0f); std::fill (br.begin(), br.begin() + n, 0.0f);

            const float tunes[2] = { p.aTune, p.bTune };
            const CloudSettings* sets[2] = { &setA, &setB };
            float* outs[2][2] = { { tl.data(), tr.data() }, { bl.data(), br.data() } };
            const float gains[2] = { gA, gB };

            for (int k = 0; k < 2; ++k)
            {
                auto* src = v.src[(size_t) k];
                if (src == nullptr || gains[k] <= 0.0005f)
                    continue;
                const float pk = pitch + tunes[k];
                const int note = roundToInt (pk);
                const auto* soft = src->pick (note, 0, v.take);
                const auto* loud = src->pick (note, src->layers - 1, v.take);
                v.cloud[(size_t) k].render (outs[k][0], outs[k][1], n, *src, soft, loud, layerMix, pk, rate, *sets[k], src->gain * gains[k]);
                view.x[k] = v.cloud[(size_t) k].lastX;
            }

            // Body and air
            const float f0 = mtof (pitch);
            v.body.set (f0, type, decay, 0.3f + 0.7f * s, rate);
            v.airBp.set (dspmath::jlimit (300.0f, 9000.0f, f0 * 3.0f), 0.35f, sr);
            const float airLevel = airAmt * (0.25f + 0.75f * s) * 0.5f;
            const float toneCut = dspmath::jlimit (60.0f, sr * 0.45f, cutoff * std::pow (2.0f, brightOct + ((float) v.note - 60.0f) / 12.0f * 0.4f));
            v.toneL.set (toneCut, 0.1f, sr);
            v.toneR.set (toneCut, 0.1f, sr);

            float envLast = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                float l = tl[(size_t) i] + bl[(size_t) i];
                float r = tr[(size_t) i] + br[(size_t) i];

                const float breathNoise = noise.pink() * airLevel;
                float bl2, bb2, bh2;
                v.airBp.process (breathNoise, bl2, bb2, bh2);
                float rl = 0.0f, rr = 0.0f;
                v.body.process ((l + r) * 0.5f * res, breathNoise, rl, rr);
                const float dryAmt = 1.0f - res * 0.45f;
                const float wet = 0.6f * res + 0.4f * airAmt;
                l = l * dryAmt + rl * wet + bb2 * 0.12f;
                r = r * dryAmt + rr * wet + bb2 * 0.12f;

                l = v.toneL.lowpass (l);
                r = v.toneR.lowpass (r);

                const float env = v.amp.process (att, 0.1f, 1.0f, rel, sr);
                if (v.killing) v.kill = dspmath::jmax (0.0f, v.kill - 1.0f / (0.03f * sr));
                const float gain = env * expGain * flutter * v.kill;
                envLast = env;

                const float pan = v.pan * wid;
                ml[(size_t) i] += l * gain * (1.0f - dspmath::jmax (0.0f, pan));
                mr[(size_t) i] += r * gain * (1.0f + dspmath::jmin (0.0f, pan));
            }

            if (v.killing && v.kill <= 0.0f)
                v.amp.reset();

            view.amp = envLast * expGain;
            view.active = v.active();
        }

        liveSwell = maxSwell;
        activeVoices = active;

        // Warmth: gentle saturation
        const float w = p.warmth / 100.0f;
        const float drive = 1.0f + 3.0f * w;
        const float comp = 1.0f / std::tanh (drive * 0.5f) * 0.5f;
        for (int i = 0; i < n; ++i)
        {
            if (w > 0.001f)
            {
                ml[(size_t) i] = map01 (w, ml[(size_t) i], std::tanh (ml[(size_t) i] * drive * 0.5f) * comp);
                mr[(size_t) i] = map01 (w, mr[(size_t) i], std::tanh (mr[(size_t) i] * drive * 0.5f) * comp);
            }
        }

        space.process (ml.data(), mr.data(), n, p.space / 100.0f, p.spaceMix / 100.0f, 0.4f + 0.6f * wid);

        const float out = dbToGain (p.volume, -60.0f);
        for (int i = 0; i < n; ++i)
        {
            // Never clip hard: a soft ceiling just under 0 dBFS
            const float l = ml[(size_t) i] * out, r = mr[(size_t) i] * out;
            L[i] += std::abs (l) < 0.8f ? l : std::copysign (0.8f + 0.18f * std::tanh ((std::abs (l) - 0.8f) / 0.18f), l);
            R[i] += std::abs (r) < 0.8f ? r : std::copysign (0.8f + 0.18f * std::tanh ((std::abs (r) - 0.8f) / 0.18f), r);
        }
    }
};

} // namespace elsewhere
