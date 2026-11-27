// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ProjectAudioTests.h"
#include "ProjectAudio.h"
#include "devices/DeviceBase.h"
#include "SomewhereBeat.h"
#include "devices/HazeDevice.h"
#include "devices/MonoDevice.h"
#include "devices/PolyDevice.h"

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

    //==============================================================================
    // A beat as the phone renders it, for checking that its MIDI sounds the same as its stems
    namespace beatRender
    {
        inline double rate = 48000.0;      // set to the device's rate, so nothing is resampled
        constexpr double bpm = 100.0;
        constexpr int steps = 32;

        int sampleAt (double beat)      { return (int) std::llround (beat * 60.0 / bpm * rate); }

        void writeWav (const juce::File& f, const juce::AudioBuffer<float>& b)
        {
            f.getParentDirectory().createDirectory();
            f.deleteFile();
            juce::WavAudioFormat wav;
            auto out = f.createOutputStream();
            std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (out.get(), rate, (unsigned int) b.getNumChannels(), 24, {}, 0));

            if (w == nullptr)
                return;

            out.release();
            w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
        }

        juce::AudioBuffer<float> readWav (const juce::File& f)
        {
            juce::AudioFormatManager m;
            m.registerBasicFormats();
            juce::AudioBuffer<float> b;

            if (std::unique_ptr<juce::AudioFormatReader> r { m.createReaderFor (f) })
            {
                b.setSize ((int) r->numChannels, (int) r->lengthInSamples);
                r->read (&b, 0, (int) r->lengthInSamples, 0, true, true);
            }

            return b;
        }

        // A kick (decaying low sine) or a hat (a burst of noise)
        juce::AudioBuffer<float> oneShot (bool kick)
        {
            juce::AudioBuffer<float> b (2, (int) (rate * (kick ? 0.25 : 0.08)));
            juce::Random rnd (7);

            for (int i = 0; i < b.getNumSamples(); ++i)
            {
                auto t = i / rate;
                auto v = kick ? 0.6f * (float) (std::sin (juce::MathConstants<double>::twoPi * 55.0 * t) * std::exp (-t / 0.07))
                              : 0.3f * (rnd.nextFloat() * 2.0f - 1.0f) * (float) std::exp (-t / 0.02);
                b.setSample (0, i, v);
                b.setSample (1, i, v);
            }

            return b;
        }

        // The phone plays a hit at its velocity / 127, nothing else
        juce::AudioBuffer<float> stem (const juce::AudioBuffer<float>& one, const std::vector<std::pair<double, int>>& hits)
        {
            juce::AudioBuffer<float> b (2, sampleAt (steps / 4.0) + (int) rate);
            b.clear();

            for (auto [beat, vel] : hits)
                for (int c = 0; c < 2; ++c)
                    b.addFrom (c, sampleAt (beat), one, c, 0, juce::jmin (one.getNumSamples(), b.getNumSamples() - sampleAt (beat)), (float) vel / 127.0f);

            return b;
        }

        juce::AudioBuffer<float> render (te::Edit& edit, juce::Array<te::Track*> tracks, const juce::File& f)
        {
            f.deleteFile();
            te::Renderer::Parameters params (edit);
            params.destFile = f;
            params.audioFormat = edit.engine.getAudioFileFormatManager().getWavFormat();
            params.bitDepth = 32;
            params.sampleRateForAudio = rate;
            params.time = { te::TimePosition(), edit.tempoSequence.toTime (te::BeatPosition::fromBeats (steps / 4.0 + 1.0)) };
            params.usePlugins = true;
            params.tracksToDo = te::toBitSet (tracks);
            te::Renderer::renderToFile ("test", params);
            return readWav (f);
        }

        double rmsDb (const juce::AudioBuffer<float>& b, int from, int n)
        {
            double sum = 0;
            n = juce::jmin (n, b.getNumSamples() - from);
            for (int i = from; i < from + n; ++i)
                sum += juce::square ((double) b.getSample (0, i));
            return juce::Decibels::gainToDecibels (std::sqrt (sum / juce::jmax (1, n)), -120.0);
        }

        double correlation (const juce::AudioBuffer<float>& a, const juce::AudioBuffer<float>& b, int from, int n)
        {
            double ab = 0, aa = 0, bb = 0;
            n = juce::jmin (n, juce::jmin (a.getNumSamples(), b.getNumSamples()) - from);
            for (int i = from; i < from + n; ++i)
            {
                double x = a.getSample (0, i), y = b.getSample (0, i);
                ab += x * y; aa += x * x; bb += y * y;
            }
            return aa > 0 && bb > 0 ? ab / std::sqrt (aa * bb) : 0.0;
        }
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
            if (r.drums != nullptr && ! r.drums->getClips().isEmpty())
                log.check (std::abs (p.getEdit().tempoSequence.toBeats (r.drums->getClips().getFirst()->getPosition().getEnd()).inBeats() - 4.0) < 0.01,
                           "the MIDI clip lasts the whole beat (16 steps), not just to its last note");

            auto* sampler = r.keys != nullptr ? r.keys->pluginList.findFirstPluginOfType<te::SamplerPlugin>() : nullptr;
            if (sampler != nullptr) sampler->flushPendingUpdates();
            log.check (sampler != nullptr && sampler->getNumSounds() == 1 && sampler->getKeyNote (0) == 60
                       && sampler->getSoundFile (0).getFile().isAChildOf (p.getFolder()), "the keys play the phone's sound, from C4, copied into the project");
            log.check (r.keysPlay && r.keys != nullptr && ! r.keys->isMuted (false) && r.stems[2]->isMuted (false), "the keys play as MIDI and their stem is muted");
        }

        // Keys in two sounds (pads 3 and 5): a track per sound from its own file, else the stem
        {
            auto mixed = sender.getChildFile ("Beats/2026-11-26 11.00.00 Beat P2 100bpm");
            writeSine (mixed.getChildFile ("17 Keys Piano.wav"));
            writeMidi (mixed.getChildFile ("Keys.mid"), 1, { { 60, 80 }, { 72, 80 } });
            writeSine (mixed.getChildFile ("Sounds/03 Piano.wav"));
            writeSine (mixed.getChildFile ("Sounds/05 Marimba.wav"));
            mixed.getChildFile (SomewhereBeat::infoFileName)
                 .replaceWithText (R"j({"app":"Somewhere","format":1,"bpm":100,"swing":0,"patterns":[1],"steps":16,"sender":"Nick (a1b2)",
                                       "keysSound":"Piano","keysPad":3,"keysPads":[3,5],
                                       "sounds":[{"pad":3,"name":"Piano","file":"Sounds/03 Piano.wav"},{"pad":5,"name":"Marimba","file":"Sounds/05 Marimba.wav"}]})j");

            auto importMixed = [&] (const juce::String& song)
            {
                auto b = SomewhereBeat::read (mixed);
                auto file = Project::createProjectFolder (outDir.getChildFile ("Beats"), song);
                auto p = std::make_unique<Project> (engine, file);
                juce::Array<juce::File> stems;
                for (auto& s : b->stems) stems.add (p->importAudioFile (s.file));
                auto r = SomewhereBeat::build (p->getEdit(), *b, stems, {}, nullptr, [&p] (const juce::File& f) { return p->importAudioFile (f); });
                return std::make_pair (std::move (p), r);
            };

            {
                auto [p, r] = importMixed ("Mixed Keys");
                log.check (r.mixedKeys && ! r.keysPlay && r.keys != nullptr && r.keys->isMuted (false)
                           && ! r.stems.isEmpty() && ! r.stems.getLast()->isMuted (false), "keys in two sounds with one file: the stem plays, the Keys track is muted");
            }

            writeMidi (mixed.getChildFile ("Keys 03.mid"), 1, { { 60, 80 } });
            writeMidi (mixed.getChildFile ("Keys 05.mid"), 1, { { 72, 80 } });

            {
                auto [p, r] = importMixed ("Split Keys");
                juce::StringArray sounds;
                for (auto* t : r.keysBySound)
                    if (auto* sm = t->pluginList.findFirstPluginOfType<te::SamplerPlugin>())
                    {
                        sm->flushPendingUpdates();
                        sounds.add (sm->getSoundFile (0).getFile().getFileNameWithoutExtension());
                    }
                log.check (r.keysBySound.size() == 2 && r.keysPlay && ! r.mixedKeys && r.stems.getLast()->isMuted (false)
                           && sounds.joinIntoString ("|").contains ("Piano") && sounds.joinIntoString ("|").contains ("Marimba")
                           && ! r.keysBySound[0]->isMuted (false) && ! r.keysBySound[1]->isMuted (false),
                           "with a file per sound: a Keys track for each sound (" + sounds.joinIntoString (", ") + "), the stem muted");
                log.check (r.keysBySound.size() == 2 && r.keysBySound[1]->getName().contains ("Marimba"), "each named for its sound");
            }
        }

        // Keys played by Haze: alone, and next to a sampled sound
        {
            auto importBeat = [&] (const juce::File& folder, const juce::String& song)
            {
                auto b = SomewhereBeat::read (folder);
                auto file = Project::createProjectFolder (outDir.getChildFile ("Beats"), song);
                auto p = std::make_unique<Project> (engine, file);
                juce::Array<juce::File> stems;
                for (auto& s : b->stems) stems.add (p->importAudioFile (s.file));
                auto r = SomewhereBeat::build (p->getEdit(), *b, stems, {}, nullptr, [&p] (const juce::File& f) { return p->importAudioFile (f); });
                return std::make_pair (std::move (p), r);
            };
            auto hazeOn = [] (te::AudioTrack* t) { return t != nullptr ? t->pluginList.findFirstPluginOfType<HazeDevice>() : nullptr; };

            auto alone = sender.getChildFile ("Beats/2026-11-27 09.00.00 Beat P3 90bpm");
            writeSine (alone.getChildFile ("17 Keys Haze Marine Layer.wav"));
            writeMidi (alone.getChildFile ("Keys.mid"), 1, { { 60, 80 } });
            alone.getChildFile (SomewhereBeat::infoFileName)
                 .replaceWithText (R"j({"app":"Somewhere","format":1,"bpm":90,"swing":0,"patterns":[1],"steps":16,"sender":"Nick (a1b2)",
                                       "keysSound":"Haze Marine Layer","keysPad":17,"keysSynth":{"pad":17,"synth":"Haze","patch":5,"patchName":"Marine Layer"},"sounds":[]})j");
            {
                auto [p, r] = importBeat (alone, "Haze Alone");
                auto* haze = hazeOn (r.keys);
                log.check (haze != nullptr && haze->getPatchIndex() == 5 && ! haze->isPatchModified() && r.keysPlay && r.missing.isEmpty()
                           && ! r.keys->isMuted (false) && r.stems.getLast()->isMuted (false), "keys played by Haze: an Elsewhere Haze on the same patch, the stem muted");
            }

            auto both = sender.getChildFile ("Beats/2026-11-27 09.10.00 Beat P4 90bpm");
            writeSine (both.getChildFile ("17 Keys Haze Pastel Mall.wav"));
            writeMidi (both.getChildFile ("Keys.mid"), 1, { { 60, 80 }, { 72, 80 } });
            writeMidi (both.getChildFile ("Keys 03.mid"), 1, { { 72, 80 } });
            writeMidi (both.getChildFile ("Keys 17.mid"), 1, { { 60, 80 } });
            writeSine (both.getChildFile ("Sounds/03 Piano.wav"));
            both.getChildFile (SomewhereBeat::infoFileName)
                .replaceWithText (R"j({"app":"Somewhere","format":1,"bpm":90,"swing":0,"patterns":[1],"steps":16,"sender":"Nick (a1b2)",
                                      "keysSound":"Haze Pastel Mall","keysPad":17,"keysPads":[17,3],"keysSynth":{"pad":17,"synth":"Haze","patch":1,"patchName":"Pastel Mall"},
                                      "sounds":[{"pad":3,"name":"Piano","file":"Sounds/03 Piano.wav"}]})j");
            {
                auto [p, r] = importBeat (both, "Haze And Piano");
                HazeDevice* haze = nullptr;
                te::SamplerPlugin* piano = nullptr;
                for (auto* t : r.keysBySound)
                {
                    if (auto* h = hazeOn (t)) haze = h;
                    if (auto* sm = t->pluginList.findFirstPluginOfType<te::SamplerPlugin>()) piano = sm;
                }
                log.check (r.keysBySound.size() == 2 && haze != nullptr && haze->getPatchIndex() == 1 && piano != nullptr && r.keysPlay
                           && r.stems.getLast()->isMuted (false), "Haze next to a sampled sound: a Haze track and a Sampler track, the stem muted");
            }

            // keysSynths: two synths at once, one with a control moved on the phone
            auto two = sender.getChildFile ("Beats/2026-11-27 09.20.00 Beat P5 90bpm");
            writeSine (two.getChildFile ("17 Keys Poly.wav"));
            writeMidi (two.getChildFile ("Keys.mid"), 1, { { 60, 80 }, { 48, 80 } });
            writeMidi (two.getChildFile ("Keys 18.mid"), 1, { { 60, 80 } });
            writeMidi (two.getChildFile ("Keys 20.mid"), 1, { { 48, 80 } });
            two.getChildFile (SomewhereBeat::infoFileName)
               .replaceWithText (R"j({"app":"Somewhere","format":1,"bpm":90,"swing":0,"patterns":[1],"steps":16,"sender":"Nick (a1b2)",
                                     "keysPad":18,"keysPads":[18,20],"keysSynth":{"pad":18,"synth":"Poly","patch":3},
                                     "keysSynths":[{"pad":18,"synth":"Poly","patch":3,"params":{"volume":-12.5}},{"pad":20,"synth":"Mono","patch":2}],"sounds":[]})j");
            {
                auto [p, r] = importBeat (two, "Poly And Mono");
                PolyDevice* poly = nullptr;
                MonoDevice* mono = nullptr;
                for (auto* t : r.keysBySound)
                {
                    if (auto* d = t->pluginList.findFirstPluginOfType<PolyDevice>()) poly = d;
                    if (auto* d = t->pluginList.findFirstPluginOfType<MonoDevice>()) mono = d;
                }
                auto vol = poly != nullptr ? poly->getAutomatableParameterByID ("volume") : nullptr;
                log.check (r.keysBySound.size() == 2 && poly != nullptr && mono != nullptr && vol != nullptr
                           && juce::approximatelyEqual (vol->getCurrentValue(), -12.5f) && mono->getPatchIndex() == 2 && r.keysPlay
                           && r.stems.getLast()->isMuted (false),
                           "keysSynths: a Poly track with the phone's volume and a Mono track on its patch, the stem muted");
            }
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

            log.check (juce::approximatelyEqual (p.getEdit().tempoSequence.getTempo (0)->getBpm(), 120.0) && juce::approximatelyEqual (r.songBpm, 120.0),
                       "a song with clips keeps its tempo, and says so");
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

    // The drum track plays the beat as the phone does: same hits, same levels, hard and soft
    {
        using namespace beatRender;
        if (auto deviceRate = engine.getDeviceManager().getSampleRate(); deviceRate > 0)
            rate = deviceRate;

        auto folder = outDir.getChildFile ("Inbox/Ana (e5f6)/Beats/2026-11-26 12.00.00 Beat P1 96bpm");
        std::vector<std::pair<double, int>> kicks, hats;

        for (int b = 0; b < steps / 4; ++b)
        {
            kicks.push_back ({ (double) b, b % 2 == 1 ? 56 : 100 });     // soft and hard, as Somewhere writes them
            hats.push_back ({ b + 0.5, 100 });
        }

        writeWav (folder.getChildFile ("01 Kick.wav"), stem (oneShot (true), kicks));
        writeWav (folder.getChildFile ("04 Hat.wav"), stem (oneShot (false), hats));
        writeWav (folder.getChildFile ("Sounds/01 Kick.wav"), oneShot (true));
        writeWav (folder.getChildFile ("Sounds/04 Hat.wav"), oneShot (false));

        {
            juce::MidiMessageSequence seq;
            for (auto [beat, vel] : kicks) { seq.addEvent (juce::MidiMessage::noteOn (10, 36, (juce::uint8) vel), beat * 96.0); seq.addEvent (juce::MidiMessage::noteOff (10, 36), beat * 96.0 + 12); }
            for (auto [beat, vel] : hats)  { seq.addEvent (juce::MidiMessage::noteOn (10, 39, (juce::uint8) vel), beat * 96.0); seq.addEvent (juce::MidiMessage::noteOff (10, 39), beat * 96.0 + 12); }
            seq.sort();
            seq.updateMatchedPairs();
            juce::MidiFile mf;
            mf.setTicksPerQuarterNote (96);
            mf.addTrack (seq);
            juce::FileOutputStream out (folder.getChildFile ("Beat.mid"));
            mf.writeTo (out);
        }

        folder.getChildFile (SomewhereBeat::infoFileName).replaceWithText (R"j({"app":"Somewhere","format":1,"bpm":100,"swing":0,"patterns":[1],"steps":32,"sender":"Ana (e5f6)",
            "sounds":[{"pad":1,"name":"Kick","file":"Sounds/01 Kick.wav"},{"pad":4,"name":"Hat","file":"Sounds/04 Hat.wav"}]})j");

        auto beat = SomewhereBeat::read (folder);
        auto f = Project::createProjectFolder (outDir.getChildFile ("Beats"), "Rendered Beat");
        Project p (engine, f);
        juce::Array<juce::File> stems;
        if (beat) for (auto& st : beat->stems) stems.add (p.importAudioFile (st.file));
        auto r = beat ? SomewhereBeat::build (p.getEdit(), *beat, stems, {}, nullptr, [&p] (const juce::File& x) { return p.importAudioFile (x); })
                      : SomewhereBeat::Result();
        log.check (r.drums != nullptr && r.drumsPlay && r.stems.size() == 2, "the rendered beat imports as MIDI");

        if (r.drums != nullptr && r.stems.size() == 2)
        {
            for (auto t : te::getAudioTracks (p.getEdit()))
                t->setMute (false);

            auto midi = render (p.getEdit(), { r.drums }, outDir.getChildFile ("beat-midi.wav"));
            auto audio = render (p.getEdit(), { r.stems[0], r.stems[1] }, outDir.getChildFile ("beat-stems.wav"));
            double worstDb = 0.0, worstCorr = 1.0;

            for (auto& hits : { kicks, hats })
                for (auto [b, vel] : hits)
                {
                    auto from = sampleAt (b), n = (int) (rate * 0.05);
                    worstDb = std::max (worstDb, std::abs (rmsDb (midi, from, n) - rmsDb (audio, from, n)));
                    worstCorr = std::min (worstCorr, correlation (midi, audio, from, n));
                }

            log.check (worstDb < 0.5, "every hit within 0.5 dB of the stems, soft ones too (worst " + juce::String (worstDb, 2) + " dB)");
            log.check (worstCorr > 0.97, "every hit lines up with the stems (worst correlation " + juce::String (worstCorr, 3) + ")");
        }
    }

    log.text = "Project audio: " + juce::String (log.failures) + " failure(s)\n" + log.text;
    outDir.getChildFile ("project-audio.txt").replaceWithText (log.text);
    std::cout << log.text;
    return log.failures;
}

} // namespace elsewhere::ProjectAudioTests
