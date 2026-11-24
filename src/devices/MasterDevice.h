// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"
#include "Dsp.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Master: a mastering chain with an assistant.

    Chain: input gain -> 6-band tonal EQ -> character stage (glue compression,
    saturation, voicing) -> stereo width (with mono bass) -> true-peak limiter.

    The assistant listens while you play the song (Analyze), then sets the input gain
    to reach the loudness target, and the EQ to move the mix's tonal balance toward a
    typical finished-master curve. Everything it sets stays editable.
*/
class MasterDevice : public ElsewhereDevice,
                     private juce::AsyncUpdater
{
public:
    static constexpr const char* xmlTypeName = "elsewhereMaster";
    static constexpr int numBands = 6;
    enum Character { clean, warm, punchy, bright, numCharacters };

    static juce::StringArray characterNames()   { return { "Clean", "Warm", "Punchy", "Bright" }; }

    struct BandSpec { Biquad::Type type; float hz, q; const char* name; };
    static const BandSpec& band (int i)
    {
        static const BandSpec b[numBands] = {
            { Biquad::Type::lowShelf,  60.0f,    0.7f, "Sub" },
            { Biquad::Type::bell,      150.0f,   0.7f, "Low" },
            { Biquad::Type::bell,      500.0f,   0.7f, "Low Mid" },
            { Biquad::Type::bell,      1700.0f,  0.7f, "Mid" },
            { Biquad::Type::bell,      5000.0f,  0.7f, "Presence" },
            { Biquad::Type::highShelf, 11000.0f, 0.7f, "Air" } };
        return b[i];
    }

    ~MasterDevice() override    { cancelPendingUpdate(); }

    MasterDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        character = addParameter ("character", "Character", (float) clean, stepped (0, numCharacters - 1),
                                  [] (float v) { return characterNames()[juce::jlimit (0, numCharacters - 1, juce::roundToInt (v))]; });
        target    = addParameter ("target", "Loudness", -14.0f, { -20.0f, -6.0f },
                                  [] (float v) { return juce::String (v, 1) + " LUFS"; });
        inputGain = addParameter ("gain", "Input Gain", 0.0f, { -24.0f, 24.0f }, dbText);
        eqAmount  = addParameter ("eqamount", "EQ Amount", 60.0f, { 0.0f, 100.0f }, percentText);
        width     = addParameter ("width", "Width", 100.0f, { 0.0f, 200.0f }, percentText);
        monoBass  = addParameter ("monobass", "Mono Bass", 1.0f, stepped (0, 1), onOffText);
        ceiling   = addParameter ("ceiling", "Ceiling", -1.0f, { -3.0f, 0.0f },
                                  [] (float v) { return juce::String (v, 1) + " dBTP"; });

        for (int i = 0; i < numBands; ++i)
            eq[(size_t) i] = addParameter ("eq" + juce::String (i + 1), juce::String (band (i).name) + " EQ", 0.0f,
                                           { -12.0f, 12.0f }, dbText);
    }

    juce::String getName() const override              { return "Elsewhere Master"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Master"; }
    int getDeviceViewWidth() const override            { return 860; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    double getLatencySeconds() override                 { return limiter.getLatencySamples() / rate; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        // (The engine can do this while audio is still running through the device)
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        rate = info.sampleRate;
        limiter.prepare (rate, 1.5);
        limiter.setReleaseMs (120.0f);
        inputMeter.prepare (rate);
        inputMeter.integrating = false;
        outputMeter.prepare (rate);
        for (auto& b : eqFilters) b.reset();
        for (auto& b : voicing) b.reset();
        for (auto& b : sideHp) b.reset();
        compEnv = 0.0f;
        lastHash = 0;
        smoothedGain.setTimeMs (50.0f, rate);
        smoothedGain.reset (juce::Decibels::decibelsToGain (inputGain.get()));
        smoothedWidth.setTimeMs (50.0f, rate);
        smoothedWidth.reset (width.get() / 100.0f);
        fftFill = 0;
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked())
            return;     // being set up again for a new rate or buffer size

        if (fc.destBuffer == nullptr || fc.destBuffer->getNumChannels() == 0)
            return;

        juce::ScopedNoDenormals noDenormals;
        auto& buf = *fc.destBuffer;
        const int n = fc.bufferNumSamples;
        float* L = buf.getWritePointer (0, fc.bufferStartSample);
        float* R = buf.getNumChannels() > 1 ? buf.getWritePointer (1, fc.bufferStartSample) : L;

        if (analyzing.load())
            analyze (L, R, n);

        updateFilters();

        const auto ch = currentCharacter();
        const auto cs = characterSpec (ch);
        smoothedGain.target = juce::Decibels::decibelsToGain (inputGain.get());
        smoothedWidth.target = width.get() / 100.0f;
        const bool mono = monoBass.get() > 0.5f;

        const float atk = std::exp (-1.0f / (cs.attackMs * 0.001f * (float) rate));
        const float rel = std::exp (-1.0f / (cs.releaseMs * 0.001f * (float) rate));
        const float rmsC = std::exp (-1.0f / (0.005f * (float) rate));
        const float thr = target.get() + cs.thresholdAboveTarget;
        float maxComp = 0.0f;

        for (int i = 0; i < n; ++i)
        {
            const float g = smoothedGain.next();
            float l = L[i] * g, r = (R != L ? R[i] : L[i]) * g;

            // Tonal EQ + character voicing
            for (int b = 0; b < numBands; ++b)
                if (eqActive[(size_t) b])
                    { l = eqFilters[(size_t) b].process (l, 0); r = eqFilters[(size_t) b].process (r, 1); }

            for (int b = 0; b < numVoicing; ++b)
                { l = voicing[(size_t) b].process (l, 0); r = voicing[(size_t) b].process (r, 1); }

            // Glue compressor (stereo linked, RMS-ish detector, soft knee)
            float det = std::max (std::abs (l), std::abs (r));
            rmsState = rmsC * rmsState + (1.0f - rmsC) * det * det;
            float levelDb = juce::Decibels::gainToDecibels (std::sqrt (rmsState) * 1.414f, -120.0f);
            float over = levelDb - thr;
            float targetGr = 0.0f;
            const float knee = 6.0f, slope = 1.0f / cs.ratio - 1.0f;
            if (over > knee * 0.5f)           targetGr = slope * over;
            else if (over > -knee * 0.5f)     targetGr = slope * (over + knee * 0.5f) * (over + knee * 0.5f) / (2.0f * knee);
            compEnv = (targetGr < compEnv ? atk : rel) * compEnv + (1.0f - (targetGr < compEnv ? atk : rel)) * targetGr;
            maxComp = std::min (maxComp, compEnv);
            const float cg = juce::Decibels::decibelsToGain (compEnv * cs.mix);
            l *= cg; r *= cg;

            // Saturation
            if (cs.saturation > 0.0f)
            {
                const float k = 1.0f + cs.saturation * 2.0f;
                l = std::tanh (l * k) / k;
                r = std::tanh (r * k) / k;
            }

            // Width: mid/side, with the side's lows removed when Mono Bass is on
            float mid = (l + r) * 0.5f, side = (l - r) * 0.5f;
            if (mono)
                side = sideHp[1].process (sideHp[0].process (side, 0), 0);
            side *= smoothedWidth.next();
            L[i] = mid + side;
            if (R != L) R[i] = mid - side;
        }

        // Limiter (0.15 dB inside the ceiling: margin for the true-peak estimate)
        const float ceil = juce::Decibels::decibelsToGain (ceiling.get() - 0.15f);
        const float lowest = limiter.process (L, R != L ? R : L, n, ceil);

        // Meters
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
            peak = std::max (peak, std::max (std::abs (L[i]), std::abs (R[i])));

        if (resetRequested.exchange (false))
            outputMeter.reset();

        outputMeter.process (L, R, n);

        // After the assistant applies its settings: check the real output loudness and trim
        // the gain (the limiter's effect on loudness depends on the music, so measure it)
        if (trimRoundsLeft.load() > 0 && outputMeter.getSecondsAudible() >= trimSeconds)
        {
            auto err = target.get() - outputMeter.getIntegrated();

            if (std::abs (err) > 0.25 && outputMeter.getIntegrated() > -70.0)
            {
                pendingTrimDb.store ((float) err);
                --trimRoundsLeft;
                triggerAsyncUpdate();
            }
            else
            {
                trimRoundsLeft = 0;
            }

            outputMeter.reset();
        }
        {
            const juce::SpinLock::ScopedTryLockType sl (meterLock);
            if (sl.isLocked())
            {
                meters.momentary = (float) outputMeter.getMomentary();
                meters.shortTerm = (float) outputMeter.getShortTerm();
                meters.integrated = (float) outputMeter.getIntegrated();
            }
        }
        meters.peak.store (std::max (meters.peak.load(), peak));
        meters.limiterGr.store (std::min (meters.limiterGr.load(), juce::Decibels::gainToDecibels (lowest)));
        meters.compGr.store (std::min (meters.compGr.load(), maxComp * cs.mix));
    }

    //==============================================================================
    // Assistant

    void startAnalysis()
    {
        const juce::SpinLock::ScopedLockType sl (analysisLock);
        inputMeter.reset();
        inputMeter.integrating = true;
        bandPower.fill (0.0);
        fftFrames = 0;
        analyzing = true;
    }

    /** Stops listening and applies the result. Returns false if there wasn't enough audio. */
    bool finishAnalysis()
    {
        analyzing = false;
        const juce::SpinLock::ScopedLockType sl (analysisLock);
        inputMeter.integrating = false;

        const auto measured = inputMeter.getIntegrated();
        if (inputMeter.getSecondsMeasured() < 3.0 || measured < -60.0 || fftFrames < 10)
            return false;

        lastMeasuredLufs = measured;

        // Loudness: gain to reach the target (+0.5 dB for what the glue/limiter take away)
        auto gainDb = juce::jlimit (-24.0, 24.0, target.get() - measured + 0.5);
        inputGain.param().setParameter ((float) gainDb, juce::sendNotification);

        // Tonal balance: energy per octave in each region vs a typical finished-master curve
        static const double edges[numBands + 1] = { 20, 90, 250, 900, 3000, 9000, 20000 };
        static const double targetCurve[numBands] = { 3.0, 2.0, 0.0, -1.5, -4.0, -8.0 };
        std::array<double, numBands> measuredDb {}, correction {};
        double meanM = 0.0, meanT = 0.0;

        for (int b = 0; b < numBands; ++b)
        {
            auto perOctave = bandPower[(size_t) b] / std::log2 (edges[b + 1] / edges[b]);
            measuredDb[(size_t) b] = 10.0 * std::log10 (std::max (1.0e-20, perOctave));
            meanM += measuredDb[(size_t) b] / numBands;
            meanT += targetCurve[b] / numBands;
        }

        double meanC = 0.0;
        for (int b = 0; b < numBands; ++b)
        {
            auto diff = (targetCurve[b] - meanT) - (measuredDb[(size_t) b] - meanM);
            correction[(size_t) b] = juce::jlimit (-6.0, 6.0, 0.6 * diff);
            meanC += correction[(size_t) b] / numBands;
        }

        for (int b = 0; b < numBands; ++b)
        {
            lastBalance[(size_t) b] = (float) (measuredDb[(size_t) b] - meanM);
            eq[(size_t) b].param().setParameter ((float) juce::jlimit (-12.0, 12.0, correction[(size_t) b] - meanC), juce::sendNotification);
        }

        hasAnalysis = true;
        trimRoundsLeft = 4;
        resetOutputMeter();
        return true;
    }

    /** True while the assistant is still fine-tuning the loudness after Apply. */
    bool isFineTuning() const               { return trimRoundsLeft.load() > 0; }

    /** Applies a loudness correction measured on the audio thread (message thread). */
    bool applyPendingTrim()
    {
        auto trim = pendingTrimDb.exchange (0.0f);
        if (std::abs (trim) < 0.01f)
            return false;

        inputGain.param().setParameter (juce::jlimit (-24.0f, 24.0f, inputGain.get() + trim), juce::sendNotification);
        resetOutputMeter();
        return true;
    }

    bool isAnalyzing() const                { return analyzing.load(); }
    double getSecondsAnalyzed() const       { const juce::SpinLock::ScopedLockType sl (analysisLock); return inputMeter.getSecondsMeasured(); }
    double getAnalysisInputLufs() const     { const juce::SpinLock::ScopedLockType sl (analysisLock); return inputMeter.getShortTerm(); }

    bool hasAnalysis = false;
    double lastMeasuredLufs = -120.0;
    std::array<float, numBands> lastBalance {};     // measured tonal balance (dB, mean removed)

    static double targetBalance (int b)
    {
        static const double t[numBands] = { 3.0, 2.0, 0.0, -1.5, -4.0, -8.0 };
        double mean = 0; for (auto v : t) mean += v / numBands;
        return t[b] - mean;
    }

    //==============================================================================
    struct MeterValues
    {
        float momentary = -120.0f, shortTerm = -120.0f, integrated = -120.0f;
        std::atomic<float> peak { 0.0f }, limiterGr { 0.0f }, compGr { 0.0f };
    };

    void getLoudness (float& momentary, float& shortTerm, float& integrated)
    {
        const juce::SpinLock::ScopedLockType sl (meterLock);
        momentary = meters.momentary; shortTerm = meters.shortTerm; integrated = meters.integrated;
    }

    float takePeak()            { return meters.peak.exchange (0.0f); }
    float takeLimiterGr()       { return meters.limiterGr.exchange (0.0f); }
    float takeCompGr()          { return meters.compGr.exchange (0.0f); }

    /** Restarts the integrated loudness measurement (done on the audio thread). */
    void resetOutputMeter()
    {
        resetRequested = true;
        const juce::SpinLock::ScopedLockType sl (meterLock);
        meters.integrated = -120.0f;
    }

    /** Combined EQ response (tonal EQ only) at a frequency, for display. */
    double getEqResponseDb (double freq) const
    {
        double total = 0.0;
        for (int b = 0; b < numBands; ++b)
        {
            Biquad f;
            f.set (band (b).type, band (b).hz, band (b).q, eq[(size_t) b].get() * eqAmount.get() / 100.0f, rate);
            total += f.magnitudeDb (freq, rate);
        }
        return total;
    }

    Param character, target, inputGain, eqAmount, width, monoBass, ceiling;
    std::array<Param, numBands> eq;

private:
    struct CharacterSpec
    {
        float ratio, attackMs, releaseMs, thresholdAboveTarget, mix, saturation;
        struct V { Biquad::Type type; float hz, q, db; };
        std::array<V, 2> voicing;
    };

    static CharacterSpec characterSpec (Character c)
    {
        using T = Biquad::Type;
        switch (c)
        {
            case warm:   return { 2.0f, 30.0f, 300.0f, 5.0f, 1.0f, 0.35f, {{ { T::lowShelf, 120, 0.7f, 1.0f },  { T::highShelf, 9000, 0.7f, -1.0f } }} };
            case punchy: return { 3.0f, 40.0f, 150.0f, 4.0f, 1.0f, 0.10f, {{ { T::bell, 80, 0.9f, 1.0f },       { T::bell, 350, 0.8f, -0.8f } }} };
            case bright: return { 1.8f, 25.0f, 200.0f, 6.0f, 1.0f, 0.10f, {{ { T::highShelf, 12000, 0.7f, 2.0f }, { T::bell, 250, 0.8f, -0.5f } }} };
            case clean:
            case numCharacters: break;
        }
        return { 1.5f, 30.0f, 250.0f, 6.0f, 1.0f, 0.0f, {{ { T::bell, 1000, 0.7f, 0.0f }, { T::bell, 1000, 0.7f, 0.0f } }} };
    }

    Character currentCharacter() const  { return (Character) juce::jlimit (0, numCharacters - 1, juce::roundToInt (character.get())); }

    static constexpr int numVoicing = 2;
    double rate = 44100.0;
    std::array<Biquad, numBands> eqFilters;
    std::array<bool, numBands> eqActive {};
    std::array<Biquad, numVoicing> voicing;
    std::array<Biquad, 2> sideHp;
    float compEnv = 0.0f, rmsState = 0.0f;
    Smoothed smoothedGain, smoothedWidth;
    TruePeakLimiter limiter;
    size_t lastHash = 0;

    LoudnessMeter inputMeter, outputMeter;
    mutable juce::SpinLock analysisLock, meterLock;
    std::atomic<bool> analyzing { false };
    std::atomic<bool> resetRequested { false };
    std::atomic<int> trimRoundsLeft { 0 };
    std::atomic<float> pendingTrimDb { 0.0f };
    static constexpr double trimSeconds = 6.0;

    void handleAsyncUpdate() override   { applyPendingTrim(); }
    MeterValues meters;

    // Spectrum analysis for the assistant
    static constexpr int fftOrder = 12, fftSize = 1 << fftOrder;
    juce::dsp::FFT fft { fftOrder };
    std::array<float, fftSize> fftInput {};
    std::array<float, fftSize * 2> fftWork {};
    int fftFill = 0;
    std::array<double, numBands> bandPower {};
    int fftFrames = 0;

    void updateFilters()
    {
        size_t h = std::hash<double>{} (rate);
        for (float v : { character.get(), eqAmount.get() })
            h = h * 31 + std::hash<float>{} (v);
        for (auto& p : eq)
            h = h * 31 + std::hash<float>{} (p.get());

        if (h == lastHash)
            return;

        lastHash = h;

        for (int b = 0; b < numBands; ++b)
        {
            auto db = eq[(size_t) b].get() * eqAmount.get() / 100.0f;
            eqActive[(size_t) b] = std::abs (db) > 0.01f;
            eqFilters[(size_t) b].set (band (b).type, band (b).hz, band (b).q, db, rate);
        }

        auto cs = characterSpec (currentCharacter());
        for (int b = 0; b < numVoicing; ++b)
            voicing[(size_t) b].set (cs.voicing[(size_t) b].type, cs.voicing[(size_t) b].hz, cs.voicing[(size_t) b].q, cs.voicing[(size_t) b].db, rate);

        for (auto& f : sideHp)
            f.set (Biquad::Type::highPass, 120.0, 0.707, 0.0, rate);
    }

    void analyze (const float* L, const float* R, int n)
    {
        const juce::SpinLock::ScopedTryLockType sl (analysisLock);
        if (! sl.isLocked())
            return;

        inputMeter.process (L, R, n);

        for (int i = 0; i < n; ++i)
        {
            fftInput[(size_t) fftFill++] = (L[i] + R[i]) * 0.5f;

            if (fftFill == fftSize)
            {
                // Hann window, power spectrum, summed into the tonal regions
                for (int k = 0; k < fftSize; ++k)
                    fftWork[(size_t) k] = fftInput[(size_t) k] * (0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) k / (float) fftSize));
                std::fill (fftWork.begin() + fftSize, fftWork.end(), 0.0f);
                fft.performFrequencyOnlyForwardTransform (fftWork.data());

                static const double edges[numBands + 1] = { 20, 90, 250, 900, 3000, 9000, 20000 };
                for (int bin = 1; bin < fftSize / 2; ++bin)
                {
                    auto f = bin * rate / fftSize;
                    for (int b = 0; b < numBands; ++b)
                        if (f >= edges[b] && f < edges[b + 1])
                            { bandPower[(size_t) b] += (double) fftWork[(size_t) bin] * fftWork[(size_t) bin]; break; }
                }

                ++fftFrames;

                // 50% overlap
                std::copy (fftInput.begin() + fftSize / 2, fftInput.end(), fftInput.begin());
                fftFill = fftSize / 2;
            }
        }
    }
};

