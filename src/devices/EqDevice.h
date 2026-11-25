// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"
#include "Dsp.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere EQ: 8-band parametric EQ with a high-pass, low shelf, four bells,
    high shelf and low-pass, a response graph and a spectrum analyzer.
*/
class EqDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereEQ";
    static constexpr int numBands = 8;

    struct BandInfo { const char* name; Biquad::Type type; float freq; float q; bool on; };
    static const BandInfo& bandInfo (int i)
    {
        static const BandInfo info[numBands] = {
            { "Low Cut",    Biquad::Type::highPass,  30.0f,    0.71f, false },
            { "Low",        Biquad::Type::lowShelf,  80.0f,    0.71f, true  },
            { "Low Mid",    Biquad::Type::bell,      200.0f,   0.8f,  true  },
            { "Mid",        Biquad::Type::bell,      600.0f,   0.8f,  true  },
            { "Upper Mid",  Biquad::Type::bell,      2000.0f,  0.8f,  true  },
            { "Presence",   Biquad::Type::bell,      5000.0f,  0.8f,  true  },
            { "High",       Biquad::Type::highShelf, 10000.0f, 0.71f, true  },
            { "High Cut",   Biquad::Type::lowPass,   18000.0f, 0.71f, false },
        };
        return info[i];
    }

    struct Band { Param on, freq, gain, q, slope; };

    EqDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        for (int i = 0; i < numBands; ++i)
        {
            auto& b = bandInfo (i);
            auto id = "b" + juce::String (i + 1);
            auto nm = juce::String (b.name) + " ";
            bands[(size_t) i].on   = addParameter (id + "on", nm + "On", b.on ? 1.0f : 0.0f, stepped (0, 1), onOffText);
            bands[(size_t) i].freq = addParameter (id + "freq", nm + "Frequency", b.freq, freqRange (20.0f, 20000.0f), hzText);

            if (isFilter (i))
                bands[(size_t) i].slope = addParameter (id + "slope", nm + "Slope", 1.0f, stepped (0, 2),
                                                        [] (float v) { return juce::String (12 << juce::roundToInt (v)) + " dB/oct"; });
            else
                bands[(size_t) i].gain = addParameter (id + "gain", nm + "Gain", 0.0f, { -24.0f, 24.0f }, dbText);

            bands[(size_t) i].q = addParameter (id + "q", nm + "Q", b.q, skewed (0.1f, 18.0f, 1.0f),
                                                [] (float v) { return juce::String (v, 2); });
        }

        outputGain = addParameter ("output", "Output", 0.0f, { -24.0f, 24.0f }, dbText);
    }

    static bool isFilter (int band)     { return band == 0 || band == numBands - 1; }

    juce::String getName() const override              { return "Elsewhere EQ"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere EQ"; }
    int getDeviceViewWidth() const override            { return 720; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    std::unique_ptr<juce::Component> createWindowView() override;
    juce::Point<int> getWindowViewSize() const override    { return { 1040, 600 }; }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        // (The engine can do this while audio is still running through the device)
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        rate = info.sampleRate;
        for (auto& stages : filters) for (auto& f : stages) f.reset();
        cachedHash = 0;
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked())
            return;     // being set up again for a new rate or buffer size

        if (fc.destBuffer == nullptr)
            return;

        juce::ScopedNoDenormals noDenormals;
        updateCoefficientsIfNeeded();

        auto& buf = *fc.destBuffer;
        const int numCh = juce::jmin (2, buf.getNumChannels());
        const float out = juce::Decibels::decibelsToGain (outputGain.get());

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto* d = buf.getWritePointer (ch, fc.bufferStartSample);

            for (int i = 0; i < fc.bufferNumSamples; ++i)
            {
                float x = d[i];

                for (int b = 0; b < numBands; ++b)
                    if (activeStages[(size_t) b] > 0)
                        for (int s = 0; s < activeStages[(size_t) b]; ++s)
                            x = filters[(size_t) b][(size_t) s].process (x, ch);

                d[i] = x * out;
            }
        }

        // Feed the analyzer (mono sum, after the EQ)
        if (analyzerFifo.getFreeSpace() >= fc.bufferNumSamples)
        {
            int s1, n1, s2, n2;
            analyzerFifo.prepareToWrite (fc.bufferNumSamples, s1, n1, s2, n2);
            auto copy = [&] (int dest, int src, int n)
            {
                for (int i = 0; i < n; ++i)
                {
                    float v = 0.0f;
                    for (int ch = 0; ch < numCh; ++ch)
                        v += buf.getSample (ch, fc.bufferStartSample + src + i);
                    analyzerData[(size_t) (dest + i)] = v / (float) juce::jmax (1, numCh);
                }
            };
            copy (s1, 0, n1);
            if (n2 > 0) copy (s2, n1, n2);
            analyzerFifo.finishedWrite (n1 + n2);
        }
    }

    /** Response of the whole EQ at a frequency (UI thread, from the current parameter values). */
    double getResponseDb (double freq) const
    {
        double total = outputGain.get();
        auto sr = rate > 0 ? rate : 48000.0;

        for (int b = 0; b < numBands; ++b)
        {
            if (bands[(size_t) b].on.get() < 0.5f)
                continue;

            Biquad f;
            configure (f, b, sr);
            total += f.magnitudeDb (freq, sr) * stagesFor (b);
        }

        return total;
    }

    /** Takes analyzer samples (UI thread). */
    int readAnalyzer (float* dest, int max)
    {
        int s1, n1, s2, n2;
        analyzerFifo.prepareToRead (juce::jmin (max, analyzerFifo.getNumReady()), s1, n1, s2, n2);
        std::copy (analyzerData.begin() + s1, analyzerData.begin() + s1 + n1, dest);
        if (n2 > 0) std::copy (analyzerData.begin() + s2, analyzerData.begin() + s2 + n2, dest + n1);
        analyzerFifo.finishedRead (n1 + n2);
        return n1 + n2;
    }

    double getRate() const  { return rate > 0 ? rate : 48000.0; }

    std::array<Band, numBands> bands;
    Param outputGain;

