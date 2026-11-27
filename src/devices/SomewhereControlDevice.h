// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "DeviceBase.h"
#include "PerformanceFx.h"

namespace elsewhere
{
//==============================================================================
/**
    Elsewhere Somewhere Control: a track's sound played through performance effects from an XY
    pad, on the phone or here with the mouse.

    Pick the track on Somewhere's FX screen and the phone's pad plays this device: the same
    seven effects, with X and Y meaning the same things, run here on the track's own sound, so
    nothing but the moves travels over the link. Picking a track that doesn't have one adds it
    at the end of the track's devices.

    Effect, X, Y and On are ordinary parameters, so moves can be undone, sync to collaborators,
    are written as automation when AUTO is on, and play back in exports.
*/
class SomewhereControlDevice : public ElsewhereDevice
{
public:
    static constexpr const char* xmlTypeName = "elsewhereSomewhereControl";

    static const juce::StringArray& effectNames()
    {
        static const juce::StringArray names { "Filter", "Delay", "Stutter", "Tape Stop", "Bitcrush", "Wash", "Lo-Fi" };
        return names;
    }

    /** What X and Y do for each effect, as the pad labels them. */
    static std::pair<juce::String, juce::String> axisNames (int effect)
    {
        switch (effect)
        {
            case PerformanceFx::filter:     return { "Low-pass / High-pass", "Resonance" };
            case PerformanceFx::delay:      return { "Time", "Feedback" };
            case PerformanceFx::stutter:    return { "Slice", "Fade" };
            case PerformanceFx::tapeStop:   return { "Stop time", {} };
            case PerformanceFx::bitcrush:   return { "Rate crush", "Bit crush" };
            case PerformanceFx::wash:       return { "Size", "Level" };
            case PerformanceFx::lofi:       return { "Rate", "Vibrato" };
            default:                        return {};
        }
    }

    /** What X means right now, for the display (the same readout as the phone's). */
    static juce::String describeX (int effect, float x)
    {
        auto hz = [] (double f) { return f >= 1000.0 ? juce::String (f / 1000.0, 1) + "k" : juce::String (juce::roundToInt (f)); };

        switch (effect)
        {
            case PerformanceFx::filter:
                if (std::abs (x - 0.5f) < 0.02f) return "Off";
                if (x < 0.5f) return "LP " + hz (20000.0 * std::pow (80.0 / 20000.0, (0.5 - x) * 2.0));
                return "HP " + hz (20.0 * std::pow (8000.0 / 20.0, (x - 0.5) * 2.0));
            case PerformanceFx::delay:      return juce::StringArray { "1/32", "1/16", "1/8", "3/16", "1/4" }[juce::jlimit (0, 4, (int) (x * 5.0f))];
            case PerformanceFx::stutter:    return juce::StringArray { "1/4", "1/8", "1/16", "1/32" }[juce::jlimit (0, 3, (int) (x * 4.0f))];
            case PerformanceFx::tapeStop:   return juce::String (0.15 + 1.85 * x, 2) + " s";
            case PerformanceFx::bitcrush:   return juce::String (juce::CharPointer_UTF8 ("\xc3\xb7")) + juce::String (juce::roundToInt (1.0f + 39.0f * x));
            case PerformanceFx::lofi:       return juce::String (0.35 * std::pow (24.0, (double) x), 1) + " Hz";
            case PerformanceFx::wash:       return juce::String (juce::roundToInt (x * 100.0f)) + " %";
            default:                        return {};
        }
    }

    SomewhereControlDevice (te::PluginCreationInfo info) : ElsewhereDevice (info)
    {
        effect = addParameter ("effect", "Effect", 0.0f, stepped (0.0f, (float) PerformanceFx::numEffects - 1.0f),
                               [] (float v) { return effectNames()[juce::jlimit (0, effectNames().size() - 1, juce::roundToInt (v))]; });
        x = addParameter ("x", "X", 0.5f, { 0.0f, 1.0f },
                          [this] (float v) { return describeX (getEffect(), v); });
        y = addParameter ("y", "Y", 0.5f, { 0.0f, 1.0f },
                          [] (float v) { return juce::String (juce::roundToInt (v * 100.0f)) + " %"; });
        on = addParameter ("on", "On", 0.0f, stepped (0.0f, 1.0f), onOffText);
        latch = addParameter ("latch", "Latch", 0.0f, stepped (0.0f, 1.0f), onOffText);
    }

    /** A track's Somewhere Control, or null. */
    static SomewhereControlDevice* findOn (te::Track& t)
    {
        for (auto* p : t.pluginList)
            if (auto* d = dynamic_cast<SomewhereControlDevice*> (p))
                return d;
        return nullptr;
    }