//==============================================================================
/** Tonal balance display: the EQ curve, and after analysis the mix's balance vs the target. */
class MasterGraph : public juce::Component, private juce::Timer
{
public:
    MasterGraph (MasterDevice& d) : dev (d)     { startTimerHz (15); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 4.0f);

        g.setFont (juce::FontOptions (9.5f));
        for (double f : { 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0 })
        {
            auto x = freqToX (f);
            g.setColour (Colours::gridLine);
            g.drawVerticalLine ((int) x, r.getY(), r.getBottom());
            g.setColour (Colours::textDim);
            g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                        juce::Rectangle<float> (x + 2, r.getBottom() - 13, 30, 12).toNearestInt(), juce::Justification::left);
        }

        g.setColour (Colours::gridBar);
        g.drawHorizontalLine ((int) dbToY (0.0f), r.getX(), r.getRight());

        // Analysis: measured balance (dots) against the target (line)
        if (dev.hasAnalysis)
        {
            juce::Path targetPath;
            for (int b = 0; b < MasterDevice::numBands; ++b)
            {
                auto p = juce::Point<float> (freqToX (centre (b)), dbToY ((float) MasterDevice::targetBalance (b)));
                if (b == 0) targetPath.startNewSubPath (p); else targetPath.lineTo (p);
            }
            g.setColour (Colours::textDim);
            g.strokePath (targetPath, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved), juce::AffineTransform());

            for (int b = 0; b < MasterDevice::numBands; ++b)
            {
                g.setColour (Colours::solo);
                g.fillEllipse (juce::Rectangle<float> (7, 7).withCentre ({ freqToX (centre (b)), dbToY (juce::jlimit (-12.0f, 12.0f, dev.lastBalance[(size_t) b])) }));
            }
        }

        // EQ curve
        juce::Path curve;
        for (int x = 0; x <= getWidth(); x += 2)
        {
            auto y = dbToY ((float) dev.getEqResponseDb (xToFreq ((float) x)));
            if (x == 0) curve.startNewSubPath ((float) x, y); else curve.lineTo ((float) x, y);
        }
        g.setColour (Colours::accent);
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        // Band handles (drag up/down)
        for (int b = 0; b < MasterDevice::numBands; ++b)
        {
            auto pos = handle (b);
            g.setColour (b == dragging || b == hovered ? Colours::text : Colours::accent);
            g.fillEllipse (juce::Rectangle<float> (9, 9).withCentre (pos));
        }

        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (10.0f));
        g.drawText (hovered >= 0 ? juce::String (MasterDevice::band (hovered).name) + "  " + dev.eq[(size_t) hovered].param().getCurrentValueAsString()
                                 : juce::String ("TONAL BALANCE"),
                    getLocalBounds().reduced (6, 3), juce::Justification::topLeft);

        if (dev.hasAnalysis)
        {
            g.setColour (Theme::readable (Colours::solo));
            g.drawText (juce::String::fromUTF8 ("\xe2\x97\x8f your mix"), getLocalBounds().reduced (6, 3), juce::Justification::topRight);
            g.setColour (Colours::textDim);
            g.drawText (juce::String::fromUTF8 ("\xe2\x80\x94 target   "), getLocalBounds().reduced (6, 3).withTrimmedRight (58), juce::Justification::topRight);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override     { auto h = find (e.position); if (h != hovered) { hovered = h; repaint(); } }
    void mouseExit (const juce::MouseEvent&) override       { hovered = -1; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = find (e.position);
        if (dragging >= 0) dev.eq[(size_t) dragging].param().parameterChangeGestureBegin();
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0) return;
        // The handle shows the band's effective gain (after EQ Amount), so undo that scaling
        auto amount = juce::jmax (0.05f, dev.eqAmount.get() / 100.0f);
        dev.eq[(size_t) dragging].param().setParameter (juce::jlimit (-12.0f, 12.0f, yToDb (e.position.y) / amount), juce::sendNotification);
        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0) dev.eq[(size_t) dragging].param().parameterChangeGestureEnd();
        dragging = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        if (auto h = find (e.position); h >= 0)
            dev.eq[(size_t) h].param().setParameter (0.0f, juce::sendNotification);
    }

