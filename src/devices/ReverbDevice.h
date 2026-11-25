// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Reverb.

    An 8-line feedback delay network (Hadamard mixing) with a modulated allpass in every line
    for density, fed through predelay, input EQ and diffusion, plus early reflections.

    What it borrows from the classics:
      - Spaces (Room, Chamber, Hall, Plate, Cathedral, Bloom), like Valhalla / Lexicon modes;
        Bloom swells slowly into a huge, unreal space (think Eventide Blackhole)
      - Decay EQ: separate decay times for lows and highs (FabFilter Pro-R, ChromaVerb)
      - Shimmer: the tail is pitch-shifted and fed back into itself
      - Ducking: the tail gets out of the way while the dry signal plays
      - Tempo-synced predelay (NI Raum), Freeze (infinite hold), modulation, width
*/
class ReverbDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereReverb";

    enum Space { room, chamber, hall, plate, cathedral, bloom, numSpaces };

    static juce::StringArray spaceNames()           { return { "Room", "Chamber", "Hall", "Plate", "Cathedral", "Bloom" }; }
    static juce::StringArray syncNames()            { return { "Free", "1/64", "1/32", "1/16", "1/8", "1/4" }; }
    static juce::StringArray shimmerNames()         { return { "-12", "+7", "+12", "+19" }; }

    ReverbDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        predelay  = addParameter ("predelay", "Predelay", 10.0f, skewed (0.0f, 250.0f, 40.0f), msText);
        size      = addParameter ("size", "Size", 60.0f, { 5.0f, 100.0f }, percentText);
        decay     = addParameter ("decay", "Decay", 2.2f, skewed (0.2f, 30.0f, 3.0f),
                                  [] (float v) { return juce::String (v, v < 10 ? 2 : 1) + " s"; });
        damping   = addParameter ("damping", "High Freq", 5000.0f, freqRange (1000.0f, 20000.0f), hzText);
        diffusion = addParameter ("diffusion", "Diffusion", 75.0f, { 0.0f, 100.0f }, percentText);
        early     = addParameter ("early", "Early", 50.0f, { 0.0f, 100.0f }, percentText);
        modDepth  = addParameter ("mod", "Motion", 30.0f, { 0.0f, 100.0f }, percentText);
        lowCut    = addParameter ("lowcut", "Low Cut", 100.0f, freqRange (20.0f, 1000.0f), hzText);
        highCut   = addParameter ("highcut", "High Cut", 12000.0f, freqRange (1000.0f, 20000.0f), hzText);
        width     = addParameter ("width", "Width", 100.0f, { 0.0f, 150.0f }, percentText);
        mix       = addParameter ("mix", "Dry/Wet", 30.0f, { 0.0f, 100.0f }, percentText);
        freeze    = addParameter ("freeze", "Freeze", 0.0f, stepped (0, 1), onOffText);

        space      = addParameter ("space", "Space", (float) hall, stepped (0, numSpaces - 1),
                                   [] (float v) { return spaceNames()[juce::jlimit (0, numSpaces - 1, juce::roundToInt (v))]; });
        lowDecay   = addParameter ("lowdecay", "Low Decay", 1.2f, skewed (0.25f, 2.5f, 1.0f), multText);
        highDecay  = addParameter ("highdecay", "High Decay", 0.5f, skewed (0.1f, 1.5f, 0.5f), multText);
        shimmer    = addParameter ("shimmer", "Shimmer", 0.0f, { 0.0f, 100.0f }, percentText);
        shimmerInterval = addParameter ("shimmerpitch", "Shimmer Pitch", 2.0f, stepped (0, 3),
                                        [] (float v) { return shimmerNames()[juce::jlimit (0, 3, juce::roundToInt (v))]; });
        duck       = addParameter ("duck", "Duck", 0.0f, { 0.0f, 100.0f }, percentText);
        sync       = addParameter ("sync", "Predelay Sync", 0.0f, stepped (0, 5),
                                   [] (float v) { return syncNames()[juce::jlimit (0, 5, juce::roundToInt (v))]; });
    }

    juce::String getName() const override              { return "Elsewhere Reverb"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Reverb"; }
    int getDeviceViewWidth() const override            { return 900; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    std::unique_ptr<juce::Component> createWindowView() override;
    juce::Point<int> getWindowViewSize() const override    { return { 1040, 620 }; }

    double getTailLength() const override               { return decay.get() * juce::jmax (1.0f, lowDecay.get()) * 1.5 + 0.3; }
    bool noTail() override                              { return false; }
    bool producesAudioWhenNoAudioInput() override       { return true; }

    //==============================================================================
    /** How each space shapes the network. */
    struct SpaceShape
    {
        float lengthScale;      // delay line lengths (room size)
        float earlyGain;        // early reflections
        float earlySpread;      // early reflection timing
        float diffusionBoost;   // extra input diffusion
        float density;          // in-loop allpass gain
        float modRate;          // Hz
        float swell;            // Bloom: slow build-up of the tail
        float level;            // output trim, so spaces sit at a similar loudness
    };

    static SpaceShape shapeFor (int s)
    {
        switch (s)
        {
            case room:      return { 0.55f, 1.0f, 0.6f, 0.0f, 0.45f, 0.9f, 0.0f, 0.85f };
            case chamber:   return { 0.80f, 0.8f, 0.9f, 0.1f, 0.55f, 0.6f, 0.0f, 0.85f };
            case plate:     return { 0.70f, 0.0f, 0.5f, 0.3f, 0.70f, 1.1f, 0.0f, 0.80f };
            case cathedral: return { 1.40f, 0.6f, 1.8f, 0.1f, 0.60f, 0.35f, 0.0f, 0.95f };
            case bloom:     return { 1.60f, 0.0f, 1.0f, 0.3f, 0.65f, 0.25f, 1.0f, 1.05f };
            case hall:
            default:        return { 1.00f, 0.5f, 1.0f, 0.0f, 0.55f, 0.7f, 0.0f, 0.90f };
        }
    }

    /** Predelay in ms, following the song tempo when Sync is on. */
    float effectivePredelayMs (double bpm) const
    {
        const int s = juce::roundToInt (sync.get());
        if (s <= 0 || bpm <= 0.0)
            return predelay.get();

        static constexpr float beats[] = { 0.0f, 1.0f / 16.0f, 1.0f / 8.0f, 0.25f, 0.5f, 1.0f };
        return juce::jmin (maxPredelayMs, (float) (beats[juce::jlimit (1, 5, s)] * 60000.0 / bpm));
    }

    float getLastTempo() const      { return lastBpm.load(); }

    // For the display (written by the audio thread)
    std::atomic<float> inputLevel { 0.0f }, tailLevel { 0.0f }, duckGain { 1.0f };

    //==============================================================================
    void initialise (const te::PluginInitialisationInfo& info) override
    {
        // The engine can re-initialise on the message thread while audio still runs through the
        // old graph (e.g. when the device's block size changes): never reshape lines under it
        const juce::SpinLock::ScopedLockType sl (dspLock);
        rate = info.sampleRate;
        const auto scale = rate / 44100.0;
        tempoPosition = std::make_unique<te::tempo::Sequence::Position> (te::createPosition (edit.tempoSequence));

        for (int i = 0; i < numLines; ++i)
        {
            lines[(size_t) i].setSize ((int) (baseLengths[i] * 1.7 * 2.0 * scale) + 64);
            loopAllpass[(size_t) i].setSize ((int) (allpassLengths[i] * 1.7 * 1.2 * scale) + 64);
        }

        for (int i = 0; i < numDiffusers; ++i)
            for (int ch = 0; ch < 2; ++ch)
                diffusers[(size_t) ch][(size_t) i].setSize ((int) (diffuserLengths[i] * (ch == 0 ? 1.0 : 1.13) * scale) + 4);

        for (int i = 0; i < numSwell; ++i)
            swell[(size_t) i].setSize ((int) (swellLengths[i] * scale) + 4);

        predelayLine.setSize ((int) (maxPredelayMs * 0.001 * rate) + 8);
        erLine.setSize ((int) (0.2 * rate) + 4);
        shimmerLine.setSize ((int) (0.1 * rate) + 8);

        clearState();
    }

    void deinitialise() override
    {
        const juce::SpinLock::ScopedLockType sl (dspLock);
        tempoPosition.reset();
    }

    void reset() override
    {
        const juce::SpinLock::ScopedLockType sl (dspLock);
        clearState();
    }

    void clearState()
    {
        for (auto& l : lines) l.clear();
        for (auto& l : loopAllpass) l.clear();
        for (auto& side : diffusers) for (auto& d : side) d.clear();
        for (auto& s : swell) s.clear();
        predelayLine.clear(); erLine.clear(); shimmerLine.clear();
        bandLow.fill (0.0f); bandHigh.fill (0.0f);
        lowCutState.fill (0.0f); highCutState.fill (0.0f);
        shimmerLp = shimmerPhase = tailMs = shiftMs = lastBlockTailMs = outEnergy = fbEnergy = 0.0f;
        governor = 1.0f;
        lfoPhase = 0.0;
        env = duckEnv = tail = 0.0f;
        duckSmoothed = 1.0f;
        smoothedPredelay = -1.0f;
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr)
            return;

        juce::ScopedNoDenormals noDenormals;
        const juce::SpinLock::ScopedTryLockType sl (dspLock);
        if (! sl.isLocked() || lines[0].data.empty())
            return;                                                 // being re-initialised: pass the signal through this block

        auto& buf = *fc.destBuffer;
        const int numCh = juce::jmin (2, buf.getNumChannels());
        if (numCh == 0) return;

        double bpm = 120.0;
        if (tempoPosition != nullptr)
        {
            tempoPosition->set (fc.editTime.getStart());
            bpm = tempoPosition->getTempo();
        }
        lastBpm = (float) bpm;

        const auto shape = shapeFor (juce::roundToInt (space.get()));
        const float sizeScale = (0.25f + size.get() / 100.0f * 1.5f) * shape.lengthScale;
        const float sr = (float) rate;
        const bool frozen = freeze.get() > 0.5f;
        const float t60 = decay.get();

        // Per-band decay: gain per pass so each band falls 60 dB in its own decay time
        float lineLen[numLines], apLen[numLines], gLow[numLines], gMid[numLines], gHigh[numLines];
        for (int i = 0; i < numLines; ++i)
        {
            lineLen[i] = (float) baseLengths[i] * sizeScale * sr / 44100.0f;
            apLen[i] = (float) allpassLengths[i] * sizeScale * sr / 44100.0f;
            const float passSec = (lineLen[i] + apLen[i]) / sr;
            auto gainFor = [&] (float t) { return frozen ? 1.0f : std::pow (10.0f, -3.0f * passSec / juce::jmax (0.05f, t)); };
            gLow[i] = gainFor (t60 * lowDecay.get());
            gMid[i] = gainFor (t60);
            gHigh[i] = gainFor (t60 * highDecay.get());
        }

        const float lowXover = std::exp (-juce::MathConstants<float>::twoPi * 250.0f / sr);
        const float highXover = std::exp (-juce::MathConstants<float>::twoPi * juce::jmax (400.0f, damping.get()) / sr);
        const float lcCoeff = std::exp (-juce::MathConstants<float>::twoPi * lowCut.get() / sr);
        const float hcCoeff = std::exp (-juce::MathConstants<float>::twoPi * highCut.get() / sr);
        const float diffGain = juce::jmin (0.78f, diffusion.get() / 100.0f * 0.7f + shape.diffusionBoost * 0.25f);
        const float density = shape.density * (0.4f + 0.6f * diffusion.get() / 100.0f);
        const float erGain = early.get() / 100.0f * shape.earlyGain;
        const float motion = modDepth.get() / 100.0f;
        const float modAmt = motion * 10.0f * sr / 44100.0f;
        const float wet = mix.get() / 100.0f;
        const float dryGain = std::cos (wet * juce::MathConstants<float>::halfPi);
        const float wetGain = std::sin (wet * juce::MathConstants<float>::halfPi) * shape.level;
        const float w = width.get() / 100.0f;
        const double lfoInc = shape.modRate * (0.5 + motion) / rate;

        const float targetPredelay = effectivePredelayMs (bpm) * 0.001f * sr;
        if (smoothedPredelay < 0.0f) smoothedPredelay = targetPredelay;

        // Shimmer: pitch ratio and how much of the tail is fed back, pitched
        static constexpr float semis[] = { -12.0f, 7.0f, 12.0f, 19.0f };
        const float ratio = std::pow (2.0f, semis[juce::jlimit (0, 3, juce::roundToInt (shimmerInterval.get()))] / 12.0f);
        // (per pass through the ~30 ms network; small, so each octave lasts a while before the next)
        const float shimmerMix = frozen ? 0.0f : shimmer.get() / 100.0f * 0.06f;
        const float shimmerWindow = 0.06f * sr;
        const float shimmerStep = (1.0f - ratio) / shimmerWindow;   // phase advance per sample
        const float rmsCoeff = 1.0f - std::exp (-1.0f / (0.05f * sr));
        const float shimmerLpCoeff = std::exp (-juce::MathConstants<float>::twoPi * 7000.0f / sr);
        const float energyCoeff = 1.0f - std::exp (-1.0f / (0.004f * sr));

        // Ducking: follows the dry input
        const float duckAmt = duck.get() / 100.0f;
        const float envAttack = 1.0f - std::exp (-1.0f / (0.005f * sr));
        const float envRelease = 1.0f - std::exp (-1.0f / (0.25f * sr));
        const float duckSmooth = 1.0f - std::exp (-1.0f / (0.02f * sr));
        const float meterRelease = 1.0f - std::exp (-1.0f / (0.12f * sr));

        float peakIn = 0.0f;

        for (int n = 0; n < fc.bufferNumSamples; ++n)
        {
            const int idx = fc.bufferStartSample + n;
            const float inL = buf.getSample (0, idx);
            const float inR = numCh > 1 ? buf.getSample (1, idx) : inL;
            const float mono = (inL + inR) * 0.5f;
            float in = frozen ? 0.0f : mono;

            // Envelopes: display level and ducking
            const float a = std::abs (mono);
            peakIn = juce::jmax (peakIn, a);
            env += (a > env ? envAttack : envRelease) * (a - env);
            duckEnv = env;

            // Predelay (glides when it changes, e.g. with tempo)
            smoothedPredelay += (targetPredelay - smoothedPredelay) * 0.0005f;
            predelayLine.push (in);
            float x = predelayLine.read (juce::jmax (1.0f, smoothedPredelay));

            // Input band-limiting (so low cut/high cut shape the whole tail)
            lowCutState[0] = lcCoeff * lowCutState[0] + (1.0f - lcCoeff) * x;
            x -= lowCutState[0];
            highCutState[0] = hcCoeff * highCutState[0] + (1.0f - hcCoeff) * x;
            x = highCutState[0];

            // Bloom: long, dense allpasses that smear the attack into a slow swell
            if (shape.swell > 0.0f)
                for (auto& s : swell)
                    x = s.allpass (x, 0.62f);

            // Shimmer: granular pitch shift of the tail. It replaces part of the tail's feedback
            // (rather than adding to it), so the energy stays the same however long the decay
            float shifted = 0.0f;
            {
                if (shimmerMix > 0.0f)
                {
                    shimmerPhase += shimmerStep;
                    if (shimmerPhase >= 1.0f) shimmerPhase -= 1.0f;
                    if (shimmerPhase < 0.0f) shimmerPhase += 1.0f;
                    const float p2 = shimmerPhase + 0.5f >= 1.0f ? shimmerPhase - 0.5f : shimmerPhase + 0.5f;
                    auto tap = [&] (float ph) { return shimmerLine.read (2.0f + ph * shimmerWindow) * std::sin (ph * juce::MathConstants<float>::pi); };
                    shifted = (tap (shimmerPhase) + tap (p2)) * 0.75f;
                    shimmerLp = shimmerLpCoeff * shimmerLp + (1.0f - shimmerLpCoeff) * shifted;
                    shifted = std::tanh (shimmerLp);
                }
            }

            // Early reflections: sparse taps, panned
            erLine.push (x);
            float erL = 0.0f, erR = 0.0f;
            if (erGain > 0.0f)
            {
                for (int t = 0; t < numErTaps; ++t)
                {
                    auto s = erLine.read (erTaps[t].timeMs * 0.001f * sr * shape.earlySpread * (0.5f + sizeScale * 0.5f)) * erTaps[t].gain;
                    erL += s * (1.0f - erTaps[t].pan);
                    erR += s * erTaps[t].pan;
                }
            }

            // Input diffusion (allpass chains, one per side for decorrelation)
            float dL = x, dR = x;
            for (int i = 0; i < numDiffusers; ++i)
            {
                dL = diffusers[0][(size_t) i].allpass (dL, diffGain);
                dR = diffusers[1][(size_t) i].allpass (dR, diffGain);
            }

            // FDN: read lines (modulated), densify, apply per-band decay, mix with Hadamard
            float out[numLines];
            lfoPhase += lfoInc;
            if (lfoPhase >= 1.0) lfoPhase -= 1.0;

            for (int i = 0; i < numLines; ++i)
            {
                auto& L = lines[(size_t) i];
                const auto phase = (float) lfoPhase + (float) i * 0.125f;
                const auto mod = modAmt * std::sin (juce::MathConstants<float>::twoPi * phase);
                float s = L.read (lineLen[i] + (i % 2 == 0 ? mod : -mod));

                // Modulated allpass inside the loop: thickens the tail into a smooth wash
                s = loopAllpass[(size_t) i].modulatedAllpass (s, density, apLen[i] + mod * 0.5f);

                // Three bands, each decaying at its own rate
                bandLow[(size_t) i] = lowXover * bandLow[(size_t) i] + (1.0f - lowXover) * s;
                bandHigh[(size_t) i] = highXover * bandHigh[(size_t) i] + (1.0f - highXover) * s;
                const float lo = bandLow[(size_t) i];
                const float mid = bandHigh[(size_t) i] - lo;
                const float hi = s - bandHigh[(size_t) i];
                out[i] = lo * gLow[i] + mid * gMid[i] + hi * gHigh[i];
            }

            hadamard8 (out);

            // Shimmer level matched to the tail it replaces (equal-power crossfade)
            float shimmerGain = 0.0f;
            if (shimmerMix > 0.0f)
            {
                float lineMs = 0.0f;
                for (int i = 0; i < numLines; ++i) lineMs += out[i] * out[i];
                lineMs /= (float) numLines;
                tailMs += (lineMs - tailMs) * rmsCoeff;
                shiftMs += (shifted * shifted - shiftMs) * rmsCoeff;
                shimmerGain = juce::jmin (3.0f, std::sqrt (tailMs / (shiftMs + 1.0e-12f))) * std::sqrt (shimmerMix) * governor;
            }
            const float keep = std::sqrt (1.0f - shimmerMix);

            // Shimmer may re-colour the tail (pitch it up or down) but never add energy to it: its
            // copies can line up with what's already ringing, and with input arriving all the time
            // (a return channel) that used to build up into a screech. The feedback is held to
            // the energy the decay allows, measured over a few milliseconds.
            float fb[numLines];
            float outE = 0.0f, fbE = 0.0f;
            for (int i = 0; i < numLines; ++i)
            {
                fb[i] = shimmerMix > 0.0f ? out[i] * keep + shifted * shimmerGain * ((i & 2) != 0 ? -1.0f : 1.0f) : out[i];
                outE += out[i] * out[i];
                fbE += fb[i] * fb[i];
            }
            if (shimmerMix > 0.0f)
            {
                outEnergy += (outE - outEnergy) * energyCoeff;
                fbEnergy += (fbE - fbEnergy) * energyCoeff;
                const float limit = fbEnergy > outEnergy && fbEnergy > 1.0e-12f ? std::sqrt (outEnergy / fbEnergy) : 1.0f;
                for (auto& v : fb) v *= limit;
            }

            // Self-healing: nothing in the loop should ever be this loud or not a number
            bool broken = false;
            for (int i = 0; i < numLines; ++i)
                broken = broken || ! std::isfinite (fb[i]) || std::abs (fb[i]) > 64.0f;
            if (broken)
            {
                clearState();
                for (auto& v : fb) v = 0.0f;
                for (auto& v : out) v = 0.0f;
            }

            for (int i = 0; i < numLines; ++i)
                lines[(size_t) i].push (fb[i] + (i % 2 == 0 ? dL : dR) * 0.35f);

            const float tailL = (out[0] + out[2] + out[4] + out[6]) * 0.5f;
            const float tailR = (out[1] + out[3] + out[5] + out[7]) * 0.5f;
            shimmerLine.push ((tailL + tailR) * 0.5f);

            float revL = tailL + erL * erGain * 0.6f;
            float revR = tailR + erR * erGain * 0.6f;

            // Width (mid/side)
            const float m = (revL + revR) * 0.5f, side = (revL - revR) * 0.5f * w;
            revL = m + side;
            revR = m - side;

            // Ducking: up to ~24 dB while the input is loud
            const float duckDb = juce::jlimit (0.0f, 1.0f, (juce::Decibels::gainToDecibels (duckEnv, -100.0f) + 42.0f) / 30.0f);
            const float duckTarget = 1.0f - duckAmt * duckDb * 0.94f;
            duckSmoothed += (duckTarget - duckSmoothed) * duckSmooth;

            const float tailAbs = 0.5f * (std::abs (revL) + std::abs (revR));
            tail += (tailAbs > tail ? 0.05f : meterRelease) * (tailAbs - tail);

            buf.setSample (0, idx, inL * dryGain + revL * wetGain * duckSmoothed);
            if (numCh > 1)
                buf.setSample (1, idx, inR * dryGain + revR * wetGain * duckSmoothed);
        }

        // Shimmer governor: pitched copies can line up with what's already in the tail and add up.
        // Without input, the tail must fade at least half as fast (in dB) as Decay says; if it
        // doesn't, shimmer is turned down until it does
        if (shimmerMix > 0.0f)
        {
            const float blockSec = (float) fc.bufferNumSamples / sr;
            const float expected = std::pow (10.0f, -6.0f * blockSec / juce::jmax (0.2f, t60));
            const bool quiet = env * env < tailMs * 0.05f;
            if (quiet && lastBlockTailMs > 1.0e-12f && tailMs > lastBlockTailMs * std::sqrt (expected))
                governor = juce::jmax (0.0f, governor * 0.8f);
            else
                governor = juce::jmin (1.0f, governor * 1.01f + 0.001f);
            lastBlockTailMs = tailMs;
        }

        inputLevel = juce::jmax (peakIn, inputLevel.load() * 0.8f);
        tailLevel = tail;
        duckGain = duckSmoothed;
    }

    Param predelay, size, decay, damping, diffusion, early, modDepth, lowCut, highCut, width, mix, freeze,
          space, lowDecay, highDecay, shimmer, shimmerInterval, duck, sync;

    static constexpr float maxPredelayMs = 1500.0f;

private:
    static juce::String multText (float v)  { return juce::String::fromUTF8 ("\xc3\x97") + juce::String (v, 2); }

    static constexpr int numLines = 8, numDiffusers = 4, numErTaps = 8, numSwell = 4;
    static constexpr int baseLengths[numLines] = { 1557, 1617, 1491, 1422, 1277, 1356, 1188, 1116 };   // prime-ish, ~25-35 ms
    static constexpr int allpassLengths[numLines] = { 223, 271, 197, 307, 241, 179, 293, 211 };
    static constexpr int diffuserLengths[numDiffusers] = { 142, 107, 379, 277 };
    static constexpr int swellLengths[numSwell] = { 2203, 3547, 4793, 6311 };

    struct ErTap { float timeMs, gain, pan; };
    static constexpr ErTap erTaps[numErTaps] = {
        { 4.3f, 0.84f, 0.2f }, { 7.9f, 0.72f, 0.85f }, { 12.1f, 0.63f, 0.35f }, { 17.3f, 0.55f, 0.7f },
        { 23.9f, 0.46f, 0.1f }, { 31.7f, 0.38f, 0.95f }, { 41.2f, 0.3f, 0.45f }, { 53.0f, 0.22f, 0.6f } };

    struct DelayLine
    {
        std::vector<float> data;
        int pos = 0;

        void setSize (int n)    { data.assign ((size_t) juce::jmax (4, n), 0.0f); pos = 0; }
        void clear()            { std::fill (data.begin(), data.end(), 0.0f); pos = 0; }
        void push (float x)     { data[(size_t) pos] = x; if (++pos >= (int) data.size()) pos = 0; }

        /** Reads `delay` samples back (fractional, linear interpolation). */
        float read (float delay) const
        {
            const int len = (int) data.size();
            delay = juce::jlimit (1.0f, (float) (len - 2), delay);
            float rp = (float) pos - delay;
            if (rp < 0) rp += (float) len;
            if (rp >= (float) len) rp = 0.0f;   // (a tiny negative plus len rounds up to len itself)
            int i0 = (int) rp;
            float frac = rp - (float) i0;
            int i1 = i0 + 1 >= len ? 0 : i0 + 1;
            return data[(size_t) i0] + frac * (data[(size_t) i1] - data[(size_t) i0]);
        }

        /** Schroeder allpass using the whole buffer as the delay. */
        float allpass (float x, float g)
        {
            float delayed = data[(size_t) pos];
            float v = x + g * delayed;
            data[(size_t) pos] = v;
            if (++pos >= (int) data.size()) pos = 0;
            return delayed - g * v;
        }

        /** Allpass with a fractional (modulated) delay. */
        float modulatedAllpass (float x, float g, float delay)
        {
            float delayed = read (delay);
            float v = x + g * delayed;
            push (v);
            return delayed - g * v;
        }
    };

    static void hadamard8 (float* x)
    {
        for (int h = 1; h < 8; h *= 2)
            for (int i = 0; i < 8; i += h * 2)
                for (int j = i; j < i + h; ++j)
                {
                    float a = x[j], b = x[j + h];
                    x[j] = a + b; x[j + h] = a - b;
                }

        const float norm = 1.0f / std::sqrt (8.0f);
        for (int i = 0; i < 8; ++i) x[i] *= norm;
    }

    double rate = 44100.0, lfoPhase = 0.0;
    std::unique_ptr<te::tempo::Sequence::Position> tempoPosition;
    std::atomic<float> lastBpm { 120.0f };
    std::array<DelayLine, numLines> lines, loopAllpass;
    std::array<std::array<DelayLine, numDiffusers>, 2> diffusers;
    std::array<DelayLine, numSwell> swell;
    DelayLine predelayLine, erLine, shimmerLine;
    std::array<float, numLines> bandLow {}, bandHigh {};
    std::array<float, 2> lowCutState {}, highCutState {};
    float shimmerLp = 0.0f, shimmerPhase = 0.0f, env = 0.0f, duckEnv = 0.0f, duckSmoothed = 1.0f, tail = 0.0f;
    float smoothedPredelay = -1.0f, tailMs = 0.0f, shiftMs = 0.0f, governor = 1.0f, lastBlockTailMs = 0.0f;
    float outEnergy = 0.0f, fbEnergy = 0.0f;
    juce::SpinLock dspLock;
};

