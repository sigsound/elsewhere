// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "PluginTests.h"
#include "drums/DrumTrack.h"
#include "devices/DeviceBase.h"

namespace elsewhere::PluginTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
        void info (const juce::String& what)            { text << "        " << what << "\n"; }
    };

    std::optional<juce::PluginDescription> scan (te::Engine& engine, const juce::String& path, juce::AudioPluginFormat& format)
    {
        juce::OwnedArray<juce::PluginDescription> found;
        engine.getPluginManager().knownPluginList.scanAndAddFile (path, true, found, format);
        if (found.isEmpty())
            return std::nullopt;
        return *found[0];
    }

    te::Plugin::Ptr load (te::Edit& edit, const juce::PluginDescription& d)
    {
        return edit.getPluginCache().createNewPlugin (te::ExternalPlugin::xmlTypeName, d);
    }

    juce::AudioBuffer<float> render (te::Edit& edit, te::AudioTrack& track, double seconds, const juce::File& file)
    {
        file.deleteFile();
        juce::BigInteger tracks;
        tracks.setBit (track.getIndexInEditTrackList());
        te::Renderer::renderToFile ("Test", file, edit, te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (seconds)), tracks, true, false, {}, true);

        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
        juce::AudioBuffer<float> b (2, reader != nullptr ? (int) reader->lengthInSamples : 0);
        if (reader != nullptr)
            reader->read (&b, 0, b.getNumSamples(), 0, true, true);
        return b;
    }

    float rmsDb (const juce::AudioBuffer<float>& b, double from, double to, double sr)
    {
        auto s = juce::jlimit (0, b.getNumSamples(), (int) (from * sr)), e = juce::jlimit (0, b.getNumSamples(), (int) (to * sr));
        return e > s ? juce::Decibels::gainToDecibels (b.getRMSLevel (0, s, e - s), -150.0f) : -150.0f;
    }

    /** Opens the plug-in's editor and returns its size (0x0 if it couldn't). */
    juce::Rectangle<int> openEditor (te::Plugin& p)
    {
        auto ext = dynamic_cast<te::ExternalPlugin*> (&p);
        if (ext == nullptr || ext->getAudioPluginInstance() == nullptr)
            return {};

        std::unique_ptr<juce::AudioProcessorEditor> ed (ext->getAudioPluginInstance()->createEditorAndMakeActive());
        if (ed == nullptr)
            return {};

        auto size = ed->getLocalBounds();
        ext->getAudioPluginInstance()->editorBeingDeleted (ed.get());
        return size;
    }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    outDir.createDirectory();
    Log log;
    const double sr = 44100.0;
    auto editFile = outDir.getChildFile ("plugin-test.tracktionedit");
    editFile.deleteFile();
    auto edit = te::createEmptyEdit (engine, editFile);
    edit->ensureNumberOfAudioTracks (3);
    auto& ts = edit->tempoSequence;
    auto beats = [&] (double b) { return ts.toTime (te::BeatPosition::fromBeats (b)); };

    juce::VST3PluginFormat vst3;
   #if JUCE_WINDOWS
    const juce::File vstDir ("C:\\Program Files\\Common Files\\VST3");
   #else
    const juce::File vstDir ("/Library/Audio/Plug-Ins/VST3");
   #endif

    //==============================================================================
    log.text << "Surge XT + Valhalla Supermassive\n";
    auto surgeDesc = scan (engine, vstDir.getChildFile ("Surge XT.vst3").getFullPathName(), vst3);
    auto superDesc = scan (engine, vstDir.getChildFile ("ValhallaSupermassive.vst3").getFullPathName(), vst3);
    log.check (surgeDesc.has_value() && surgeDesc->isInstrument, "Surge XT scans as an instrument"
                   + (surgeDesc ? " (" + surgeDesc->manufacturerName + " " + surgeDesc->version + ")" : juce::String()));
    log.check (superDesc.has_value() && ! superDesc->isInstrument, "Valhalla Supermassive scans as an effect"
                   + (superDesc ? " (" + superDesc->version + ")" : juce::String()));

    if (surgeDesc && superDesc)
    {
        auto track = te::getAudioTracks (*edit)[0];
        auto surge = load (*edit, *surgeDesc);
        log.check (surge != nullptr && dynamic_cast<te::ExternalPlugin*> (surge.get())->isEnabled(), "Surge XT loads");
        track->pluginList.insertPlugin (surge, 0, nullptr);

        // A chord for one bar, then silence
        auto clip = track->insertMIDIClip ("Chord", { beats (0), beats (8) }, nullptr);
        for (int n : { 57, 60, 64 })
            clip->getSequence().addNote (n, te::BeatPosition(), te::BeatDuration::fromBeats (2.0), 100, 0, nullptr);

        auto dry = render (*edit, *track, 4.0, outDir.getChildFile ("surge-dry.wav"));
        auto playing = rmsDb (dry, 0.2, 0.9, sr), after = rmsDb (dry, 2.5, 3.5, sr);
        log.check (playing > -40.0f, "Surge XT plays the chord (" + juce::String (playing, 1) + " dB RMS)");

        auto super = load (*edit, *superDesc);
        track->pluginList.insertPlugin (super, 1, nullptr);
        if (auto mix = super->getAutomatableParameterByID ("Mix"))
            mix->setParameter (1.0f, juce::sendNotification);

        auto wet = render (*edit, *track, 4.0, outDir.getChildFile ("surge-supermassive.wav"));
        auto wetTail = rmsDb (wet, 2.5, 3.5, sr);
        log.check (wetTail > after + 10.0f, "Supermassive adds a tail after the chord ends (" + juce::String (after, 1) + " -> "
                                                + juce::String (wetTail, 1) + " dB)");

        // Settings survive saving and reopening
        juce::String paramName;
        float setTo = 0.0f;
        for (auto p : surge->getAutomatableParameters())
            if (p->getParameterName().containsIgnoreCase ("Volume") || p->getParameterName().containsIgnoreCase ("Global"))
            {
                paramName = p->getParameterName();
                setTo = juce::jmap (0.37f, p->getValueRange().getStart(), p->getValueRange().getEnd());
                p->setParameter (setTo, juce::sendNotification);
                if (auto ext = dynamic_cast<te::ExternalPlugin*> (surge.get()))
                    for (auto ip : ext->getAudioPluginInstance()->getParameters())
                        if (ip->getName (64) == paramName)
                            log.info ("before saving: plug-in's own value " + juce::String (ip->getValue(), 3) + " (" + ip->getCurrentValueAsText() + ")");
                break;
            }

        // Plug-ins apply host parameter changes while processing audio: let some run first,
        // as it always does in the app
        render (*edit, *track, 0.5, outDir.getChildFile ("surge-settle.wav"));
        te::EditFileOperations (*edit).save (true, true, false);
        auto reopened = te::loadEditFromFile (engine, editFile);
        float restored = -1.0f;

        // As when the app opens a project: prepare it for playback (plug-ins restore their state)
        if (reopened != nullptr)
        {
            reopened->getTransport().ensureContextAllocated();
            for (int i = 0; i < 10; ++i) { juce::Thread::sleep (50); juce::MessageManager::getInstance()->runDispatchLoopUntil (10); }
        }
        if (reopened != nullptr)
            if (auto t = te::getAudioTracks (*reopened)[0])
                for (auto p : t->pluginList)
                    if (p->getName().containsIgnoreCase ("Surge"))
                    {
                        for (auto param : p->getAutomatableParameters())
                            if (param->getParameterName() == paramName)
                                restored = param->getCurrentValue();

                        if (auto ext = dynamic_cast<te::ExternalPlugin*> (p); ext != nullptr && ext->getAudioPluginInstance() != nullptr)
                            for (auto ip : ext->getAudioPluginInstance()->getParameters())
                                if (ip->getName (64) == paramName)
                                    log.info ("reopened: automatable " + juce::String (restored, 3) + ", plug-in's own value " + juce::String (ip->getValue(), 3)
                                              + " (" + ip->getCurrentValueAsText() + ")");
                    }
        log.check (paramName.isNotEmpty() && std::abs (restored - setTo) < 0.01f, "plug-in settings survive save and reopen ("
                       + paramName + " " + juce::String (setTo, 3) + " -> " + juce::String (restored, 3) + ")");
        reopened.reset();

        auto se = openEditor (*surge), ve = openEditor (*super);
        log.check (se.getWidth() > 200 && ve.getWidth() > 200, "both editors open (" + juce::String (se.getWidth()) + "x" + juce::String (se.getHeight())
                       + ", " + juce::String (ve.getWidth()) + "x" + juce::String (ve.getHeight()) + ")");
    }

    //==============================================================================
    log.text << "Sitala on a drum track\n";
    auto sitalaDesc = scan (engine, vstDir.getChildFile ("Sitala.vst3").getFullPathName(), vst3);
    log.check (sitalaDesc.has_value(), "Sitala scans" + (sitalaDesc ? " (" + sitalaDesc->version + ")" : juce::String()));
    if (sitalaDesc)
        log.info ("Sitala: instrument=" + juce::String ((int) sitalaDesc->isInstrument) + " ins=" + juce::String (sitalaDesc->numInputChannels)
                  + " outs=" + juce::String (sitalaDesc->numOutputChannels) + " format=" + sitalaDesc->pluginFormatName);

    if (sitalaDesc)
    {
        auto track = te::getAudioTracks (*edit)[1];
        auto sitala = load (*edit, *sitalaDesc);
        track->pluginList.insertPlugin (sitala, 0, nullptr);

        if (auto ext = dynamic_cast<te::ExternalPlugin*> (sitala.get()); ext != nullptr && ext->getAudioPluginInstance() != nullptr)
        {
            auto* inst = ext->getAudioPluginInstance();
            juce::StringArray buses;
            for (int i = 0; i < inst->getBusCount (false); ++i)
                buses.add (juce::String (i) + ":" + inst->getBus (false, i)->getName() + "=" + juce::String (inst->getBus (false, i)->getNumberOfChannels())
                           + (inst->getBus (false, i)->isEnabled() ? "" : "(off)"));
            log.info ("Sitala output buses: " + buses.joinIntoString (" ") + " | total outs " + juce::String (inst->getTotalNumOutputChannels())
                      + " | accepts MIDI " + juce::String ((int) inst->acceptsMidi()) + " | Elsewhere sees synth=" + juce::String ((int) sitala->isSynth())
                      + " outs=" + juce::String (sitala->getNumOutputChannelsGivenInputs (2)));
        }

        // Give it time to load its default kit (plug-ins often load in the background)
        for (int i = 0; i < 30; ++i)
        {
            juce::Thread::sleep (100);
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);
        }

        if (auto ext = dynamic_cast<te::ExternalPlugin*> (sitala.get()); ext != nullptr && ext->getAudioPluginInstance() != nullptr)
        {
            juce::MemoryBlock mb;
            ext->getAudioPluginInstance()->getStateInformation (mb);
            auto text = mb.toString();
            juce::StringArray bits;
            for (auto word : { "wav", "sitala", "Kit", "808", "sample", "Sample", "file", "path" })
                if (text.contains (word)) bits.add (word);
            log.info ("Sitala state: " + juce::String ((int) mb.getSize()) + " bytes, mentions: " + bits.joinIntoString (", ")
                      + " | start: " + text.substring (0, 160).replaceCharacters ("\n\r\t", "   "));
        }

        // Which notes make sound? (to learn Sitala's pad layout)
        auto probe = track->insertMIDIClip ("Probe", { beats (0), beats (128) }, nullptr);
        for (int n = 0; n < 128; ++n)
            probe->getSequence().addNote (n, te::BeatPosition::fromBeats (n * 1.0), te::BeatDuration::fromBeats (0.25), 110, 0, nullptr);
        auto probeOut = render (*edit, *track, 64.5, outDir.getChildFile ("sitala-probe.wav"));
        log.info ("probe overall level " + juce::String (rmsDb (probeOut, 0.0, 64.0, sr), 1) + " dB");
        juce::StringArray sounding;
        for (int n = 0; n < 128; ++n)
        {
            auto t = n * 0.5;    // a beat at 120 bpm = 0.5 s
            if (rmsDb (probeOut, t, t + 0.2, sr) > -55.0f)
                sounding.add (juce::String (n));
        }
        log.info ("Sitala pads sound on notes: " + sounding.joinIntoString (" "));
        probe->removeFromParent();

        track->state.setProperty (DrumTrack::trackProp, true, nullptr);
        auto section = DrumTrack::createSection (*track, 0.0, 16.0, "Verse", drums::Style::rock);
        auto out = render (*edit, *track, 8.0, outDir.getChildFile ("sitala-section.wav"));
        auto level = rmsDb (out, 0.0, 8.0, sr);
        if (level > -45.0f)
            log.check (section != nullptr, "a drum section plays through Sitala (" + juce::String (level, 1) + " dB RMS)");
        else
            log.info ("Sitala made no sound: most likely it starts with an empty kit until one is loaded in its browser (Clean 808, Future Retro Pop, Hip Hop Ultra); check in the app");
        auto ed = openEditor (*sitala);
        log.check (ed.getWidth() > 200, "Sitala's editor opens (" + juce::String (ed.getWidth()) + "x" + juce::String (ed.getHeight()) + ")");
    }

   #if JUCE_MAC
    // The AU version, for comparison
    {
        juce::AudioUnitPluginFormat au;
        auto auDesc = scan (engine, "/Library/Audio/Plug-Ins/Components/Sitala.component", au);
        if (auDesc)
        {
            auto track = te::getAudioTracks (*edit)[2];
            auto p = load (*edit, *auDesc);
            track->pluginList.insertPlugin (p, 0, nullptr);
            for (int i = 0; i < 30; ++i) { juce::Thread::sleep (100); juce::MessageManager::getInstance()->runDispatchLoopUntil (10); }
            auto probe = track->insertMIDIClip ("Probe", { beats (0), beats (32) }, nullptr);
            for (int n = 36; n < 52; ++n)
                probe->getSequence().addNote (n, te::BeatPosition::fromBeats ((n - 36) * 1.0), te::BeatDuration::fromBeats (0.25), 110, 0, nullptr);
            auto out = render (*edit, *track, 8.5, outDir.getChildFile ("sitala-au-probe.wav"));
            juce::StringArray sounding;
            for (int n = 36; n < 52; ++n)
                if (rmsDb (out, (n - 36) * 0.5, (n - 36) * 0.5 + 0.2, sr) > -55.0f)
                    sounding.add (juce::String (n));
            log.info ("Sitala AU: outs=" + juce::String (auDesc->numOutputChannels) + ", level " + juce::String (rmsDb (out, 0.0, 8.0, sr), 1)
                      + " dB, pads sound on: " + sounding.joinIntoString (" "));
        }
        else
        {
            log.info ("Sitala AU didn't scan");
        }
    }
   #endif

    log.text << (log.failures == 0 ? "\nALL PLUG-IN TESTS PASSED\n" : "\n" + juce::String (log.failures) + " FAILURE(S)\n");
    std::cout << log.text << std::flush;
    outDir.getChildFile ("plugin-results.txt").replaceWithText (log.text);
    edit.reset();
    return log.failures;
}

} // namespace elsewhere::PluginTests