private:
    MasterDevice& dev;
    int dragging = -1, hovered = -1;

    static double centre (int b)
    {
        static const double c[] = { 50, 150, 480, 1650, 5200, 13000 };
        return c[b];
    }

    float freqToX (double f) const  { return (float) (std::log (f / 20.0) / std::log (1000.0)) * (float) getWidth(); }
    double xToFreq (float x) const  { return 20.0 * std::pow (1000.0, (double) x / juce::jmax (1, getWidth())); }
    float dbToY (float db) const    { return juce::jmap (db, 12.0f, -12.0f, 16.0f, (float) getHeight() - 16.0f); }
    float yToDb (float y) const     { return juce::jmap (y, 16.0f, (float) getHeight() - 16.0f, 12.0f, -12.0f); }

    juce::Point<float> handle (int b) const
    {
        return { freqToX (MasterDevice::band (b).hz), dbToY (dev.eq[(size_t) b].get() * dev.eqAmount.get() / 100.0f) };
    }

    int find (juce::Point<float> p) const
    {
        for (int b = 0; b < MasterDevice::numBands; ++b)
            if (handle (b).getDistanceFrom (p) < 9.0f)
                return b;
        return -1;
    }

    void timerCallback() override   { repaint(); }
};

//==============================================================================
class MasterView : public juce::Component, private juce::Timer
{
public:
    MasterView (MasterDevice& d)
        : dev (d), graph (d),
          characterChoice (d.character.param(), MasterDevice::characterNames(), Colours::solo),
          gainKnob (d.inputGain.param(), "Gain", Colours::solo), targetKnob (d.target.param(), "Loudness", Colours::solo),
          eqKnob (d.eqAmount.param(), "EQ Amount"), widthKnob (d.width.param()), ceilingKnob (d.ceiling.param()),
          monoBass (d.monoBass.param(), "Mono Bass")
    {
        analyzeButton.setColour (juce::TextButton::buttonColourId, Colours::solo.withAlpha (0.35f));
        analyzeButton.onClick = [this]
        {
            if (dev.isAnalyzing())
            {
                message = dev.finishAnalysis()
                            ? "Done: measured " + juce::String (dev.lastMeasuredLufs, 1) + " LUFS, set gain and EQ."
                            : "Not enough audio heard. Play at least a few seconds of the song, then try again.";
            }
            else
            {
                dev.startAnalysis();
                message = "Listening... play the song (ideally the whole thing, or its loudest part), then click Apply.";
            }
            refresh();
        };
        addAndMakeVisible (analyzeButton);

        resetButton.onClick = [this] { dev.resetOutputMeter(); };
        resetButton.setTooltip ("Restart the integrated loudness measurement");
        addAndMakeVisible (resetButton);

        for (juce::Component* c : { (juce::Component*) &graph, (juce::Component*) &characterChoice, (juce::Component*) &gainKnob,
                                    (juce::Component*) &targetKnob, (juce::Component*) &eqKnob, (juce::Component*) &widthKnob,
                                    (juce::Component*) &ceilingKnob, (juce::Component*) &monoBass })
            addAndMakeVisible (c);

        message = "Put Elsewhere Master last on the Master track. Click Analyze and play your song to have it set loudness and tone.";
        refresh();
        startTimerHz (15);
    }