    /** A track's Somewhere Control, added at the end of its devices (before the fader and meter) if it has none. */
    static SomewhereControlDevice* findOrAddTo (te::AudioTrack& t)
    {
        if (auto* d = findOn (t))
            return d;

        auto& list = t.pluginList;
        int index = list.size();

        for (int i = 0; i < list.size(); ++i)
            if (EngineHelpers::isMixerPlugin (*list[i]))
            {
                index = i;
                break;
            }

        auto p = DeviceRegistry::create (t.edit, xmlTypeName);
        if (p == nullptr)
            return nullptr;

        list.insertPlugin (p, index, nullptr);
        return dynamic_cast<SomewhereControlDevice*> (p.get());
    }

    juce::String getName() const override              { return "Somewhere Control"; }
    juce::String getPluginType() override              { return xmlTypeName; }
    juce::String getSelectableDescription() override   { return "Elsewhere Somewhere Control"; }
    std::unique_ptr<juce::Component> createDeviceView() override;
    int getDeviceViewWidth() const override             { return 320; }

    Param effect, x, y, on, latch;

    int getEffect() const       { return juce::jlimit (0, (int) PerformanceFx::numEffects - 1, juce::roundToInt (effect.get())); }
    bool isPlaying() const      { return on.get() > 0.5f || latch.get() > 0.5f; }

    /** The loudest input since the last call, for the pad's picture. */
    float takePeak()            { return fx.peak.exchange (0.0f, std::memory_order_relaxed); }
    float getTapeSpeed() const  { return fx.tapeSpeedOut.load (std::memory_order_relaxed); }

    /** The phone playing this device now, if one is (message thread). */
    juce::String remoteName;
    juce::Time lastRemoteMove;

    //==============================================================================
    void initialise (const te::PluginInitialisationInfo& info) override
    {
        const juce::SpinLock::ScopedLockType dspGuard (dspLock);
        fx.prepare (info.sampleRate);
        scratch.setSize (1, juce::jmax (info.blockSizeSamples, 512) * 2);
        tempoPosition = std::make_unique<te::tempo::Sequence::Position> (te::createPosition (edit.tempoSequence));
    }

    void deinitialise() override
    {
        tempoPosition.reset();
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        const juce::SpinLock::ScopedTryLockType dspGuard (dspLock);

        if (! dspGuard.isLocked() || fc.destBuffer == nullptr || fc.bufferNumSamples == 0)
            return;

        auto& buf = *fc.destBuffer;
        const int start = fc.bufferStartSample, n = fc.bufferNumSamples;
        const int numCh = buf.getNumChannels();

        if (numCh == 0)
            return;

        double bpm = 120.0;

        if (tempoPosition != nullptr)
        {
            tempoPosition->set (fc.editTime.getStart());
            bpm = tempoPosition->getTempo();
        }

        fx.type.store (getEffect(), std::memory_order_relaxed);
        fx.x.store (x.get(), std::memory_order_relaxed);
        fx.y.store (y.get(), std::memory_order_relaxed);
        fx.active.store (isPlaying(), std::memory_order_relaxed);

        auto* l = buf.getWritePointer (0, start);

        if (numCh > 1)
            return fx.process (l, buf.getWritePointer (1, start), n, bpm);

        // A mono track: run the right side on a copy and keep the left
        if (scratch.getNumSamples() < n)
            return;

        auto* r = scratch.getWritePointer (0);
        std::copy (l, l + n, r);
        fx.process (l, r, n, bpm);
    }

private:
    PerformanceFx fx;
    juce::AudioBuffer<float> scratch;
    std::unique_ptr<te::tempo::Sequence::Position> tempoPosition;
};

//==============================================================================
/** The pad: press to play the effect where you press, drag to move it, let go to stop (unless
    it's latched). */
class SomewhereControlPad : public juce::Component, private juce::Timer
{
public:
    SomewhereControlPad (SomewhereControlDevice& d) : device (d)
    {
        setTitle ("Effect pad");
        setDescription ("Press and drag to play the effect");
        setRepaintsOnMouseActivity (false);
        startTimerHz (30);
    }

    ~SomewhereControlPad() override
    {
        if (dragging)
            finish();
    }

