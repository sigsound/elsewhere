// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "Mixing.h"
#include "devices/DrumsDevice.h"
#include "devices/HazeDevice.h"
#include "devices/ModularDevice.h"
#include "devices/MonoDevice.h"
#include "devices/OrganicDevice.h"
#include "devices/PolyDevice.h"
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
            Keys 03.mid ...                 with keys in more than one sound: the notes each one plays,
                                            numbered by its pad
            Sounds/01 Kick.wav ...          each sound the beat plays (a one-shot), bounced with its pad's settings
            beat.somewhere.json             tempo, swing, patterns, keysSound, keysPad (the sound most
                                            keys play), keysPads (every one, when there's more than one),
                                            keysSynth (when the keys play a synth: { pad (17-21), synth
                                            "Haze", "Poly", "Organic", "Mono" or "Modular", patch,
                                            patchName, params { id: value } }: the one playing most),
                                            keysSynths (every synth the keys play, in that form),
                                            and "sounds":
                                            [{ pad (1-based), name, file (relative to the folder) }]

    Beats sent before Sounds/ was added have no one-shots of their own: then a pad's sound is
    found among the samples the phone sent (beside Beats, each with a .somewhere.json that
    names it), which never include its built-in kit sounds.

    Dropping or importing the folder (or the files in it) makes:
      - a drum track playing Beat.mid through Elsewhere Drums, each pad loaded with its sound,
        so the beat can be edited as MIDI;
      - a MIDI track playing Keys.mid through a Sampler loaded with the keys' sound (pitched
        from C4, as on the phone); keys in several sounds get a track per sound, from its own
        MIDI file, and play from their stem when there are no such files; keys played by one
        of its synths get that Elsewhere device, on the same patch with the same settings;
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
        int keysPad = -1;           // 0-15: the pad whose sound plays the keys (most of them)
        std::vector<int> keysPads;  // 0-15: every sound the keys play, when there's more than one
        std::map<int, juce::File> keysMidiBySound;  // pad -> "Keys NN.mid", the notes in that sound
        struct Synth
        {
            int pad = -1;           // as a "pad" after the 16 (16: "Keys 17.mid")
            juce::String name, patchName;   // "Haze", "Poly", "Organic", "Mono" or "Modular"
            int patch = 0;
            std::vector<std::pair<juce::String, float>> params;     // controls moved away from the patch
        };
        std::vector<Synth> synths;  // the synths the keys play, each in a "pad" of its own

        const Synth* synthFor (int pad) const
        {
            for (auto& s : synths)
                if (s.pad == pad)
                    return &s;
            return nullptr;
        }
        std::map<int, juce::File> sounds;   // pad (0-15) -> its one-shot, from "sounds"
        double bpm = 0.0;
        int steps = 0;              // sixteenths
        std::vector<Stem> stems;    // in pad order, the keys last
        juce::File beatMidi, keysMidi;

        /** The folder Somewhere writes everything else into (its samples sit here). */
        juce::File senderFolder() const     { return folder.getParentDirectory().getParentDirectory(); }
    };

    namespace detail
    {
        // The synths Somewhere's keys can play, by the name it gives them, and what to do with each
        template <typename Fn>
        bool withSynth (const juce::String& name, Fn&& fn)
        {
            if (name == "Haze")     { fn ((HazeDevice*) nullptr); return true; }
            if (name == "Poly")     { fn ((PolyDevice*) nullptr); return true; }
            if (name == "Organic")  { fn ((OrganicDevice*) nullptr); return true; }
            if (name == "Mono")     { fn ((MonoDevice*) nullptr); return true; }
            if (name == "Modular")  { fn ((ModularDevice*) nullptr); return true; }
            return false;
        }

        inline int synthPatchCount (const juce::String& name)
        {
            int n = 0;
            withSynth (name, [&n] (auto* d) { n = (int) std::remove_pointer_t<decltype (d)>::patches().size(); });
            return n;
        }

        inline juce::String synthPatchName (const juce::String& name, int patch)
        {
            juce::String s;
            withSynth (name, [&] (auto* d)
            {
                const auto& all = std::remove_pointer_t<decltype (d)>::patches();
                if (patch >= 0 && patch < (int) all.size())
                    s = all[(size_t) patch].name;
            });
            return s;
        }

        /** Puts the synth on the track, on its patch, with the controls Somewhere moved; false if it can't. */
        inline bool addSynth (te::Edit& edit, te::AudioTrack& track, const Beat::Synth& synth)
        {
            bool made = false;
            withSynth (synth.name, [&] (auto* type)
            {
                using Device = std::remove_pointer_t<decltype (type)>;
                if (auto p = DeviceRegistry::create (edit, Device::xmlTypeName))
                {
                    track.pluginList.insertPlugin (p, 0, nullptr);
                    if (auto* d = dynamic_cast<Device*> (p.get()))
                    {
                        d->loadPatch (synth.patch, false);      // part of the import
                        for (auto& [id, value] : synth.params)
                            if (auto ap = d->getAutomatableParameterByID (id))
                                ap->setParameter (ap->getValueRange().clipValue (value), juce::sendNotification);
                        made = true;
                    }
                }
            });
            return made;
        }
    }

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
        b.steps = (int) info["steps"];
        b.keysSound = info["keysSound"].toString();
        b.keysPad = info.hasProperty ("keysPad") ? (int) info["keysPad"] - 1 : -1;

        // keysSynths: every synth, the main one first; keysSynth: the main one (older beats have only that)
        auto readSynth = [&b] (const juce::var& v)
        {
            Beat::Synth s;
            s.name = v["synth"].toString();
            const int count = detail::synthPatchCount (s.name);
            if (! v.isObject() || count == 0)
                return;

            s.pad = juce::jlimit (DrumsDevice::numPads, 63, (int) v["pad"] - 1);
            s.patch = juce::jlimit (0, count - 1, (int) v["patch"]);
            s.patchName = v["patchName"].toString().isNotEmpty() ? v["patchName"].toString() : detail::synthPatchName (s.name, s.patch);
            if (auto* obj = v["params"].getDynamicObject())
                for (auto& prop : obj->getProperties())
                    if (prop.value.isDouble() || prop.value.isInt() || prop.value.isInt64())
                        s.params.push_back ({ prop.name.toString(), (float) (double) prop.value });

            if (b.synthFor (s.pad) == nullptr)
                b.synths.push_back (s);
        };

        if (auto* all = info["keysSynths"].getArray())
            for (auto& v : *all)
                readSynth (v);

        if (auto* list = info["keysSynth"].getArray())
            for (auto& v : *list)
                readSynth (v);
        else
            readSynth (info["keysSynth"]);

        if (auto* pads = info["keysPads"].getArray())
            for (auto& v : *pads)
                if (auto pad = (int) v - 1; pad >= 0 && (pad < DrumsDevice::numPads || b.synthFor (pad) != nullptr) && std::find (b.keysPads.begin(), b.keysPads.end(), pad) == b.keysPads.end())
                {
                    b.keysPads.push_back (pad);
                    if (auto m = folder.getChildFile ("Keys " + juce::String (pad + 1).paddedLeft ('0', 2) + ".mid"); m.existsAsFile())
                        b.keysMidiBySound[pad] = m;
                }

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
        juce::Array<te::AudioTrack*> keysBySound;   // with keys in several sounds: a track for each
        juce::StringArray missing;      // sounds that weren't found
        bool mixedKeys = false;         // keys in several sounds with no file per sound: they play from their stem
        double songBpm = 0.0;           // set when the song kept a tempo other than the beat's
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

        /** A clip lasts the whole beat, not just to its last note, so it loops as on the phone. */
        inline void lastWholeBeat (te::Clip& clip, int steps)
        {
            if (steps <= 0)
                return;

            auto& ts = clip.edit.tempoSequence;
            auto end = ts.toTime (ts.toBeats (clip.getPosition().getStart()) + te::BeatDuration::fromBeats (steps / 4.0));

            if (end > clip.getPosition().getEnd())
                clip.setEnd (end, true);
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

        if (auto* tempo = edit.tempoSequence.getTempo (0); tempo != nullptr && beat.bpm > 0.0)
        {
            if (empty)
                tempo->setBpm (juce::jlimit (20.0, 999.0, beat.bpm));
            else if (std::abs (tempo->getBpm() - beat.bpm) > 0.01)
                r.songBpm = tempo->getBpm();
        }

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



                    if (auto* d = dynamic_cast<DrumsDevice*> (p.get()))
                    {
                        d->setPlainVelocity (true);     // soft and hard steps as loud as on the phone

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

                if (auto* mc = detail::addMidi (*r.drums, beat.beatMidi, start))
                    detail::lastWholeBeat (*mc, beat.steps);
            }
        }

        // A MIDI track playing a file through a Sampler with one sound; false when the sound isn't found
        auto addKeys = [&] (const juce::File& midi, int pad, const juce::String& sound, const juce::String& trackName) -> std::pair<te::AudioTrack*, bool>
        {
            auto* track = detail::newTrack (edit, after, trackName);
            bool plays = false;

            if (track == nullptr)
                return { nullptr, false };

            after = track;

            if (auto* synth = beat.synthFor (pad))
            {
                plays = detail::addSynth (edit, *track, *synth);
            }
            else if (auto p = EngineHelpers::insertBuiltInPlugin (*track, te::SamplerPlugin::xmlTypeName, 0))
            {
                if (auto* sampler = dynamic_cast<te::SamplerPlugin*> (p.get()))
                {
                    if (auto f = soundFor (beat, pad, sound); f.existsAsFile())
                    {
                        auto local = copy ? copy (f) : f;

                        if (sampler->addSound (detail::pathFor (edit, local), sound, 0.0, 0.0, 0.0f).isEmpty())
                        {
                            sampler->setSoundParams (sampler->getNumSounds() - 1, 60, 0, 127);
                            plays = true;
                        }
                    }
                    else
                    {
                        r.missing.add (sound.isNotEmpty() ? sound : juce::String ("the keys' sound"));
                    }
                }
            }

            if (auto* mc = detail::addMidi (*track, midi, start))
                detail::lastWholeBeat (*mc, beat.steps);

            return { track, plays };
        };

        // A sound's name, from its one-shot ("03 Marimba.wav")
        auto soundName = [&beat] (int pad)
        {
            if (auto* synth = beat.synthFor (pad))
                return synth->name + " " + synth->patchName;

            if (pad == beat.keysPad && beat.keysSound.isNotEmpty())
                return beat.keysSound;

            if (auto f = soundInBeat (beat, pad, {}); f.existsAsFile())
                return f.getFileNameWithoutExtension().fromFirstOccurrenceOf (" ", false, false);

            return "Sound " + juce::String (pad + 1);
        };

        if (beat.keysMidi.existsAsFile())
        {
            const bool split = beat.keysPads.size() > 1 && beat.keysMidiBySound.size() == beat.keysPads.size();

            if (split)
            {
                r.keysPlay = true;

                for (auto pad : beat.keysPads)
                {
                    auto name = soundName (pad);
                    auto [track, plays] = addKeys (beat.keysMidiBySound.at (pad), pad, name, "Keys " + name + " (" + beat.name + ")");

                    if (track != nullptr)
                        r.keysBySound.add (track);

                    r.keysPlay = r.keysPlay && plays && track != nullptr;
                }

                r.keys = r.keysBySound.getFirst();
            }
            else
            {
                auto [track, plays] = addKeys (beat.keysMidi, beat.keysPad, beat.keysSound, "Keys (" + beat.name + ")");
                r.keys = track;

                // One sampler can't play notes in several sounds: the stem has them right
                r.mixedKeys = beat.keysPads.size() > 1;
                r.keysPlay = plays && ! r.mixedKeys;
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
        for (auto* t : r.keysBySound)  t->setMute (! r.keysPlay && ! r.stems.isEmpty());
        return r;
    }
}

} // namespace elsewhere
