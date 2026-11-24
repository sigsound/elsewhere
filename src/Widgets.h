// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Theme.h"
#include "Accessibility.h"

namespace elsewhere
{
//==============================================================================
/** Peak meter fed by a track's LevelMeterPlugin (or any LevelMeasurer). */
class LevelMeter : public juce::Component, private juce::Timer
{
public:
    LevelMeter (te::LevelMeasurer& m, bool vertical = true) : measurer (&m), isVertical (vertical)
    {
        measurer->addClient (client);
        startTimerHz (30);
        setInterceptsMouseClicks (false, false);
    }

    // Screen readers skip meters: a value that changes 30 times a second can't be read out
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return createIgnoredAccessibilityHandler (*this);
    }

    ~LevelMeter() override
    {
        if (measurer != nullptr)
            measurer->removeClient (client);
    }

    void paint (juce::Graphics& g) override
    {
        // Orbit: a thin capsule per channel, grey while it's fine, ink when it's loud, red when it clips
        auto r = getLocalBounds().toFloat();

        const int numCh = 2;
        const float gap = 1.0f;

        for (int ch = 0; ch < numCh; ++ch)
        {
            auto n = (float) numCh, c = (float) ch;
            auto chR = isVertical ? r.withWidth ((r.getWidth() - gap) / n).withX (r.getX() + c * (r.getWidth() + gap) / n)
                                  : r.withHeight ((r.getHeight() - gap) / n).withY (r.getY() + c * (r.getHeight() + gap) / n);

            const auto round = juce::jmin (chR.getWidth(), chR.getHeight()) * 0.5f;
            g.setColour (Colours::panelLight);
            g.fillRoundedRectangle (chR, round);

            auto level = dbToProportion (levels[ch]);
            auto fill = isVertical ? chR.withTop (chR.getBottom() - chR.getHeight() * level)
                                   : chR.withWidth (chR.getWidth() * level);

            auto colour = levels[ch] > -0.5f ? Colours::meterRed
                        : levels[ch] > -9.0f ? Colours::meterYellow
                                             : Colours::meterGreen;
            g.setColour (Theme::indicator (colour));
            g.fillRoundedRectangle (fill, round);

            // Peak hold line
            auto peak = dbToProportion (peaks[ch]);
            g.setColour (Colours::text.withAlpha (0.7f));

            if (isVertical)
                g.fillRect (chR.withHeight (1.0f).withY (chR.getBottom() - chR.getHeight() * peak));
            else
                g.fillRect (chR.withWidth (1.0f).withX (chR.getX() + chR.getWidth() * peak));
        }
    }

private:
    te::LevelMeasurer* measurer;
    te::LevelMeasurer::Client client;
    bool isVertical;
    float levels[2] { -100.0f, -100.0f }, peaks[2] { -100.0f, -100.0f };
    int peakHoldFrames[2] {};

    static float dbToProportion (float db)
    {
        // -60 dB .. +6 dB, with a slightly curved scale so the useful range is bigger
        auto p = juce::jlimit (0.0f, 1.0f, (db + 60.0f) / 66.0f);
        return std::pow (p, 1.6f);
    }

    void timerCallback() override
    {
        bool changed = false;

        for (int ch = 0; ch < 2; ++ch)
        {
            auto newLevel = client.getAndClearAudioLevel (ch).dB;
            // Smooth decay
            auto decayed = levels[ch] - 1.5f;
            auto l = juce::jmax (newLevel, decayed);

            if (l > peaks[ch]) { peaks[ch] = l; peakHoldFrames[ch] = 45; }
            else if (--peakHoldFrames[ch] <= 0) peaks[ch] = juce::jmax (-100.0f, peaks[ch] - 1.0f);

            if (std::abs (l - levels[ch]) > 0.05f)
                changed = true;

            levels[ch] = l;
        }

        if (changed)
            repaint();
    }
};

//==============================================================================
/** Small toggle button for track activators (M / S / ● ). The title is what screen readers
    say ("Mute"); the on/off state is read out with it. */