private:
    double rate = 0;
    std::array<std::array<Biquad, 4>, numBands> filters;
    std::array<int, numBands> activeStages {};
    size_t cachedHash = 0;

    juce::AbstractFifo analyzerFifo { 16384 };
    std::array<float, 16384> analyzerData {};

    int stagesFor (int b) const
    {
        if (! isFilter (b))
            return 1;

        return 1 << juce::jlimit (0, 2, juce::roundToInt (bands[(size_t) b].slope.get()));   // 12/24/48 dB/oct
    }

    void configure (Biquad& f, int b, double sr) const
    {
        auto& band = bands[(size_t) b];
        auto gain = isFilter (b) ? 0.0 : (double) band.gain.get();
        f.set (bandInfo (b).type, band.freq.get(), band.q.get(), gain, sr);
    }

    void updateCoefficientsIfNeeded()
    {
        // Only recompute when something changed (cheap hash of the band values)
        size_t h = std::hash<double>{} (rate);
        for (int b = 0; b < numBands; ++b)
        {
            auto& band = bands[(size_t) b];
            for (float v : { band.on.get(), band.freq.get(), band.q.get(),
                             isFilter (b) ? band.slope.get() : band.gain.get() })
                h = h * 31 + std::hash<float>{} (v);
        }

        if (h == cachedHash)
            return;

        cachedHash = h;

        for (int b = 0; b < numBands; ++b)
        {
            auto on = bands[(size_t) b].on.get() > 0.5f;
            auto stages = on ? stagesFor (b) : 0;

            if (stages > activeStages[(size_t) b])
                for (int s = activeStages[(size_t) b]; s < stages; ++s)
                    filters[(size_t) b][(size_t) s].reset();

            for (int s = 0; s < stages; ++s)
            {
                // Butterworth-ish cascade: keep the band's Q on each stage for a sharper knee
                auto z1 = filters[(size_t) b][(size_t) s].z1, z2 = filters[(size_t) b][(size_t) s].z2;
                configure (filters[(size_t) b][(size_t) s], b, getRate());
                std::copy (z1, z1 + 2, filters[(size_t) b][(size_t) s].z1);
                std::copy (z2, z2 + 2, filters[(size_t) b][(size_t) s].z2);
            }

            activeStages[(size_t) b] = stages;
        }
    }
};