    void paint (juce::Graphics& g) override
    {
        auto r = getLocalBounds().toFloat();
        g.setColour (Colours::display);
        g.fillRoundedRectangle (r, 6.0f);

        const auto playing = device.isPlaying();
        const auto px = r.getX() + r.getWidth() * device.x.get();
        const auto py = r.getBottom() - r.getHeight() * device.y.get();

        // Crosshair through the point, and a ring that swells with the input
        g.setColour (Colours::gridLine);
        g.drawLine (r.getX(), py, r.getRight(), py, 1.0f);
        g.drawLine (px, r.getY(), px, r.getBottom(), 1.0f);

        if (device.getEffect() == PerformanceFx::filter)
        {
            g.setColour (Colours::gridBar);
            g.drawLine (r.getCentreX(), r.getY(), r.getCentreX(), r.getBottom(), 1.0f);
        }

        const auto ring = 7.0f + 14.0f * level;
        g.setColour (Colours::accent.withAlpha (playing ? 0.35f : 0.12f));
        g.fillEllipse (px - ring, py - ring, ring * 2.0f, ring * 2.0f);
        g.setColour (playing ? Colours::accent : Colours::textDim);
        g.fillEllipse (px - 5.0f, py - 5.0f, 10.0f, 10.0f);

        // What X and Y do, and where X is now
        auto [xName, yName] = SomewhereControlDevice::axisNames (device.getEffect());
        auto inner = r.reduced (6.0f, 4.0f);
        g.setFont (juce::FontOptions (10.0f));
        g.setColour (Colours::textDim);
        g.drawText (xName, inner.removeFromBottom (14.0f), juce::Justification::centredLeft);

        if (yName.isNotEmpty())
        {
            juce::Graphics::ScopedSaveState s (g);
            g.addTransform (juce::AffineTransform::rotation (-juce::MathConstants<float>::halfPi, r.getX() + 12.0f, r.getCentreY()));
            g.drawText (yName, juce::Rectangle<float> (r.getX() + 12.0f - 60.0f, r.getCentreY() - 7.0f, 120.0f, 14.0f), juce::Justification::centred);
        }

        g.setColour (Colours::text);
        g.setFont (juce::FontOptions (11.0f, juce::Font::bold));
        g.drawText (SomewhereControlDevice::describeX (device.getEffect(), device.x.get()),
                    r.reduced (8.0f, 5.0f).removeFromTop (14.0f), juce::Justification::centredRight);
    }

    void mouseDown (const juce::MouseEvent& e) override
    {
        device.edit.getUndoManager().beginNewTransaction ("Play Somewhere Control");

        for (auto* p : { &device.x.param(), &device.y.param(), &device.on.param() })
            p->parameterChangeGestureBegin();

        dragging = true;
        device.on.param().setParameter (1.0f, juce::sendNotification);
        moveTo (e.position);
    }

    void mouseDrag (const juce::MouseEvent& e) override     { moveTo (e.position); }
    void mouseUp (const juce::MouseEvent&) override         { finish(); }

private:
    SomewhereControlDevice& device;
    bool dragging = false;
    float level = 0.0f;

    void moveTo (juce::Point<float> p)
    {
        auto r = getLocalBounds().toFloat();
        device.x.param().setParameter (juce::jlimit (0.0f, 1.0f, (p.x - r.getX()) / r.getWidth()), juce::sendNotification);
        device.y.param().setParameter (juce::jlimit (0.0f, 1.0f, (r.getBottom() - p.y) / r.getHeight()), juce::sendNotification);
        repaint();
    }

    void finish()
    {
        dragging = false;
        device.on.param().setParameter (0.0f, juce::sendNotification);

        for (auto* p : { &device.x.param(), &device.y.param(), &device.on.param() })
            p->parameterChangeGestureEnd();

        repaint();
    }

    void timerCallback() override
    {
        const auto peak = juce::jmin (1.0f, device.takePeak());
        level = juce::jmax (peak, level * 0.85f);
        repaint();
    }
};

//==============================================================================
class SomewhereControlView : public juce::Component, private juce::Timer
{
public:
    SomewhereControlView (SomewhereControlDevice& d)
        : device (d),
          effects (d.effect.param(), SomewhereControlDevice::effectNames()),
          latchToggle (d.latch.param(), "Latch"),
          pad (d)
    {
        addAndMakeVisible (effects);
        addAndMakeVisible (latchToggle);
        addAndMakeVisible (pad);
        startTimerHz (4);
    }

    void paint (juce::Graphics& g) override
    {
        g.setFont (juce::FontOptions (11.0f));
        g.setColour (Colours::textDim);
        g.drawText (status(), statusArea, juce::Justification::centredLeft, true);
    }

    void resized() override
    {
        auto r = getLocalBounds();
        effects.setBounds (r.removeFromTop (24));
        r.removeFromTop (6);
        auto bottom = r.removeFromBottom (22);
        latchToggle.setBounds (bottom.removeFromRight (80));
        statusArea = bottom;
        r.removeFromBottom (6);
        pad.setBounds (r);
    }

    juce::String status() const
    {
        if (device.remoteName.isNotEmpty() && juce::Time::getCurrentTime() - device.lastRemoteMove < juce::RelativeTime::seconds (10))
            return "Played from " + device.remoteName;

        return "Pick this track on Somewhere's FX screen";
    }

private:
    SomewhereControlDevice& device;
    DeviceUI::Choice effects;
    DeviceUI::Toggle latchToggle;
    SomewhereControlPad pad;
    juce::Rectangle<int> statusArea;
    juce::String shown;

    void timerCallback() override
    {
        if (auto s = status(); s != shown)
        {
            shown = s;
            repaint (statusArea);
        }
    }
};

inline std::unique_ptr<juce::Component> SomewhereControlDevice::createDeviceView()   { return std::make_unique<SomewhereControlView> (*this); }

} // namespace elsewhere
