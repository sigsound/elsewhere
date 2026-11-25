// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"
#include "SynthCore.h"

namespace elsewhere
{
//==============================================================================
/**
    Flat panel parts shared by Elsewhere's synths: the LED display, compact knobs, bar
    sliders, step selectors (with waveform glyphs) and dot toggles.

    Every control reports when you touch it (onTouch), so the synth's display can show
    the parameter's name and value while you turn it, like a hardware synth's screen.
*/
namespace SynthPanel
{
    using TouchFn = std::function<void (te::AutomatableParameter&)>;

    //==============================================================================
    /**
        An OLED-style display: mint light on black (it stays a dark screen in both themes,
        like the hardware). Shows a live oscilloscope and the patch name; while a control is
        being touched, its name, value and a bar instead. Drawn at full resolution: clean
        type and a smooth trace.
    */
    class Oled : public juce::Component, private juce::Timer
    {
    public:
        std::function<void (float*, int)> readScope;     // latest output samples
        std::function<float()> getSamplesPerCycle;       // for a steady scope picture
        std::function<void (juce::Graphics&, juce::Rectangle<float>)> drawBody;   // instead of the scope (e.g. a sample's waveform)
        juce::String header, patchName;

        static juce::Colour ink()      { return juce::Colour (0xff8ef5cb); }
        static juce::Colour dim()      { return ink().withAlpha (0.35f); }
        static juce::Colour faint()    { return ink().withAlpha (0.12f); }

        Oled()                          { startTimerHz (30); }

        void showParameter (te::AutomatableParameter& p)
        {
            touched = &p;
            customName = {};
            touchedUntil = juce::Time::getMillisecondCounterHiRes() + 1400.0;
        }

