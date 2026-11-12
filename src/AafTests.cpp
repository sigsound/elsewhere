// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "AafTests.h"
#include "Aaf.h"
#include "Project.h"

namespace elsewhere::AafTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
        void info (const juce::String& what)            { text << "        " << what << "\n"; }

        void near (double actual, double expected, const juce::String& what, double tolerance = 1.0e-3)
        {
            check (std::abs (actual - expected) <= tolerance, what + " = " + juce::String (actual, 4) + " (expected " + juce::String (expected, 4) + ")");
        }
    };

    te::AudioTrack* findTrack (te::Edit& edit, const juce::String& name)
    {
        for (auto t : te::getAudioTracks (edit))
            if (t->getName() == name)
                return t;
        return nullptr;
    }

    juce::Array<te::WaveAudioClip*> waveClips (te::AudioTrack* t)
    {
        juce::Array<te::WaveAudioClip*> clips;
        if (t != nullptr)
            for (auto* c : t->getClips())
                if (auto* w = dynamic_cast<te::WaveAudioClip*> (c))
                    clips.add (w);
        return clips;
    }

    void checkClip (Log& log, te::WaveAudioClip* c, double start, double end, double offset, const juce::String& what)
    {
        log.check (c != nullptr, what + " exists");

        if (c == nullptr)
            return;

        auto pos = c->getPosition();
        log.near (pos.getStart().inSeconds(), start, what + " start");
        log.near (pos.getEnd().inSeconds(), end, what + " end");
        log.near (pos.getOffset().inSeconds(), offset, what + " offset");
    }

    /** The first second of two files' samples match. */
    bool sameAudio (const juce::File& a, const juce::File& b)
    {
        juce::AudioFormatManager fm;
        fm.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> ra (fm.createReaderFor (a)), rb (fm.createReaderFor (b));

        if (ra == nullptr || rb == nullptr || ra->numChannels != rb->numChannels || ra->lengthInSamples != rb->lengthInSamples)
            return false;

        juce::AudioBuffer<float> ba ((int) ra->numChannels, 48000), bb ((int) rb->numChannels, 48000);
        ra->read (&ba, 0, 48000, 0, true, true);
        rb->read (&bb, 0, 48000, 0, true, true);

        for (int ch = 0; ch < ba.getNumChannels(); ++ch)
            for (int i = 0; i < 48000; ++i)
                if (std::abs (ba.getSample (ch, i) - bb.getSample (ch, i)) > 1.0e-6f)
                    return false;

        return true;
    }
}

int run (te::Engine& engine, const juce::File& fixtureDir)
{
    Log log;
    auto aafFile = fixtureDir.getChildFile ("pt-style.aaf");
    log.check (aafFile.existsAsFile(), "fixture " + aafFile.getFullPathName() + " (make it with scripts/make-aaf-fixture.py)");

    auto outDir = fixtureDir.getChildFile ("import");
    outDir.deleteRecursively();
    auto editFile = Project::createProjectFolder (outDir, "Imported");

    Aaf::Importer importer (aafFile);
    log.check (importer.getError().isEmpty(), "opened the AAF " + importer.getError());
    log.check (importer.extractAudio (editFile.getParentDirectory().getChildFile ("Samples/Imported")), "copied the audio");

    Project project (engine, editFile);
    auto& edit = project.getEdit();
    auto summary = importer.build (edit);
    log.info (summary.describe().replace ("\n", "\n        "));
    log.check (summary.missingFiles.isEmpty(), "found every file, including the moved external ones");
    log.check (summary.applicationName.isNotEmpty(), "read the application name: " + summary.applicationName);
    log.check (te::getAudioTracks (edit).size() == 4, "4 audio tracks (picture and timecode left out): " + juce::String (te::getAudioTracks (edit).size()));

    auto samples = editFile.getParentDirectory().getChildFile ("Samples/Imported");
    log.check (sameAudio (samples.getChildFile ("Kick.wav"), fixtureDir.getChildFile ("kick.wav")), "embedded raw PCM (16-bit mono) decodes exactly");
    log.check (sameAudio (samples.getChildFile ("Vox.wav"), fixtureDir.getChildFile ("vox.wav")), "embedded raw PCM (24-bit stereo) decodes exactly");
    log.check (sameAudio (samples.getChildFile ("Gtr.L.wav"), fixtureDir.getChildFile ("Audio Files/Gtr.L.wav")), "external file copied in");

    auto kick = waveClips (findTrack (edit, "Kick"));
    log.check (kick.size() == 2, "Kick has 2 clips: " + juce::String (kick.size()));
    checkClip (log, kick[0], 1.0, 2.0, 0.0, "Kick clip 1");
    checkClip (log, kick[1], 2.5, 3.0, 0.5, "Kick clip 2");
    if (kick.size() == 2)
    {
        log.near (kick[0]->getGainDB(), 0.0, "Kick clip 1 gain");
        log.near (kick[1]->getGainDB(), -6.0206, "Kick clip 2 gain (-6 dB)", 0.01);
    }

    // 25 fps: 1 s gap, 2 s long, from 12 frames (0.48 s) in
    for (auto [name, pan] : { std::pair<const char*, float> { "Gtr L", -1.0f }, { "Gtr R", 1.0f } })
    {
        auto* t = findTrack (edit, name);
        log.check (t != nullptr, juce::String (name) + " track (stereo pair named by side)");

        if (t == nullptr)
            continue;

        auto clips = waveClips (t);
        log.check (clips.size() == 1, juce::String (name) + " has 1 clip");
        checkClip (log, clips[0], 1.0, 3.0, 0.48, name);
        log.near (t->getVolumePlugin()->getPan(), pan, juce::String (name) + " pan");
    }

    auto* voxTrack = findTrack (edit, "Vox");
    auto vox = waveClips (voxTrack);
    log.check (vox.size() == 2, "Vox has 2 clips: " + juce::String (vox.size()));
    checkClip (log, vox[0], 0.0, 2.0, 0.0, "Vox clip 1");
    checkClip (log, vox[1], 1.8, 3.3, 2.5, "Vox clip 2 (overlaps clip 1 by the crossfade)");

    if (vox.size() == 2)
    {
        log.near (vox[0]->getFadeOut().inSeconds(), 0.2, "crossfade out");
        log.near (vox[1]->getFadeIn().inSeconds(), 0.2, "crossfade in");
        log.near (vox[1]->getFadeOut().inSeconds(), 0.1, "fade out into silence");
    }

    if (voxTrack != nullptr)
        log.near (voxTrack->getVolumePlugin()->getVolumeDb(), 6.0, "Vox track volume (+6 dB, the fader's top)", 0.01);

    log.text << "\n" << (log.failures == 0 ? "All passed" : juce::String (log.failures) + " failed") << "\n";
    fixtureDir.getChildFile ("aaf-test.txt").replaceWithText (log.text);
    std::cout << log.text;
    return log.failures;
}

} // namespace elsewhere::AafTests
