// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "Cubase.h"
#include "DawProject.h"

namespace elsewhere::Cubase
{
namespace
{
    juce::uint16 u16 (const juce::uint8* p)    { return juce::ByteOrder::bigEndianShort (p); }
    juce::uint32 u32 (const juce::uint8* p)    { return juce::ByteOrder::bigEndianInt (p); }

    juce::int64 i64 (const juce::uint8* p)     { return (juce::int64) juce::ByteOrder::bigEndianInt64 (p); }

    double f64 (const juce::uint8* p)
    {
        auto bits = juce::ByteOrder::bigEndianInt64 (p);
        double d;
        std::memcpy (&d, &bits, sizeof (d));
        return d;
    }

    float f32 (const juce::uint8* p)
    {
        auto bits = u32 (p);
        float f;
        std::memcpy (&f, &bits, sizeof (f));
        return f;
    }

    //==============================================================================
    /** One serialized object: its class and where its contents are in the archive. */
    struct Object
    {
        juce::String cls;
        int version = 0;
        size_t start = 0, body = 0, end = 0;
        std::vector<Object> kids;
    };

    /** An ARCH chunk: objects introduced by class declarations or references to them. Objects
        nest inside each other's contents among plain values, so they're found by scanning for
        headers that are consistent (a known class, a length that fits). */
    class Archive
    {
    public:
        Archive (const juce::uint8* d, size_t n) : data (d), size (n) {}

        std::vector<Object> parse (size_t from, size_t to, int depth = 0)
        {
            std::vector<Object> found;

            for (size_t p = from; p + 8 <= to && depth < 64;)
            {
                Object o;

                if (! readHeader (p, to, o))
                {
                    ++p;
                    continue;
                }

                o.kids = parse (o.body, o.end, depth + 1);
                p = o.end;
                found.push_back (std::move (o));
            }

            return found;
        }

        const juce::uint8* data;
        size_t size;

    private:
        struct Declared { juce::String name; int version; };
        std::map<size_t, Declared> classes;     // archive offset of a class's declaration

        bool readName (size_t p, size_t to, juce::String& name, size_t& next) const
        {
            if (p + 8 > to)
                return false;

            auto n = u32 (data + p + 4);

            if (n < 2 || n > 64 || p + 8 + n + 2 > to || data[p + 8 + n - 1] != 0 || ! juce::CharacterFunctions::isLetter ((char) data[p + 8]))
                return false;

            for (size_t i = 0; i + 1 < n; ++i)
                if (data[p + 8 + i] < 32 || data[p + 8 + i] > 126)
                    return false;

            name = juce::String (reinterpret_cast<const char*> (data + p + 8), n - 1);
            next = p + 8 + n;
            return true;
        }

        bool readHeader (size_t p, size_t to, Object& o)
        {
            auto q = p;
            bool hasBases = false;
            std::vector<std::pair<size_t, Declared>> bases;

            while (q + 8 <= to)
            {
                auto tag = u32 (data + q);
                juce::String name;
                size_t next = 0;

                if (tag == 0xfffffffe)
                {
                    if (! readName (q, to, name, next))
                        return false;

                    bases.push_back ({ q, { name, u16 (data + next) } });
                    hasBases = true;
                    q = next + 2;
                }
                else if (tag == 0xffffffff)
                {
                    if (! readName (q, to, name, next) || next + 6 > to)
                        return false;

                    auto length = u32 (data + next + 2);

                    if (next + 6 + length > to)
                        return false;

                    for (auto& b : bases)
                        classes[b.first] = b.second;

                    o.cls = name;
                    o.version = u16 (data + next);
                    classes[q] = { o.cls, o.version };
                    o.start = p;
                    o.body = next + 6;
                    o.end = o.body + length;
                    return true;
                }
                else if ((tag & 0x80000000) != 0 && ! hasBases)
                {
                    auto it = classes.find (tag & 0x7fffffff);

                    if (it == classes.end() || q + 8 > to)
                        return false;

                    auto length = u32 (data + q + 4);

                    if (q + 8 + length > to)
                        return false;

                    o.cls = it->second.name;
                    o.version = it->second.version;
                    o.start = p;
                    o.body = q + 8;
                    o.end = o.body + length;
                    return true;
                }
                else
                {
                    return false;
                }
            }

            return false;
        }
    };

