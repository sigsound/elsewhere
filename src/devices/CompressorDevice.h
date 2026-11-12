// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"

namespace elsewhere
{
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE ("-Wswitch-enum")

//==============================================================================
/**
    Elsewhere Compressor: a feed-forward compressor with character models.

      Clean    - transparent, program-independent
      VCA      - punchy, fast and precise, hard-ish knee (bus glue)
      FET      - aggressive, fast attack, adds harmonics when pushed
      Opto     - slow, smooth, program-dependent release (vocals, bass)
      Vintage  - soft knee, gentle and warm with light tube-style saturation
      Limiter  - brickwall-style peak limiting with lookahead-free fast attack

    Each model changes the detector (peak / RMS), knee, attack/release behaviour
    and saturation, rather than just being a preset of the knobs.
*/
class CompressorDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereCompressor";
    enum Model { clean, vca, fet, opto, vintage, limiter, numModels };

    static juce::StringArray modelNames()   { return { "Clean", "VCA", "FET", "Opto", "Vintage", "Limiter" }; }

    CompressorDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        model     = addParameter ("model", "Model", (float) vca, stepped (0, numModels - 1),
                                  [] (float v) { return modelNames()[juce::jlimit (0, numModels - 1, juce::roundToInt (v))]; });
        threshold = addParameter ("threshold", "Threshold", -18.0f, { -60.0f, 0.0f }, dbText);
        ratio     = addParameter ("ratio", "Ratio", 4.0f, skewed (1.0f, 30.0f, 4.0f),
                                  [] (float v) { return v >= 29.9f ? juce::String ("inf : 1") : juce::String (v, 1) + " : 1"; });
        attack    = addParameter ("attack", "Attack", 10.0f, skewed (0.05f, 200.0f, 10.0f), msText);
        release   = addParameter ("release", "Release", 120.0f, skewed (5.0f, 2000.0f, 150.0f), msText);
        knee      = addParameter ("knee", "Knee", 6.0f, { 0.0f, 24.0f }, dbText);
        makeup    = addParameter ("makeup", "Make-Up", 0.0f, { -12.0f, 24.0f }, dbText);
        autoGain  = addParameter ("autogain", "Auto Gain", 1.0f, stepped (0, 1), onOffText);
        mix       = addParameter ("mix", "Mix", 100.0f, { 0.0f, 100.0f }, percentText);
        scHpf     = addParameter ("schpf", "Sidechain HPF", 20.0f, freqRange (20.0f, 500.0f), hzText);
        drive     = addParameter ("drive", "Distortion", 0.0f, { 0.0f, 100.0f }, percentText);
        output    = addParameter ("output", "Output", 0.0f, { -24.0f, 12.0f }, dbText);
        listen    = addParameter ("sclisten", "Listen", 0.0f, stepped (0, 1), onOffText);
    }

    // A second stereo input for a key: another track's sound decides how much to compress
    void getChannelNames (juce::StringArray* ins, juce::StringArray* outs) override
    {
        if (ins != nullptr)  *ins = { "Left", "Right", "Key Left", "Key Right" };
        if (outs != nullptr) *outs = { "Left", "Right" };
    }

    BusLayout getBusses() const override
    {
        BusLayout b;
        b.inputs.push_back (te::ChannelConfiguration::stereo());
        b.inputs.push_back (te::ChannelConfiguration::stereo());
        b.outputs.push_back (te::ChannelConfiguration::stereo());
        return b;
    }

    bool isKeyed() const    { return getSidechainSourceID().isValid(); }

    juce::String getName() const override              { return "Elsewhere Compressor"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Compressor"; }
    int getDeviceViewWidth() const override            { return 600; }
    std::unique_ptr<juce::Component> createDeviceView() override;

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        rate = info.sampleRate;
        envDb = 0.0f;
        rmsState = 0.0f;
        optoSlow = 0.0f;
        hpfZ[0] = hpfZ[1] = hpfX[0] = hpfX[1] = 0.0f;
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr)
            return;

        juce::ScopedNoDenormals noDenormals;
        auto& buf = *fc.destBuffer;
        const int numCh = juce::jmin (2, buf.getNumChannels());
        if (numCh == 0) return;

        // With a key, the detector hears channels 3 and 4 (the engine adds them after ours)
        const bool keyed = buf.getNumChannels() >= 4 && isKeyed();
        const int keyCh = keyed ? 2 : 0;
        const bool listening = listen.get() > 0.5f;

        const auto m = (Model) juce::jlimit (0, numModels - 1, juce::roundToInt (model.get()));
        const float thr = threshold.get();
        const float rat = m == limiter ? 1000.0f : (ratio.get() >= 29.9f ? 1000.0f : ratio.get());
        const float kneeDb = m == limiter ? 0.5f : knee.get() * modelKneeScale (m);

        auto coeff = [this] (float ms) { return std::exp (-1.0f / (juce::jmax (0.01f, ms) * 0.001f * (float) rate)); };
        const float atk = coeff (m == limiter ? 0.05f : attack.get() * modelAttackScale (m));
        const float rel = coeff (m == limiter ? juce::jmin (release.get(), 80.0f) : release.get());
        const float rmsCoeff = coeff (m == vintage || m == opto ? 20.0f : 5.0f);
        const float optoSlowCoeff = coeff (1500.0f);

        // Auto make-up: roughly half the gain reduction a signal at 0 dB would get,
        // so perceived loudness stays about the same as you move threshold/ratio.
        float makeupDb = makeup.get();
        if (autoGain.get() > 0.5f && m != limiter)
            makeupDb += -0.5f * gainCurve (0.0f - 6.0f, thr, rat, kneeDb);
        if (m == limiter)
            makeupDb += -thr;

        const float makeupGain = juce::Decibels::decibelsToGain (makeupDb);
        const float outGain = juce::Decibels::decibelsToGain (output.get());
        const float wet = mix.get() / 100.0f;
        const float driveAmt = drive.get() / 100.0f + modelColour (m);

        // Sidechain one-pole high-pass
        const float hpA = std::exp (-juce::MathConstants<float>::twoPi * scHpf.get() / (float) rate);

        float maxGr = 0.0f, inPeak = 0.0f, outPeak = 0.0f;

        for (int i = 0; i < fc.bufferNumSamples; ++i)
        {
            const int idx = fc.bufferStartSample + i;

            // Detector: linked stereo, sidechain filtered
            float det = 0.0f;
            float heard[2] {};

            for (int ch = 0; ch < numCh; ++ch)
            {
                inPeak = juce::jmax (inPeak, std::abs (buf.getSample (ch, idx)));
                float x = buf.getSample (keyCh + ch, idx);
                float y = hpA * (hpfZ[ch] + x - hpfX[ch]);
                hpfX[ch] = x; hpfZ[ch] = y;
                heard[ch] = scHpf.get() > 21.0f ? y : x;
                det = juce::jmax (det, std::abs (heard[ch]));
            }

            float levelDb;
            if (m == vintage || m == opto || m == clean)
            {
                rmsState = rmsCoeff * rmsState + (1.0f - rmsCoeff) * det * det;
                levelDb = juce::Decibels::gainToDecibels (std::sqrt (rmsState) * 1.414f, -120.0f);
            }
            else
            {
                levelDb = juce::Decibels::gainToDecibels (det, -120.0f);
            }

            // Target gain reduction (dB, <= 0) and smoothing
            float target = gainCurve (levelDb, thr, rat, kneeDb);
            float c = target < envDb ? atk : rel;

            if (m == opto)
            {
                // Program dependent: release slows down the longer/harder it has been compressing
                optoSlow = optoSlowCoeff * optoSlow + (1.0f - optoSlowCoeff) * juce::jmin (1.0f, -envDb / 12.0f);
                if (target > envDb)
                    c = std::pow (rel, 1.0f / (1.0f + 3.0f * optoSlow));
            }

            envDb = c * envDb + (1.0f - c) * target;

            const float g = juce::Decibels::decibelsToGain (envDb) * makeupGain;
            maxGr = juce::jmin (maxGr, envDb);

            for (int ch = 0; ch < numCh; ++ch)
            {
                auto* d = buf.getWritePointer (ch);

                // Listen: what the detector hears (the key, filtered), to set the HPF by ear
                if (listening)
                {
                    d[idx] = heard[ch];
                    outPeak = juce::jmax (outPeak, std::abs (heard[ch]));
                    continue;
                }

                float dry = d[idx];
                float y = dry * g;

                if (driveAmt > 0.001f)
                    y = saturate (y, driveAmt, m);

                if (m == limiter)
                    y = juce::jlimit (-outCeiling, outCeiling, y);

                y = (dry * (1.0f - wet) + y * wet) * outGain;
                outPeak = juce::jmax (outPeak, std::abs (y));
                d[idx] = y;
            }
        }

        // Meters (read by the UI)
        grMeter.store (juce::jmin (grMeter.load(), maxGr));
        inMeter.store (juce::jmax (inMeter.load(), inPeak));
        outMeter.store (juce::jmax (outMeter.load(), outPeak));
    }

    /** Static curve, in dB of gain change (<= 0) for an input level. */
    static float gainCurve (float levelDb, float thr, float ratio, float kneeDb)
    {
        const float over = levelDb - thr;
        const float slope = 1.0f / ratio - 1.0f;

        if (kneeDb > 0.0f && std::abs (over) <= kneeDb * 0.5f)
        {
            const float t = over + kneeDb * 0.5f;
            return slope * t * t / (2.0f * kneeDb);
        }

        return over > 0.0f ? slope * over : 0.0f;
    }

    float getCurveDb (float inDb) const
    {
        auto m = (Model) juce::jlimit (0, numModels - 1, juce::roundToInt (model.get()));
        auto rat = m == limiter || ratio.get() >= 29.9f ? 1000.0f : ratio.get();
        return inDb + gainCurve (inDb, threshold.get(), rat, m == limiter ? 0.5f : knee.get() * modelKneeScale (m));
    }

    /** Takes (and resets) the meter peaks, for the UI. */
    float takeGainReductionDb()     { return grMeter.exchange (0.0f); }
    float takeInputDb()             { return juce::Decibels::gainToDecibels (inMeter.exchange (0.0f), -100.0f); }
    float takeOutputDb()            { return juce::Decibels::gainToDecibels (outMeter.exchange (0.0f), -100.0f); }

    Param model, threshold, ratio, attack, release, knee, makeup, autoGain, mix, scHpf, drive, output, listen;

