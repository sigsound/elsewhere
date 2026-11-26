// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ExportTests.h"
#include "Export.h"
#include "Normalise.h"
#include "Project.h"

namespace elsewhere::ExportTests
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

    std::unique_ptr<juce::AudioFormatReader> open (const juce::File& f)
    {
        juce::AudioFormatManager m;
        m.registerBasicFormats();
        return std::unique_ptr<juce::AudioFormatReader> (m.createReaderFor (f));
    }

    juce::AudioBuffer<float> readAll (const juce::File& f)
    {
        juce::AudioBuffer<float> b;

        if (auto r = open (f))
        {
            b.setSize ((int) r->numChannels, (int) r->lengthInSamples);
            r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
        }

        return b;
    }

    te::ClipPosition secs (double a, double b)  { return { { te::TimePosition::fromSeconds (a), te::TimePosition::fromSeconds (b) }, {} }; }

    void render (te::Edit& edit, Export::Plan& plan)
    {
        plan.folder.createDirectory();

        // The way the export's RenderQueue renders each job, waited for here
        for (auto& job : plan.jobs)
        {
            te::ScopedTrackMuter muter (edit, job.tracksToMute);
            // It needs the message thread, so keep that turning while it renders
            std::atomic<bool> done { false };
            auto handle = te::EditRenderer::render (job.params, [&done] (auto) { done = true; }, nullptr);
            auto deadline = juce::Time::getMillisecondCounter() + 60000;

            while (! done && juce::Time::getMillisecondCounter() < deadline)
                juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

            if (! done)
                std::cout << "render timed out: " << job.name << std::endl;

            handle.reset();
        }
    }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    Log log;
    outDir.deleteRecursively();
    outDir.createDirectory();

    auto editFile = Project::createProjectFolder (outDir, "Export");
    auto samples = editFile.getParentDirectory().getChildFile ("Samples/Imported");
    writeSine (samples.getChildFile ("bass.wav"), 2.0, 0.01f, 0.4f);
    writeSine (samples.getChildFile ("keys.wav"), 2.0, 0.05f, 0.3f);

    Project p (engine, editFile);
    auto& edit = p.getEdit();
    auto exports = p.getFolder().getChildFile ("Exports");

    // Bass and Keys (the keys sending to a return), an empty track and a muted one
    edit.ensureNumberOfAudioTracks (4);
    auto tracks = te::getAudioTracks (edit);
    tracks[0]->setName ("Bass");
    tracks[1]->setName ("Keys");
    tracks[2]->setName ("Empty");
    tracks[3]->setName ("Muted");
    tracks[0]->insertWaveClip ("bass", samples.getChildFile ("bass.wav"), secs (0, 2), false);
    tracks[1]->insertWaveClip ("keys", samples.getChildFile ("keys.wav"), secs (1, 3), false);
    tracks[3]->insertWaveClip ("bass", samples.getChildFile ("bass.wav"), secs (0, 2), false);
    tracks[3]->setMute (true);

    auto ret = Mixing::createReturnTrack (edit);

    for (auto send : Mixing::getSends (*tracks[1]))
        send->setGainDb (-6.0f);

    // No master processing, so the mix and the sum of the stems should be the same
    for (auto plugin : edit.getMasterPluginList().getPlugins())
        if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) == nullptr && dynamic_cast<te::LevelMeterPlugin*> (plugin) == nullptr)
            plugin->deleteFromParent();

    Export::Settings s;
    s.tails = false;
    s.format = te::RenderFormat::wav;
    s.bitDepth = 32;
    s.sampleRate = 48000.0;

    {
        auto plan = Export::plan (edit, s, exports, "Song");
        log.check (plan.jobs.size() == 1 && plan.mixFile == exports.getChildFile ("Song.wav"), "the mix goes to Exports/Song.wav");
        log.check (Export::describe (plan, s, p.getFolder()).startsWith ("Writes Song.wav to Exports, 32-bit float 48 kHz WAV"),
                   "and says so: " + Export::describe (plan, s, p.getFolder()).upToFirstOccurrenceOf ("\n", false, false));
    }

    s.what = Export::What::mixAndStems;
    auto plan = Export::plan (edit, s, exports, "Song");
    juce::StringArray names;
    for (auto& job : plan.jobs)
        names.add (job.params.destFile.getFileName());

    log.check (plan.problems.isEmpty(), "no problems planning (" + plan.problems.joinIntoString ("; ") + ")");
    log.check (plan.numStems == 3, "3 stems: the empty and muted tracks are left out (" + juce::String (plan.numStems) + ")");
    log.check (names.joinIntoString (", ") == "00 Song (Mix).wav, 01 Bass.wav, 02 Keys.wav, 03 " + ret->getName() + ".wav",
               "numbered in track order, with the mix first (" + names.joinIntoString (", ") + ")");
    log.check (plan.folder == exports.getChildFile ("Song Stems"), "in Exports/Song Stems");

    render (edit, plan);

    auto mix = readAll (plan.mixFile);
    log.check (mix.getNumSamples() == 3 * 48000, "the mix is the song's 3 s at 48 kHz (" + juce::String (mix.getNumSamples()) + " samples)");

    if (auto r = open (plan.mixFile))
        log.check (r->bitsPerSample == 32 && r->usesFloatingPointData && r->numChannels == 2, "32-bit float stereo");

    // The null test: everything the mix has is in exactly one stem
    juce::AudioBuffer<float> sum (mix.getNumChannels(), mix.getNumSamples());
    sum.clear();
    bool sameLengths = true;

    for (auto& job : plan.jobs)
    {
        if (job.params.destFile == plan.mixFile)
            continue;

        auto stem = readAll (job.params.destFile);
        sameLengths = sameLengths && stem.getNumSamples() == mix.getNumSamples();

        for (int ch = 0; ch < juce::jmin (sum.getNumChannels(), stem.getNumChannels()); ++ch)
            sum.addFrom (ch, 0, stem, ch, 0, juce::jmin (sum.getNumSamples(), stem.getNumSamples()));
    }

    // Stems come before the master fader, so it's applied here to compare
    auto masterDb = edit.getMasterVolumePlugin()->getVolumeDb();
    sum.applyGain (juce::Decibels::decibelsToGain (masterDb));

    // The engine's short automatic fades at clip edges come out very slightly differently when a
    // track renders alone, so those few milliseconds are checked apart from the rest
    auto nearClipEdge = [&] (int i)
    {
        for (auto t : tracks)
            for (auto c : t->getClips())
                for (auto edge : { c->getPosition().getStart(), c->getPosition().getEnd() })
                    if (std::abs (i - edge.inSeconds() * 48000.0) < 240.0)
                        return true;
        return false;
    };

    float worst = 0.0f, worstAtEdges = 0.0f;
    for (int ch = 0; ch < sum.getNumChannels(); ++ch)
        for (int i = 0; i < sum.getNumSamples(); ++i)
            (nearClipEdge (i) ? worstAtEdges : worst) = juce::jmax (nearClipEdge (i) ? worstAtEdges : worst,
                                                                    std::abs (sum.getSample (ch, i) - mix.getSample (ch, i)));

    log.check (sameLengths, "every stem starts and ends with the mix");
    log.check (mix.getMagnitude (0, mix.getNumSamples()) > 0.1f, "the mix has the music in it");
    log.check (worst < 1.0e-6f, "with the master fader (" + juce::String (masterDb, 1) + " dB), the stems add up to the mix (worst difference " + juce::String (juce::Decibels::gainToDecibels (worst), 1) + " dB)");
    log.check (worstAtEdges < 3.0e-3f, "and within -50 dB in the clip-edge fades (" + juce::String (juce::Decibels::gainToDecibels (worstAtEdges), 1) + " dB)");

    // Rendering is repeatable: the same stem twice gives the same audio, and the return alone
    // (its source muted but still feeding it) matches the return in the mix
    {
        auto again = Export::plan (edit, s, exports, "Again");
        render (edit, again);
        auto a = readAll (plan.jobs.back().params.destFile), b = readAll (again.jobs.back().params.destFile);
        float diff = 0.0f;
        for (int ch = 0; ch < juce::jmin (a.getNumChannels(), b.getNumChannels()); ++ch)
            for (int i = 0; i < juce::jmin (a.getNumSamples(), b.getNumSamples()); ++i)
                diff = juce::jmax (diff, std::abs (a.getSample (ch, i) - b.getSample (ch, i)));
        log.check (diff == 0.0f, "the return's stem renders the same twice (difference " + juce::String (juce::Decibels::gainToDecibels (diff), 1) + " dB)");

        // Mix with only the keys and the return playing, minus the keys' stem, is the return in the mix
        tracks[0]->setMute (true);
        auto keysMix = Export::plan (edit, [&] { auto k = s; k.what = Export::What::mix; return k; }(), exports, "KeysMix");
        render (edit, keysMix);
        tracks[0]->setMute (false);
        auto km = readAll (keysMix.mixFile), keys = readAll (plan.jobs[2].params.destFile);
        float rdiff = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < km.getNumSamples(); ++i)
                rdiff = juce::jmax (rdiff, std::abs (km.getSample (ch, i) / juce::Decibels::decibelsToGain (masterDb) - keys.getSample (ch, i) - a.getSample (ch, i)));
        log.check (rdiff < 1.0e-5f, "the return in the mix is its stem (difference " + juce::String (juce::Decibels::gainToDecibels (rdiff), 1) + " dB)");
    }

    auto retStem = readAll (plan.jobs.back().params.destFile);
    log.check (retStem.getMagnitude (0, 48000) < 1.0e-6f && retStem.getMagnitude (48000, 96000) > 0.05f,
               "the return's stem has only the Keys' send in it");

    // Smaller formats
    s.what = Export::What::mix;
    s.format = te::RenderFormat::flac;
    s.bitDepth = 16;
    s.sampleRate = 44100.0;
    s.dither = true;
    {
        auto flac = Export::plan (edit, s, exports, "Song");
        render (edit, flac);

        if (auto r = open (flac.mixFile))
            log.check (flac.mixFile.hasFileExtension ("flac") && r->bitsPerSample == 16 && r->sampleRate == 44100.0, "16-bit 44.1 kHz FLAC, dithered");
        else
            log.check (false, "16-bit FLAC written");
    }

    s.format = te::RenderFormat::wav;
    s.bitDepth = 24;
    s.loudness = Export::Loudness::streaming;
    {
        auto loud = Export::plan (edit, s, exports, "Loud");
        render (edit, loud);

        auto clip = te::getAudioTracks (edit)[2]->insertWaveClip ("loud", loud.mixFile, secs (10, 13), false);
        auto m = Normalise::measure (Normalise::wholeClip (*clip));
        auto lufs = Normalise::loudnessOf (m.blocks);
        auto peak = juce::Decibels::gainToDecibels (m.peak);
        log.check (lufs < -13.5 && peak <= -0.9f, "streaming target: " + juce::String (lufs, 1) + " LUFS, peak " + juce::String (peak, 1) + " dB");
        clip->removeFromParent();
    }

    if (Export::canWriteMp3 (engine))
    {
        s.format = te::RenderFormat::mp3;
        s.quality = 23;
        s.loudness = Export::Loudness::off;
        auto mp3 = Export::plan (edit, s, exports, "Song");
        render (edit, mp3);
        log.check (mp3.mixFile.hasFileExtension ("mp3") && mp3.mixFile.getSize() > 10000, "MP3 through LAME (" + juce::String (mp3.mixFile.getSize()) + " bytes)");
    }
    else
    {
        log.text << "  SKIP  MP3 (LAME isn't installed)\n";
    }

    // The AI training notice: in each format, the file carries it and still plays exactly as rendered
    {
        auto contains = [] (const juce::File& f, const juce::String& text)
        {
            juce::MemoryBlock m;
            f.loadFileAsData (m);
            auto needle = text.toStdString();
            return std::string (static_cast<const char*> (m.getData()), m.getSize()).find (needle) != std::string::npos;
        };

        auto sameAudio = [] (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b)
        {
            if (a.getNumSamples() == 0 || a.getNumChannels() != b.getNumChannels() || a.getNumSamples() != b.getNumSamples())
                return false;

            for (int ch = 0; ch < a.getNumChannels(); ++ch)
                if (std::memcmp (a.getReadPointer (ch), b.getReadPointer (ch), sizeof (float) * (size_t) a.getNumSamples()) != 0)
                    return false;

            return true;
        };

        s.what = Export::What::mixAndStems;
        s.loudness = Export::Loudness::off;
        s.reserveRights = true;
        s.sampleRate = 44100.0;

        std::vector<te::RenderFormat> formats { te::RenderFormat::wav, te::RenderFormat::aiff, te::RenderFormat::flac, te::RenderFormat::ogg };

        if (Export::canWriteMp3 (engine))
            formats.push_back (te::RenderFormat::mp3);

        for (auto format : formats)
        {
            s.format = format;
            s.bitDepth = 24;

            if (auto q = Export::qualityChoices (format); ! q.empty())
                s.quality = q.front().value;

            auto marked = Export::plan (edit, s, exports, "Marked " + Export::formatName (format));
            auto name = Export::formatName (format);
            log.check (marked.notice.has_value() && Export::describe (marked, s, p.getFolder()).contains ("not licensed for AI training"),
                       name + ": the export plans the notice and says so");
            render (edit, marked);

            bool allMarked = true, allSame = true, allOpen = true;
            juce::String why;

            for (auto& job : marked.jobs)
            {
                auto f = job.params.destFile;
                auto before = readAll (f);

                if (auto problem = Export::finishFile (marked, f); problem.isNotEmpty())
                {
                    allMarked = false;
                    why << f.getFileName() << ": " << problem << "; ";
                    continue;
                }

                allOpen = allOpen && open (f) != nullptr;
                auto after = readAll (f);

                // MP3 can't be read back here without its decoder, so the tag's shape is checked instead
                if (format == te::RenderFormat::mp3 && after.getNumSamples() == 0)
                {
                    juce::MemoryBlock m;
                    f.loadFileAsData (m);
                    allSame = allSame && m.getSize() > 10 && std::memcmp (m.getData(), "ID3\x03", 4) == 0;
                }
                else
                {
                    allSame = allSame && sameAudio (before, after);
                }

                bool has = format == te::RenderFormat::flac || format == te::RenderFormat::ogg
                             ? contains (f, "TDM_RESERVATION=1") && contains (f, "COMMENT=Not licensed for AI training")
                             : contains (f, "DMI-PROHIBITED-AIMLTRAINING") && contains (f, "<tdm:reservation>1</tdm:reservation>");
                allMarked = allMarked && has;
                if (! has) why << f.getFileName() << " has no notice; ";
            }

            log.check (allMarked, name + ": the mix and " + juce::String (marked.numStems) + " stems carry the notice" + (why.isEmpty() ? juce::String() : " (" + why + ")"));
            log.check (allSame, name + ": and play exactly as rendered");

            if (format != te::RenderFormat::mp3)
                log.check (allOpen, name + ": and still open");

            if (format == te::RenderFormat::wav)
                if (auto r = open (marked.mixFile))
                    log.check (r->metadataValues["ICOP"].contains ("All rights reserved") && r->metadataValues["ICMT"].startsWith ("Not licensed for AI training"),
                               "WAV: the copyright and comment show in its INFO chunk (" + r->metadataValues["ICOP"] + ")");

            if (format == te::RenderFormat::ogg)
                if (auto r = open (marked.mixFile))
                    log.check (r->metadataValues[juce::OggVorbisAudioFormat::id3comment].startsWith ("Not licensed for AI training"),
                               "Ogg: the comment shows to a Vorbis reader");
        }

        // Turned off, nothing is added
        s.reserveRights = false;
        s.format = te::RenderFormat::wav;
        auto plain = Export::plan (edit, s, exports, "Plain");
        log.check (! plain.notice.has_value() && Export::finishFile (plain, plain.mixFile).isEmpty()
                     && ! Export::describe (plain, s, p.getFolder()).contains ("AI training"),
                   "with No notice, nothing is added");
    }

    // Match level: a device that turns the sound up by 6 dB (its most) plays at its input's level with it on
    {
        auto matchFile = Project::createProjectFolder (outDir, "Match");
        auto matchSamples = matchFile.getParentDirectory().getChildFile ("Samples/Imported");
        writeSine (matchSamples.getChildFile ("tone.wav"), 6.0, 0.02f, 0.1f);
        Project mp (engine, matchFile);
        auto& me = mp.getEdit();
        me.ensureNumberOfAudioTracks (1);
        auto track = te::getAudioTracks (me)[0];
        track->insertWaveClip ("tone", matchSamples.getChildFile ("tone.wav"), secs (0, 6), false);

        for (auto plugin : me.getMasterPluginList().getPlugins())
            if (dynamic_cast<te::VolumeAndPanPlugin*> (plugin) == nullptr && dynamic_cast<te::LevelMeterPlugin*> (plugin) == nullptr)
                plugin->deleteFromParent();

        auto boost = me.getPluginCache().createNewPlugin (te::VolumeAndPanPlugin::xmlTypeName, {});
        track->pluginList.insertPlugin (boost, 0, nullptr);
        auto* vol = dynamic_cast<te::VolumeAndPanPlugin*> (boost.get());

        Export::Settings ms;
        ms.tails = false;
        ms.format = te::RenderFormat::wav;
        ms.bitDepth = 32;
        ms.sampleRate = 48000.0;
        ms.reserveRights = false;

        // The last two seconds, once the level has settled
        auto tailDb = [] (const juce::AudioBuffer<float>& b)
        {
            auto n = juce::jmin (b.getNumSamples(), 96000);
            return b.getNumSamples() == 0 ? -200.0f : juce::Decibels::gainToDecibels (b.getRMSLevel (0, b.getNumSamples() - n, n));
        };

        auto renderAs = [&] (const juce::String& name)
        {
            auto plan = Export::plan (me, ms, mp.getFolder().getChildFile ("Exports"), name);
            render (me, plan);
            return tailDb (readAll (plan.mixFile));
        };

        vol->setVolumeDb (0.0f);
        const auto dry = renderAs ("Dry");
        vol->setVolumeDb (6.0f);
        const auto loud = renderAs ("Loud");
        boost->setGainMatched (true);
        const auto matched = renderAs ("Matched");

        log.check (vol != nullptr && std::abs (loud - dry - 6.0f) < 0.5f, "a +6 dB device plays 6 dB louder (" + juce::String (loud - dry, 2) + " dB)");
        log.check (std::abs (matched - dry) < 0.5f, "with Match Level on, it plays at its input's level (" + juce::String (matched - dry, 2) + " dB)");
        log.check (std::abs (boost->getGainMatchDb() + 6.0f) < 0.5f, "and reports the gain it applied (" + juce::String (boost->getGainMatchDb(), 2) + " dB)");
    }

    std::cout << "Export tests\n" << log.text << (log.failures == 0 ? "All passed\n" : juce::String (log.failures) + " failed\n");
    return log.failures;
}

} // namespace elsewhere::ExportTests