//==============================================================================
/** The EQ's graph: response curve, draggable band handles and a spectrum analyzer. */
class EqGraph : public juce::Component, private juce::Timer
{
public:
    EqGraph (EqDevice& d) : eq (d)
    {
        startTimerHz (30);
    }

    /** Bigger handles, for when the graph has a window to itself. */
    void setLarge (bool shouldBeLarge)      { handleSize = shouldBeLarge ? 16.0f : 12.0f; repaint(); }

    static juce::Colour bandColour (int b)
    {
        static const juce::uint32 c[] = { 0xff8b8e94, 0xffff6b6b, 0xffff9a3c, 0xfff3d34a, 0xff9ad64b, 0xff4cc9a6, 0xff4fb3ff, 0xff8b8e94 };
        return juce::Colour (c[b]);
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 4.0f);

        // Grid
        g.setFont (juce::FontOptions (10.0f));
        for (double f : { 20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0 })
        {
            auto x = freqToX (f);
            g.setColour (Colours::gridLine);
            g.drawVerticalLine (juce::roundToInt (x), r.getY(), r.getBottom());
            g.setColour (Colours::textDim);
            g.drawText (f >= 1000 ? juce::String ((int) (f / 1000)) + "k" : juce::String ((int) f),
                        juce::Rectangle<float> (x + 2, r.getBottom() - 14, 30, 12).toNearestInt(), juce::Justification::left);
        }

        for (int db = -18; db <= 18; db += 6)
        {
            auto y = dbToY ((float) db);
            g.setColour (db == 0 ? Colours::gridBar : Colours::gridLine);
            g.drawHorizontalLine (juce::roundToInt (y), r.getX(), r.getRight());
            g.setColour (Colours::textDim);
            g.drawText (juce::String (db), juce::Rectangle<float> (r.getRight() - 26, y - 6, 22, 12).toNearestInt(), juce::Justification::right);
        }

        // Spectrum
        if (! spectrum.empty())
        {
            juce::Path p;
            p.startNewSubPath (r.getX(), r.getBottom());
            for (size_t i = 0; i < spectrum.size(); ++i)
                p.lineTo ((float) i * r.getWidth() / (float) (spectrum.size() - 1),
                          juce::jmap (spectrum[i], -90.0f, 0.0f, r.getBottom(), r.getY() + r.getHeight() * 0.1f));
            p.lineTo (r.getRight(), r.getBottom());
            p.closeSubPath();
            g.setColour (Colours::solo.withAlpha (0.13f));
            g.fillPath (p);
        }

        // Response curve
        juce::Path curve;
        for (int x = 0; x <= getWidth(); x += 2)
        {
            auto y = dbToY ((float) eq.getResponseDb (xToFreq ((float) x)));
            if (x == 0) curve.startNewSubPath ((float) x, y); else curve.lineTo ((float) x, y);
        }

        juce::Path fill (curve);
        fill.lineTo (r.getRight(), dbToY (0)); fill.lineTo (0, dbToY (0)); fill.closeSubPath();
        g.setColour (Colours::accent.withAlpha (0.12f));
        g.fillPath (fill);
        g.setColour (Colours::accent);
        g.strokePath (curve, juce::PathStrokeType (2.0f));

        // Band handles
        for (int b = 0; b < EqDevice::numBands; ++b)
        {
            auto pos = handlePos (b);
            bool on = eq.bands[(size_t) b].on.get() > 0.5f;
            auto c = bandColour (b);
            g.setColour (on ? c : c.withAlpha (0.3f));
            g.fillEllipse (juce::Rectangle<float> (handleSize, handleSize).withCentre (pos));
            g.setColour (juce::Colours::black.withAlpha (0.6f));
            g.setFont (juce::FontOptions (handleSize * 0.75f, juce::Font::bold));
            g.drawText (juce::String (b + 1), juce::Rectangle<float> (handleSize, handleSize).withCentre (pos).toNearestInt(), juce::Justification::centred);

            if (b == dragging || b == hovered)
            {
                g.setColour (Colours::text);
                g.drawEllipse (juce::Rectangle<float> (handleSize + 4.0f, handleSize + 4.0f).withCentre (pos), 1.5f);
            }
        }

