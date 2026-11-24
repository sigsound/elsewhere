// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Poly: a polyphonic analog-style synth with three models.

    Prism (after modern hybrid analog polys)
      Two analog-style oscillators with a Shape control (saw morphs towards a thin pulse,
      triangle folds, square narrows), sync, ring and cross mod; a digital "Multi" engine
      (filtered noise, or two-operator FM); a 2- or 4-pole filter with drive; amp and mod
      envelopes; an LFO.

    Quad (after four-voice analog boxes)
      Two oscillators with pulse width, a sub oscillator and noise; two filters in series
      (24 dB ladder low-pass into a 12 dB high-pass), each with its own envelope amount;
      per-voice overdrive; an LFO with five destinations. Four voices.

    Velvet (after the 80s chorus polys)
      One oscillator producing saw, pulse (with PWM from the LFO or envelope), a sub and
      noise from one phase; a 4-step high-pass; a warm 24 dB low-pass; one envelope; an LFO
      with a delayed fade-in. Best with the stereo chorus on (I, II or I+II).

    Nova (after the great 70s polyphonic flagship)
      Two complete channels per voice, detuned against each other: each has its own
      oscillator (saw, square or pulse, with a sine), footage, and a high-pass into a
      resonant low-pass. The filter envelope starts at an Initial Level and moves to an Attack
      Level before it decays (the slow brassy swell), and touch shapes every note: velocity
      sets brightness and level, polyphonic aftertouch (or channel pressure) adds
      brightness, vibrato and a pitch growl. Huge brass, glassy pads, singing leads.

    Shared: voice modes (Poly, Unison, Mono, Chord), voices, glide, drift, velocity,
    bend, and the chorus. Voices run at twice the sample rate (oversampled).
