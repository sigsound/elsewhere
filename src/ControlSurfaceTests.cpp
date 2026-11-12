// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ControlSurfaceTests.h"
#include "Project.h"
#include "ControlSurfaces.h"
#include "PluginWindow.h"

namespace elsewhere::ControlSurfaceTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
    };

    /** Turns the message loop until a condition holds. */
    bool waitFor (std::function<bool()> condition, int ms)
    {
        auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) ms;

        while (! condition())
        {
            if (juce::Time::getMillisecondCounter() > deadline)
                return false;

            juce::MessageManager::getInstance()->runDispatchLoopUntil (20);
        }

        return true;
    }

    void sendToSurface (te::ExternalController& c, std::initializer_list<int> bytes)
    {
        std::vector<juce::uint8> b;
        for (auto x : bytes) b.push_back ((juce::uint8) x);
        c.getControlSurface().acceptMidiMessage (0, juce::MidiMessage (b.data(), (int) b.size()));
    }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    Log log;
    outDir.deleteRecursively();
    outDir.createDirectory();

    Project a (engine, Project::createProjectFolder (outDir, "A"));
    auto& editA = a.getEdit();

    //==============================================================================
    // Mackie Control
    auto mcu = ControlSurfaces::findMackie (engine);
    log.check (mcu != nullptr, "a Mackie Control surface to set up");

    if (mcu != nullptr)
    {
        auto wasIn = mcu->getMidiInputDevice (0), wasOut = mcu->getBackChannelDevice (0);
        mcu->setMidiInputDevice (0, {});
        mcu->setBackChannelDevice (0, {});
        log.check (! ControlSurfaces::isOn (*mcu) && ControlSurfaces::describe (*mcu).startsWith ("Choose the surface's MIDI ports"),
                   "off until it has ports, and says so (" + ControlSurfaces::describe (*mcu) + ")");

        editA.ensureNumberOfAudioTracks (3);
        auto tracks = te::getAudioTracks (editA);
        tracks[0]->setName ("Drums");
        tracks[1]->setName ("Hidden");
        tracks[2]->setName ("Bass");
        EditViewState::setTrackHidden (*tracks[1], true);

        te::SelectionManager sm (engine);
        EditViewState vs (editA, sm);
        ControlSurfaces::attach (vs);
        ElsewhereUIBehaviour::currentEdit() = &editA;    // what the surface's transport keys act on
        auto& ecm = engine.getExternalControllerManager();
        log.check (ecm.isAttachedToEdit (editA), "it follows the open project");
        log.check (ecm.getChannelTrack (0) == tracks[0] && ecm.getChannelTrack (1) == tracks[2], "hidden tracks are skipped on the faders");

        // Fader 2 to the top: that's the bass
        auto before = tracks[2]->getVolumePlugin()->getSliderPos();
        sendToSurface (*mcu, { 0xe1, 0x70, 0x7e });
        auto after = tracks[2]->getVolumePlugin()->getSliderPos();
        log.check (after > 0.95f && before < 0.95f, "moving a fader moves the track's (" + juce::String (before, 2) + " to " + juce::String (after, 2) + ")");

        sendToSurface (*mcu, { 0x90, 0x10, 0x7f });
        sendToSurface (*mcu, { 0x90, 0x10, 0x00 });
        log.check (tracks[0]->isMuted (false), "its mute button mutes Drums");

        sendToSurface (*mcu, { 0x90, 0x5e, 0x7f });
        sendToSurface (*mcu, { 0x90, 0x5e, 0x00 });
        auto playing = waitFor ([&] { return editA.getTransport().isPlaying(); }, 2000);
        log.check (playing, "Play plays");
        // Stop goes to the surface's own project, which it only has once it's on: borrow a real
        // MIDI input for that (the engine won't take a virtual one), if this machine has one
        if (auto ports = mcu->getMidiInputPorts(); ports.size() > 1)
        {
            mcu->setMidiInputDevice (0, ports[1]);
            ecm.setCurrentEdit (&editA, &sm);
            log.check (ControlSurfaces::isOn (*mcu), "with a port it's on (" + ports[1] + ")");

            sendToSurface (*mcu, { 0x90, 0x5d, 0x7f });
            sendToSurface (*mcu, { 0x90, 0x5d, 0x00 });
            auto stopped = waitFor ([&] { return ! editA.getTransport().isPlaying(); }, 2000);
            log.check (stopped, "and Stop stops");
        }
        else
        {
            editA.getTransport().stop (false, false);
            log.text << "  SKIP  Stop (needs a MIDI input on this machine to switch the surface on)\n";
        }

        ControlSurfaces::detach (vs);
        log.check (! ecm.isAttachedToEdit (editA), "and lets go when the project closes");
        ElsewhereUIBehaviour::currentEdit() = nullptr;
        mcu->setMidiInputDevice (0, ControlSurfaces::portOrNone (wasIn));
        mcu->setBackChannelDevice (0, ControlSurfaces::portOrNone (wasOut));
    }

    std::cout << "Control surface tests\n" << log.text << (log.failures == 0 ? "All passed\n" : juce::String (log.failures) + " failed\n");
    return log.failures;
}
}