//==============================================================================
/**
    The reverb's "space": a flat, live picture of the sound.

    The space is drawn as a field of dots inside its walls (each space has its own shape:
    Room a rounded square, Hall a wide rectangle, Plate a sharp one, Cathedral an arch, Bloom
    a circle). Sound enters at the source and spreads out as energy that bounces off the walls
    and fades at the decay time of its band. Dots light up where the energy is, in the colour
    of the band carrying it: orange lows, violet mids, cyan highs, so the decay EQ is
    something you can see. Early reflections flash near the source, Diffusion turns clean
    bounces into a scattered spread, Motion makes the energy swirl, Shimmer lifts sparks
    upward, Freeze holds everything in orbit, and Ducking dims it while the input plays.

    Below, in its own strip, is the decay curve (decay time across frequency). Drag its
    points: Low sets the low decay, the middle sets the decay time, and High sets the high
    frequency and its decay.

    Everything is flat: solid dots, lines and fills, no glows or gradients.
*/
class ReverbSpace : public juce::Component, private juce::Timer
{
public:
    /** `inWindow`: drawn large, so the dots and the decay curve grow with the space given. */
    ReverbSpace (ReverbDevice& d, bool inWindow = false) : rev (d), roomy (inWindow)
    {
        lastParams = readParams();
        startTimerHz (45);
    }