class SmallToggle : public juce::TextButton
{
public:
    SmallToggle (const juce::String& text, juce::Colour on, const juce::String& accessibleTitle = {}) : juce::TextButton (text)
    {
        setClickingTogglesState (false);
        setToggleable (true);
        setTitle (accessibleTitle);
        setColour (juce::TextButton::buttonOnColourId, on);
        setColour (juce::TextButton::buttonColourId, Colours::panelLight);
        setColour (juce::TextButton::textColourOnId, Colours::textInverse);
        setColour (juce::TextButton::textColourOffId, Colours::textDim);
    }
};

//==============================================================================
/** A slider bound to an AutomatableParameter, e.g. a track's volume or pan. */
/** "Map" mode for modulators: while active, clicking a knob picks it as a target. */
namespace ModulationMap
{
    struct State
    {
        std::function<void (te::AutomatableParameter&)> onPick;
        juce::String sourceName;
    };

    inline State& get()                 { static State s; return s; }
    inline bool isActive()              { return get().onPick != nullptr; }
    inline void begin (const juce::String& source, std::function<void (te::AutomatableParameter&)> f)
    {
        get().sourceName = source;
        get().onPick = std::move (f);
    }
    inline void cancel()                { get().onPick = nullptr; }

    inline void pick (te::AutomatableParameter& p)
    {
        auto f = std::move (get().onPick);
        get().onPick = nullptr;
        if (f) f (p);
    }
}

class ParameterSlider : public juce::Slider, private te::AutomatableParameter::Listener, private juce::Timer
{
public:
    ParameterSlider (te::AutomatableParameter& p, SliderStyle style) : param (&p)
    {
        setSliderStyle (style);
        setTextBoxStyle (juce::Slider::NoTextBox, true, 0, 0);
        // Keep the parameter's skew (e.g. logarithmic-feeling frequency knobs)
        auto& vr = p.valueRange;
        setNormalisableRange ({ (double) vr.start, (double) vr.end, (double) vr.interval,
                                (double) vr.skew, vr.symmetricSkew });

        if (auto def = p.getDefaultValue())
            setDoubleClickReturnValue (true, *def);

        setValue (p.getCurrentBaseValue(), juce::dontSendNotification);
        param->addListener (this);
        setPopupDisplayEnabled (true, true, nullptr);
        startTimerHz (20);   // MIDI-map / modulation-map highlighting, modulation display

        // Changing a value takes a drag (or the keyboard). The wheel passes through to whatever
        // scrolls underneath, so moving around a project never nudges a fader or knob.
        setScrollWheelEnabled (false);

        // Reachable with Tab and adjustable with the arrow keys, but a click doesn't take the
        // keyboard away from the shortcuts
        setTitle (p.getParameterName());
        setWantsKeyboardFocus (true);
        setMouseClickGrabsKeyboardFocus (false);
    }

    ~ParameterSlider() override
    {
        param->removeListener (this);
    }

    juce::String getTextFromValue (double) override       { return param->getCurrentValueAsStringWithLabel(); }

    te::Edit* getEdit() const
    {
        if (auto p = param->getPlugin())
            return &p->edit;

        return nullptr;
    }

    bool isMidiMapped() const
    {
        if (auto e = getEdit())
            return e->getParameterControlMappings().isParameterMapped (*param);

        return false;
    }