*/
class PolyDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewherePoly";
    static constexpr int maxVoices = 16;

    enum Model { prism, quad, velvet, nova, numModels };
    enum VoiceMode { poly, unison, mono, chord };

    static juce::StringArray modelNames()       { return { "Prism", "Quad", "Velvet", "Nova" }; }
    static juce::StringArray voiceModeNames()   { return { "Poly", "Unison", "Mono", "Chord" }; }
    static juce::StringArray chordNames()       { return { "5th", "sus2", "Minor", "Major", "sus4", "m7", "7", "Maj7", "m9", "Maj9" }; }
    static juce::StringArray chorusNames()      { return { "Off", "I", "II", "I+II" }; }

    static std::vector<int> chordIntervals (int c)
    {
        switch (c)
        {
            case 0:  return { 0, 7, 12 };
            case 1:  return { 0, 2, 7 };
            case 2:  return { 0, 3, 7 };
            case 3:  return { 0, 4, 7 };
            case 4:  return { 0, 5, 7 };
            case 5:  return { 0, 3, 7, 10 };
            case 6:  return { 0, 4, 7, 10 };
            case 7:  return { 0, 4, 7, 11 };
            case 8:  return { 0, 3, 7, 10, 14 };
            default: return { 0, 4, 7, 11, 14 };
        }
    }

    PolyDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        auto secs = [] (float v) { return v < 1.0f ? juce::String (juce::roundToInt (v * 1000.0f)) + " ms" : juce::String (v, 2) + " s"; };
        auto tenths = [] (float v) { return juce::String (v, 1); };
        auto semis = [] (float v) { return (v >= 0.0f ? "+" : "") + juce::String (v, 1) + " st"; };
        auto cents = [] (float v) { return (v >= 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " ct"; };
        auto signedPct = [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " %"; };
        auto hz = [] (float v) { return juce::String (v, v < 10.0f ? 2 : 1) + " Hz"; };
        auto pick = [] (juce::StringArray names) { return [names] (float v) { return names[juce::jlimit (0, names.size() - 1, juce::roundToInt (v))]; }; };
        auto timeRange = skewed (0.001f, 10.0f, 0.4f);
        auto octNames = juce::StringArray { "16'", "8'", "4'", "2'" };

        // ---- Shared ----
        model     = addParameter ("model", "Model", (float) prism, stepped (0, numModels - 1), pick (modelNames()));
        voiceMode = addParameter ("vmode", "Voice Mode", (float) poly, stepped (0, 3), pick (voiceModeNames()));
        voices    = addParameter ("voices", "Voices", 8.0f, stepped (1, maxVoices), [] (float v) { return juce::String (juce::roundToInt (v)); });
        detune    = addParameter ("udetune", "Unison Detune", 25.0f, { 0.0f, 100.0f }, percentText);
        chordType = addParameter ("chord", "Chord", 3.0f, stepped (0, 9), pick (chordNames()));
        glide     = addParameter ("glide", "Glide", 0.0f, skewed (0.0f, 3.0f, 0.25f), secs);
        volume    = addParameter ("volume", "Volume", -8.0f, { -60.0f, 6.0f }, dbText);
        velocity  = addParameter ("vel", "Velocity", 40.0f, { 0.0f, 100.0f }, percentText);
        bendRange = addParameter ("bend", "Bend Range", 2.0f, stepped (1, 12), [] (float v) { return juce::String (juce::roundToInt (v)) + " st"; });
        drift     = addParameter ("drift", "Drift", 25.0f, { 0.0f, 100.0f }, percentText);
        chorus    = addParameter ("chorus", "Chorus", 0.0f, stepped (0, 3), pick (chorusNames()));

        // ---- Prism ----
        auto waveP = pick ({ "Saw", "Triangle", "Square" });
        pWave1 = addParameter ("p1wave", "VCO 1 Wave", 0.0f, stepped (0, 2), waveP);
        pShape1 = addParameter ("p1shape", "VCO 1 Shape", 0.0f, { 0.0f, 100.0f }, percentText);
        pOct1 = addParameter ("p1oct", "VCO 1 Octave", 1.0f, stepped (0, 3), pick (octNames));
        pWave2 = addParameter ("p2wave", "VCO 2 Wave", 0.0f, stepped (0, 2), waveP);
        pShape2 = addParameter ("p2shape", "VCO 2 Shape", 0.0f, { 0.0f, 100.0f }, percentText);
        pOct2 = addParameter ("p2oct", "VCO 2 Octave", 1.0f, stepped (0, 3), pick (octNames));
        pPitch2 = addParameter ("p2pitch", "VCO 2 Pitch", 7.0f, { -1200.0f, 1200.0f }, cents);
        pSync = addParameter ("p2sync", "Sync", 0.0f, stepped (0, 1), onOffText);
        pRing = addParameter ("p2ring", "Ring", 0.0f, stepped (0, 1), onOffText);
        pXmod = addParameter ("pxmod", "Cross Mod", 0.0f, { 0.0f, 100.0f }, percentText);
        pMultiType = addParameter ("pmtype", "Multi Type", 1.0f, stepped (0, 1), pick ({ "Noise", "FM" }));
        pMultiShape = addParameter ("pmshape", "Multi Shape", 30.0f, { 0.0f, 100.0f }, percentText);
        pMultiRatio = addParameter ("pmratio", "FM Ratio", 3.0f, stepped (0, 7), pick (ratioNames()));
        pMix1 = addParameter ("pmix1", "VCO 1 Level", 7.0f, { 0.0f, 10.0f }, tenths);
        pMix2 = addParameter ("pmix2", "VCO 2 Level", 6.0f, { 0.0f, 10.0f }, tenths);
        pMixM = addParameter ("pmixm", "Multi Level", 0.0f, { 0.0f, 10.0f }, tenths);
        pCutoff = addParameter ("pcutoff", "Cutoff", 3000.0f, freqRange (20.0f, 20000.0f), hzText);
        pRes = addParameter ("pres", "Resonance", 2.0f, { 0.0f, 10.0f }, tenths);
        pDrive = addParameter ("pdrive", "Drive", 0.0f, stepped (0, 2), pick ({ "0%", "50%", "100%" }));
        pPoles = addParameter ("ppoles", "Filter Type", 1.0f, stepped (0, 1), pick ({ "2-Pole", "4-Pole" }));
        pKey = addParameter ("pkey", "Key Track", 1.0f, stepped (0, 2), pick ({ "0%", "50%", "100%" }));
        pEgInt = addParameter ("pegint", "EG Int", 40.0f, { -100.0f, 100.0f }, signedPct);
        pEgTarget = addParameter ("pegtarget", "EG Target", 0.0f, stepped (0, 2), pick ({ "Cutoff", "Pitch", "Pitch 2" }));
        pAA = addParameter ("paa", "Amp Attack", 0.005f, timeRange, secs);
        pAD = addParameter ("pad", "Amp Decay", 0.6f, timeRange, secs);
        pAS = addParameter ("pas", "Amp Sustain", 8.0f, { 0.0f, 10.0f }, tenths);
        pAR = addParameter ("par", "Amp Release", 0.4f, timeRange, secs);
        pEA = addParameter ("pea", "EG Attack", 0.005f, timeRange, secs);
        pED = addParameter ("ped", "EG Decay", 0.5f, timeRange, secs);
        pES = addParameter ("pes", "EG Sustain", 2.0f, { 0.0f, 10.0f }, tenths);
        pER = addParameter ("per", "EG Release", 0.4f, timeRange, secs);
        pLfoWave = addParameter ("plfowave", "LFO Wave", 1.0f, stepped (0, 2), pick ({ "Saw", "Triangle", "Square" }));
        pLfoRate = addParameter ("plforate", "LFO Rate", 2.0f, skewed (0.05f, 30.0f, 3.0f), hz);
        pLfoInt = addParameter ("plfoint", "LFO Int", 0.0f, { -100.0f, 100.0f }, signedPct);
        pLfoTarget = addParameter ("plfotarget", "LFO Target", 0.0f, stepped (0, 2), pick ({ "Cutoff", "Shape", "Pitch" }));

        // ---- Quad ----
        auto waveQ = pick ({ "Saw", "Pulse", "Triangle" });
        qWave1 = addParameter ("q1wave", "Osc 1 Wave", 0.0f, stepped (0, 2), waveQ);
        qPw1 = addParameter ("q1pw", "Osc 1 Width", 50.0f, { 5.0f, 50.0f }, percentText);
        qTune1 = addParameter ("q1tune", "Osc 1 Tune", 0.0f, { -24.0f, 24.0f }, semis);
        qWave2 = addParameter ("q2wave", "Osc 2 Wave", 1.0f, stepped (0, 2), waveQ);
        qPw2 = addParameter ("q2pw", "Osc 2 Width", 35.0f, { 5.0f, 50.0f }, percentText);
        qTune2 = addParameter ("q2tune", "Osc 2 Tune", 0.0f, { -24.0f, 24.0f }, semis);
        qDetune = addParameter ("q2detune", "Osc 2 Detune", 8.0f, { -50.0f, 50.0f }, cents);
        qMix1 = addParameter ("qmix1", "Osc 1 Level", 8.0f, { 0.0f, 10.0f }, tenths);
        qMix2 = addParameter ("qmix2", "Osc 2 Level", 6.0f, { 0.0f, 10.0f }, tenths);
        qSub = addParameter ("qsub", "Sub Level", 4.0f, { 0.0f, 10.0f }, tenths);
        qSubOct = addParameter ("qsuboct", "Sub Octave", 0.0f, stepped (0, 1), pick ({ "-1", "-2" }));
        qNoise = addParameter ("qnoise", "Noise Level", 0.0f, { 0.0f, 10.0f }, tenths);
        qLp = addParameter ("qlp", "Low-Pass", 1500.0f, freqRange (20.0f, 20000.0f), hzText);
        qLpRes = addParameter ("qlpres", "LP Resonance", 3.0f, { 0.0f, 10.0f }, tenths);
        qLpEnv = addParameter ("qenvlp", "LP Env", 4.0f, { -10.0f, 10.0f }, tenths);
        qHp = addParameter ("qhp", "High-Pass", 30.0f, freqRange (20.0f, 4000.0f), hzText);
        qHpRes = addParameter ("qhpres", "HP Resonance", 0.0f, { 0.0f, 10.0f }, tenths);
        qHpEnv = addParameter ("qenvhp", "HP Env", 0.0f, { -10.0f, 10.0f }, tenths);
        qKey = addParameter ("qkey", "Key Track", 2.0f, stepped (0, 3), pick ({ "Off", "1/3", "2/3", "Full" }));
        qDrive = addParameter ("qdrive", "Overdrive", 25.0f, { 0.0f, 100.0f }, percentText);
        qFA = addParameter ("qfa", "Filter Attack", 0.003f, timeRange, secs);
        qFD = addParameter ("qfd", "Filter Decay", 0.4f, timeRange, secs);
        qFS = addParameter ("qfs", "Filter Sustain", 3.0f, { 0.0f, 10.0f }, tenths);
        qFR = addParameter ("qfr", "Filter Release", 0.3f, timeRange, secs);
        qAA = addParameter ("qaa", "Amp Attack", 0.003f, timeRange, secs);
        qAD = addParameter ("qad", "Amp Decay", 0.5f, timeRange, secs);
        qAS = addParameter ("qas", "Amp Sustain", 7.0f, { 0.0f, 10.0f }, tenths);
        qAR = addParameter ("qar", "Amp Release", 0.25f, timeRange, secs);
        qLfoRate = addParameter ("qlforate", "LFO Rate", 4.0f, skewed (0.05f, 30.0f, 3.0f), hz);
        qLfoAmt = addParameter ("qlfoamt", "LFO Depth", 0.0f, { 0.0f, 100.0f }, percentText);
        qLfoDest = addParameter ("qlfodest", "LFO Target", 2.0f, stepped (0, 4), pick ({ "Pitch", "Width", "Low-Pass", "High-Pass", "Amp" }));

        // ---- Velvet ----
        vRange = addParameter ("vrange", "Range", 1.0f, stepped (0, 2), pick ({ "16'", "8'", "4'" }));
        vSaw = addParameter ("vsaw", "Saw", 1.0f, stepped (0, 1), onOffText);
        vPulse = addParameter ("vpulse", "Pulse", 0.0f, stepped (0, 1), onOffText);
        vPw = addParameter ("vpw", "Pulse Width", 40.0f, { 0.0f, 100.0f }, percentText);
        vPwmSrc = addParameter ("vpwmsrc", "PWM Source", 1.0f, stepped (0, 2), pick ({ "Manual", "LFO", "Env" }));
        vSub = addParameter ("vsub", "Sub", 4.0f, { 0.0f, 10.0f }, tenths);
        vNoise = addParameter ("vnoise", "Noise", 0.0f, { 0.0f, 10.0f }, tenths);
        vLfoPitch = addParameter ("vlfopitch", "DCO LFO", 0.0f, { 0.0f, 10.0f }, tenths);
        vHpf = addParameter ("vhpf", "HPF", 1.0f, stepped (0, 3), [] (float v) { return juce::String (juce::roundToInt (v)); });
        vCutoff = addParameter ("vcutoff", "Cutoff", 2200.0f, freqRange (20.0f, 20000.0f), hzText);
        vRes = addParameter ("vres", "Resonance", 1.0f, { 0.0f, 10.0f }, tenths);
        vEnv = addParameter ("venv", "Env", 4.0f, { 0.0f, 10.0f }, tenths);
        vEnvPol = addParameter ("venvpol", "Env Polarity", 0.0f, stepped (0, 1), pick ({ "Normal", "Inverted" }));
        vLfoF = addParameter ("vlfof", "VCF LFO", 0.0f, { 0.0f, 10.0f }, tenths);
        vKey = addParameter ("vkey", "Key Follow", 5.0f, { 0.0f, 10.0f }, tenths);
        vVca = addParameter ("vvca", "VCA Mode", 0.0f, stepped (0, 1), pick ({ "Env", "Gate" }));
        vA = addParameter ("va", "Attack", 0.01f, timeRange, secs);
        vD = addParameter ("vd", "Decay", 0.8f, timeRange, secs);
        vS = addParameter ("vs", "Sustain", 6.0f, { 0.0f, 10.0f }, tenths);
        vR = addParameter ("vr", "Release", 0.6f, timeRange, secs);
        vLfoRate = addParameter ("vlforate", "LFO Rate", 3.0f, skewed (0.1f, 20.0f, 3.0f), hz);
        vLfoDelay = addParameter ("vlfodelay", "LFO Delay", 0.4f, { 0.0f, 3.0f }, secs);

        // ---- Nova ----
        auto waveN = pick ({ "Saw", "Square", "Pulse" });
        auto feetN = pick ({ "16'", "8'", "4'" });
        auto bipolarPct = [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " %"; };
        nWave1 = addParameter ("n1wave", "I Wave", 0.0f, stepped (0, 2), waveN);
        nFeet1 = addParameter ("n1feet", "I Feet", 1.0f, stepped (0, 2), feetN);
        nSine1 = addParameter ("n1sine", "I Sine", 0.0f, { 0.0f, 10.0f }, tenths);
        nWave2 = addParameter ("n2wave", "II Wave", 0.0f, stepped (0, 2), waveN);
        nFeet2 = addParameter ("n2feet", "II Feet", 1.0f, stepped (0, 2), feetN);
        nSine2 = addParameter ("n2sine", "II Sine", 0.0f, { 0.0f, 10.0f }, tenths);
        nDetune = addParameter ("ndetune", "II Detune", 9.0f, { 0.0f, 40.0f }, cents);
        nBalance = addParameter ("nbal", "I/II Balance", 50.0f, { 0.0f, 100.0f }, percentText);
        nPw = addParameter ("npw", "Pulse Width", 30.0f, { 5.0f, 50.0f }, percentText);
        nPwm = addParameter ("npwm", "PWM", 0.0f, { 0.0f, 100.0f }, percentText);
        nHpf = addParameter ("nhpf", "High-Pass", 40.0f, freqRange (20.0f, 3000.0f), hzText);
        nLpf = addParameter ("nlpf", "Low-Pass", 1400.0f, freqRange (60.0f, 18000.0f), hzText);
        nRes = addParameter ("nres", "Resonance", 2.0f, { 0.0f, 10.0f }, tenths);
        nEnv = addParameter ("nenv", "Filter Env", 55.0f, { 0.0f, 100.0f }, percentText);
        nIL = addParameter ("nil", "Initial Level", -30.0f, { -100.0f, 100.0f }, bipolarPct);
        nAL = addParameter ("nal", "Attack Level", 80.0f, { -100.0f, 100.0f }, bipolarPct);
        nFA = addParameter ("nfa", "Filter Attack", 0.3f, timeRange, secs);
        nFD = addParameter ("nfd", "Filter Decay", 1.8f, timeRange, secs);
        nAA = addParameter ("naa", "Amp Attack", 0.08f, timeRange, secs);
        nAD = addParameter ("nad", "Amp Decay", 1.0f, timeRange, secs);
        nAS = addParameter ("nas", "Amp Sustain", 9.0f, { 0.0f, 10.0f }, tenths);
        nAR = addParameter ("nar", "Amp Release", 1.6f, timeRange, secs);
        nInitBright = addParameter ("nibright", "Initial Brilliance", 50.0f, { 0.0f, 100.0f }, percentText);
        nInitLevel = addParameter ("nilevel", "Initial Level Touch", 40.0f, { 0.0f, 100.0f }, percentText);
        nAfterBright = addParameter ("nabright", "After Brilliance", 60.0f, { 0.0f, 100.0f }, percentText);
        nAfterVib = addParameter ("navib", "After Vibrato", 45.0f, { 0.0f, 100.0f }, percentText);
        nAfterPitch = addParameter ("napitch", "After Pitch", 0.0f, { -100.0f, 100.0f }, bipolarPct);
        nVibRate = addParameter ("nvibrate", "Vibrato Rate", 5.6f, skewed (0.5f, 12.0f, 5.0f), hz);
        nVib = addParameter ("nvib", "Vibrato", 0.0f, { 0.0f, 100.0f }, percentText);
    }

    static juce::StringArray ratioNames()   { return { "0.5", "1", "1.5", "2", "3", "4", "5", "7" }; }

    juce::String getName() const override              { return "Elsewhere Poly"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Poly"; }
    int getDeviceViewWidth() const override            { return 1640; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 6.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    double getLatencySeconds() override                 { return oversampler != nullptr ? oversampler->getLatencyInSamples() / rate : 0.0; }

    int getModel() const            { return juce::jlimit (0, numModels - 1, juce::roundToInt (model.get())); }
    int getActiveVoices() const     { return activeVoices.load(); }

    /** Quad is a four-voice instrument; the others go up to the Voices setting. */
    int polyphony() const
    {
        const int v = juce::jlimit (1, maxVoices, juce::roundToInt (voices.get()));
        return getModel() == quad ? juce::jmin (4, v) : v;
    }

    //==============================================================================
    // Factory patches: bank A is Prism, B is Quad, C is Velvet
    struct Patch { int model; const char* name; std::vector<std::pair<const char*, float>> values; };

    static const std::vector<Patch>& patches()
    {
        static const std::vector<Patch> all = {
            { prism, "Prism Pad",       { { "p2pitch", 9 }, { "pcutoff", 1400 }, { "pegint", 25 }, { "pea", 0.8f }, { "ped", 1.5f }, { "pes", 4 },
                                          { "paa", 0.7f }, { "pas", 9 }, { "par", 1.6f }, { "chorus", 1 }, { "plfoint", 10 }, { "plforate", 0.4f } } },
            { prism, "Hybrid Bell",     { { "pmix1", 3 }, { "pmix2", 0 }, { "pmixm", 8 }, { "pmtype", 1 }, { "pmratio", 4 }, { "pmshape", 55 },
                                          { "pcutoff", 9000 }, { "pegint", 0 }, { "paa", 0.002f }, { "pad", 1.4f }, { "pas", 0 }, { "par", 1.6f } } },
            { prism, "Sync Keys",       { { "p2sync", 1 }, { "p2pitch", 700 }, { "pegtarget", 2 }, { "pegint", 45 }, { "ped", 0.6f }, { "pes", 0 },
                                          { "pcutoff", 4000 }, { "pad", 0.8f }, { "pas", 5 } } },
            { prism, "Ring Lead",       { { "vmode", 2 }, { "p2ring", 1 }, { "p2pitch", 500 }, { "pmix1", 4 }, { "pcutoff", 2500 }, { "pres", 4 },
                                          { "glide", 0.06f }, { "plfoint", 8 }, { "plfotarget", 2 }, { "plforate", 5.5f } } },
            { prism, "Unison Saws",     { { "vmode", 1 }, { "voices", 6 }, { "udetune", 35 }, { "pcutoff", 3500 }, { "pres", 1 }, { "pegint", 20 }, { "chorus", 1 } } },
            { prism, "Cross Mod Bass",  { { "vmode", 2 }, { "p1oct", 0 }, { "p2oct", 0 }, { "pxmod", 35 }, { "p2pitch", 0 }, { "pcutoff", 500 }, { "pres", 3 },
                                          { "pegint", 55 }, { "ped", 0.3f }, { "pes", 0 }, { "pdrive", 1 } } },
            { prism, "Chord Stab",      { { "vmode", 3 }, { "chord", 7 }, { "pcutoff", 900 }, { "pres", 3 }, { "pegint", 60 }, { "ped", 0.25f }, { "pes", 0 },
                                          { "pad", 0.4f }, { "pas", 0 }, { "par", 0.3f } } },

            { quad, "Quad Pluck",       { { "qlp", 600 }, { "qlpres", 4 }, { "qenvlp", 7 }, { "qfd", 0.25f }, { "qfs", 0 }, { "qad", 0.6f }, { "qas", 0 }, { "qar", 0.4f } } },
            { quad, "Dual Filter Pad",  { { "qlp", 1200 }, { "qhp", 300 }, { "qhpres", 4 }, { "qenvhp", 3 }, { "qfa", 1.2f }, { "qfd", 2.0f }, { "qfs", 5 },
                                          { "qaa", 0.9f }, { "qas", 9 }, { "qar", 1.5f }, { "qlfoamt", 20 }, { "qlfodest", 3 }, { "qlforate", 0.3f } } },
            { quad, "Overdrive Keys",   { { "qdrive", 75 }, { "qlp", 2200 }, { "qlpres", 2 }, { "qenvlp", 4 }, { "qfd", 0.5f }, { "qas", 6 } } },
            { quad, "Sub Bass",         { { "vmode", 2 }, { "q1wave", 0 }, { "qmix2", 0 }, { "qsub", 8 }, { "qsuboct", 0 }, { "qlp", 380 }, { "qlpres", 2 },
                                          { "qenvlp", 5 }, { "qfd", 0.3f }, { "qfs", 1 }, { "glide", 0.04f } } },
            { quad, "PWM Strings",      { { "q1wave", 1 }, { "q1pw", 30 }, { "q2wave", 1 }, { "qlfoamt", 45 }, { "qlfodest", 1 }, { "qlforate", 0.7f },
                                          { "qlp", 2600 }, { "qaa", 0.5f }, { "qas", 9 }, { "qar", 1.2f }, { "chorus", 1 } } },
            { quad, "High-Pass Sweep",  { { "qhp", 60 }, { "qhpres", 6 }, { "qenvhp", 8 }, { "qfa", 0.002f }, { "qfd", 1.2f }, { "qfs", 0 }, { "qlp", 9000 } } },

            { velvet, "Velvet Strings", { { "vsaw", 1 }, { "vpulse", 1 }, { "vpw", 55 }, { "vpwmsrc", 1 }, { "vlforate", 0.6f }, { "vlfodelay", 0 },
                                          { "vcutoff", 3200 }, { "venv", 2 }, { "va", 0.5f }, { "vs", 9 }, { "vr", 1.1f }, { "chorus", 2 } } },
            { velvet, "Brass 86",       { { "vsaw", 1 }, { "vsub", 3 }, { "vcutoff", 700 }, { "vres", 1.5f }, { "venv", 6 }, { "va", 0.06f }, { "vd", 0.7f },
                                          { "vs", 6 }, { "vr", 0.3f }, { "chorus", 1 } } },
            { velvet, "Hollow Pulse",   { { "vsaw", 0 }, { "vpulse", 1 }, { "vpw", 70 }, { "vpwmsrc", 2 }, { "vcutoff", 1800 }, { "venv", 4 }, { "vd", 1.0f },
                                          { "vs", 4 }, { "chorus", 1 } } },
            { velvet, "Soft Keys",      { { "vsaw", 1 }, { "vcutoff", 1100 }, { "venv", 5 }, { "vd", 1.2f }, { "vs", 0 }, { "vr", 0.8f }, { "vkey", 8 }, { "chorus", 1 } } },
            { velvet, "Velvet Bass",    { { "vmode", 2 }, { "vrange", 0 }, { "vsub", 8 }, { "vcutoff", 420 }, { "vres", 3 }, { "venv", 5 }, { "vd", 0.35f },
                                          { "vs", 2 }, { "vvca", 1 }, { "chorus", 0 } } },
            { velvet, "Lush Pad",       { { "vsaw", 1 }, { "vpulse", 1 }, { "vpwmsrc", 1 }, { "vlforate", 0.35f }, { "vhpf", 2 }, { "vcutoff", 2600 },
                                          { "va", 1.4f }, { "vd", 2.0f }, { "vs", 8 }, { "vr", 2.4f }, { "chorus", 3 } } },

            { nova, "Nova Brass",       { { "model", nova }, { "ndetune", 9 }, { "nhpf", 60 }, { "nlpf", 900 }, { "nres", 2 }, { "nenv", 60 }, { "nil", -40 }, { "nal", 90 },
                                          { "nfa", 0.35f }, { "nfd", 2.0f }, { "naa", 0.12f }, { "nas", 9 }, { "nar", 1.8f }, { "nabright", 70 }, { "navib", 50 },
                                          { "nvibrate", 5.8f }, { "chorus", 2 } } },
            { nova, "Rain Pad",         { { "n2wave", 2 }, { "ndetune", 12 }, { "npw", 25 }, { "npwm", 30 }, { "nlpf", 1400 }, { "nenv", 35 }, { "nil", -20 },
                                          { "nal", 60 }, { "nfa", 1.2f }, { "nfd", 3.0f }, { "naa", 0.9f }, { "nar", 3.5f }, { "n1sine", 3 }, { "chorus", 3 } } },
            { nova, "Spinner Lead",     { { "vmode", 2 }, { "glide", 0.08f }, { "n1wave", 1 }, { "n2feet", 2 }, { "ndetune", 6 }, { "nlpf", 2200 }, { "nres", 4 },
                                          { "nenv", 30 }, { "napitch", 30 }, { "navib", 70 }, { "naa", 0.03f }, { "nar", 0.6f }, { "chorus", 1 } } },
            { nova, "Neon Strings",     { { "n1wave", 2 }, { "n2wave", 2 }, { "npw", 25 }, { "npwm", 40 }, { "nvibrate", 4.5f }, { "nlpf", 3000 }, { "nhpf", 200 },
                                          { "nenv", 20 }, { "naa", 0.5f }, { "nar", 2.0f }, { "chorus", 2 } } },
            { nova, "Memory Bell",      { { "n1sine", 8 }, { "n2sine", 8 }, { "n2feet", 2 }, { "ndetune", 20 }, { "nbal", 40 }, { "nlpf", 1800 }, { "nres", 3 },
                                          { "nenv", 50 }, { "nil", 60 }, { "nal", 100 }, { "nfa", 0.002f }, { "nfd", 1.5f }, { "naa", 0.002f }, { "nad", 2.5f },
                                          { "nas", 0 }, { "nar", 2.5f }, { "chorus", 1 } } },
            { nova, "Night Rain Choir", { { "n1wave", 2 }, { "n1sine", 5 }, { "n2wave", 2 }, { "n2sine", 5 }, { "ndetune", 14 }, { "nlpf", 1200 }, { "nres", 6 },
                                          { "nenv", 25 }, { "nvib", 20 }, { "nvibrate", 5.2f }, { "naa", 0.7f }, { "nar", 3.0f }, { "chorus", 3 } } },
        };
        return all;
    }

    static std::vector<int> patchesFor (int m)
    {
        std::vector<int> result;
        for (int i = 0; i < (int) patches().size(); ++i)
            if (patches()[(size_t) i].model == m)
                result.push_back (i);
        return result;
    }

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

    void loadPatch (int index)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);
        const auto& patch = patches()[(size_t) index];

        edit.getUndoManager().beginNewTransaction ("Load Poly patch");
        for (auto& p : params)
            p->getParameter().setParameter (patchValue (patch, p->getParameter()), juce::sendNotification);

        state.setProperty ("patch", index, &edit.getUndoManager());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", -1); }

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
        // (The engine can do this while audio is still running through the device)
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        rate = info.sampleRate;
        osRate = (float) (rate * 2.0);
        oversampler = std::make_unique<juce::dsp::Oversampling<float>> (2, 1, juce::dsp::Oversampling<float>::filterHalfBandPolyphaseIIR, true, false);
        oversampler->initProcessing ((size_t) maxChunk);
        work.setSize (2, maxChunk);
        work.clear();
        chorusFx.prepare (rate);
        for (auto& v : voice) v = Voice();
        for (size_t i = 0; i < voice.size(); ++i)
            voice[i].drift = { SynthCore::Drift ((int) i * 7 + 3), SynthCore::Drift ((int) i * 11 + 5) };
        stack.clear();
        noteCounter = 0;
        bend = modWheel = lfoPhase = 0.0f;
        pedal = false;
    }

    void deinitialise() override    { const juce::SpinLock::ScopedLockType dspGuard (dspLock); oversampler.reset(); }

    void midiPanic() override
    {
        for (auto& v : voice) { v.held = v.sustained = false; v.amp.gate (false); v.mod.gate (false); v.novaEnv.gate (false); }
        stack.clear();
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked())
            return;     // being set up again for a new rate or buffer size

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

    Param model, voiceMode, voices, detune, chordType, glide, volume, velocity, bendRange, drift, chorus;
    Param pWave1, pShape1, pOct1, pWave2, pShape2, pOct2, pPitch2, pSync, pRing, pXmod, pMultiType, pMultiShape, pMultiRatio;
    Param pMix1, pMix2, pMixM, pCutoff, pRes, pDrive, pPoles, pKey, pEgInt, pEgTarget;
    Param pAA, pAD, pAS, pAR, pEA, pED, pES, pER, pLfoWave, pLfoRate, pLfoInt, pLfoTarget;
    Param qWave1, qPw1, qTune1, qWave2, qPw2, qTune2, qDetune, qMix1, qMix2, qSub, qSubOct, qNoise;
    Param qLp, qLpRes, qLpEnv, qHp, qHpRes, qHpEnv, qKey, qDrive, qFA, qFD, qFS, qFR, qAA, qAD, qAS, qAR, qLfoRate, qLfoAmt, qLfoDest;
    Param vRange, vSaw, vPulse, vPw, vPwmSrc, vSub, vNoise, vLfoPitch, vHpf, vCutoff, vRes, vEnv, vEnvPol, vLfoF, vKey, vVca;
    Param vA, vD, vS, vR, vLfoRate, vLfoDelay;
    Param nWave1, nFeet1, nSine1, nWave2, nFeet2, nSine2, nDetune, nBalance, nPw, nPwm, nHpf, nLpf, nRes, nEnv;
    Param nIL, nAL, nFA, nFD, nAA, nAD, nAS, nAR, nInitBright, nInitLevel, nAfterBright, nAfterVib, nAfterPitch, nVibRate, nVib;