    void paint (juce::Graphics& g) override
    {
        // Assistant message
        g.setColour (dev.isAnalyzing() ? Colours::solo : Colours::textDim);
        g.setFont (juce::FontOptions (11.0f));
        g.drawFittedText (message, messageArea, juce::Justification::topLeft, 4);

        if (dev.isFineTuning() && ! dev.isAnalyzing())
        {
            g.setColour (Theme::readable (Colours::solo));
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText ("Fine-tuning loudness... keep playing", messageArea.withTop (messageArea.getBottom() - 16), juce::Justification::bottomLeft);
        }

        if (dev.isAnalyzing())
        {
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (juce::String (juce::roundToInt (dev.getSecondsAnalyzed())) + " s heard   " + juce::String (dev.getAnalysisInputLufs(), 1) + " LUFS",
                        messageArea.withTop (messageArea.getBottom() - 16), juce::Justification::bottomLeft);
        }

        // Meters
        auto m = meterArea;
        DeviceUI::paintSection (g, m, "Output");
        m.removeFromTop (16);
        m.reduce (8, 2);

        auto line = [&] (const juce::String& label, const juce::String& value, bool big, juce::Colour c)
        {
            auto row = m.removeFromTop (big ? 30 : 16);
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (10.0f));
            g.drawText (label, row, juce::Justification::centredLeft);
            g.setColour (c);
            g.setFont (juce::FontOptions (big ? 20.0f : 12.0f, juce::Font::bold));
            g.drawText (value, row, juce::Justification::centredRight);
        };

