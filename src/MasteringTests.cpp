// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "MasteringTests.h"
#include "Mastering.h"

namespace elsewhere::MasteringTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
    };

    /** A sine of `cycles` radians per sample, `level` peak, starting at `phase`, mono or stereo. */
    void writeSine (const juce::File& f, double seconds, double cycles, float level, int channels = 1, double phase = 0.0)
    {
        f.getParentDirectory().createDirectory();
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream().release());
        auto w = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (48000.0)
                                               .withNumChannels (channels).withBitsPerSample (24));

        if (w == nullptr)
            return;

        juce::AudioBuffer<float> b (channels, (int) (48000.0 * seconds));

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < b.getNumSamples(); ++i)
                b.setSample (ch, i, level * (float) std::sin (i * cycles + phase));

        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    double lengthOf (const juce::File& f)
    {
        juce::AudioFormatManager m;
        m.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> r (m.createReaderFor (f));
        return r != nullptr ? (double) r->lengthInSamples / r->sampleRate : 0.0;
    }

    /** A song's project: one track playing a sine for `seconds`. */
    juce::File makeSong (te::Engine& engine, const juce::File& parent, const juce::String& name, float level, double seconds)
    {
        auto file = Project::createProjectFolder (parent, name);
        auto sample = file.getParentDirectory().getChildFile ("Samples/Imported/tone.wav");
        writeSine (sample, seconds, 2.0 * juce::MathConstants<double>::pi * 220.0 / 48000.0, level);

        Project p (engine, file);
        auto& edit = p.getEdit();
        edit.ensureNumberOfAudioTracks (1);
        te::getAudioTracks (edit)[0]->insertWaveClip ("tone", sample, { { te::TimePosition(), te::TimeDuration::fromSeconds (seconds) }, {} }, false);
        p.save (false);
        return file;
    }

    bool waitFor (std::function<bool()> done, int ms = 120000)
    {
        auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) ms;

        while (! done() && juce::Time::getMillisecondCounter() < deadline)
            juce::MessageManager::getInstance()->runDispatchLoopUntil (10);

        return done();
    }

    void render (te::Edit& edit, Export::Plan& plan)
    {
        plan.folder.createDirectory();

        for (auto& job : plan.jobs)
        {
            te::ScopedTrackMuter muter (edit, job.tracksToMute);
            std::atomic<bool> done { false };
            auto handle = te::EditRenderer::render (job.params, [&done] (auto) { done = true; }, nullptr);
            waitFor ([&done] { return done.load(); }, 60000);
            handle.reset();
        }
    }

    void testReferences (Log& log, const juce::File& outDir)
    {
        auto root = outDir.getChildFile ("References");
        auto album = root.getChildFile ("Album Project");
        auto song = root.getChildFile ("Songs/One Project/One.elsewhere");
        album.createDirectory();
        song.create();

        auto ref = Mastering::makeRef (song, album);
        log.check (ref.relative == "../Songs/One Project/One.elsewhere", "a song is kept relative to the album (" + ref.relative + ")");
        log.check (Mastering::resolve (ref, album) == song, "and found there");

        // The album and its songs move together: found beside the album, not where they were
        auto moved = outDir.getChildFile ("Moved");
        moved.deleteRecursively();
        root.copyDirectoryTo (moved);
        root.getChildFile ("Songs").deleteRecursively();
        log.check (Mastering::resolve (ref, moved.getChildFile ("Album Project")) == moved.getChildFile ("Songs/One Project/One.elsewhere"),
                   "when the folders move together, the song is found beside the album");

        // Only the album moved: the song is still where it was
        song.create();
        auto alone = outDir.getChildFile ("Alone/Album Project");
        alone.createDirectory();
        log.check (Mastering::resolve (ref, alone) == song, "when only the album moves, the song is found where it was");

        // Neither: where it's expected, so it shows as missing
        song.deleteFile();
        auto missing = Mastering::resolve (ref, alone);
        log.check (! missing.existsAsFile() && missing == alone.getChildFile (ref.relative), "a song that's gone is missing, where it was expected");
    }

    void testStaleness (Log& log, const juce::File& outDir)
    {
        auto song = outDir.getChildFile ("Stale/Song Project/Song.elsewhere");
        auto render = outDir.getChildFile ("Stale/Album Project/Renders/Song.wav");
        song.create();
        auto now = juce::Time::getCurrentTime() - juce::RelativeTime::minutes (10);
        song.setLastModificationTime (now);
        auto version = Mastering::songModTime (song);

        log.check (Mastering::needsRender (song, render, version), "a song with no render needs one");
        render.create();
        log.check (! Mastering::needsRender (song, render, version), "a render of this version is up to date");

        song.setLastModificationTime (now + juce::RelativeTime::seconds (30));
        log.check (Mastering::needsRender (song, render, version), "saving the song makes its render out of date");

        song.setLastModificationTime (now);
        auto snapshot = song.getParentDirectory().getChildFile ("Collab/snapshots/Someone.elsewhere");
        snapshot.create();
        snapshot.setLastModificationTime (now + juce::RelativeTime::seconds (60));
        log.check (Mastering::needsRender (song, render, version), "so does a collaborator's newer snapshot of it");
    }

    void testLayout (Log& log)
    {
        auto starts = Mastering::layout ({ { 1.0, 10.0 }, { 2.0, 20.0 }, { -3.0, 5.0 }, { -100.0, 5.0 } });
        log.check (starts == std::vector<double> { 1.0, 13.0, 30.0, 30.0 },
                   "songs follow each other after their gaps; a negative gap overlaps, but never starts before the song before");

        auto first = Mastering::layout ({ { -5.0, 10.0 } });
        log.check (first == std::vector<double> { 0.0 }, "the album never starts before zero");
    }

    void testLoudness (Log& log, const juce::File& outDir)
    {
        // A 1 kHz sine at -20 dBFS in one channel is -23 LUFS (K-weighting adds about 0.7 dB there)
        auto tone = outDir.getChildFile ("Loudness/tone.wav");
        writeSine (tone, 5.0, 2.0 * juce::MathConstants<double>::pi * 1000.0 / 48000.0, 0.1f);
        auto m = Mastering::measureFile (tone);
        log.check (m.ok && std::abs (m.lufs - -23.0) < 0.2, "a -20 dBFS 1 kHz tone measures -23 LUFS (" + juce::String (m.lufs, 2) + ")");
        log.check (std::abs (m.seconds - 5.0) < 0.001, "and 5 s long");

        // A quarter of the sample rate, a quarter cycle out: every sample is at 0.707 of the peak, which
        // falls between samples. True peak finds it
        auto between = outDir.getChildFile ("Loudness/between.wav");
        writeSine (between, 2.0, juce::MathConstants<double>::pi / 2.0, 0.5f, 2, juce::MathConstants<double>::pi / 4.0);
        auto p = Mastering::measureFile (between);
        log.check (std::abs (p.truePeak - -6.02) < 0.3, "true peak finds a peak between samples: -6.0 dBTP where the samples reach -9.0 (" + juce::String (p.truePeak, 2) + ")");

        auto gains = Mastering::matchGains ({ -10.0, -18.5, -std::numeric_limits<double>::infinity(), -12.0 }, { 0.0f, 2.0f, 1.0f, 20.0f }, -14.0);
        log.check (gains == std::vector<float> { -4.0f, 6.5f, 1.0f, 18.0f } || (std::abs (gains[0] + 4.0f) < 1.0e-4f && std::abs (gains[1] - 6.5f) < 1.0e-4f
                                                                             && gains[2] == 1.0f && std::abs (gains[3] - 18.0f) < 1.0e-4f),
                   "matching moves each song's gain by how far it missed, and leaves silent ones alone");
        log.check (Mastering::matchGains ({ -40.0 }, { 0.0f }, -14.0)[0] == 24.0f, "within the chain's gain range");
        log.check (Mastering::isMatched ({ -14.2, -13.9 }, -14.0, 0.3) && ! Mastering::isMatched ({ -14.2, -13.5 }, -14.0, 0.3), "matched within a tolerance");
    }

    void testNames (Log& log)
    {
        auto names = Mastering::songFileNames ({ "Opening", "AC/DC: Live?", "Opening", "" }, ".wav");
        log.check (names.joinIntoString ("|") == "01 Opening.wav|02 ACDC Live.wav|03 Opening 2.wav|04 Track 4.wav",
                   "songs are numbered in order, with names a file can have and no two alike (" + names.joinIntoString ("|") + ")");

        juce::StringArray many;
        for (int i = 0; i < 100; ++i)
            many.add ("S");
        log.check (Mastering::songFileNames (many, ".flac")[0] == "001 S.flac", "three digits past 99 songs");
        log.check (Mastering::albumFileName ("Night Drive", ".wav") == "Night Drive (Full Album).wav", "the whole album is one file named after it");
    }

    void testAlbum (Log& log, te::Engine& engine, const juce::File& outDir)
    {
        auto root = outDir.getChildFile ("Album");
        auto songs = root.getChildFile ("Songs");
        auto loud = makeSong (engine, songs, "Loud Song", 0.5f, 3.0);
        auto quiet = makeSong (engine, songs, "Quiet Song", 0.08f, 2.0);

        auto file = Mastering::createProject (engine, root, "Test Album");
        log.check (Mastering::isMasteringFile (file), "a new mastering project is marked as one in its file");
        log.check (! Mastering::isMasteringFile (loud), "and a song isn't");

        Project project (engine, file);
        auto& edit = project.getEdit();
        log.check (Mastering::isMastering (edit) && Mastering::albumChainOf (edit) != nullptr && Mastering::getSongs (edit).isEmpty(),
                   "it starts with no songs and a mastering chain for the album");

        auto* first = Mastering::addSong (edit, loud);
        auto* second = Mastering::addSong (edit, quiet);
        log.check (first != nullptr && second != nullptr && Mastering::chainOf (*first) != nullptr, "each song gets its own mastering chain");

        auto album = std::make_unique<Mastering::Album> (edit);
        auto rendered = [&]
        {
            return ! album->isBusy() && Mastering::clipOf (*first) != nullptr && Mastering::clipOf (*second) != nullptr;
        };
        log.check (waitFor (rendered), "both songs are rendered in the background");

        if (! rendered())
            return;

        auto r1 = Mastering::rangeOf (*first), r2 = Mastering::rangeOf (*second);
        log.check (r1.getStart() == te::TimePosition() && std::abs (r1.getLength().inSeconds() - 3.0) < 0.1,
                   "the first song starts the album, as long as its mix (" + juce::String (r1.getLength().inSeconds(), 2) + " s)");
        log.check (std::abs ((r2.getStart() - r1.getEnd()).inSeconds() - Mastering::defaultGap) < 1.0e-6, "the second follows after the gap");

        auto mix1 = Mastering::getProperty (*first, Mastering::mixLufsId).value_or (-100.0);
        auto mix2 = Mastering::getProperty (*second, Mastering::mixLufsId).value_or (-100.0);
        log.check (mix1 - mix2 > 14.0 && mix1 - mix2 < 18.0, "each mix's loudness is measured (" + juce::String (mix1, 1) + " and " + juce::String (mix2, 1) + " LUFS)");

        // Reordering swaps them, keeping the album's start
        Mastering::moveSong (edit, *second, 0);
        log.check (Mastering::getSongs (edit)[0] == second && Mastering::rangeOf (*second).getStart() == te::TimePosition()
                     && std::abs ((Mastering::rangeOf (*first).getStart() - Mastering::rangeOf (*second).getEnd()).inSeconds() - Mastering::defaultGap) < 1.0e-6,
                   "moving a song first reorders the album, and the gaps go with it");
        Mastering::moveSong (edit, *second, 1);

        Mastering::setGap (*second, 0.5);
        log.check (std::abs ((Mastering::rangeOf (*second).getStart() - Mastering::rangeOf (*first).getEnd()).inSeconds() - 0.5) < 1.0e-6, "a gap can be set");

        // Match loudness: both reach the album's target after their chains
        Mastering::setTarget (edit, -14.0);
        album->measure (true);
        log.check (waitFor ([&] { return ! album->isMeasuring(); }), "matching the loudness finishes");
        auto m1 = Mastering::getProperty (*first, Mastering::masterLufsId).value_or (-100.0);
        auto m2 = Mastering::getProperty (*second, Mastering::masterLufsId).value_or (-100.0);
        auto p1 = Mastering::getProperty (*first, Mastering::masterPeakId).value_or (100.0);
        log.check (std::abs (m1 - -14.0) < 1.0 && std::abs (m2 - -14.0) < 1.0,
                   "both songs reach -14 LUFS after mastering (" + juce::String (m1, 1) + " and " + juce::String (m2, 1) + ")");
        log.check (p1 < 0.0, "with true peaks measured, under 0 dBTP (" + juce::String (p1, 1) + ")");

        // Exports: each song, numbered, and the whole album
        Export::Settings s;
        s.format = te::RenderFormat::wav;
        s.bitDepth = 24;
        s.sampleRate = 48000.0;
        s.reserveRights = false;
        auto folder = Mastering::newExportFolder (project.getFolder().getChildFile ("Exports"), "Test Album");
        log.check (folder.getFileName() == "Test Album", "exports go in a folder named after the album");
        auto plan = Mastering::planExport (edit, s, folder, true, true);
        juce::StringArray names;
        for (auto& job : plan.jobs)
            names.add (job.params.destFile.getFileName());
        log.check (plan.problems.isEmpty() && names.joinIntoString ("|") == "01 Loud Song.wav|02 Quiet Song.wav|Test Album (Full Album).wav",
                   "one file per song in order, and one for the album (" + names.joinIntoString ("|") + plan.problems.joinIntoString ("; ") + ")");

        render (edit, plan);
        auto song1 = lengthOf (folder.getChildFile ("01 Loud Song.wav"));
        auto whole = lengthOf (folder.getChildFile ("Test Album (Full Album).wav"));
        log.check (std::abs (song1 - Mastering::rangeOf (*first).getLength().inSeconds()) < 0.02, "a song's file is just that song (" + juce::String (song1, 2) + " s)");
        log.check (std::abs (whole - Mastering::albumEnd (edit).inSeconds()) < 0.6, "the album's file is the whole album (" + juce::String (whole, 2) + " s)");
        log.check (std::abs (Mastering::measureFile (folder.getChildFile ("02 Quiet Song.wav")).lufs - -14.0) < 1.0, "and the songs come out matched");
        log.check (Mastering::newExportFolder (project.getFolder().getChildFile ("Exports"), "Test Album").getFileName() == "Test Album 2",
                   "exporting again doesn't write over the last one");

        // Saving the song makes the album render it again
        auto oldRender = Mastering::clipOf (*first)->getSourceFileReference().getFile();
        loud.setLastModificationTime (juce::Time::getCurrentTime() + juce::RelativeTime::seconds (5));
        album->check();
        log.check (album->getStatus (*first) == Mastering::Album::Status::waiting || album->getStatus (*first) == Mastering::Album::Status::rendering,
                   "a song saved since its render is rendered again");
        waitFor ([&] { return ! album->isBusy(); });
        auto newRender = Mastering::clipOf (*first)->getSourceFileReference().getFile();
        log.check (newRender != oldRender && newRender.existsAsFile(), "into a new file, so the one playing isn't written over");
        log.check (! Mastering::needsRender (loud, newRender, (juce::int64) first->state[Mastering::renderedFromId]), "and it's up to date");

        // A song that moved shows as missing, and can be relinked
        auto movedTo = root.getChildFile ("Elsewhere/Quiet Song Project");
        movedTo.getParentDirectory().createDirectory();
        quiet.getParentDirectory().moveFileTo (movedTo);
        log.check (album->getStatus (*second) == Mastering::Album::Status::missing, "a song whose project has gone is missing");
        Mastering::relink (*second, movedTo.getChildFile ("Quiet Song.elsewhere"));
        log.check (album->getStatus (*second) != Mastering::Album::Status::missing && Mastering::refOf (*second).relative == "../Elsewhere/Quiet Song Project/Quiet Song.elsewhere",
                   "relinked, it's found again (" + Mastering::refOf (*second).relative + ")");

        album.reset();
        project.save (false);
    }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    Log log;
    outDir.deleteRecursively();
    outDir.createDirectory();

    testReferences (log, outDir);
    testStaleness (log, outDir);
    testLayout (log);
    testLoudness (log, outDir);
    testNames (log);
    testAlbum (log, engine, outDir);

    std::cout << "Mastering tests\n" << log.text << (log.failures == 0 ? "All passed\n" : juce::String (log.failures) + " failed\n");
    return log.failures;
}

} // namespace elsewhere::MasteringTests