private:
    static constexpr int maxChunk = 256;

    /** Nova's filter envelope, after the 70s flagship: from the Initial Level it moves to the
        Attack Level, then decays to the cutoff itself; on release it returns to the Initial Level. */
    struct NovaEnvelope
    {
        float value = 0.0f;
        bool on = false, attacking = false;

        void gate (bool g)      { if (g && ! on) attacking = true; if (g) restart = true; on = g; }
        bool restart = false;

        float process (float il, float al, float a, float d, float r, float rate) noexcept
        {
            auto coeff = [rate] (float t) { return 1.0f - std::exp (-1.0f / (juce::jmax (0.0005f, t) * rate)); };
            if (restart) { value = il; attacking = true; restart = false; }
            if (on)
            {
                if (attacking)
                {
                    value += (al + (al - il) * 0.3f - value) * coeff (a * 0.6f);   // aims a little past, like an RC curve
                    if ((al >= il && value >= al) || (al < il && value <= al)) { value = al; attacking = false; }
                }
                else
                    value += (0.0f - value) * coeff (d * 0.35f);
            }
            else
                value += (il - value) * coeff (r * 0.35f);
            return value;
        }
    };

    struct Voice
    {
        int note = -1, key = -1;                // key: the key that started it (for chords)
        float vel = 1.0f;
        bool held = false, sustained = false;
        juce::uint32 age = 0;
        float pitch = 60.0f, target = 60.0f, detuneSemis = 0.0f, pan = 0.0f, lfoFade = 0.0f;
        std::array<SynthCore::Oscillator, 3> osc;
        SynthCore::Oscillator sub;
        bool subHigh = true;
        float fmMod = 0.0f, fmCar = 0.0f, fmFb = 0.0f, noiseLp = 0.0f;
        SynthCore::Ladder ladder;
        SynthCore::Svf svf, hp, lp2, hp2;       // Nova: channel I uses hp + svf, channel II hp2 + lp2
        SynthCore::AnalogEnvelope amp, mod;
        NovaEnvelope novaEnv;
        float press = 0.0f, pressSmooth = 0.0f;   // polyphonic aftertouch
        std::array<SynthCore::Drift, 2> drift;
        bool active() const { return amp.isActive(); }
    };

    double rate = 44100.0;
    float osRate = 88200.0f;
    std::unique_ptr<juce::dsp::Oversampling<float>> oversampler;
    juce::AudioBuffer<float> work;
    std::array<Voice, maxVoices> voice;
    SynthCore::NoteStack stack;
    SynthCore::Noise noiseGen;
    SynthCore::StereoChorus chorusFx;
    std::array<float, maxChunk * 2> lfoBuf {};
    juce::uint32 noteCounter = 0;
    float bend = 0.0f, modWheel = 0.0f, lfoPhase = 0.0f, lastPitch = 60.0f;
    bool pedal = false;
    std::atomic<int> activeVoices { 0 };
    std::atomic<float> samplesPerCycle { 200.0f };

    int mode() const    { return juce::jlimit (0, 3, juce::roundToInt (voiceMode.get())); }

    //==============================================================================
    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())                       noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())                 noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())              bend = (float) (m.getPitchWheelValue() - 8192) / 8192.0f;
        else if (m.isControllerOfType (1))      modWheel = (float) m.getControllerValue() / 127.0f;
        else if (m.isAftertouch())
        {
            for (auto& v : voice)
                if (v.key == m.getNoteNumber() && v.held)
                    v.press = (float) m.getAfterTouchValue() / 127.0f;
        }
        else if (m.isChannelPressure())
        {
            for (auto& v : voice)
                if (v.held)
                    v.press = (float) m.getChannelPressureValue() / 127.0f;
        }
        else if (m.isSustainPedalOn())          pedal = true;
        else if (m.isSustainPedalOff())
        {
            pedal = false;
            for (auto& v : voice)
                if (v.sustained && ! v.held) { v.sustained = false; v.amp.gate (false); v.mod.gate (false); v.novaEnv.gate (false); }
        }
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            midiPanic();
    }

    void startVoice (Voice& v, int note, int key, float vel, float fromPitch, float detuneSemis, float pan)
    {
        const bool wasSilent = ! v.active();
        v.note = note; v.key = key; v.vel = vel; v.held = true; v.sustained = false; v.age = ++noteCounter;
        v.target = (float) note;
        v.pitch = glide.get() > 0.0f ? fromPitch : (float) note;
        v.detuneSemis = detuneSemis;
        v.pan = pan;

        if (wasSilent)
        {
            for (auto& o : v.osc) o.reset (noiseGen.white() * 0.5f + 0.5f);
            v.sub.reset();
            v.ladder.reset(); v.svf.reset(); v.hp.reset(); v.lp2.reset(); v.hp2.reset();
            v.press = v.pressSmooth = 0.0f;
            v.fmMod = v.fmCar = v.fmFb = v.noiseLp = 0.0f;
            v.lfoFade = 0.0f;
        }

        v.amp.gate (true);
        v.mod.gate (true); v.novaEnv.gate (true);
    }

    Voice& allocate (int note)
    {
        const int n = polyphony();

        for (int i = 0; i < n; ++i)
            if (voice[(size_t) i].active() && voice[(size_t) i].note == note)
                return voice[(size_t) i];

        for (int i = 0; i < n; ++i)
            if (! voice[(size_t) i].active())
                return voice[(size_t) i];

        // Steal: the oldest released voice, otherwise the oldest voice
        Voice* best = nullptr;
        for (int pass = 0; pass < 2 && best == nullptr; ++pass)
        {
            juce::uint32 oldest = std::numeric_limits<juce::uint32>::max();
            for (int i = 0; i < n; ++i)
            {
                auto& v = voice[(size_t) i];
                if ((pass == 1 || ! v.held) && v.age < oldest) { oldest = v.age; best = &v; }
            }
        }
        return *best;
    }

    /** Mono and Unison: one note at a time, played by one or several voices. */
    void playStack (float vel, bool retrigger)
    {
        const int note = stack.current (SynthCore::NoteStack::Priority::last);
        const bool uni = mode() == unison;
        const int count = uni ? juce::jlimit (2, juce::jmin (8, maxVoices), polyphony()) : 1;
        const float spread = detune.get() / 100.0f * 0.5f;

        for (int i = 0; i < count; ++i)
        {
            auto& v = voice[(size_t) i];
            const float k = count > 1 ? (float) i / (float) (count - 1) * 2.0f - 1.0f : 0.0f;   // -1 .. 1
            if (retrigger || ! v.active())
                startVoice (v, note, note, vel, lastPitch, k * spread, uni ? k * 0.8f : 0.0f);
            else
            {
                v.note = note; v.target = (float) note; v.held = true;
                if (glide.get() <= 0.0f) v.pitch = (float) note;
            }
        }
        lastPitch = (float) note;
    }

    void noteOn (int note, float vel)
    {
        const int m = mode();
        if (m == mono || m == unison)
        {
            const bool wasEmpty = stack.empty();
            stack.press (note);
            playStack (vel, wasEmpty);
            return;
        }

        const auto intervals = m == chord ? chordIntervals (juce::roundToInt (chordType.get())) : std::vector<int> { 0 };
        for (size_t i = 0; i < intervals.size(); ++i)
        {
            const int n = note + intervals[i];
            if (n > 127) continue;
            auto& v = allocate (n);
            const float pan = ((int) (noteCounter % 2) == 0 ? 1.0f : -1.0f) * 0.18f;
            startVoice (v, n, note, vel, lastPitch + (float) intervals[i], 0.0f, pan);
        }
        lastPitch = (float) note;
    }

    void noteOff (int note)
    {
        const int m = mode();
        if (m == mono || m == unison)
        {
            stack.release (note);
            if (! stack.empty())
            {
                playStack (1.0f, false);
                return;
            }
        }

        for (auto& v : voice)
        {
            const bool matches = (m == mono || m == unison) ? v.held : (v.key == note && v.held);
            if (! matches) continue;
            v.held = false;
            if (pedal) v.sustained = true;
            else { v.amp.gate (false); v.mod.gate (false); v.novaEnv.gate (false); }
        }
    }

    static float mtof (float note)      { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }
    static float level (float tenths)   { auto x = juce::jlimit (0.0f, 1.0f, tenths / 10.0f); return x * x * 0.6f + x * 0.4f; }

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

    struct Common
    {
        float sr, bendSemis, driftSemis, velAmt, glideCoeff;
    };

    void renderChunk (juce::AudioBuffer<float>& buf, int start, int n)
    {
        Common c;
        c.sr = osRate;
        c.bendSemis = bend * bendRange.get();
        c.driftSemis = drift.get() / 100.0f * 0.1f;
        c.velAmt = velocity.get() / 100.0f;
        c.glideCoeff = glide.get() <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (glide.get() * 0.35f * c.sr));

        auto inBlock = juce::dsp::AudioBlock<float> (work).getSubBlock (0, (size_t) n);
        auto up = oversampler->processSamplesUp (inBlock);
        float* L = up.getChannelPointer (0);
        float* R = up.getChannelPointer (1);
        const int osN = (int) up.getNumSamples();
        std::fill (L, L + osN, 0.0f);
        std::fill (R, R + osN, 0.0f);

        // One LFO for all voices
        const int currentModel = getModel();
        {
            const float rateHz = currentModel == prism ? pLfoRate.get() : currentModel == quad ? qLfoRate.get()
                               : currentModel == nova ? nVibRate.get() : vLfoRate.get();
            const int shape = currentModel == prism ? juce::roundToInt (pLfoWave.get()) : (currentModel == quad || currentModel == nova) ? 3 : 1;
            const float inc = rateHz / c.sr;
            for (int i = 0; i < osN; ++i)
            {
                lfoPhase += inc;
                if (lfoPhase >= 1.0f) lfoPhase -= 1.0f;
                lfoBuf[(size_t) i] = shape == 0 ? 2.0f * lfoPhase - 1.0f
                                   : shape == 1 ? 4.0f * std::abs (lfoPhase - 0.5f) - 1.0f
                                   : shape == 2 ? (lfoPhase < 0.5f ? 1.0f : -1.0f)
                                                : std::sin (lfoPhase * SynthCore::twoPi);
            }
        }

        int sounding = 0;
        float scopePitch = lastPitch;
        for (int i = 0; i < polyphony(); ++i)
        {
            auto& v = voice[(size_t) i];
            if (! v.active())
                continue;

            ++sounding;
            scopePitch = v.pitch;
            switch (currentModel)
            {
                case quad:      renderQuad (v, L, R, osN, c); break;
                case velvet:    renderVelvet (v, L, R, osN, c); break;
                case nova:      renderNova (v, L, R, osN, c); break;
                default:        renderPrism (v, L, R, osN, c); break;
            }
        }

        // Voices beyond the current polyphony fall silent
        for (int i = polyphony(); i < maxVoices; ++i)
            voice[(size_t) i].amp.reset();

        activeVoices = sounding;

        auto outBlock = juce::dsp::AudioBlock<float> (work).getSubBlock (0, (size_t) n);
        oversampler->processSamplesDown (outBlock);

        float* wl = work.getWritePointer (0);
        float* wr = work.getWritePointer (1);
        chorusFx.process (wl, wr, n, juce::roundToInt (chorus.get()), (float) rate);

        const float outGain = juce::Decibels::decibelsToGain (volume.get(), -60.0f);
        const bool stereo = buf.getNumChannels() > 1;
        float* dl = buf.getWritePointer (0, start);
        float* dr = stereo ? buf.getWritePointer (1, start) : nullptr;
        for (int i = 0; i < n; ++i)
        {
            const float l = std::tanh (wl[i] * outGain * 1.2f) / 1.2f;
            const float r = std::tanh (wr[i] * outGain * 1.2f) / 1.2f;
            if (stereo) { dl[i] = l; dr[i] = r; }
            else dl[i] = (l + r) * 0.5f;
            scope.push ((l + r) * 0.5f);
        }

        work.clear();
        samplesPerCycle = (float) rate / juce::jmax (1.0f, mtof (scopePitch));
    }

    static void addPanned (float* L, float* R, int i, float s, float pan)
    {
        L[i] += s * (1.0f - pan) * 0.7f;
        R[i] += s * (1.0f + pan) * 0.7f;
    }

    //==============================================================================
    /** Prism's analog-style oscillator: Shape morphs saw -> thin pulse, folds the triangle, narrows the square. */
    static float shapedVco (SynthCore::Oscillator& o, int wave, float shape, float dt)
    {
        float y;
        if (wave == 0)          y = o.sawAt (dt) * (1.0f - shape) + o.pulseAt (0.5f - 0.42f * shape, dt) * shape;
        else if (wave == 1)
        {
            const float t = o.triAt (dt);
            y = shape < 0.01f ? t : std::sin (t * (1.0f + 3.0f * shape) * juce::MathConstants<float>::halfPi);
        }
        else                    y = o.pulseAt (0.5f - 0.45f * shape, dt);
        return y;
    }

    void renderPrism (Voice& v, float* L, float* R, int osN, const Common& c)
    {
        const float sr = c.sr;
        static constexpr float octs[] = { -12.0f, 0.0f, 12.0f, 24.0f };
        static constexpr float ratios[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f };
        const int w1 = juce::roundToInt (pWave1.get()), w2 = juce::roundToInt (pWave2.get());
        const float oct1 = octs[juce::jlimit (0, 3, juce::roundToInt (pOct1.get()))];
        const float oct2 = octs[juce::jlimit (0, 3, juce::roundToInt (pOct2.get()))] + pPitch2.get() / 100.0f;
        const float s1 = pShape1.get() / 100.0f, s2 = pShape2.get() / 100.0f;
        const bool sync = pSync.get() > 0.5f, ring = pRing.get() > 0.5f;
        const float xmod = pXmod.get() / 100.0f;
        const bool fm = pMultiType.get() > 0.5f;
        const float mShape = pMultiShape.get() / 100.0f;
        const float ratio = ratios[juce::jlimit (0, 7, juce::roundToInt (pMultiRatio.get()))];
        const float g1 = level (pMix1.get()), g2 = level (pMix2.get()), gm = level (pMixM.get());
        const float baseCutoff = pCutoff.get();
        const float res = std::pow (pRes.get() / 10.0f, 1.3f);
        const float driveGain = 0.55f * (1.0f + std::round (pDrive.get()) * 1.2f);
        const bool fourPole = pPoles.get() > 0.5f;
        const float keyAmt = (float) juce::roundToInt (pKey.get()) * 0.5f;
        const float egInt = pEgInt.get() / 100.0f;
        const int egTarget = juce::roundToInt (pEgTarget.get());
        const float lfoInt = juce::jlimit (-1.0f, 1.0f, pLfoInt.get() / 100.0f + modWheel * (pLfoInt.get() >= 0.0f ? 1.0f : -1.0f));
        const int lfoTarget = juce::roundToInt (pLfoTarget.get());
        const float velGain = 1.0f - c.velAmt * (1.0f - v.vel);
        const float aa = pAA.get(), ad = pAD.get(), as = pAS.get() / 10.0f, ar = pAR.get();
        const float ea = pEA.get(), ed = pED.get(), es = pES.get() / 10.0f, er = pER.get();
        const float noiseLpCoeff = std::exp (-SynthCore::twoPi * (200.0f * std::pow (90.0f, mShape)) / sr);
        v.ladder.poles = 4;

        for (int i = 0; i < osN; ++i)
        {
            v.pitch += (v.target - v.pitch) * c.glideCoeff;
            const float envA = v.amp.process (aa, ad, as, ar, sr);
            const float eg = v.mod.process (ea, ed, es, er, sr);
            const float lfo = lfoBuf[(size_t) i] * lfoInt;

            const float pitchAll = (egTarget == 1 ? eg * egInt * 24.0f : 0.0f) + (lfoTarget == 2 ? lfo * 7.0f : 0.0f);
            const float pitch2 = egTarget == 2 ? eg * egInt * 24.0f : 0.0f;
            const float shapeMod = lfoTarget == 1 ? lfo * 0.5f : 0.0f;
            const float base = v.pitch + v.detuneSemis + c.bendSemis + pitchAll;

            const float f1 = mtof (base + oct1 + v.drift[0].process (c.driftSemis, sr));
            const float dt1 = f1 / sr;
            const float o1 = shapedVco (v.osc[0], w1, juce::jlimit (0.0f, 1.0f, s1 + shapeMod), dt1);
            const bool wrapped = v.osc[0].advance (dt1);

            float dt2 = mtof (base + oct2 + pitch2 + v.drift[1].process (c.driftSemis, sr)) / sr;
            dt2 *= juce::jmax (0.05f, 1.0f + xmod * o1 * 1.5f);
            if (sync && wrapped)
                v.osc[1].phase = v.osc[0].phase * dt2 / juce::jmax (1.0e-7f, dt1);
            float o2 = shapedVco (v.osc[1], w2, juce::jlimit (0.0f, 1.0f, s2 + shapeMod), dt2);
            v.osc[1].advance (dt2);
            if (ring) o2 *= o1;

            float multi = 0.0f;
            if (gm > 0.0f)
            {
                if (fm)
                {
                    // Two-operator FM: a sine carrier, modulated by a sine at Ratio, with a touch of feedback
                    const float fc = mtof (base) / sr;
                    v.fmMod += fc * ratio; if (v.fmMod >= 1.0f) v.fmMod -= 1.0f;
                    v.fmCar += fc;         if (v.fmCar >= 1.0f) v.fmCar -= 1.0f;
                    const float modulator = std::sin ((v.fmMod + v.fmFb * 0.15f * mShape) * SynthCore::twoPi);
                    v.fmFb = modulator;
                    multi = std::sin ((v.fmCar + modulator * mShape * 0.9f) * SynthCore::twoPi);
                }
                else
                {
                    v.noiseLp = noiseLpCoeff * v.noiseLp + (1.0f - noiseLpCoeff) * noiseGen.white();
                    multi = v.noiseLp * (1.0f + mShape);
                }
            }

            const float mixed = (o1 * g1 + o2 * g2 + multi * gm) * driveGain;

            if ((i & 7) == 0)
            {
                const float oct = (egTarget == 0 ? eg * egInt * 5.5f : 0.0f) + (v.pitch - 60.0f) / 12.0f * keyAmt
                                + (lfoTarget == 0 ? lfo * 2.5f : 0.0f) + c.velAmt * v.vel * 1.2f;
                const float fc = baseCutoff * std::pow (2.0f, oct);
                if (fourPole) v.ladder.set (fc, res, sr);
                else          v.svf.set (fc, res * 0.95f, sr);
            }

            const float filtered = fourPole ? v.ladder.process (mixed) : std::tanh (v.svf.lowpass (mixed));
            addPanned (L, R, i, filtered * envA * velGain, v.pan);
        }
    }

    //==============================================================================
    void renderQuad (Voice& v, float* L, float* R, int osN, const Common& c)
    {
        const float sr = c.sr;
        const int w1 = juce::roundToInt (qWave1.get()), w2 = juce::roundToInt (qWave2.get());
        const float pw1 = qPw1.get() / 100.0f, pw2 = qPw2.get() / 100.0f;
        const float t1 = qTune1.get(), t2 = qTune2.get() + qDetune.get() / 100.0f;
        const float g1 = level (qMix1.get()), g2 = level (qMix2.get()), gs = level (qSub.get()), gn = level (qNoise.get()) * 0.7f;
        const float subOct = qSubOct.get() > 0.5f ? -24.0f : -12.0f;
        const float lp = qLp.get(), lpRes = std::pow (qLpRes.get() / 10.0f, 1.3f), lpEnv = qLpEnv.get() / 10.0f * 5.5f;
        const float hpF = qHp.get(), hpRes = qHpRes.get() / 10.0f * 0.9f, hpEnv = qHpEnv.get() / 10.0f * 5.5f;
        static constexpr float keyAmounts[] = { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
        const float keyAmt = keyAmounts[juce::jlimit (0, 3, juce::roundToInt (qKey.get()))];
        const float drive = 1.0f + qDrive.get() / 100.0f * 5.0f;
        const float lfoAmt = juce::jlimit (0.0f, 1.0f, qLfoAmt.get() / 100.0f + modWheel);
        const int lfoDest = juce::roundToInt (qLfoDest.get());
        const float velGain = 1.0f - c.velAmt * (1.0f - v.vel);
        const float fa = qFA.get(), fd = qFD.get(), fs = qFS.get() / 10.0f, fr = qFR.get();
        const float aa = qAA.get(), ad = qAD.get(), as = qAS.get() / 10.0f, ar = qAR.get();
        v.ladder.poles = 4;

        auto wave = [] (SynthCore::Oscillator& o, int w, float pw, float dt)
        {
            const float y = w == 0 ? o.sawAt (dt) : w == 1 ? o.pulseAt (pw, dt) : o.triAt (dt);
            o.advance (dt);
            return y;
        };

        for (int i = 0; i < osN; ++i)
        {
            v.pitch += (v.target - v.pitch) * c.glideCoeff;
            const float envF = v.mod.process (fa, fd, fs, fr, sr);
            const float envA = v.amp.process (aa, ad, as, ar, sr);
            const float lfo = lfoBuf[(size_t) i] * lfoAmt;

            const float base = v.pitch + v.detuneSemis + c.bendSemis + (lfoDest == 0 ? lfo * 1.0f : 0.0f);
            const float pwMod = lfoDest == 1 ? lfo * 0.4f : 0.0f;
            const float o1 = wave (v.osc[0], w1, juce::jlimit (0.05f, 0.95f, pw1 + pwMod), mtof (base + t1 + v.drift[0].process (c.driftSemis, sr)) / sr);
            const float o2 = wave (v.osc[1], w2, juce::jlimit (0.05f, 0.95f, pw2 + pwMod), mtof (base + t2 + v.drift[1].process (c.driftSemis, sr)) / sr);
            const float sub = wave (v.sub, 1, 0.5f, mtof (base + t1 + subOct) / sr);
            const float mixed = o1 * g1 + o2 * g2 + sub * gs + noiseGen.white() * gn;

            if ((i & 7) == 0)
            {
                const float keyOct = (v.pitch - 60.0f) / 12.0f * keyAmt;
                v.ladder.set (lp * std::pow (2.0f, lpEnv * envF + keyOct + (lfoDest == 2 ? lfo * 2.5f : 0.0f) + c.velAmt * v.vel), lpRes, sr);
                v.hp.set (hpF * std::pow (2.0f, hpEnv * envF + (lfoDest == 3 ? lfo * 2.5f : 0.0f)), hpRes, sr);
            }

            float s = v.ladder.process (mixed * 0.55f);
            s = v.hp.highpass (s);
            s = std::tanh (s * drive) / std::sqrt (drive);
            const float ampMod = lfoDest == 4 ? 1.0f - (lfo * 0.5f + 0.5f) * lfoAmt : 1.0f;
            addPanned (L, R, i, s * envA * velGain * ampMod * 1.3f, v.pan);
        }
    }

    //==============================================================================
    void renderVelvet (Voice& v, float* L, float* R, int osN, const Common& c)
    {
        const float sr = c.sr;
        static constexpr float ranges[] = { -12.0f, 0.0f, 12.0f };
        const float range = ranges[juce::jlimit (0, 2, juce::roundToInt (vRange.get()))];
        const bool sawOn = vSaw.get() > 0.5f, pulseOn = vPulse.get() > 0.5f;
        const float pwAmt = vPw.get() / 100.0f;
        const int pwmSrc = juce::roundToInt (vPwmSrc.get());
        const float gSub = level (vSub.get()) * 0.8f, gNoise = level (vNoise.get()) * 0.5f;
        const float lfoPitch = vLfoPitch.get() / 10.0f * 1.5f;
        const int hpf = juce::roundToInt (vHpf.get());
        const float cutoff = vCutoff.get(), res = std::pow (vRes.get() / 10.0f, 1.3f);
        const float envOct = vEnv.get() / 10.0f * 5.5f * (vEnvPol.get() > 0.5f ? -1.0f : 1.0f);
        const float lfoF = vLfoF.get() / 10.0f * 2.5f;
        const float keyAmt = vKey.get() / 10.0f;
        const bool gateVca = vVca.get() > 0.5f;
        const float a = vA.get(), d = vD.get(), s = vS.get() / 10.0f, r = vR.get();
        const float fadeInc = vLfoDelay.get() <= 0.0f ? 1.0f : 1.0f / (vLfoDelay.get() * sr);
        const float lfoGain = juce::jlimit (0.0f, 1.0f, 1.0f + modWheel);
        const float velGain = 1.0f - c.velAmt * (1.0f - v.vel);
        v.ladder.poles = 4;

        if (hpf >= 2)
            v.hp.set (hpf == 2 ? 240.0f : 720.0f, 0.1f, sr);
        else if (hpf == 0)
            v.hp.set (120.0f, 0.1f, sr);

        for (int i = 0; i < osN; ++i)
        {
            v.pitch += (v.target - v.pitch) * c.glideCoeff;
            v.lfoFade = juce::jmin (1.0f, v.lfoFade + fadeInc);
            const float lfo = lfoBuf[(size_t) i] * v.lfoFade * lfoGain;

            const float env = v.mod.process (a, d, s, r, sr);
            const float amp = gateVca ? v.amp.process (0.002f, 0.01f, 1.0f, 0.01f, sr) : v.amp.process (a, d, s, r, sr);

            // One phase gives saw, pulse and the sub, like a DCO
            const float dt = mtof (v.pitch + v.detuneSemis + range + c.bendSemis + lfo * lfoPitch + v.drift[0].process (c.driftSemis * 0.3f, sr)) / sr;
            const float pwDepth = pwmSrc == 0 ? pwAmt : pwmSrc == 1 ? pwAmt * (lfo * 0.5f + 0.5f) : pwAmt * env;
            const float width = 0.5f - 0.45f * pwDepth;
            const float saw = sawOn ? v.osc[0].sawAt (dt) : 0.0f;
            const float pulse = pulseOn ? v.osc[0].pulseAt (width, dt) : 0.0f;
            if (v.osc[0].advance (dt))
                v.subHigh = ! v.subHigh;
            const float sub = v.subHigh ? 1.0f : -1.0f;
            float mixed = saw * 0.75f + pulse * 0.75f + sub * gSub + noiseGen.white() * gNoise;

            // HPF: 0 lifts the bass a little, 1 is flat, 2 and 3 cut more and more
            if (hpf == 0)       mixed += v.hp.lowpass (mixed) * 0.6f;
            else if (hpf >= 2)  mixed = v.hp.highpass (mixed);

            if ((i & 7) == 0)
                v.ladder.set (cutoff * std::pow (2.0f, envOct * env + lfo * lfoF + (v.pitch - 60.0f) / 12.0f * keyAmt + c.velAmt * v.vel), res, sr);

            addPanned (L, R, i, v.ladder.process (mixed * 0.55f) * amp * velGain * 1.2f, v.pan);
        }

        // Gate mode: the amp follows the key; keep the filter envelope's release in step
        if (gateVca && ! v.amp.isActive())
            v.mod.reset();
    }

    //==============================================================================
    /** Nova: two detuned channels, each oscillator -> high-pass -> resonant low-pass; touch everywhere. */
    void renderNova (Voice& v, float* L, float* R, int osN, const Common& c)
    {
        const float sr = c.sr;
        static constexpr float feet[] = { -12.0f, 0.0f, 12.0f };
        const int w1 = juce::roundToInt (nWave1.get()), w2 = juce::roundToInt (nWave2.get());
        const float oct1 = feet[juce::jlimit (0, 2, juce::roundToInt (nFeet1.get()))];
        const float oct2 = feet[juce::jlimit (0, 2, juce::roundToInt (nFeet2.get()))] + nDetune.get() / 100.0f;
        const float sine1 = level (nSine1.get()), sine2 = level (nSine2.get());
        const float bal = nBalance.get() / 100.0f;
        const float g1 = std::cos (bal * juce::MathConstants<float>::halfPi) * 1.2f, g2 = std::sin (bal * juce::MathConstants<float>::halfPi) * 1.2f;
        const float pw = nPw.get() / 100.0f, pwm = nPwm.get() / 100.0f;
        const float res = std::pow (nRes.get() / 10.0f, 1.2f) * 0.92f;
        const float envOct = nEnv.get() / 100.0f * 6.0f;
        const float il = nIL.get() / 100.0f, al = nAL.get() / 100.0f;
        const float fa = nFA.get(), fd = nFD.get();
        const float aa = nAA.get(), ad = nAD.get(), as = nAS.get() / 10.0f, ar = nAR.get();
        const float vel = v.vel;
        const float initBrightOct = nInitBright.get() / 100.0f * 2.5f * (vel - 0.6f);
        const float levelTouch = 1.0f - nInitLevel.get() / 100.0f * (1.0f - vel);
        const float afterBright = nAfterBright.get() / 100.0f * 2.5f;
        const float afterVib = nAfterVib.get() / 100.0f, baseVib = juce::jlimit (0.0f, 1.0f, nVib.get() / 100.0f + modWheel);
        const float afterPitch = nAfterPitch.get() / 100.0f;        // up to a semitone of growl
        const float pressCoeff = 1.0f - std::exp (-1.0f / (0.06f * sr));
        const float hpCut = nHpf.get(), lpCut = nLpf.get();

        auto wave = [] (SynthCore::Oscillator& o, int w, float width, float dt)
        {
            return w == 0 ? o.sawAt (dt) : w == 1 ? o.pulseAt (0.5f, dt) : o.pulseAt (width, dt);
        };

        for (int i = 0; i < osN; ++i)
        {
            v.pitch += (v.target - v.pitch) * c.glideCoeff;
            v.pressSmooth += (v.press - v.pressSmooth) * pressCoeff;
            const float p = v.pressSmooth;

            // Vibrato from the sub-oscillator, deeper as you press; aftertouch can bend the pitch too
            const float vib = lfoBuf[(size_t) i] * (baseVib * 0.25f + afterVib * p * 0.27f);
            const float base = v.pitch + v.detuneSemis + c.bendSemis + vib + afterPitch * p;
            const float dt1 = mtof (base + oct1 + v.drift[0].process (c.driftSemis, sr)) / sr;
            const float dt2 = mtof (base + oct2 + v.drift[1].process (c.driftSemis, sr)) / sr;
            const float width = juce::jlimit (0.05f, 0.5f, pw + pwm * 0.4f * lfoBuf[(size_t) i]);

            float ch1 = wave (v.osc[0], w1, width, dt1) + sine1 * std::sin (v.osc[0].phase * SynthCore::twoPi);
            float ch2 = wave (v.osc[1], w2, width, dt2) + sine2 * std::sin (v.osc[1].phase * SynthCore::twoPi);
            v.osc[0].advance (dt1);
            v.osc[1].advance (dt2);

            const float fenv = v.novaEnv.process (il, al, fa, fd, ar, sr);
            const float amp = v.amp.process (aa, ad, as, ar, sr);

            if ((i & 7) == 0)
            {
                const float keyOct = (v.pitch - 60.0f) / 12.0f * 0.5f;
                const float lp = lpCut * std::pow (2.0f, envOct * fenv + initBrightOct + afterBright * p + keyOct);
                const float hp = hpCut * std::pow (2.0f, keyOct);
                v.svf.set (lp, res, sr); v.lp2.set (lp, res, sr);
                v.hp.set (hp, 0.05f, sr); v.hp2.set (hp, 0.05f, sr);
            }

            ch1 = v.svf.lowpass (v.hp.highpass (ch1));
            ch2 = v.lp2.lowpass (v.hp2.highpass (ch2));

            const float gain = amp * levelTouch * (1.0f + 0.25f * p) * 0.5f;
            // Channel I a little left, channel II a little right: the two channels spread the sound
            addPanned (L, R, i, ch1 * g1 * gain, v.pan - 0.22f);
            addPanned (L, R, i, ch2 * g2 * gain, v.pan + 0.22f);
        }
    }
};