        auto fmt = [] (float v) { return v <= -99.0f ? juce::String ("--") : juce::String (v, 1); };
        auto target = dev.target.get();
        auto close = integrated > -99.0f && std::abs (integrated - target) < 1.0f;
        line ("Integrated", fmt (integrated), true, close ? Colours::meterGreen : Colours::text);
        line ("Short-term", fmt (shortTerm), false, Colours::text);
        line ("Momentary", fmt (momentary), false, Colours::text);
        line ("Peak", peakShown < 0.00001f ? juce::String ("--") : juce::String (juce::Decibels::gainToDecibels (peakShown), 1) + " dB", false,
              peakShown > juce::Decibels::decibelsToGain (dev.ceiling.get() + 0.05f) ? Colours::meterRed : Colours::text);
        line ("Limiting", juce::String (limiterShown, 1) + " dB", false, limiterShown < -6.0f ? Colours::meterYellow : Colours::text);
        line ("Glue", juce::String (compShown, 1) + " dB", false, Colours::text);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto left = r.removeFromLeft (170);
        analyzeButton.setBounds (left.removeFromTop (34));
        left.removeFromTop (6);
        messageArea = left;

        r.removeFromLeft (8);
        meterArea = r.removeFromRight (150);
        resetButton.setBounds (meterArea.getRight() - 46, meterArea.getY() + 1, 44, 15);
        r.removeFromRight (8);

