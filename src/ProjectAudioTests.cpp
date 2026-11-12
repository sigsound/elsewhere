// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ProjectAudioTests.h"
#include "ProjectAudio.h"
#include "devices/DeviceBase.h"

namespace elsewhere::ProjectAudioTests
{
namespace
{
    struct Log
    {
        int failures = 0;
        juce::String text;
        void check (bool ok, const juce::String& what)  { text << (ok ? "  PASS  " : "  FAIL  ") << what << "\n"; if (! ok) ++failures; }
    };

    void writeSine (const juce::File& f)
    {
        f.getParentDirectory().createDirectory();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream().release());
        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (out.get(), 44100.0, 1, 16, {}, 0));

        if (w == nullptr)
            return;

        out.release();
        juce::AudioBuffer<float> b (1, 44100);

        for (int i = 0; i < b.getNumSamples(); ++i)
            b.setSample (0, i, 0.5f * std::sin ((float) i * 0.06f));

        w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    juce::String firstSource (Project& p)
    {
        auto nodes = ProjectAudio::findSourceNodes (p.getEdit().state);
        return nodes.isEmpty() ? juce::String() : nodes.getFirst()[te::IDs::source].toString();
    }

    bool clipPlays (Project& p)
    {
        for (auto t : te::getAudioTracks (p.getEdit()))
            for (auto c : t->getClips())
                if (auto w = dynamic_cast<te::WaveAudioClip*> (c))
                    return w->getAudioFile().getFile().existsAsFile();

        return false;
    }
}

