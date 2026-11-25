// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Widgets.h"

namespace elsewhere
{
//==============================================================================
/**
    Base class for Elsewhere's own devices (EQ, compressor, reverb, drums ...).

    Parameters are declared with addParameter(); each is a normal Tracktion automatable
    parameter backed by a property on the plugin's state, so automation, MIDI mapping,
    presets, undo and collaboration sync all work without any extra code.

    Each device also provides its own view for the device chain (and plugin window).
*/
class ElsewhereDevice : public te::Plugin
{
public:
    ElsewhereDevice (te::PluginCreationInfo info) : te::Plugin (info) {}

    ~ElsewhereDevice() override
    {
        notifyListenersOfDeletion();

        for (auto& p : params)
            p->reset();
    }

    //==============================================================================
    /** The device's panel, shown in the device chain and in its window. */
    virtual std::unique_ptr<juce::Component> createDeviceView() = 0;

    /** Width the panel would like in the device chain. */
    virtual int getDeviceViewWidth() const          { return 360; }

    /** A roomier panel for the device's own window, which has the space the chain doesn't.
        Devices without one show the chain's panel there too. */
    virtual std::unique_ptr<juce::Component> createWindowView()     { return {}; }

    /** The size the window panel would like to open at. */
    virtual juce::Point<int> getWindowViewSize() const              { return {}; }

    //==============================================================================
    int getNumOutputChannelsGivenInputs (int n) override   { return juce::jmin (n, 2); }
    BusLayout getBusses() const override                   { return BusLayout::singleStereoInOut(); }
    void deinitialise() override                           {}

    /** Held while a device's sound-making state is rebuilt (initialise) and tried, never waited
        for, by the audio thread: the engine can set a device up again for a new sample rate or
        buffer size while audio is still running through it. */
    juce::SpinLock dspLock;

    void restorePluginStateFromValueTree (const juce::ValueTree& v) override
    {
        for (auto& p : params)
            p->setFromValueTree (v);
    }

    std::unique_ptr<EditorComponent> createEditor() override;

    /** Set by the app: copies a file a device uses (e.g. a drum sample) into the project and returns the copy. */
    static inline std::function<juce::File (const juce::File&)> importFileHook;

    //==============================================================================
    /** A parameter, and a quick way to read its current value (safe on the audio thread). */
    struct Param
    {
        te::ParameterWithStateValue* p = nullptr;
        float get() const                           { return p->getCurrentValue(); }
        te::AutomatableParameter& param() const     { return p->getParameter(); }
        operator float() const                      { return get(); }
    };

protected:
    Param addParameter (const juce::String& id, const juce::String& name, float defaultValue,
                        juce::NormalisableRange<float> range,
                        std::function<juce::String (float)> toText = {})
    {
        if (! toText)
            toText = [] (float v) { return juce::String (v, 2); };

        auto p = std::make_unique<te::ParameterWithStateValue> (*this, id, juce::Identifier (id), name, defaultValue, range,
                                                                toText, [] (const juce::String& s) { return s.getFloatValue(); });
        params.push_back (std::move (p));
        return { params.back().get() };
    }

    // Common value formats
    static juce::String dbText (float v)        { return v <= -59.9f ? juce::String ("-inf dB") : juce::String (v, 1) + " dB"; }
    static juce::String hzText (float v)        { return v >= 1000.0f ? juce::String (v / 1000.0f, v >= 10000.0f ? 1 : 2) + " kHz" : juce::String (juce::roundToInt (v)) + " Hz"; }
    static juce::String msText (float v)        { return v >= 1000.0f ? juce::String (v / 1000.0f, 2) + " s" : juce::String (v, v < 10.0f ? 1 : 0) + " ms"; }
    static juce::String percentText (float v)   { return juce::String (juce::roundToInt (v)) + " %"; }
    static juce::String onOffText (float v)     { return v > 0.5f ? "On" : "Off"; }

    /** Frequency range with a logarithmic feel. */
    static juce::NormalisableRange<float> freqRange (float lo, float hi)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (std::sqrt (lo * hi));
        return r;
    }

    static juce::NormalisableRange<float> skewed (float lo, float hi, float centre)
    {
        juce::NormalisableRange<float> r (lo, hi);
        r.setSkewForCentre (centre);
        return r;
    }

    static juce::NormalisableRange<float> stepped (float lo, float hi)   { return { lo, hi, 1.0f }; }

    std::vector<std::unique_ptr<te::ParameterWithStateValue>> params;
};

//==============================================================================
/** Registry of Elsewhere devices, for the "Add Device" menu and the engine's plugin factory. */
namespace DeviceRegistry
{
    struct Entry
    {
        juce::String type, name, description;
        bool isInstrument = false;
        std::function<te::Plugin::Ptr (te::PluginCreationInfo)> create;
        bool isMidiEffect = false;
        bool hidden = false;    // still loads in old projects, but not offered in the menu
    };

