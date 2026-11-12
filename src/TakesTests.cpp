// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "TakesTests.h"
#include "Project.h"
#include "Takes.h"

namespace elsewhere::TakesTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
    };

    void writeSine (const juce::File& f, double seconds, float freq)
    {
        f.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream().release());
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (out.get(), 44100.0, 1, 16, {}, 0));

        if (w == nullptr)
            return;

        out.release();
        juce::AudioBuffer<float> b (1, (int) (44100.0 * seconds));

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * std::sin ((float) i * freq));

        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    te::TimeRange secs (double a, double b)  { return { te::TimePosition::fromSeconds (a), te::TimePosition::fromSeconds (b) }; }

    /** "start-end:file" for each clip on the track, in order, e.g. "2-3:b 3-6:c". */
    juce::String describe (te::AudioTrack& t)
    {
        juce::StringArray parts;
        auto clips = t.getClips();
        std::sort (clips.begin(), clips.end(), [] (auto a, auto b) { return a->getPosition().getStart() < b->getPosition().getStart(); });

        for (auto c : clips)
            if (auto w = dynamic_cast<te::WaveAudioClip*> (c))
                parts.add (juce::String (w->getPosition().getStart().inSeconds(), 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + "-"
                           + juce::String (w->getPosition().getEnd().inSeconds(), 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".") + ":"
                           + w->getOriginalFile().getFileNameWithoutExtension());

        return parts.joinIntoString (" ");
    }

    Takes::Take takeNumbered (te::Track& t, int n)
    {
        for (auto& take : Takes::getTakes (t))
            if (take.number == n)
                return take;

        return {};
    }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    Log log;
    outDir.deleteRecursively();
    outDir.createDirectory();

    auto editFile = Project::createProjectFolder (outDir, "Takes");
    auto recorded = editFile.getParentDirectory().getChildFile ("Samples/Recorded");

    for (auto [name, freq] : { std::pair { "a", 0.03f }, { "b", 0.05f }, { "c", 0.07f }, { "d", 0.09f },
                               { "l1", 0.02f }, { "l2", 0.04f }, { "l3", 0.06f } })
        writeSine (recorded.getChildFile (juce::String (name) + ".wav"), 4.0, freq);

    {
        Project p (engine, editFile);
        auto& edit = p.getEdit();
        auto& um = edit.getUndoManager();
        auto& track = *te::getAudioTracks (edit).getFirst();

        // Three passes over 2-6 s and one take elsewhere
        um.beginNewTransaction();
        Takes::addTake (track, recorded.getChildFile ("a.wav"), secs (2, 6), {});
        Takes::addTake (track, recorded.getChildFile ("b.wav"), secs (2, 6), {});
        Takes::addTake (track, recorded.getChildFile ("c.wav"), secs (2, 6), {});
        Takes::addTake (track, recorded.getChildFile ("d.wav"), secs (10, 12), {});

        int numLanes = 0;
        auto lanes = Takes::packLanes (Takes::getTakes (track), numLanes);
        log.check (Takes::getNumTakes (track) == 4, "4 takes");
        log.check (numLanes == 3, "overlapping takes stack in 3 lanes (" + juce::String (numLanes) + ")");
        log.check (lanes.size() == 4 && lanes[3] == 0, "a take elsewhere shares the first lane");
        log.check (takeNumbered (track, 3).file == recorded.getChildFile ("c.wav"), "takes are numbered and find their files");

        um.beginNewTransaction();
        Takes::useWholeTake (track, takeNumbered (track, 3));
        log.check (describe (track) == "2-6:c", "whole take 3 on the track (" + describe (track) + ")");

        um.beginNewTransaction();
        Takes::comp (track, takeNumbered (track, 1), secs (3, 4));
        log.check (describe (track) == "2-3:c 3-4:a 4-6:c", "swipe 3-4 s from take 1 (" + describe (track) + ")");
        log.check (Takes::getUsedRanges (track, takeNumbered (track, 3)).size() == 2, "take 3 is used in two places");
        log.check (Takes::getUsedRanges (track, takeNumbered (track, 1)).size() == 1, "take 1 is used once");

        // The comped piece plays the right part of its file
        for (auto c : track.getClips())
            if (auto w = dynamic_cast<te::WaveAudioClip*> (c); w != nullptr && w->getOriginalFile().getFileName() == "a.wav")
                log.check (std::abs (w->getPosition().getOffset().inSeconds() - 1.0) < 0.001, "the take 1 piece starts 1 s into its file");

        um.beginNewTransaction();
        Takes::comp (track, takeNumbered (track, 2), secs (1, 3.5));
        log.check (describe (track) == "2-3.5:b 3.5-4:a 4-6:c", "a swipe past the take's start is cut to the take (" + describe (track) + ")");

        auto unused = Takes::getUnusedTakes (track);
        log.check (unused.size() == 1 && unused[0].number == 4, "take 4 is the only unused one");

        um.undo();
        log.check (describe (track) == "2-3:c 3-4:a 4-6:c", "undo brings back the comp before (" + describe (track) + ")");

        // A loop recording: the engine hands back one clip holding a take per pass
        um.beginNewTransaction();
        auto loopClip = track.insertWaveClip ("Loop", recorded.getChildFile ("l1.wav"), { secs (14, 18), {} }, false);

        for (auto f : { "l1.wav", "l2.wav", "l3.wav" })
            loopClip->addTake (recorded.getChildFile (f));

        auto made = Takes::absorbRecording (track, *loopClip);
        log.check (made == 3, "a 3-pass loop recording makes 3 takes (" + juce::String (made) + ")");
        log.check (describe (track).endsWith ("14-18:l3"), "the newest pass goes on the track (" + describe (track) + ")");
        log.check (Takes::getNumTakes (track) == 7 && takeNumbered (track, 7).file.getFileName() == "l3.wav", "passes are numbered on from 5");

        p.save();
    }

    {
        Project p (engine, editFile);
        auto& track = *te::getAudioTracks (p.getEdit()).getFirst();
        auto take = takeNumbered (track, 2);
        log.check (Takes::getNumTakes (track) == 7, "takes are saved with the project");
        log.check (take.state[Takes::sourceProp].toString() == "Samples/Recorded/b.wav",
                   "take sources are relative to the project (" + take.state[Takes::sourceProp].toString() + ")");
        log.check (take.file.existsAsFile(), "and found again on reopening");
    }

    std::cout << "Takes tests\n" << log.text << (log.failures == 0 ? "All passed\n" : juce::String (log.failures) + " failed\n");
    return log.failures;
}

} // namespace elsewhere::TakesTests