    /** A length-prefixed string: the text stops at its NUL (Cubase 10 follows it with a BOM). */
    juce::String text (const juce::uint8* d, size_t p, size_t end, size_t* next = nullptr)
    {
        if (p + 4 > end)
            return {};

        auto n = std::min<size_t> (u32 (d + p), end - p - 4);
        size_t len = 0;

        while (len < n && d[p + 4 + len] != 0)
            ++len;

        if (next != nullptr)
            *next = p + 4 + n;

        return juce::String::fromUTF8 (reinterpret_cast<const char*> (d + p + 4), (int) len);
    }

    const Object* find (const std::vector<Object>& objects, const char* cls)
    {
        for (auto& o : objects)
        {
            if (o.cls == cls)
                return &o;

            if (auto* f = find (o.kids, cls))
                return f;
        }

        return nullptr;
    }
}

//==============================================================================
struct Importer::Impl
{
    juce::File source;
    juce::String error, appName;
    juce::MemoryBlock fileData;
    std::unique_ptr<juce::XmlElement> converted;
    const juce::XmlElement* convertedView = nullptr;
    std::unique_ptr<DawProject::Importer> inner;
    std::map<juce::String, juce::File> nearbyFiles, filesAround;
    int tempoPoints = 1, overlapsTrimmed = 0;

    struct Clip
    {
        double start, length, offset;       // seconds
        juce::String name, file, folder;
    };

    struct Track
    {
        juce::String name;
        std::vector<Clip> clips;
    };

    std::vector<Track> tracks;
    double bpm = 120.0;
    int numerator = 4, denominator = 4;

    //==============================================================================
    void read()
    {
        if (! source.loadFileAsData (fileData) || fileData.getSize() < 12)
        {
            error = "Couldn't open \"" + source.getFileName() + "\".";
            return;
        }

        auto* d = static_cast<const juce::uint8*> (fileData.getData());
        auto size = fileData.getSize();

        if (std::memcmp (d, "RIFF", 4) != 0 || std::memcmp (d + 8, "NUND", 4) != 0)
        {
            error = "\"" + source.getFileName() + "\" isn't a Cubase project, or it's damaged.";
            return;
        }

        // ROOT (name, class) and ARCH (the object) chunks, in pairs
        juce::String rootClass;
        bool foundArrangement = false;

        for (size_t p = 12; p + 8 <= size;)
        {
            auto length = (size_t) u32 (d + p + 4);
            auto body = p + 8;

            if (body + length > size)
                break;

            if (std::memcmp (d + p, "ROOT", 4) == 0 && length >= 8)
            {
                auto n = std::min<size_t> (u32 (d + body), length - 4);
                auto m = body + 4 + n + 4 <= body + length ? std::min<size_t> (u32 (d + body + 4 + n), length - 8 - n) : 0;
                rootClass = juce::String (reinterpret_cast<const char*> (d + body + 8 + n), m);
            }
            else if (std::memcmp (d + p, "ARCH", 4) == 0)
            {
                if (rootClass == "PAppVersion")
                    readVersion (d + body, length);
                else if (rootClass == "PArrangement" && ! foundArrangement)
                {
                    readArrangement (d + body, length);
                    foundArrangement = true;
                }
            }

            p = body + length;
        }

        if (! foundArrangement)
        {
            error = "\"" + source.getFileName() + "\" has no arrangement Elsewhere can read.";
            return;
        }

        convert();
        convertedView = converted.get();
        inner = std::make_unique<DawProject::Importer> (std::move (converted), source.getParentDirectory());
    }

