// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Ableton.h"
#include "DawProject.h"

namespace elsewhere::Ableton
{
namespace
{
    using Xml = juce::XmlElement;

    const Xml* child (const Xml* e, const char* path)
    {
        for (auto& name : juce::StringArray::fromTokens (path, "/", ""))
            e = e != nullptr ? e->getChildByName (name) : nullptr;

        return e;
    }

    juce::String value (const Xml* e, const char* path, const juce::String& fallback = {})
    {
        auto* c = child (e, path);
        return c != nullptr && c->hasAttribute ("Value") ? c->getStringAttribute ("Value") : fallback;
    }

    double number (const Xml* e, const char* path, double fallback = 0.0)
    {
        auto v = value (e, path);
        return v.isNotEmpty() ? v.getDoubleValue() : fallback;
    }

    bool isTrue (const Xml* e, const char* path, bool fallback)
    {
        auto v = value (e, path);
        return v.isEmpty() ? fallback : v == "true";
    }

    struct Point { double time, value; };

    /** A parameter's automation: Live 9+ keeps it in the track's AutomationEnvelopes (matched by
        AutomationTarget id), Live 8 inside the parameter. Times are beats; the first event of a
        Live 8 parameter sits far before zero and is its value at the start. */
    std::vector<Point> automation (const Xml* track, const Xml* param)
    {
        std::vector<Point> points;

        auto read = [&points] (const Xml* events)
        {
            if (events != nullptr)
                for (auto* ev : events->getChildIterator())
                    if (ev->hasAttribute ("Time"))
                        points.push_back ({ ev->getDoubleAttribute ("Time"), ev->getStringAttribute ("Value") == "true" ? 1.0
                                                                            : ev->getStringAttribute ("Value") == "false" ? 0.0
                                                                            : ev->getDoubleAttribute ("Value") });
        };

        read (child (param, "ArrangerAutomation/Events"));

        if (auto* target = child (param, "AutomationTarget"); target != nullptr && track != nullptr)
            if (auto* envelopes = child (track, "AutomationEnvelopes/Envelopes"))
                for (auto* env : envelopes->getChildIterator())
                    if (value (env, "EnvelopeTarget/PointeeId") == target->getStringAttribute ("Id"))
                        read (child (env, "Automation/Events"));

        std::stable_sort (points.begin(), points.end(), [] (auto& a, auto& b) { return a.time < b.time; });
        return points;
    }

    /** A parameter's value at the start of the song. */
    double initialValue (const Xml* track, const Xml* param, double fallback)
    {
        if (param == nullptr)
            return fallback;

        if (auto v = value (param, "Manual"); v.isNotEmpty())
            return v == "true" ? 1.0 : v == "false" ? 0.0 : v.getDoubleValue();

        auto points = automation (track, param);
        return points.empty() ? fallback : points.front().value;
    }

    /** The mixer of a track (Live 8's master has a MasterChain instead of a DeviceChain). */
    const Xml* mixerOf (const Xml* track)
    {
        if (auto* m = child (track, "DeviceChain/Mixer"))
            return m;

        return child (track, "MasterChain/Mixer");
    }

    juce::String trackName (const Xml* track, int number)
    {
        if (auto user = value (track, "Name/UserName"); user.isNotEmpty())
            return user;

        // Unnamed tracks are shown as "2-Audio": make that "Audio 2"
        auto name = value (track, "Name/EffectiveName");
        auto dash = name.indexOfChar ('-');

        if (dash > 0 && name.substring (0, dash).containsOnly ("0123456789"))
            name = name.substring (dash + 1) + " " + name.substring (0, dash);

        if (name.isEmpty())
            name = (track->hasTagName ("MidiTrack") ? "MIDI " : track->hasTagName ("GroupTrack") ? "Group " : "Audio ") + juce::String (number);

        return name;
    }
}

//==============================================================================
struct Importer::Impl
{
    juce::File source;
    juce::String error;
    std::unique_ptr<Xml> set;                   // the Live Set
    std::unique_ptr<Xml> converted;             // as DAWproject, until it's handed over
    const Xml* convertedView = nullptr;
    std::unique_ptr<DawProject::Importer> inner;
    std::map<juce::String, juce::File> nearbyFiles, filesAround;
    int sessionClips = 0, returnTracks = 0, nextId = 1;

