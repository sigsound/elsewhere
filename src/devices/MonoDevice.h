// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Mono: a monophonic analog-style synth with three models.

    Seventy (after the classic 70s three-oscillator monosynth)
      Controllers -> tune, glide, modulation (oscillator 3 and/or noise, to pitch and/or filter)
      Oscillators -> three, each with a footage range (LO, 32' ... 2') and six waveforms;
                     oscillator 3 can leave the keyboard and run as a slow modulator
      Mixer       -> the oscillators, noise (white/pink) and "feedback" (the output patched
                     back into the mixer, a famous trick for grit)
      Filter      -> 24 dB transistor ladder that saturates when the mixer is pushed and
                     self-oscillates at high emphasis, with its own contour
      Loudness    -> attack / decay / sustain; "Release" makes the release follow decay

    Acid (after the classic bassline machine)
      One oscillator (saw or square) into an 18 dB ladder with a decay-only envelope.
      Accented notes (played hard) are louder and brighter, and the accent "wow" builds
      up over runs of accents. Overlapping notes slide without retriggering.

    Sync Lead (after the classic 80s sync lead)
      Oscillator B is hard-synced to oscillator A; the filter envelope sweeps B's pitch for
      the tearing sync sound. Pulse width, ADSR filter and amp envelopes, and an LFO.

    The voice runs at four times the sample rate (oversampled), so the saturating filter, the
    feedback and hard sync stay clean of aliasing.

    Character is the analog-ness. Every model gets a warm, slightly asymmetric output stage;
    Seventy also gets the oscillators' slightly curved ramps, the droop of their squares (AC
    coupling), cycle-to-cycle pitch jitter and a mixer that overloads into the filter when it's
    pushed. Seventy's filter is a zero-delay-feedback transistor ladder that saturates in
    every stage.