        /** The same readout for values that aren't automatable parameters (e.g. a module's knob). */
        void showValue (const juce::String& name, const juce::String& value, float proportion)
        {
            touched = nullptr;
            customName = name; customValue = value; customNorm = proportion;
            touchedUntil = juce::Time::getMillisecondCounterHiRes() + 1400.0;
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (juce::Colour (0xff060908));
            g.fillRoundedRectangle (r, 5.0f);

            // Type and margins grow with a big screen (a device's own window); in the chain they stay as they were
            const float s = juce::jlimit (1.0f, 2.0f, r.getHeight() / 160.0f);
            auto c = r.reduced (10.0f * s, 8.0f * s);
            auto top = c.removeFromTop (12.0f * s);
            auto bottom = c.removeFromBottom (16.0f * s);

            g.setFont (juce::FontOptions (9.5f * s, juce::Font::bold).withKerningFactor (0.12f));
            g.setColour (dim());
            g.drawText (header, top, juce::Justification::centredLeft);

            const bool recent = juce::Time::getMillisecondCounterHiRes() < touchedUntil;
            const bool showParam = recent && (touched != nullptr || customName.isNotEmpty());

            if (showParam)
            {
                const float norm = touched != nullptr ? touched->valueRange.convertTo0to1 (touched->getCurrentBaseValue()) : customNorm;
                const auto name = touched != nullptr ? touched->getParameterName() : customName;
                const auto value = touched != nullptr ? touched->getCurrentValueAsString() : customValue;
                auto area = c.reduced (0.0f, 4.0f * s);
                g.setColour (dim());
                g.setFont (juce::FontOptions (10.5f * s, juce::Font::bold).withKerningFactor (0.08f));
                g.drawText (name.toUpperCase(), area.removeFromTop (16.0f * s), juce::Justification::centredLeft);
                g.setColour (ink());
                g.setFont (juce::FontOptions (22.0f * s));
                g.drawFittedText (value, area.removeFromTop (30.0f * s).toNearestInt(), juce::Justification::centredLeft, 1, 0.7f);

                auto bar = bottom.withSizeKeepingCentre (bottom.getWidth(), 3.0f * s);
                g.setColour (faint());
                g.fillRoundedRectangle (bar, 1.5f * s);
                g.setColour (ink());
                g.fillRoundedRectangle (bar.withWidth (juce::jmax (3.0f, bar.getWidth() * juce::jlimit (0.0f, 1.0f, norm))), 1.5f * s);
                return;
            }

            if (drawBody)
                drawBody (g, c.reduced (0.0f, 4.0f * s));

            // Oscilloscope: about two cycles, starting at a rising zero crossing
            else if (readScope)
            {
                const float spc = getSamplesPerCycle ? juce::jlimit (8.0f, 900.0f, getSamplesPerCycle()) : 200.0f;
                const int window = juce::jlimit (32, 1024, (int) (spc * 2.0f));
                const int total = window + (int) spc + 2;
                readScope (scope.data(), total);

                int start = 0;
                for (int i = 1; i < (int) spc + 1; ++i)
                    if (scope[(size_t) (i - 1)] <= 0.0f && scope[(size_t) i] > 0.0f) { start = i; break; }

                float peak = 0.05f;
                for (int i = 0; i < window; ++i) peak = juce::jmax (peak, std::abs (scope[(size_t) (start + i)]));

                auto area = c.reduced (0.0f, 6.0f * s);
                const float mid = area.getCentreY(), amp = area.getHeight() * 0.46f / peak;
                const int points = juce::jmax (2, (int) area.getWidth());
                juce::Path p;
                for (int x = 0; x < points; ++x)
                {
                    const float v = scope[(size_t) (start + x * window / points)];
                    const float px = area.getX() + (float) x, py = mid - v * amp;
                    if (x == 0) p.startNewSubPath (px, py); else p.lineTo (px, py);
                }

                g.setColour (faint());
                g.fillRect (area.getX(), mid, area.getWidth(), 1.0f);
                g.setColour (ink());
                g.strokePath (p, juce::PathStrokeType (1.4f * std::sqrt (s), juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }

            g.setColour (ink());
            g.setFont (juce::FontOptions (12.5f * s, juce::Font::bold));
            g.drawFittedText (patchName, bottom.toNearestInt(), juce::Justification::centredLeft, 1, 0.8f);
        }

    private:
        te::AutomatableParameter::Ptr touched;
        juce::String customName, customValue;
        float customNorm = 0.0f;
        double touchedUntil = 0.0;
        std::array<float, 2048> scope {};

        void timerCallback() override
        {
            if (isShowing())
                repaint();
        }
    };

    //==============================================================================
    /** Reports touches on a ParameterSlider (mouse down, drag, wheel). */
    struct TouchListener : public juce::MouseListener
    {
        te::AutomatableParameter& param;
        TouchFn& onTouch;
        TouchListener (te::AutomatableParameter& p, TouchFn& fn) : param (p), onTouch (fn) {}
        void mouseDown (const juce::MouseEvent&) override                                  { if (onTouch) onTouch (param); }
        void mouseDrag (const juce::MouseEvent&) override                                  { if (onTouch) onTouch (param); }
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override { if (onTouch) onTouch (param); }
    };

    /** Compact knob: the flat dot knob with its name underneath (the value shows on the display). */
    class Knob : public juce::Component
    {
    public:
        TouchFn onTouch;

        Knob (te::AutomatableParameter& p, juce::String labelText, juce::Colour colour)
            : slider (p, juce::Slider::RotaryHorizontalVerticalDrag), label (std::move (labelText)), touch (p, onTouch)
        {
            slider.setColour (juce::Slider::rotarySliderFillColourId, colour);
            slider.setPopupDisplayEnabled (false, false, nullptr);
            slider.addMouseListener (&touch, false);
            addAndMakeVisible (slider);
        }

        ~Knob() override    { slider.removeMouseListener (&touch); }

        int maxSize = 38;   // knob diameter limit; bigger for panels with few, large knobs
        bool roomy = false; // in the device's own window, where the cells are bigger: a larger knob

        void paint (juce::Graphics& g) override
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (10.0f));
            g.drawFittedText (label, labelArea, juce::Justification::centred, 1, 0.7f);
        }

        void resized() override
        {
            // Knob and name as one block, centred in the cell, with the name just under the knob
            auto r = getLocalBounds();
            const int labelH = 12, gap = 2;
            const int d = juce::jmin (r.getWidth(), r.getHeight() - labelH - gap, roomy ? maxSize * 5 / 4 + 2 : maxSize);
            const int top = r.getY() + (r.getHeight() - (d + gap + labelH)) / 2;
            slider.setBounds (r.getCentreX() - d / 2, top, d, d);
            labelArea = { r.getX(), top + d + gap, r.getWidth(), labelH };
        }

    private:
        ParameterSlider slider;
        juce::String label;
        juce::Rectangle<int> labelArea;
        TouchListener touch;
    };

    //==============================================================================
    /** Flat horizontal bar slider (for mixer levels). */
    class BarSlider : public ParameterSlider
    {
    public:
        TouchFn onTouch;
        juce::Colour colour;

        BarSlider (te::AutomatableParameter& p, juce::Colour c)
            : ParameterSlider (p, juce::Slider::LinearBar), colour (c), touch (p, onTouch)
        {
            setPopupDisplayEnabled (false, false, nullptr);
            addMouseListener (&touch, false);
        }