    void readVersion (const juce::uint8* d, size_t size)
    {
        Archive archive (d, size);
        auto objects = archive.parse (0, size);

        if (auto* v = find (objects, "PAppVersion"))
        {
            size_t next = 0;
            auto app = text (d, v->body, v->end, &next);
            auto version = text (d, next, v->end);
            appName = (app + " " + version.fromFirstOccurrenceOf ("Version ", false, false)).trim();
        }
    }

    void readArrangement (const juce::uint8* d, size_t size)
    {
        Archive archive (d, size);
        auto objects = archive.parse (0, size);

        // What references point at: the offset of an object's length field
        std::map<size_t, const Object*> byRef;
        std::function<void (const std::vector<Object>&)> index = [&] (const std::vector<Object>& list)
        {
            for (auto& o : list)
            {
                byRef[o.body - 4] = &o;
                index (o.kids);
            }
        };
        index (objects);

        struct Raw { double start, length, offset; const Object* clip; };
        struct RawTrack { juce::String name; bool linear = false; std::vector<Raw> events; };
        std::vector<RawTrack> raw;

        std::function<void (const std::vector<Object>&, RawTrack*)> walk = [&] (const std::vector<Object>& list, RawTrack* track)
        {
            for (auto& o : list)
            {
                auto* t = track;

                if (o.cls == "MAudioTrackEvent")
                {
                    raw.push_back ({});
                    t = &raw.back();
                }
                else if (o.cls == "MListNode" && t != nullptr && t->name.isEmpty())
                {
                    size_t next = 0;
                    t->name = text (d, o.body, o.end, &next);

                    // Linear timebase (seconds) rather than musical (ticks)
                    t->linear = next + 4 <= o.end && u32 (d + next) == 1;

                    if (t->name.isEmpty())
                        t->name = " ";
                }
                else if (o.cls == "MTempoTrackEvent" && o.end - o.body >= 30)
                {
                    tempoPoints = (int) u32 (d + o.body);
                    auto value = (double) f32 (d + o.body + 26);

                    if (value >= 20.0 && value <= 999.0)
                        bpm = value;
                }
                else if (o.cls == "MSignatureTrackEvent" && o.end - o.body >= 12)
                {
                    auto num = u16 (d + o.body + 8), den = u16 (d + o.body + 10);

                    if (num >= 1 && num <= 32 && (den == 1 || den == 2 || den == 4 || den == 8 || den == 16 || den == 32))
                    {
                        numerator = num;
                        denominator = den;
                    }
                }
                else if (o.cls == "MAudioEvent" && t != nullptr && o.end - o.body >= 30)
                {
                    // The clip follows the start, length and offset: written out, or a
                    // reference to one written before
                    const Object* clip = nullptr;

                    for (auto& k : o.kids)
                        if (k.cls == "PAudioClip")
                            clip = &k;

                    if (clip == nullptr)
                        if (auto it = byRef.find (u32 (d + o.body + 26)); it != byRef.end() && it->second->cls == "PAudioClip")
                            clip = it->second;

                    t->events.push_back ({ f64 (d + o.body + 2), f64 (d + o.body + 10), f64 (d + o.body + 18), clip });
                }

                walk (o.kids, t);
            }
        };
        walk (objects, nullptr);

        auto secondsPerTick = 60.0 / bpm / 480.0;

        // An audio file object: its name and folder (in an FNPath inside it, or one it refers
        // to) and its sample rate
        struct FileInfo { juce::String file, folder; double rate = 44100.0; };

        auto pathInfo = [&] (const Object& path, FileInfo& info)
        {
            info.file = text (d, path.body, path.end);

            // The folder it was recorded in is the string that starts with a slash (or C:)
            for (auto p = path.body; p + 5 < path.end; ++p)
            {
                if (d[p + 4] == '/' || (d[p + 5] == ':' && juce::CharacterFunctions::isLetter ((char) d[p + 4])))
                {
                    auto n = u32 (d + p);

                    if (n > 1 && p + 4 + n <= path.end)
                    {
                        info.folder = text (d, p, path.end);
                        break;
                    }
                }
            }
        };

        auto fileInfo = [&] (const Object& audioFile)
        {
            FileInfo info;

            if (audioFile.end - audioFile.body >= 18)
                if (auto r = (double) f32 (d + audioFile.body + 14); r >= 8000.0 && r <= 384000.0)
                    info.rate = r;

            if (auto* path = find (audioFile.kids, "FNPath"))
                pathInfo (*path, info);
            else if (audioFile.end - audioFile.body >= 22)
                if (auto it = byRef.find (u32 (d + audioFile.body + 18)); it != byRef.end() && it->second->cls == "FNPath")
                    pathInfo (*it->second, info);

            return info;
        };

        for (auto& rt : raw)
        {
            Track t;
            t.name = rt.name;

            for (auto& e : rt.events)
            {
                if (e.clip == nullptr)
                    continue;

                auto clipName = text (d, e.clip->body, e.clip->end);

                // A clip is made of segments: parts of audio files placed along it (several when
                // part of it was processed, as the processed audio goes in a file of its own).
                // The clip's own AudioCluster, not the ones in its edit history.
                struct Segment { FileInfo file; double fileStart, length, at; };
                std::vector<Segment> segments;

                for (auto& k : e.clip->kids)
                {
                    if (k.cls != "AudioCluster")
                        continue;

                    for (auto& seg : k.kids)
                    {
                        if (seg.cls != "AClusterSegment" || seg.end - seg.body < 28)
                            continue;

                        auto it = byRef.find (u32 (d + seg.body));

                        if (it == byRef.end() || it->second->cls != "AudioFile")
                            continue;

                        segments.push_back ({ fileInfo (*it->second), (double) i64 (d + seg.body + 4),
                                              (double) i64 (d + seg.body + 12), (double) i64 (d + seg.body + 20) });
                    }

                    break;
                }

                // Older or simpler clips: one file, the whole clip
                if (segments.empty())
                {
                    Segment whole { {}, 0.0, 1.0e18, 0.0 };

                    if (auto* audio = find (e.clip->kids, "AudioFile"))
                        whole.file = fileInfo (*audio);

                    if (whole.file.file.isEmpty())
                        if (auto* path = find (e.clip->kids, "FNPath"))
                            pathInfo (*path, whole.file);

                    segments.push_back (whole);
                }

                // The event plays the clip from its offset, for its length (in the clip's samples)
                auto start = rt.linear ? e.start : e.start * secondsPerTick;

                for (auto& seg : segments)
                {
                    auto from = std::max (e.offset, seg.at), to = std::min (e.offset + e.length, seg.at + seg.length);

                    if (to <= from || seg.file.file.isEmpty())
                        continue;

                    auto rate = seg.file.rate;
                    Clip c;
                    c.name = clipName;
                    c.file = seg.file.file;
                    c.folder = seg.file.folder;
                    c.start = start + (from - e.offset) / rate;
                    c.length = (to - from) / rate;
                    c.offset = (seg.fileStart + (from - seg.at)) / rate;
                    t.clips.push_back (c);
                }
            }

            tracks.push_back (removeHiddenParts (t));
        }
    }