int run (te::Engine& engine, const juce::File& outDir)
{
    Log log;
    outDir.deleteRecursively();
    outDir.createDirectory();

    // A take recorded into another song's folder (how the ambiant project lost its Solar part)
    auto take = outDir.getChildFile ("Drive/Songs/Other Song Project/Samples/Recorded/Solar_Take_4.wav");
    writeSine (take);

    auto editFile = Project::createProjectFolder (outDir.getChildFile ("Drive/Songs"), "Song");
    juce::File copyFile;

    {
        Project p (engine, editFile);
        auto& edit = p.getEdit();
        auto track = te::getAudioTracks (edit).getFirst();
        track->insertWaveClip ("Solar", take, te::ClipPosition { te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (1.0)), {} }, false);
        p.save();
        log.check (firstSource (p).startsWith ("../"), "outside recording is stored as ../ (" + firstSource (p) + ")");

        auto copied = ProjectAudio::collectExternal (p);
        log.check (copied == 1, "collect copies 1 file (" + juce::String (copied) + ")");
        log.check (firstSource (p) == "Samples/Imported/Solar_Take_4.wav", "clip now points inside the project (" + firstSource (p) + ")");
        log.check (p.getFolder().getChildFile ("Samples/Imported/Solar_Take_4.wav").existsAsFile(), "copy is in Samples/Imported");
        log.check (clipPlays (p), "clip plays from the copy");
        log.check (ProjectAudio::collectExternal (p) == 0, "collecting again copies nothing");

        // A Somewhere take, which Save As used to leave behind
        writeSine (p.getFolder().getChildFile ("Samples/Somewhere/Nick (e8c4)/Takes/Take 1.wav"));

        // Save As from inside "Song Project/" shouldn't nest another one in it
        auto target = outDir.getChildFile ("iCloud/Song Project");
        target.createDirectory();
        auto copy = p.saveCopyAs (target, "Song");
        log.check (copy.has_value(), "save as");

        if (copy)
        {
            copyFile = *copy;
            log.check (copy->getParentDirectory().getParentDirectory() == outDir.getChildFile ("iCloud"), "save as doesn't nest (" + copy->getFullPathName() + ")");
            log.check (copy->getParentDirectory().getChildFile ("Samples/Somewhere/Nick (e8c4)/Takes/Take 1.wav").existsAsFile(), "save as copies Somewhere takes");
        }
    }

    // The original folders go away (e.g. a drive that's no longer there)
    outDir.getChildFile ("Drive").deleteRecursively();

    if (copyFile.existsAsFile())
    {
        {
            Project p (engine, copyFile);
            log.check (clipPlays (p), "the copy plays after the originals are gone");
            log.check (ProjectAudio::findMissing (p).isEmpty(), "nothing missing");
        }

        // A file goes missing: moved somewhere else entirely
        auto inProject = copyFile.getParentDirectory().getChildFile ("Samples/Imported/Solar_Take_4.wav");
        auto movedFile = outDir.getChildFile ("Found/deep/folder/Solar_Take_4.wav");
        movedFile.getParentDirectory().createDirectory();
        inProject.moveFileTo (movedFile);

        {
            Project p (engine, copyFile);
            auto missing = ProjectAudio::findMissing (p);
            log.check (missing.size() == 1 && missing[0] == "Solar_Take_4.wav", "missing file reported (" + missing.joinIntoString (", ") + ")");
            log.check (ProjectAudio::relinkFrom (p, p.getFolder()) == 0, "not in the project folder");
            log.check (ProjectAudio::relinkFrom (p, outDir.getChildFile ("Found")) == 1, "found by searching a folder");
            log.check (ProjectAudio::findMissing (p).isEmpty(), "nothing missing after relinking");
            log.check (inProject.existsAsFile(), "found file copied back into the project");
            log.check (clipPlays (p), "clip plays again");
        }

        // Moved within the project (the ambiant case after copying the take in): found in place
        auto edit = copyFile.loadFileAsString().replace ("Samples/Imported/Solar_Take_4.wav", "../../../../../Gone/Samples/Recorded/Solar_Take_4.wav");
        copyFile.replaceWithText (edit);

        {
            Project p (engine, copyFile);
            log.check (ProjectAudio::findMissing (p).size() == 1, "a broken ../ path is missing");
            log.check (ProjectAudio::relinkFrom (p, p.getFolder()) == 1, "found inside the project folder");
            log.check (firstSource (p) == "Samples/Imported/Solar_Take_4.wav", "relinked relative to the project (" + firstSource (p) + ")");
        }
    }

    // A project saved while Elsewhere was called Tandem opens with its devices and settings
    {
        auto newFile = Project::createProjectFolder (outDir.getChildFile ("Legacy"), "Old Song");

        {
            Project p (engine, newFile);
            auto track = te::getAudioTracks (p.getEdit()).getFirst();

            if (auto reverb = DeviceRegistry::create (p.getEdit(), "elsewhereReverb"))
                track->pluginList.insertPlugin (reverb, 0, nullptr);

            p.savedByName = "Nick";
            p.save();
        }

        // Write it back the way Tandem saved it
        auto oldFile = newFile.withFileExtension (Legacy::projectExtension);
        oldFile.replaceWithText (newFile.loadFileAsString()
                                   .replace (" elsewhere", " tandem").replace ("\"elsewhere", "\"tandem")          // properties, device types
                                   .replace ("<ELSEWHERE_", "<TANDEM_").replace ("</ELSEWHERE_", "</TANDEM_"));    // node types
        newFile.deleteFile();
        log.check (oldFile.loadFileAsString().contains ("type=\"tandemReverb\""), "old file names its device tandemReverb");

        {
            Project p (engine, oldFile);
            bool hasReverb = false;

            for (auto t : te::getAudioTracks (p.getEdit()))
                for (auto plugin : t->pluginList)
                    hasReverb = hasReverb || plugin->getPluginType() == "elsewhereReverb";

            log.check (hasReverb, "Tandem Reverb opens as Elsewhere Reverb");
            log.check (p.getEdit().state["elsewhereSavedBy"].toString() == "Nick", "saved-by survives the rename");
            log.check (! p.getEdit().state.hasProperty ("tandemSavedBy"), "old property name is gone");

            p.save();
            log.check (oldFile.existsAsFile() && ! newFile.existsAsFile(), "an old project keeps its .tandem file");
            log.check (p.getFolder().getChildFile ("Backups").findChildFiles (juce::File::findFiles, false, "*.tandem").size() == 1,
                       "its backups are .tandem too");
        }

        auto music = outDir.getChildFile ("Music");
        auto legacyRoot = music.getChildFile ("Tandem Projects");
        legacyRoot.createDirectory();
        log.check (Legacy::preferExisting (music.getChildFile ("Elsewhere Projects"), legacyRoot) == legacyRoot,
                   "projects stay in Tandem Projects while that's all there is");
        music.getChildFile ("Elsewhere Projects").createDirectory();
        log.check (Legacy::preferExisting (music.getChildFile ("Elsewhere Projects"), legacyRoot) == music.getChildFile ("Elsewhere Projects"),
                   "Elsewhere Projects wins once it exists");
    }

    log.text = "Project audio: " + juce::String (log.failures) + " failure(s)\n" + log.text;
    outDir.getChildFile ("project-audio.txt").replaceWithText (log.text);
    std::cout << log.text;
    return log.failures;
}

} // namespace elsewhere::ProjectAudioTests