    const std::vector<Entry>& getAll();

    inline bool isMidiEffect (const juce::String& type)
    {
        for (auto& e : getAll())
            if (e.type == type)
                return e.isMidiEffect;
        return false;
    }

    /** Creates a device by type (via a state tree, which is how the engine creates custom plugins). */
    inline te::Plugin::Ptr create (te::Edit& edit, const juce::String& type)
    {
        juce::ValueTree v (te::IDs::PLUGIN);
        v.setProperty (te::IDs::type, type, nullptr);
        return edit.getPluginCache().createNewPlugin (v);
    }
}

//==============================================================================
namespace DeviceUI
{
    /** Knob with its name above and value below, bound to a parameter. */
    class Knob : public juce::Component
    {
    public:
        Knob (te::AutomatableParameter& p, const juce::String& labelText = {}, juce::Colour colour = Colours::accent)
            : slider (p, juce::Slider::RotaryHorizontalVerticalDrag), param (&p)
        {
            slider.setColour (juce::Slider::rotarySliderFillColourId, colour);
            slider.setPopupDisplayEnabled (false, false, nullptr);
            slider.onValueChange = [this] { updateValue(); };
            addAndMakeVisible (slider);

            name.setText (labelText.isNotEmpty() ? labelText : p.getParameterName(), juce::dontSendNotification);
            name.setJustificationType (juce::Justification::centred);
            name.setFont (juce::FontOptions (11.0f));
            name.setColour (juce::Label::textColourId, Colours::textDim);
            name.setMinimumHorizontalScale (0.6f);
            addAndMakeVisible (name);

            value.setJustificationType (juce::Justification::centred);
            value.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            value.setColour (juce::Label::textColourId, valueColour (colour));
            addAndMakeVisible (value);

            updateValue();
            timer.startTimerHz (10);
        }

        void lookAndFeelChanged() override
        {
            value.setColour (juce::Label::textColourId, valueColour (slider.findColour (juce::Slider::rotarySliderFillColourId)));
        }

        static juce::Colour valueColour (juce::Colour knobColour)
        {
            return Theme::readable (Theme::isLight() ? knobColour.darker (0.35f) : knobColour.brighter (0.3f));
        }

        void resized() override
        {
            auto r = getLocalBounds();
            name.setBounds (r.removeFromTop (14));
            value.setBounds (r.removeFromBottom (14));
            slider.setBounds (r.reduced (2));
        }

    private:
        ParameterSlider slider;
        te::AutomatableParameter::Ptr param;
        juce::Label name, value;
        te::LambdaTimer timer { [this] { updateValue(); } };

        void updateValue()  { value.setText (param->valueToString (param->getCurrentBaseValue()), juce::dontSendNotification); }
    };

    /** Row of buttons choosing one value of a stepped parameter (e.g. compressor model). */
    class Choice : public juce::Component
    {
    public:
        Choice (te::AutomatableParameter& p, juce::StringArray options, juce::Colour colour = Colours::accent)
            : param (&p), names (std::move (options)), accent (colour)
        {
            timer.startTimerHz (10);
        }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            auto w = r.getWidth() / (float) names.size();
            auto current = juce::roundToInt (param->getCurrentValue());

            for (int i = 0; i < names.size(); ++i)
            {
                auto cell = juce::Rectangle<float> (r.getX() + w * (float) i, r.getY(), w, r.getHeight()).reduced (1.0f);
                bool on = i == current;
                g.setColour (on ? accent : Colours::panelDark);
                g.fillRoundedRectangle (cell, 3.0f);
                g.setColour (on ? Colours::textInverse : Colours::textDim);
                g.setFont (juce::FontOptions (11.0f, on ? juce::Font::bold : juce::Font::plain));
                g.drawFittedText (names[i], cell.toNearestInt().reduced (2), juce::Justification::centred, 2);
            }
        }

        void mouseDown (const juce::MouseEvent& e) override
        {
            auto i = juce::jlimit (0, names.size() - 1, e.x * names.size() / juce::jmax (1, getWidth()));
            param->parameterChangeGestureBegin();
            param->setParameter ((float) i, juce::sendNotification);
            param->parameterChangeGestureEnd();
            repaint();
        }