    /** Where events overlap, the later one plays, as in Cubase: cut what it covers out of the
        earlier ones. */
    Track removeHiddenParts (const Track& track)
    {
        Track result { track.name, {} };

        for (auto& clip : track.clips)
        {
            std::vector<Clip> kept;
            auto s = clip.start, e = clip.start + clip.length;

            for (auto& c : result.clips)
            {
                auto cs = c.start, ce = c.start + c.length;

                if (ce <= s + 1.0e-6 || cs >= e - 1.0e-6)
                {
                    kept.push_back (c);
                    continue;
                }

                ++overlapsTrimmed;

                if (cs < s)
                {
                    auto left = c;
                    left.length = s - cs;
                    kept.push_back (left);
                }

                if (ce > e)
                {
                    auto right = c;
                    right.start = e;
                    right.offset = c.offset + (e - cs);
                    right.length = ce - e;
                    kept.push_back (right);
                }
            }

            kept.push_back (clip);
            result.clips = std::move (kept);
        }

        std::sort (result.clips.begin(), result.clips.end(), [] (auto& a, auto& b) { return a.start < b.start; });
        return result;
    }

    //==============================================================================
    juce::File findAudio (const Clip& c)
    {
        auto folder = source.getParentDirectory();

        for (auto f : { folder.getChildFile ("Audio").getChildFile (c.file), folder.getChildFile (c.file) })
            if (f.existsAsFile())
                return f;

        if (c.folder.isNotEmpty() && juce::File::isAbsolutePath (c.folder))
            if (auto f = juce::File (c.folder).getChildFile (c.file); f.existsAsFile())
                return f;

        if (auto f = SessionImport::findFileNear (folder, c.file, nearbyFiles); f.existsAsFile())
            return f;

        return SessionImport::findFileNear (folder.getParentDirectory(), c.file, filesAround);
    }