    void resized() override
    {
        layoutGrid();

        // Warm start: one burst, a little way in, so there's something to see straight away
        if (! warmedUp && getWidth() > 0)
        {
            warmedUp = true;
            trigger (1.0f, false);
            for (int i = 0; i < 22; ++i)
                step (1.0f / 45.0f);
        }
    }

    void paint (juce::Graphics& g) override
    {
        const bool light = Theme::isLight();
        const auto ink = light ? Colours::text : juce::Colours::white;
        g.setColour (Colours::display);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 5.0f);

        auto field = fieldArea();
        const int sp = spaceIndex();

        // Walls: the outline and the inner rings are all the same thin line
        const int rings = 2 + juce::roundToInt (rev.diffusion.get() / 100.0f * 3.0f);
        g.setColour (ink.withAlpha (light ? 0.16f : 0.14f));
        for (int i = 1; i <= rings; ++i)
            g.strokePath (roomPath ((float) i / (float) rings), juce::PathStrokeType (1.0f));

        // Dots: a faint grid inside the space; lit dots where the sound is
        const float spacing = gridSpacing;
        const float baseR = 0.1f * spacing;
        for (size_t i = 0; i < cells.size(); ++i)
        {
            const auto& c = cells[i];

            const float e = juce::jmin (1.0f, energy[i]);
            if (e < 0.05f)
            {
                // Resting dots: the whole field is the largest room; the current space is a little brighter
                g.setColour (ink.withAlpha (c.inside ? (light ? 0.16f : 0.15f) : (light ? 0.07f : 0.06f)));
                g.fillEllipse (c.pos.x - baseR, c.pos.y - baseR, baseR * 2.0f, baseR * 2.0f);
                continue;
            }

            const float r = baseR + (spacing * 0.36f - baseR) * std::sqrt (e);
            g.setColour (bandColour[i].withAlpha (juce::jlimit (0.15f, 1.0f, 0.1f + e * 1.1f)));
            g.fillEllipse (c.pos.x - r, c.pos.y - r, r * 2.0f, r * 2.0f);
        }