        if (hovered >= 0)
        {
            auto& band = eq.bands[(size_t) hovered];
            juce::String info = juce::String (EqDevice::bandInfo (hovered).name) + "  " + band.freq.param().getCurrentValueAsString();
            if (! EqDevice::isFilter (hovered)) info << "  " << band.gain.param().getCurrentValueAsString();
            info << "  Q " << juce::String (band.q.get(), 2);
            g.setColour (Colours::text);
            g.setFont (juce::FontOptions (11.0f));
            g.drawText (info, getLocalBounds().reduced (8, 4), juce::Justification::topLeft);
        }
    }

    void mouseMove (const juce::MouseEvent& e) override
    {
        auto h = findHandle (e.position);
        if (h != hovered) { hovered = h; repaint(); }
    }

    void mouseExit (const juce::MouseEvent&) override      { hovered = -1; repaint(); }

    void mouseDown (const juce::MouseEvent& e) override
    {
        dragging = findHandle (e.position);

        if (dragging < 0)
            return;

        auto& band = eq.bands[(size_t) dragging];
        band.freq.param().parameterChangeGestureBegin();
        if (! EqDevice::isFilter (dragging)) band.gain.param().parameterChangeGestureBegin();

        // Dragging a band that's off turns it on
        if (band.on.get() < 0.5f)
            band.on.param().setParameter (1.0f, juce::sendNotification);
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (dragging < 0)
            return;

        auto& band = eq.bands[(size_t) dragging];
        band.freq.param().setParameter ((float) juce::jlimit (20.0, 20000.0, xToFreq (e.position.x)), juce::sendNotification);

        if (! EqDevice::isFilter (dragging))
            band.gain.param().setParameter (juce::jlimit (-24.0f, 24.0f, yToDb (e.position.y)), juce::sendNotification);

        repaint();
    }

    void mouseUp (const juce::MouseEvent&) override
    {
        if (dragging >= 0)
        {
            auto& band = eq.bands[(size_t) dragging];
            band.freq.param().parameterChangeGestureEnd();
            if (! EqDevice::isFilter (dragging)) band.gain.param().parameterChangeGestureEnd();
        }

        dragging = -1;
    }

    void mouseDoubleClick (const juce::MouseEvent& e) override
    {
        auto h = findHandle (e.position);
        if (h < 0) return;
        auto& on = eq.bands[(size_t) h].on.param();
        on.setParameter (on.getCurrentValue() > 0.5f ? 0.0f : 1.0f, juce::sendNotification);
        repaint();
    }

    void mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w) override
    {
        auto h = hovered >= 0 ? hovered : findHandle (e.position);
        if (h < 0) return;
        auto& q = eq.bands[(size_t) h].q.param();
        q.setParameter (juce::jlimit (0.1f, 18.0f, q.getCurrentValue() * std::pow (1.25f, w.deltaY * 4.0f)), juce::sendNotification);
        repaint();
    }