    juce::String id()   { return "a" + juce::String (nextId++); }

    void read()
    {
        juce::FileInputStream file (source);

        if (! file.openedOk())
        {
            error = "Couldn't open \"" + source.getFileName() + "\".";
            return;
        }

        juce::GZIPDecompressorInputStream gz (&file, false, juce::GZIPDecompressorInputStream::gzipFormat);
        set = juce::parseXML (gz.readEntireStreamAsString());

        if (set == nullptr || ! set->hasTagName ("Ableton") || child (set.get(), "LiveSet") == nullptr)
        {
            error = "\"" + source.getFileName() + "\" isn't an Ableton Live Set, or it's damaged.";
            return;
        }

        convert();
        convertedView = converted.get();
        inner = std::make_unique<DawProject::Importer> (std::move (converted), source.getParentDirectory());
    }

    //==============================================================================
    /** Where a clip's file is: Live 11+ stores a relative and an absolute path, older sets a
        list of folders relative to the project. Otherwise look for it by name nearby. */
    juce::File findAudio (const Xml* fileRef)
    {
        auto folder = source.getParentDirectory();
        auto relative = value (fileRef, "RelativePath");

        if (relative.isNotEmpty())
            if (auto f = folder.getChildFile (relative); f.existsAsFile())
                return f;

        if (auto path = value (fileRef, "Path"); path.isNotEmpty() && juce::File::isAbsolutePath (path))
            if (auto f = juce::File (path); f.existsAsFile())
                return f;

        auto name = value (fileRef, "Name");

        if (name.isEmpty())
            name = (relative.isNotEmpty() ? relative : value (fileRef, "Path")).replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);

        if (auto* dirs = child (fileRef, "RelativePath"); dirs != nullptr && dirs->getNumChildElements() > 0)
        {
            auto f = folder;

            for (auto* d : dirs->getChildWithTagNameIterator ("RelativePathElement"))
                f = f.getChildFile (d->getStringAttribute ("Dir"));

            if (f.getChildFile (name).existsAsFile())
                return f.getChildFile (name);
        }

        if (name.isEmpty())
            return {};

        if (auto f = SessionImport::findFileNear (folder, name, nearbyFiles); f.existsAsFile())
            return f;

        // Sets often use audio that sits beside the project folder rather than in it
        if (auto f = SessionImport::findFileNear (folder.getParentDirectory(), name, filesAround); f.existsAsFile())
            return f;

        return {};
    }

    /** The name to report for a file that couldn't be found. */
    static juce::String fileName (const Xml* fileRef)
    {
        auto name = value (fileRef, "Name");

        if (name.isEmpty())
            name = (value (fileRef, "RelativePath").isNotEmpty() ? value (fileRef, "RelativePath") : value (fileRef, "Path"))
                       .replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false);