*/
class MonoDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereMono";

    enum Model { seventy, acid, syncLead, numModels };

    static juce::StringArray modelNames()       { return { "Seventy", "Acid", "Sync Lead" }; }
    static juce::StringArray rangeNames()       { return { "LO", "32'", "16'", "8'", "4'", "2'" }; }
    static juce::StringArray keyTrackNames()    { return { "Off", "1/3", "2/3", "Full" }; }
    static juce::StringArray priorityNames()    { return { "Last", "Low", "High" }; }
    static juce::StringArray lfoDestNames()     { return { "B Pitch", "Width", "Cutoff", "Vibrato" }; }

    /** Seventy: oscillators 1 and 2 have the "shark tooth" (tri-saw); oscillator 3 a reverse saw instead. */
    static std::vector<SynthCore::Wave> wavesFor (int osc)
    {
        using W = SynthCore::Wave;
        return { W::triangle, osc == 2 ? W::reverseSaw : W::shark, W::saw, W::square, W::widePulse, W::narrowPulse };
    }

    static juce::StringArray waveNames (int osc)
    {
        return { "Triangle", osc == 2 ? "Reverse Saw" : "Shark Tooth", "Sawtooth", "Square", "Wide Pulse", "Narrow Pulse" };
    }

    MonoDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        auto secs = [] (float v) { return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s"; };
        auto tenths = [] (float v) { return juce::String (v, 1); };
        auto semis = [] (float v) { return (v >= 0.0f ? "+" : "") + juce::String (v, 2) + " st"; };
        auto cents = [] (float v) { return (v >= 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct"; };
        auto pick = [] (juce::StringArray names) { return [names] (float v) { return names[juce::jlimit (0, names.size() - 1, juce::roundToInt (v))]; }; };
        auto timeRange = skewed (0.001f, 10.0f, 0.4f);

        model = addParameter ("model", "Model", (float) seventy, stepped (0, numModels - 1), pick (modelNames()));

        // ---- Seventy -------------------------------------------------------------
        tune      = addParameter ("tune", "Tune", 0.0f, { -100.0f, 100.0f }, cents);
        glide     = addParameter ("glide", "Glide", 0.0f, skewed (0.0f, 5.0f, 0.3f), secs);
        modMix    = addParameter ("modmix", "Mod Mix", 0.0f, { 0.0f, 100.0f },
                                  [] (float v) { return v < 1.0f ? juce::String ("Osc 3") : v > 99.0f ? juce::String ("Noise") : juce::String (juce::roundToInt (v)) + "% noise"; });
        modAmount = addParameter ("modamt", "Mod Amount", 0.0f, { 0.0f, 100.0f }, percentText);
        oscMod    = addParameter ("oscmod", "Osc Mod", 0.0f, stepped (0, 1), onOffText);
        filterMod = addParameter ("filtmod", "Filter Mod", 0.0f, stepped (0, 1), onOffText);

        for (int i = 0; i < 3; ++i)
        {
            auto& o = oscs[(size_t) i];
            const auto id = "o" + juce::String (i + 1), nm = "Osc " + juce::String (i + 1) + " ";
            o.range = addParameter (id + "range", nm + "Range", i == 2 ? 2.0f : 3.0f, stepped (0, 5), pick (rangeNames()));
            o.wave  = addParameter (id + "wave", nm + "Wave", 2.0f, stepped (0, 5), pick (waveNames (i)));
            if (i > 0)
                o.freq = addParameter (id + "freq", nm + "Frequency", i == 1 ? 0.08f : -0.06f, { -7.0f, 7.0f }, semis);
        }
        osc3Key = addParameter ("o3key", "Osc 3 Keyboard", 1.0f, stepped (0, 1), onOffText);

        for (int i = 0; i < 3; ++i)
        {
            const auto id = "m" + juce::String (i + 1);
            mix[(size_t) i]   = addParameter (id, "Osc " + juce::String (i + 1) + " Level", i == 0 ? 7.0f : i == 1 ? 6.0f : 0.0f, { 0.0f, 10.0f }, tenths);
            mixOn[(size_t) i] = addParameter (id + "on", "Osc " + juce::String (i + 1) + " On", i < 2 ? 1.0f : 0.0f, stepped (0, 1), onOffText);
        }
        feedback    = addParameter ("fb", "Feedback", 0.0f, { 0.0f, 10.0f }, tenths);
        feedbackOn  = addParameter ("fbon", "Feedback On", 0.0f, stepped (0, 1), onOffText);
        noise       = addParameter ("noise", "Noise Level", 0.0f, { 0.0f, 10.0f }, tenths);
        noiseOn     = addParameter ("noiseon", "Noise On", 0.0f, stepped (0, 1), onOffText);
        noiseColour = addParameter ("noisecol", "Noise Colour", 0.0f, stepped (0, 1), pick ({ "White", "Pink" }));

        cutoff   = addParameter ("cutoff", "Cutoff", 1200.0f, freqRange (20.0f, 20000.0f), hzText);
        emphasis = addParameter ("emph", "Emphasis", 2.0f, { 0.0f, 10.0f }, tenths);
        contour  = addParameter ("contour", "Contour Amount", 5.0f, { 0.0f, 10.0f }, tenths);
        keyTrack = addParameter ("keytrack", "Keyboard Control", 1.0f, stepped (0, 3), pick (keyTrackNames()));
        fAttack  = addParameter ("fa", "Filter Attack", 0.005f, timeRange, secs);
        fDecay   = addParameter ("fd", "Filter Decay", 0.6f, timeRange, secs);
        fSustain = addParameter ("fs", "Filter Sustain", 3.0f, { 0.0f, 10.0f }, tenths);

        aAttack  = addParameter ("aa", "Attack", 0.003f, timeRange, secs);
        aDecay   = addParameter ("ad", "Decay", 0.6f, timeRange, secs);
        aSustain = addParameter ("as", "Sustain", 8.0f, { 0.0f, 10.0f }, tenths);
        release  = addParameter ("release", "Release", 1.0f, stepped (0, 1),
                                 [] (float v) { return v > 0.5f ? juce::String ("Follows decay") : juce::String ("Short"); });

        // ---- Acid ----------------------------------------------------------------
        acWave     = addParameter ("acwave", "Acid Wave", 0.0f, stepped (0, 1), pick ({ "Sawtooth", "Square" }));
        acTune     = addParameter ("actune", "Acid Tune", 0.0f, { -12.0f, 12.0f }, semis);
        acCutoff   = addParameter ("accutoff", "Acid Cutoff", 500.0f, freqRange (60.0f, 8000.0f), hzText);
        acRes      = addParameter ("acres", "Acid Resonance", 7.0f, { 0.0f, 10.0f }, tenths);
        acEnvMod   = addParameter ("acenv", "Env Mod", 5.0f, { 0.0f, 10.0f }, tenths);
        acDecay    = addParameter ("acdecay", "Acid Decay", 0.5f, skewed (0.1f, 3.0f, 0.6f), secs);
        acAccent   = addParameter ("acaccent", "Accent", 6.0f, { 0.0f, 10.0f }, tenths);
        acSlide    = addParameter ("acslide", "Slide Time", 0.06f, skewed (0.01f, 0.5f, 0.08f), secs);
        acDist     = addParameter ("acdist", "Distortion", 0.0f, { 0.0f, 100.0f }, percentText);

        // ---- Sync Lead -----------------------------------------------------------
        syAWave    = addParameter ("syawave", "Osc A Wave", 0.0f, stepped (0, 1), pick ({ "Sawtooth", "Pulse" }));
        syWidth    = addParameter ("sywidth", "Pulse Width", 35.0f, { 5.0f, 50.0f }, percentText);
        syBWave    = addParameter ("sybwave", "Osc B Wave", 0.0f, stepped (0, 2), pick ({ "Sawtooth", "Pulse", "Triangle" }));
        syBPitch   = addParameter ("sybpitch", "Osc B Pitch", 7.0f, { 0.0f, 36.0f }, semis);
        sySync     = addParameter ("sysync", "Sync", 1.0f, stepped (0, 1), onOffText);
        syEnvToB   = addParameter ("syenvb", "Env to B Pitch", 18.0f, { 0.0f, 48.0f }, semis);
        syMixA     = addParameter ("symixa", "Osc A Level", 5.0f, { 0.0f, 10.0f }, tenths);
        syMixB     = addParameter ("symixb", "Osc B Level", 8.0f, { 0.0f, 10.0f }, tenths);
        syNoise    = addParameter ("synoise", "Sync Noise", 0.0f, { 0.0f, 10.0f }, tenths);
        syCutoff   = addParameter ("sycutoff", "Sync Cutoff", 2500.0f, freqRange (20.0f, 20000.0f), hzText);
        syRes      = addParameter ("syres", "Sync Resonance", 2.0f, { 0.0f, 10.0f }, tenths);
        syEnvAmt   = addParameter ("syenv", "Filter Env", 4.0f, { 0.0f, 10.0f }, tenths);
        syKey      = addParameter ("sykey", "Sync Key Track", 2.0f, stepped (0, 3), pick (keyTrackNames()));
        syFA = addParameter ("syfa", "Filter Attack", 0.002f, timeRange, secs);
        syFD = addParameter ("syfd", "Filter Decay", 0.9f, timeRange, secs);
        syFS = addParameter ("syfs", "Filter Sustain", 2.0f, { 0.0f, 10.0f }, tenths);
        syFR = addParameter ("syfr", "Filter Release", 0.4f, timeRange, secs);
        syAA = addParameter ("syaa", "Amp Attack", 0.003f, timeRange, secs);
        syAD = addParameter ("syad", "Amp Decay", 0.5f, timeRange, secs);
        syAS = addParameter ("syas", "Amp Sustain", 8.0f, { 0.0f, 10.0f }, tenths);
        syAR = addParameter ("syar", "Amp Release", 0.3f, timeRange, secs);
        syLfoRate  = addParameter ("sylforate", "LFO Rate", 5.0f, skewed (0.05f, 20.0f, 3.0f), [] (float v) { return juce::String (v, 2) + " Hz"; });
        syLfoAmt   = addParameter ("sylfoamt", "LFO Amount", 0.0f, { 0.0f, 100.0f }, percentText);
        syLfoDest  = addParameter ("sylfodest", "LFO Target", 0.0f, stepped (0, 3), pick (lfoDestNames()));

        // ---- Output (all models) -------------------------------------------------
        volume   = addParameter ("volume", "Volume", -6.0f, { -60.0f, 6.0f }, dbText);
        drive    = addParameter ("drive", "Drive", 15.0f, { 0.0f, 100.0f }, percentText);
        drift    = addParameter ("drift", "Drift", 25.0f, { 0.0f, 100.0f }, percentText);
        velocity = addParameter ("vel", "Velocity", 0.0f, { 0.0f, 100.0f }, percentText);
        bendRange = addParameter ("bend", "Bend Range", 2.0f, stepped (1, 12), [] (float v) { return juce::String (juce::roundToInt (v)) + " st"; });
        legato   = addParameter ("legato", "Legato", 1.0f, stepped (0, 1), onOffText);
        priority = addParameter ("priority", "Note Priority", 0.0f, stepped (0, 2), pick (priorityNames()));
        character = addParameter ("character", "Character", 60.0f, { 0.0f, 100.0f }, percentText);
    }

    juce::String getName() const override              { return "Elsewhere Mono"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Mono"; }
    int getDeviceViewWidth() const override            { return 1400; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 3.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    double getLatencySeconds() override                 { return oversampler != nullptr ? oversampler->getLatencyInSamples() / rate : 0.0; }

    int getModel() const    { return juce::jlimit (0, numModels - 1, juce::roundToInt (model.get())); }

    //==============================================================================
    // Factory patches: bank A is Seventy, B is Acid, C is Sync Lead
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

    /** Global patch indices for a model, in order. */
    static std::vector<int> patchesFor (int m)
    {
        std::vector<int> result;
        for (int i = 0; i < (int) patches().size(); ++i)
            if (patches()[(size_t) i].model == m)
                result.push_back (i);
        return result;
    }

    /** "A001 Classic Bass": bank letter for the model, number within it. */
    static juce::String patchLabel (int index)
    {
        if (index < 0 || index >= (int) patches().size())
            return "INIT";

        const auto& p = patches()[(size_t) index];
        auto inModel = patchesFor (p.model);
        const int n = (int) (std::find (inModel.begin(), inModel.end(), index) - inModel.begin()) + 1;
        return juce::String::charToString ((juce::juce_wchar) ('A' + p.model)) + juce::String (n).paddedLeft ('0', 3) + " " + p.name;
    }

    float patchValue (const Patch& patch, te::AutomatableParameter& ap) const
    {
        if (ap.paramID == "model")
            return (float) patch.model;
        for (auto& [id, value] : patch.values)
            if (ap.paramID == id)
                return value;
        return ap.getDefaultValue().value_or (ap.getCurrentBaseValue());
    }

    /** Sets every parameter to the patch's value (or its default), as one undoable step. */
    void loadPatch (int index)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);
        const auto& patch = patches()[(size_t) index];

        edit.getUndoManager().beginNewTransaction ("Load Mono patch");
        for (auto& p : params)
            p->getParameter().setParameter (patchValue (patch, p->getParameter()), juce::sendNotification);

        state.setProperty ("patch", index, &edit.getUndoManager());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", -1); }

    /** Whether the current settings still match the loaded patch. */
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
        osRate = (float) (rate * 4.0);
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (1, 2, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        outDc.set (6.0f, (float) rate);
        oversampler->initProcessing ((size_t) maxChunk);
        work.setSize (1, maxChunk);
        work.clear();
        resetVoice();
    }

    void deinitialise() override    { oversampler.reset(); }

    void midiPanic() override
    {
        stack.clear(); sustained.clear();
        ampEnv.gate (false); filterEnv.gate (false);
    }

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

    // For the display
    SynthCore::ScopeBuffer scope;
    float getSamplesPerCycle() const    { return samplesPerCycle.load(); }

    struct OscParams { Param range, wave, freq; };
    std::array<OscParams, 3> oscs;
    std::array<Param, 3> mix, mixOn;
    Param model;
    Param tune, glide, modMix, modAmount, oscMod, filterMod, osc3Key;
    Param feedback, feedbackOn, noise, noiseOn, noiseColour;
    Param cutoff, emphasis, contour, keyTrack, fAttack, fDecay, fSustain;
    Param aAttack, aDecay, aSustain, release;
    Param acWave, acTune, acCutoff, acRes, acEnvMod, acDecay, acAccent, acSlide, acDist;
    Param syAWave, syWidth, syBWave, syBPitch, sySync, syEnvToB, syMixA, syMixB, syNoise;
    Param syCutoff, syRes, syEnvAmt, syKey, syFA, syFD, syFS, syFR, syAA, syAD, syAS, syAR;
    Param syLfoRate, syLfoAmt, syLfoDest;
    Param volume, drive, drift, velocity, bendRange, legato, priority, character;

private:
    std::array<float, 512> warmed {};
    static constexpr int maxChunk = 256;

    double rate = 44100.0;
    float osRate = 88200.0f;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> work;

    // The voice
    std::array<SynthCore::Oscillator, 3> osc;
    std::array<SynthCore::Drift, 3> drifts { SynthCore::Drift (11), SynthCore::Drift (23), SynthCore::Drift (37) };
    SynthCore::Ladder ladder;
    SynthCore::MoogLadder moog;
    std::array<SynthCore::DcBlock, 3> oscAc;
    SynthCore::DcBlock outDc;
    std::array<float, 3> jitter {};
    juce::Random jitterRng { 4711 };
    SynthCore::AnalogEnvelope ampEnv, filterEnv;
    SynthCore::Noise noiseGen;
    SynthCore::NoteStack stack;
    std::vector<int> sustained;
    float pitch = 60.0f, targetPitch = 60.0f, noteVelocity = 1.0f, lastOsc3 = 0.0f, lastOut = 0.0f;
    float lfoPhase = 0.0f, accentCharge = 0.0f;
    bool pedal = false, accented = false, sliding = false;
    float bend = 0.0f, modWheel = 0.0f;
    int controlCounter = 0;
    std::atomic<float> samplesPerCycle { 200.0f };

    void resetVoice()
    {
        for (auto& o : osc) o.reset();
        ladder.reset();
        moog.reset();
        for (auto& a : oscAc) a.reset();
        outDc.reset();
        jitter.fill (0.0f);
        ampEnv.reset(); filterEnv.reset();
        stack.clear(); sustained.clear();
        pedal = accented = sliding = false;
        bend = modWheel = lastOsc3 = lastOut = lfoPhase = accentCharge = 0.0f;
    }

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
            for (auto n : sustained) noteOff (n);
            sustained.clear();
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            midiPanic();
    }

    SynthCore::NoteStack::Priority notePriority() const
    {
        return (SynthCore::NoteStack::Priority) juce::jlimit (0, 2, juce::roundToInt (priority.get()));
    }

    void noteOn (int note, float vel)
    {
        const bool wasEmpty = stack.empty();
        stack.press (note);
        sustained.erase (std::remove (sustained.begin(), sustained.end(), note), sustained.end());
        targetPitch = (float) stack.current (notePriority());

        // Acid: overlapping notes slide and don't retrigger; others always retrigger
        const bool isAcid = getModel() == acid;
        sliding = isAcid && ! wasEmpty;
        const bool retrigger = isAcid ? wasEmpty : (wasEmpty || legato.get() < 0.5f);

        if (retrigger)
        {
            noteVelocity = vel;
            accented = vel >= 100.0f / 127.0f;
            ampEnv.gate (true);
            filterEnv.gate (true);
        }

        const bool instant = isAcid ? ! sliding : glide.get() <= 0.0f;
        if (instant)
            pitch = targetPitch;
    }

    void noteOff (int note)
    {
        if (pedal) { sustained.push_back (note); return; }

        stack.release (note);
        if (stack.empty())
        {
            ampEnv.gate (false);
            filterEnv.gate (false);
            return;
        }

        // Back to the note still held (like a monosynth keyboard)
        targetPitch = (float) stack.current (notePriority());
        const bool isAcid = getModel() == acid;
        if (isAcid) sliding = true;
        else if (glide.get() <= 0.0f) pitch = targetPitch;
        if (! isAcid && legato.get() < 0.5f) { ampEnv.gate (true); filterEnv.gate (true); }
    }

    static float rangeOctaves (float r)
    {
        static constexpr float oct[] = { -6.0f, -2.0f, -1.0f, 0.0f, 1.0f, 2.0f };
        return oct[juce::jlimit (0, 5, juce::roundToInt (r))];
    }

    static float level (float tenths)       { auto x = juce::jlimit (0.0f, 1.0f, tenths / 10.0f); return x * x * 0.6f + x * 0.4f; }
    static float mtof (float note)          { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }

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

    /** Settings shared by all models, read once per chunk. */
    struct Common
    {
        float sr, bendSemis, driftSemis, velAmt, velGain, outGain, glideCoeff, inputGain;
    };

    void renderChunk (juce::AudioBuffer<float>& buf, int start, int n)
    {
        Common c;
        c.sr = osRate;
        c.bendSemis = bend * bendRange.get();
        c.driftSemis = drift.get() / 100.0f * 0.12f;
        c.velAmt = velocity.get() / 100.0f;
        c.velGain = 1.0f - c.velAmt * (1.0f - noteVelocity);
        c.outGain = juce::Decibels::decibelsToGain (volume.get(), -60.0f);
        c.glideCoeff = glide.get() <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (glide.get() * 0.35f * c.sr));
        c.inputGain = 0.55f * (1.0f + drive.get() / 100.0f * 2.5f);

        // Render at twice the rate into the oversampler's block, then bring it back down
        auto inBlock = juce::dsp::AudioBlock<float> (work).getSubBlock (0, (size_t) n);
        auto up = oversampler->processSamplesUp (inBlock);
        float* os = up.getChannelPointer (0);
        const int osN = (int) up.getNumSamples();

        float cyclePitch = pitch;
        switch (getModel())
        {
            case acid:      cyclePitch = renderAcid (os, osN, c); break;
            case syncLead:  cyclePitch = renderSync (os, osN, c); break;
            default:        cyclePitch = renderSeventy (os, osN, c); break;
        }

        auto outBlock = juce::dsp::AudioBlock<float> (work).getSubBlock (0, (size_t) n);
        oversampler->processSamplesDown (outBlock);

        const float* mono = work.getReadPointer (0);

        // The output stage: a touch of even-harmonic warmth (asymmetric), then a soft ceiling
        const float warmth = character.get() / 100.0f * 0.14f;
        for (int i = 0; i < n; ++i)
        {
            const float x = mono[i] * c.outGain;
            warmed[(size_t) i] = std::tanh (outDc.process (x + warmth * x * x) * 1.4f) / 1.4f;
        }

        for (int ch = 0; ch < buf.getNumChannels(); ++ch)
        {
            float* dst = buf.getWritePointer (ch, start);
            for (int i = 0; i < n; ++i)
                dst[i] = warmed[(size_t) i];
        }

        for (int i = 0; i < n; ++i)
            scope.push (mono[i]);

        work.clear();
        samplesPerCycle = (float) rate / juce::jmax (1.0f, mtof (cyclePitch));
    }

    //==============================================================================
    float renderSeventy (float* os, int osN, const Common& c)
    {
        const float sr = c.sr;
        std::array<SynthCore::Wave, 3> wave;
        std::array<float, 3> octs, gains;
        for (int i = 0; i < 3; ++i)
        {
            wave[(size_t) i] = wavesFor (i)[(size_t) juce::jlimit (0, 5, juce::roundToInt (oscs[(size_t) i].wave.get()))];
            octs[(size_t) i] = rangeOctaves (oscs[(size_t) i].range.get()) * 12.0f + (i > 0 ? oscs[(size_t) i].freq.get() : 0.0f);
            gains[(size_t) i] = mixOn[(size_t) i].get() > 0.5f ? level (mix[(size_t) i].get()) : 0.0f;
        }

        const float fbGain = feedbackOn.get() > 0.5f ? level (feedback.get()) * 1.6f : 0.0f;
        const float noiseGain = noiseOn.get() > 0.5f ? level (noise.get()) * 0.8f : 0.0f;
        const bool pink = noiseColour.get() > 0.5f;
        const bool osc3Keyed = osc3Key.get() > 0.5f;
        const float tuneSemis = tune.get() / 100.0f;
        const float modDepth = juce::jlimit (0.0f, 1.0f, modAmount.get() / 100.0f + modWheel);
        const float noiseMix = modMix.get() / 100.0f;
        const bool toPitch = oscMod.get() > 0.5f, toFilter = filterMod.get() > 0.5f;
        const float baseCutoff = cutoff.get();
        const float res = std::pow (emphasis.get() / 10.0f, 1.3f);
        const float contourOct = contour.get() / 10.0f * 5.5f;
        static constexpr float keyAmounts[] = { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
        const float keyAmount = keyAmounts[juce::jlimit (0, 3, juce::roundToInt (keyTrack.get()))];
        const float velFilterOct = c.velAmt * noteVelocity * 1.5f;
        const float fa = fAttack.get(), fd = fDecay.get(), fs = fSustain.get() / 10.0f;
        const float aa = aAttack.get(), ad = aDecay.get(), as = aSustain.get() / 10.0f;
        const bool longRelease = release.get() > 0.5f;

        // Character: curved ramps, square droop, pitch jitter, a mixer that overloads into the filter
        const float ch = character.get() / 100.0f;
        const float curve = 0.08f * ch;
        for (auto& a : oscAc) a.set (2.0f + 9.0f * ch, sr);
        const float jitterAmt = 0.006f * ch;                 // semitones of cycle-to-cycle wander
        const float mixK = 0.45f + 0.65f * ch, mixBias = 0.18f * ch;
        const float mixNorm = 1.0f / (mixK * (1.0f - std::tanh (mixBias) * std::tanh (mixBias)));
        const float mixBiasOut = std::tanh (mixBias);
        std::array<bool, 3> ramp;
        for (int k = 0; k < 3; ++k)
            ramp[(size_t) k] = wave[(size_t) k] == SynthCore::Wave::saw || wave[(size_t) k] == SynthCore::Wave::reverseSaw || wave[(size_t) k] == SynthCore::Wave::shark;

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

            const float out = moog.process (mixed * c.inputGain) * envA * c.velGain;
            lastOut = out;
            os[i] = out;
        }

        return pitch + tuneSemis + octs[0];
    }

    //==============================================================================
    float renderAcid (float* os, int osN, const Common& c)
    {
        const float sr = c.sr;
        const auto wave = acWave.get() > 0.5f ? SynthCore::Wave::square : SynthCore::Wave::saw;
        const float tuneSemis = acTune.get();
        const float baseCutoff = acCutoff.get();
        const float res = std::pow (acRes.get() / 10.0f, 1.2f) * 0.97f;
        const float accentAmt = acAccent.get() / 10.0f;
        const bool acc = accented && accentAmt > 0.0f;
        const float envOct = acEnvMod.get() / 10.0f * 4.5f * (acc ? 1.0f + accentAmt * 0.6f : 1.0f);
        const float decay = acc ? 0.2f : acDecay.get();     // accented notes use a short decay, as on the original
        const float slideCoeff = sliding ? 1.0f - std::exp (-1.0f / (acSlide.get() * 0.35f * sr)) : 1.0f;
        const float accentGain = acc ? 1.0f + accentAmt * 0.7f : 1.0f;
        const float distGain = 1.0f + acDist.get() / 100.0f * 12.0f;
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

            float out = ladder.process (o * c.inputGain) * envA * accentGain * c.velGain;
            if (distGain > 1.01f)
                out = std::tanh (out * distGain) / std::sqrt (distGain);
            os[i] = out;
        }

        return pitch + tuneSemis;
    }

    //==============================================================================
    float renderSync (float* os, int osN, const Common& c)
    {
        const float sr = c.sr;
        const bool pulseA = syAWave.get() > 0.5f;
        const int waveB = juce::roundToInt (syBWave.get());
        const float width = syWidth.get() / 100.0f;
        const float bPitch = syBPitch.get(), envToB = syEnvToB.get();
        const bool hardSync = sySync.get() > 0.5f;
        const float gainA = level (syMixA.get()), gainB = level (syMixB.get()), gainN = level (syNoise.get()) * 0.8f;
        const float baseCutoff = syCutoff.get();
        const float res = std::pow (syRes.get() / 10.0f, 1.3f);
        const float envOct = syEnvAmt.get() / 10.0f * 5.5f;
        static constexpr float keyAmounts[] = { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
        const float keyAmount = keyAmounts[juce::jlimit (0, 3, juce::roundToInt (syKey.get()))];
        const float fa = syFA.get(), fd = syFD.get(), fs = syFS.get() / 10.0f, fr = syFR.get();
        const float aa = syAA.get(), ad = syAD.get(), as = syAS.get() / 10.0f, ar = syAR.get();
        const float lfoInc = syLfoRate.get() / sr;
        const float lfoAmt = juce::jlimit (0.0f, 1.0f, syLfoAmt.get() / 100.0f + modWheel);
        const int lfoDest = juce::roundToInt (syLfoDest.get());
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
            const float w = juce::jlimit (0.05f, 0.95f, width + (lfoDest == 1 ? lfo * 0.42f : 0.0f));

            const float dtA = fA / sr, dtB = fB / sr;
            const float a = pulseA ? osc[0].pulse (w, dtA) : osc[0].process (SynthCore::Wave::saw, dtA);

            // Hard sync: when A starts a new cycle, B starts again too (at the matching
            // fraction of a sample, which keeps the reset clean)
            if (hardSync && osc[0].wrapped (dtA))
                osc[1].phase = osc[0].phase * dtB / juce::jmax (1.0e-7f, dtA);

            const float b = waveB == 1 ? osc[1].pulse (w, dtB)
                          : osc[1].process (waveB == 2 ? SynthCore::Wave::triangle : SynthCore::Wave::saw, dtB);

            const float noiseSample = gainN > 0.0f ? noiseGen.white() : 0.0f;
            const float mixed = a * gainA + b * gainB + noiseSample * gainN;

            if ((controlCounter++ & 7) == 0)
            {
                const float keyOct = (pitch - 60.0f) / 12.0f * keyAmount;
                ladder.set (baseCutoff * std::pow (2.0f, envOct * envF + keyOct + (lfoDest == 2 ? lfo * 3.0f : 0.0f) + velFilterOct), res, sr);
            }

            os[i] = ladder.process (mixed * c.inputGain) * envA * c.velGain;
        }

        return pitch;
    }
};

