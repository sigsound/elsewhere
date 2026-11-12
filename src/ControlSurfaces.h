// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "EditViewState.h"
#include "Widgets.h"

namespace elsewhere
{
//==============================================================================
/**
    Hardware control surfaces. Elsewhere speaks Mackie Control (MCU), which most fader
    surfaces understand: Mackie's own, Behringer X-Touch, iCON, PreSonus FaderPort and
    Softube Console 1 in MCU mode, and Logic and Pro Tools users' existing desks.

    The engine does the protocol: eight faders following the tracks shown (hidden tracks
    skipped), banking, pans, mute/solo/arm, transport keys, the jog wheel, and track names
    and the timecode on the surface's displays. This picks the ports and keeps it pointed at
    the open project.
*/
namespace ControlSurfaces
{
    inline te::ExternalController* findMackie (te::Engine& engine)
    {
        for (auto c : engine.getExternalControllerManager().getControllers())
            if (c->getName() == "Mackie Control Universal")
                return c;

        return nullptr;
    }

    /** Points the surfaces at a project, and at what's visible in it. */
    inline void attach (EditViewState& vs)
    {
        auto& ecm = vs.edit.engine.getExternalControllerManager();
        // The mixer's channels: audio tracks and groups, not the master (it has its own fader),
        // the engine's tempo and marker tracks or anything hidden
        ecm.isVisibleOnControlSurface = [] (const te::Track& t)
        {
            return (t.isAudioTrack() || t.isFolderTrack()) && ! t.isMasterTrack() && ! EditViewState::isTrackHidden (t);
        };
        ecm.setCurrentEdit (&vs.edit, &vs.selectionManager);
    }

    inline void detach (EditViewState& vs)
    {
        auto& ecm = vs.edit.engine.getExternalControllerManager();
        ecm.detachFromEdit (&vs.edit);
        ecm.detachFromSelectionManager (&vs.selectionManager);
    }

    /** A port name as the engine stores it, or empty for none. */
    inline juce::String portOrNone (const juce::String& s)
    {
        return s == te::ExternalController::getNoDeviceSelectedMessage() ? juce::String() : s;
    }

    /** On once it has a port to listen to: the engine's rule for MIDI surfaces. */
    inline bool isOn (te::ExternalController& c)     { return c.isEnabled(); }

    /** What's set up, or what's missing, for the settings window. */
    inline juce::String describe (te::ExternalController& c)
    {
        auto in = portOrNone (c.getMidiInputDevice (0)), out = portOrNone (c.getBackChannelDevice (0));

        if (in.isEmpty() && out.isEmpty())  return "Choose the surface's MIDI ports to use it";
        if (in.isEmpty())                   return "Choose the port the surface sends on";
        if (out.isEmpty())                  return "Choose the port back to the surface, for its motors, lights and displays";
        return "Faders, transport and track names on " + in;
    }
}

//==============================================================================
/** Options > Control Surfaces: the ports a Mackie Control surface is on. */
class ControlSurfaceSettings : public juce::Component, private juce::Timer
{
public:
    explicit ControlSurfaceSettings (te::Engine& e) : engine (e)
    {
        for (auto* lab : { &title, &state, &inLabel, &outLabel, &status })
        {
            lab->setColour (juce::Label::textColourId, Colours::text);
            addAndMakeVisible (lab);
        }

        title.setFont (juce::FontOptions (13.0f, juce::Font::bold));
        title.setText ("Mackie Control", juce::dontSendNotification);
        state.setColour (juce::Label::textColourId, Colours::textDim);
        state.setFont (juce::FontOptions (11.0f));
        status.setJustificationType (juce::Justification::centredRight);
        status.setColour (juce::Label::textColourId, Colours::textDim);

        inLabel.setText ("From surface", juce::dontSendNotification);
        outLabel.setText ("To surface", juce::dontSendNotification);

        inBox.onChange = [this]
        {
            if (auto mcu = ControlSurfaces::findMackie (engine))
                mcu->setMidiInputDevice (0, ControlSurfaces::portOrNone (inBox.getText()));
            refresh();
        };
        outBox.onChange = [this]
        {
            if (auto mcu = ControlSurfaces::findMackie (engine))
                mcu->setBackChannelDevice (0, ControlSurfaces::portOrNone (outBox.getText()));
            refresh();
        };
        addAndMakeVisible (inBox);
        addAndMakeVisible (outBox);

        setSize (440, 128);
        refresh();
        startTimerHz (4);
    }

    void paint (juce::Graphics& g) override     { g.fillAll (Colours::panel); }

    void resized() override
    {
        auto r = getLocalBounds().reduced (16, 12);
        auto row = [&] (int h) { return r.removeFromTop (h); };
        auto control = [] (juce::Rectangle<int> line, int w) { return line.removeFromRight (w).withSizeKeepingCentre (w, 24); };

        auto line = row (28);
        status.setBounds (line.removeFromRight (100));
        title.setBounds (line);
        state.setBounds (row (16).withTrimmedLeft (4));
        line = row (30);
        inBox.setBounds (control (line, 260));
        inLabel.setBounds (line);
        line = row (30);
        outBox.setBounds (control (line, 260));
        outLabel.setBounds (line);
    }

private:
    te::Engine& engine;
    juce::Label title, state, inLabel, outLabel, status;
    juce::ComboBox inBox, outBox;

    static void fill (juce::ComboBox& box, const juce::StringArray& items, const juce::String& current)
    {
        box.clear (juce::dontSendNotification);

        for (int i = 0; i < items.size(); ++i)
            box.addItem (i == 0 ? juce::String ("None") : items[i], i + 1);

        box.setSelectedId (juce::jmax (0, items.indexOf (current.isEmpty() ? items[0] : current)) + 1, juce::dontSendNotification);
    }

    void refresh()
    {
        auto mcu = ControlSurfaces::findMackie (engine);
        status.setText (mcu != nullptr && ControlSurfaces::isOn (*mcu) ? "Enabled" : "Disabled", juce::dontSendNotification);
        state.setText (mcu == nullptr ? juce::String ("Control surfaces aren't part of this build")
                                      : ControlSurfaces::describe (*mcu), juce::dontSendNotification);

        if (mcu != nullptr && ! inBox.isPopupActive() && ! outBox.isPopupActive())
        {
            fill (inBox, mcu->getMidiInputPorts(), mcu->getMidiInputDevice (0));
            fill (outBox, mcu->getMidiOutputPorts(), mcu->getBackChannelDevice (0));
        }

        for (auto* c : std::initializer_list<juce::Component*> { &inBox, &outBox, &inLabel, &outLabel })
            c->setEnabled (mcu != nullptr);
    }

    void timerCallback() override   { refresh(); }   // surfaces get plugged in and out
};

} // namespace elsewhere