//==============================================================================
using PolyPanelBase = SynthPanel::PanelBase;

/** Prism: two VCOs, the Multi engine, mixer, filter, EG, amp and LFO. */
class PrismPanel : public PolyPanelBase
{
public:
    PrismPanel (PolyDevice& d)
    {
        using W = SynthCore::Wave;
        const auto cOsc = Colours::accent, cMulti = Colours::clips[6], cMix = Colours::clips[5], cFilt = Colours::solo, cEg = Colours::clips[2], cLfo = Colours::clips[6];
        const juce::StringArray octs { "16'", "8'", "4'", "2'" };
        const std::vector<W> w { W::saw, W::triangle, W::square };

        wave1 = waves (d.pWave1, w, cOsc); oct1 = selector (d.pOct1, octs, cOsc); shape1 = knob (d.pShape1, "Shape", cOsc);
        wave2 = waves (d.pWave2, w, cOsc); oct2 = selector (d.pOct2, octs, cOsc);
        vco2 = { knob (d.pPitch2, "Pitch", cOsc), knob (d.pShape2, "Shape", cOsc), knob (d.pXmod, "X-Mod", cOsc) };
        sync = toggle (d.pSync, "Sync", cOsc); ring = toggle (d.pRing, "Ring", cOsc);

        multiType = selector (d.pMultiType, { "Noise", "FM" }, cMulti);
        multi = { knob (d.pMultiShape, "Shape", cMulti), knob (d.pMultiRatio, "Ratio", cMulti) };

        bars = { bar (d.pMix1, cMix), bar (d.pMix2, cMix), bar (d.pMixM, cMix) };

        filt = { knob (d.pCutoff, "Cutoff", cFilt), knob (d.pRes, "Resonance", cFilt), knob (d.pEgInt, "EG Int", cFilt), knob (d.pKey, "Key", cFilt) };
        poles = selector (d.pPoles, { "2-Pole", "4-Pole" }, cFilt);
        drive = selector (d.pDrive, { "0", "50", "100" }, cFilt);

        egTarget = selector (d.pEgTarget, { "Cut", "Pitch", "P2" }, cEg);
        eg = { knob (d.pEA, "A", cEg), knob (d.pED, "D", cEg), knob (d.pES, "S", cEg), knob (d.pER, "R", cEg) };
        amp = { knob (d.pAA, "A", cEg), knob (d.pAD, "D", cEg), knob (d.pAS, "S", cEg), knob (d.pAR, "R", cEg) };

        lfoWave = waves (d.pLfoWave, w, cLfo);
        lfoTarget = selector (d.pLfoTarget, { "Cut", "Shape", "Pitch" }, cLfo);
        lfo = { knob (d.pLfoRate, "Rate", cLfo), knob (d.pLfoInt, "Int", cLfo) };
    }