    void paintOverChildren (juce::Graphics& g) override
    {
        auto learning = getEdit() != nullptr && getEdit()->engine.getMidiLearnState().isActive();

        if (learning)
        {
            g.setColour (Colours::solo.withAlpha (0.25f));
            g.fillRect (getLocalBounds());
        }

        if (ModulationMap::isActive())
        {
            g.setColour (Colours::modulation.withAlpha (isMouseOver() ? 0.45f : 0.2f));
            g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
        }

        // Modulated: show where a modulator is moving it right now
        if (param->hasActiveModifierAssignments() && isRotary())
        {
            auto rp = getRotaryParameters();
            auto bounds = getLocalBounds().toFloat().reduced (3.0f);
            auto radius = juce::jmin (bounds.getWidth(), bounds.getHeight()) / 2.0f;
            auto centre = bounds.getCentre();
            auto angle = [&] (double v) { return rp.startAngleRadians + (float) valueToProportionOfLength (v) * (rp.endAngleRadians - rp.startAngleRadians); };
            auto a0 = angle (param->getCurrentBaseValue()), a1 = angle (param->getCurrentValue());
            auto r = radius + 1.5f;

            juce::Path arc;
            arc.addCentredArc (centre.x, centre.y, r, r, 0.0f, juce::jmin (a0, a1), juce::jmax (a0, a1), true);
            g.setColour (Colours::modulation);
            g.strokePath (arc, juce::PathStrokeType (1.5f));
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre ({ centre.x + r * std::sin (a1), centre.y - r * std::cos (a1) }));
        }

        if (isMidiMapped())
        {
            g.setColour (Colours::solo);
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withPosition ((float) getWidth() - 5.0f, 0.0f));
        }
    }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        if (auto ed = getEdit(); ed != nullptr && ed->engine.getMidiLearnState().isActive())
            return;

        juce::Slider::mouseDrag (e);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        // Modulator "Map" mode: this knob becomes the target
        if (ModulationMap::isActive() && ! e.mods.isPopupMenu())
        {
            ModulationMap::pick (*param);
            repaint();
            return;
        }

        // In MIDI Map mode a click picks this control as the target, without changing it
        if (auto ed = getEdit(); ed != nullptr && ed->engine.getMidiLearnState().isActive() && ! e.mods.isPopupMenu())
        {
            ed->getParameterChangeHandler().parameterChanged (*param, false);
            return;
        }

        if (e.mods.isPopupMenu() && isMidiMapped())
        {
            juce::PopupMenu m;
            m.addItem ("Remove MIDI Mapping", [sp = SafePointer<ParameterSlider> (this)]
            {
                if (sp != nullptr)
                    if (auto ed = sp->getEdit())
                    {
                        ed->getParameterControlMappings().removeParameterMapping (*sp->param);
                        sp->repaint();
                    }
            });
            m.showMenuAsync ({});
            return;
        }

        juce::Slider::mouseDown (e);
    }

    /** Arrow keys move it by 1% of its travel (Shift: 0.1%), so knobs with a skewed range (e.g.
        frequency) step as evenly as they turn, and stepped ones always move to the next step. */
    bool keyPressed (const juce::KeyPress& k) override
    {
        const auto mods = k.getModifiers();

        if (mods.isCommandDown() || mods.isAltDown() || mods.isCtrlDown())
            return false;

        const int dir = (k.isKeyCode (juce::KeyPress::upKey) || k.isKeyCode (juce::KeyPress::rightKey)) ? 1
                      : (k.isKeyCode (juce::KeyPress::downKey) || k.isKeyCode (juce::KeyPress::leftKey)) ? -1 : 0;

        if (dir == 0)
            return false;

        nudge (dir, mods.isShiftDown());
        return true;
    }

    /** Moves the value one step along the knob's travel, as the arrow keys and screen readers do. */
    void nudge (int direction, bool fine = false)
    {
        const auto before = getValue();
        const auto start = valueToProportionOfLength (before);
        const auto step = fine ? 0.001 : 0.01;

        for (int i = 1; i <= 1000; ++i)
        {
            const auto p = juce::jlimit (0.0, 1.0, start + direction * step * i);
            const auto v = getNormalisableRange().snapToLegalValue (proportionOfLengthToValue (p));

            if (! juce::approximatelyEqual (v, before))
            {
                ScopedDragNotification gesture (*this);
                setValue (v, juce::sendNotificationSync);
                return;
            }

            if (p <= 0.0 || p >= 1.0)
                return;
        }
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return std::make_unique<AccessibilityHandler> (*this);
    }

    void valueChanged() override                          { param->setParameter ((float) getValue(), juce::sendNotification); }
    void startedDragging() override                       { param->parameterChangeGestureBegin(); }
    void stoppedDragging() override                       { param->parameterChangeGestureEnd(); }