private:
    double rate = 44100.0;
    float envDb = 0.0f, rmsState = 0.0f, optoSlow = 0.0f;
    float hpfZ[2] {}, hpfX[2] {};
    static constexpr float outCeiling = 0.989f;   // -0.1 dBFS

    std::atomic<float> grMeter { 0.0f }, inMeter { 0.0f }, outMeter { 0.0f };

    static float modelKneeScale (Model m)
    {
        switch (m)
        {
            case vca:       return 0.5f;
            case fet:       return 0.35f;
            case opto:      return 1.5f;
            case vintage:   return 2.0f;
            default:        return 1.0f;
        }
    }

    static float modelAttackScale (Model m)
    {
        switch (m)
        {
            case fet:       return 0.25f;   // FETs grab transients almost instantly
            case opto:      return 2.5f;    // optical cells are lazy
            case vintage:   return 1.5f;
            default:        return 1.0f;
        }
    }

    static float modelColour (Model m)
    {
        switch (m)
        {
            case fet:       return 0.08f;
            case vintage:   return 0.12f;
            case opto:      return 0.04f;
            default:        return 0.0f;
        }
    }

    static float saturate (float x, float amount, Model m)
    {
        const float k = 1.0f + amount * 6.0f;

        if (m == vintage)
        {
            // Asymmetric, tube-ish: adds even harmonics
            float y = std::tanh (k * (x + 0.1f * amount)) - std::tanh (k * 0.1f * amount);
            return y / k * (1.0f + amount);
        }

        return std::tanh (k * x) / k;   // unity gain for small signals, rounds off peaks
    }
};