//==============================================================================
using MonoPanelBase = SynthPanel::PanelBase;

//==============================================================================
/** Seventy: controllers, oscillator bank, mixer, filter, loudness. */
class SeventyPanel : public MonoPanelBase
{
public:
    SeventyPanel (MonoDevice& d)
    {
        const auto cCtrl = Colours::clips[6], cOsc = Colours::accent, cMix = Colours::clips[5], cFilt = Colours::solo, cLoud = Colours::clips[2];

        ctrl = { knob (d.tune, "Tune", cCtrl), knob (d.glide, "Glide", cCtrl), knob (d.modMix, "Mod Mix", cCtrl), knob (d.modAmount, "Mod", cCtrl) };
        ctrlToggles = { toggle (d.oscMod, "Osc Mod", cCtrl), toggle (d.filterMod, "Filter Mod", cCtrl) };

        for (int i = 0; i < 3; ++i)
        {
            auto& o = d.oscs[(size_t) i];
            rangeSel[(size_t) i] = selector (o.range, MonoDevice::rangeNames(), cOsc);
            waveSel[(size_t) i] = waves (o.wave, MonoDevice::wavesFor (i), cOsc);
            if (i > 0) freqKnob[(size_t) i] = knob (o.freq, "Freq", cOsc);
        }
        osc3Key = toggle (d.osc3Key, "Key", cOsc);

        const char* mixNames[] = { "Osc 1", "Osc 2", "Osc 3" };
        for (int i = 0; i < 3; ++i)
        {
            mixToggles.push_back (toggle (d.mixOn[(size_t) i], mixNames[i], cMix));
            bars.push_back (bar (d.mix[(size_t) i], cMix));
        }
        mixToggles.push_back (toggle (d.feedbackOn, "Feedback", cMix)); bars.push_back (bar (d.feedback, cMix));
        mixToggles.push_back (toggle (d.noiseOn, "Noise", cMix));       bars.push_back (bar (d.noise, cMix));
        noiseColour = selector (d.noiseColour, { "W", "P" }, cMix);

        filt = { knob (d.cutoff, "Cutoff", cFilt), knob (d.emphasis, "Emphasis", cFilt), knob (d.contour, "Contour", cFilt), knob (d.keyTrack, "Key", cFilt),
                 knob (d.fAttack, "Attack", cFilt), knob (d.fDecay, "Decay", cFilt), knob (d.fSustain, "Sustain", cFilt) };
        loud = { knob (d.aAttack, "Attack", cLoud), knob (d.aDecay, "Decay", cLoud), knob (d.aSustain, "Sustain", cLoud) };
        releaseToggle = toggle (d.release, "Release", cLoud);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, areas[0], "Controllers", Colours::clips[6]);
        SynthPanel::paintSection (g, areas[1], "Oscillators", Colours::accent);
        SynthPanel::paintSection (g, areas[2], "Mixer", Colours::clips[5]);
        SynthPanel::paintSection (g, areas[3], "Filter", Colours::solo);
        SynthPanel::paintSection (g, areas[4], "Loudness", Colours::clips[2]);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        for (int i = 0; i < 3; ++i)
            g.drawText (juce::String (i + 1), oscRows[(size_t) i].withWidth (14), juce::Justification::centred);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 118, 320, 166, 220, 140 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        {
            auto c = content (areas[0]);
            auto toggles = c.removeFromBottom (36);
            const int w = c.getWidth() / 2, h = c.getHeight() / 2;
            for (size_t i = 0; i < ctrl.size(); ++i)
                ctrl[i]->setBounds (c.getX() + (int) (i % 2) * w, c.getY() + (int) (i / 2) * h, w, h);
            for (auto* t : ctrlToggles) t->setBounds (toggles.removeFromTop (18));
        }