        ~BarSlider() override   { removeMouseListener (&touch); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat().withSizeKeepingCentre ((float) getWidth(), juce::jmin (8.0f, (float) getHeight()));
            g.setColour (Colours::panelDark);
            g.fillRoundedRectangle (r, 2.0f);
            const float norm = (float) valueToProportionOfLength (getValue());
            g.setColour (colour);
            g.fillRoundedRectangle (r.withWidth (juce::jmax (2.0f, r.getWidth() * norm)), 2.0f);
        }

    private:
        TouchListener touch;
    };

    //==============================================================================
    /** Glyph for a waveform, drawn into a cell. */
    inline void drawWave (juce::Graphics& g, SynthCore::Wave w, juce::Rectangle<float> r, float thickness = 1.4f)
    {
        r = r.reduced (r.getWidth() * 0.18f, r.getHeight() * 0.28f);
        const float x0 = r.getX(), x1 = r.getRight(), y0 = r.getY(), y1 = r.getBottom(), xm = r.getCentreX(), ym = r.getCentreY();
        juce::Path p;
        using W = SynthCore::Wave;
        auto pulse = [&] (float width)
        {
            const float xe = x0 + (x1 - x0) * width;
            p.startNewSubPath (x0, y1); p.lineTo (x0, y0); p.lineTo (xe, y0); p.lineTo (xe, y1); p.lineTo (x1, y1); p.lineTo (x1, y0);
        };

        switch (w)
        {
            case W::triangle:    p.startNewSubPath (x0, y1); p.lineTo (xm, y0); p.lineTo (x1, y1); break;
            case W::shark:       p.startNewSubPath (x0, y1); p.quadraticTo (x0 + (x1 - x0) * 0.75f, y0 - (y1 - y0) * 0.1f, x1 - 1.0f, y0); p.lineTo (x1, y1); break;
            case W::saw:         p.startNewSubPath (x0, y1); p.lineTo (x1, y0); p.lineTo (x1, y1); break;
            case W::reverseSaw:  p.startNewSubPath (x0, y1); p.lineTo (x0, y0); p.lineTo (x1, y1); break;
            case W::square:      pulse (0.5f); break;
            case W::widePulse:   pulse (0.32f); break;
            case W::narrowPulse: pulse (0.14f); break;
            case W::sine:
                for (int i = 0; i <= 16; ++i)
                {
                    const float x = x0 + (x1 - x0) * (float) i / 16.0f, y = ym - (y1 - y0) * 0.5f * std::sin ((float) i / 16.0f * SynthCore::twoPi);
                    if (i == 0) p.startNewSubPath (x, y); else p.lineTo (x, y);
                }
                break;
        }
        g.strokePath (p, juce::PathStrokeType (thickness, juce::PathStrokeType::mitered, juce::PathStrokeType::square));
    }

    /**
        A row of cells choosing one value of a stepped parameter: text labels, or
        waveform glyphs. Flat: the selected cell is a solid block of colour.
    */
    class Selector : public juce::Component
    {
    public:
        TouchFn onTouch;

        Selector (te::AutomatableParameter& p, juce::StringArray labels, juce::Colour c)
            : param (&p), names (std::move (labels)), colour (c)
        {
            timer.startTimerHz (10);
        }

        Selector (te::AutomatableParameter& p, std::vector<SynthCore::Wave> waveGlyphs, juce::Colour c)
            : param (&p), waves (std::move (waveGlyphs)), colour (c)
        {
            timer.startTimerHz (10);
        }

        int count() const   { return waves.empty() ? names.size() : (int) waves.size(); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            const float w = r.getWidth() / (float) count();
            const int current = juce::roundToInt (param->getCurrentValue() - param->valueRange.start);

            for (int i = 0; i < count(); ++i)
            {
                auto cell = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f, 0.0f);
                const bool on = i == current;
                g.setColour (on ? colour : Colours::panelDark);
                g.fillRoundedRectangle (cell, 2.5f);
                g.setColour (on ? Colours::textInverse : Colours::textDim);

                if (waves.empty())
                {
                    g.setFont (juce::FontOptions (10.0f, on ? juce::Font::bold : juce::Font::plain));
                    g.drawFittedText (names[i], cell.toNearestInt(), juce::Justification::centred, 1, 0.6f);
                }
                else
                    drawWave (g, waves[(size_t) i], cell, 1.3f);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            const int i = juce::jlimit (0, count() - 1, e.x * count() / juce::jmax (1, getWidth()));
            param->parameterChangeGestureBegin();
            param->setParameter (param->valueRange.start + (float) i, juce::sendNotification);
            param->parameterChangeGestureEnd();
            if (onTouch) onTouch (*param);
            repaint();
        }

