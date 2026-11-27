// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "Mixing.h"
#include "devices/DrumsDevice.h"
#include "drums/DrumTrack.h"

namespace elsewhere
{
//==============================================================================
/**
    Beats sent from Somewhere. Its Export to Elsewhere writes one folder per beat:

        <phone's folder>/Beats/2026-10-05 21.04.10 Beat P1 120bpm/
            01 Kick.wav ... 16 Zap.wav      one stem per pad, all starting at bar 1
            17 Keys Piano.wav               the keys, played by their pad's sound (when there are notes)
            Beat.mid                        the drum steps: pad 1 = C1 (36), soft and hard velocities
            Keys.mid                        the keys as ordinary notes (when there are notes)
            Sounds/01 Kick.wav ...          each sound the beat plays (a one-shot), bounced with its pad's settings
            beat.somewhere.json             tempo, swing, patterns, keysSound, keysPad, and "sounds":
                                            [{ pad (1-based), name, file (relative to the folder) }]

    Beats sent before Sounds/ was added have no one-shots of their own: then a pad's sound is
    found among the samples the phone sent (beside Beats, each with a .somewhere.json that
    names it), which never include its built-in kit sounds.

    Dropping or importing the folder (or the files in it) makes:
      - a drum track playing Beat.mid through Elsewhere Drums, each pad loaded with its sound,
        so the beat can be edited as MIDI;
      - a MIDI track playing Keys.mid through a Sampler loaded with the keys' sound (pitched
        from C4, as on the phone);
      - an audio track per stem, exactly as exported.
    The MIDI tracks play when every sound they need was found; then the stems are muted.
    Otherwise the stems play and the MIDI track is muted, so the beat never plays twice.
*/
namespace SomewhereBeat
{
    inline constexpr const char* infoFileName = "beat.somewhere.json";
    inline constexpr int keysStemNumber = DrumsDevice::numPads + 1;

    struct Stem
    {
        juce::File file;
        int pad = -1;               // 0-15, or -1 for the keys
        juce::String sound;         // the sound's name ("NN <sound>.wav")
    };

    struct Beat
    {
        juce::File folder;
        juce::String name, keysSound;
        int keysPad = -1;           // 0-15: the pad whose sound plays the keys
        std::map<int, juce::File> sounds;   // pad (0-15) -> its one-shot, from "sounds"
        double bpm = 0.0;
        std::vector<Stem> stems;    // in pad order, the keys last
        juce::File beatMidi, keysMidi;

        /** The folder Somewhere writes everything else into (its samples sit here). */
        juce::File senderFolder() const     { return folder.getParentDirectory().getParentDirectory(); }
    };

    inline bool isBeatFolder (const juce::File& f)
    {
        return f.isDirectory() && f.getChildFile (infoFileName).existsAsFile();
    }

    /** The beat folder these files are, or are all inside; none otherwise. */
    inline juce::File findBeatFolder (const juce::Array<juce::File>& files)
    {
        if (files.isEmpty())
            return {};

        for (auto& f : files)
            if (isBeatFolder (f))
                return f;

        auto parent = files.getFirst().getParentDirectory();

        for (auto& f : files)
            if (f.getParentDirectory() != parent)
                return {};

        return isBeatFolder (parent) ? parent : juce::File();
    }

