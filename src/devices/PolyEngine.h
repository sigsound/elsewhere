// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SynthCore.h"
#include "Downsampler.h"
#include "EngineControl.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace elsewhere
{
//==============================================================================
/**
    The sound of Elsewhere Poly, on its own: four models of voice (Prism, Quad, Velvet and
    Nova), the voice modes, glide, drift and the chorus, in plain C++ with nothing of JUCE, so
    it builds anywhere. The Poly device plays through it, and so can anything else that wants
    to sound the same.

    Prism: two oscillators with a Shape control (saw morphs towards a thin pulse, triangle
    folds, square narrows), sync, ring and cross mod; a Multi source (filtered noise, or
    two-operator FM); a 2- or 4-pole filter with drive; amp and mod envelopes; an LFO.

    Quad: two oscillators with pulse width, a sub oscillator and noise; a 24 dB ladder
    low-pass into a 12 dB high-pass, each with its own envelope amount; per-voice overdrive;
    an LFO with five destinations. Four voices.

    Velvet: one oscillator giving saw, pulse (with PWM from the LFO or envelope), a sub and
    noise from one phase; a 4-step high-pass; a warm 24 dB low-pass; one envelope; an LFO
    that fades in after a delay.

    Nova: two channels per voice, detuned against each other, each an oscillator with a sine
    into a high-pass and a resonant low-pass. The filter envelope starts at an Initial Level
    and moves to an Attack Level before it decays, and touch shapes every note: velocity sets
    brightness and level, aftertouch adds brightness, vibrato and a pitch bend.

    Voices run at twice the sample rate and are brought back down by a half-band filter.

    Call prepare() first, then noteOn and noteOff as notes come, and render() for each block.
    None of them allocates or locks. Everything that varies (oscillator start phases, drift,
    noise) starts from fixed seeds on every prepare(), so the same notes always render the
    same audio.
*/
class PolyEngine
{
public:
    static constexpr int maxVoices = 16;
    static constexpr int maxChunk = 256;

    enum Model { prism, quad, velvet, nova, numModels };
    enum VoiceMode { poly, unison, mono, chord };

    /** Every control, in the units the device shows (percent, tenths, seconds, Hz, cents, dB). */
    struct Params
    {
        // ---- Shared ----
        float model = prism, voiceMode = poly, voices = 8.0f, detune = 25.0f, chordType = 3.0f, glide = 0.0f;
        float volume = -8.0f, velocity = 40.0f, bendRange = 2.0f, drift = 25.0f, chorus = 0.0f;
        // ---- Prism ----
        float pWave1 = 0.0f, pShape1 = 0.0f, pOct1 = 1.0f, pWave2 = 0.0f, pShape2 = 0.0f, pOct2 = 1.0f;
        float pPitch2 = 7.0f, pSync = 0.0f, pRing = 0.0f, pXmod = 0.0f, pMultiType = 1.0f, pMultiShape = 30.0f;
        float pMultiRatio = 3.0f, pMix1 = 7.0f, pMix2 = 6.0f, pMixM = 0.0f, pCutoff = 3000.0f, pRes = 2.0f;
        float pDrive = 0.0f, pPoles = 1.0f, pKey = 1.0f, pEgInt = 40.0f, pEgTarget = 0.0f, pAA = 0.005f;
        float pAD = 0.6f, pAS = 8.0f, pAR = 0.4f, pEA = 0.005f, pED = 0.5f, pES = 2.0f;
        float pER = 0.4f, pLfoWave = 1.0f, pLfoRate = 2.0f, pLfoInt = 0.0f, pLfoTarget = 0.0f;
        // ---- Quad ----
        float qWave1 = 0.0f, qPw1 = 50.0f, qTune1 = 0.0f, qWave2 = 1.0f, qPw2 = 35.0f, qTune2 = 0.0f;
        float qDetune = 8.0f, qMix1 = 8.0f, qMix2 = 6.0f, qSub = 4.0f, qSubOct = 0.0f, qNoise = 0.0f;
        float qLp = 1500.0f, qLpRes = 3.0f, qLpEnv = 4.0f, qHp = 30.0f, qHpRes = 0.0f, qHpEnv = 0.0f;
        float qKey = 2.0f, qDrive = 25.0f, qFA = 0.003f, qFD = 0.4f, qFS = 3.0f, qFR = 0.3f;
        float qAA = 0.003f, qAD = 0.5f, qAS = 7.0f, qAR = 0.25f, qLfoRate = 4.0f, qLfoAmt = 0.0f;
        float qLfoDest = 2.0f;
        // ---- Velvet ----
        float vRange = 1.0f, vSaw = 1.0f, vPulse = 0.0f, vPw = 40.0f, vPwmSrc = 1.0f, vSub = 4.0f;
        float vNoise = 0.0f, vLfoPitch = 0.0f, vHpf = 1.0f, vCutoff = 2200.0f, vRes = 1.0f, vEnv = 4.0f;
        float vEnvPol = 0.0f, vLfoF = 0.0f, vKey = 5.0f, vVca = 0.0f, vA = 0.01f, vD = 0.8f;
        float vS = 6.0f, vR = 0.6f, vLfoRate = 3.0f, vLfoDelay = 0.4f;
        // ---- Nova ----
        float nWave1 = 0.0f, nFeet1 = 1.0f, nSine1 = 0.0f, nWave2 = 0.0f, nFeet2 = 1.0f, nSine2 = 0.0f;
        float nDetune = 9.0f, nBalance = 50.0f, nPw = 30.0f, nPwm = 0.0f, nHpf = 40.0f, nLpf = 1400.0f;
        float nRes = 2.0f, nEnv = 55.0f, nIL = -30.0f, nAL = 80.0f, nFA = 0.3f, nFD = 1.8f;
        float nAA = 0.08f, nAD = 1.0f, nAS = 9.0f, nAR = 1.6f, nInitBright = 50.0f, nInitLevel = 40.0f;
        float nAfterBright = 60.0f, nAfterVib = 45.0f, nAfterPitch = 0.0f, nVibRate = 5.6f, nVib = 0.0f;

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
                { "model", &Params::model }, { "vmode", &Params::voiceMode }, { "voices", &Params::voices }, { "udetune", &Params::detune },
                { "chord", &Params::chordType }, { "glide", &Params::glide }, { "volume", &Params::volume }, { "vel", &Params::velocity },
                { "bend", &Params::bendRange }, { "drift", &Params::drift }, { "chorus", &Params::chorus }, { "p1wave", &Params::pWave1 },
                { "p1shape", &Params::pShape1 }, { "p1oct", &Params::pOct1 }, { "p2wave", &Params::pWave2 }, { "p2shape", &Params::pShape2 },
                { "p2oct", &Params::pOct2 }, { "p2pitch", &Params::pPitch2 }, { "p2sync", &Params::pSync }, { "p2ring", &Params::pRing },
                { "pxmod", &Params::pXmod }, { "pmtype", &Params::pMultiType }, { "pmshape", &Params::pMultiShape }, { "pmratio", &Params::pMultiRatio },
                { "pmix1", &Params::pMix1 }, { "pmix2", &Params::pMix2 }, { "pmixm", &Params::pMixM }, { "pcutoff", &Params::pCutoff },
                { "pres", &Params::pRes }, { "pdrive", &Params::pDrive }, { "ppoles", &Params::pPoles }, { "pkey", &Params::pKey },
                { "pegint", &Params::pEgInt }, { "pegtarget", &Params::pEgTarget }, { "paa", &Params::pAA }, { "pad", &Params::pAD },
                { "pas", &Params::pAS }, { "par", &Params::pAR }, { "pea", &Params::pEA }, { "ped", &Params::pED },
                { "pes", &Params::pES }, { "per", &Params::pER }, { "plfowave", &Params::pLfoWave }, { "plforate", &Params::pLfoRate },
                { "plfoint", &Params::pLfoInt }, { "plfotarget", &Params::pLfoTarget }, { "q1wave", &Params::qWave1 }, { "q1pw", &Params::qPw1 },
                { "q1tune", &Params::qTune1 }, { "q2wave", &Params::qWave2 }, { "q2pw", &Params::qPw2 }, { "q2tune", &Params::qTune2 },
                { "q2detune", &Params::qDetune }, { "qmix1", &Params::qMix1 }, { "qmix2", &Params::qMix2 }, { "qsub", &Params::qSub },
                { "qsuboct", &Params::qSubOct }, { "qnoise", &Params::qNoise }, { "qlp", &Params::qLp }, { "qlpres", &Params::qLpRes },
                { "qenvlp", &Params::qLpEnv }, { "qhp", &Params::qHp }, { "qhpres", &Params::qHpRes }, { "qenvhp", &Params::qHpEnv },
                { "qkey", &Params::qKey }, { "qdrive", &Params::qDrive }, { "qfa", &Params::qFA }, { "qfd", &Params::qFD },
                { "qfs", &Params::qFS }, { "qfr", &Params::qFR }, { "qaa", &Params::qAA }, { "qad", &Params::qAD },
                { "qas", &Params::qAS }, { "qar", &Params::qAR }, { "qlforate", &Params::qLfoRate }, { "qlfoamt", &Params::qLfoAmt },
                { "qlfodest", &Params::qLfoDest }, { "vrange", &Params::vRange }, { "vsaw", &Params::vSaw }, { "vpulse", &Params::vPulse },
                { "vpw", &Params::vPw }, { "vpwmsrc", &Params::vPwmSrc }, { "vsub", &Params::vSub }, { "vnoise", &Params::vNoise },
                { "vlfopitch", &Params::vLfoPitch }, { "vhpf", &Params::vHpf }, { "vcutoff", &Params::vCutoff }, { "vres", &Params::vRes },
                { "venv", &Params::vEnv }, { "venvpol", &Params::vEnvPol }, { "vlfof", &Params::vLfoF }, { "vkey", &Params::vKey },
                { "vvca", &Params::vVca }, { "va", &Params::vA }, { "vd", &Params::vD }, { "vs", &Params::vS },
                { "vr", &Params::vR }, { "vlforate", &Params::vLfoRate }, { "vlfodelay", &Params::vLfoDelay }, { "n1wave", &Params::nWave1 },
                { "n1feet", &Params::nFeet1 }, { "n1sine", &Params::nSine1 }, { "n2wave", &Params::nWave2 }, { "n2feet", &Params::nFeet2 },
                { "n2sine", &Params::nSine2 }, { "ndetune", &Params::nDetune }, { "nbal", &Params::nBalance }, { "npw", &Params::nPw },
                { "npwm", &Params::nPwm }, { "nhpf", &Params::nHpf }, { "nlpf", &Params::nLpf }, { "nres", &Params::nRes },
                { "nenv", &Params::nEnv }, { "nil", &Params::nIL }, { "nal", &Params::nAL }, { "nfa", &Params::nFA },
                { "nfd", &Params::nFD }, { "naa", &Params::nAA }, { "nad", &Params::nAD }, { "nas", &Params::nAS },
                { "nar", &Params::nAR }, { "nibright", &Params::nInitBright }, { "nilevel", &Params::nInitLevel }, { "nabright", &Params::nAfterBright },
                { "navib", &Params::nAfterVib }, { "napitch", &Params::nAfterPitch }, { "nvibrate", &Params::nVibRate }, { "nvib", &Params::nVib },
            };
            return all;
        }
    };

    Params params;

    //==============================================================================
    using Control = EngineControl;

    /** Every control, in the order the device panel shows them: the shared ones (model, voice
        and output), then each model's, in sections named after the model ("Prism · Filter"). */
    static const std::vector<SynthControl>& controls()
    {
        const Params d;     // the defaults
        static const std::vector<SynthControl> all = {
            { "model",     "Model",                 "Model",               0, 3, d.model, 1, false, "", { "Prism", "Quad", "Velvet", "Nova" } },
            { "vmode",     "Voice Mode",            "Voice",               0, 3, d.voiceMode, 1, false, "", { "Poly", "Unison", "Mono", "Chord" } },
            { "voices",    "Voices",                "Voice",               1, 16, d.voices, 1, false, "", {} },
            { "udetune",   "Unison Detune",         "Voice",               0.0f, 100.0f, d.detune, 0, false, "%", {} },
            { "chord",     "Chord",                 "Voice",               0, 9, d.chordType, 1, false, "", { "5th", "sus2", "Minor", "Major", "sus4", "m7", "7", "Maj7", "m9", "Maj9" } },
            { "glide",     "Glide",                 "Voice",               0.0f, 3.0f, d.glide, 0, false, "s", {} },
            { "volume",    "Volume",                "Output",              -60.0f, 6.0f, d.volume, 0, false, "dB", {} },
            { "vel",       "Velocity",              "Output",              0.0f, 100.0f, d.velocity, 0, false, "%", {} },
            { "drift",     "Drift",                 "Output",              0.0f, 100.0f, d.drift, 0, false, "%", {} },
            { "bend",      "Bend Range",            "Output",              1, 12, d.bendRange, 1, false, "st", {} },
            { "chorus",    "Chorus",                "Output",              0, 3, d.chorus, 1, false, "", { "Off", "I", "II", "I+II" } },
            { "p1wave",    "VCO 1 Wave",            "Prism · VCO 1",       0, 2, d.pWave1, 1, false, "", { "Saw", "Triangle", "Square" } },
            { "p1oct",     "VCO 1 Octave",          "Prism · VCO 1",       0, 3, d.pOct1, 1, false, "", { "16'", "8'", "4'", "2'" } },
            { "p1shape",   "VCO 1 Shape",           "Prism · VCO 1",       0.0f, 100.0f, d.pShape1, 0, false, "%", {} },
            { "p2wave",    "VCO 2 Wave",            "Prism · VCO 2",       0, 2, d.pWave2, 1, false, "", { "Saw", "Triangle", "Square" } },
            { "p2sync",    "Sync",                  "Prism · VCO 2",       0, 1, d.pSync, 1, false, "", { "Off", "On" } },
            { "p2ring",    "Ring",                  "Prism · VCO 2",       0, 1, d.pRing, 1, false, "", { "Off", "On" } },
            { "p2oct",     "VCO 2 Octave",          "Prism · VCO 2",       0, 3, d.pOct2, 1, false, "", { "16'", "8'", "4'", "2'" } },
            { "p2pitch",   "VCO 2 Pitch",           "Prism · VCO 2",       -1200.0f, 1200.0f, d.pPitch2, 0, false, "ct", {} },
            { "p2shape",   "VCO 2 Shape",           "Prism · VCO 2",       0.0f, 100.0f, d.pShape2, 0, false, "%", {} },
            { "pxmod",     "Cross Mod",             "Prism · VCO 2",       0.0f, 100.0f, d.pXmod, 0, false, "%", {} },
            { "pmtype",    "Multi Type",            "Prism · Multi",       0, 1, d.pMultiType, 1, false, "", { "Noise", "FM" } },
            { "pmshape",   "Multi Shape",           "Prism · Multi",       0.0f, 100.0f, d.pMultiShape, 0, false, "%", {} },
            { "pmratio",   "FM Ratio",              "Prism · Multi",       0, 7, d.pMultiRatio, 1, false, "", { "0.5", "1", "1.5", "2", "3", "4", "5", "7" } },
            { "pmix1",     "VCO 1 Level",           "Prism · Mixer",       0.0f, 10.0f, d.pMix1, 0, false, "", {} },
            { "pmix2",     "VCO 2 Level",           "Prism · Mixer",       0.0f, 10.0f, d.pMix2, 0, false, "", {} },
            { "pmixm",     "Multi Level",           "Prism · Mixer",       0.0f, 10.0f, d.pMixM, 0, false, "", {} },
            { "pcutoff",   "Cutoff",                "Prism · Filter",      20.0f, 20000.0f, d.pCutoff, 0, true, "Hz", {} },
            { "pres",      "Resonance",             "Prism · Filter",      0.0f, 10.0f, d.pRes, 0, false, "", {} },
            { "pegint",    "EG Int",                "Prism · Filter",      -100.0f, 100.0f, d.pEgInt, 0, false, "%", {} },
            { "pkey",      "Key Track",             "Prism · Filter",      0, 2, d.pKey, 1, false, "", { "0%", "50%", "100%" } },
            { "ppoles",    "Filter Type",           "Prism · Filter",      0, 1, d.pPoles, 1, false, "", { "2-Pole", "4-Pole" } },
            { "pdrive",    "Drive",                 "Prism · Filter",      0, 2, d.pDrive, 1, false, "", { "0%", "50%", "100%" } },
            { "pegtarget", "EG Target",             "Prism · EG",          0, 2, d.pEgTarget, 1, false, "", { "Cutoff", "Pitch", "Pitch 2" } },
            { "pea",       "EG Attack",             "Prism · EG",          0.001f, 10, d.pEA, 0, true, "s", {} },
            { "ped",       "EG Decay",              "Prism · EG",          0.001f, 10, d.pED, 0, true, "s", {} },
            { "pes",       "EG Sustain",            "Prism · EG",          0.0f, 10.0f, d.pES, 0, false, "", {} },
            { "per",       "EG Release",            "Prism · EG",          0.001f, 10, d.pER, 0, true, "s", {} },
            { "paa",       "Amp Attack",            "Prism · Amp",         0.001f, 10, d.pAA, 0, true, "s", {} },
            { "pad",       "Amp Decay",             "Prism · Amp",         0.001f, 10, d.pAD, 0, true, "s", {} },
            { "pas",       "Amp Sustain",           "Prism · Amp",         0.0f, 10.0f, d.pAS, 0, false, "", {} },
            { "par",       "Amp Release",           "Prism · Amp",         0.001f, 10, d.pAR, 0, true, "s", {} },
            { "plfowave",  "LFO Wave",              "Prism · LFO",         0, 2, d.pLfoWave, 1, false, "", { "Saw", "Triangle", "Square" } },
            { "plforate",  "LFO Rate",              "Prism · LFO",         0.05f, 30.0f, d.pLfoRate, 0, true, "Hz", {} },
            { "plfoint",   "LFO Int",               "Prism · LFO",         -100.0f, 100.0f, d.pLfoInt, 0, false, "%", {} },
            { "plfotarget", "LFO Target",            "Prism · LFO",         0, 2, d.pLfoTarget, 1, false, "", { "Cutoff", "Shape", "Pitch" } },
            { "q1wave",    "Osc 1 Wave",            "Quad · Osc 1",        0, 2, d.qWave1, 1, false, "", { "Saw", "Pulse", "Triangle" } },
            { "q1pw",      "Osc 1 Width",           "Quad · Osc 1",        5.0f, 50.0f, d.qPw1, 0, false, "%", {} },
            { "q1tune",    "Osc 1 Tune",            "Quad · Osc 1",        -24.0f, 24.0f, d.qTune1, 0, false, "st", {} },
            { "q2wave",    "Osc 2 Wave",            "Quad · Osc 2",        0, 2, d.qWave2, 1, false, "", { "Saw", "Pulse", "Triangle" } },
            { "q2pw",      "Osc 2 Width",           "Quad · Osc 2",        5.0f, 50.0f, d.qPw2, 0, false, "%", {} },
            { "q2tune",    "Osc 2 Tune",            "Quad · Osc 2",        -24.0f, 24.0f, d.qTune2, 0, false, "st", {} },
            { "q2detune",  "Osc 2 Detune",          "Quad · Osc 2",        -50.0f, 50.0f, d.qDetune, 0, false, "ct", {} },
            { "qmix1",     "Osc 1 Level",           "Quad · Mixer",        0.0f, 10.0f, d.qMix1, 0, false, "", {} },
            { "qmix2",     "Osc 2 Level",           "Quad · Mixer",        0.0f, 10.0f, d.qMix2, 0, false, "", {} },
            { "qsub",      "Sub Level",             "Quad · Mixer",        0.0f, 10.0f, d.qSub, 0, false, "", {} },
            { "qsuboct",   "Sub Octave",            "Quad · Mixer",        0, 1, d.qSubOct, 1, false, "", { "-1", "-2" } },
            { "qnoise",    "Noise Level",           "Quad · Mixer",        0.0f, 10.0f, d.qNoise, 0, false, "", {} },
            { "qlp",       "Low-Pass",              "Quad · Filters",      20.0f, 20000.0f, d.qLp, 0, true, "Hz", {} },
            { "qlpres",    "LP Resonance",          "Quad · Filters",      0.0f, 10.0f, d.qLpRes, 0, false, "", {} },
            { "qenvlp",    "LP Env",                "Quad · Filters",      -10.0f, 10.0f, d.qLpEnv, 0, false, "", {} },
            { "qkey",      "Key Track",             "Quad · Filters",      0, 3, d.qKey, 1, false, "", { "Off", "1/3", "2/3", "Full" } },
            { "qhp",       "High-Pass",             "Quad · Filters",      20.0f, 4000.0f, d.qHp, 0, true, "Hz", {} },
            { "qhpres",    "HP Resonance",          "Quad · Filters",      0.0f, 10.0f, d.qHpRes, 0, false, "", {} },
            { "qenvhp",    "HP Env",                "Quad · Filters",      -10.0f, 10.0f, d.qHpEnv, 0, false, "", {} },
            { "qdrive",    "Overdrive",             "Quad · Filters",      0.0f, 100.0f, d.qDrive, 0, false, "%", {} },
            { "qfa",       "Filter Attack",         "Quad · Filter Env",   0.001f, 10, d.qFA, 0, true, "s", {} },
            { "qfd",       "Filter Decay",          "Quad · Filter Env",   0.001f, 10, d.qFD, 0, true, "s", {} },
            { "qfs",       "Filter Sustain",        "Quad · Filter Env",   0.0f, 10.0f, d.qFS, 0, false, "", {} },
            { "qfr",       "Filter Release",        "Quad · Filter Env",   0.001f, 10, d.qFR, 0, true, "s", {} },
            { "qaa",       "Amp Attack",            "Quad · Amp Env",      0.001f, 10, d.qAA, 0, true, "s", {} },
            { "qad",       "Amp Decay",             "Quad · Amp Env",      0.001f, 10, d.qAD, 0, true, "s", {} },
            { "qas",       "Amp Sustain",           "Quad · Amp Env",      0.0f, 10.0f, d.qAS, 0, false, "", {} },
            { "qar",       "Amp Release",           "Quad · Amp Env",      0.001f, 10, d.qAR, 0, true, "s", {} },
            { "qlforate",  "LFO Rate",              "Quad · LFO",          0.05f, 30.0f, d.qLfoRate, 0, true, "Hz", {} },
            { "qlfoamt",   "LFO Depth",             "Quad · LFO",          0.0f, 100.0f, d.qLfoAmt, 0, false, "%", {} },
            { "qlfodest",  "LFO Target",            "Quad · LFO",          0, 4, d.qLfoDest, 1, false, "", { "Pitch", "Width", "Low-Pass", "High-Pass", "Amp" } },
            { "vlforate",  "LFO Rate",              "Velvet · LFO",        0.1f, 20.0f, d.vLfoRate, 0, true, "Hz", {} },
            { "vlfodelay", "LFO Delay",             "Velvet · LFO",        0.0f, 3.0f, d.vLfoDelay, 0, false, "s", {} },
            { "vrange",    "Range",                 "Velvet · DCO",        0, 2, d.vRange, 1, false, "", { "16'", "8'", "4'" } },
            { "vsaw",      "Saw",                   "Velvet · DCO",        0, 1, d.vSaw, 1, false, "", { "Off", "On" } },
            { "vpulse",    "Pulse",                 "Velvet · DCO",        0, 1, d.vPulse, 1, false, "", { "Off", "On" } },
            { "vpwmsrc",   "PWM Source",            "Velvet · DCO",        0, 2, d.vPwmSrc, 1, false, "", { "Manual", "LFO", "Env" } },
            { "vlfopitch", "DCO LFO",               "Velvet · DCO",        0.0f, 10.0f, d.vLfoPitch, 0, false, "", {} },
            { "vpw",       "Pulse Width",           "Velvet · DCO",        0.0f, 100.0f, d.vPw, 0, false, "%", {} },
            { "vsub",      "Sub",                   "Velvet · DCO",        0.0f, 10.0f, d.vSub, 0, false, "", {} },
            { "vnoise",    "Noise",                 "Velvet · DCO",        0.0f, 10.0f, d.vNoise, 0, false, "", {} },
            { "vhpf",      "HPF",                   "Velvet · HPF",        0, 3, d.vHpf, 1, false, "", { "0", "1", "2", "3" } },
            { "vcutoff",   "Cutoff",                "Velvet · VCF",        20.0f, 20000.0f, d.vCutoff, 0, true, "Hz", {} },
            { "vres",      "Resonance",             "Velvet · VCF",        0.0f, 10.0f, d.vRes, 0, false, "", {} },
            { "venv",      "Env",                   "Velvet · VCF",        0.0f, 10.0f, d.vEnv, 0, false, "", {} },
            { "venvpol",   "Env Polarity",          "Velvet · VCF",        0, 1, d.vEnvPol, 1, false, "", { "Normal", "Inverted" } },
            { "vlfof",     "VCF LFO",               "Velvet · VCF",        0.0f, 10.0f, d.vLfoF, 0, false, "", {} },
            { "vkey",      "Key Follow",            "Velvet · VCF",        0.0f, 10.0f, d.vKey, 0, false, "", {} },
            { "vvca",      "VCA Mode",              "Velvet · VCA",        0, 1, d.vVca, 1, false, "", { "Env", "Gate" } },
            { "va",        "Attack",                "Velvet · Env",        0.001f, 10, d.vA, 0, true, "s", {} },
            { "vd",        "Decay",                 "Velvet · Env",        0.001f, 10, d.vD, 0, true, "s", {} },
            { "vs",        "Sustain",               "Velvet · Env",        0.0f, 10.0f, d.vS, 0, false, "", {} },
            { "vr",        "Release",               "Velvet · Env",        0.001f, 10, d.vR, 0, true, "s", {} },
            { "n1wave",    "I Wave",                "Nova · Channel I",    0, 2, d.nWave1, 1, false, "", { "Saw", "Square", "Pulse" } },
            { "n1feet",    "I Feet",                "Nova · Channel I",    0, 2, d.nFeet1, 1, false, "", { "16'", "8'", "4'" } },
            { "n1sine",    "I Sine",                "Nova · Channel I",    0.0f, 10.0f, d.nSine1, 0, false, "", {} },
            { "nbal",      "I/II Balance",          "Nova · Channel I",    0.0f, 100.0f, d.nBalance, 0, false, "%", {} },
            { "n2wave",    "II Wave",               "Nova · Channel II",   0, 2, d.nWave2, 1, false, "", { "Saw", "Square", "Pulse" } },
            { "n2feet",    "II Feet",               "Nova · Channel II",   0, 2, d.nFeet2, 1, false, "", { "16'", "8'", "4'" } },
            { "n2sine",    "II Sine",               "Nova · Channel II",   0.0f, 10.0f, d.nSine2, 0, false, "", {} },
            { "ndetune",   "II Detune",             "Nova · Channel II",   0.0f, 40.0f, d.nDetune, 0, false, "ct", {} },
            { "npw",       "Pulse Width",           "Nova · Pulse",        5.0f, 50.0f, d.nPw, 0, false, "%", {} },
            { "npwm",      "PWM",                   "Nova · Pulse",        0.0f, 100.0f, d.nPwm, 0, false, "%", {} },
            { "nhpf",      "High-Pass",             "Nova · Filter",       20.0f, 3000.0f, d.nHpf, 0, true, "Hz", {} },
            { "nlpf",      "Low-Pass",              "Nova · Filter",       60.0f, 18000.0f, d.nLpf, 0, true, "Hz", {} },
            { "nres",      "Resonance",             "Nova · Filter",       0.0f, 10.0f, d.nRes, 0, false, "", {} },
            { "nenv",      "Filter Env",            "Nova · Filter",       0.0f, 100.0f, d.nEnv, 0, false, "%", {} },
            { "nil",       "Initial Level",         "Nova · Filter EG",    -100.0f, 100.0f, d.nIL, 0, false, "%", {} },
            { "nal",       "Attack Level",          "Nova · Filter EG",    -100.0f, 100.0f, d.nAL, 0, false, "%", {} },
            { "nfa",       "Filter Attack",         "Nova · Filter EG",    0.001f, 10, d.nFA, 0, true, "s", {} },
            { "nfd",       "Filter Decay",          "Nova · Filter EG",    0.001f, 10, d.nFD, 0, true, "s", {} },
            { "naa",       "Amp Attack",            "Nova · Amp",          0.001f, 10, d.nAA, 0, true, "s", {} },
            { "nad",       "Amp Decay",             "Nova · Amp",          0.001f, 10, d.nAD, 0, true, "s", {} },
            { "nas",       "Amp Sustain",           "Nova · Amp",          0.0f, 10.0f, d.nAS, 0, false, "", {} },
            { "nar",       "Amp Release",           "Nova · Amp",          0.001f, 10, d.nAR, 0, true, "s", {} },
            { "nibright",  "Initial Brilliance",    "Nova · Touch",        0.0f, 100.0f, d.nInitBright, 0, false, "%", {} },
            { "nilevel",   "Initial Level Touch",   "Nova · Touch",        0.0f, 100.0f, d.nInitLevel, 0, false, "%", {} },
            { "nvib",      "Vibrato",               "Nova · Touch",        0.0f, 100.0f, d.nVib, 0, false, "%", {} },
            { "nvibrate",  "Vibrato Rate",          "Nova · Touch",        0.5f, 12.0f, d.nVibRate, 0, true, "Hz", {} },
            { "nabright",  "After Brilliance",      "Nova · Touch",        0.0f, 100.0f, d.nAfterBright, 0, false, "%", {} },
            { "navib",     "After Vibrato",         "Nova · Touch",        0.0f, 100.0f, d.nAfterVib, 0, false, "%", {} },
            { "napitch",   "After Pitch",           "Nova · Touch",        -100.0f, 100.0f, d.nAfterPitch, 0, false, "%", {} },
        };
        return all;
    }

    /** The shared controls and those of one model (0 Prism, 1 Quad, 2 Velvet, 3 Nova). */
    static std::vector<const Control*> controlsFor (int model)
    {
        static const char* const prefixes[] = { "Prism ", "Quad ", "Velvet ", "Nova " };
        std::vector<const Control*> result;
        for (auto& c : controls())
        {
            bool ofAModel = false, ofThisOne = false;
            for (int m = 0; m < numModels; ++m)
                if (std::strncmp (c.section, prefixes[m], std::strlen (prefixes[m])) == 0)
                {
                    ofAModel = true;
                    ofThisOne = m == model;
                }
            if (! ofAModel || ofThisOne)
                result.push_back (&c);
        }
        return result;
    }

    /** The names of a menu control's choices (the models, the voice modes, ...). */
    static const std::vector<const char*>& choicesOf (const char* id)
    {
        static const std::vector<const char*> none;
        for (auto& c : controls())
            if (std::strcmp (c.id, id) == 0)
                return c.choices;
        return none;
    }

    //==============================================================================
    // Patches: each belongs to a model, sets the controls it names and leaves the rest at their defaults
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

    static int patchCount() noexcept                { return (int) patches().size(); }
    static const char* patchName (int i) noexcept   { return i >= 0 && i < patchCount() ? patches()[(size_t) i].name : ""; }
    static int patchModel (int i) noexcept          { return i >= 0 && i < patchCount() ? patches()[(size_t) i].model : prism; }

    /** A control's value in a patch: the patch's model, the patch's own value, or the default. */
    static float patchValue (int i, const char* id)
    {
        if (i >= 0 && i < patchCount())
        {
            if (std::strcmp (id, "model") == 0)
                return (float) patches()[(size_t) i].model;
            for (auto& [name, value] : patches()[(size_t) i].values)
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
    /** The notes of each chord type, from the key played. */
    static int chordIntervals (int c, int* out) noexcept
    {
        static constexpr int notes[10][5] = { { 0, 7, 12 }, { 0, 2, 7 }, { 0, 3, 7 }, { 0, 4, 7 }, { 0, 5, 7 },
                                              { 0, 3, 7, 10 }, { 0, 4, 7, 10 }, { 0, 4, 7, 11 }, { 0, 3, 7, 10, 14 }, { 0, 4, 7, 11, 14 } };
        static constexpr int counts[10] = { 3, 3, 3, 3, 3, 4, 4, 4, 5, 5 };
        c = dspmath::jlimit (0, 9, c);
        for (int i = 0; i < counts[c]; ++i) out[i] = notes[c][i];
        return counts[c];
    }

    int getModel() const noexcept   { return dspmath::jlimit (0, numModels - 1, whole (params.model)); }

    /** Quad is a four-voice instrument; the others go up to the Voices setting. */
    int polyphony() const noexcept
    {
        const int v = dspmath::jlimit (1, maxVoices, whole (params.voices));
        return getModel() == quad ? dspmath::jmin (4, v) : v;
    }

    //==============================================================================
    void prepare (double sampleRate, int /*maxBlock*/ = 0)
    {
        rate = sampleRate;
        osRate = (float) (rate * 2.0);
        for (auto& d : downsampler) d.prepare (1);
        work[0].fill (0.0f);
        work[1].fill (0.0f);
        chorusFx.prepare (rate);
        for (size_t i = 0; i < voice.size(); ++i)
        {
            voice[i] = Voice();
            voice[i].drift = { SynthCore::Drift ((int) i * 7 + 3), SynthCore::Drift ((int) i * 11 + 5) };
        }
        noiseGen = SynthCore::Noise();
        stack.clear();
        noteCounter = 0;
        bend = modWheel = lfoPhase = 0.0f;
        lastPitch = 60.0f;
        pedal = false;
        activeVoices = 0;
        samplesPerCycle = 200.0f;
        prepared = true;
    }

    bool isPrepared() const noexcept    { return prepared; }

    /** The delay the downsampling adds, in samples at the output rate. */
    double latencySamples() const noexcept     { return prepared ? downsampler[0].latencySamples() : 0.0; }

    //==============================================================================
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

        int intervals[5] = { 0 };
        const int count = m == chord ? chordIntervals (whole (params.chordType), intervals) : 1;
        for (int i = 0; i < count; ++i)
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
            else release (v);
        }
    }

    /** Every note let go (they ring out through their release). */
    void allNotesOff()
    {
        for (auto& v : voice) { v.held = v.sustained = false; release (v); }
        stack.clear();
    }

    void setPitchBend (float minusOneToOne)     { bend = minusOneToOne; }    // times the Bend Range
    void setModWheel (float zeroToOne)          { modWheel = zeroToOne; }    // deepens each model's LFO

    void setSustainPedal (bool isDown)
    {
        pedal = isDown;
        if (! isDown)
            for (auto& v : voice)
                if (v.sustained && ! v.held) { v.sustained = false; release (v); }
    }

    /** Polyphonic aftertouch on a held key, 0..1 (Nova plays with it). */
    void setAftertouch (int note, float zeroToOne)
    {
        for (auto& v : voice)
            if (v.key == note && v.held)
                v.press = zeroToOne;
    }

    /** Channel pressure, 0..1: aftertouch on every held key. */
    void setChannelPressure (float zeroToOne)
    {
        for (auto& v : voice)
            if (v.held)
                v.press = zeroToOne;
    }

    //==============================================================================
    /** Renders n samples, adding them to L and R. */
    void render (float* L, float* R, int num)
    {
        if (! prepared)
            return;

        for (int done = 0; done < num; )
        {
            const int n = std::min (maxChunk, num - done);
            renderChunk (L + done, R + done, n);
            done += n;
        }
    }

    // What's going on, for drawing
    SynthCore::ScopeBuffer scope;
    std::atomic<int> activeVoices { 0 };
    std::atomic<float> samplesPerCycle { 200.0f };   // at the output rate, for the scope to lock to