    void paint (juce::Graphics& g) override
    {
        const juce::StringArray titles { "VCO 1", "VCO 2", "Multi", "Mixer", "Filter", "EG", "Amp", "LFO" };
        const std::array<juce::Colour, 8> colours { Colours::accent, Colours::accent, Colours::clips[6], Colours::clips[5],
                                                    Colours::solo, Colours::clips[2], Colours::clips[2], Colours::clips[6] };
        for (int i = 0; i < 8; ++i)
            SynthPanel::paintSection (g, areas[(size_t) i], titles[i], colours[(size_t) i]);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        const juce::StringArray mixNames { "VCO 1", "VCO 2", "Multi" };
        for (int i = 0; i < 3; ++i)
            g.drawText (mixNames[i], mixRows[(size_t) i].withWidth (40), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 120, 190, 120, 130, 200, 150, 130, 130 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        {
            auto c = content (areas[0]);
            wave1->setBounds (c.removeFromTop (20)); c.removeFromTop (3);
            oct1->setBounds (c.removeFromTop (18)); c.removeFromTop (2);
            shape1->setBounds (c);
        }
        {
            auto c = content (areas[1]);
            auto top = c.removeFromTop (20);
            ring->setBounds (top.removeFromRight (44)); sync->setBounds (top.removeFromRight (46)); top.removeFromRight (4);
            wave2->setBounds (top); c.removeFromTop (3);
            oct2->setBounds (c.removeFromTop (18)); c.removeFromTop (2);
            row (c, { vco2[0], vco2[1], vco2[2] }, 3);
        }
        {
            auto c = content (areas[2]);
            multiType->setBounds (c.removeFromTop (20)); c.removeFromTop (3);
            row (c, { multi[0], multi[1] }, 2);
        }
        {
            auto c = content (areas[3]);
            const int rowH = c.getHeight() / 3;
            for (int i = 0; i < 3; ++i)
            {
                auto r = c.removeFromTop (rowH);
                mixRows[(size_t) i] = r;
                bars[(size_t) i]->setBounds (r.withTrimmedLeft (42));
            }
        }
        {
            auto c = content (areas[4]);
            auto bottom = c.removeFromBottom (18);
            row (c, { filt[0], filt[1], filt[2], filt[3] }, 4);
            poles->setBounds (bottom.removeFromLeft (bottom.getWidth() / 2 - 3));
            bottom.removeFromLeft (6);
            drive->setBounds (bottom);
        }
        {
            auto c = content (areas[5]);
            egTarget->setBounds (c.removeFromTop (18)); c.removeFromTop (3);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { eg[0], eg[1] }, 2);
            row (c, { eg[2], eg[3] }, 2);
        }
        {
            auto c = content (areas[6]);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { amp[0], amp[1] }, 2);
            row (c, { amp[2], amp[3] }, 2);
        }
        {
            auto c = content (areas[7]);
            lfoWave->setBounds (c.removeFromTop (20)); c.removeFromTop (3);
            lfoTarget->setBounds (c.removeFromBottom (18)); c.removeFromBottom (2);
            row (c, { lfo[0], lfo[1] }, 2);
        }
    }