        // The source
        auto src = centre();
        g.setColour (Colours::accent);
        g.fillEllipse (juce::Rectangle<float> (7.0f, 7.0f).withCentre (src));

        // Labels
        g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
        auto top = getLocalBounds().toFloat().reduced (10.0f, 6.0f).removeFromTop (12.0f);
        g.setColour (Colours::textDim);
        g.drawText (ReverbDevice::spaceNames()[sp].toUpperCase(), top, juce::Justification::centredLeft);

        juce::String right = rev.freeze.get() > 0.5f ? juce::String ("FROZEN") : rev.decay.param().getCurrentValueAsString();
        if (rev.shimmer.get() > 0.5f)
            right = "SHIMMER " + rev.shimmerInterval.param().getCurrentValueAsString() + "   " + right;
        g.setColour (rev.freeze.get() > 0.5f ? Theme::readable (Colours::solo) : Colours::textDim);
        g.drawText (right, top, juce::Justification::centredRight);

        // Ducking: a thin bar on the right edge of the field
        const float duckDb = juce::Decibels::gainToDecibels (rev.duckGain.load(), -40.0f);
        if (rev.duck.get() > 0.5f && duckDb < -0.5f)
        {
            auto bar = juce::Rectangle<float> (field.getRight() + 3.0f, field.getY(), 2.0f, field.getHeight() * juce::jmin (1.0f, -duckDb / 24.0f));
            g.setColour (Colours::accent);
            g.fillRect (bar);
        }