    inline std::optional<Beat> read (const juce::File& folder)
    {
        if (! isBeatFolder (folder))
            return std::nullopt;

        auto info = juce::JSON::parse (folder.getChildFile (infoFileName));

        if (info["app"].toString() != "Somewhere")
            return std::nullopt;

        Beat b;
        b.folder = folder;
        b.name = folder.getFileName().fromFirstOccurrenceOf (" ", false, false).fromFirstOccurrenceOf (" ", false, false).trim();
        if (b.name.isEmpty()) b.name = folder.getFileName();
        b.bpm = (double) info["bpm"];
        b.keysSound = info["keysSound"].toString();
        b.keysPad = info.hasProperty ("keysPad") ? (int) info["keysPad"] - 1 : -1;

        if (auto* list = info["sounds"].getArray())
            for (auto& e : *list)
                if (auto pad = (int) e["pad"] - 1; pad >= 0 && pad < DrumsDevice::numPads && e["file"].toString().isNotEmpty())
                    if (auto f = folder.getChildFile (e["file"].toString()); f.existsAsFile() && f.isAChildOf (folder))
                        b.sounds[pad] = f;

        for (auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.wav"))
        {
            auto base = f.getFileNameWithoutExtension();
            auto number = base.upToFirstOccurrenceOf (" ", false, false);

            if (! number.containsOnly ("0123456789") || number.isEmpty())
                continue;

            auto n = number.getIntValue();
            Stem s { f, -1, base.fromFirstOccurrenceOf (" ", false, false) };

            if (n >= 1 && n <= DrumsDevice::numPads)
                s.pad = n - 1;
            else if (n == keysStemNumber)
                s.sound = b.keysSound;     // the word before it ("Keys") is in the phone's language
            else
                continue;

            b.stems.push_back (s);
        }

        std::sort (b.stems.begin(), b.stems.end(), [] (const Stem& x, const Stem& y)
        {
            return (x.pad < 0 ? 99 : x.pad) < (y.pad < 0 ? 99 : y.pad);
        });

        if (auto m = folder.getChildFile ("Beat.mid"); m.existsAsFile()) b.beatMidi = m;
        if (auto m = folder.getChildFile ("Keys.mid"); m.existsAsFile()) b.keysMidi = m;
        return b;
    }

    /**
        A sound Somewhere sent, by name, from the sample files in its folder. When no name
        matches, the one that was on the pad (pad >= 0), if only one was.
    */
    inline juce::File findSound (const juce::File& senderFolder, const juce::String& name, int pad = -1)
    {
        juce::Array<juce::File> onPad;
        juce::File named;
        juce::String namedAt;

        for (auto& json : senderFolder.findChildFiles (juce::File::findFiles, false, "*.somewhere.json"))
        {
            if (json.getFileName() == infoFileName)
                continue;

            auto wav = json.getSiblingFile (json.getFileName().upToLastOccurrenceOf (".somewhere.json", false, true) + ".wav");

            if (! wav.existsAsFile())
                continue;

            auto info = juce::JSON::parse (json);

            // Names needn't be unique: the newest recording wins
            if (name.isNotEmpty() && info["name"].toString() == name)
            {
                if (auto at = info["recordedAt"].toString(); named == juce::File() || at > namedAt)
                    named = wav, namedAt = at;

                continue;
            }

            if (pad >= 0 && (int) info["pad"] == pad + 1)
                onPad.add (wav);
        }

        if (named != juce::File())
            return named;

        return onPad.size() == 1 ? onPad.getFirst() : juce::File();
    }

    /** A pad's sound from the beat's own Sounds folder ("NN <name>.wav"), by pad, else by name. */
    inline juce::File soundInBeat (const Beat& beat, int pad, const juce::String& name)
    {
        if (auto it = beat.sounds.find (pad); it != beat.sounds.end())
            return it->second;

        auto files = beat.folder.getChildFile ("Sounds").findChildFiles (juce::File::findFiles, false, "*.wav");
        files.sort();

        for (auto& f : files)
            if (pad >= 0 && f.getFileName().upToFirstOccurrenceOf (" ", false, false).getIntValue() == pad + 1)
                return f;

        for (auto& f : files)
            if (name.isNotEmpty() && f.getFileNameWithoutExtension().fromFirstOccurrenceOf (" ", false, false) == name)
                return f;

        return {};
    }

    /** Where a pad's sound comes from: the beat itself, else what the phone sent. */
    inline juce::File soundFor (const Beat& beat, int pad, const juce::String& name)
    {
        if (auto f = soundInBeat (beat, pad, name); f.existsAsFile())
            return f;

        return findSound (beat.senderFolder(), name, pad);
    }

    struct Result
    {
        te::AudioTrack* drums = nullptr;
        te::AudioTrack* keys = nullptr;
        juce::Array<te::AudioTrack*> stems;
        juce::StringArray missing;      // sounds that weren't found
        bool drumsPlay = false, keysPlay = false;
    };

    namespace detail
    {
        inline te::AudioTrack* newTrack (te::Edit& edit, te::Track* after, const juce::String& name)
        {
            auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, after), nullptr);

            if (track == nullptr)
                return nullptr;

            track->setName (name);
            track->setColour (EngineHelpers::trackColourFor (te::getAudioTracks (edit).indexOf (track.get())));
            track->getClipSlotList().ensureNumberOfSlots (edit.getSceneList().getScenes().size());
            Mixing::ensureSends (*track);
            return track.get();
        }

        inline te::MidiClip* addMidi (te::AudioTrack& track, const juce::File& midi, te::TimePosition start)
        {
            auto clip = te::createClipFromFile (midi, track, false);
            auto* mc = dynamic_cast<te::MidiClip*> (clip.get());

            if (mc != nullptr)
                mc->setStart (start, false, true);

            return mc;
        }

        /** A path the edit can find again when the project moves (relative inside the project). */
        inline juce::String pathFor (te::Edit& edit, const juce::File& f)
        {
            if (auto editFile = edit.editFileRetriever ? edit.editFileRetriever() : juce::File(); editFile != juce::File())
                if (f.isAChildOf (editFile.getParentDirectory()))
                    return f.getRelativePathFrom (editFile.getParentDirectory());

            return f.getFullPathName();
        }
    }