        return name.isNotEmpty() ? name : "audio file";
    }

    //==============================================================================
    void addMixer (Xml& channel, const Xml* track, const Xml* mixer)
    {
        auto* mute = channel.createNewChildElement ("Mute");
        mute->setAttribute ("value", initialValue (track, child (mixer, "Speaker"), 1.0) < 0.5 ? "true" : "false");

        auto* pan = channel.createNewChildElement ("Pan");
        pan->setAttribute ("unit", "normalized");
        pan->setAttribute ("min", 0.0);
        pan->setAttribute ("max", 1.0);
        pan->setAttribute ("value", (initialValue (track, child (mixer, "Pan"), 0.0) + 1.0) / 2.0);

        // Live's fader is linear gain: 1 is 0 dB
        auto* volume = channel.createNewChildElement ("Volume");
        volume->setAttribute ("unit", "linear");
        volume->setAttribute ("value", initialValue (track, child (mixer, "Volume"), 1.0));
    }

    void convertTempo (Xml& project, Xml& arrangement)
    {
        auto* liveSet = child (set.get(), "LiveSet");
        auto* master = child (liveSet, "MainTrack");

        if (master == nullptr)
            master = child (liveSet, "MasterTrack");

        auto* mixer = mixerOf (master);
        auto* tempoParam = child (mixer, "Tempo");
        auto points = automation (master, tempoParam);
        auto bpm = initialValue (master, tempoParam, 120.0);

        auto* transport = project.createNewChildElement ("Transport");
        auto* tempo = transport->createNewChildElement ("Tempo");
        tempo->setAttribute ("unit", "bpm");
        tempo->setAttribute ("value", bpm);

        // Time signature: (numerator - 1) + 99 * log2 (denominator)
        auto sig = (int) initialValue (master, child (mixer, "TimeSignature"), 201);
        auto* timeSig = transport->createNewChildElement ("TimeSignature");
        timeSig->setAttribute ("numerator", sig % 99 + 1);
        timeSig->setAttribute ("denominator", 1 << juce::jlimit (0, 6, sig / 99));

        // Automated tempo: Live ramps linearly between points; two points at one time are a jump
        std::vector<Point> changes;
        for (auto& p : points)
            if (p.time >= 0.0)
                changes.push_back (p);

        if (changes.empty())
            return;

        auto* automationXml = arrangement.createNewChildElement ("TempoAutomation");
        automationXml->setAttribute ("unit", "bpm");
        automationXml->setAttribute ("timeUnit", "beats");

        double last = bpm;

        auto add = [automationXml, &last] (double time, double v, bool ramp)
        {
            last = v;
            auto* p = automationXml->createNewChildElement ("RealPoint");
            p->setAttribute ("time", time);
            p->setAttribute ("value", v);
            p->setAttribute ("interpolation", ramp ? "linear" : "hold");
        };

        if (changes.front().time > 0.0)
            add (0.0, bpm, changes.front().value != bpm);

        for (size_t i = 0; i < changes.size(); ++i)
        {
            auto& p = changes[i];
            bool lastAtThisTime = i + 1 == changes.size() || changes[i + 1].time - p.time > 1.0e-6;

            if (! lastAtThisTime)
            {
                // The value it ramps up to just before the jump (if it moved)
                if (p.value != last)
                    add (std::max (0.0, p.time - 1.0e-3), p.value, false);

                continue;
            }

            bool ramp = i + 1 < changes.size() && changes[i + 1].value != p.value;
            add (p.time, p.value, ramp);
        }
    }

    void convertAudioClip (Xml& clips, const Xml& clip)
    {
        auto start = number (&clip, "CurrentStart", clip.getDoubleAttribute ("Time"));
        auto end = number (&clip, "CurrentEnd", start);

        if (end <= start)
            return;

        auto* fileRef = child (&clip, "SampleRef/FileRef");
        auto audioFile = fileRef != nullptr ? findAudio (fileRef) : juce::File();
        bool warped = isTrue (&clip, "IsWarped", true);
        auto* loop = child (&clip, "Loop");

        auto* c = clips.createNewChildElement ("Clip");
        c->setAttribute ("time", start);
        c->setAttribute ("duration", end - start);
        c->setAttribute ("name", value (&clip, "Name"));
        c->setAttribute ("contentTimeUnit", warped ? "beats" : "seconds");
        c->setAttribute ("playStart", number (loop, "LoopStart") + number (loop, "StartRelative"));

        if (isTrue (loop, "LoopOn", false))
        {
            c->setAttribute ("loopStart", number (loop, "LoopStart"));
            c->setAttribute ("loopEnd", number (loop, "LoopEnd"));
        }

        if (isTrue (&clip, "Disabled", false))
            c->setAttribute ("enable", "false");

        // Arrangement fades are in beats
        c->setAttribute ("fadeTimeUnit", "beats");
        c->setAttribute ("fadeInTime", number (&clip, "Fades/FadeInLength"));
        c->setAttribute ("fadeOutTime", number (&clip, "Fades/FadeOutLength"));

        c->setAttribute ("elsewhereGain", number (&clip, "SampleVolume", 1.0));
        c->setAttribute ("elsewhereTranspose", number (&clip, "PitchCoarse") + number (&clip, "PitchFine") / 100.0);

        auto addAudio = [&] (Xml& parent)
        {
            auto* audio = parent.createNewChildElement ("Audio");

            // Re-Pitch changes pitch with speed, like a record; the other modes keep it
            audio->setAttribute ("algorithm", (int) number (&clip, "WarpMode") == 3 ? "repitch" : "stretch");

            auto* f = audio->createNewChildElement ("File");
            f->setAttribute ("path", audioFile.existsAsFile() ? audioFile.getFullPathName() : fileName (fileRef));
            f->setAttribute ("external", "true");
        };

        if (! warped)
        {
            addAudio (*c);
            return;
        }

        // Warped: the clip's beats map to seconds in the file through its warp markers
        auto* warps = c->createNewChildElement ("Warps");
        warps->setAttribute ("timeUnit", "beats");
        warps->setAttribute ("contentTimeUnit", "seconds");
        addAudio (*warps);

        if (auto* markers = child (&clip, "WarpMarkers"))
        {
            for (auto* m : markers->getChildWithTagNameIterator ("WarpMarker"))
            {
                auto* w = warps->createNewChildElement ("Warp");
                w->setAttribute ("time", m->getDoubleAttribute ("BeatTime"));
                w->setAttribute ("contentTime", m->getDoubleAttribute ("SecTime"));
            }
        }
    }

    void convertMidiClip (Xml& clips, const Xml& clip)
    {
        auto start = number (&clip, "CurrentStart", clip.getDoubleAttribute ("Time"));
        auto end = number (&clip, "CurrentEnd", start);

        if (end <= start)
            return;

        auto* loop = child (&clip, "Loop");
        auto* c = clips.createNewChildElement ("Clip");
        c->setAttribute ("time", start);
        c->setAttribute ("duration", end - start);
        c->setAttribute ("name", value (&clip, "Name"));
        c->setAttribute ("contentTimeUnit", "beats");
        c->setAttribute ("playStart", number (loop, "LoopStart") + number (loop, "StartRelative"));

        if (isTrue (loop, "LoopOn", false))
        {
            c->setAttribute ("loopStart", number (loop, "LoopStart"));
            c->setAttribute ("loopEnd", number (loop, "LoopEnd"));
        }

        if (isTrue (&clip, "Disabled", false))
            c->setAttribute ("enable", "false");

        auto* notes = c->createNewChildElement ("Notes");
        notes->setAttribute ("timeUnit", "beats");

        if (auto* keyTracks = child (&clip, "Notes/KeyTracks"))
        {
            for (auto* key : keyTracks->getChildWithTagNameIterator ("KeyTrack"))
            {
                auto pitch = (int) number (key, "MidiKey", 60);

                if (auto* events = child (key, "Notes"))
                {
                    for (auto* ev : events->getChildWithTagNameIterator ("MidiNoteEvent"))
                    {
                        if (ev->getStringAttribute ("IsEnabled") == "false")
                            continue;

                        auto* n = notes->createNewChildElement ("Note");
                        n->setAttribute ("time", ev->getDoubleAttribute ("Time"));
                        n->setAttribute ("duration", ev->getDoubleAttribute ("Duration"));
                        n->setAttribute ("key", pitch);
                        n->setAttribute ("vel", ev->getDoubleAttribute ("Velocity", 100.0) / 127.0);
                    }
                }
            }
        }
    }

    void convert()
    {
        auto* liveSet = child (set.get(), "LiveSet");

        converted = std::make_unique<Xml> ("Project");
        converted->setAttribute ("version", "1.0");

        auto creator = set->getStringAttribute ("Creator");
        auto* app = converted->createNewChildElement ("Application");
        app->setAttribute ("name", creator.isNotEmpty() ? creator : "Ableton Live " + set->getStringAttribute ("MinorVersion").upToFirstOccurrenceOf ("_", false, false));

        // The Transport comes first, but tempo automation goes in the Arrangement
        auto arrangement = std::make_unique<Xml> ("Arrangement");
        convertTempo (*converted, *arrangement);

        auto* structure = converted->createNewChildElement ("Structure");
        auto* lanes = arrangement->createNewChildElement ("Lanes");
        lanes->setAttribute ("timeUnit", "beats");

        std::map<juce::String, Xml*> groups;     // Live track id -> its DAWproject track

        if (auto* tracks = child (liveSet, "Tracks"))
        {
            int number = 0;

            for (auto* track : tracks->getChildIterator())
            {
                auto kind = track->getTagName();
                ++number;

                if (kind == "ReturnTrack")
                {
                    ++returnTracks;
                    continue;
                }

                if (kind != "AudioTrack" && kind != "MidiTrack" && kind != "GroupTrack")
                    continue;

                // Tracks in a group sit inside it (groups always come before their tracks)
                auto group = value (track, "TrackGroupId", "-1");
                auto parent = groups.find (group);
                auto* t = (parent != groups.end() ? parent->second : structure)->createNewChildElement ("Track");
                auto trackId = id();

                t->setAttribute ("id", trackId);
                t->setAttribute ("name", trackName (track, number));
                t->setAttribute ("contentType", kind == "GroupTrack" ? "tracks" : kind == "MidiTrack" ? "notes" : "audio");

                auto* channel = t->createNewChildElement ("Channel");
                channel->setAttribute ("role", kind == "GroupTrack" ? "submix" : "regular");
                addMixer (*channel, track, mixerOf (track));

                if (kind == "GroupTrack")
                {
                    groups[track->getStringAttribute ("Id")] = t;
                    continue;
                }

                auto* trackLanes = lanes->createNewChildElement ("Lanes");
                trackLanes->setAttribute ("track", trackId);
                auto* clips = trackLanes->createNewChildElement ("Clips");

                // Arrangement clips (Session View clips live in the clip slots and aren't imported)
                auto* events = child (track, "DeviceChain/MainSequencer/Sample/ArrangerAutomation/Events");

                if (events == nullptr)
                    events = child (track, "DeviceChain/MainSequencer/ClipTimeable/ArrangerAutomation/Events");

                if (events != nullptr)
                {
                    for (auto* clip : events->getChildIterator())
                    {
                        if (clip->hasTagName ("AudioClip"))
                            convertAudioClip (*clips, *clip);
                        else if (clip->hasTagName ("MidiClip"))
                            convertMidiClip (*clips, *clip);
                    }
                }

                if (auto* slots = child (track, "DeviceChain/MainSequencer/ClipSlotList"))
                    for (auto* slot : slots->getChildIterator())
                        if (auto* value = child (slot, "ClipSlot/Value"); value != nullptr && value->getNumChildElements() > 0)
                            ++sessionClips;
            }
        }

        // The master
        auto* liveMaster = child (liveSet, "MainTrack");

        if (liveMaster == nullptr)
            liveMaster = child (liveSet, "MasterTrack");

        auto* master = structure->createNewChildElement ("Track");
        master->setAttribute ("id", id());
        master->setAttribute ("name", "Master");
        auto* masterChannel = master->createNewChildElement ("Channel");
        masterChannel->setAttribute ("role", "master");
        addMixer (*masterChannel, liveMaster, mixerOf (liveMaster));

        converted->addChildElement (arrangement.release());
    }
};

//==============================================================================
Importer::Importer (const juce::File& f) : impl (std::make_unique<Impl>())
{
    impl->source = f;
    impl->read();
}

Importer::~Importer() = default;

juce::String Importer::getError() const
{
    return impl->error.isNotEmpty() ? impl->error : impl->inner != nullptr ? impl->inner->getError() : juce::String();
}

const juce::XmlElement* Importer::getConvertedProject() const    { return impl->convertedView; }

bool Importer::extractAudio (const juce::File& destDir, std::function<bool (double)> progress)
{
    return getError().isEmpty() && impl->inner->extractAudio (destDir, std::move (progress));
}

SessionImport::Summary Importer::build (te::Edit& edit)
{
    auto summary = impl->inner->build (edit);
    summary.skippedReturnTracks = impl->returnTracks;
    summary.notes.clear();

    if (impl->sessionClips > 0)
        summary.notes.add (juce::String (impl->sessionClips) + (impl->sessionClips == 1 ? " Session View clip was" : " Session View clips were")
                            + " left out: only the Arrangement is imported.");

    summary.notes.add ("Devices (instruments and effects), sends and automation other than tempo aren't brought across.");
    return summary;
}

} // namespace elsewhere::Ableton