        paintCurve (g, ink);
    }

    //==============================================================================
    void mouseMove (const juce::MouseEvent& e) override
    {
        auto h = hitNode (e.position);
        if (h != hover) { hover = h; repaint(); }
        setMouseCursor (h >= 0 ? juce::MouseCursor::DraggingHandCursor : juce::MouseCursor::NormalCursor);
    }

    void mouseExit (const juce::MouseEvent&) override   { hover = -1; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = hitNode (e.position);
        for (auto* p : paramsFor (dragging))
            p->parameterChangeGestureBegin();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;

        auto curve = curveArea();
        auto yToMult = [&] (float y) { return std::pow (2.0f, juce::jmap (y, curve.getBottom(), curve.getY(), -2.0f, 1.3f)); };

        if (dragging == 0)
            rev.lowDecay.param().setParameter (juce::jlimit (0.25f, 2.5f, yToMult (e.position.y)), juce::sendNotification);
        else if (dragging == 1)
            rev.decay.param().setParameter (juce::jlimit (0.2f, 30.0f, 2.2f * yToMult (e.position.y) * 1.5f), juce::sendNotification);
        else if (dragging == 2)
        {
            rev.highDecay.param().setParameter (juce::jlimit (0.1f, 1.5f, yToMult (e.position.y)), juce::sendNotification);
            rev.damping.param().setParameter (juce::jlimit (1000.0f, 20000.0f, xToFreq (e.position.x, curve)), juce::sendNotification);
        }
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        for (auto* p : paramsFor (dragging))
            p->parameterChangeGestureEnd();
        dragging = -1;
    }

