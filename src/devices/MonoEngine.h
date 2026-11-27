// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "SynthCore.h"
#include "Downsampler.h"
#include "EngineControl.h"

namespace elsewhere
{
//==============================================================================
/**
    The sound of Elsewhere Mono, on its own: the voice of all three models (Seventy, Acid and
    Sync Lead), the note stack with its priority, glide and slide, and the output stage, in
    plain C++ with nothing of JUCE, so it builds anywhere. The Mono device plays through it.

    The voice runs at four times the sample rate and is brought back down by a half-band
    Downsampler, so the saturating filters, the feedback and hard sync stay clean of aliasing.

    Call prepare() first, then noteOn and noteOff as notes come, and render() for each block.
    None of them allocates or locks. The randomness (drift, noise, pitch jitter) starts from
    fixed seeds on every prepare(), so the same notes always render the same audio.
*/
class MonoEngine
{
public:
    static constexpr int maxVoices = 1;
    static constexpr int chunk = 256;               // samples at the output rate, rendered at a time
    static constexpr int oversampling = 4;

    enum Model { seventy, acid, syncLead, numModels };

    static const char* modelName (int m) noexcept
    {
        static const char* names[] = { "Seventy", "Acid", "Sync Lead" };
        return m >= 0 && m < numModels ? names[m] : "";
    }

    /** Seventy: oscillators 1 and 2 have the "shark tooth" (tri-saw); oscillator 3 a reverse saw instead. */
    static SynthCore::Wave waveFor (int osc, int index) noexcept
    {
        using W = SynthCore::Wave;
        static constexpr W waves[] = { W::triangle, W::shark, W::saw, W::square, W::widePulse, W::narrowPulse };
        const int i = index < 0 ? 0 : (index > 5 ? 5 : index);
        return i == 1 && osc == 2 ? W::reverseSaw : waves[i];
    }

    //==============================================================================
    /** Every control, in the units the device shows (cents, seconds, Hz, tenths, percent, dB). */
    struct Params
    {
        float model = seventy;

        // Seventy
        float tune = 0.0f, glide = 0.0f, modMix = 0.0f, modAmount = 0.0f, oscMod = 0.0f, filterMod = 0.0f;
        float o1range = 3.0f, o1wave = 2.0f;
        float o2range = 3.0f, o2wave = 2.0f, o2freq = 0.08f;
        float o3range = 2.0f, o3wave = 2.0f, o3freq = -0.06f;
        float osc3Key = 1.0f;
        float m1 = 7.0f, m1on = 1.0f, m2 = 6.0f, m2on = 1.0f, m3 = 0.0f, m3on = 0.0f;
        float feedback = 0.0f, feedbackOn = 0.0f, noise = 0.0f, noiseOn = 0.0f, noiseColour = 0.0f;
        float cutoff = 1200.0f, emphasis = 2.0f, contour = 5.0f, keyTrack = 1.0f, fAttack = 0.005f, fDecay = 0.6f, fSustain = 3.0f;
        float aAttack = 0.003f, aDecay = 0.6f, aSustain = 8.0f, release = 1.0f;

        // Acid
        float acWave = 0.0f, acTune = 0.0f, acCutoff = 500.0f, acRes = 7.0f, acEnvMod = 5.0f, acDecay = 0.5f;
        float acAccent = 6.0f, acSlide = 0.06f, acDist = 0.0f;

        // Sync Lead
        float syAWave = 0.0f, syWidth = 35.0f, syBWave = 0.0f, syBPitch = 7.0f, sySync = 1.0f, syEnvToB = 18.0f;
        float syMixA = 5.0f, syMixB = 8.0f, syNoise = 0.0f;
        float syCutoff = 2500.0f, syRes = 2.0f, syEnvAmt = 4.0f, syKey = 2.0f;
        float syFA = 0.002f, syFD = 0.9f, syFS = 2.0f, syFR = 0.4f;
        float syAA = 0.003f, syAD = 0.5f, syAS = 8.0f, syAR = 0.3f;
        float syLfoRate = 5.0f, syLfoAmt = 0.0f, syLfoDest = 0.0f;

