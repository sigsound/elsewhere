// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthPanel.h"
#include "PolyEngine.h"

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
    bend, and the chorus. Voices run at twice the sample rate (oversampled). The sound
    itself is PolyEngine; the device feeds it parameters and MIDI.
*/
class PolyDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewherePoly";
    static constexpr int maxVoices = PolyEngine::maxVoices;

    using Model = PolyEngine::Model;
    using VoiceMode = PolyEngine::VoiceMode;
    static constexpr int prism = PolyEngine::prism, quad = PolyEngine::quad, velvet = PolyEngine::velvet, nova = PolyEngine::nova,
                         numModels = PolyEngine::numModels;
    static constexpr int poly = PolyEngine::poly, unison = PolyEngine::unison, mono = PolyEngine::mono, chord = PolyEngine::chord;

    /** A menu control's choices, as the engine names them. */
    static juce::StringArray names (const char* id)
    {
        juce::StringArray result;
        for (auto* c : PolyEngine::choicesOf (id))
            result.add (juce::String::fromUTF8 (c));
        return result;
    }

    static juce::StringArray modelNames()       { return names ("model"); }
    static juce::StringArray voiceModeNames()   { return names ("vmode"); }
    static juce::StringArray chordNames()       { return names ("chord"); }
    static juce::StringArray chorusNames()      { return names ("chorus"); }
    static juce::StringArray ratioNames()       { return names ("pmratio"); }

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
        const PolyEngine::Params d;     // the defaults

        // ---- Shared ----
        model     = addParameter ("model", "Model", d.model, stepped (0, numModels - 1), pick (modelNames()));
        voiceMode = addParameter ("vmode", "Voice Mode", d.voiceMode, stepped (0, 3), pick (voiceModeNames()));
        voices    = addParameter ("voices", "Voices", d.voices, stepped (1, maxVoices), [] (float v) { return juce::String (juce::roundToInt (v)); });
        detune    = addParameter ("udetune", "Unison Detune", d.detune, { 0.0f, 100.0f }, percentText);
        chordType = addParameter ("chord", "Chord", d.chordType, stepped (0, 9), pick (chordNames()));
        glide     = addParameter ("glide", "Glide", d.glide, skewed (0.0f, 3.0f, 0.25f), secs);
        volume    = addParameter ("volume", "Volume", d.volume, { -60.0f, 6.0f }, dbText);
        velocity  = addParameter ("vel", "Velocity", d.velocity, { 0.0f, 100.0f }, percentText);
        bendRange = addParameter ("bend", "Bend Range", d.bendRange, stepped (1, 12), [] (float v) { return juce::String (juce::roundToInt (v)) + " st"; });
        drift     = addParameter ("drift", "Drift", d.drift, { 0.0f, 100.0f }, percentText);
        chorus    = addParameter ("chorus", "Chorus", d.chorus, stepped (0, 3), pick (chorusNames()));

        // ---- Prism ----
        auto waveP = pick (names ("p1wave"));
        pWave1 = addParameter ("p1wave", "VCO 1 Wave", d.pWave1, stepped (0, 2), waveP);
        pShape1 = addParameter ("p1shape", "VCO 1 Shape", d.pShape1, { 0.0f, 100.0f }, percentText);
        pOct1 = addParameter ("p1oct", "VCO 1 Octave", d.pOct1, stepped (0, 3), pick (octNames));
        pWave2 = addParameter ("p2wave", "VCO 2 Wave", d.pWave2, stepped (0, 2), waveP);
        pShape2 = addParameter ("p2shape", "VCO 2 Shape", d.pShape2, { 0.0f, 100.0f }, percentText);
        pOct2 = addParameter ("p2oct", "VCO 2 Octave", d.pOct2, stepped (0, 3), pick (octNames));
        pPitch2 = addParameter ("p2pitch", "VCO 2 Pitch", d.pPitch2, { -1200.0f, 1200.0f }, cents);
        pSync = addParameter ("p2sync", "Sync", d.pSync, stepped (0, 1), onOffText);
        pRing = addParameter ("p2ring", "Ring", d.pRing, stepped (0, 1), onOffText);
        pXmod = addParameter ("pxmod", "Cross Mod", d.pXmod, { 0.0f, 100.0f }, percentText);
        pMultiType = addParameter ("pmtype", "Multi Type", d.pMultiType, stepped (0, 1), pick ({ "Noise", "FM" }));
        pMultiShape = addParameter ("pmshape", "Multi Shape", d.pMultiShape, { 0.0f, 100.0f }, percentText);
        pMultiRatio = addParameter ("pmratio", "FM Ratio", d.pMultiRatio, stepped (0, 7), pick (ratioNames()));
        pMix1 = addParameter ("pmix1", "VCO 1 Level", d.pMix1, { 0.0f, 10.0f }, tenths);
        pMix2 = addParameter ("pmix2", "VCO 2 Level", d.pMix2, { 0.0f, 10.0f }, tenths);
        pMixM = addParameter ("pmixm", "Multi Level", d.pMixM, { 0.0f, 10.0f }, tenths);
        pCutoff = addParameter ("pcutoff", "Cutoff", d.pCutoff, freqRange (20.0f, 20000.0f), hzText);
        pRes = addParameter ("pres", "Resonance", d.pRes, { 0.0f, 10.0f }, tenths);
        pDrive = addParameter ("pdrive", "Drive", d.pDrive, stepped (0, 2), pick ({ "0%", "50%", "100%" }));
        pPoles = addParameter ("ppoles", "Filter Type", d.pPoles, stepped (0, 1), pick ({ "2-Pole", "4-Pole" }));
        pKey = addParameter ("pkey", "Key Track", d.pKey, stepped (0, 2), pick ({ "0%", "50%", "100%" }));
        pEgInt = addParameter ("pegint", "EG Int", d.pEgInt, { -100.0f, 100.0f }, signedPct);
        pEgTarget = addParameter ("pegtarget", "EG Target", d.pEgTarget, stepped (0, 2), pick ({ "Cutoff", "Pitch", "Pitch 2" }));
        pAA = addParameter ("paa", "Amp Attack", d.pAA, timeRange, secs);
        pAD = addParameter ("pad", "Amp Decay", d.pAD, timeRange, secs);
        pAS = addParameter ("pas", "Amp Sustain", d.pAS, { 0.0f, 10.0f }, tenths);
        pAR = addParameter ("par", "Amp Release", d.pAR, timeRange, secs);
        pEA = addParameter ("pea", "EG Attack", d.pEA, timeRange, secs);
        pED = addParameter ("ped", "EG Decay", d.pED, timeRange, secs);
        pES = addParameter ("pes", "EG Sustain", d.pES, { 0.0f, 10.0f }, tenths);
        pER = addParameter ("per", "EG Release", d.pER, timeRange, secs);
        pLfoWave = addParameter ("plfowave", "LFO Wave", d.pLfoWave, stepped (0, 2), pick ({ "Saw", "Triangle", "Square" }));
        pLfoRate = addParameter ("plforate", "LFO Rate", d.pLfoRate, skewed (0.05f, 30.0f, 3.0f), hz);
        pLfoInt = addParameter ("plfoint", "LFO Int", d.pLfoInt, { -100.0f, 100.0f }, signedPct);
        pLfoTarget = addParameter ("plfotarget", "LFO Target", d.pLfoTarget, stepped (0, 2), pick ({ "Cutoff", "Shape", "Pitch" }));

        // ---- Quad ----
        auto waveQ = pick ({ "Saw", "Pulse", "Triangle" });
        qWave1 = addParameter ("q1wave", "Osc 1 Wave", d.qWave1, stepped (0, 2), waveQ);
        qPw1 = addParameter ("q1pw", "Osc 1 Width", d.qPw1, { 5.0f, 50.0f }, percentText);
        qTune1 = addParameter ("q1tune", "Osc 1 Tune", d.qTune1, { -24.0f, 24.0f }, semis);
        qWave2 = addParameter ("q2wave", "Osc 2 Wave", d.qWave2, stepped (0, 2), waveQ);
        qPw2 = addParameter ("q2pw", "Osc 2 Width", d.qPw2, { 5.0f, 50.0f }, percentText);
        qTune2 = addParameter ("q2tune", "Osc 2 Tune", d.qTune2, { -24.0f, 24.0f }, semis);
        qDetune = addParameter ("q2detune", "Osc 2 Detune", d.qDetune, { -50.0f, 50.0f }, cents);
        qMix1 = addParameter ("qmix1", "Osc 1 Level", d.qMix1, { 0.0f, 10.0f }, tenths);
        qMix2 = addParameter ("qmix2", "Osc 2 Level", d.qMix2, { 0.0f, 10.0f }, tenths);
        qSub = addParameter ("qsub", "Sub Level", d.qSub, { 0.0f, 10.0f }, tenths);
        qSubOct = addParameter ("qsuboct", "Sub Octave", d.qSubOct, stepped (0, 1), pick ({ "-1", "-2" }));
        qNoise = addParameter ("qnoise", "Noise Level", d.qNoise, { 0.0f, 10.0f }, tenths);
        qLp = addParameter ("qlp", "Low-Pass", d.qLp, freqRange (20.0f, 20000.0f), hzText);
        qLpRes = addParameter ("qlpres", "LP Resonance", d.qLpRes, { 0.0f, 10.0f }, tenths);
        qLpEnv = addParameter ("qenvlp", "LP Env", d.qLpEnv, { -10.0f, 10.0f }, tenths);
        qHp = addParameter ("qhp", "High-Pass", d.qHp, freqRange (20.0f, 4000.0f), hzText);
        qHpRes = addParameter ("qhpres", "HP Resonance", d.qHpRes, { 0.0f, 10.0f }, tenths);
        qHpEnv = addParameter ("qenvhp", "HP Env", d.qHpEnv, { -10.0f, 10.0f }, tenths);
        qKey = addParameter ("qkey", "Key Track", d.qKey, stepped (0, 3), pick ({ "Off", "1/3", "2/3", "Full" }));
        qDrive = addParameter ("qdrive", "Overdrive", d.qDrive, { 0.0f, 100.0f }, percentText);
        qFA = addParameter ("qfa", "Filter Attack", d.qFA, timeRange, secs);
        qFD = addParameter ("qfd", "Filter Decay", d.qFD, timeRange, secs);
        qFS = addParameter ("qfs", "Filter Sustain", d.qFS, { 0.0f, 10.0f }, tenths);
        qFR = addParameter ("qfr", "Filter Release", d.qFR, timeRange, secs);
        qAA = addParameter ("qaa", "Amp Attack", d.qAA, timeRange, secs);
        qAD = addParameter ("qad", "Amp Decay", d.qAD, timeRange, secs);
        qAS = addParameter ("qas", "Amp Sustain", d.qAS, { 0.0f, 10.0f }, tenths);
        qAR = addParameter ("qar", "Amp Release", d.qAR, timeRange, secs);
        qLfoRate = addParameter ("qlforate", "LFO Rate", d.qLfoRate, skewed (0.05f, 30.0f, 3.0f), hz);
        qLfoAmt = addParameter ("qlfoamt", "LFO Depth", d.qLfoAmt, { 0.0f, 100.0f }, percentText);
        qLfoDest = addParameter ("qlfodest", "LFO Target", d.qLfoDest, stepped (0, 4), pick ({ "Pitch", "Width", "Low-Pass", "High-Pass", "Amp" }));

        // ---- Velvet ----
        vRange = addParameter ("vrange", "Range", d.vRange, stepped (0, 2), pick ({ "16'", "8'", "4'" }));
        vSaw = addParameter ("vsaw", "Saw", d.vSaw, stepped (0, 1), onOffText);
        vPulse = addParameter ("vpulse", "Pulse", d.vPulse, stepped (0, 1), onOffText);
        vPw = addParameter ("vpw", "Pulse Width", d.vPw, { 0.0f, 100.0f }, percentText);
        vPwmSrc = addParameter ("vpwmsrc", "PWM Source", d.vPwmSrc, stepped (0, 2), pick ({ "Manual", "LFO", "Env" }));
        vSub = addParameter ("vsub", "Sub", d.vSub, { 0.0f, 10.0f }, tenths);
        vNoise = addParameter ("vnoise", "Noise", d.vNoise, { 0.0f, 10.0f }, tenths);
        vLfoPitch = addParameter ("vlfopitch", "DCO LFO", d.vLfoPitch, { 0.0f, 10.0f }, tenths);
        vHpf = addParameter ("vhpf", "HPF", d.vHpf, stepped (0, 3), [] (float v) { return juce::String (juce::roundToInt (v)); });
        vCutoff = addParameter ("vcutoff", "Cutoff", d.vCutoff, freqRange (20.0f, 20000.0f), hzText);
        vRes = addParameter ("vres", "Resonance", d.vRes, { 0.0f, 10.0f }, tenths);
        vEnv = addParameter ("venv", "Env", d.vEnv, { 0.0f, 10.0f }, tenths);
        vEnvPol = addParameter ("venvpol", "Env Polarity", d.vEnvPol, stepped (0, 1), pick ({ "Normal", "Inverted" }));
        vLfoF = addParameter ("vlfof", "VCF LFO", d.vLfoF, { 0.0f, 10.0f }, tenths);
        vKey = addParameter ("vkey", "Key Follow", d.vKey, { 0.0f, 10.0f }, tenths);
        vVca = addParameter ("vvca", "VCA Mode", d.vVca, stepped (0, 1), pick ({ "Env", "Gate" }));
        vA = addParameter ("va", "Attack", d.vA, timeRange, secs);
        vD = addParameter ("vd", "Decay", d.vD, timeRange, secs);
        vS = addParameter ("vs", "Sustain", d.vS, { 0.0f, 10.0f }, tenths);
        vR = addParameter ("vr", "Release", d.vR, timeRange, secs);
        vLfoRate = addParameter ("vlforate", "LFO Rate", d.vLfoRate, skewed (0.1f, 20.0f, 3.0f), hz);
        vLfoDelay = addParameter ("vlfodelay", "LFO Delay", d.vLfoDelay, { 0.0f, 3.0f }, secs);

        // ---- Nova ----
        auto waveN = pick ({ "Saw", "Square", "Pulse" });
        auto feetN = pick ({ "16'", "8'", "4'" });
        auto bipolarPct = [] (float v) { return (v > 0.0f ? "+" : "") + juce::String (juce::roundToInt (v)) + " %"; };
        nWave1 = addParameter ("n1wave", "I Wave", d.nWave1, stepped (0, 2), waveN);
        nFeet1 = addParameter ("n1feet", "I Feet", d.nFeet1, stepped (0, 2), feetN);
        nSine1 = addParameter ("n1sine", "I Sine", d.nSine1, { 0.0f, 10.0f }, tenths);
        nWave2 = addParameter ("n2wave", "II Wave", d.nWave2, stepped (0, 2), waveN);
        nFeet2 = addParameter ("n2feet", "II Feet", d.nFeet2, stepped (0, 2), feetN);
        nSine2 = addParameter ("n2sine", "II Sine", d.nSine2, { 0.0f, 10.0f }, tenths);
        nDetune = addParameter ("ndetune", "II Detune", d.nDetune, { 0.0f, 40.0f }, cents);
        nBalance = addParameter ("nbal", "I/II Balance", d.nBalance, { 0.0f, 100.0f }, percentText);
        nPw = addParameter ("npw", "Pulse Width", d.nPw, { 5.0f, 50.0f }, percentText);
        nPwm = addParameter ("npwm", "PWM", d.nPwm, { 0.0f, 100.0f }, percentText);
        nHpf = addParameter ("nhpf", "High-Pass", d.nHpf, freqRange (20.0f, 3000.0f), hzText);
        nLpf = addParameter ("nlpf", "Low-Pass", d.nLpf, freqRange (60.0f, 18000.0f), hzText);
        nRes = addParameter ("nres", "Resonance", d.nRes, { 0.0f, 10.0f }, tenths);
        nEnv = addParameter ("nenv", "Filter Env", d.nEnv, { 0.0f, 100.0f }, percentText);
        nIL = addParameter ("nil", "Initial Level", d.nIL, { -100.0f, 100.0f }, bipolarPct);
        nAL = addParameter ("nal", "Attack Level", d.nAL, { -100.0f, 100.0f }, bipolarPct);
        nFA = addParameter ("nfa", "Filter Attack", d.nFA, timeRange, secs);
        nFD = addParameter ("nfd", "Filter Decay", d.nFD, timeRange, secs);
        nAA = addParameter ("naa", "Amp Attack", d.nAA, timeRange, secs);
        nAD = addParameter ("nad", "Amp Decay", d.nAD, timeRange, secs);
        nAS = addParameter ("nas", "Amp Sustain", d.nAS, { 0.0f, 10.0f }, tenths);
        nAR = addParameter ("nar", "Amp Release", d.nAR, timeRange, secs);
        nInitBright = addParameter ("nibright", "Initial Brilliance", d.nInitBright, { 0.0f, 100.0f }, percentText);
        nInitLevel = addParameter ("nilevel", "Initial Level Touch", d.nInitLevel, { 0.0f, 100.0f }, percentText);
        nAfterBright = addParameter ("nabright", "After Brilliance", d.nAfterBright, { 0.0f, 100.0f }, percentText);
        nAfterVib = addParameter ("navib", "After Vibrato", d.nAfterVib, { 0.0f, 100.0f }, percentText);
        nAfterPitch = addParameter ("napitch", "After Pitch", d.nAfterPitch, { -100.0f, 100.0f }, bipolarPct);
        nVibRate = addParameter ("nvibrate", "Vibrato Rate", d.nVibRate, skewed (0.5f, 12.0f, 5.0f), hz);
        nVib = addParameter ("nvib", "Vibrato", d.nVib, { 0.0f, 100.0f }, percentText);

        // Each parameter and the engine control it feeds
        for (auto& p : params)
        {
            auto* target = engine.params.find (p->getParameter().paramID.toRawUTF8());
            jassert (target != nullptr);
            if (target != nullptr)
                feeds.push_back ({ p.get(), target });
        }
    }

    juce::String getName() const override              { return "Elsewhere Poly"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Poly"; }
    int getDeviceViewWidth() const override            { return 1640; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    std::unique_ptr<juce::Component> createWindowView() override;
    juce::Point<int> getWindowViewSize() const override    { return { 1260, 660 }; }

    bool isSynth() override                             { return true; }
    bool takesMidiInput() override                      { return true; }
    bool takesAudioInput() override                     { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }
    bool noTail() override                              { return false; }
    double getTailLength() const override               { return 6.0; }
    int getNumOutputChannelsGivenInputs (int) override  { return 2; }
    double getLatencySeconds() override                 { return engine.latencySamples() / rate; }

    int getModel() const            { return juce::jlimit (0, numModels - 1, juce::roundToInt (model.get())); }
    int getActiveVoices() const     { return engine.activeVoices.load(); }

    //==============================================================================
    // Factory patches (PolyEngine keeps them, so anything playing the engine has the same ones):
    // bank A is Prism, B Quad, C Velvet, D Nova
    using Patch = PolyEngine::Patch;
    static const std::vector<Patch>& patches()          { return PolyEngine::patches(); }

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

    float patchValue (int index, te::AutomatableParameter& ap) const
    {
        return PolyEngine::patchValue (index, ap.paramID.toRawUTF8());
    }

    /** Sets every control to a patch: as its own undo step, or as part of the one in progress. */
    void loadPatch (int index, bool ownUndoStep = true)
    {
        index = juce::jlimit (0, (int) patches().size() - 1, index);

        if (ownUndoStep)
            edit.getUndoManager().beginNewTransaction ("Load Poly patch");
        for (auto& p : params)
            p->getParameter().setParameter (patchValue (index, p->getParameter()), juce::sendNotification);

        state.setProperty ("patch", index, &edit.getUndoManager());
    }

    int getPatchIndex() const       { return (int) state.getProperty ("patch", -1); }

    bool isPatchModified() const
    {
        const int index = getPatchIndex();
        if (index < 0 || index >= (int) patches().size())
            return true;

        for (auto& p : params)
        {
            auto& ap = p->getParameter();
            const float v = patchValue (index, ap);
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
        engine.prepare (rate, info.blockSizeSamples);
    }

    void deinitialise() override {}

    void midiPanic() override       { engine.allNotesOff(); }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked())
            return;     // being set up again for a new rate or buffer size

        if (fc.destBuffer == nullptr || ! engine.isPrepared())
            return;

        juce::ScopedNoDenormals noDenormals;
        auto& buf = *fc.destBuffer;
        buf.clear (fc.bufferStartSample, fc.bufferNumSamples);
        updateEngine();

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

    /** The sound itself. */
    PolyEngine engine;

    // For the screen
    SynthCore::ScopeBuffer& scope = engine.scope;
    float getSamplesPerCycle() const    { return engine.samplesPerCycle.load(); }

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
    double rate = 48000.0;
    std::vector<std::pair<te::ParameterWithStateValue*, float*>> feeds;
    std::array<float, PolyEngine::maxChunk> monoL {}, monoR {};     // for a mono buffer

    /** The parameters as they are now (automation included) into the engine. */
    void updateEngine()
    {
        for (auto& [param, value] : feeds)
            *value = param->getCurrentValue();
    }

    void handleMidi (const juce::MidiMessage& m)
    {
        if (m.isNoteOn())                       engine.noteOn (m.getNoteNumber(), m.getFloatVelocity());
        else if (m.isNoteOff())                 engine.noteOff (m.getNoteNumber());
        else if (m.isPitchWheel())              engine.setPitchBend ((float) (m.getPitchWheelValue() - 8192) / 8192.0f);
        else if (m.isControllerOfType (1))      engine.setModWheel ((float) m.getControllerValue() / 127.0f);
        else if (m.isAftertouch())              engine.setAftertouch (m.getNoteNumber(), (float) m.getAfterTouchValue() / 127.0f);
        else if (m.isChannelPressure())         engine.setChannelPressure ((float) m.getChannelPressureValue() / 127.0f);
        else if (m.isSustainPedalOn())          engine.setSustainPedal (true);
        else if (m.isSustainPedalOff())         engine.setSustainPedal (false);
        else if (m.isAllNotesOff() || m.isAllSoundOff())
            midiPanic();
    }

    void render (juce::AudioBuffer<float>& buf, int start, int num)
    {
        auto* L = buf.getWritePointer (0, start);

        if (buf.getNumChannels() > 1)
            return engine.render (L, buf.getWritePointer (1, start), num);

        // A mono buffer: both sides, folded together
        for (int done = 0; done < num; )
        {
            const int n = juce::jmin ((int) monoL.size(), num - done);
            std::fill (monoL.begin(), monoL.begin() + n, 0.0f);
            std::fill (monoR.begin(), monoR.begin() + n, 0.0f);
            engine.render (monoL.data(), monoR.data(), n);
            for (int i = 0; i < n; ++i)
                L[done + i] += (monoL[(size_t) i] + monoR[(size_t) i]) * 0.5f;
            done += n;
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
    /** `inWindow`: the layout for the device's own window (a big display, the sections in two rows). */
    PolyView (PolyDevice& d, bool inWindow = false) : polyDev (d), window (inWindow)
    {
        if (window) setWindowRows (2);
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
        const int gap = window ? 8 : 6;

        auto display = r.removeFromLeft (window ? juce::jmin (320, r.getWidth() * 32 / 100) : 200);
        modelSel->setBounds (display.removeFromTop (window ? 22 : 20));
        display.removeFromTop (window ? 6 : 4);
        if (window)
        {
            // Voice and output sit under the display, leaving the rest of the window to the model's panel
            auto under = display.removeFromBottom (192);
            display.removeFromBottom (gap);
            voiceArea = under.removeFromLeft ((under.getWidth() - gap) / 2);
            under.removeFromLeft (gap);
            outArea = under;
        }
        auto nav = display.removeFromBottom (24);
        display.removeFromBottom (window ? 6 : 4);
        oled.setBounds (display);
        prevButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        nextButton.setBounds (nav.removeFromLeft (30)); nav.removeFromLeft (4);
        patchButton.setBounds (nav);
        r.removeFromLeft (gap);

        if (! window)
        {
            outArea = r.removeFromRight (150);
            r.removeFromRight (gap);
            voiceArea = r.removeFromRight (150);
            r.removeFromRight (gap);
        }
        // A narrow window: a third row, so the sections keep their width
        if (window && body != nullptr) body->setWindowRows (r.getWidth() < 540 ? 3 : 2);
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
    const bool window;
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
        if (window) body->setWindowRows (2);
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
inline std::unique_ptr<juce::Component> PolyDevice::createWindowView()     { return std::make_unique<SynthPanel::Zoomed> (std::make_unique<PolyView> (*this, true), 1.25f, getWindowViewSize()); }

} // namespace elsewhere