private:
    struct Particle
    {
        juce::Point<float> pos, vel;    // in "space units": the walls are at norm 1
        float age = 0, life = 1, weight = 1, twinkle = 0;
        int kind = 0;                   // 0 low, 1 mid, 2 high, 3 early reflection, 4 shimmer
    };

    struct Pending { float at, strength; bool ping; };
    struct Cell { juce::Point<float> pos; bool inside = false; };

    ReverbDevice& rev;
    const bool roomy;
    std::vector<Particle> particles;
    std::vector<Pending> pending;
    std::vector<Cell> cells;
    std::vector<float> energy;
    std::vector<juce::Colour> bandColour;
    std::vector<std::array<float, 5>> bandEnergy;
    int gridCols = 0, gridRows = 0;
    float gridSpacing = 9.0f;
    juce::Point<float> gridOrigin;
    juce::Random rng { 7 };
    float clock = 0.0f, lastInput = 0.0f, idleTime = 0.0f, pingCooldown = 0.0f;
    std::array<float, 12> lastParams {};
    int hover = -1, dragging = -1, gridSpace = -1;
    float gridSize = -1.0f, gridWidth = -1.0f;
    bool warmedUp = false;

    static constexpr int maxParticles = 700;

    //==============================================================================
    std::array<float, 12> readParams() const
    {
        return { rev.size.get(), rev.decay.get(), rev.predelay.get(), rev.diffusion.get(), rev.early.get(), rev.modDepth.get(),
                 rev.width.get(), rev.lowDecay.get(), rev.highDecay.get(), rev.damping.get(), rev.shimmer.get(), rev.space.get() };
    }

    std::vector<te::AutomatableParameter*> paramsFor (int node)
    {
        if (node == 0) return { &rev.lowDecay.param() };
        if (node == 1) return { &rev.decay.param() };
        if (node == 2) return { &rev.highDecay.param(), &rev.damping.param() };
        return {};
    }

    static juce::Colour colourFor (int kind)
    {
        switch (kind)
        {
            case 0:  return Colours::accent;
            case 1:  return Colours::clips[5];
            case 2:  return Colours::solo;
            case 3:  return Theme::isLight() ? Colours::text : juce::Colours::white;
            default: return Colours::clips[6];
        }
    }

    //==============================================================================
    // Geometry
    int spaceIndex() const                      { return juce::jlimit (0, ReverbDevice::numSpaces - 1, juce::roundToInt (rev.space.get())); }

    juce::Rectangle<float> curveArea() const
    {
        auto r = getLocalBounds().toFloat().reduced (10.0f, 6.0f);
        return r.removeFromBottom (juce::jmin (roomy ? 84.0f : 30.0f, r.getHeight() * 0.22f));
    }

    /** The largest possible room: everything above the decay curve, edge to edge. */
    juce::Rectangle<float> fieldArea() const
    {
        auto r = getLocalBounds().toFloat().reduced (5.0f);
        r.removeFromBottom (curveArea().getHeight() + 12.0f);
        return r;
    }

    juce::Point<float> centre() const           { return fieldArea().getCentre(); }

    juce::Point<float> radii() const
    {
        auto f = fieldArea();
        const float s = 0.35f + rev.size.get() / 100.0f * 0.65f;
        const float spaceScale = juce::jlimit (0.7f, 1.0f, ReverbDevice::shapeFor (spaceIndex()).lengthScale + 0.15f);
        const float wx = 0.7f + rev.width.get() / 150.0f * 0.6f;
        const int sp = spaceIndex();
        const float aspect = sp == ReverbDevice::room ? 1.0f : sp == ReverbDevice::bloom ? 1.15f : sp == ReverbDevice::cathedral ? 0.95f : 1.9f;
        const float ry = f.getHeight() * 0.5f * s * spaceScale;
        return { juce::jmin (f.getWidth() * 0.5f, ry * aspect * wx * 1.25f), juce::jmin (f.getHeight() * 0.5f, ry) };
    }

    /** How "square" the walls are, for bouncing: 2 is an ellipse, higher is boxier. */
    float wallPower() const
    {
        switch (spaceIndex())
        {
            case ReverbDevice::bloom:     return 2.0f;
            case ReverbDevice::room:      return 3.5f;
            case ReverbDevice::cathedral: return 3.0f;
            case ReverbDevice::plate:     return 12.0f;
            default:                      return 6.0f;
        }
    }

    /** The walls, scaled towards the source by k (1 = the outer wall). */
    juce::Path roomPath (float k) const
    {
        auto c = centre(); auto r = radii();
        auto rect = juce::Rectangle<float> (r.x * 2.0f * k, r.y * 2.0f * k).withCentre (c);
        juce::Path p;

        switch (spaceIndex())
        {
            case ReverbDevice::bloom:       p.addEllipse (rect); break;
            case ReverbDevice::plate:       p.addRectangle (rect); break;
            case ReverbDevice::room:        p.addRoundedRectangle (rect, rect.getHeight() * 0.22f); break;
            case ReverbDevice::chamber:     p.addRoundedRectangle (rect, rect.getHeight() * 0.12f); break;
            case ReverbDevice::cathedral:
            {
                const float arch = rect.getWidth() * 0.5f;
                p.addRoundedRectangle (rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight(), arch, juce::jmin (arch, rect.getHeight() * 0.55f),
                                       true, true, false, false);
                break;
            }
            case ReverbDevice::hall:
            default:                        p.addRoundedRectangle (rect, 3.0f); break;
        }
        return p;
    }

    juce::Point<float> toScreen (juce::Point<float> p) const
    {
        auto c = centre(); auto r = radii();
        return { c.x + p.x * r.x, c.y + p.y * r.y };
    }

    /** Rebuilds the dot grid when the space's shape or size changes. */
    void layoutGrid()
    {
        auto f = fieldArea();
        // (a bigger picture gets bigger dots, not a great many more of them)
        gridSpacing = roomy ? juce::jlimit (9.0f, 20.0f, f.getHeight() / 22.0f) : 9.0f;
        gridCols = juce::jmax (1, (int) (f.getWidth() / gridSpacing));
        gridRows = juce::jmax (1, (int) (f.getHeight() / gridSpacing));
        gridOrigin = { f.getCentreX() - (float) (gridCols - 1) * gridSpacing * 0.5f, f.getCentreY() - (float) (gridRows - 1) * gridSpacing * 0.5f };

        const auto walls = roomPath (1.0f);
        cells.assign ((size_t) (gridCols * gridRows), {});
        for (int y = 0; y < gridRows; ++y)
            for (int x = 0; x < gridCols; ++x)
            {
                auto& c = cells[(size_t) (y * gridCols + x)];
                c.pos = { gridOrigin.x + (float) x * gridSpacing, gridOrigin.y + (float) y * gridSpacing };
                c.inside = walls.contains (c.pos);   // inside the current space (the field is the largest one)
            }

        energy.assign (cells.size(), 0.0f);
        bandEnergy.assign (cells.size(), {});
        bandColour.assign (cells.size(), Colours::accent);
        gridSpace = spaceIndex();
        gridSize = rev.size.get();
        gridWidth = rev.width.get();
    }

    //==============================================================================
    void timerCallback() override
    {
        if (! isShowing())
            return;

        step (1.0f / 45.0f);
        repaint();
    }

    /** Sends a burst of sound into the space. `ping` is a softer one from a parameter change. */
    void trigger (float strength, bool ping)
    {
        const bool synced = juce::roundToInt (rev.sync.get()) > 0;
        const float pre = (synced ? rev.effectivePredelayMs (rev.getLastTempo()) : rev.predelay.get()) * 0.001f;
        pending.push_back ({ clock + juce::jmin (0.8f, pre), strength, ping });
    }

    void spawn (float strength, bool ping)
    {
        const int sp = spaceIndex();
        const auto shape = ReverbDevice::shapeFor (sp);
        const float decay = rev.decay.get();
        const float diffusion = rev.diffusion.get() / 100.0f;
        const float earlyAmt = rev.early.get() / 100.0f * shape.earlyGain;
        const float shimmerAmt = rev.shimmer.get() / 100.0f;
        const float speed = 1.3f / juce::jmax (0.3f, 0.35f + rev.size.get() / 100.0f * 0.9f);

        auto lifeFor = [&] (int kind)
        {
            float t = decay * (kind == 0 ? rev.lowDecay.get() : kind == 2 ? rev.highDecay.get() : 1.0f);
            return juce::jlimit (0.15f, 9.0f, t * 0.9f);
        };

        // Early reflections: sharp flashes at fixed angles, close to the source
        const int facets = (int) std::round (earlyAmt * 8.0f);
        for (int i = 0; i < facets && (int) particles.size() < maxParticles; ++i)
        {
            Particle p;
            p.kind = 3;
            const float a = juce::MathConstants<float>::twoPi * (0.07f + 0.123f * (float) i * 1.61f);
            const float d = 0.25f + 0.07f * (float) i * shape.earlySpread;
            p.pos = { std::cos (a) * d * 0.3f, std::sin (a) * d * 0.3f };
            p.vel = { std::cos (a) * d * 2.5f, std::sin (a) * d * 2.5f };
            p.life = 0.12f + 0.05f * (float) i;
            p.weight = 1.6f;
            particles.push_back (p);
        }

        const int count = juce::jlimit (4, 120, (int) (strength * (ping ? 45.0f : 110.0f)));
        for (int i = 0; i < count && (int) particles.size() < maxParticles; ++i)
        {
            Particle p;
            const float r = rng.nextFloat();
            p.kind = r < shimmerAmt * 0.35f ? 4 : r < 0.42f ? 0 : r < 0.75f ? 1 : 2;

            float a = rng.nextFloat() * juce::MathConstants<float>::twoPi;
            if (sp == ReverbDevice::plate)
                a = juce::MathConstants<float>::halfPi * 0.5f + juce::MathConstants<float>::halfPi * (float) rng.nextInt (4)
                    + (rng.nextFloat() - 0.5f) * 0.12f;   // diagonals, like waves across a metal sheet

            const float v = speed * (0.55f + rng.nextFloat() * 0.7f) * (sp == ReverbDevice::bloom ? 0.35f : 1.0f);
            p.pos = { (rng.nextFloat() - 0.5f) * 0.06f, (rng.nextFloat() - 0.5f) * 0.06f };
            p.vel = { std::cos (a) * v, std::sin (a) * v };
            p.life = p.kind == 4 ? lifeFor (2) * 1.4f : lifeFor (p.kind);
            p.weight = (p.kind == 0 ? 1.2f : p.kind == 2 ? 0.8f : 1.0f) * (ping ? 0.75f : 1.0f);
            p.twinkle = rng.nextFloat() * 6.28f;
            p.age = -rng.nextFloat() * (0.04f + diffusion * 0.08f) * (sp == ReverbDevice::bloom ? 6.0f : 1.0f);
            particles.push_back (p);
        }
    }

    void step (float dt)
    {
        clock += dt;
        pingCooldown -= dt;

        if (gridSpace != spaceIndex() || std::abs (gridSize - rev.size.get()) > 0.01f || std::abs (gridWidth - rev.width.get()) > 0.01f)
            layoutGrid();

        // Input: bursts on attacks, a steady stream while sound is playing
        const float in = juce::jlimit (0.0f, 1.0f, rev.inputLevel.load() * 1.6f);
        if (in > lastInput * 1.4f + 0.04f)
            trigger (juce::jlimit (0.3f, 1.0f, in), false);
        else if (in > 0.02f)
            trigger (in * 0.12f, false);
        lastInput = in;

        // Parameter changes "ping" the space so you can feel what they do
        auto now = readParams();
        bool changed = false;
        for (size_t i = 0; i < now.size(); ++i)
            if (std::abs (now[i] - lastParams[i]) > 1.0e-4f * juce::jmax (1.0f, std::abs (lastParams[i])))
                changed = true;
        lastParams = now;

        if (changed && pingCooldown <= 0.0f)
        {
            trigger (0.7f, true);
            pingCooldown = 0.18f;
        }

        // Idle: a gentle pulse every few seconds, so the space is never empty
        idleTime = in > 0.02f || changed ? 0.0f : idleTime + dt;
        if (idleTime > 3.2f && rev.freeze.get() < 0.5f)
        {
            trigger (0.35f, true);
            idleTime = 0.0f;
        }

        for (size_t i = 0; i < pending.size();)
        {
            if (pending[i].at <= clock)
            {
                spawn (pending[i].strength, pending[i].ping);
                pending.erase (pending.begin() + (long) i);
            }
            else ++i;
        }

        simulate (dt);
        accumulate (dt);
    }

    void simulate (float dt)
    {
        const bool frozen = rev.freeze.get() > 0.5f;
        const float diffusion = rev.diffusion.get() / 100.0f;
        const float motion = rev.modDepth.get() / 100.0f;
        const float shimmerAmt = rev.shimmer.get() / 100.0f;
        const int sp = spaceIndex();
        const float spin = motion * 1.4f + (frozen ? 0.35f : 0.0f);
        const float pw = wallPower();

        for (auto& p : particles)
        {
            if (! frozen || p.kind == 3)
                p.age += dt;

            if (p.age < 0.0f)
                continue;

            // Motion: velocity slowly rotates, so the energy swirls
            if (spin > 0.0f)
            {
                const float ang = std::sin (clock * 1.7f + p.twinkle) * spin * dt * 2.2f;
                p.vel = { p.vel.x * std::cos (ang) - p.vel.y * std::sin (ang), p.vel.x * std::sin (ang) + p.vel.y * std::cos (ang) };
            }

            if (sp == ReverbDevice::bloom)
                p.vel *= 1.0f + dt * 0.6f;   // slow start, then the swell opens up

            if (p.kind == 4)
                p.vel.y -= dt * (0.25f + shimmerAmt * 0.6f);   // shimmer lifts upward

            if (frozen && p.kind != 3)
            {
                const juce::Point<float> tangent { -p.pos.y, p.pos.x };
                p.vel = p.vel * (1.0f - dt * 1.5f) + tangent * (dt * 1.6f);
            }

            p.pos += p.vel * dt;

            // Walls (a superellipse matching the space's shape): bounce back in;
            // diffusion scatters the bounce
            const float ax = std::abs (p.pos.x), ay = std::abs (p.pos.y);
            const float norm = std::pow (std::pow (ax, pw) + std::pow (ay, pw), 1.0f / pw);
            if (norm > 1.0f && p.kind != 3)
            {
                juce::Point<float> nrm { std::copysign (std::pow (ax, pw - 1.0f), p.pos.x), std::copysign (std::pow (ay, pw - 1.0f), p.pos.y) };
                nrm /= juce::jmax (1.0e-6f, nrm.getDistanceFromOrigin());
                const float dot = p.vel.x * nrm.x + p.vel.y * nrm.y;
                if (dot > 0.0f)
                {
                    p.vel -= nrm * (2.0f * dot);
                    const float scatter = (rng.nextFloat() - 0.5f) * diffusion * 2.4f;
                    p.vel = { p.vel.x * std::cos (scatter) - p.vel.y * std::sin (scatter), p.vel.x * std::sin (scatter) + p.vel.y * std::cos (scatter) };
                    p.vel *= 0.86f;
                }
                p.pos /= norm * 1.001f;
            }
        }

        particles.erase (std::remove_if (particles.begin(), particles.end(),
                                         [frozen] (const Particle& p) { return ! frozen && p.age > p.life; }),
                         particles.end());

        if (frozen && particles.size() > (size_t) maxParticles)
            particles.erase (particles.begin(), particles.begin() + (long) (particles.size() - maxParticles));
    }

    /** Spreads each particle's energy onto the nearest dots, then lets the dots relax. */
    void accumulate (float dt)
    {
        if (cells.empty())
            return;

        const float duck = juce::jlimit (0.15f, 1.0f, rev.duckGain.load());
        const float relax = std::exp (-dt * 14.0f);
        for (auto& b : bandEnergy)
            for (auto& v : b)
                v *= relax;

        for (auto& p : particles)
        {
            if (p.age < 0.0f)
                continue;

            float level = std::pow (10.0f, -3.0f * p.age / p.life);
            if (p.kind == 3) level = 1.0f - p.age / p.life;
            if (p.kind == 4) level *= 0.5f + 0.5f * std::sin (clock * 14.0f + p.twinkle);
            level = std::sqrt (juce::jmax (0.0f, level)) * p.weight * duck * 0.24f;
            if (level < 0.01f)
                continue;

            // Bilinear: share the energy between the four surrounding dots
            auto s = toScreen (p.pos);
            const float gx = (s.x - gridOrigin.x) / gridSpacing, gy = (s.y - gridOrigin.y) / gridSpacing;
            const int x0 = (int) std::floor (gx), y0 = (int) std::floor (gy);
            const float fx = gx - (float) x0, fy = gy - (float) y0;

            for (int dy = 0; dy <= 1; ++dy)
                for (int dx = 0; dx <= 1; ++dx)
                {
                    const int x = x0 + dx, y = y0 + dy;
                    if (x < 0 || y < 0 || x >= gridCols || y >= gridRows)
                        continue;
                    const float w = (dx ? fx : 1.0f - fx) * (dy ? fy : 1.0f - fy);
                    bandEnergy[(size_t) (y * gridCols + x)][(size_t) p.kind] += level * w;
                }
        }

        // Each dot shows its total energy, in the colour of the band carrying most of it
        for (size_t i = 0; i < cells.size(); ++i)
        {
            const auto& b = bandEnergy[i];
            float total = 0.0f, best = 0.0f;
            int bestKind = 0;
            for (int k = 0; k < 5; ++k)
            {
                total += b[(size_t) k];
                if (b[(size_t) k] > best) { best = b[(size_t) k]; bestKind = k; }
            }
            energy[i] = total;
            bandColour[i] = colourFor (bestKind);
        }
    }

    //==============================================================================
    // The decay curve
    static float freqToX (float f, juce::Rectangle<float> r)    { return r.getX() + r.getWidth() * std::log (f / 20.0f) / std::log (1000.0f); }
    static float xToFreq (float x, juce::Rectangle<float> r)    { return 20.0f * std::pow (1000.0f, juce::jlimit (0.0f, 1.0f, (x - r.getX()) / r.getWidth())); }

    float multToY (float m, juce::Rectangle<float> r) const
    {
        return juce::jmap (juce::jlimit (-2.0f, 1.3f, std::log2 (juce::jmax (0.01f, m))), -2.0f, 1.3f, r.getBottom(), r.getY());
    }

    /** Decay at each frequency, relative to a 2.2 s reference (so the middle node moves with Decay). */
    std::array<juce::Point<float>, 3> nodes() const
    {
        auto r = curveArea();
        const float midMult = rev.decay.get() / (2.2f * 1.5f);
        return { juce::Point<float> { freqToX (120.0f, r), multToY (rev.lowDecay.get() * midMult, r) },
                 juce::Point<float> { freqToX (1000.0f, r), multToY (midMult, r) },
                 juce::Point<float> { freqToX (rev.damping.get(), r), multToY (rev.highDecay.get() * midMult, r) } };
    }

    int hitNode (juce::Point<float> p) const
    {
        auto n = nodes();
        for (int i = 2; i >= 0; --i)
            if (n[(size_t) i].getDistanceFrom (p) < 9.0f)
                return i;
        return -1;
    }

    void paintCurve (juce::Graphics& g, juce::Colour ink)
    {
        auto r = curveArea();
        auto n = nodes();

        // Hairline between the space and the curve
        g.setColour (ink.withAlpha (0.12f));
        g.fillRect (r.getX(), r.getY() - 5.0f, r.getWidth(), 1.0f);

        juce::Path p;
        for (int i = 0; i <= 60; ++i)
        {
            const float x = r.getX() + r.getWidth() * (float) i / 60.0f;
            const float f = xToFreq (x, r);
            const float tLow = 1.0f / (1.0f + std::pow (f / 250.0f, 2.0f));
            const float tHigh = 1.0f / (1.0f + std::pow (rev.damping.get() / f, 2.0f));
            const float y = n[0].y * tLow + n[2].y * tHigh + n[1].y * juce::jmax (0.0f, 1.0f - tLow - tHigh);
            if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
        }

        auto fill = p;
        fill.lineTo (r.getRight(), r.getBottom());
        fill.lineTo (r.getX(), r.getBottom());
        fill.closeSubPath();
        g.setColour (ink.withAlpha (0.06f));
        g.fillPath (fill);
        g.setColour (ink.withAlpha (0.55f));
        g.strokePath (p, juce::PathStrokeType (1.5f));

        const juce::StringArray names { "LOW", "DECAY", "HIGH" };
        for (int i = 0; i < 3; ++i)
        {
            const bool active = hover == i || dragging == i;
            const float d = active ? 10.0f : 7.0f;
            g.setColour (colourFor (i));
            g.fillEllipse (juce::Rectangle<float> (d, d).withCentre (n[(size_t) i]));

            if (active)
            {
                juce::String text = i == 0 ? rev.lowDecay.param().getCurrentValueAsString()
                                  : i == 1 ? rev.decay.param().getCurrentValueAsString()
                                           : rev.damping.param().getCurrentValueAsString() + "  " + rev.highDecay.param().getCurrentValueAsString();
                g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
                g.setColour (ink.withAlpha (0.8f));
                auto lr = juce::Rectangle<float> (130.0f, 12.0f).withCentre (n[(size_t) i].translated (0.0f, -12.0f));
                g.drawText (names[i] + "  " + text, lr.constrainedWithin (getLocalBounds().toFloat()), juce::Justification::centred);
            }
        }
    }
};