    private:
        te::AutomatableParameter::Ptr param;
        juce::StringArray names;
        juce::Colour accent;
        int last = -1;
        te::LambdaTimer timer { [this] { auto v = juce::roundToInt (param->getCurrentValue()); if (v != last) { last = v; repaint(); } } };
    };

    /** On/off toggle bound to a 0/1 parameter. */
    class Toggle : public juce::Component
    {
    public:
        Toggle (te::AutomatableParameter& p, const juce::String& text, juce::Colour colour = Colours::accent)
            : param (&p), label (text), accent (colour)
        {
            timer.startTimerHz (10);
        }

        void paint (juce::Graphics& g) override
        {
            bool on = param->getCurrentValue() > 0.5f;
            auto r = getLocalBounds().toFloat().reduced (1.0f);
            g.setColour (on ? accent : Colours::panelDark);
            g.fillRoundedRectangle (r, 3.0f);
            g.setColour (on ? Colours::textInverse : Colours::textDim);
            g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
            g.drawText (label, r.toNearestInt(), juce::Justification::centred);
        }

        void mouseDown (const juce::MouseEvent&) override
        {
            param->parameterChangeGestureBegin();
            param->setParameter (param->getCurrentValue() > 0.5f ? 0.0f : 1.0f, juce::sendNotification);
            param->parameterChangeGestureEnd();
            repaint();
        }

    private:
        te::AutomatableParameter::Ptr param;
        juce::String label;
        juce::Colour accent;
        bool last = false;
        te::LambdaTimer timer { [this] { bool v = param->getCurrentValue() > 0.5f; if (v != last) { last = v; repaint(); } } };
    };

    /** Simple vertical meter fed with a level in dB. */
    class Meter : public juce::Component
    {
    public:
        std::function<float()> getDb;
        bool invert = false;       // gain-reduction style: fills from the top
        juce::Colour colour = Colours::meterGreen;

        Meter() { timer.startTimerHz (30); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().toFloat();
            g.setColour (Colours::panelDark);
            g.fillRoundedRectangle (r, 2.0f);
            auto prop = juce::jlimit (0.0f, 1.0f, invert ? shown / 24.0f : (shown + 60.0f) / 66.0f);
            auto fill = invert ? r.withHeight (r.getHeight() * prop) : r.withTop (r.getBottom() - r.getHeight() * prop);
            g.setColour (invert ? Colours::accent : (shown > -0.5f ? Colours::meterRed : shown > -9.0f ? Colours::meterYellow : colour));
            g.fillRoundedRectangle (fill, 2.0f);
        }

    private:
        float shown = -100.0f;
        te::LambdaTimer timer { [this]
        {
            if (! getDb) return;
            auto v = getDb();
            shown = invert ? juce::jmax (v, shown * 0.85f) : juce::jmax (v, shown - 1.5f);
            repaint();
        } };
    };

    /** Panel background with a section title. */
    inline void paintSection (juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title)
    {
        g.setColour (Colours::panelDark.withAlpha (0.6f));
        g.fillRoundedRectangle (r.toFloat(), 4.0f);

        if (title.isNotEmpty())
        {
            g.setColour (Colours::textDim);
            g.setFont (juce::FontOptions (10.0f, juce::Font::bold));
            g.drawText (title.toUpperCase(), r.removeFromTop (16).reduced (6, 0), juce::Justification::centredLeft);
        }
    }
}

//==============================================================================
/**
    Responsive device windows. A long device (the synths) wraps into rows when its window is
    narrower than it is, like a web page: views say where their sections are, so rows are only
    ever cut between sections, and every row is a live copy of the view showing its part.
*/
namespace Reflow
{
    struct Sections
    {
        virtual ~Sections() = default;
        /** The sections, in this component's coordinates. */
        virtual std::vector<juce::Rectangle<int>> getLayoutSections() const = 0;
        /** The width the view wants to be laid out at (when it isn't the device's usual width). */
        virtual int getNaturalWidth() const     { return -1; }
    };

    /** Horizontal spans nothing may be cut through: sections, and any other component. */
    inline void collectBlocks (juce::Component& c, juce::Component& root, std::vector<juce::Range<int>>& blocks)
    {
        std::vector<juce::Rectangle<int>> secs;
        if (auto* s = dynamic_cast<Sections*> (&c))
            secs = s->getLayoutSections();
        for (auto& r : secs)
            blocks.push_back (root.getLocalArea (&c, r).getHorizontalRange());

        for (auto* ch : c.getChildren())
        {
            if (! ch->isVisible()) continue;
            const auto b = ch->getBounds();
            if (std::any_of (secs.begin(), secs.end(), [&] (const juce::Rectangle<int>& r) { return r.contains (b); }))
                continue;
            if (auto* vp = dynamic_cast<juce::Viewport*> (ch); vp != nullptr && vp->getViewedComponent() != nullptr)
                collectBlocks (*vp->getViewedComponent(), root, blocks);
            else if (dynamic_cast<Sections*> (ch) != nullptr)
                collectBlocks (*ch, root, blocks);
            else
                blocks.push_back (root.getLocalArea (&c, b).getHorizontalRange());
        }
    }