private:
    te::AutomatableParameter::Ptr param;

    /** Screen readers see the value along the knob's travel (0 to 1) and hear the parameter's own
        text ("-6.0 dB", "1.2 kHz"). Their increment and decrement step as the arrow keys do. */
    struct AccessibilityHandler : public juce::AccessibilityHandler
    {
        explicit AccessibilityHandler (ParameterSlider& s)
            : juce::AccessibilityHandler (s, juce::AccessibilityRole::slider, {},
                                          Interfaces { std::unique_ptr<juce::AccessibilityValueInterface> (new Value (s)) }),
              slider (s) {}

        juce::String getHelp() const override     { return slider.getTooltip(); }

        struct Value : public juce::AccessibilityValueInterface
        {
            explicit Value (ParameterSlider& s) : slider (s) {}

            bool isReadOnly() const override                { return ! slider.isEnabled(); }
            double getCurrentValue() const override         { return slider.valueToProportionOfLength (slider.getValue()); }
            juce::String getCurrentValueAsString() const override  { return slider.getTextFromValue (slider.getValue()); }
            AccessibleValueRange getRange() const override  { return { { 0.0, 1.0 }, 0.01 }; }

            void setValue (double proportion) override
            {
                const auto delta = proportion - getCurrentValue();

                // An increment or decrement (one step either way): step as the arrow keys do
                if (std::abs (std::abs (delta) - 0.01) < 1.0e-6)
                    return slider.nudge (delta > 0.0 ? 1 : -1);

                ScopedDragNotification gesture (slider);
                slider.setValue (slider.proportionOfLengthToValue (juce::jlimit (0.0, 1.0, proportion)), juce::sendNotificationSync);
            }

            void setValueAsString (const juce::String& text) override
            {
                ScopedDragNotification gesture (slider);
                slider.setValue (slider.getValueFromText (text), juce::sendNotificationSync);
            }

            ParameterSlider& slider;
        };

        ParameterSlider& slider;
    };

    bool lastLearnState = false, lastMapped = false, lastModMapping = false, lastModulated = false;
    float lastShownValue = 0.0f;

    void timerCallback() override
    {
        auto learning = getEdit() != nullptr && getEdit()->engine.getMidiLearnState().isActive();
        auto mapped = isMidiMapped();
        auto modMapping = ModulationMap::isActive();
        auto modulated = param->hasActiveModifierAssignments();

        if (learning != lastLearnState || mapped != lastMapped || modMapping != lastModMapping || modulated != lastModulated)
        {
            lastLearnState = learning;
            lastMapped = mapped;
            lastModMapping = modMapping;
            lastModulated = modulated;
            repaint();
        }
        else if (modulated && std::abs (param->getCurrentValue() - lastShownValue) > 1.0e-6f)
        {
            lastShownValue = param->getCurrentValue();
            repaint();
        }
    }

    void curveHasChanged (te::AutomatableParameter&) override {}
    void currentValueChanged (te::AutomatableParameter&) override
    {
        juce::MessageManager::callAsync ([sp = SafePointer<ParameterSlider> (this)]
        {
            // The knob shows the value you set; modulation is drawn on top of it
            if (sp != nullptr && ! sp->isMouseButtonDown())
                sp->setValue (sp->param->getCurrentBaseValue(), juce::dontSendNotification);
        });
    }
};