        auto knobs = r.removeFromRight (190);
        r.removeFromRight (8);
        characterChoice.setBounds (r.removeFromTop (22));
        r.removeFromTop (4);
        graph.setBounds (r);

        auto row1 = knobs.removeFromTop (knobs.getHeight() / 2);
        auto w = knobs.getWidth() / 3;
        gainKnob.setBounds (row1.removeFromLeft (w));
        targetKnob.setBounds (row1.removeFromLeft (w));
        eqKnob.setBounds (row1);
        widthKnob.setBounds (knobs.removeFromLeft (w));
        ceilingKnob.setBounds (knobs.removeFromLeft (w));
        monoBass.setBounds (knobs.withSizeKeepingCentre (w - 6, 22));
    }

private:
    MasterDevice& dev;
    MasterGraph graph;
    DeviceUI::Choice characterChoice;
    DeviceUI::Knob gainKnob, targetKnob, eqKnob, widthKnob, ceilingKnob;
    DeviceUI::Toggle monoBass;
    juce::TextButton analyzeButton, resetButton { "Reset" };
    juce::String message;
    juce::Rectangle<int> messageArea, meterArea;
    float momentary = -120.0f, shortTerm = -120.0f, integrated = -120.0f, peakShown = 0.0f, limiterShown = 0.0f, compShown = 0.0f;

    void refresh()
    {
        analyzeButton.setButtonText (dev.isAnalyzing() ? "Apply" : (dev.hasAnalysis ? "Analyze Again" : "Analyze"));
        repaint();
    }

    void timerCallback() override
    {
        dev.getLoudness (momentary, shortTerm, integrated);
        peakShown = juce::jmax (dev.takePeak(), peakShown * 0.9f);
        limiterShown = juce::jmin (dev.takeLimiterGr(), limiterShown * 0.9f);
        compShown = juce::jmin (dev.takeCompGr(), compShown * 0.9f);
        repaint();
    }
};

inline std::unique_ptr<juce::Component> MasterDevice::createDeviceView()    { return std::make_unique<MasterView> (*this); }

} // namespace elsewhere