    /** Splits a view of width `total` into rows no wider than `maxWidth` (where it can). */
    inline std::vector<juce::Range<int>> rows (juce::Component& view, int total, int maxWidth)
    {
        std::vector<juce::Range<int>> blocks;
        collectBlocks (view, view, blocks);
        std::vector<int> cuts;
        for (auto& b : blocks)
        {
            const int x = b.getStart();
            if (x <= 0 || x >= total) continue;
            if (std::none_of (blocks.begin(), blocks.end(), [x] (const juce::Range<int>& o) { return o.getStart() < x && o.getEnd() > x; }))
                cuts.push_back (x);
        }
        std::sort (cuts.begin(), cuts.end());
        cuts.erase (std::unique (cuts.begin(), cuts.end()), cuts.end());

        std::vector<juce::Range<int>> out;
        int start = 0;
        while (start < total)
        {
            int end = total;
            if (start + maxWidth < total)
            {
                int best = -1;
                for (int c : cuts) if (c > start && c <= start + maxWidth) best = c;
                if (best < 0) for (int c : cuts) if (c > start) { best = c; break; }
                end = best > 0 ? best : total;
            }
            out.push_back ({ start, end });
            start = end;
        }
        return out;
    }
}

//==============================================================================
/** Plugin window content for an Elsewhere device: its view, wrapped into rows when the window is narrow. */
struct ElsewhereDeviceEditor : public te::Plugin::EditorComponent
{
    ElsewhereDeviceEditor (ElsewhereDevice& d) : device (d)
    {
        if ((windowView = d.createWindowView()) != nullptr)
        {
            addAndMakeVisible (*windowView);
            const auto size = d.getWindowViewSize();
            setSize (size.x + 2 * pad, size.y + 2 * pad);
            return;
        }

        addRowView();
        setSize (juce::jmax (420, naturalWidth() + 2 * pad), 300);
    }

    bool allowWindowResizing() override                                 { return true; }
    juce::ComponentBoundsConstrainer* getBoundsConstrainer() override    { return nullptr; }
    void paint (juce::Graphics& g) override                             { g.fillAll (Colours::panel); }

    void resized() override
    {
        if (windowView != nullptr)
        {
            windowView->setBounds (getLocalBounds().reduced (pad));
            return;
        }

        const int natural = naturalWidth();
        const int avail = getWidth() - 2 * pad;

        if (avail >= natural || natural <= 0)
        {
            // Room for it all: one view, as wide and tall as the window
            for (size_t i = 1; i < clips.size(); ++i) clips[i]->setVisible (false);
            clips[0]->setBounds (getLocalBounds().reduced (pad));
            views[0]->setBounds (clips[0]->getLocalBounds());
            if (wrapped) { wrapped = false; resizeHeight (rowH + 2 * pad); }
            return;
        }

        // Narrower than the device: rows, cut between sections
        views[0]->setBounds (0, 0, natural, rowH);
        const auto spans = Reflow::rows (*views[0], natural, avail);
        while (views.size() < spans.size()) addRowView();
        for (size_t i = 0; i < clips.size(); ++i)
        {
            const bool used = i < spans.size();
            clips[i]->setVisible (used);
            if (! used) continue;
            clips[i]->setBounds (pad, pad + (int) i * (rowH + gap), spans[i].getLength(), rowH);
            views[i]->setBounds (-spans[i].getStart(), 0, natural, rowH);
        }
        wrapped = true;
        resizeHeight (pad * 2 + (int) spans.size() * rowH + ((int) spans.size() - 1) * gap);
    }

private:
    static constexpr int pad = 8, gap = 8, rowH = 174;
    ElsewhereDevice& device;
    std::vector<std::unique_ptr<juce::Component>> views, clips;
    std::unique_ptr<juce::Component> windowView;    // the device's own window panel, when it has one
    bool wrapped = false;

    int naturalWidth() const
    {
        if (auto* s = dynamic_cast<Reflow::Sections*> (views[0].get()))
            if (s->getNaturalWidth() > 0) return s->getNaturalWidth();
        return device.getDeviceViewWidth();
    }

    void addRowView()
    {
        auto clip = std::make_unique<juce::Component>();
        auto view = device.createDeviceView();
        clip->addAndMakeVisible (*view);
        addAndMakeVisible (*clip);
        views.push_back (std::move (view));
        clips.push_back (std::move (clip));
    }

    /** The window follows its content: grow or shrink to fit the rows (after this layout pass). */
    void resizeHeight (int h)
    {
        if (getHeight() == h) return;
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<ElsewhereDeviceEditor> (this), h]
        {
            if (safe != nullptr) safe->setSize (safe->getWidth(), h);
        });
    }
};

inline std::unique_ptr<te::Plugin::EditorComponent> ElsewhereDevice::createEditor()
{
    return std::make_unique<ElsewhereDeviceEditor> (*this);
}

} // namespace elsewhere