//==============================================================================
/** The compressor's transfer curve with the live input level dot. */
class CompressorCurve : public juce::Component, private juce::Timer
{
public:
    CompressorCurve (CompressorDevice& d) : comp (d)    { startTimerHz (30); }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 4.0f);

        auto toX = [&] (float db) { return juce::jmap (db, -60.0f, 0.0f, r.getX() + 4, r.getRight() - 4); };
        auto toY = [&] (float db) { return juce::jmap (db, -60.0f, 0.0f, r.getBottom() - 4, r.getY() + 4); };

        g.setColour (Colours::gridLine);
        for (float db = -48; db < 0; db += 12)
        {
            g.drawVerticalLine ((int) toX (db), r.getY(), r.getBottom());
            g.drawHorizontalLine ((int) toY (db), r.getX(), r.getRight());
        }

        g.setColour (Colours::gridBar);
        g.drawLine (toX (-60), toY (-60), toX (0), toY (0), 1.0f);

        juce::Path p;
        for (float db = -60.0f; db <= 0.0f; db += 0.5f)
        {
            auto y = toY (juce::jmax (-60.0f, comp.getCurveDb (db)));
            if (db <= -60.0f) p.startNewSubPath (toX (db), y); else p.lineTo (toX (db), y);
        }
        g.setColour (Colours::accent);
        g.strokePath (p, juce::PathStrokeType (2.0f));

        // Threshold marker
        g.setColour (Colours::accent.withAlpha (0.35f));
        g.drawVerticalLine ((int) toX (comp.threshold.get()), r.getY(), r.getBottom());

        if (level > -60.0f)
        {
            g.setColour (Colours::text);
            g.fillEllipse (juce::Rectangle<float> (8, 8).withCentre ({ toX (level), toY (juce::jmax (-60.0f, comp.getCurveDb (level))) }));
        }
    }

    float level = -100.0f;