        // Output (all models)
        float volume = -6.0f, drive = 15.0f, drift = 25.0f, velocity = 0.0f, bendRange = 2.0f;
        float legato = 1.0f, priority = 0.0f, character = 60.0f;

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
                { "model", &Params::model },
                { "tune", &Params::tune }, { "glide", &Params::glide }, { "modmix", &Params::modMix }, { "modamt", &Params::modAmount },
                { "oscmod", &Params::oscMod }, { "filtmod", &Params::filterMod },
                { "o1range", &Params::o1range }, { "o1wave", &Params::o1wave },
                { "o2range", &Params::o2range }, { "o2wave", &Params::o2wave }, { "o2freq", &Params::o2freq },
                { "o3range", &Params::o3range }, { "o3wave", &Params::o3wave }, { "o3freq", &Params::o3freq },
                { "o3key", &Params::osc3Key },
                { "m1", &Params::m1 }, { "m1on", &Params::m1on }, { "m2", &Params::m2 }, { "m2on", &Params::m2on },
                { "m3", &Params::m3 }, { "m3on", &Params::m3on },
                { "fb", &Params::feedback }, { "fbon", &Params::feedbackOn }, { "noise", &Params::noise }, { "noiseon", &Params::noiseOn },
                { "noisecol", &Params::noiseColour },
                { "cutoff", &Params::cutoff }, { "emph", &Params::emphasis }, { "contour", &Params::contour }, { "keytrack", &Params::keyTrack },
                { "fa", &Params::fAttack }, { "fd", &Params::fDecay }, { "fs", &Params::fSustain },
                { "aa", &Params::aAttack }, { "ad", &Params::aDecay }, { "as", &Params::aSustain }, { "release", &Params::release },
                { "acwave", &Params::acWave }, { "actune", &Params::acTune }, { "accutoff", &Params::acCutoff }, { "acres", &Params::acRes },
                { "acenv", &Params::acEnvMod }, { "acdecay", &Params::acDecay }, { "acaccent", &Params::acAccent }, { "acslide", &Params::acSlide },
                { "acdist", &Params::acDist },
                { "syawave", &Params::syAWave }, { "sywidth", &Params::syWidth }, { "sybwave", &Params::syBWave }, { "sybpitch", &Params::syBPitch },
                { "sysync", &Params::sySync }, { "syenvb", &Params::syEnvToB }, { "symixa", &Params::syMixA }, { "symixb", &Params::syMixB },
                { "synoise", &Params::syNoise }, { "sycutoff", &Params::syCutoff }, { "syres", &Params::syRes }, { "syenv", &Params::syEnvAmt },
                { "sykey", &Params::syKey }, { "syfa", &Params::syFA }, { "syfd", &Params::syFD }, { "syfs", &Params::syFS }, { "syfr", &Params::syFR },
                { "syaa", &Params::syAA }, { "syad", &Params::syAD }, { "syas", &Params::syAS }, { "syar", &Params::syAR },
                { "sylforate", &Params::syLfoRate }, { "sylfoamt", &Params::syLfoAmt }, { "sylfodest", &Params::syLfoDest },
                { "volume", &Params::volume }, { "drive", &Params::drive }, { "drift", &Params::drift }, { "vel", &Params::velocity },
                { "bend", &Params::bendRange }, { "legato", &Params::legato }, { "priority", &Params::priority }, { "character", &Params::character },
            };
            return all;
        }
    };

    Params params;

    //==============================================================================
    using Control = EngineControl;

    /** Every control, in the order the device panel shows them: the model and the output
        (shared by all three models), then Seventy's, Acid's and Sync Lead's. Glide is shown by
        Seventy (Controllers) and Sync Lead (Osc A); it is listed once, under Seventy, and
        controlsFor (syncLead) includes it too. */
    static const std::vector<SynthControl>& controls()
    {
        static const std::vector<const char*> onOff { "Off", "On" };
        static const std::vector<const char*> range { "LO", "32'", "16'", "8'", "4'", "2'" };
        static const std::vector<const char*> wave12 { "Triangle", "Shark Tooth", "Sawtooth", "Square", "Wide Pulse", "Narrow Pulse" };
        static const std::vector<const char*> wave3 { "Triangle", "Reverse Saw", "Sawtooth", "Square", "Wide Pulse", "Narrow Pulse" };
        static const std::vector<const char*> keyTrack { "Off", "1/3", "2/3", "Full" };

        #define MONO_SEP " \xc2\xb7 "
        static const std::vector<SynthControl> all = {
            { "model",     "Model",          "Model",   0, 2, seventy, 1, false, "", { "Seventy", "Acid", "Sync Lead" } },

            { "volume",    "Volume",         "Output",  -60, 6, -6, 0, false, "dB", {} },
            { "drive",     "Drive",          "Output",  0, 100, 15, 0, false, "%", {} },
            { "drift",     "Drift",          "Output",  0, 100, 25, 0, false, "%", {} },
            { "vel",       "Velocity",       "Output",  0, 100, 0, 0, false, "%", {} },
            { "bend",      "Bend Range",     "Output",  1, 12, 2, 1, false, "st", {} },
            { "character", "Character",      "Output",  0, 100, 60, 0, false, "%", {} },
            { "legato",    "Legato",         "Output",  0, 1, 1, 1, false, "", onOff },
            { "priority",  "Note Priority",  "Output",  0, 2, 0, 1, false, "", { "Last", "Low", "High" } },

            { "tune",      "Tune",           "Seventy" MONO_SEP "Controllers", -100, 100, 0, 0, false, "ct", {} },
            { "glide",     "Glide",          "Seventy" MONO_SEP "Controllers", 0, 5, 0, 0, true, "s", {} },
            { "modmix",    "Mod Mix",        "Seventy" MONO_SEP "Controllers", 0, 100, 0, 0, false, "%", {} },
            { "modamt",    "Mod Amount",     "Seventy" MONO_SEP "Controllers", 0, 100, 0, 0, false, "%", {} },
            { "oscmod",    "Osc Mod",        "Seventy" MONO_SEP "Controllers", 0, 1, 0, 1, false, "", onOff },
            { "filtmod",   "Filter Mod",     "Seventy" MONO_SEP "Controllers", 0, 1, 0, 1, false, "", onOff },

            { "o1range",   "Osc 1 Range",    "Seventy" MONO_SEP "Oscillators", 0, 5, 3, 1, false, "", range },
            { "o1wave",    "Osc 1 Wave",     "Seventy" MONO_SEP "Oscillators", 0, 5, 2, 1, false, "", wave12 },
            { "o2range",   "Osc 2 Range",    "Seventy" MONO_SEP "Oscillators", 0, 5, 3, 1, false, "", range },
            { "o2freq",    "Osc 2 Frequency","Seventy" MONO_SEP "Oscillators", -7, 7, 0.08f, 0, false, "st", {} },
            { "o2wave",    "Osc 2 Wave",     "Seventy" MONO_SEP "Oscillators", 0, 5, 2, 1, false, "", wave12 },
            { "o3range",   "Osc 3 Range",    "Seventy" MONO_SEP "Oscillators", 0, 5, 2, 1, false, "", range },
            { "o3freq",    "Osc 3 Frequency","Seventy" MONO_SEP "Oscillators", -7, 7, -0.06f, 0, false, "st", {} },
            { "o3wave",    "Osc 3 Wave",     "Seventy" MONO_SEP "Oscillators", 0, 5, 2, 1, false, "", wave3 },
            { "o3key",     "Osc 3 Keyboard", "Seventy" MONO_SEP "Oscillators", 0, 1, 1, 1, false, "", onOff },

            { "m1on",      "Osc 1 On",       "Seventy" MONO_SEP "Mixer", 0, 1, 1, 1, false, "", onOff },
            { "m1",        "Osc 1 Level",    "Seventy" MONO_SEP "Mixer", 0, 10, 7, 0, false, "", {} },
            { "m2on",      "Osc 2 On",       "Seventy" MONO_SEP "Mixer", 0, 1, 1, 1, false, "", onOff },
            { "m2",        "Osc 2 Level",    "Seventy" MONO_SEP "Mixer", 0, 10, 6, 0, false, "", {} },
            { "m3on",      "Osc 3 On",       "Seventy" MONO_SEP "Mixer", 0, 1, 0, 1, false, "", onOff },
            { "m3",        "Osc 3 Level",    "Seventy" MONO_SEP "Mixer", 0, 10, 0, 0, false, "", {} },
            { "fbon",      "Feedback On",    "Seventy" MONO_SEP "Mixer", 0, 1, 0, 1, false, "", onOff },
            { "fb",        "Feedback",       "Seventy" MONO_SEP "Mixer", 0, 10, 0, 0, false, "", {} },
            { "noiseon",   "Noise On",       "Seventy" MONO_SEP "Mixer", 0, 1, 0, 1, false, "", onOff },
            { "noise",     "Noise Level",    "Seventy" MONO_SEP "Mixer", 0, 10, 0, 0, false, "", {} },
            { "noisecol",  "Noise Colour",   "Seventy" MONO_SEP "Mixer", 0, 1, 0, 1, false, "", { "White", "Pink" } },

            { "cutoff",    "Cutoff",         "Seventy" MONO_SEP "Filter", 20, 20000, 1200, 0, true, "Hz", {} },
            { "emph",      "Emphasis",       "Seventy" MONO_SEP "Filter", 0, 10, 2, 0, false, "", {} },
            { "contour",   "Contour Amount", "Seventy" MONO_SEP "Filter", 0, 10, 5, 0, false, "", {} },
            { "keytrack",  "Keyboard Control","Seventy" MONO_SEP "Filter", 0, 3, 1, 1, false, "", keyTrack },
            { "fa",        "Filter Attack",  "Seventy" MONO_SEP "Filter", 0.001f, 10, 0.005f, 0, true, "s", {} },
            { "fd",        "Filter Decay",   "Seventy" MONO_SEP "Filter", 0.001f, 10, 0.6f, 0, true, "s", {} },
            { "fs",        "Filter Sustain", "Seventy" MONO_SEP "Filter", 0, 10, 3, 0, false, "", {} },

            { "aa",        "Attack",         "Seventy" MONO_SEP "Loudness", 0.001f, 10, 0.003f, 0, true, "s", {} },
            { "ad",        "Decay",          "Seventy" MONO_SEP "Loudness", 0.001f, 10, 0.6f, 0, true, "s", {} },
            { "as",        "Sustain",        "Seventy" MONO_SEP "Loudness", 0, 10, 8, 0, false, "", {} },
            { "release",   "Release",        "Seventy" MONO_SEP "Loudness", 0, 1, 1, 1, false, "", { "Short", "Follows decay" } },

            { "acwave",    "Wave",           "Acid" MONO_SEP "Oscillator", 0, 1, 0, 1, false, "", { "Sawtooth", "Square" } },
            { "actune",    "Tune",           "Acid" MONO_SEP "Oscillator", -12, 12, 0, 0, false, "st", {} },
            { "accutoff",  "Cutoff",         "Acid" MONO_SEP "Filter", 60, 8000, 500, 0, true, "Hz", {} },
            { "acres",     "Resonance",      "Acid" MONO_SEP "Filter", 0, 10, 7, 0, false, "", {} },
            { "acenv",     "Env Mod",        "Acid" MONO_SEP "Filter", 0, 10, 5, 0, false, "", {} },
            { "acdecay",   "Decay",          "Acid" MONO_SEP "Filter", 0.1f, 3, 0.5f, 0, true, "s", {} },
            { "acaccent",  "Accent",         "Acid" MONO_SEP "Accent & Slide", 0, 10, 6, 0, false, "", {} },
            { "acslide",   "Slide Time",     "Acid" MONO_SEP "Accent & Slide", 0.01f, 0.5f, 0.06f, 0, true, "s", {} },
            { "acdist",    "Distortion",     "Acid" MONO_SEP "Distortion", 0, 100, 0, 0, false, "%", {} },

            { "syawave",   "Osc A Wave",     "Sync Lead" MONO_SEP "Osc A", 0, 1, 0, 1, false, "", { "Sawtooth", "Pulse" } },
            { "sywidth",   "Pulse Width",    "Sync Lead" MONO_SEP "Osc A", 5, 50, 35, 0, false, "%", {} },
            { "sybwave",   "Osc B Wave",     "Sync Lead" MONO_SEP "Osc B", 0, 2, 0, 1, false, "", { "Sawtooth", "Pulse", "Triangle" } },
            { "sysync",    "Sync",           "Sync Lead" MONO_SEP "Osc B", 0, 1, 1, 1, false, "", onOff },
            { "sybpitch",  "Osc B Pitch",    "Sync Lead" MONO_SEP "Osc B", 0, 36, 7, 0, false, "st", {} },
            { "syenvb",    "Env to B Pitch", "Sync Lead" MONO_SEP "Osc B", 0, 48, 18, 0, false, "st", {} },
            { "symixa",    "Osc A Level",    "Sync Lead" MONO_SEP "Mixer", 0, 10, 5, 0, false, "", {} },
            { "symixb",    "Osc B Level",    "Sync Lead" MONO_SEP "Mixer", 0, 10, 8, 0, false, "", {} },
            { "synoise",   "Noise",          "Sync Lead" MONO_SEP "Mixer", 0, 10, 0, 0, false, "", {} },
            { "sycutoff",  "Cutoff",         "Sync Lead" MONO_SEP "Filter", 20, 20000, 2500, 0, true, "Hz", {} },
            { "syres",     "Resonance",      "Sync Lead" MONO_SEP "Filter", 0, 10, 2, 0, false, "", {} },
            { "syenv",     "Filter Env",     "Sync Lead" MONO_SEP "Filter", 0, 10, 4, 0, false, "", {} },
            { "sykey",     "Key Track",      "Sync Lead" MONO_SEP "Filter", 0, 3, 2, 1, false, "", keyTrack },
            { "syfa",      "Filter Attack",  "Sync Lead" MONO_SEP "Filter", 0.001f, 10, 0.002f, 0, true, "s", {} },
            { "syfd",      "Filter Decay",   "Sync Lead" MONO_SEP "Filter", 0.001f, 10, 0.9f, 0, true, "s", {} },
            { "syfs",      "Filter Sustain", "Sync Lead" MONO_SEP "Filter", 0, 10, 2, 0, false, "", {} },
            { "syfr",      "Filter Release", "Sync Lead" MONO_SEP "Filter", 0.001f, 10, 0.4f, 0, true, "s", {} },
            { "syaa",      "Amp Attack",     "Sync Lead" MONO_SEP "Amp", 0.001f, 10, 0.003f, 0, true, "s", {} },
            { "syad",      "Amp Decay",      "Sync Lead" MONO_SEP "Amp", 0.001f, 10, 0.5f, 0, true, "s", {} },
            { "syas",      "Amp Sustain",    "Sync Lead" MONO_SEP "Amp", 0, 10, 8, 0, false, "", {} },
            { "syar",      "Amp Release",    "Sync Lead" MONO_SEP "Amp", 0.001f, 10, 0.3f, 0, true, "s", {} },
            { "sylforate", "LFO Rate",       "Sync Lead" MONO_SEP "LFO", 0.05f, 20, 5, 0, true, "Hz", {} },
            { "sylfoamt",  "LFO Amount",     "Sync Lead" MONO_SEP "LFO", 0, 100, 0, 0, false, "%", {} },
            { "sylfodest", "LFO Target",     "Sync Lead" MONO_SEP "LFO", 0, 3, 0, 1, false, "", { "B Pitch", "Width", "Cutoff", "Vibrato" } },
        };
        #undef MONO_SEP
        return all;
    }

    /** The controls a model uses, in panel order: the shared ones, then the model's own. */
    static std::vector<const Control*> controlsFor (int model)
    {
        std::vector<const Control*> result;
        const char* own = modelName (model);
        const size_t ownLen = std::strlen (own);
        auto isModels = [] (const char* section)
        {
            for (int m = 0; m < numModels; ++m)
                if (std::strncmp (section, modelName (m), std::strlen (modelName (m))) == 0)
                    return true;
            return false;
        };

        const Control* glide = nullptr;
        for (auto& c : controls())
            if (std::strcmp (c.id, "glide") == 0)
                glide = &c;

        for (auto& c : controls())
        {
            if (! isModels (c.section))
                result.push_back (&c);
            else if (ownLen > 0 && std::strncmp (c.section, own, ownLen) == 0)
            {
                result.push_back (&c);
                if (model == syncLead && std::strcmp (c.id, "sywidth") == 0 && glide != nullptr)
                    result.push_back (glide);       // Sync Lead shows Glide beside the pulse width
            }
        }
        return result;
    }

    //==============================================================================
    // Patches: bank A is Seventy, B is Acid, C is Sync Lead. Each sets the controls it names
    // and leaves the rest at their defaults.
    struct Patch { int model; const char* name; std::vector<std::pair<const char*, float>> values; };

    static const std::vector<Patch>& patches()
    {
        static const std::vector<Patch> all = {
            { seventy, "Classic Bass",   { { "o1range", 2 }, { "o2range", 2 }, { "o3range", 1 }, { "o3wave", 3 }, { "m3", 4 }, { "m3on", 1 },
                                           { "cutoff", 420 }, { "emph", 3 }, { "contour", 6 }, { "fd", 0.35f }, { "fs", 2 }, { "ad", 0.5f }, { "as", 8 } } },
            { seventy, "Seventy Lead",   { { "glide", 0.08f }, { "o2freq", 0.12f }, { "o3range", 4 }, { "m3", 3 }, { "m3on", 1 },
                                           { "cutoff", 1500 }, { "emph", 4 }, { "contour", 4 }, { "fd", 0.8f }, { "fs", 5 } } },
            { seventy, "Brass Section",  { { "o2freq", 0.1f }, { "cutoff", 600 }, { "emph", 1.5f }, { "contour", 6.5f }, { "fa", 0.15f }, { "fd", 0.7f }, { "fs", 6 },
                                           { "aa", 0.04f }, { "ad", 0.8f }, { "as", 9 } } },
            { seventy, "Squelch",        { { "o1wave", 3 }, { "m2on", 0 }, { "cutoff", 260 }, { "emph", 8 }, { "contour", 8 }, { "fd", 0.22f }, { "fs", 0 },
                                           { "drive", 55 }, { "glide", 0.05f } } },
            { seventy, "Sub Drop",       { { "o1range", 1 }, { "o1wave", 0 }, { "o2range", 2 }, { "o2wave", 0 }, { "glide", 0.3f },
                                           { "cutoff", 800 }, { "emph", 1 }, { "contour", 1 } } },
            { seventy, "Pluck",          { { "o1range", 3 }, { "o2range", 4 }, { "o2freq", 7.0f }, { "m2", 4 }, { "cutoff", 250 }, { "contour", 9 }, { "fd", 0.15f }, { "fs", 0 },
                                           { "ad", 0.35f }, { "as", 0 } } },
            { seventy, "Wobble",         { { "o1range", 2 }, { "o2range", 2 }, { "o3range", 0 }, { "o3key", 0 }, { "o3wave", 0 }, { "o3freq", 2.0f },
                                           { "modamt", 60 }, { "filtmod", 1 }, { "cutoff", 500 }, { "emph", 6 }, { "contour", 2 } } },
            { seventy, "Vibrato Lead",   { { "o3range", 0 }, { "o3key", 0 }, { "o3wave", 0 }, { "o3freq", 3.0f }, { "modamt", 12 }, { "oscmod", 1 },
                                           { "cutoff", 1800 }, { "emph", 3 }, { "glide", 0.06f } } },
            { seventy, "Overdrive Bass", { { "o1range", 2 }, { "o2range", 2 }, { "m1", 10 }, { "m2", 10 }, { "fb", 6 }, { "fbon", 1 }, { "drive", 70 },
                                           { "cutoff", 700 }, { "emph", 2 }, { "contour", 5 }, { "fd", 0.4f }, { "fs", 3 } } },
            { seventy, "Soft Flute",     { { "o1range", 4 }, { "o1wave", 0 }, { "m2on", 0 }, { "noise", 1.5f }, { "noiseon", 1 }, { "noisecol", 1 },
                                           { "cutoff", 2000 }, { "emph", 0 }, { "contour", 1 }, { "aa", 0.08f }, { "drive", 0 } } },
            { seventy, "Siren",          { { "o3range", 0 }, { "o3key", 0 }, { "o3wave", 2 }, { "o3freq", -3.0f }, { "modamt", 80 }, { "oscmod", 1 },
                                           { "cutoff", 3000 }, { "emph", 2 } } },
            { seventy, "Fat Stack",      { { "o1range", 2 }, { "o2range", 2 }, { "o2freq", 0.1f }, { "o3range", 3 }, { "o3wave", 3 }, { "m1", 8 }, { "m2", 8 },
                                           { "m3", 6 }, { "m3on", 1 }, { "cutoff", 700 }, { "emph", 2 }, { "contour", 6 }, { "fd", 0.5f }, { "fs", 4 },
                                           { "character", 75 } } },
            { seventy, "Funk Bass",      { { "o1range", 1 }, { "o2range", 2 }, { "o2freq", 0.06f }, { "cutoff", 350 }, { "emph", 4 }, { "contour", 7 },
                                           { "fd", 0.18f }, { "fs", 1 }, { "ad", 0.3f }, { "as", 5 }, { "glide", 0.03f } } },
            { seventy, "Prog Lead",      { { "o2freq", 0.15f }, { "o3range", 4 }, { "o3wave", 0 }, { "m1", 7 }, { "m2", 7 }, { "m3", 4 }, { "m3on", 1 },
                                           { "glide", 0.1f }, { "cutoff", 2200 }, { "emph", 3 }, { "contour", 3 }, { "fd", 0.9f }, { "fs", 6 } } },
            { seventy, "Hollow Square",  { { "o1wave", 5 }, { "o2wave", 3 }, { "o2freq", 0.05f }, { "cutoff", 1600 }, { "emph", 5 }, { "contour", 3 } } },
            { seventy, "Mixer Overload", { { "o1range", 2 }, { "o2range", 2 }, { "o2freq", 0.12f }, { "o3range", 3 }, { "m1", 10 }, { "m2", 10 }, { "m3", 10 },
                                           { "m3on", 1 }, { "drive", 60 }, { "character", 100 }, { "cutoff", 900 }, { "emph", 3 }, { "contour", 5 },
                                           { "fd", 0.6f }, { "fs", 4 } } },
            { seventy, "Noise Sweep",    { { "m1on", 0 }, { "m2on", 0 }, { "noise", 10 }, { "noiseon", 1 }, { "cutoff", 150 }, { "emph", 7 }, { "contour", 10 },
                                           { "fa", 2.0f }, { "fd", 3.0f }, { "fs", 0 }, { "aa", 0.5f }, { "ad", 3.0f }, { "as", 0 } } },

            { acid, "Acid Line",         { { "accutoff", 420 }, { "acres", 8 }, { "acenv", 6 }, { "acdecay", 0.45f }, { "acaccent", 7 }, { "drive", 20 } } },
            { acid, "Square Squelch",    { { "acwave", 1 }, { "accutoff", 300 }, { "acres", 8.5f }, { "acenv", 7 }, { "acdecay", 0.3f }, { "acaccent", 8 } } },
            { acid, "Deep Rubber",       { { "actune", -12 }, { "accutoff", 260 }, { "acres", 4 }, { "acenv", 3 }, { "acdecay", 0.9f }, { "acaccent", 4 } } },
            { acid, "Screamer",          { { "accutoff", 700 }, { "acres", 9.3f }, { "acenv", 8 }, { "acdecay", 0.35f }, { "acaccent", 9 }, { "acdist", 75 } } },
            { acid, "Liquid Slide",      { { "accutoff", 520 }, { "acres", 7 }, { "acenv", 5 }, { "acdecay", 0.6f }, { "acslide", 0.14f }, { "acaccent", 5 } } },
            { acid, "Warehouse",         { { "acwave", 1 }, { "accutoff", 380 }, { "acres", 9 }, { "acenv", 6 }, { "acdecay", 0.25f }, { "acdist", 45 }, { "acaccent", 8 } } },

            { syncLead, "Sync Lead",     {} },
            { syncLead, "Sync Brass",    { { "sybpitch", 12 }, { "syenvb", 12 }, { "syfa", 0.08f }, { "syfd", 0.8f }, { "syfs", 5 }, { "sycutoff", 1200 },
                                           { "syenv", 6 }, { "syaa", 0.03f } } },
            { syncLead, "Tearing Sweep", { { "sybpitch", 3 }, { "syenvb", 36 }, { "syfd", 1.6f }, { "syfs", 0 }, { "sycutoff", 4000 }, { "syres", 3 },
                                           { "drive", 40 } } },
            { syncLead, "Hollow Sync",   { { "syawave", 1 }, { "sybwave", 1 }, { "sywidth", 20 }, { "sybpitch", 10 }, { "syenvb", 14 } } },
            { syncLead, "Glide Sync",    { { "glide", 0.12f }, { "sybpitch", 14 }, { "syenvb", 20 }, { "sylfoamt", 20 }, { "sylfodest", 3 }, { "sylforate", 5.5f } } },
            { syncLead, "PWM Lead",      { { "sysync", 0 }, { "syawave", 1 }, { "sybwave", 1 }, { "sybpitch", 0.08f }, { "syenvb", 0 }, { "sylfoamt", 55 },
                                           { "sylfodest", 1 }, { "sylforate", 0.8f }, { "sycutoff", 1800 }, { "syenv", 3 } } },
        };
        return all;
    }

    static int patchCount() noexcept                { return (int) patches().size(); }
    static const char* patchName (int i) noexcept   { return i >= 0 && i < patchCount() ? patches()[(size_t) i].name : ""; }
    static int patchModel (int i) noexcept          { return i >= 0 && i < patchCount() ? patches()[(size_t) i].model : seventy; }

    /** A control's value in a patch: the patch's own (its model, for "model"), or the default. */
    static float patchValue (int i, const char* id)
    {
        if (i >= 0 && i < patchCount())
        {
            const auto& p = patches()[(size_t) i];
            if (std::strcmp (id, "model") == 0)
                return (float) p.model;
            for (auto& [name, value] : p.values)
                if (std::strcmp (name, id) == 0)
                    return value;
        }

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
        osRate = (float) (rate * oversampling);
        down.prepare (2);
        outDc = SynthCore::DcBlock();
        outDc.set (6.0f, (float) rate);

        for (auto& o : osc) o = SynthCore::Oscillator();
        drifts = { SynthCore::Drift (11), SynthCore::Drift (23), SynthCore::Drift (37) };
        ladder = SynthCore::Ladder();
        moog = SynthCore::MoogLadder();
        for (auto& a : oscAc) a = SynthCore::DcBlock();
        jitter.fill (0.0f);
        jitterRng.setSeed (4711);
        ampEnv = SynthCore::AnalogEnvelope();
        filterEnv = SynthCore::AnalogEnvelope();
        noiseGen = SynthCore::Noise();
        stack.clear();
        numSustained = 0;
        pitch = targetPitch = 60.0f;
        noteVelocity = 1.0f;
        lastOsc3 = lastOut = lfoPhase = accentCharge = 0.0f;
        pedal = accented = sliding = false;
        bend = modWheel = 0.0f;
        controlCounter = 0;
        os.fill (0.0f);
        samplesPerCycle = 200.0f;
        activeVoices = 0;
        prepared = true;
    }

    bool isPrepared() const noexcept    { return prepared; }

    /** The delay through the downsampler, in samples at the output rate. */
    double latencySamples() const noexcept     { return prepared ? down.latencySamples() : 0.0; }

    int model() const noexcept          { return clampInt (whole (params.model), 0, numModels - 1); }

    //==============================================================================
    void noteOn (int note, float velocity)
    {
        if (note < 0 || note > 127)
            return;

        const bool wasEmpty = stack.empty();
        stack.press (note);
        unsustain (note);
        targetPitch = (float) stack.current (notePriority());

        // Acid: overlapping notes slide and don't retrigger; others always retrigger
        const bool isAcid = model() == acid;
        sliding = isAcid && ! wasEmpty;
        const bool retrigger = isAcid ? wasEmpty : (wasEmpty || params.legato < 0.5f);

        if (retrigger)
        {
            noteVelocity = velocity;
            accented = velocity >= 100.0f / 127.0f;
            ampEnv.gate (true);
            filterEnv.gate (true);
        }

        const bool instant = isAcid ? ! sliding : params.glide <= 0.0f;
        if (instant)
            pitch = targetPitch;
    }

    void noteOff (int note)
    {
        if (pedal)
        {
            if (numSustained < (int) sustained.size())
                sustained[(size_t) numSustained++] = note;
            return;
        }

        stack.release (note);
        if (stack.empty())
        {
            ampEnv.gate (false);
            filterEnv.gate (false);
            return;
        }

        // Back to the note still held
        targetPitch = (float) stack.current (notePriority());
        const bool isAcid = model() == acid;
        if (isAcid) sliding = true;
        else if (params.glide <= 0.0f) pitch = targetPitch;
        if (! isAcid && params.legato < 0.5f) { ampEnv.gate (true); filterEnv.gate (true); }
    }

    /** Every key let go (the note rings out through its release). */
    void allNotesOff()
    {
        stack.clear();
        numSustained = 0;
        ampEnv.gate (false);
        filterEnv.gate (false);
    }

    void setPitchBend (float minusOneToOne)     { bend = minusOneToOne; }       // times the Bend Range
    void setModWheel (float zeroToOne)          { modWheel = zeroToOne; }       // adds to Seventy's Mod Amount and Sync Lead's LFO

    void setSustainPedal (bool isDown)
    {
        if (isDown) { pedal = true; return; }
        pedal = false;
        const int n = numSustained;
        numSustained = 0;
        for (int i = 0; i < n; ++i)
            noteOff (sustained[(size_t) i]);
    }

    //==============================================================================
    /** Renders n samples, adding them to L and R (the voice is mono: the same in both). */
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

    // What's going on, for drawing
    SynthCore::ScopeBuffer scope;
    std::atomic<float> samplesPerCycle { 200.0f };     // the note's period at the output rate
    std::atomic<int> activeVoices { 0 };