private:
    /** Nova's filter envelope: from the Initial Level it moves to the Attack Level, then decays
        to the cutoff itself; on release it returns to the Initial Level. */
    struct NovaEnvelope
    {
        float value = 0.0f;
        bool on = false, attacking = false;

        void gate (bool g)      { if (g && ! on) attacking = true; if (g) restart = true; on = g; }
        bool restart = false;

        float process (float il, float al, float a, float d, float r, float sampleRate) noexcept
        {
            auto coeff = [sampleRate] (float t) { return 1.0f - std::exp (-1.0f / (dspmath::jmax (0.0005f, t) * sampleRate)); };
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
        std::uint32_t age = 0;
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

    double rate = 48000.0;
    float osRate = 96000.0f;
    bool prepared = false;
    std::array<Downsampler, 2> downsampler;
    std::array<std::array<float, maxChunk * 2>, 2> work {};     // the voices at twice the rate
    std::array<Voice, maxVoices> voice;
    SynthCore::NoteStack stack;
    SynthCore::Noise noiseGen;
    SynthCore::StereoChorus chorusFx;
    std::array<float, maxChunk * 2> lfoBuf {};
    std::uint32_t noteCounter = 0;
    float bend = 0.0f, modWheel = 0.0f, lfoPhase = 0.0f, lastPitch = 60.0f;
    bool pedal = false;

    static int whole (float v) noexcept     { return (int) std::lround (v); }
    int mode() const noexcept               { return dspmath::jlimit (0, 3, whole (params.voiceMode)); }

    static void release (Voice& v)          { v.amp.gate (false); v.mod.gate (false); v.novaEnv.gate (false); }

    //==============================================================================
    void startVoice (Voice& v, int note, int key, float vel, float fromPitch, float detuneSemis, float pan)
    {
        const bool wasSilent = ! v.active();
        v.note = note; v.key = key; v.vel = vel; v.held = true; v.sustained = false; v.age = ++noteCounter;
        v.target = (float) note;
        v.pitch = params.glide > 0.0f ? fromPitch : (float) note;
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
            std::uint32_t oldest = std::numeric_limits<std::uint32_t>::max();
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
        const int count = uni ? dspmath::jlimit (2, dspmath::jmin (8, maxVoices), polyphony()) : 1;
        const float spread = params.detune / 100.0f * 0.5f;

        for (int i = 0; i < count; ++i)
        {
            auto& v = voice[(size_t) i];
            const float k = count > 1 ? (float) i / (float) (count - 1) * 2.0f - 1.0f : 0.0f;   // -1 .. 1
            if (retrigger || ! v.active())
                startVoice (v, note, note, vel, lastPitch, k * spread, uni ? k * 0.8f : 0.0f);
            else
            {
                v.note = note; v.target = (float) note; v.held = true;
                if (params.glide <= 0.0f) v.pitch = (float) note;
            }
        }
        lastPitch = (float) note;
    }

    static float mtof (float note)      { return 440.0f * std::pow (2.0f, (note - 69.0f) / 12.0f); }
    static float level (float tenths)   { auto x = dspmath::jlimit (0.0f, 1.0f, tenths / 10.0f); return x * x * 0.6f + x * 0.4f; }

    struct Common
    {
        float sr, bendSemis, driftSemis, velAmt, glideCoeff;
    };

    void renderChunk (float* outL, float* outR, int n)
    {
        Common c;
        c.sr = osRate;
        c.bendSemis = bend * params.bendRange;
        c.driftSemis = params.drift / 100.0f * 0.1f;
        c.velAmt = params.velocity / 100.0f;
        c.glideCoeff = params.glide <= 0.0f ? 1.0f : 1.0f - std::exp (-1.0f / (params.glide * 0.35f * c.sr));

        float* L = work[0].data();
        float* R = work[1].data();
        const int osN = n * 2;
        std::fill (L, L + osN, 0.0f);
        std::fill (R, R + osN, 0.0f);

        // One LFO for all voices
        const int currentModel = getModel();
        {
            const float rateHz = currentModel == prism ? params.pLfoRate : currentModel == quad ? params.qLfoRate
                               : currentModel == nova ? params.nVibRate : params.vLfoRate;
            const int shape = currentModel == prism ? whole (params.pLfoWave) : (currentModel == quad || currentModel == nova) ? 3 : 1;
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
        const int voices = polyphony();
        for (int i = 0; i < voices; ++i)
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
        for (int i = voices; i < maxVoices; ++i)
            voice[(size_t) i].amp.reset();

        activeVoices = sounding;

        // Back down to the output rate: the first n samples of each now hold the result
        downsampler[0].process (L, n);
        downsampler[1].process (R, n);

        chorusFx.process (L, R, n, whole (params.chorus), (float) rate);

        const float outGain = params.volume > -60.0f ? std::pow (10.0f, params.volume * 0.05f) : 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const float l = std::tanh (L[i] * outGain * 1.2f) / 1.2f;
            const float r = std::tanh (R[i] * outGain * 1.2f) / 1.2f;
            outL[i] += l;
            outR[i] += r;
            scope.push ((l + r) * 0.5f);
        }

        samplesPerCycle = (float) rate / dspmath::jmax (1.0f, mtof (scopePitch));
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
            y = shape < 0.01f ? t : std::sin (t * (1.0f + 3.0f * shape) * dspmath::MathConstants<float>::halfPi);
        }
        else                    y = o.pulseAt (0.5f - 0.45f * shape, dt);
        return y;
    }

    void renderPrism (Voice& v, float* L, float* R, int osN, const Common& c)
    {
        const float sr = c.sr;
        static constexpr float octs[] = { -12.0f, 0.0f, 12.0f, 24.0f };
        static constexpr float ratios[] = { 0.5f, 1.0f, 1.5f, 2.0f, 3.0f, 4.0f, 5.0f, 7.0f };
        const int w1 = whole (params.pWave1), w2 = whole (params.pWave2);
        const float oct1 = octs[dspmath::jlimit (0, 3, whole (params.pOct1))];
        const float oct2 = octs[dspmath::jlimit (0, 3, whole (params.pOct2))] + params.pPitch2 / 100.0f;
        const float s1 = params.pShape1 / 100.0f, s2 = params.pShape2 / 100.0f;
        const bool sync = params.pSync > 0.5f, ring = params.pRing > 0.5f;
        const float xmod = params.pXmod / 100.0f;
        const bool fm = params.pMultiType > 0.5f;
        const float mShape = params.pMultiShape / 100.0f;
        const float ratio = ratios[dspmath::jlimit (0, 7, whole (params.pMultiRatio))];
        const float g1 = level (params.pMix1), g2 = level (params.pMix2), gm = level (params.pMixM);
        const float baseCutoff = params.pCutoff;
        const float res = std::pow (params.pRes / 10.0f, 1.3f);
        const float driveGain = 0.55f * (1.0f + std::round (params.pDrive) * 1.2f);
        const bool fourPole = params.pPoles > 0.5f;
        const float keyAmt = (float) whole (params.pKey) * 0.5f;
        const float egInt = params.pEgInt / 100.0f;
        const int egTarget = whole (params.pEgTarget);
        const float lfoInt = dspmath::jlimit (-1.0f, 1.0f, params.pLfoInt / 100.0f + modWheel * (params.pLfoInt >= 0.0f ? 1.0f : -1.0f));
        const int lfoTarget = whole (params.pLfoTarget);
        const float velGain = 1.0f - c.velAmt * (1.0f - v.vel);
        const float aa = params.pAA, ad = params.pAD, as = params.pAS / 10.0f, ar = params.pAR;
        const float ea = params.pEA, ed = params.pED, es = params.pES / 10.0f, er = params.pER;
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
            const float o1 = shapedVco (v.osc[0], w1, dspmath::jlimit (0.0f, 1.0f, s1 + shapeMod), dt1);
            const bool wrapped = v.osc[0].advance (dt1);

            float dt2 = mtof (base + oct2 + pitch2 + v.drift[1].process (c.driftSemis, sr)) / sr;
            dt2 *= dspmath::jmax (0.05f, 1.0f + xmod * o1 * 1.5f);
            if (sync && wrapped)
                v.osc[1].phase = v.osc[0].phase * dt2 / dspmath::jmax (1.0e-7f, dt1);
            float o2 = shapedVco (v.osc[1], w2, dspmath::jlimit (0.0f, 1.0f, s2 + shapeMod), dt2);
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
        const int w1 = whole (params.qWave1), w2 = whole (params.qWave2);
        const float pw1 = params.qPw1 / 100.0f, pw2 = params.qPw2 / 100.0f;
        const float t1 = params.qTune1, t2 = params.qTune2 + params.qDetune / 100.0f;
        const float g1 = level (params.qMix1), g2 = level (params.qMix2), gs = level (params.qSub), gn = level (params.qNoise) * 0.7f;
        const float subOct = params.qSubOct > 0.5f ? -24.0f : -12.0f;
        const float lp = params.qLp, lpRes = std::pow (params.qLpRes / 10.0f, 1.3f), lpEnv = params.qLpEnv / 10.0f * 5.5f;
        const float hpF = params.qHp, hpRes = params.qHpRes / 10.0f * 0.9f, hpEnv = params.qHpEnv / 10.0f * 5.5f;
        static constexpr float keyAmounts[] = { 0.0f, 1.0f / 3.0f, 2.0f / 3.0f, 1.0f };
        const float keyAmt = keyAmounts[dspmath::jlimit (0, 3, whole (params.qKey))];
        const float drive = 1.0f + params.qDrive / 100.0f * 5.0f;
        const float lfoAmt = dspmath::jlimit (0.0f, 1.0f, params.qLfoAmt / 100.0f + modWheel);
        const int lfoDest = whole (params.qLfoDest);
        const float velGain = 1.0f - c.velAmt * (1.0f - v.vel);
        const float fa = params.qFA, fd = params.qFD, fs = params.qFS / 10.0f, fr = params.qFR;
        const float aa = params.qAA, ad = params.qAD, as = params.qAS / 10.0f, ar = params.qAR;
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
            const float o1 = wave (v.osc[0], w1, dspmath::jlimit (0.05f, 0.95f, pw1 + pwMod), mtof (base + t1 + v.drift[0].process (c.driftSemis, sr)) / sr);
            const float o2 = wave (v.osc[1], w2, dspmath::jlimit (0.05f, 0.95f, pw2 + pwMod), mtof (base + t2 + v.drift[1].process (c.driftSemis, sr)) / sr);
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
        const float range = ranges[dspmath::jlimit (0, 2, whole (params.vRange))];
        const bool sawOn = params.vSaw > 0.5f, pulseOn = params.vPulse > 0.5f;
        const float pwAmt = params.vPw / 100.0f;
        const int pwmSrc = whole (params.vPwmSrc);
        const float gSub = level (params.vSub) * 0.8f, gNoise = level (params.vNoise) * 0.5f;
        const float lfoPitch = params.vLfoPitch / 10.0f * 1.5f;
        const int hpf = whole (params.vHpf);
        const float cutoff = params.vCutoff, res = std::pow (params.vRes / 10.0f, 1.3f);
        const float envOct = params.vEnv / 10.0f * 5.5f * (params.vEnvPol > 0.5f ? -1.0f : 1.0f);
        const float lfoF = params.vLfoF / 10.0f * 2.5f;
        const float keyAmt = params.vKey / 10.0f;
        const bool gateVca = params.vVca > 0.5f;
        const float a = params.vA, d = params.vD, s = params.vS / 10.0f, r = params.vR;
        const float fadeInc = params.vLfoDelay <= 0.0f ? 1.0f : 1.0f / (params.vLfoDelay * sr);
        const float lfoGain = dspmath::jlimit (0.0f, 1.0f, 1.0f + modWheel);
        const float velGain = 1.0f - c.velAmt * (1.0f - v.vel);
        v.ladder.poles = 4;

        if (hpf >= 2)
            v.hp.set (hpf == 2 ? 240.0f : 720.0f, 0.1f, sr);
        else if (hpf == 0)
            v.hp.set (120.0f, 0.1f, sr);

        for (int i = 0; i < osN; ++i)
        {
            v.pitch += (v.target - v.pitch) * c.glideCoeff;
            v.lfoFade = dspmath::jmin (1.0f, v.lfoFade + fadeInc);
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
        const int w1 = whole (params.nWave1), w2 = whole (params.nWave2);
        const float oct1 = feet[dspmath::jlimit (0, 2, whole (params.nFeet1))];
        const float oct2 = feet[dspmath::jlimit (0, 2, whole (params.nFeet2))] + params.nDetune / 100.0f;
        const float sine1 = level (params.nSine1), sine2 = level (params.nSine2);
        const float bal = params.nBalance / 100.0f;
        const float g1 = std::cos (bal * dspmath::MathConstants<float>::halfPi) * 1.2f, g2 = std::sin (bal * dspmath::MathConstants<float>::halfPi) * 1.2f;
        const float pw = params.nPw / 100.0f, pwm = params.nPwm / 100.0f;
        const float res = std::pow (params.nRes / 10.0f, 1.2f) * 0.92f;
        const float envOct = params.nEnv / 100.0f * 6.0f;
        const float il = params.nIL / 100.0f, al = params.nAL / 100.0f;
        const float fa = params.nFA, fd = params.nFD;
        const float aa = params.nAA, ad = params.nAD, as = params.nAS / 10.0f, ar = params.nAR;
        const float vel = v.vel;
        const float initBrightOct = params.nInitBright / 100.0f * 2.5f * (vel - 0.6f);
        const float levelTouch = 1.0f - params.nInitLevel / 100.0f * (1.0f - vel);
        const float afterBright = params.nAfterBright / 100.0f * 2.5f;
        const float afterVib = params.nAfterVib / 100.0f, baseVib = dspmath::jlimit (0.0f, 1.0f, params.nVib / 100.0f + modWheel);
        const float afterPitch = params.nAfterPitch / 100.0f;        // up to a semitone of growl
        const float pressCoeff = 1.0f - std::exp (-1.0f / (0.06f * sr));
        const float hpCut = params.nHpf, lpCut = params.nLpf;

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
            const float width = dspmath::jlimit (0.05f, 0.5f, pw + pwm * 0.4f * lfoBuf[(size_t) i]);

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
    }};

} // namespace elsewhere