private:
    CompressorDevice& comp;
    void timerCallback() override   { repaint(); }
};

//==============================================================================
class CompressorView : public juce::Component, private juce::Timer
{
public:
    CompressorView (CompressorDevice& d)
        : comp (d), curve (d),
          modelChoice (d.model.param(), CompressorDevice::modelNames()),
          threshold (d.threshold.param()), ratio (d.ratio.param()), attack (d.attack.param()), release (d.release.param()),
          knee (d.knee.param()), makeup (d.makeup.param()), mix (d.mix.param()), scHpf (d.scHpf.param(), "SC HPF"),
          drive (d.drive.param(), "Drive", Colours::record), output (d.output.param()),
          autoGain (d.autoGain.param(), "Auto"), listen (d.listen.param(), "Listen")
    {
        for (juce::Component* c : { (juce::Component*) &curve, (juce::Component*) &modelChoice, (juce::Component*) &threshold,
                                    (juce::Component*) &ratio, (juce::Component*) &attack, (juce::Component*) &release,
                                    (juce::Component*) &knee, (juce::Component*) &makeup, (juce::Component*) &mix,
                                    (juce::Component*) &scHpf, (juce::Component*) &drive, (juce::Component*) &output,
                                    (juce::Component*) &autoGain, (juce::Component*) &listen, (juce::Component*) &inMeter, (juce::Component*) &grMeter,
                                    (juce::Component*) &outMeter })
            addAndMakeVisible (c);

        grMeter.invert = true;
        inMeter.getDb  = [this] { return inDb; };
        outMeter.getDb = [this] { return outDb; };
        grMeter.getDb  = [this] { return -grDb; };
        startTimerHz (30);
    }

    void paint (juce::Graphics& g) override
    {
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.0f, juce::Font::bold));
        auto w = meterArea.getWidth() / 3;
        int i = 0;
        for (auto* t : { "IN", "GR", "OUT" })
            g.drawText (t, meterArea.getX() + w * i++, meterArea.getBottom() - 12, w, 12, juce::Justification::centred);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        modelChoice.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);

        meterArea = r.removeFromRight (78);
        auto m = meterArea.withTrimmedBottom (14);
        auto w = m.getWidth() / 3;
        inMeter.setBounds (m.removeFromLeft (w).reduced (4, 0));
        grMeter.setBounds (m.removeFromLeft (w).reduced (4, 0));
        outMeter.setBounds (m.reduced (4, 0));
        r.removeFromRight (6);

        curve.setBounds (r.removeFromLeft (juce::jmin (r.getHeight(), 170)));
        r.removeFromLeft (6);

        auto row1 = r.removeFromTop (r.getHeight() / 2);
        auto kw = row1.getWidth() / 5;
        for (auto* k : { &threshold, &ratio, &attack, &release, &knee })
            k->setBounds (row1.removeFromLeft (kw));

        auto toggleArea = r.removeFromRight (kw).withSizeKeepingCentre (kw - 10, 50);
        autoGain.setBounds (toggleArea.removeFromTop (22));
        listen.setBounds (toggleArea.removeFromBottom (22));
        kw = r.getWidth() / 5;
        for (auto* k : { &makeup, &mix, &scHpf, &drive, &output })
            k->setBounds (r.removeFromLeft (kw));
    }

private:
    CompressorDevice& comp;
    CompressorCurve curve;
    DeviceUI::Choice modelChoice;
    DeviceUI::Knob threshold, ratio, attack, release, knee, makeup, mix, scHpf, drive, output;
    DeviceUI::Toggle autoGain, listen;
    DeviceUI::Meter inMeter, grMeter, outMeter;
    juce::Rectangle<int> meterArea;
    float inDb = -100.0f, outDb = -100.0f, grDb = 0.0f;

    void timerCallback() override
    {
        inDb = comp.takeInputDb();
        outDb = comp.takeOutputDb();
        grDb = comp.takeGainReductionDb();
        curve.level = inDb;
    }
};

inline std::unique_ptr<juce::Component> CompressorDevice::createDeviceView()    { return std::make_unique<CompressorView> (*this); }

JUCE_END_IGNORE_WARNINGS_GCC_LIKE
} // namespace elsewhere