    private:
        te::AutomatableParameter::Ptr param;
        juce::StringArray names;
        std::vector<SynthCore::Wave> waves;
        juce::Colour colour;
        int last = -1;
        te::LambdaTimer timer { [this] { auto v = juce::roundToInt (param->getCurrentValue()); if (v != last) { last = v; repaint(); } } };
    };

    //==============================================================================
    /** A dot and a label: click to toggle a 0/1 parameter. */
    class Toggle : public juce::Component
    {
    public:
        TouchFn onTouch;

        Toggle (te::AutomatableParameter& p, juce::String text, juce::Colour c)
            : param (&p), label (std::move (text)), colour (c)
        {
            timer.startTimerHz (10);
        }

        void paint (juce::Graphics& g) override
        {
            const bool on = param->getCurrentValue() > 0.5f;
            auto r = getLocalBounds().toFloat();
            auto dot = r.removeFromLeft (r.getHeight()).withSizeKeepingCentre (9.0f, 9.0f);
            g.setColour (on ? colour : Colours::panelDark);
            g.fillEllipse (dot);
            g.setColour (on ? Colours::text : Colours::textDim);
            g.setFont (juce::FontOptions (10.0f));
            g.drawFittedText (label, r.toNearestInt().withTrimmedLeft (2), juce::Justification::centredLeft, 1, 0.7f);
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            param->parameterChangeGestureBegin();
            param->setParameter (param->getCurrentValue() > 0.5f ? 0.0f : 1.0f, juce::sendNotification);
            param->parameterChangeGestureEnd();
            if (onTouch) onTouch (*param);
            repaint();
        }

    private:
        te::AutomatableParameter::Ptr param;
        juce::String label;
        juce::Colour colour;
        bool last = false;
        te::LambdaTimer timer { [this] { bool v = param->getCurrentValue() > 0.5f; if (v != last) { last = v; repaint(); } } };
    };

    //==============================================================================
    /** Helpers for building a synth panel: creates touch-reporting controls and keeps them. */
    class PanelBase : public juce::Component, public Reflow::Sections
    {
    public:
        TouchFn onTouch;

        std::vector<juce::Rectangle<int>> getLayoutSections() const override    { return sections; }

        /**
            For the device's own window: columns() stacks the sections in this many rows
            (each filling the width) instead of one strip, and the knobs may grow.
        */
        void setWindowRows (int rows)
        {
            windowRows = rows;
            for (auto& c : owned)
                if (auto* k = dynamic_cast<Knob*> (c.get()))
                    k->roomy = rows > 0;
        }

    protected:
        /** The panel's sections (set by columns(), or by the panel itself), for wrapping into rows. */
        std::vector<juce::Rectangle<int>> sections;
        int windowRows = 0;

        std::vector<std::unique_ptr<juce::Component>> owned;

        template <typename T, typename... Args>
        T* make (Args&&... args)
        {
            auto c = std::make_unique<T> (std::forward<Args> (args)...);
            c->onTouch = [this] (te::AutomatableParameter& p) { if (onTouch) onTouch (p); };
            if (auto* k = dynamic_cast<Knob*> (c.get()))
                k->roomy = windowRows > 0;
            addAndMakeVisible (*c);
            auto* raw = c.get();
            owned.push_back (std::move (c));
            return raw;
        }

        Knob* knob (ElsewhereDevice::Param p, const char* name, juce::Colour c)       { return make<Knob> (p.param(), name, c); }
        Toggle* toggle (ElsewhereDevice::Param p, const char* name, juce::Colour c)   { return make<Toggle> (p.param(), name, c); }
        BarSlider* bar (ElsewhereDevice::Param p, juce::Colour c)                     { return make<BarSlider> (p.param(), c); }

        Selector* selector (ElsewhereDevice::Param p, juce::StringArray names, juce::Colour c)
        {
            return make<Selector> (p.param(), std::move (names), c);
        }

        Selector* waves (ElsewhereDevice::Param p, std::vector<SynthCore::Wave> w, juce::Colour c)
        {
            return make<Selector> (p.param(), std::move (w), c);
        }

        static juce::Rectangle<int> content (juce::Rectangle<int> a)    { a.removeFromTop (16); return a.reduced (6, 2).withTrimmedBottom (2); }

        template <typename T>
        static void row (juce::Rectangle<int> r, std::initializer_list<T*> items, int slots)
        {
            const int w = r.getWidth() / slots;
            for (auto* k : items)
                k->setBounds (r.removeFromLeft (w));
        }