//==============================================================================
/** Small button showing a stepped parameter's value; click for a menu of the choices. */
class ReverbMenuButton : public juce::Component
{
public:
    ReverbMenuButton (te::AutomatableParameter& p, juce::String prefix, juce::StringArray choices)
        : param (&p), label (std::move (prefix)), options (std::move (choices))
    {
        timer.startTimerHz (10);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat().reduced (1.0f);
        const int v = juce::jlimit (0, options.size() - 1, juce::roundToInt (param->getCurrentValue()));
        const bool on = v > 0 || label == "Shimmer";
        g.setColour (Colours::panelDark);
        g.fillRoundedRectangle (r, 3.0f);
        g.setFont (juce::FontOptions (10.0f));
        g.setColour (Colours::textDim);
        auto t = r.reduced (5.0f, 0.0f);
        g.drawText (label, t, juce::Justification::centredLeft);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.setColour (on ? Colours::accent : Colours::text);
        g.drawText (options[v] + juce::String::fromUTF8 (" \xe2\x96\xbe"), t, juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent&) override
    {
        juce::PopupMenu m;
        const int v = juce::roundToInt (param->getCurrentValue());
        for (int i = 0; i < options.size(); ++i)
            m.addItem (i + 1, options[i], true, i == v);

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this), [p = param] (int result)
        {
            if (result <= 0) return;
            p->parameterChangeGestureBegin();
            p->setParameter ((float) (result - 1), juce::sendNotification);
            p->parameterChangeGestureEnd();
        });
    }

private:
    te::AutomatableParameter::Ptr param;
    juce::String label;
    juce::StringArray options;
    int last = -1;
    te::LambdaTimer timer { [this] { auto v = juce::roundToInt (param->getCurrentValue()); if (v != last) { last = v; repaint(); } } };
};