private:
    std::array<juce::Rectangle<int>, 8> areas;
    std::array<juce::Rectangle<int>, 3> mixRows;
    SynthPanel::Selector *wave1 = nullptr, *oct1 = nullptr, *wave2 = nullptr, *oct2 = nullptr, *multiType = nullptr, *poles = nullptr, *drive = nullptr,
                         *egTarget = nullptr, *lfoWave = nullptr, *lfoTarget = nullptr;
    SynthPanel::Knob* shape1 = nullptr;
    SynthPanel::Toggle *sync = nullptr, *ring = nullptr;
    std::vector<SynthPanel::Knob*> vco2, multi, filt, eg, amp, lfo;
    std::vector<SynthPanel::BarSlider*> bars;
};

//==============================================================================
/** Quad: two oscillators, mixer (with sub and noise), two filters, overdrive, envelopes, LFO. */
class QuadPanel : public PolyPanelBase
{
public:
    QuadPanel (PolyDevice& d)
    {
        using W = SynthCore::Wave;
        const auto cOsc = Colours::accent, cMix = Colours::clips[5], cFilt = Colours::solo, cEnv = Colours::clips[2], cLfo = Colours::clips[6];
        const std::vector<W> w { W::saw, W::square, W::triangle };

        wave1 = waves (d.qWave1, w, cOsc); osc1 = { knob (d.qPw1, "Width", cOsc), knob (d.qTune1, "Tune", cOsc) };
        wave2 = waves (d.qWave2, w, cOsc); osc2 = { knob (d.qPw2, "Width", cOsc), knob (d.qTune2, "Tune", cOsc), knob (d.qDetune, "Detune", cOsc) };

        bars = { bar (d.qMix1, cMix), bar (d.qMix2, cMix), bar (d.qSub, cMix), bar (d.qNoise, cMix) };
        subOct = selector (d.qSubOct, { "-1", "-2" }, cMix);

        lp = { knob (d.qLp, "Low-Pass", cFilt), knob (d.qLpRes, "Res", cFilt), knob (d.qLpEnv, "Env", cFilt), knob (d.qKey, "Key", cFilt) };
        hp = { knob (d.qHp, "High-Pass", cFilt), knob (d.qHpRes, "Res", cFilt), knob (d.qHpEnv, "Env", cFilt), knob (d.qDrive, "Drive", Colours::record) };

        fenv = { knob (d.qFA, "A", cEnv), knob (d.qFD, "D", cEnv), knob (d.qFS, "S", cEnv), knob (d.qFR, "R", cEnv) };
        aenv = { knob (d.qAA, "A", cEnv), knob (d.qAD, "D", cEnv), knob (d.qAS, "S", cEnv), knob (d.qAR, "R", cEnv) };

        lfo = { knob (d.qLfoRate, "Rate", cLfo), knob (d.qLfoAmt, "Depth", cLfo) };
        lfoDest = selector (d.qLfoDest, { "Pit", "PW", "LP", "HP", "Amp" }, cLfo);
    }