//==============================================================================
/** Draggable numeric field, as used for the tempo. */
class DragNumberBox : public juce::Label
{
public:
    DragNumberBox (double minV, double maxV, int decimals) : minValue (minV), maxValue (maxV), numDecimals (decimals)
    {
        setEditable (false, true, false);
        setJustificationType (juce::Justification::centred);
        setColour (juce::Label::backgroundColourId, Colours::panelDark);
        setColour (juce::Label::outlineColourId, Colours::outline);
        setMouseCursor (juce::MouseCursor::UpDownResizeCursor);
        onTextChange = [this] { setValue (getText().getDoubleValue(), true); };
        setMouseClickGrabsKeyboardFocus (false);
    }

    /** What screen readers call it, e.g. "Tempo", and the unit they read after the value ("BPM"). */
    juce::String accessibleName, unit;

    /** Up and Down change it by one (Shift: by a hundredth); Return types a value. */
    bool keyPressed (const juce::KeyPress& k) override
    {
        if (k.getModifiers().isCommandDown() || k.getModifiers().isAltDown())
            return false;

        const double step = k.getModifiers().isShiftDown() ? std::pow (10.0, -numDecimals) : 1.0;

        if (k.isKeyCode (juce::KeyPress::upKey))    { setValue (value + step, true); announceValue(); return true; }
        if (k.isKeyCode (juce::KeyPress::downKey))  { setValue (value - step, true); announceValue(); return true; }
        if (k.isKeyCode (juce::KeyPress::returnKey)) { showEditor(); return true; }

        return juce::Label::keyPressed (k);
    }

    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        struct Handler : public juce::AccessibilityHandler
        {
            explicit Handler (DragNumberBox& b)
                : juce::AccessibilityHandler (b, juce::AccessibilityRole::slider,
                                              juce::AccessibilityActions().addAction (juce::AccessibilityActionType::press, [&b] { b.showEditor(); }),
                                              Interfaces { std::unique_ptr<juce::AccessibilityValueInterface> (new Value (b)) }),
                  box (b) {}

            juce::String getTitle() const override    { return box.accessibleName; }
            juce::String getHelp() const override     { return box.getTooltip(); }

            juce::AccessibleState getCurrentState() const override
            {
                return box.isBeingEdited() ? juce::AccessibleState() : juce::AccessibilityHandler::getCurrentState();
            }

            struct Value : public juce::AccessibilityValueInterface
            {
                explicit Value (DragNumberBox& b) : box (b) {}
                bool isReadOnly() const override                { return false; }
                double getCurrentValue() const override         { return box.value; }
                void setValue (double v) override               { box.setValue (v, true); }
                AccessibleValueRange getRange() const override  { return { { box.minValue, box.maxValue }, 1.0 }; }
                juce::String getCurrentValueAsString() const override   { return (juce::String (box.value, box.numDecimals) + " " + box.unit).trim(); }
                void setValueAsString (const juce::String& t) override  { setValue (t.getDoubleValue()); }
                DragNumberBox& box;
            };

            DragNumberBox& box;
        };

        return std::make_unique<Handler> (*this);
    }

    std::function<void (double)> onValueChange;

    void setValue (double v, bool notify)
    {
        value = juce::jlimit (minValue, maxValue, v);
        setText (juce::String (value, numDecimals), juce::dontSendNotification);

        if (notify && onValueChange)
            onValueChange (value);
    }

    double getValue() const { return value; }

    void mouseDown (const juce::MouseEvent& e) override    { dragStartValue = value; juce::Label::mouseDown (e); }

    void mouseDrag (const juce::MouseEvent& e) override
    {
        auto sensitivity = e.mods.isShiftDown() ? 0.01 : (numDecimals > 0 ? 0.25 : 0.1);
        setValue (std::round ((dragStartValue - e.getDistanceFromDragStartY() * sensitivity) * std::pow (10.0, numDecimals))
                    / std::pow (10.0, numDecimals), true);
    }

private:
    double minValue, maxValue, value = 0.0, dragStartValue = 0.0;
    int numDecimals;

    void announceValue()
    {
        if (auto* h = getAccessibilityHandler())
            h->notifyAccessibilityEvent (juce::AccessibilityEvent::valueChanged);
    }
};

} // namespace elsewhere