        {
            auto c = content (areas[1]);
            osc3Key->setBounds (areas[1].withHeight (16).removeFromRight (52).translated (-6, 0));
            const int rowH = c.getHeight() / 3;
            for (int i = 0; i < 3; ++i)
            {
                auto r = c.removeFromTop (rowH);
                oscRows[(size_t) i] = r;
                r.removeFromLeft (16);
                const int selH = juce::jmin (20, r.getHeight() - 8);
                rangeSel[(size_t) i]->setBounds (r.removeFromLeft (124).withSizeKeepingCentre (124, selH));
                r.removeFromLeft (4);
                auto knobArea = r.removeFromLeft (46);
                if (freqKnob[(size_t) i] != nullptr) freqKnob[(size_t) i]->setBounds (knobArea);
                r.removeFromLeft (4);
                waveSel[(size_t) i]->setBounds (r.withSizeKeepingCentre (r.getWidth(), selH));
            }
        }

        {
            auto c = content (areas[2]);
            const int rowH = c.getHeight() / 5;
            for (int i = 0; i < 5; ++i)
            {
                auto r = c.removeFromTop (rowH);
                mixToggles[(size_t) i]->setBounds (r.removeFromLeft (70));
                if (i == 4)
                {
                    noiseColour->setBounds (r.removeFromRight (30).withSizeKeepingCentre (30, juce::jmin (16, rowH - 4)));
                    r.removeFromRight (4);
                }
                bars[(size_t) i]->setBounds (r);
            }
        }