    void paint (juce::Graphics& g) override
    {
        const juce::StringArray titles { "Osc 1", "Osc 2", "Mixer", "Filters", "Filter Env", "Amp Env", "LFO" };
        const std::array<juce::Colour, 7> colours { Colours::accent, Colours::accent, Colours::clips[5], Colours::solo,
                                                    Colours::clips[2], Colours::clips[2], Colours::clips[6] };
        for (int i = 0; i < 7; ++i)
            SynthPanel::paintSection (g, areas[(size_t) i], titles[i], colours[(size_t) i]);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        const juce::StringArray mixNames { "Osc 1", "Osc 2", "Sub", "Noise" };
        for (int i = 0; i < 4; ++i)
            g.drawText (mixNames[i], mixRows[(size_t) i].withWidth (40), juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 130, 180, 160, 280, 150, 150, 130 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        { auto c = content (areas[0]); wave1->setBounds (c.removeFromTop (20)); c.removeFromTop (4); row (c, { osc1[0], osc1[1] }, 2); }
        { auto c = content (areas[1]); wave2->setBounds (c.removeFromTop (20)); c.removeFromTop (4); row (c, { osc2[0], osc2[1], osc2[2] }, 3); }
        {
            auto c = content (areas[2]);
            const int rowH = c.getHeight() / 4;
            for (int i = 0; i < 4; ++i)
            {
                auto r = c.removeFromTop (rowH);
                mixRows[(size_t) i] = r;
                r.removeFromLeft (40);
                if (i == 2) { subOct->setBounds (r.removeFromRight (40).withSizeKeepingCentre (40, juce::jmin (16, rowH - 4))); r.removeFromRight (4); }
                bars[(size_t) i]->setBounds (r);
            }
        }
        {
            auto c = content (areas[3]);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { lp[0], lp[1], lp[2], lp[3] }, 4);
            row (c, { hp[0], hp[1], hp[2], hp[3] }, 4);
        }
        { auto c = content (areas[4]); auto t = c.removeFromTop (c.getHeight() / 2); row (t, { fenv[0], fenv[1] }, 2); row (c, { fenv[2], fenv[3] }, 2); }
        { auto c = content (areas[5]); auto t = c.removeFromTop (c.getHeight() / 2); row (t, { aenv[0], aenv[1] }, 2); row (c, { aenv[2], aenv[3] }, 2); }
        { auto c = content (areas[6]); lfoDest->setBounds (c.removeFromBottom (18)); c.removeFromBottom (3); row (c, { lfo[0], lfo[1] }, 2); }
    }

private:
    std::array<juce::Rectangle<int>, 7> areas;
    std::array<juce::Rectangle<int>, 4> mixRows;
    SynthPanel::Selector *wave1 = nullptr, *wave2 = nullptr, *subOct = nullptr, *lfoDest = nullptr;
    std::vector<SynthPanel::Knob*> osc1, osc2, lp, hp, fenv, aenv, lfo;
    std::vector<SynthPanel::BarSlider*> bars;
};

//==============================================================================
/** Velvet: DCO, HPF, VCF, VCA, envelope and LFO, laid out like the classic panel. */
class VelvetPanel : public PolyPanelBase
{
public:
    VelvetPanel (PolyDevice& d)
    {
        const auto cLfo = Colours::clips[6], cDco = Colours::accent, cHpf = Colours::clips[5], cVcf = Colours::solo, cEnv = Colours::clips[2];

        lfo = { knob (d.vLfoRate, "Rate", cLfo), knob (d.vLfoDelay, "Delay", cLfo) };

        range = selector (d.vRange, { "16'", "8'", "4'" }, cDco);
        sawOn = toggle (d.vSaw, "Saw", cDco); pulseOn = toggle (d.vPulse, "Pulse", cDco);
        pwmSrc = selector (d.vPwmSrc, { "Man", "LFO", "Env" }, cDco);
        dco = { knob (d.vLfoPitch, "LFO", cDco), knob (d.vPw, "PWM", cDco), knob (d.vSub, "Sub", cDco), knob (d.vNoise, "Noise", cDco) };

        hpf = selector (d.vHpf, { "0", "1", "2", "3" }, cHpf);

        vcf = { knob (d.vCutoff, "Freq", cVcf), knob (d.vRes, "Res", cVcf), knob (d.vEnv, "Env", cVcf), knob (d.vLfoF, "LFO", cVcf), knob (d.vKey, "Kybd", cVcf) };
        envPol = selector (d.vEnvPol, { "+", "-" }, cVcf);

        vca = selector (d.vVca, { "Env", "Gate" }, cEnv);
        env = { knob (d.vA, "A", cEnv), knob (d.vD, "D", cEnv), knob (d.vS, "S", cEnv), knob (d.vR, "R", cEnv) };
    }

    void paint (juce::Graphics& g) override
    {
        const juce::StringArray titles { "LFO", "DCO", "HPF", "VCF", "VCA", "ENV" };
        const std::array<juce::Colour, 6> colours { Colours::clips[6], Colours::accent, Colours::clips[5], Colours::solo, Colours::clips[2], Colours::clips[2] };
        for (int i = 0; i < 6; ++i)
            SynthPanel::paintSection (g, areas[(size_t) i], titles[i], colours[(size_t) i]);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 110, 300, 80, 290, 90, 220 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        { auto c = content (areas[0]); auto t = c.removeFromTop (c.getHeight() / 2); lfo[0]->setBounds (t); lfo[1]->setBounds (c); }
        {
            auto c = content (areas[1]);
            auto top = c.removeFromTop (20);
            range->setBounds (top.removeFromLeft (90)); top.removeFromLeft (6);
            sawOn->setBounds (top.removeFromLeft (52)); pulseOn->setBounds (top.removeFromLeft (60)); top.removeFromLeft (4);
            pwmSrc->setBounds (top);
            c.removeFromTop (4);
            row (c, { dco[0], dco[1], dco[2], dco[3] }, 4);
        }
        { auto c = content (areas[2]); hpf->setBounds (c.withSizeKeepingCentre (c.getWidth(), 20)); }
        {
            auto c = content (areas[3]);
            envPol->setBounds (areas[3].withHeight (16).removeFromRight (44).reduced (0, 1).translated (-6, 0));
            row (c, { vcf[0], vcf[1], vcf[2], vcf[3], vcf[4] }, 5);
        }
        { auto c = content (areas[4]); vca->setBounds (c.withSizeKeepingCentre (c.getWidth(), 20)); }
        { auto c = content (areas[5]); row (c, { env[0], env[1], env[2], env[3] }, 4); }

        for (auto* k : { vcf[0], vcf[1], vcf[2], vcf[3], vcf[4], env[0], env[1], env[2], env[3], dco[0], dco[1], dco[2], dco[3] })
            k->maxSize = 44;
    }

private:
    std::array<juce::Rectangle<int>, 6> areas;
    SynthPanel::Selector *range = nullptr, *pwmSrc = nullptr, *hpf = nullptr, *envPol = nullptr, *vca = nullptr;
    SynthPanel::Toggle *sawOn = nullptr, *pulseOn = nullptr;
    std::vector<SynthPanel::Knob*> lfo, dco, vcf, env;
};


