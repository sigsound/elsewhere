// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ProjectAudioTests.h"
#include "ProjectAudio.h"
#include "devices/DeviceBase.h"
#include "SomewhereBeat.h"

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

    void writeMidi (const juce::File& f, int channel, std::initializer_list<std::pair<int, int>> notes)
    {
        juce::MidiMessageSequence seq;
        int tick = 0;

        for (auto [note, velocity] : notes)
        {
            seq.addEvent (juce::MidiMessage::noteOn (channel, note, (juce::uint8) velocity), tick);
            seq.addEvent (juce::MidiMessage::noteOff (channel, note), tick + 12);
            tick += 24;
        }

        seq.updateMatchedPairs();
        juce::MidiFile mf;
        mf.setTicksPerQuarterNote (96);
        mf.addTrack (seq);
        f.deleteFile();
        juce::FileOutputStream out (f);
        mf.writeTo (out);
    }

    void writeSound (const juce::File& senderFolder, const juce::String& name, int pad)
    {
        writeSine (senderFolder.getChildFile (name + ".wav"));
        auto* o = new juce::DynamicObject();
        o->setProperty ("name", name);
        if (pad > 0) o->setProperty ("pad", pad);
        senderFolder.getChildFile (name + ".somewhere.json").replaceWithText (juce::JSON::toString (juce::var (o)));
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
            log.check (History::read (oldFile).size() >= 1 && History::fileFor (oldFile).getFileName() == "." + oldFile.getFileNameWithoutExtension() + ".history",
                       "its versions are kept in the history beside it");
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

    // A beat from Somewhere: its drums and keys as MIDI, playing the phone's own sounds
    {
        auto sender = outDir.getChildFile ("Inbox/Nick (a1b2)");
        auto folder = sender.getChildFile ("Beats/2026-11-26 10.00.00 Beat P1 96bpm");
        writeSine (folder.getChildFile ("01 Kick.wav"));
        writeSine (folder.getChildFile ("02 Snare.wav"));
        writeSine (folder.getChildFile ("17 Teclas Piano.wav"));
        writeMidi (folder.getChildFile ("Beat.mid"), 10, { { 36, 100 }, { 37, 56 } });
        writeMidi (folder.getChildFile ("Keys.mid"), 1, { { 60, 80 }, { 64, 80 } });
        folder.getChildFile (SomewhereBeat::infoFileName)
              .replaceWithText (R"j({"app":"Somewhere","format":1,"bpm":96,"swing":0,"patterns":[1],"steps":16,"sender":"Nick (a1b2)","keysSound":"Piano"})j");
        writeSound (sender, "Kick", 1);
        writeSound (sender, "Piano", 0);

        log.check (SomewhereBeat::findBeatFolder ({ folder }) == folder, "a beat folder is recognised");
        log.check (SomewhereBeat::findBeatFolder ({ folder.getChildFile ("01 Kick.wav"), folder.getChildFile ("Beat.mid") }) == folder,
                   "so are the files inside it");
        log.check (SomewhereBeat::findBeatFolder ({ take }) == juce::File(), "other files aren't");

        auto beat = SomewhereBeat::read (folder);
        log.check (beat.has_value() && beat->name == "Beat P1 96bpm", "beat name from the folder");
        log.check (beat.has_value() && beat->stems.size() == 3 && beat->stems[1].pad == 1 && beat->stems[2].pad == -1
                   && beat->stems[2].sound == "Piano", "stems: pads in order, the keys last whatever the language");

        auto build = [&] (Project& p)
        {
            juce::Array<juce::File> stems;
            for (auto& s : beat->stems) stems.add (p.importAudioFile (s.file));
            return SomewhereBeat::build (p.getEdit(), *beat, stems, {}, nullptr, [&p] (const juce::File& f) { return p.importAudioFile (f); });
        };

        if (beat)
        {
            auto beatFile = Project::createProjectFolder (outDir.getChildFile ("Beats"), "Beat Song");
            Project p (engine, beatFile);
            auto r = build (p);

            log.check (juce::approximatelyEqual (p.getEdit().tempoSequence.getTempo (0)->getBpm(), 96.0), "an empty song takes the beat's tempo");
            log.check (r.drums != nullptr && DrumTrack::isDrumTrack (*r.drums), "a drum track");
            log.check (r.missing.contains ("Snare") && ! r.drumsPlay, "a missing pad sound is reported");
            log.check (r.drums != nullptr && r.drums->isMuted (false) && r.stems.size() == 3
                       && ! r.stems[0]->isMuted (false) && ! r.stems[1]->isMuted (false), "then the drum stems play and the drum track is muted");

            auto* drums = r.drums != nullptr ? r.drums->pluginList.findFirstPluginOfType<DrumsDevice>() : nullptr;
            log.check (drums != nullptr && drums->padHasUserSample (0) && drums->getPadDisplayName (0) == "Kick" && ! drums->padHasUserSample (1),
                       "the kick pad has the phone's kick");

            int notes = 0, soft = 0;
            if (r.drums != nullptr)
                for (auto c : r.drums->getClips())
                    if (auto* mc = dynamic_cast<te::MidiClip*> (c))
                        for (auto n : mc->getSequence().getNotes())
                            ++notes, soft += n->getVelocity() < 90 ? 1 : 0;
            log.check (notes == 2 && soft == 1, "Beat.mid is on the drum track, soft and hard steps kept");

            auto* sampler = r.keys != nullptr ? r.keys->pluginList.findFirstPluginOfType<te::SamplerPlugin>() : nullptr;
            if (sampler != nullptr) sampler->flushPendingUpdates();
            log.check (sampler != nullptr && sampler->getNumSounds() == 1 && sampler->getKeyNote (0) == 60
                       && sampler->getSoundFile (0).getFile().isAChildOf (p.getFolder()), "the keys play the phone's sound, from C4, copied into the project");
            log.check (r.keysPlay && r.keys != nullptr && ! r.keys->isMuted (false) && r.stems[2]->isMuted (false), "the keys play as MIDI and their stem is muted");
        }

        writeSound (sender, "Snare", 2);

        if (beat)
        {
            auto beatFile = Project::createProjectFolder (outDir.getChildFile ("Beats"), "Beat Song 2");
            Project p (engine, beatFile);
            p.getEdit().tempoSequence.getTempo (0)->setBpm (120.0);
            auto first = te::getAudioTracks (p.getEdit()).getFirst();
            first->insertWaveClip ("Other", take, te::ClipPosition { te::TimeRange (te::TimePosition(), te::TimePosition::fromSeconds (1.0)), {} }, false);
            auto r = build (p);

            log.check (juce::approximatelyEqual (p.getEdit().tempoSequence.getTempo (0)->getBpm(), 120.0), "a song with clips keeps its tempo");
            log.check (r.missing.isEmpty() && r.drumsPlay && ! r.drums->isMuted (false) && r.stems[0]->isMuted (false) && r.stems[1]->isMuted (false),
                       "with every sound there, the drum track plays and its stems are muted");
        }

        // A self-contained beat: its sounds in its own Sounds folder, kit sounds included
        {
            auto own = outDir.getChildFile ("Inbox/Mia (c3d4)/Beats/2026-11-26 11.00.00 Beat P2 100bpm");
            writeSine (own.getChildFile ("01 Lo-Fi Kick.wav"));
            writeSine (own.getChildFile ("03 Choir.wav"));
            writeMidi (own.getChildFile ("Beat.mid"), 10, { { 36, 100 } });
            writeMidi (own.getChildFile ("Keys.mid"), 1, { { 60, 80 } });
            writeSine (own.getChildFile ("Sounds/01 Lo-Fi Kick.wav"));
            writeSine (own.getChildFile ("Sounds/03 Choir_.wav"));     // a safe name that differs from the sound's
            own.getChildFile (SomewhereBeat::infoFileName)
               .replaceWithText (R"j({"app":"Somewhere","format":1,"bpm":100,"swing":0,"patterns":[2],"steps":16,"sender":"Mia (c3d4)","keysSound":"Choir","keysPad":3,
                                   "sounds":[{"pad":1,"name":"Lo-Fi Kick","file":"Sounds/01 Lo-Fi Kick.wav"},{"pad":3,"name":"Choir","file":"Sounds/03 Choir_.wav"},
                                             {"pad":2,"name":"Escape","file":"../../../Kick.wav"}]})j");
            auto b2 = SomewhereBeat::read (own);
            log.check (b2.has_value() && b2->keysPad == 2, "keysPad read (1-based)");
            log.check (b2.has_value() && b2->sounds.size() == 2 && b2->sounds[2].getFileName() == "03 Choir_.wav", "\"sounds\" read, files kept inside the beat folder");

            if (b2)
            {
                auto f = Project::createProjectFolder (outDir.getChildFile ("Beats"), "Beat Song 3");
                Project p (engine, f);
                juce::Array<juce::File> stems;
                for (auto& s : b2->stems) stems.add (p.importAudioFile (s.file));
                auto r = SomewhereBeat::build (p.getEdit(), *b2, stems, {}, nullptr, [&p] (const juce::File& x) { return p.importAudioFile (x); });
                log.check (r.missing.isEmpty() && r.drumsPlay && r.keysPlay, "a beat's own Sounds folder is enough, with nothing else sent");
            }
        }

        // Two samples with the same name: the newest
        writeSound (sender, "Twin", 0);
        sender.getChildFile ("Twin.somewhere.json").replaceWithText (R"j({"name":"Kick","recordedAt":"2026-11-27T09:00:00Z"})j");
        sender.getChildFile ("Twin.wav").setLastModificationTime (juce::Time::getCurrentTime());
        sender.getChildFile ("Kick.somewhere.json").replaceWithText (R"j({"name":"Kick","pad":1,"recordedAt":"2026-11-20T09:00:00Z"})j");
        log.check (SomewhereBeat::findSound (sender, "Kick", 0).getFileName() == "Twin.wav", "the newest of two same-named sounds");
    }

    log.text = "Project audio: " + juce::String (log.failures) + " failure(s)\n" + log.text;
    outDir.getChildFile ("project-audio.txt").replaceWithText (log.text);
    std::cout << log.text;
    return log.failures;
}

} // namespace elsewhere::ProjectAudioTests