    /**
        Makes the beat's tracks after `after`. `stemFiles` are the stems as they'll be used
        (copied into the project), in the same order as beat.stems; `copy` copies a sound into
        the project. Sets the tempo too when the edit has no clips yet.
    */
    inline Result build (te::Edit& edit, const Beat& beat, const juce::Array<juce::File>& stemFiles, te::TimePosition start,
                         te::Track* after, const std::function<juce::File (const juce::File&)>& copy)
    {
        Result r;
        auto& um = edit.getUndoManager();
        auto sender = beat.senderFolder();
        bool empty = true;

        for (auto t : te::getAudioTracks (edit))
            if (! t->getClips().isEmpty())
                empty = false;

        if (empty && beat.bpm > 0.0)
            edit.tempoSequence.getTempo (0)->setBpm (juce::jlimit (20.0, 999.0, beat.bpm));

        if (beat.beatMidi.existsAsFile())
        {
            if ((r.drums = detail::newTrack (edit, after, "Drums (" + beat.name + ")")) != nullptr)
            {
                after = r.drums;
                r.drums->state.setProperty (DrumTrack::trackProp, true, &um);

                if (auto p = DeviceRegistry::create (edit, DrumsDevice::xmlTypeName))
                {
                    r.drums->pluginList.insertPlugin (p, 0, nullptr);
                    bool all = true;

                    // As a new drum track: kick + crash together can reach full scale
                    if (auto vol = p->getAutomatableParameterByID ("volume"))
                        vol->setParameter (-6.0f, juce::sendNotification);

                    if (auto* d = dynamic_cast<DrumsDevice*> (p.get()))
                    {
                        for (auto& s : beat.stems)
                        {
                            if (s.pad < 0)
                                continue;

                            if (auto f = soundFor (beat, s.pad, s.sound); f.existsAsFile())
                                d->setPadSample (s.pad, copy ? copy (f) : f);
                            else
                                r.missing.add (s.sound), all = false;
                        }
                    }

                    r.drumsPlay = all;
                }

                detail::addMidi (*r.drums, beat.beatMidi, start);
            }
        }

        if (beat.keysMidi.existsAsFile())
        {
            if ((r.keys = detail::newTrack (edit, after, "Keys (" + beat.name + ")")) != nullptr)
            {
                after = r.keys;

                if (auto p = EngineHelpers::insertBuiltInPlugin (*r.keys, te::SamplerPlugin::xmlTypeName, 0))
                {
                    if (auto* sampler = dynamic_cast<te::SamplerPlugin*> (p.get()))
                    {
                        if (auto f = soundFor (beat, beat.keysPad, beat.keysSound); f.existsAsFile())
                        {
                            auto local = copy ? copy (f) : f;

                            if (sampler->addSound (detail::pathFor (edit, local), beat.keysSound, 0.0, 0.0, 0.0f).isEmpty())
                            {
                                sampler->setSoundParams (sampler->getNumSounds() - 1, 60, 0, 127);
                                r.keysPlay = true;
                            }
                        }
                        else
                        {
                            r.missing.add (beat.keysSound.isNotEmpty() ? beat.keysSound : juce::String ("the keys' sound"));
                        }
                    }
                }

                detail::addMidi (*r.keys, beat.keysMidi, start);
            }
        }

        for (size_t i = 0; i < beat.stems.size() && (int) i < stemFiles.size(); ++i)
        {
            auto& s = beat.stems[i];
            te::AudioFile af (edit.engine, stemFiles[(int) i]);

            if (! af.isValid())
                continue;

            auto* track = detail::newTrack (edit, after, s.file.getFileNameWithoutExtension());

            if (track == nullptr)
                continue;

            after = track;
            r.stems.add (track);

            if (auto clip = track->insertWaveClip (s.file.getFileNameWithoutExtension(), stemFiles[(int) i],
                                                   { { start, te::TimeDuration::fromSeconds (af.getLength()) }, {} }, false))
            {
                clip->setAutoTempo (false);
                clip->setFadeIn ({});
                clip->setFadeOut ({});
            }

            // Whichever version plays, the other is there but muted
            bool midiPlays = s.pad < 0 ? (r.keys != nullptr && r.keysPlay) : (r.drums != nullptr && r.drumsPlay);
            track->setMute (midiPlays);
        }

        if (r.drums != nullptr) r.drums->setMute (! r.drumsPlay && ! r.stems.isEmpty());
        if (r.keys != nullptr)  r.keys->setMute (! r.keysPlay && ! r.stems.isEmpty());
        return r;
    }
}

} // namespace elsewhere