        /** Splits a row into sections with the given relative widths. */
        std::vector<juce::Rectangle<int>> columns (juce::Rectangle<int> r, std::initializer_list<int> weights, int gap = 6)
        {
            if (windowRows > 1)
                return stacked (r, weights, windowRows, 8);

            int total = 0;
            for (auto w : weights) total += w;
            const int avail = r.getWidth() - gap * ((int) weights.size() - 1);
            std::vector<juce::Rectangle<int>> out;
            for (auto w : weights)
            {
                out.push_back (r.removeFromLeft (avail * w / total));
                r.removeFromLeft (gap);
            }
            sections = out;
            return out;
        }

        /**
            The same sections in several rows, for a window: the order is kept, the rows are
            cut where they come out most even, and every row fills the width.
        */
        static std::vector<juce::Rectangle<int>> stacked (juce::Rectangle<int> r, const std::vector<int>& weights, int rows, int gap)
        {
            const int n = (int) weights.size();
            rows = juce::jlimit (1, juce::jmax (1, n), rows);

            // Where each row ends: every way of cutting is tried (there are only a few sections)
            std::vector<int> ends, best;
            int bestWidest = std::numeric_limits<int>::max();
            std::function<void (int, int)> cut = [&] (int from, int widest)
            {
                if ((int) ends.size() == rows - 1)
                {
                    int sum = 0;
                    for (int i = from; i < n; ++i) sum += weights[(size_t) i];
                    widest = juce::jmax (widest, sum);
                    if (widest < bestWidest) { bestWidest = widest; best = ends; }
                    return;
                }
                int sum = 0;
                for (int end = from + 1; end <= n - (rows - 1 - (int) ends.size()); ++end)
                {
                    sum += weights[(size_t) end - 1];
                    ends.push_back (end);
                    cut (end, juce::jmax (widest, sum));
                    ends.pop_back();
                }
            };
            cut (0, 0);
            best.push_back (n);

            std::vector<juce::Rectangle<int>> out;
            const int rowH = (r.getHeight() - gap * (rows - 1)) / rows;
            int from = 0;
            for (int end : best)
            {
                auto line = r.removeFromTop (rowH);
                r.removeFromTop (gap);
                int total = 0;
                for (int i = from; i < end; ++i) total += weights[(size_t) i];
                const int avail = line.getWidth() - gap * (end - from - 1);
                for (int i = from; i < end; ++i)
                {
                    out.push_back (i == end - 1 ? line : line.removeFromLeft (avail * weights[(size_t) i] / juce::jmax (1, total)));
                    line.removeFromLeft (gap);
                }
                from = end;
            }
            return out;
        }
    };

    //==============================================================================
    /**
        Shows a panel enlarged, for a device's own window: the panel is laid out in a
        smaller space and scaled up to fill this one, so its controls and type all grow together.
        A window made smaller than it opens at gives the enlargement up first (down to actual
        size), so the panel keeps its room for as long as it can.
    */
    class Zoomed : public juce::Component
    {
    public:
        Zoomed (std::unique_ptr<juce::Component> panel, float zoomToUse, juce::Point<int> openSize)
            : inner (std::move (panel)), zoom (zoomToUse), open (openSize)
        {
            addAndMakeVisible (*inner);
        }

        void resized() override
        {
            const float fit = juce::jmin ((float) getWidth() / (float) juce::jmax (1, open.x), (float) getHeight() / (float) juce::jmax (1, open.y));
            const float z = juce::jlimit (1.0f, zoom, zoom * fit);
            inner->setTransform (juce::AffineTransform::scale (z));
            inner->setBounds (0, 0, juce::roundToInt ((float) getWidth() / z), juce::roundToInt ((float) getHeight() / z));
        }

    private:
        std::unique_ptr<juce::Component> inner;
        float zoom;
        juce::Point<int> open;
    };

    //==============================================================================
    /** Section background and title, flat. */
    inline void paintSection (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title, juce::Colour colour)
    {
        g.setColour (Colours::panelDark.withAlpha (0.55f));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);
        auto t = r.removeFromTop (16).reduced (7, 0);
        g.setColour (colour);
        g.fillRect (t.removeFromLeft (3).withSizeKeepingCentre (3, 8));
        t.removeFromLeft (5);
        g.setColour (Colours::textDim);
        g.setFont (juce::FontOptions (9.5f, juce::Font::bold));
        g.drawText (title.toUpperCase(), t, juce::Justification::centredLeft);
    }
}
} // namespace elsewhere