//==============================================================================
class ReverbView : public juce::Component
{
public:
    ReverbView (ReverbDevice& d)
        : spaceView (d),
          spaceChoice (d.space.param(), ReverbDevice::spaceNames(), Colours::clips[5]),
          syncButton (d.sync.param(), "Sync", ReverbDevice::syncNames()),
          shimmerButton (d.shimmerInterval.param(), "Shimmer", ReverbDevice::shimmerNames()),
          freeze (d.freeze.param(), "Freeze", Colours::solo),
          size (d.size.param()), decay (d.decay.param()), predelay (d.predelay.param()), diffusion (d.diffusion.param()),
          early (d.early.param()), width (d.width.param()), mix (d.mix.param(), {}, Colours::solo),
          lowDecay (d.lowDecay.param(), "Low Decay", Colours::accent), highDecay (d.highDecay.param(), "High Decay", Colours::solo),
          damping (d.damping.param(), "High Freq", Colours::solo), lowCut (d.lowCut.param()), highCut (d.highCut.param()),
          motion (d.modDepth.param(), {}, Colours::clips[5]), shimmer (d.shimmer.param(), {}, Colours::clips[6]),
          duck (d.duck.param(), {}, Colours::accent)
    {
        addAndMakeVisible (spaceView);
        for (auto* c : std::initializer_list<juce::Component*> { &spaceChoice, &syncButton, &shimmerButton, &freeze })
            addAndMakeVisible (c);
        for (auto* k : row1()) addAndMakeVisible (k);
        for (auto* k : row2()) addAndMakeVisible (k);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        spaceView.setBounds (r.removeFromLeft (juce::jmin (330, r.getWidth() * 3 / 8)));
        r.removeFromLeft (8);

        auto top = r.removeFromTop (22);
        freeze.setBounds (top.removeFromRight (62));
        top.removeFromRight (4);
        shimmerButton.setBounds (top.removeFromRight (98));
        top.removeFromRight (4);
        syncButton.setBounds (top.removeFromRight (84));
        top.removeFromRight (6);
        spaceChoice.setBounds (top);

        r.removeFromTop (4);
        auto rowH = r.getHeight() / 2;
        layoutRow (row1(), r.removeFromTop (rowH));
        layoutRow (row2(), r);
    }

private:
    ReverbSpace spaceView;
    DeviceUI::Choice spaceChoice;
    ReverbMenuButton syncButton, shimmerButton;
    DeviceUI::Toggle freeze;
    DeviceUI::Knob size, decay, predelay, diffusion, early, width, mix;
    DeviceUI::Knob lowDecay, highDecay, damping, lowCut, highCut, motion, shimmer, duck;

    std::vector<DeviceUI::Knob*> row1()     { return { &size, &decay, &predelay, &diffusion, &early, &width, &mix }; }
    std::vector<DeviceUI::Knob*> row2()     { return { &lowDecay, &highDecay, &damping, &lowCut, &highCut, &motion, &shimmer, &duck }; }

    static void layoutRow (std::vector<DeviceUI::Knob*> knobs, juce::Rectangle<int> r)
    {
        const int kw = r.getWidth() / 8;   // both rows on the same grid
        for (auto* k : knobs)
            k->setBounds (r.removeFromLeft (kw));
    }
};

//==============================================================================
/** The reverb in its own window: the space takes the left, with the choice of space over it;
    the controls are on the right, grouped by what they shape. */
class ReverbWindowView : public juce::Component
{
public:
    ReverbWindowView (ReverbDevice& d)
        : spaceView (d, true),
          spaceChoice (d.space.param(), ReverbDevice::spaceNames(), Colours::clips[5]),
          syncButton (d.sync.param(), "Sync", ReverbDevice::syncNames()),
          shimmerButton (d.shimmerInterval.param(), "Shimmer", ReverbDevice::shimmerNames()),
          freeze (d.freeze.param(), "Freeze", Colours::solo),
          size (d.size.param()), decay (d.decay.param()), predelay (d.predelay.param()), diffusion (d.diffusion.param()),
          early (d.early.param()), width (d.width.param()), mix (d.mix.param(), {}, Colours::solo),
          lowDecay (d.lowDecay.param(), "Low Decay", Colours::accent), highDecay (d.highDecay.param(), "High Decay", Colours::solo),
          damping (d.damping.param(), "High Freq", Colours::solo), lowCut (d.lowCut.param()), highCut (d.highCut.param()),
          motion (d.modDepth.param(), {}, Colours::clips[5]), shimmer (d.shimmer.param(), {}, Colours::clips[6]),
          duck (d.duck.param(), {}, Colours::accent)
    {
        for (auto* c : std::initializer_list<juce::Component*> { &spaceView, &spaceChoice, &syncButton, &shimmerButton, &freeze,
                                                                 &size, &decay, &predelay, &diffusion, &early, &width, &mix,
                                                                 &lowDecay, &highDecay, &damping, &lowCut, &highCut, &motion, &shimmer, &duck })
            addAndMakeVisible (c);
    }

    void paint (juce::Graphics& g) override
    {
        DeviceUI::paintSection (g, spaceArea, "Space");
        DeviceUI::paintSection (g, decayArea, "Decay");
        DeviceUI::paintSection (g, filterArea, "Filter");
        DeviceUI::paintSection (g, motionArea, "Motion");
        DeviceUI::paintSection (g, outputArea, "Output");

        // (a switch that is off is the colour of the panel it sits on, so each gets an edge)
        g.setColour (Colours::textDim.withAlpha (0.3f));
        for (auto* c : std::initializer_list<juce::Component*> { &freeze, &syncButton, &shimmerButton })
            g.drawRoundedRectangle (c->getBounds().toFloat().reduced (0.5f), 3.5f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto controls = r.removeFromRight (juce::jlimit (360, 440, r.getWidth() * 2 / 5));
        r.removeFromRight (8);

        spaceChoice.setBounds (r.removeFromTop (28));
        r.removeFromTop (8);
        spaceView.setBounds (r);

        // Four rows of four places; a row's switch or menu sits in its title line
        const int gap = 8;
        const int rowH = (controls.getHeight() - 3 * gap) / 4;
        auto nextRow = [&] { auto row = controls.removeFromTop (rowH); controls.removeFromTop (gap); return row; };

        spaceArea = nextRow();
        decayArea = nextRow();
        auto third = nextRow();
        filterArea = third.removeFromLeft ((third.getWidth() - gap) / 2);
        third.removeFromLeft (gap);
        motionArea = third;
        outputArea = controls.withHeight (rowH);

        auto knobs = [] (juce::Rectangle<int> area, std::initializer_list<DeviceUI::Knob*> list, int places)
        {
            area = area.reduced (6).withTrimmedTop (22);
            area = area.withSizeKeepingCentre (area.getWidth(), juce::jmin (96, area.getHeight()));
            const int w = area.getWidth() / places;
            for (auto* k : list)
                k->setBounds (area.removeFromLeft (w));
        };
        auto corner = [] (juce::Rectangle<int> area, int w)     { return area.reduced (6, 5).removeFromTop (22).removeFromRight (w); };

        knobs (spaceArea, { &size, &predelay, &diffusion, &early }, 4);
        syncButton.setBounds (corner (spaceArea, 92));
        knobs (decayArea, { &decay, &lowDecay, &highDecay, &damping }, 4);
        freeze.setBounds (corner (decayArea, 70));
        knobs (filterArea, { &lowCut, &highCut }, 2);
        knobs (motionArea, { &motion, &shimmer }, 2);
        shimmerButton.setBounds (corner (motionArea, 100));
        knobs (outputArea, { &width, &duck, &mix }, 4);
    }

private:
    ReverbSpace spaceView;
    DeviceUI::Choice spaceChoice;
    ReverbMenuButton syncButton, shimmerButton;
    DeviceUI::Toggle freeze;
    DeviceUI::Knob size, decay, predelay, diffusion, early, width, mix;
    DeviceUI::Knob lowDecay, highDecay, damping, lowCut, highCut, motion, shimmer, duck;
    juce::Rectangle<int> spaceArea, decayArea, filterArea, motionArea, outputArea;
};

inline std::unique_ptr<juce::Component> ReverbDevice::createDeviceView()    { return std::make_unique<ReverbView> (*this); }
inline std::unique_ptr<juce::Component> ReverbDevice::createWindowView()    { return std::make_unique<ReverbWindowView> (*this); }

} // namespace elsewhere