private:
    double rate = 44100.0;
    float osRate = 176400.0f;
    bool prepared = false;
    Downsampler down;
    std::array<float, chunk * oversampling> os {};
    std::array<float, chunk> warmed {};

    // The voice
    std::array<SynthCore::Oscillator, 3> osc;
    std::array<SynthCore::Drift, 3> drifts { SynthCore::Drift (11), SynthCore::Drift (23), SynthCore::Drift (37) };
    SynthCore::Ladder ladder;
    SynthCore::MoogLadder moog;
    std::array<SynthCore::DcBlock, 3> oscAc;
    SynthCore::DcBlock outDc;
    std::array<float, 3> jitter {};
    SynthCore::Random jitterRng { 4711 };
    SynthCore::AnalogEnvelope ampEnv, filterEnv;
    SynthCore::Noise noiseGen;
    SynthCore::NoteStack stack;
    std::array<int, 128> sustained {};
    int numSustained = 0;
    float pitch = 60.0f, targetPitch = 60.0f, noteVelocity = 1.0f, lastOsc3 = 0.0f, lastOut = 0.0f;
    float lfoPhase = 0.0f, accentCharge = 0.0f;
    bool pedal = false, accented = false, sliding = false;
    float bend = 0.0f, modWheel = 0.0f;
    int controlCounter = 0;

    static int whole (float v) noexcept                     { return (int) std::lround (v); }
    static int clampInt (int v, int lo, int hi) noexcept    { return v < lo ? lo : (v > hi ? hi : v); }
    static float mtof (float note)                          { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }
    static float level (float tenths)       { auto x = dspmath::jlimit (0.0f, 1.0f, tenths / 10.0f); return x * x * 0.6f + x * 0.4f; }

    static float rangeOctaves (float r)
    {
        static constexpr float oct[] = { -6.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
        return oct[clampInt (whole (r), 0, 5)];
    }

    void unsustain (int note) noexcept
    {
        int k = 0;
        for (int i = 0; i < numSustained; ++i)
            if (sustained[(size_t) i] != note)
                sustained[(size_t) k++] = sustained[(size_t) i];
        numSustained = k;
    }

    SynthCore::NoteStack::Priority notePriority() const
    {
        return (SynthCore::NoteStack::Priority) clampInt (whole (params.priority), 0, 2);
    }

    /** Settings shared by all models, read once per chunk. */
    struct Common
    {
        float sr, bendSemis, driftSemis, velAmt, velGain, outGain, glideCoeff, inputGain;
    };

    void renderChunk (float* L, float* R, int n)
    {
        const auto& p = params;
        Common c;
        c.sr = osRate;
        c.bendSemis = bend * p.bendRange;
        c.driftSemis = p.drift / 100.0f * 0.12f;
        c.velAmt = p.velocity / 100.0f;
        c.velGain = 1.0f - c.velAmt * (1.0f - noteVelocity);
        c.outGain = p.volume > -60.0f ? std::pow (10.0f, p.volume * 0.05f) : 0.0f;
        c.glideCoeff = p.glide <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (p.glide * 0.35f * c.sr));
        c.inputGain = 0.55f * (1.0f + p.drive / 100.0f * 2.5f);

        // The voice at four times the rate, then brought back down
        const int osN = n * oversampling;
        float cyclePitch = pitch;
        switch (model())
        {
            case acid:      cyclePitch = renderAcid (os.data(), osN, c); break;
            case syncLead:  cyclePitch = renderSync (os.data(), osN, c); break;
            default:        cyclePitch = renderSeventy (os.data(), osN, c); break;
        }
        down.process (os.data(), n);
        const float* mono = os.data();

        // The output stage: a touch of even-harmonic warmth (asymmetric), then a soft ceiling
        const float warmth = p.character / 100.0f * 0.14f;
        for (int i = 0; i < n; ++i)
        {
            const float x = mono[i] * c.outGain;
            warmed[(size_t) i] = std::tanh (outDc.process (x + warmth * x * x) * 1.4f) / 1.4f;
        }

        for (int i = 0; i < n; ++i)
        {
            L[i] += warmed[(size_t) i];
            R[i] += warmed[(size_t) i];
        }

        for (int i = 0; i < n; ++i)
            scope.push (mono[i]);

        samplesPerCycle = (float) rate / dspmath::jmax (1.0f, mtof (cyclePitch));
        activeVoices = ampEnv.isActive() ? 1 : 0;
    }

    //==============================================================================
    float renderSeventy (float* out, int osN, const Common& c)
    {
        const auto& p = params;
        const float sr = c.sr;
        const float rangeP[] = { p.o1range, p.o2range, p.o3range }, waveP[] = { p.o1wave, p.o2wave, p.o3wave };
        const float freqP[] = { 0.0f, p.o2freq, p.o3freq };
        const float mixP[] = { p.m1, p.m2, p.m3 }, mixOnP[] = { p.m1on, p.m2on, p.m3on };
        std::array<SynthCore::Wave, 3> wave;
        std::array<float, 3> octs, gains;
        for (int i = 0; i < 3; ++i)
        {
            wave[(size_t) i] = waveFor (i, whole (waveP[i]));
            octs[(size_t) i] = rangeOctaves (rangeP[i]) * 12.0f + freqP[i];
            gains[(size_t) i] = mixOnP[i] > 0.5f ? level (mixP[i]) : 0.0f;
        }

        const float fbGain = p.feedbackOn > 0.5f ? level (p.feedback) * 1.6f : 0.0f;
        const float noiseGain = p.noiseOn > 0.5f ? level (p.noise) * 0.8f : 0.0f;
        const bool pink = p.noiseColour > 0.5f;
        const bool osc3Keyed = p.osc3Key > 0.5f;
        const float tuneSemis = p.tune / 100.0f;
        const float modDepth = dspmath::jlimit (0.0f, 1.0f, p.modAmount / 100.0f + modWheel);
        const float noiseMix = p.modMix / 100.0f;
        const bool toPitch = p.oscMod > 0.5f, toFilter = p.filterMod > 0.5f;
        const float baseCutoff = p.cutoff;
        const float res = std::pow (p.emphasis / 10.0f, 1.3f);
        const float contourOct = p.contour / 10.0f * 5.5f;
        static constexpr float keyAmounts[] = { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
        const float keyAmount = keyAmounts[clampInt (whole (p.keyTrack), 0, 3)];
        const float velFilterOct = c.velAmt * noteVelocity * 1.5f;
        const float fa = p.fAttack, fd = p.fDecay, fs = p.fSustain / 10.0f;
        const float aa = p.aAttack, ad = p.aDecay, as = p.aSustain / 10.0f;
        const bool longRelease = p.release > 0.5f;

        // Character: curved ramps, square droop, pitch jitter, a mixer that overloads into the filter
        const float ch = p.character / 100.0f;
        const float curve = 0.08f * ch;
        for (auto& a : oscAc) a.set (2.0f + 9.0f * ch, sr);
        const float jitterAmt = 0.006f * ch;                 // semitones of cycle-to-cycle wander
        const float mixK = 0.45f + 0.65f * ch, mixBias = 0.18f * ch;
        const float mixNorm = 1.0f / (mixK * (1.0f - std::tanh (mixBias) * std::tanh (mixBias)));
        const float mixBiasOut = std::tanh (mixBias);
        std::array<bool, 3> ramp;
        for (size_t k = 0; k < 3; ++k)
            ramp[k] = wave[k] == SynthCore::Wave::saw || wave[k] == SynthCore::Wave::reverseSaw || wave[k] == SynthCore::Wave::shark;

        for (int i = 0; i < osN; ++i)
        {
            pitch += (targetPitch - pitch) * c.glideCoeff;
            for (auto& j : jitter)
                j += ((jitterRng.nextFloat() * 2.0f - 1.0f) * jitterAmt - j) * 0.002f;

            const float envF = filterEnv.process (fa, fd, fs, longRelease ? fd : 0.01f, sr);
            const float envA = ampEnv.process (aa, ad, as, longRelease ? ad : 0.01f, sr);

            const float noiseSample = pink ? noiseGen.pink() : noiseGen.white();
            const float modSource = lastOsc3 * (1.0f - noiseMix) + noiseSample * noiseMix;
            const float modPitch = toPitch ? modSource * modDepth * 12.0f : 0.0f;
            const float modFilter = toFilter ? modSource * modDepth * 4.0f : 0.0f;

            const float base = pitch + tuneSemis + c.bendSemis;
            const float f1 = mtof (base + octs[0] + drifts[0].process (c.driftSemis, sr) + modPitch + jitter[0]);
            const float f2 = mtof (base + octs[1] + drifts[1].process (c.driftSemis, sr) + modPitch + jitter[1]);
            const float f3 = osc3Keyed ? mtof (base + octs[2] + drifts[2].process (c.driftSemis, sr) + jitter[2]) : mtof (60.0f + octs[2]);

            std::array<float, 3> o { osc[0].process (wave[0], f1 / sr), osc[1].process (wave[1], f2 / sr), osc[2].process (wave[2], f3 / sr) };
            for (size_t k = 0; k < 3; ++k)
            {
                // A ramp charging a capacitor isn't quite straight; the output is AC coupled
                if (ramp[k]) o[k] -= curve * (o[k] * o[k] - 0.3333f);
                o[k] = oscAc[k].process (o[k]);
            }
            lastOsc3 = o[2];

            const float sum = o[0] * gains[0] + o[1] * gains[1] + o[2] * gains[2] + noiseSample * noiseGain + lastOut * fbGain;
            // The mixer: clean at moderate levels, rounding off and thickening when pushed
            const float mixed = (std::tanh (mixK * sum + mixBias) - mixBiasOut) * mixNorm;

            if ((controlCounter++ & 7) == 0)
            {
                const float keyOct = (pitch - 60.0f) / 12.0f * keyAmount;
                moog.set (baseCutoff * std::pow (2.0f, contourOct * envF + keyOct + modFilter + velFilterOct), res, sr);
            }

            const float y = moog.process (mixed * c.inputGain) * envA * c.velGain;
            lastOut = y;
            out[i] = y;
        }

        return pitch + tuneSemis + octs[0];
    }

    //==============================================================================
    float renderAcid (float* out, int osN, const Common& c)
    {
        const auto& p = params;
        const float sr = c.sr;
        const auto wave = p.acWave > 0.5f ? SynthCore::Wave::square : SynthCore::Wave::saw;
        const float tuneSemis = p.acTune;
        const float baseCutoff = p.acCutoff;
        const float res = std::pow (p.acRes / 10.0f, 1.2f) * 0.97f;
        const float accentAmt = p.acAccent / 10.0f;
        const bool acc = accented && accentAmt > 0.0f;
        const float envOct = p.acEnvMod / 10.0f * 4.5f * (acc ? 1.0f + accentAmt * 0.6f : 1.0f);
        const float decay = acc ? 0.2f : p.acDecay;          // accented notes use a short decay
        const float slideCoeff = sliding ? 1.0f - std::exp (-1.0f / (p.acSlide * 0.35f * sr)) : 1.0f;
        const float accentGain = acc ? 1.0f + accentAmt * 0.7f : 1.0f;
        const float distGain = 1.0f + p.acDist / 100.0f * 12.0f;
        const float chargeCoeff = 1.0f - std::exp (-1.0f / (0.18f * sr));
        ladder.poles = 3;

        for (int i = 0; i < osN; ++i)
        {
            pitch += (targetPitch - pitch) * slideCoeff;

            // Decay-only filter envelope; the amp holds while the key is down, then lets go quickly
            const float envF = filterEnv.process (0.001f, decay, 0.0f, decay, sr);
            const float envA = ampEnv.process (0.002f, 6.0f, 0.0f, 0.012f, sr);

            // Accent "wow": accents charge a capacitor that adds to the cutoff, so runs of
            // accents climb higher and higher
            accentCharge += ((acc ? envF : 0.0f) - accentCharge) * chargeCoeff;

            const float f = mtof (pitch + tuneSemis + c.bendSemis + drifts[0].process (c.driftSemis * 0.5f, sr));
            const float o = osc[0].process (wave, f / sr);

            if ((controlCounter++ & 7) == 0)
                ladder.set (baseCutoff * std::pow (2.0f, envOct * envF + accentCharge * accentAmt * 2.5f), res, sr);

            float y = ladder.process (o * c.inputGain) * envA * accentGain * c.velGain;
            if (distGain > 1.01f)
                y = std::tanh (y * distGain) / std::sqrt (distGain);
            out[i] = y;
        }

        return pitch + tuneSemis;
    }

    //==============================================================================
    float renderSync (float* out, int osN, const Common& c)
    {
        const auto& p = params;
        const float sr = c.sr;
        const bool pulseA = p.syAWave > 0.5f;
        const int waveB = whole (p.syBWave);
        const float width = p.syWidth / 100.0f;
        const float bPitch = p.syBPitch, envToB = p.syEnvToB;
        const bool hardSync = p.sySync > 0.5f;
        const float gainA = level (p.syMixA), gainB = level (p.syMixB), gainN = level (p.syNoise) * 0.8f;
        const float baseCutoff = p.syCutoff;
        const float res = std::pow (p.syRes / 10.0f, 1.3f);
        const float envOct = p.syEnvAmt / 10.0f * 5.5f;
        static constexpr float keyAmounts[] = { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
        const float keyAmount = keyAmounts[clampInt (whole (p.syKey), 0, 3)];
        const float fa = p.syFA, fd = p.syFD, fs = p.syFS / 10.0f, fr = p.syFR;
        const float aa = p.syAA, ad = p.syAD, as = p.syAS / 10.0f, ar = p.syAR;
        const float lfoInc = p.syLfoRate / sr;
        const float lfoAmt = dspmath::jlimit (0.0f, 1.0f, p.syLfoAmt / 100.0f + modWheel);
        const int lfoDest = whole (p.syLfoDest);
        const float velFilterOct = c.velAmt * noteVelocity * 1.5f;
        ladder.poles = 4;

        for (int i = 0; i < osN; ++i)
        {
            pitch += (targetPitch - pitch) * c.glideCoeff;

            const float envF = filterEnv.process (fa, fd, fs, fr, sr);
            const float envA = ampEnv.process (aa, ad, as, ar, sr);

            lfoPhase += lfoInc;
            if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
            const float lfo = std::sin (lfoPhase * SynthCore::twoPi) * lfoAmt;

            const float base = pitch + c.bendSemis + (lfoDest == 3 ? lfo * 0.6f : 0.0f);
            const float fA = mtof (base + drifts[0].process (c.driftSemis, sr));
            const float fB = mtof (base + bPitch + envToB * envF + (lfoDest == 0 ? lfo * 12.0f : 0.0f) + drifts[1].process (c.driftSemis, sr));
            const float w = dspmath::jlimit (0.05f, 0.95f, width + (lfoDest == 1 ? lfo * 0.42f : 0.0f));

            const float dtA = fA / sr, dtB = fB / sr;
            const float a = pulseA ? osc[0].pulse (w, dtA) : osc[0].process (SynthCore::Wave::saw, dtA);

            // Hard sync: when A starts a new cycle, B starts again too (at the matching
            // fraction of a sample, which keeps the reset clean)
            if (hardSync && osc[0].wrapped (dtA))
                osc[1].phase = osc[0].phase * dtB / dspmath::jmax (1.0e-7f, dtA);

            const float b = waveB == 1 ? osc[1].pulse (w, dtB)
                          : osc[1].process (waveB == 2 ? SynthCore::Wave::triangle : SynthCore::Wave::saw, dtB);

            const float noiseSample = gainN > 0.0f ? noiseGen.white() : 0.0f;
            const float mixed = a * gainA + b * gainB + noiseSample * gainN;

            if ((controlCounter++ & 7) == 0)
            {
                const float keyOct = (pitch - 60.0f) / 12.0f * keyAmount;
                ladder.set (baseCutoff * std::pow (2.0f, envOct * envF + keyOct + (lfoDest == 2 ? lfo * 3.0f : 0.0f) + velFilterOct), res, sr);
            }

            out[i] = ladder.process (mixed * c.inputGain) * envA * c.velGain;
        }

        return pitch;
    }
};

} // namespace elsewhere