private:
    EqDevice& eq;
    int dragging = -1, hovered = -1;
    float handleSize = 12.0f;
    std::vector<float> spectrum;
    juce::dsp::FFT fft { 12 };
    std::vector<float> ring = std::vector<float> (4096, 0.0f), fftData = std::vector<float> (8192, 0.0f);
    int ringPos = 0;

    static constexpr float minDb = -24.0f, maxDb = 24.0f;

    float freqToX (double f) const  { return (float) (std::log (f / 20.0) / std::log (1000.0)) * (float) getWidth(); }
    double xToFreq (float x) const  { return 20.0 * std::pow (1000.0, (double) x / juce::jmax (1, getWidth())); }
    float dbToY (float db) const    { return juce::jmap (db, maxDb, minDb, 6.0f, (float) getHeight() - 16.0f); }
    float yToDb (float y) const     { return juce::jmap (y, 6.0f, (float) getHeight() - 16.0f, maxDb, minDb); }

    juce::Point<float> handlePos (int b) const
    {
        auto& band = eq.bands[(size_t) b];
        auto y = EqDevice::isFilter (b) ? dbToY (0.0f) : dbToY (band.gain.get());
        return { freqToX (band.freq.get()), y };
    }

    int findHandle (juce::Point<float> p) const
    {
        for (int b = EqDevice::numBands; --b >= 0;)
            if (handlePos (b).getDistanceFrom (p) < handleSize - 2.0f)
                return b;

        return -1;
    }

    void timerCallback() override
    {
        // Pull new audio for the analyzer and compute a smoothed, log-spaced spectrum
        float incoming[4096];
        auto n = eq.readAnalyzer (incoming, 4096);

        for (int i = 0; i < n; ++i)
        {
            ring[(size_t) ringPos] = incoming[i];
            ringPos = (ringPos + 1) % (int) ring.size();
        }

        if (n > 0)
        {
            std::fill (fftData.begin(), fftData.end(), 0.0f);

            for (size_t i = 0; i < ring.size(); ++i)
            {
                auto w = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::twoPi * (float) i / (float) ring.size());
                fftData[i] = ring[(size_t) ((ringPos + (int) i) % (int) ring.size())] * w;
            }

            fft.performFrequencyOnlyForwardTransform (fftData.data());

            auto points = juce::jmax (2, getWidth() / 3);
            if (spectrum.size() != (size_t) points) spectrum.assign ((size_t) points, -120.0f);

            for (int i = 0; i < points; ++i)
            {
                auto f = xToFreq ((float) i * (float) getWidth() / (float) (points - 1));
                auto bin = juce::jlimit (1, 2047, (int) std::round (f / eq.getRate() * 4096.0));
                auto db = juce::Decibels::gainToDecibels (fftData[(size_t) bin] / 1024.0f, -120.0f);
                spectrum[(size_t) i] = juce::jmax (db, spectrum[(size_t) i] - 2.0f);
            }
        }
        else
        {
            for (auto& s : spectrum) s = juce::jmax (-120.0f, s - 2.0f);
        }

        repaint();
    }
};

//==============================================================================
class EqView : public juce::Component
{
public:
    EqView (EqDevice& d) : graph (d), output (d.outputGain.param(), "Output")
    {
        addAndMakeVisible (graph);
        addAndMakeVisible (output);

        for (int b = 0; b < EqDevice::numBands; ++b)
            addAndMakeVisible (toggles.add (new DeviceUI::Toggle (d.bands[(size_t) b].on.param(), juce::String (b + 1), EqGraph::bandColour (b))));
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto right = r.removeFromRight (70);
        output.setBounds (right.removeFromBottom (80));
        auto toggleRow = r.removeFromBottom (20);
        graph.setBounds (r.withTrimmedBottom (4));

        auto w = toggleRow.getWidth() / EqDevice::numBands;
        for (auto* t : toggles)
            t->setBounds (toggleRow.removeFromLeft (w).reduced (2, 0));
    }

private:
    EqGraph graph;
    DeviceUI::Knob output;
    juce::OwnedArray<DeviceUI::Toggle> toggles;
};

//==============================================================================
/** The EQ in its own window: the graph across the top, and under it every band's own
    controls, so a band can be set by number as well as by dragging its handle. */