        {
            auto c = content (areas[3]);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { filt[0], filt[1], filt[2], filt[3] }, 4);
            row (c, { filt[4], filt[5], filt[6] }, 4);
        }

        {
            auto c = content (areas[4]);
            auto bottom = c.removeFromBottom (c.getHeight() / 2);
            row (c, { loud[0], loud[1], loud[2] }, 3);
            releaseToggle->setBounds (bottom.removeFromTop (20).reduced (4, 0));
        }
    }

private:
    std::array<juce::Rectangle<int>, 5> areas;
    std::array<juce::Rectangle<int>, 3> oscRows;
    std::vector<SynthPanel::Knob*> ctrl, filt, loud;
    std::vector<SynthPanel::Toggle*> ctrlToggles, mixToggles;
    std::vector<SynthPanel::BarSlider*> bars;
    std::array<SynthPanel::Selector*, 3> rangeSel {}, waveSel {};
    std::array<SynthPanel::Knob*, 3> freqKnob {};
    SynthPanel::Toggle* osc3Key = nullptr;
    SynthPanel::Toggle* releaseToggle = nullptr;
    SynthPanel::Selector* noiseColour = nullptr;
};

//==============================================================================
/** Acid: one row of big controls, like the original's panel. */
class AcidPanel : public MonoPanelBase
{
public:
    AcidPanel (MonoDevice& d)
    {
        const auto cOsc = Colours::accent, cFilt = Colours::solo, cAcc = Colours::clips[6], cDist = Colours::record;
        waveSel = waves (d.acWave, { SynthCore::Wave::saw, SynthCore::Wave::square }, cOsc);
        tune = knob (d.acTune, "Tune", cOsc);
        filt = { knob (d.acCutoff, "Cutoff", cFilt), knob (d.acRes, "Resonance", cFilt), knob (d.acEnvMod, "Env Mod", cFilt), knob (d.acDecay, "Decay", cFilt) };
        acc = { knob (d.acAccent, "Accent", cAcc), knob (d.acSlide, "Slide", cAcc) };
        dist = knob (d.acDist, "Distortion", cDist);

        // Big knobs, like the original's single row
        for (auto* k : { tune, filt[0], filt[1], filt[2], filt[3], acc[0], acc[1], dist })
            k->maxSize = 56;
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, areas[0], "Oscillator", Colours::accent);
        SynthPanel::paintSection (g, areas[1], "Filter", Colours::solo);
        SynthPanel::paintSection (g, areas[2], "Accent & Slide", Colours::clips[6]);
        SynthPanel::paintSection (g, areas[3], "Distortion", Colours::record);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawFittedText ("Play hard (velocity 100+) to accent a note; overlap notes to slide between them.",
                          hint, juce::Justification::centredLeft, 2);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 170, 440, 230, 150 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        auto big = [] (juce::Rectangle<int> c) { return c.withTrimmedBottom (c.getHeight() / 4); };