    void convert()
    {
        converted = std::make_unique<juce::XmlElement> ("Project");
        converted->setAttribute ("version", "1.0");
        converted->createNewChildElement ("Application")->setAttribute ("name", appName.isNotEmpty() ? appName : "Cubase");

        auto* transport = converted->createNewChildElement ("Transport");
        auto* tempo = transport->createNewChildElement ("Tempo");
        tempo->setAttribute ("unit", "bpm");
        tempo->setAttribute ("value", bpm);
        auto* sig = transport->createNewChildElement ("TimeSignature");
        sig->setAttribute ("numerator", numerator);
        sig->setAttribute ("denominator", denominator);

        auto* structure = converted->createNewChildElement ("Structure");
        auto* arrangement = converted->createNewChildElement ("Arrangement");

        // Clips are placed in seconds: exact for both timebases
        auto* lanes = arrangement->createNewChildElement ("Lanes");
        lanes->setAttribute ("timeUnit", "seconds");
        int number = 0;

        for (auto& t : tracks)
        {
            auto id = "c" + juce::String (++number);
            auto* track = structure->createNewChildElement ("Track");
            track->setAttribute ("id", id);
            track->setAttribute ("name", t.name.trim().isNotEmpty() ? t.name.trim() : "Audio " + juce::String (number));
            track->setAttribute ("contentType", "audio");
            track->createNewChildElement ("Channel")->setAttribute ("role", "regular");

            auto* trackLanes = lanes->createNewChildElement ("Lanes");
            trackLanes->setAttribute ("track", id);
            auto* clips = trackLanes->createNewChildElement ("Clips");

            for (auto& c : t.clips)
            {
                auto file = findAudio (c);

                auto* clip = clips->createNewChildElement ("Clip");
                clip->setAttribute ("time", c.start);
                clip->setAttribute ("duration", c.length);
                clip->setAttribute ("contentTimeUnit", "seconds");
                clip->setAttribute ("playStart", c.offset);
                clip->setAttribute ("name", c.name);

                auto* f = clip->createNewChildElement ("Audio")->createNewChildElement ("File");
                f->setAttribute ("path", file.existsAsFile() ? file.getFullPathName() : c.file);
                f->setAttribute ("external", "true");
            }
        }
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
    summary.notes.clear();

    if (impl->tempoPoints > 1)
        summary.notes.add ("This project has tempo changes, which Elsewhere can't read from Cubase files yet: it starts at "
                            + juce::String (impl->bpm, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".")
                            + " BPM, and clips after a tempo change may be out of place.");

    if (impl->overlapsTrimmed > 0)
        summary.notes.add ("Where clips overlapped, the later one plays (as in Cubase).");

    summary.notes.add ("Volume, pan, mute, fades, plug-ins and MIDI aren't brought across from Cubase projects yet.");
    return summary;
}

} // namespace elsewhere::Cubase
