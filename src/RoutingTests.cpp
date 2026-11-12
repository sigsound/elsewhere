// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "RoutingTests.h"
#include "Export.h"
#include "Project.h"
#include "Routing.h"
#include "TrackHeader.h"
#include "devices/CompressorDevice.h"

namespace elsewhere::RoutingTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
    };

    void writeSine (const juce::File& f, double seconds, float freq, float level)
    {
        f.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream().release());
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (out.get(), 48000.0, 1, 24, {}, 0));

        if (w == nullptr)
            return;

        out.release();
        juce::AudioBuffer<float> b (1, (int) (48000.0 * seconds));

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, level * std::sin ((float) i * freq));

        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    juce::AudioBuffer<float> readAll (const juce::File& f)
    {
        juce::AudioFormatManager m;
        m.registerBasicFormats();
        juce::AudioBuffer<float> b;

        if (std::unique_ptr<juce::AudioFormatReader> r (m.createReaderFor (f)); r != nullptr)
        {
            b.setSize ((int) r->numChannels, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
        }

        return b;
    }

    /** Level in dB of the left channel between two times. */
    float levelDb (const juce::AudioBuffer<float>& b, double from, double to)
    {
        auto s = juce::jlimit (0, b.getNumSamples(), (int) (from * 48000.0));
        auto e = juce::jlimit (0, b.getNumSamples(), (int) (to * 48000.0));
        return e > s ? juce::Decibels::gainToDecibels (b.getRMSLevel (0, s, e - s), -150.0f) : -150.0f;
    }

    te::ClipPosition secs (double a, double b)  { return { { te::TimePosition::fromSeconds (a), te::TimePosition::fromSeconds (b) }, {} }; }

    /** Renders what an export plan would write, waiting for it. */
    void render (te::Edit& edit, Export::Plan& plan)
    {
        plan.folder.createDirectory();

        for (auto& job : plan.jobs)
        {
            te::ScopedTrackMuter muter (edit, job.tracksToMute);
            std::atomic<bool> done { false };
            auto handle = te::EditRenderer::render (job.params, [&done] (auto) { done = true; }, nullptr);
            auto deadline = juce::Time::getMillisecondCounter() + 60000;

            while (! done && juce::Time::getMillisecondCounter() < deadline)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

            handle.reset();
        }
    }

    juce::AudioBuffer<float> renderMix (te::Edit& edit, const juce::File& folder, const juce::String& name)
    {
        Export::Settings s;
        s.tails = false;
        s.format = te::RenderFormat::wav;
        s.bitDepth = 32;
        s.sampleRate = 48000.0;
        auto plan = Export::plan (edit, s, folder, name);
        render (edit, plan);
        return readAll (plan.mixFile);
    }

    void set (ElsewhereDevice::Param& p, float v)     { p.param().setParameter (v, juce::sendNotificationSync); }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    Log log;
    outDir.deleteRecursively();
    outDir.createDirectory();

    auto editFile = Project::createProjectFolder (outDir, "Routing");
    auto samples = editFile.getParentDirectory().getChildFile ("Samples/Imported");
    writeSine (samples.getChildFile ("kick.wav"), 1.0, 0.008f, 0.8f);
    writeSine (samples.getChildFile ("bass.wav"), 2.0, 0.02f, 0.25f);
    writeSine (samples.getChildFile ("pad.wav"), 2.0, 0.05f, 0.25f);

    Project p (engine, editFile);
    auto& edit = p.getEdit();
    auto& um = edit.getUndoManager();
    auto renders = p.getFolder().getChildFile ("Renders");

    // Kick (the first second only), Bass and Pad (two seconds each)
    edit.ensureNumberOfAudioTracks (3);
    auto tracks = te::getAudioTracks (edit);
    auto& kick = *tracks[0];
    auto& bass = *tracks[1];
    auto& pad = *tracks[2];
    kick.setName ("Kick");
    bass.setName ("Bass");
    pad.setName ("Pad");
    kick.insertWaveClip ("kick", samples.getChildFile ("kick.wav"), secs (0, 1), false);
    bass.insertWaveClip ("bass", samples.getChildFile ("bass.wav"), secs (0, 2), false);
    pad.insertWaveClip ("pad", samples.getChildFile ("pad.wav"), secs (0, 2), false);
    auto ret = Mixing::createReturnTrack (edit);

    for (auto plugin : edit.getMasterPluginList().getPlugins())
        if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) == nullptr && dynamic_cast<te::LevelMeterPlugin*> (plugin) == nullptr)
            plugin->deleteFromParent();

    // A compressor on the bass, set to clamp down hard on anything it hears
    auto plugin = DeviceRegistry::create (edit, CompressorDevice::xmlTypeName);
    auto comp = dynamic_cast<CompressorDevice*> (plugin.get());
    log.check (comp != nullptr, "a compressor to key");

    if (comp == nullptr)
        return 1;

    bass.pluginList.insertPlugin (plugin, 0, nullptr);
    set (comp->threshold, -40.0f);
    set (comp->ratio, 30.0f);
    set (comp->attack, 1.0f);
    set (comp->release, 20.0f);
    set (comp->autoGain, 0.0f);
    set (comp->model, (float) CompressorDevice::clean);

    // Which routes are offered
    log.check (Routing::canSidechain (*comp), "the compressor takes a key");
    log.check (Routing::canSidechainFrom (*comp, kick), "the bass can hear the kick");
    log.check (! Routing::canSidechainFrom (*comp, bass), "but not itself");
    log.check (ret != nullptr && ! Routing::canSidechainFrom (*comp, *ret), "nor a return, which the bass sends into");

    um.beginNewTransaction();
    Routing::setSidechain (*comp, &kick);
    log.check (Routing::getSidechainSource (*comp) == &kick && comp->getNumWires() == 4, "keyed from the kick, its channels wired in (" + juce::String (comp->getNumWires()) + " wires)");
    log.check (Routing::getSidechainTargets (kick).contains (comp), "and the kick knows what it keys");

    auto kickComp = DeviceRegistry::create (edit, CompressorDevice::xmlTypeName);
    kick.pluginList.insertPlugin (kickComp, 0, nullptr);
    log.check (! Routing::canSidechainFrom (*kickComp, bass), "a compressor on the kick can't hear the bass now: that would loop");
    kickComp->deleteFromParent();

    // Audio: the kick keys the bass but isn't heard itself (the pad waits for the bus, below)
    pad.setMute (true);
    Routing::setNoOutput (kick);
    log.check (getOutputName (kick) == "No Output", "a track can have no output (" + getOutputName (kick) + ")");

    {
        auto mix = renderMix (edit, renders, "keyed");
        auto under = levelDb (mix, 0.2, 0.9), free = levelDb (mix, 1.3, 1.9);
        log.check (free > -20.0f && under < free - 12.0f, "the bass ducks while the kick plays (" + juce::String (under, 1) + " dB, then " + juce::String (free, 1) + " dB)");
    }

    set (comp->listen, 1.0f);
    {
        auto mix = renderMix (edit, renders, "listen");
        auto keyLevel = levelDb (mix, 0.2, 0.9), after = levelDb (mix, 1.3, 1.9);
        log.check (keyLevel > -10.0f && after < -60.0f, "Listen plays the key in place of the track (" + juce::String (keyLevel, 1) + " dB, then " + juce::String (after, 1) + " dB)");
    }
    set (comp->listen, 0.0f);

    um.beginNewTransaction();
    Routing::setSidechain (*comp, nullptr);
    log.check (Routing::getSidechainSource (*comp) == nullptr && comp->getNumWires() == 0, "the key comes off");
    {
        auto mix = renderMix (edit, renders, "unkeyed");
        auto first = levelDb (mix, 0.2, 0.9), second = levelDb (mix, 1.3, 1.9);
        log.check (std::abs (first - second) < 1.5f, "without it the bass compresses evenly, and the silent kick isn't heard (" + juce::String (first, 1) + ", " + juce::String (second, 1) + " dB)");
    }

    um.undo();
    log.check (Routing::getSidechainSource (*comp) == &kick, "undo puts the key back");
    comp->setEnabled (false);

    // Buses
    pad.setMute (false);
    auto bus = Routing::createBus (edit);
    log.check (bus != nullptr && bus->getName() == "Bus" && Routing::isBus (*bus), "a new bus");

    if (bus == nullptr)
        return log.failures;

    log.check (te::getAllTracks (edit).indexOf (bus) < te::getAllTracks (edit).indexOf (ret), "placed before the returns");
    Routing::setOutput (pad, bus);
    log.check (pad.getOutput().outputsToDestTrack (*bus) && getOutputName (pad) == juce::String::fromUTF8 ("\xe2\x86\x92 Bus"), "the pad plays into it (" + getOutputName (pad) + ")");
    log.check (! Routing::canOutputTo (*bus, pad), "the bus can't play back into the pad");

    auto padComp = DeviceRegistry::create (edit, CompressorDevice::xmlTypeName);
    pad.pluginList.insertPlugin (padComp, 0, nullptr);
    log.check (! Routing::canSidechainFrom (*padComp, *bus) && Routing::canSidechainFrom (*padComp, kick), "the pad's devices can't hear the bus (the pad is in it), but can hear the kick");
    padComp->deleteFromParent();

    {
        auto before = levelDb (renderMix (edit, renders, "bus"), 0.2, 1.9);
        bus->setMute (true);
        auto muted = levelDb (renderMix (edit, renders, "busmuted"), 0.2, 1.9);
        bus->setMute (false);
        bass.setMute (true);
        auto padOnly = levelDb (renderMix (edit, renders, "padonly"), 0.2, 1.9);
        bass.setMute (false);
        log.check (padOnly > -30.0f && muted < before - 1.0f, "the pad is heard through the bus: muting the bus takes it out ("
                     + juce::String (before, 1) + " dB, " + juce::String (muted, 1) + " muted, pad alone " + juce::String (padOnly, 1) + ")");
    }

    // Stems: what reaches Main. The pad is in the bus's; the kick, with no output, isn't one
    Export::Settings s;
    s.what = Export::What::stems;
    s.tails = false;
    s.format = te::RenderFormat::wav;
    s.bitDepth = 32;
    auto plan = Export::plan (edit, s, renders, "Song");
    juce::StringArray names;
    for (auto& job : plan.jobs)
        names.add (job.params.destFile.getFileNameWithoutExtension());
    log.check (names.joinIntoString (", ") == "01 Bass, 02 Bus, 03 A Reverb", "stems: " + names.joinIntoString (", "));

    render (edit, plan);
    auto busStem = readAll (plan.folder.getChildFile ("02 Bus.wav"));
    log.check (levelDb (busStem, 0.2, 1.9) > -30.0f, "the bus stem has the pad in it (" + juce::String (levelDb (busStem, 0.2, 1.9), 1) + " dB)");

    um.beginNewTransaction();
    Routing::setOutput (pad, nullptr);
    log.check (getOutputName (pad) == "Main" && ! Routing::getFeeders (*bus).contains (&pad), "and back to Main");

    std::cout << "Routing tests\n" << log.text << (log.failures == 0 ? "All passed\n" : juce::String (log.failures) + " failed\n");
    return log.failures;
}
}