        {
            auto c = content (areas[0]);
            waveSel->setBounds (c.removeFromTop (24).reduced (4, 0));
            tune->setBounds (big (c));
        }
        { auto c = content (areas[1]); row (big (c), { filt[0], filt[1], filt[2], filt[3] }, 4); }
        {
            auto c = content (areas[2]);
            hint = c.removeFromBottom (c.getHeight() / 4).reduced (4, 0);
            row (c, { acc[0], acc[1] }, 2);
        }
        { auto c = content (areas[3]); dist->setBounds (big (c)); }
    }

private:
    std::array<juce::Rectangle<int>, 4> areas;
    juce::Rectangle<int> hint;
    SynthPanel::Selector* waveSel = nullptr;
    SynthPanel::Knob* tune = nullptr;
    SynthPanel::Knob* dist = nullptr;
    std::vector<SynthPanel::Knob*> filt, acc;
};

//==============================================================================
/** Sync Lead: two oscillators, mixer, filter, amp, LFO. */
class SyncPanel : public MonoPanelBase
{
public:
    SyncPanel (MonoDevice& d)
    {
        const auto cOsc = Colours::accent, cMix = Colours::clips[5], cFilt = Colours::solo, cAmp = Colours::clips[2], cLfo = Colours::clips[6];
        aWave = waves (d.syAWave, { SynthCore::Wave::saw, SynthCore::Wave::square }, cOsc);
        aKnobs = { knob (d.syWidth, "Width", cOsc), knob (d.glide, "Glide", cOsc) };
        bWave = waves (d.syBWave, { SynthCore::Wave::saw, SynthCore::Wave::square, SynthCore::Wave::triangle }, cOsc);
        bKnobs = { knob (d.syBPitch, "Pitch", cOsc), knob (d.syEnvToB, "Env > B", cOsc) };
        syncToggle = toggle (d.sySync, "Sync", cOsc);

        mixNames = { "Osc A", "Osc B", "Noise" };
        bars = { bar (d.syMixA, cMix), bar (d.syMixB, cMix), bar (d.syNoise, cMix) };

        filt = { knob (d.syCutoff, "Cutoff", cFilt), knob (d.syRes, "Resonance", cFilt), knob (d.syEnvAmt, "Env", cFilt), knob (d.syKey, "Key", cFilt),
                 knob (d.syFA, "A", cFilt), knob (d.syFD, "D", cFilt), knob (d.syFS, "S", cFilt), knob (d.syFR, "R", cFilt) };
        amp = { knob (d.syAA, "A", cAmp), knob (d.syAD, "D", cAmp), knob (d.syAS, "S", cAmp), knob (d.syAR, "R", cAmp) };
        lfo = { knob (d.syLfoRate, "Rate", cLfo), knob (d.syLfoAmt, "Amount", cLfo) };
        lfoDest = selector (d.syLfoDest, { "B", "PW", "Cut", "Vib" }, cLfo);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, areas[0], "Osc A", Colours::accent);
        SynthPanel::paintSection (g, areas[1], "Osc B", Colours::accent);
        SynthPanel::paintSection (g, areas[2], "Mixer", Colours::clips[5]);
        SynthPanel::paintSection (g, areas[3], "Filter", Colours::solo);
        SynthPanel::paintSection (g, areas[4], "Amp", Colours::clips[2]);
        SynthPanel::paintSection (g, areas[5], "LFO", Colours::clips[6]);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        for (int i = 0; i < 3; ++i)
            g.drawText (mixNames[i], mixRows[(size_t) i].withWidth (44), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 130, 190, 140, 250, 140, 130 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        {
            auto c = content (areas[0]);
            aWave->setBounds (c.removeFromTop (22).reduced (2, 0));
            c.removeFromTop (4);
            row (c, { aKnobs[0], aKnobs[1] }, 2);
        }
        {
            auto c = content (areas[1]);
            auto top = c.removeFromTop (22);
            syncToggle->setBounds (top.removeFromRight (54));
            top.removeFromRight (4);
            bWave->setBounds (top.reduced (2, 0));
            c.removeFromTop (4);
            row (c, { bKnobs[0], bKnobs[1] }, 2);
        }
        {
            auto c = content (areas[2]);
            const int rowH = c.getHeight() / 3;
            for (int i = 0; i < 3; ++i)
            {
                auto r = c.removeFromTop (rowH);
                mixRows[(size_t) i] = r;
                bars[(size_t) i]->setBounds (r.withTrimmedLeft (46));
            }
        }
        {
            auto c = content (areas[3]);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { filt[0], filt[1], filt[2], filt[3] }, 4);
            row (c, { filt[4], filt[5], filt[6], filt[7] }, 4);
        }
        {
            auto c = content (areas[4]);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { amp[0], amp[1] }, 2);
            row (c, { amp[2], amp[3] }, 2);
        }
        {
            auto c = content (areas[5]);
            lfoDest->setBounds (c.removeFromBottom (20));
            c.removeFromBottom (4);
            row (c, { lfo[0], lfo[1] }, 2);
        }
    }