//==============================================================================
/** Nova: two channels, the filter and its envelope, the amp, and touch (velocity and aftertouch). */
class NovaPanel : public PolyPanelBase
{
public:
    NovaPanel (PolyDevice& d)
    {
        const auto cCh = Colours::accent, cFilt = Colours::solo, cEg = Colours::clips[2], cTouch = Colours::clips[6];
        const juce::StringArray waveNames { "Saw", "Sq", "Pulse" }, feetNames { "16'", "8'", "4'" };

        wave1 = selector (d.nWave1, waveNames, cCh); feet1 = selector (d.nFeet1, feetNames, cCh);
        wave2 = selector (d.nWave2, waveNames, cCh); feet2 = selector (d.nFeet2, feetNames, cCh);
        ch1 = { knob (d.nSine1, "Sine", cCh), knob (d.nBalance, "I / II", cCh) };
        ch2 = { knob (d.nSine2, "Sine", cCh), knob (d.nDetune, "Detune", cCh) };
        pulse = { knob (d.nPw, "Width", cCh), knob (d.nPwm, "PWM", cCh) };
        filt = { knob (d.nHpf, "High-Pass", cFilt), knob (d.nLpf, "Low-Pass", cFilt), knob (d.nRes, "Res", cFilt), knob (d.nEnv, "Env", cFilt) };
        feg = { knob (d.nIL, "Initial", cEg), knob (d.nAL, "Attack Lvl", cEg), knob (d.nFA, "Attack", cEg), knob (d.nFD, "Decay", cEg) };
        amp = { knob (d.nAA, "A", cEg), knob (d.nAD, "D", cEg), knob (d.nAS, "S", cEg), knob (d.nAR, "R", cEg) };
        touch = { knob (d.nInitBright, "Brilliance", cTouch), knob (d.nInitLevel, "Level", cTouch), knob (d.nVib, "Vibrato", cTouch),
                  knob (d.nAfterBright, "Brilliance", cTouch), knob (d.nAfterVib, "Vibrato", cTouch), knob (d.nAfterPitch, "Pitch", cTouch) };
    }

    void paint (juce::Graphics& g) override
    {
        const juce::StringArray titles { "Channel I", "Channel II", "Pulse", "Filter", "Filter EG", "Amp", "Touch" };
        const std::array<juce::Colour, 7> colours { Colours::accent, Colours::accent, Colours::accent, Colours::solo, Colours::clips[2], Colours::clips[2], Colours::clips[6] };
        for (int i = 0; i < 7; ++i)
            SynthPanel::paintSection (g, areas[(size_t) i], titles[i], colours[(size_t) i]);

        // Touch: velocity on the top row, aftertouch below
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        g.drawText ("INITIAL", touchLabels[0], juce::Justification::centredLeft);
        g.drawText ("AFTER", touchLabels[1], juce::Justification::centredLeft);
    }

    void resized() override
    {
        auto cols = columns (getLocalBounds(), { 132, 132, 96, 196, 176, 176, 216 });
        for (size_t i = 0; i < cols.size(); ++i) areas[i] = cols[i];

        auto channel = [] (juce::Rectangle<int> c, SynthPanel::Selector* w, SynthPanel::Selector* f, std::vector<SynthPanel::Knob*>& k)
        {
            w->setBounds (c.removeFromTop (18)); c.removeFromTop (3);
            f->setBounds (c.removeFromTop (18)); c.removeFromTop (3);
            row (c, { k[0], k[1] }, 2);
        };
        channel (content (areas[0]), wave1, feet1, ch1);
        channel (content (areas[1]), wave2, feet2, ch2);
        { auto c = content (areas[2]); auto t = c.removeFromTop (c.getHeight() / 2); pulse[0]->setBounds (t); pulse[1]->setBounds (c); }
        { auto c = content (areas[3]); auto t = c.removeFromTop (c.getHeight() / 2); row (t, { filt[0], filt[1] }, 2); row (c, { filt[2], filt[3] }, 2); }
        { auto c = content (areas[4]); auto t = c.removeFromTop (c.getHeight() / 2); row (t, { feg[0], feg[1] }, 2); row (c, { feg[2], feg[3] }, 2); }
        { auto c = content (areas[5]); auto t = c.removeFromTop (c.getHeight() / 2); row (t, { amp[0], amp[1] }, 2); row (c, { amp[2], amp[3] }, 2); }
        {
            auto c = content (areas[6]);
            auto top = c.removeFromTop (c.getHeight() / 2);
            touchLabels[0] = top.removeFromTop (10); touchLabels[1] = c.removeFromTop (10);
            row (top, { touch[0], touch[1], touch[2] }, 3);
            row (c, { touch[3], touch[4], touch[5] }, 3);
        }
        for (auto* k : { pulse[0], pulse[1] }) k->maxSize = 44;
    }

private:
    std::array<juce::Rectangle<int>, 7> areas;
    std::array<juce::Rectangle<int>, 2> touchLabels;
    SynthPanel::Selector *wave1 = nullptr, *feet1 = nullptr, *wave2 = nullptr, *feet2 = nullptr;
    std::vector<SynthPanel::Knob*> ch1, ch2, pulse, filt, feg, amp, touch;
};

//==============================================================================
/** The Poly: model choice and display, the model's panel, then voice and output controls. */
class PolyView : public PolyPanelBase, private juce::Timer
{
public:
    PolyView (PolyDevice& d) : polyDev (d)
    {
        modelSel = selector (d.model, PolyDevice::modelNames(), Colours::accent);

        oled.readScope = [this] (float* dst, int n) { polyDev.scope.read (dst, n); };
        oled.getSamplesPerCycle = [this] { return polyDev.getSamplesPerCycle(); };
        addAndMakeVisible (oled);
        onTouch = [this] (te::AutomatableParameter& p) { oled.showParameter (p); };

        for (auto* b : { &prevButton, &nextButton, &patchButton })
            addAndMakeVisible (b);
        prevButton.onClick = [this] { step (-1); };
        prevButton.setTitle ("Previous Patch");
        nextButton.onClick = [this] { step (1); };
        nextButton.setTitle ("Next Patch");
        patchButton.onClick = [this] { showPatchMenu(); };

        const auto cV = Colours::clips[5], cOut = Colours::accent;
        modeSel = selector (d.voiceMode, PolyDevice::voiceModeNames(), cV);
        voiceKnobs = { knob (d.voices, "Voices", cV), knob (d.detune, "Detune", cV), knob (d.chordType, "Chord", cV), knob (d.glide, "Glide", cV) };
        outKnobs = { knob (d.volume, "Volume", cOut), knob (d.velocity, "Velocity", cOut), knob (d.drift, "Drift", cOut), knob (d.bendRange, "Bend", cOut) };
        chorusSel = selector (d.chorus, PolyDevice::chorusNames(), Colours::solo);

        rebuildBody();
        updatePatchName();
        startTimerHz (5);
    }

    void paint (juce::Graphics& g) override
    {
        SynthPanel::paintSection (g, voiceArea, "Voice", Colours::clips[5]);
        SynthPanel::paintSection (g, outArea, "Output", Colours::accent);

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText ("CHORUS", chorusLabel, juce::Justification::centredLeft);
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
        prevButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        nextButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        patchButton.setBounds (nav);
        r.removeFromLeft (gap);

        outArea = r.removeFromRight (150);
        r.removeFromRight (gap);
        voiceArea = r.removeFromRight (150);
        r.removeFromRight (gap);
        if (body != nullptr) body->setBounds (r);
        sections = { voiceArea, outArea };

        {
            auto c = content (voiceArea);
            modeSel->setBounds (c.removeFromTop (18)); c.removeFromTop (3);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { voiceKnobs[0], voiceKnobs[1] }, 2);
            row (c, { voiceKnobs[2], voiceKnobs[3] }, 2);
        }
        {
            auto c = content (outArea);
            auto chorusRow = c.removeFromBottom (18);
            chorusLabel = chorusRow.removeFromLeft (46);
            chorusSel->setBounds (chorusRow);
            c.removeFromBottom (3);
            auto top = c.removeFromTop (c.getHeight() / 2);
            row (top, { outKnobs[0], outKnobs[1] }, 2);
            row (c, { outKnobs[2], outKnobs[3] }, 2);
        }
    }

private:
    PolyDevice& polyDev;
    SynthPanel::Oled oled;
    juce::TextButton prevButton { juce::String::fromUTF8 ("\xe2\x97\x80") }, nextButton { juce::String::fromUTF8 ("\xe2\x96\xb6") }, patchButton { "Patches" };
    std::unique_ptr<PolyPanelBase> body;
    int bodyModel = -1;
    SynthPanel::Selector *modelSel = nullptr, *modeSel = nullptr, *chorusSel = nullptr;
    std::vector<SynthPanel::Knob*> voiceKnobs, outKnobs;
    juce::Rectangle<int> voiceArea, outArea, chorusLabel;

    void rebuildBody()
    {
        bodyModel = polyDev.getModel();
        body.reset();
        switch (bodyModel)
        {
            case PolyDevice::quad:      body = std::make_unique<QuadPanel> (polyDev); break;
            case PolyDevice::velvet:    body = std::make_unique<VelvetPanel> (polyDev); break;
            case PolyDevice::nova:      body = std::make_unique<NovaPanel> (polyDev); break;
            default:                    body = std::make_unique<PrismPanel> (polyDev); break;
        }
        body->onTouch = [this] (te::AutomatableParameter& p) { oled.showParameter (p); };
        addAndMakeVisible (*body);
        oled.header = PolyDevice::modelNames()[bodyModel].toUpperCase() + "  /  POLY";
        resized();
    }

    void step (int delta)
    {
        auto list = PolyDevice::patchesFor (polyDev.getModel());
        if (list.empty()) return;
        auto it = std::find (list.begin(), list.end(), polyDev.getPatchIndex());
        int pos = it == list.end() ? (delta > 0 ? -1 : 0) : (int) (it - list.begin());
        pos = (pos + delta + (int) list.size()) % (int) list.size();
        polyDev.loadPatch (list[(size_t) pos]);
        updatePatchName();
    }

    void showPatchMenu()
    {
        juce::PopupMenu m;
        for (int model = 0; model < PolyDevice::numModels; ++model)
        {
            juce::PopupMenu sub;
            for (auto i : PolyDevice::patchesFor (model))
                sub.addItem (i + 1, PolyDevice::patchLabel (i), true, i == polyDev.getPatchIndex());
            m.addSubMenu (PolyDevice::modelNames()[model], sub, true, nullptr, model == polyDev.getModel());
        }

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&patchButton), [safe = juce::Component::SafePointer<PolyView> (this)] (int result)
        {
            if (safe != nullptr && result > 0)
            {
                safe->polyDev.loadPatch (result - 1);
                safe->updatePatchName();
            }
        });
    }

    void updatePatchName()
    {
        const int index = polyDev.getPatchIndex();
        auto name = PolyDevice::patchLabel (index);
        if (index >= 0 && polyDev.isPatchModified())
            name << " *";
        oled.patchName = name;
    }

    void timerCallback() override
    {
        if (polyDev.getModel() != bodyModel)
            rebuildBody();
        updatePatchName();
    }
};

inline std::unique_ptr<juce::Component> PolyDevice::createDeviceView()     { return std::make_unique<PolyView> (*this); }

} // namespace elsewhere