class EqWindowView : public juce::Component
{
public:
    EqWindowView (EqDevice& d) : graph (d), output (d.outputGain.param(), "Output")
    {
        graph.setLarge (true);
        addAndMakeVisible (graph);
        addAndMakeVisible (output);

        for (int b = 0; b < EqDevice::numBands; ++b)
        {
            auto& band = d.bands[(size_t) b];
            const auto colour = EqGraph::bandColour (b);
            auto* s = strips.add (new Strip());

            s->on = std::make_unique<DeviceUI::Toggle> (band.on.param(), juce::String (b + 1) + "  " + EqDevice::bandInfo (b).name, colour);
            s->freq = std::make_unique<DeviceUI::Knob> (band.freq.param(), "Freq", colour);
            s->q = std::make_unique<DeviceUI::Knob> (band.q.param(), "Q", colour);

            if (EqDevice::isFilter (b))
                s->slope = std::make_unique<DeviceUI::Choice> (band.slope.param(), juce::StringArray { "12", "24", "48" }, colour);
            else
                s->gain = std::make_unique<DeviceUI::Knob> (band.gain.param(), "Gain", colour);

            for (juce::Component* c : { (juce::Component*) s->on.get(), (juce::Component*) s->freq.get(), (juce::Component*) s->q.get(),
                                        (juce::Component*) s->slope.get(), (juce::Component*) s->gain.get() })
                if (c != nullptr)
                    addAndMakeVisible (c);
        }
    }

    void paint (juce::Graphics& g) override
    {
        for (auto* s : strips)
        {
            DeviceUI::paintSection (g, s->area, {});

            if (s->slope != nullptr)
            {
                g.setColour (Colours::textDim);
                g.setFont (juce::FontOptions (11.0f));
                g.drawText ("Slope", s->slopeLabel, juce::Justification::centred);
                g.drawText ("dB/oct", s->slopeLabel.translated (0, s->slopeLabel.getHeight() + s->slope->getHeight() + 4), juce::Justification::centred);
            }
        }

        DeviceUI::paintSection (g, outputArea, "Output");

        // (a band that is off is the colour of the panel it sits on, so each switch gets an edge)
        g.setColour (Colours::textDim.withAlpha (0.3f));
        for (auto* s : strips)
            g.drawRoundedRectangle (s->on->getBounds().toFloat().reduced (0.5f), 3.5f, 1.0f);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        auto lower = r.removeFromBottom (juce::jmin (206, r.getHeight() / 2));
        r.removeFromBottom (8);
        graph.setBounds (r);

        outputArea = lower.removeFromRight (84);
        output.setBounds (outputArea.reduced (6).withTrimmedTop (12).withSizeKeepingCentre (outputArea.getWidth() - 12, juce::jmin (96, outputArea.getHeight() - 30)));

        const int w = lower.getWidth() / EqDevice::numBands;
        for (auto* s : strips)
        {
            s->area = lower.removeFromLeft (w).withTrimmedRight (8);
            auto c = s->area.reduced (6);
            s->on->setBounds (c.removeFromTop (22));
            c.removeFromTop (6);

            auto lowerRow = c.removeFromBottom (juce::jmin (70, c.getHeight() / 2));
            s->freq->setBounds (c.withSizeKeepingCentre (c.getWidth(), juce::jmin (96, c.getHeight())));

            auto left = lowerRow.removeFromLeft (lowerRow.getWidth() / 2);
            if (s->gain != nullptr)
            {
                s->gain->setBounds (left);
            }
            else
            {
                // (the slope sits where a bell's gain does, with its name over it like a knob's)
                auto block = left.withSizeKeepingCentre (left.getWidth(), 14 + 22 + 4 + 14);
                s->slopeLabel = block.removeFromTop (14);
                s->slope->setBounds (block.removeFromTop (22));
            }
            s->q->setBounds (lowerRow);
        }
    }

private:
    struct Strip
    {
        std::unique_ptr<DeviceUI::Toggle> on;
        std::unique_ptr<DeviceUI::Knob> freq, gain, q;
        std::unique_ptr<DeviceUI::Choice> slope;
        juce::Rectangle<int> area, slopeLabel;
    };

    EqGraph graph;
    DeviceUI::Knob output;
    juce::OwnedArray<Strip> strips;
    juce::Rectangle<int> outputArea;
};

inline std::unique_ptr<juce::Component> EqDevice::createDeviceView()    { return std::make_unique<EqView> (*this); }
inline std::unique_ptr<juce::Component> EqDevice::createWindowView()    { return std::make_unique<EqWindowView> (*this); }

} // namespace elsewhere