private:
    std::array<juce::Rectangle<int>, 6> areas;
    std::array<juce::Rectangle<int>, 3> mixRows;
    juce::StringArray mixNames;
    SynthPanel::Selector *aWave = nullptr, *bWave = nullptr, *lfoDest = nullptr;
    SynthPanel::Toggle* syncToggle = nullptr;
    std::vector<SynthPanel::Knob*> aKnobs, bKnobs, filt, amp, lfo;
    std::vector<SynthPanel::BarSlider*> bars;
};

//==============================================================================
/** The Mono: model choice and display on the left, the model's panel, then the output. */
class MonoView : public MonoPanelBase, private juce::Timer
{
public:
    MonoView (MonoDevice& d) : mono (d)
    {
        modelSel = selector (d.model, MonoDevice::modelNames(), Colours::accent);

        oled.readScope = [this] (float* dst, int n) { mono.scope.read (dst, n); };
        oled.getSamplesPerCycle = [this] { return mono.getSamplesPerCycle(); };
        addAndMakeVisible (oled);
        onTouch = [this] (te::AutomatableParameter& p) { oled.showParameter (p); };

        for (auto* b : { &prevButton, &nextButton, &patchButton })
            addAndMakeVisible (b);
        prevButton.onClick = [this] { step (-1); };
        prevButton.setTitle ("Previous Patch");
        nextButton.onClick = [this] { step (1); };
        nextButton.setTitle ("Next Patch");
        patchButton.onClick = [this] { showPatchMenu(); };

        const auto cOut = Colours::accent;
        out = { knob (d.volume, "Volume", cOut), knob (d.drive, "Drive", cOut), knob (d.drift, "Drift", cOut),
                knob (d.velocity, "Velocity", cOut), knob (d.bendRange, "Bend", cOut), knob (d.character, "Character", cOut) };
        legatoToggle = toggle (d.legato, "Legato", cOut);
        prioritySel = selector (d.priority, MonoDevice::priorityNames(), cOut);

        rebuildBody();
        updatePatchName();
        startTimerHz (5);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, outArea, "Output", Colours::accent);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        const int gap = 6;

        auto display = r.removeFromLeft (200);
        modelSel->setBounds (display.removeFromTop (20));
        display.removeFromTop (4);
        auto nav = display.removeFromBottom (24);
        display.removeFromBottom (4);
        oled.setBounds (display);
        prevButton.setBounds (nav.removeFromLeft (30));
        nav.removeFromLeft (4);
        nextButton.setBounds (nav.removeFromLeft (30));
        nav.removeFromLeft (4);
        patchButton.setBounds (nav);
        r.removeFromLeft (gap);

        outArea = r.removeFromRight (186);
        r.removeFromRight (gap);
        if (body != nullptr) body->setBounds (r);
        sections = { outArea };

        auto c = content (outArea);
        auto bottom = c.removeFromBottom (22);
        auto top = c.removeFromTop (c.getHeight() / 2);
        row (top, { out[0], out[1], out[2] }, 3);
        row (c, { out[3], out[4], out[5] }, 3);
        legatoToggle->setBounds (bottom.removeFromLeft (64));
        prioritySel->setBounds (bottom.withSizeKeepingCentre (bottom.getWidth(), 18));
    }

private:
    MonoDevice& mono;
    SynthPanel::Oled oled;
    juce::TextButton prevButton { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextButton { juce::String::fromUTF8 ("\xe2\x96\xb6") }, patchButton { "Patches" };
    std::unique_ptr<MonoPanelBase> body;
    int bodyModel = -1;
    SynthPanel::Selector* modelSel = nullptr;
    SynthPanel::Selector* prioritySel = nullptr;
    SynthPanel::Toggle* legatoToggle = nullptr;
    std::vector<SynthPanel::Knob*> out;
    juce::Rectangle<int> outArea;

    void rebuildBody()
    {
        bodyModel = mono.getModel();
        body.reset();
        switch (bodyModel)
        {
            case MonoDevice::acid:      body = std::make_unique<AcidPanel> (mono); break;
            case MonoDevice::syncLead:  body = std::make_unique<SyncPanel> (mono); break;
            default:                    body = std::make_unique<SeventyPanel> (mono); break;
        }
        body->onTouch = [this] (te::AutomatableParameter& p) { oled.showParameter (p); };
        addAndMakeVisible (*body);
        oled.header = MonoDevice::modelNames()[bodyModel].toUpperCase() + "  /  MONO";
        resized();
    }

    void step (int delta)
    {
        auto list = MonoDevice::patchesFor (mono.getModel());
        if (list.empty()) return;
        auto it = std::find (list.begin(), list.end(), mono.getPatchIndex());
        int pos = it == list.end() ? (delta > 0 ? -1 : 0) : (int) (it - list.begin());
        pos = (pos + delta + (int) list.size()) % (int) list.size();
        mono.loadPatch (list[(size_t) pos]);
        updatePatchName();
    }

    void showPatchMenu()
    {
        juce::PopupMenu m;
        for (int model = 0; model < MonoDevice::numModels; ++model)
        {
            juce::PopupMenu sub;
            for (auto i : MonoDevice::patchesFor (model))
                sub.addItem (i + 1, MonoDevice::patchLabel (i), true, i == mono.getPatchIndex());
            m.addSubMenu (MonoDevice::modelNames()[model], sub, true, nullptr, model == mono.getModel());
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchButton), [safe = juce::Component::SafePointer<MonoView> (this)] (int result)
        {
            if (safe != nullptr && result > 0)
            {
                safe->mono.loadPatch (result - 1);
                safe->updatePatchName();
            }
        });
    }

    void updatePatchName()
    {
        const int index = mono.getPatchIndex();
        auto name = MonoDevice::patchLabel (index);
        if (index >= 0 && mono.isPatchModified())
            name << " *";
        oled.patchName = name;
    }

    void timerCallback() override
    {
        if (mono.getModel() != bodyModel)
            rebuildBody();
        updatePatchName();
    }
};

inline std::unique_ptr<juce::Component> MonoDevice::createDeviceView()     { return std::make_unique<MonoView> (*this); }

} // namespace elsewhere
