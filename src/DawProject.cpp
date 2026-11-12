// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "DawProject.h"
#include "Mixing.h"

namespace elsewhere::DawProject
{
namespace
{
    constexpr double inf = std::numeric_limits<double>::infinity();

    //==============================================================================
    // A small zip reader/writer. juce::ZipFile can't handle archives over 4 GB (no zip64),
    // and a project's worth of 24-bit stems gets there easily. Entries are stored or deflated.
    juce::uint32 crc32 (juce::uint32 crc, const void* data, size_t size)
    {
        static const auto table = []
        {
            std::array<juce::uint32, 256> t {};

            for (juce::uint32 i = 0; i < 256; ++i)
            {
                auto c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1) ? 0xedb88320u ^ (c >> 1) : c >> 1;
                t[i] = c;
            }

            return t;
        }();

        crc = ~crc;
        auto* p = static_cast<const juce::uint8*> (data);

        for (size_t i = 0; i < size; ++i)
            crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);

        return ~crc;
    }

    class ZipReader
    {
    public:
        struct Entry
        {
            juce::String name;
            int method = 0;
            juce::int64 compressedSize = 0, size = 0, headerOffset = 0;
        };

        explicit ZipReader (const juce::File& f) : file (f)
        {
            juce::FileInputStream in (file);

            if (in.openedOk())
                readDirectory (in);
        }

        const std::vector<Entry>& getEntries() const    { return entries; }

        const Entry* find (juce::String path) const
        {
            path = path.replaceCharacter ('\\', '/').trimCharactersAtStart ("/");

            if (path.startsWith ("./"))
                path = path.substring (2);

            for (auto& e : entries)
                if (e.name.equalsIgnoreCase (path))
                    return &e;

            return nullptr;
        }

        std::unique_ptr<juce::InputStream> open (const Entry& e) const
        {
            auto in = std::make_unique<juce::FileInputStream> (file);
            juce::uint8 header[30];

            if (! in->openedOk() || ! in->setPosition (e.headerOffset) || in->read (header, 30) != 30
                 || juce::ByteOrder::littleEndianInt (header) != 0x04034b50)
                return {};

            auto dataStart = e.headerOffset + 30 + juce::ByteOrder::littleEndianShort (header + 26)
                                                 + juce::ByteOrder::littleEndianShort (header + 28);

            auto region = std::make_unique<juce::SubregionStream> (in.release(), dataStart, e.compressedSize, true);

            if (e.method == 0)
                return region;

            if (e.method == 8)
                return std::make_unique<juce::GZIPDecompressorInputStream> (region.release(), true,
                                                                            juce::GZIPDecompressorInputStream::deflateFormat, e.size);
            return {};
        }

    private:
        static juce::uint64 u64 (const juce::uint8* p)  { return juce::ByteOrder::littleEndianInt64 (p); }
        static juce::uint32 u32 (const juce::uint8* p)  { return juce::ByteOrder::littleEndianInt (p); }
        static juce::uint16 u16 (const juce::uint8* p)  { return juce::ByteOrder::littleEndianShort (p); }

        void readDirectory (juce::FileInputStream& in)
        {
            // The end-of-directory record sits in the last 64 KB (it can be followed by a comment)
            auto total = in.getTotalLength();
            auto tailSize = (int) std::min<juce::int64> (total, 65536 + 22);
            juce::HeapBlock<juce::uint8> tail ((size_t) tailSize);
            in.setPosition (total - tailSize);

            if (in.read (tail, tailSize) != tailSize)
                return;

            int eocd = -1;
            for (int i = tailSize - 22; i >= 0; --i)
                if (u32 (tail + i) == 0x06054b50) { eocd = i; break; }

            if (eocd < 0)
                return;

            juce::uint64 count = u16 (tail + eocd + 10), dirOffset = u32 (tail + eocd + 16);

            // zip64: a locator just before the record points at the 64-bit version
            if (eocd >= 20 && u32 (tail + eocd - 20) == 0x07064b50)
            {
                juce::uint8 z[56];
                in.setPosition ((juce::int64) u64 (tail + eocd - 20 + 8));

                if (in.read (z, 56) == 56 && u32 (z) == 0x06064b50)
                {
                    count = u64 (z + 32);
                    dirOffset = u64 (z + 48);
                }
            }

            in.setPosition ((juce::int64) dirOffset);

            for (juce::uint64 i = 0; i < count; ++i)
            {
                juce::uint8 h[46];

                if (in.read (h, 46) != 46 || u32 (h) != 0x02014b50)
                    return;

                auto nameLen = u16 (h + 28), extraLen = u16 (h + 30), commentLen = u16 (h + 32);
                juce::MemoryBlock name (nameLen), extra (extraLen);
                in.read (name.getData(), nameLen);
                in.read (extra.getData(), extraLen);
                in.skipNextBytes (commentLen);

                Entry e;
                e.name = juce::String::fromUTF8 (static_cast<const char*> (name.getData()), (int) nameLen);
                e.method = u16 (h + 10);
                juce::uint64 compressed = u32 (h + 20), size = u32 (h + 24), offset = u32 (h + 42);

                // zip64 extra field: the values that didn't fit, in this order
                auto* x = static_cast<const juce::uint8*> (extra.getData());
                for (size_t p = 0; p + 4 <= extraLen;)
                {
                    auto id = u16 (x + p), len = u16 (x + p + 2);
                    auto* v = x + p + 4;

                    if (id == 1)
                    {
                        if (size == 0xffffffff)         { size = u64 (v); v += 8; }
                        if (compressed == 0xffffffff)   { compressed = u64 (v); v += 8; }
                        if (offset == 0xffffffff)       { offset = u64 (v); }
                    }

                    p += 4 + len;
                }

                e.compressedSize = (juce::int64) compressed;
                e.size = (juce::int64) size;
                e.headerOffset = (juce::int64) offset;

                if (! e.name.endsWithChar ('/'))
                    entries.push_back (e);
            }
        }

        juce::File file;
        std::vector<Entry> entries;
    };

    class ZipWriter
    {
    public:
        explicit ZipWriter (juce::FileOutputStream& o) : out (o) {}

        bool addText (const juce::String& name, const juce::String& text)
        {
            juce::MemoryOutputStream deflated;
            {
                juce::GZIPCompressorOutputStream gz (deflated, 9, juce::GZIPCompressorOutputStream::windowBitsRaw);
                gz.write (text.toRawUTF8(), text.getNumBytesAsUTF8());
            }

            Entry e { name, crc32 (0, text.toRawUTF8(), text.getNumBytesAsUTF8()), 8,
                      (juce::uint64) deflated.getDataSize(), (juce::uint64) text.getNumBytesAsUTF8(), (juce::uint64) out.getPosition() };
            writeLocalHeader (e);
            out.write (deflated.getData(), deflated.getDataSize());
            entries.push_back (e);
            return ! out.getStatus().failed();
        }

        /** Stores a file uncompressed (audio doesn't deflate well). Reads it twice: once for the
            checksum, then to copy it, so it never has to fit in memory. */
        bool addFile (const juce::String& name, const juce::File& f, const std::function<bool (juce::int64)>& bytesDone)
        {
            juce::HeapBlock<char> buffer (bufferSize);
            juce::uint32 crc = 0;
            juce::uint64 size = 0;

            {
                juce::FileInputStream in (f);
                if (! in.openedOk())
                    return false;

                for (int n; (n = in.read (buffer, bufferSize)) > 0;)
                {
                    crc = crc32 (crc, buffer, (size_t) n);
                    size += (juce::uint64) n;
                }
            }

            Entry e { name, crc, 0, size, size, (juce::uint64) out.getPosition() };
            writeLocalHeader (e);

            juce::FileInputStream in (f);
            if (! in.openedOk())
                return false;

            for (int n; (n = in.read (buffer, bufferSize)) > 0;)
            {
                if (! out.write (buffer, (size_t) n))
                    return false;

                if (bytesDone && ! bytesDone (n))
                    return false;
            }

            entries.push_back (e);
            return true;
        }

        bool finish()
        {
            auto dirStart = (juce::uint64) out.getPosition();

            for (auto& e : entries)
            {
                juce::MemoryOutputStream extra;
                if (e.size >= 0xffffffff)       extra.writeInt64 ((juce::int64) e.size);
                if (e.compressed >= 0xffffffff) extra.writeInt64 ((juce::int64) e.compressed);
                if (e.offset >= 0xffffffff)     extra.writeInt64 ((juce::int64) e.offset);

                auto name = e.name.toUTF8();
                auto nameLen = (int) name.sizeInBytes() - 1;
                bool z64 = extra.getDataSize() > 0;

                out.writeInt (0x02014b50);
                out.writeShort (z64 ? 45 : 20);    // made by
                out.writeShort (z64 ? 45 : 20);    // needed to extract
                out.writeShort (1 << 11);          // UTF-8 names
                out.writeShort ((short) e.method);
                writeTime();
                out.writeInt ((int) e.crc);
                out.writeInt ((int) clamp32 (e.compressed));
                out.writeInt ((int) clamp32 (e.size));
                out.writeShort ((short) nameLen);
                out.writeShort ((short) (z64 ? extra.getDataSize() + 4 : 0));
                out.writeShort (0);                // comment
                out.writeShort (0);                // disk
                out.writeShort (0);                // internal attributes
                out.writeInt (0);                  // external attributes
                out.writeInt ((int) clamp32 (e.offset));
                out.write (name.getAddress(), (size_t) nameLen);

                if (z64)
                {
                    out.writeShort (1);
                    out.writeShort ((short) extra.getDataSize());
                    out << extra;
                }
            }

            auto dirEnd = (juce::uint64) out.getPosition();
            auto dirSize = dirEnd - dirStart;

            if (entries.size() >= 0xffff || dirStart >= 0xffffffff || dirSize >= 0xffffffff)
            {
                out.writeInt (0x06064b50);
                out.writeInt64 (44);
                out.writeShort (45);
                out.writeShort (45);
                out.writeInt (0);
                out.writeInt (0);
                out.writeInt64 ((juce::int64) entries.size());
                out.writeInt64 ((juce::int64) entries.size());
                out.writeInt64 ((juce::int64) dirSize);
                out.writeInt64 ((juce::int64) dirStart);

                out.writeInt (0x07064b50);
                out.writeInt (0);
                out.writeInt64 ((juce::int64) dirEnd);
                out.writeInt (1);
            }

            out.writeInt (0x06054b50);
            out.writeShort (0);
            out.writeShort (0);
            out.writeShort ((short) std::min<size_t> (entries.size(), 0xffff));
            out.writeShort ((short) std::min<size_t> (entries.size(), 0xffff));
            out.writeInt ((int) clamp32 (dirSize));
            out.writeInt ((int) clamp32 (dirStart));
            out.writeShort (0);
            out.flush();
            return ! out.getStatus().failed();
        }

    private:
        struct Entry
        {
            juce::String name;
            juce::uint32 crc;
            int method;
            juce::uint64 compressed, size, offset;
        };

        static constexpr int bufferSize = 1 << 20;

        static juce::uint32 clamp32 (juce::uint64 v)    { return v >= 0xffffffff ? 0xffffffff : (juce::uint32) v; }

        void writeTime()
        {
            auto t = juce::Time::getCurrentTime();
            out.writeShort ((short) ((t.getSeconds() / 2) + (t.getMinutes() << 5) + (t.getHours() << 11)));
            out.writeShort ((short) (t.getDayOfMonth() + ((t.getMonth() + 1) << 5) + ((t.getYear() - 1980) << 9)));
        }

        void writeLocalHeader (const Entry& e)
        {
            auto name = e.name.toUTF8();
            auto nameLen = (int) name.sizeInBytes() - 1;
            bool z64 = e.size >= 0xffffffff;

            out.writeInt (0x04034b50);
            out.writeShort (z64 ? 45 : 20);
            out.writeShort (1 << 11);
            out.writeShort ((short) e.method);
            writeTime();
            out.writeInt ((int) e.crc);
            out.writeInt ((int) (z64 ? 0xffffffff : e.compressed));
            out.writeInt ((int) (z64 ? 0xffffffff : e.size));
            out.writeShort ((short) nameLen);
            out.writeShort (z64 ? 20 : 0);
            out.write (name.getAddress(), (size_t) nameLen);

            if (z64)
            {
                out.writeShort (1);
                out.writeShort (16);
                out.writeInt64 ((juce::int64) e.size);
                out.writeInt64 ((juce::int64) e.compressed);
            }
        }

        juce::FileOutputStream& out;
        std::vector<Entry> entries;
    };

    //==============================================================================
    enum class Unit { beats, seconds };

    double num (const juce::XmlElement& e, const char* name, double def = 0.0)
    {
        auto s = e.getStringAttribute (name).trim();
        return s.isEmpty() ? def : s.getDoubleValue();
    }

    bool flag (const juce::XmlElement& e, const char* name, bool def)
    {
        auto s = e.getStringAttribute (name).trim();
        return s.isEmpty() ? def : (s.equalsIgnoreCase ("true") || s == "1");
    }

    Unit unitOf (const juce::XmlElement& e, const char* name, Unit inherited)
    {
        auto s = e.getStringAttribute (name);
        return s == "seconds" ? Unit::seconds : s == "beats" ? Unit::beats : inherited;
    }

    std::optional<juce::Colour> parseColour (juce::String s)
    {
        s = s.trim().trimCharactersAtStart ("#");

        if (s.length() != 6 && s.length() != 8)
            return {};

        auto v = (juce::uint32) s.getHexValue64();

        if (s.length() == 8)
            return juce::Colour ((juce::uint8) (v >> 24), (juce::uint8) (v >> 16), (juce::uint8) (v >> 8), (juce::uint8) v);

        return juce::Colour ((juce::uint8) (v >> 16), (juce::uint8) (v >> 8), (juce::uint8) v);
    }

    juce::String colourString (juce::Colour c)
    {
        return "#" + juce::String::toHexString ((int) (c.getARGB() & 0xffffff)).paddedLeft ('0', 6);
    }

    /** A Volume parameter as dB. Exporters write linear gain (Bitwig, Studio One) or dB. */
    float volumeDb (const juce::XmlElement& v)
    {
        auto value = num (v, "value", 1.0);

        if (v.getStringAttribute ("unit") == "decibel")
            return (float) value;

        return juce::Decibels::gainToDecibels ((float) value, -100.0f);
    }

    /** A Pan parameter as -1..1. Usually normalized 0..1 with 0.5 in the middle. */
    float panValue (const juce::XmlElement& p)
    {
        auto value = num (p, "value", 0.5);
        auto lo = num (p, "min", 0.0), hi = num (p, "max", 1.0);

        if (p.getStringAttribute ("unit") != "normalized" && p.hasAttribute ("min") && p.hasAttribute ("max") && hi > lo)
            return (float) juce::jlimit (-1.0, 1.0, 2.0 * (value - lo) / (hi - lo) - 1.0);

        if (p.getStringAttribute ("unit") == "normalized" || lo >= 0.0)
            return (float) juce::jlimit (-1.0, 1.0, value * 2.0 - 1.0);

        return (float) juce::jlimit (-1.0, 1.0, value);
    }

    juce::String archiveNameFor (const juce::String& path)
    {
        return juce::File::createLegalFileName (path.replaceCharacter ('\\', '/').fromLastOccurrenceOf ("/", false, false));
    }
}

//==============================================================================
struct Importer::Impl
{
    juce::File source;
    std::unique_ptr<ZipReader> zip;
    std::unique_ptr<juce::XmlElement> project;
    juce::String error;
    std::map<juce::String, juce::File> localFiles;     // <File path="..."> -> its copy in the Elsewhere project

    //==============================================================================
    // Building. Positions are held in the arrangement's own time unit (beats, usually) and only
    // turned into seconds at the end, via the tempo map, so tempo changes land exactly.
    struct Affine
    {
        double a = 1.0, b = 0.0;
        double operator() (double x) const      { return a * x + b; }
        double inverse (double g) const         { return (g - b) / a; }
    };

    struct Context
    {
        Unit unit = Unit::beats;                 // unit of times inside the current element
        Affine map;                              // local time -> arrangement time
        double w0 = -inf, w1 = inf;              // visible window, arrangement time
        te::AudioTrack* track = nullptr;
        juce::String name;
        bool disabled = false;
        double gain = 1.0, transpose = 0.0;     // Elsewhere's own clip attributes (from converted formats)
        double fadeInAt = inf, fadeInLength = 0, fadeOutAt = inf, fadeOutLength = 0;   // at: arrangement time, length: seconds
    };

    struct Piece
    {
        te::AudioTrack* track;
        juce::File file;
        double t0, t1, s0, s1;                   // timeline seconds -> source seconds
        bool stretch, disabled;
        double gain = 1.0, transpose = 0.0;
        juce::String name;
        double fadeIn = 0, fadeOut = 0;
    };

    te::Edit* edit = nullptr;
    Unit arrangementUnit = Unit::beats;
    std::map<juce::String, te::Track*> trackForId;
    std::vector<double> tempoChanges;            // arrangement time; audio warped in beats is split at these
    std::vector<Piece> pieces;
    std::set<te::AudioTrack*> midiTracks;
    juce::Array<juce::File> usedFiles;
    ImportSummary summary;

    te::TempoSequence& tempo() const            { return edit->tempoSequence; }

    double toSeconds (double g) const
    {
        return arrangementUnit == Unit::seconds ? g : tempo().toTime (te::BeatPosition::fromBeats (g)).inSeconds();
    }

    double toBeats (double g) const
    {
        return arrangementUnit == Unit::beats ? g : tempo().toBeats (te::TimePosition::fromSeconds (g)).inBeats();
    }

    double bpmAt (double g) const
    {
        return tempo().getBpmAt (te::TimePosition::fromSeconds (std::max (0.0, toSeconds (g))));
    }

    /** Arrangement units per one unit of u, around arrangement time g. */
    double scaleOf (Unit u, double g) const
    {
        if (u == arrangementUnit)
            return 1.0;

        return u == Unit::seconds ? bpmAt (g) / 60.0 : 60.0 / bpmAt (g);
    }

    //==============================================================================
    void buildTempo()
    {
        auto& ts = tempo();
        double minBpm = 20.0, maxBpm = 999.0;

        if (auto* transport = project->getChildByName ("Transport"))
        {
            if (auto* t = transport->getChildByName ("Tempo"))
            {
                minBpm = num (*t, "min", minBpm);
                maxBpm = num (*t, "max", maxBpm);
                ts.getTempo (0)->setBpm (juce::jlimit (20.0, 999.0, num (*t, "value", 120.0)));
            }

            if (auto* sig = transport->getChildByName ("TimeSignature"))
                ts.getTimeSig (0)->setStringTimeSig (juce::String ((int) num (*sig, "numerator", 4)) + "/"
                                                      + juce::String ((int) num (*sig, "denominator", 4)));
        }

        auto* arrangement = project->getChildByName ("Arrangement");

        if (arrangement == nullptr)
            return;

        if (auto* automation = arrangement->getChildByName ("TempoAutomation"))
        {
            auto unit = unitOf (*automation, "timeUnit", Unit::beats);
            bool normalised = automation->getStringAttribute ("unit") == "normalized";

            std::vector<const juce::XmlElement*> points;
            for (auto* p : automation->getChildWithTagNameIterator ("RealPoint"))
                points.push_back (p);

            std::sort (points.begin(), points.end(), [] (auto* a, auto* b) { return num (*a, "time") < num (*b, "time"); });

            for (auto* p : points)
            {
                auto value = num (*p, "value", 120.0);
                auto bpm = juce::jlimit (20.0, 999.0, normalised ? minBpm + value * (maxBpm - minBpm) : value);
                auto time = num (*p, "time");
                auto beat = unit == Unit::beats ? time : ts.toBeats (te::TimePosition::fromSeconds (time)).inBeats();

                // The point's interpolation says how it moves on to the next one. Tracktion: 1 jumps, 0 ramps.
                auto curve = p->getStringAttribute ("interpolation") == "linear" ? 0.0f : 1.0f;

                if (beat < 1.0e-6)
                {
                    ts.getTempo (0)->setBpm (bpm);
                    ts.getTempo (0)->setCurve (curve);
                }
                else
                {
                    ts.insertTempo (te::BeatPosition::fromBeats (beat), bpm, curve);
                }
            }
        }

        if (auto* automation = arrangement->getChildByName ("TimeSignatureAutomation"))
        {
            auto unit = unitOf (*automation, "timeUnit", Unit::beats);

            for (auto* p : automation->getChildWithTagNameIterator ("TimeSignaturePoint"))
            {
                auto time = num (*p, "time");
                auto beat = unit == Unit::beats ? time : ts.toBeats (te::TimePosition::fromSeconds (time)).inBeats();
                auto sig = juce::String ((int) num (*p, "numerator", 4)) + "/" + juce::String ((int) num (*p, "denominator", 4));

                if (beat < 1.0e-6)
                    ts.getTimeSig (0)->setStringTimeSig (sig);
                else if (auto s = ts.insertTimeSig (te::BeatPosition::fromBeats (beat)))
                    s->setStringTimeSig (sig);
            }
        }
    }

    void findTempoChanges()
    {
        auto& ts = tempo();
        auto toArrangement = [this, &ts] (double beat)
        {
            return arrangementUnit == Unit::beats ? beat : ts.toTime (te::BeatPosition::fromBeats (beat)).inSeconds();
        };

        for (int i = 1; i < ts.getNumTempos(); ++i)
            tempoChanges.push_back (toArrangement (ts.getTempo (i)->getStartBeat().inBeats()));

        // Ramps: split every beat, so stretched audio follows the curve closely
        for (int i = 0; i + 1 < ts.getNumTempos(); ++i)
        {
            auto* t = ts.getTempo (i);

            if (std::abs (std::abs (t->getCurve()) - 1.0f) > 1.0e-4f)
                for (auto b = std::floor (t->getStartBeat().inBeats()) + 1.0; b < ts.getTempo (i + 1)->getStartBeat().inBeats(); b += 1.0)
                    tempoChanges.push_back (toArrangement (b));
        }

        std::sort (tempoChanges.begin(), tempoChanges.end());
    }

    //==============================================================================
    void applyChannel (const juce::XmlElement& channel, te::VolumeAndPanPlugin* volume, te::Track* track)
    {
        if (volume != nullptr)
        {
            if (auto* v = channel.getChildByName ("Volume"))
                volume->setVolumeDb (volumeDb (*v));

            if (auto* p = channel.getChildByName ("Pan"))
                volume->setPan (panValue (*p));
        }

        if (track != nullptr)
        {
            if (auto* m = channel.getChildByName ("Mute"))
                track->setMute (flag (*m, "value", false));

            track->setSolo (flag (channel, "solo", false));
        }
    }

    void buildTracks (const juce::XmlElement& parent, te::FolderTrack* folder)
    {
        te::Track* preceding = nullptr;

        for (auto* e : parent.getChildIterator())
        {
            // Some exporters put the master channel straight into the Structure
            if (e->hasTagName ("Channel"))
            {
                if (e->getStringAttribute ("role") == "master")
                    applyChannel (*e, edit->getMasterVolumePlugin().get(), nullptr);

                continue;
            }

            if (! e->hasTagName ("Track"))
                continue;

            auto* channel = e->getChildByName ("Channel");
            auto role = channel != nullptr ? channel->getStringAttribute ("role") : juce::String();

            if (role == "master")
            {
                applyChannel (*channel, edit->getMasterVolumePlugin().get(), nullptr);
                continue;
            }

            if (role == "effect")
            {
                ++summary.skippedReturnTracks;
                continue;
            }

            bool isGroup = e->getStringAttribute ("contentType").contains ("tracks") || e->getChildByName ("Track") != nullptr;
            te::Track* track = nullptr;
            te::VolumeAndPanPlugin* volume = nullptr;
            te::TrackInsertPoint where (folder, preceding);

            if (isGroup)
            {
                if (auto ft = edit->insertNewFolderTrack (where, nullptr, true))
                {
                    track = ft.get();
                    volume = ft->getVolumePlugin();
                }
            }
            else if (auto at = edit->insertNewAudioTrack (where, nullptr))
            {
                track = at.get();
                volume = at->getVolumePlugin();
                ++summary.tracks;
            }

            if (track == nullptr)
                continue;

            preceding = track;
            track->setName (e->getStringAttribute ("name", isGroup ? "Group" : "Track"));

            if (auto c = parseColour (e->getStringAttribute ("color")))
                track->setColour (*c);
            else
                track->setColour (EngineHelpers::trackColourFor (te::getAllTracks (*edit).indexOf (track)));

            if (channel != nullptr)
                applyChannel (*channel, volume, track);

            trackForId[e->getStringAttribute ("id")] = track;

            if (channel != nullptr && channel->getStringAttribute ("id").isNotEmpty())
                trackForId[channel->getStringAttribute ("id")] = track;

            if (isGroup)
                buildTracks (*e, dynamic_cast<te::FolderTrack*> (track));
        }
    }

    //==============================================================================
    void walkChildren (const juce::XmlElement& e, const Context& ctx)
    {
        for (auto* child : e.getChildIterator())
            walk (*child, ctx);
    }

    void walk (const juce::XmlElement& e, Context ctx)
    {
        ctx.unit = unitOf (e, "timeUnit", ctx.unit);

        if (e.hasTagName ("Lanes"))
        {
            if (auto id = e.getStringAttribute ("track"); id.isNotEmpty())
            {
                auto it = trackForId.find (id);
                ctx.track = it != trackForId.end() ? dynamic_cast<te::AudioTrack*> (it->second) : nullptr;

                if (ctx.track == nullptr)
                    return;     // a return, the master or a group: nothing to place
            }

            walkChildren (e, ctx);
        }
        else if (e.hasTagName ("Clips"))
        {
            walkChildren (e, ctx);
        }
        else if (e.hasTagName ("Clip"))     walkClip (e, ctx);
        else if (e.hasTagName ("Warps"))    addWarps (e, ctx);
        else if (e.hasTagName ("Audio"))    addAudio (e, ctx, {});
        else if (e.hasTagName ("Notes"))    addNotes (e, ctx);
        // Points (automation), Markers and Video aren't imported
    }

    /** A clip shows a window of its content, optionally looped. Its content can be more clips. */
    void walkClip (const juce::XmlElement& clip, Context ctx)
    {
        if (! flag (clip, "enable", true))
            ctx.disabled = true;

        ctx.gain *= num (clip, "elsewhereGain", 1.0);
        ctx.transpose += num (clip, "elsewhereTranspose", 0.0);

        if (ctx.name.isEmpty())
            ctx.name = clip.getStringAttribute ("name");

        auto time = num (clip, "time");
        auto g0 = ctx.map (time);
        auto g1 = clip.hasAttribute ("duration") ? ctx.map (time + num (clip, "duration")) : inf;
        auto w0 = std::max (ctx.w0, g0), w1 = std::min (ctx.w1, g1);

        if (w1 <= w0)
            return;

        auto contentUnit = unitOf (clip, "contentTimeUnit", ctx.unit);
        auto a = contentUnit == ctx.unit ? ctx.map.a : scaleOf (contentUnit, g0);

        auto fadeUnit = unitOf (clip, "fadeTimeUnit", ctx.unit);
        auto fadeSeconds = fadeUnit == Unit::seconds ? 1.0 : 60.0 / bpmAt (g0);

        if (auto f = num (clip, "fadeInTime") * fadeSeconds; f > 0.0)
        {
            ctx.fadeInAt = g0;
            ctx.fadeInLength = f;
        }

        if (auto f = num (clip, "fadeOutTime") * fadeSeconds; f > 0.0 && std::isfinite (g1))
        {
            ctx.fadeOutAt = g1;
            ctx.fadeOutLength = f;
        }

        auto showContent = [&] (double at, double contentAt, double end)
        {
            Context c = ctx;
            c.unit = contentUnit;
            c.map = { a, at - a * contentAt };
            c.w0 = std::max (w0, at);
            c.w1 = std::min (w1, end);

            if (c.w1 > c.w0)
                for (auto* child : clip.getChildIterator())
                    walk (*child, c);
        };

        auto playStart = num (clip, "playStart");
        auto loopStart = num (clip, "loopStart"), loopEnd = num (clip, "loopEnd");

        if (! clip.hasAttribute ("loopEnd") || loopEnd <= loopStart || ! std::isfinite (g1))
            return showContent (g0, playStart, inf);

        // Looped: play from playStart to the loop end, then repeat the loop
        auto at = g0;
        auto content = playStart < loopEnd ? playStart : loopStart;

        for (int i = 0; at < w1 && i < 10000; ++i)
        {
            auto length = a * (loopEnd - content);

            if (length <= 1.0e-9)
                break;

            showContent (at, content, at + length);
            at += length;
            content = loopStart;
        }
    }

    void addWarps (const juce::XmlElement& warps, const Context& ctx)
    {
        std::vector<std::pair<double, double>> points;     // local time -> content time
        const juce::XmlElement* audio = nullptr;

        for (auto* c : warps.getChildIterator())
        {
            if (c->hasTagName ("Warp"))
                points.push_back ({ num (*c, "time"), num (*c, "contentTime") });
            else if (c->hasTagName ("Audio") && audio == nullptr)
                audio = c;
        }

        if (audio == nullptr)
            return;

        // Content in beats (unusual): read it at the tempo where the clip starts
        if (unitOf (warps, "contentTimeUnit", Unit::seconds) == Unit::beats && ! points.empty())
            for (auto& p : points)
                p.second *= 60.0 / bpmAt (ctx.map (points.front().first));

        std::sort (points.begin(), points.end());
        addAudio (*audio, ctx, points);
    }

    /** Places audio whose content (seconds) is a piecewise-linear function of local time. */
    void addAudio (const juce::XmlElement& audio, const Context& ctx, std::vector<std::pair<double, double>> warp)
    {
        if (ctx.track == nullptr)
            return;

        auto* fileElement = audio.getChildByName ("File");

        if (fileElement == nullptr)
            return;

        auto path = fileElement->getStringAttribute ("path");
        auto found = localFiles.find (path);

        if (found == localFiles.end() || ! found->second.existsAsFile())
        {
            summary.missingFiles.addIfNotAlreadyThere (archiveNameFor (path));
            return;
        }

        auto file = found->second;
        te::AudioFile af (edit->engine, file);

        if (! af.isValid())
        {
            summary.missingFiles.addIfNotAlreadyThere (archiveNameFor (path));
            return;
        }

        usedFiles.addIfNotAlreadyThere (file);

        auto algorithm = audio.getStringAttribute ("algorithm").toLowerCase();
        bool stretch = ! (algorithm.contains ("repitch") || algorithm.contains ("varispeed") || algorithm == "raw");
        auto fileLength = af.getLength();

        // Unwarped (or a single anchor): plays at its own speed, so source time follows real time
        if (warp.size() < 2)
        {
            auto anchor = warp.empty() ? std::pair<double, double> { 0.0, 0.0 } : warp.front();
            auto start = std::max (ctx.w0, ctx.map (anchor.first));
            auto local = ctx.map.inverse (start) - anchor.first;
            auto s0 = anchor.second + (ctx.unit == Unit::seconds ? local : local * 60.0 / bpmAt (start));
            auto t0 = toSeconds (start);
            auto t1 = std::isfinite (ctx.w1) ? toSeconds (ctx.w1) : t0 + fileLength - s0;
            t1 = std::min (t1, t0 + fileLength - s0);
            addPiece (ctx, file, stretch, start, ctx.w1, t0, t1, s0, s0 + (t1 - t0));
            return;
        }

        // Each stretch between two warp markers is linear; the ends carry on at the outer slopes
        for (size_t i = 0; i + 1 < warp.size(); ++i)
        {
            auto [la, ca] = warp[i];
            auto [lb, cb] = warp[i + 1];

            if (lb - la <= 1.0e-12)
                continue;

            auto slope = (cb - ca) / (lb - la);
            auto lo = i == 0 ? -inf : ctx.map (la);
            auto hi = i + 2 == warp.size() ? inf : ctx.map (lb);
            auto g0 = std::max (lo, ctx.w0), g1 = std::min (hi, ctx.w1);

            if (! std::isfinite (g0))
                g0 = ctx.map (std::min (la, ctx.map.inverse (ctx.w0)));

            if (! std::isfinite (g1))
                g1 = ctx.map (la + (fileLength - ca) / std::max (slope, 1.0e-9));

            if (g1 <= g0)
                continue;

            auto contentAt = [&] (double g) { return ca + (ctx.map.inverse (g) - la) * slope; };

            // In beats, a stretch is only constant between tempo changes
            std::vector<double> cuts { g0 };

            if (ctx.unit == Unit::beats)
                for (auto c : tempoChanges)
                    if (c > g0 + 1.0e-9 && c < g1 - 1.0e-9)
                        cuts.push_back (c);

            cuts.push_back (g1);

            for (size_t k = 0; k + 1 < cuts.size(); ++k)
                addPiece (ctx, file, stretch, cuts[k], cuts[k + 1],
                          toSeconds (cuts[k]), toSeconds (cuts[k + 1]), contentAt (cuts[k]), contentAt (cuts[k + 1]));
        }
    }

    void addPiece (const Context& ctx, const juce::File& file, bool stretch, double g0, double g1,
                   double t0, double t1, double s0, double s1)
    {
        if (t1 - t0 < 1.0e-4 || s1 - s0 < 1.0e-6)
            return;     // empty, or played backwards (not supported)

        te::AudioFile af (edit->engine, file);
        auto fileLength = af.getLength();

        // Keep within the file
        auto rate = (s1 - s0) / (t1 - t0);

        if (s0 < 0.0)
        {
            t0 += -s0 / rate;
            s0 = 0.0;
        }

        if (s1 > fileLength)
        {
            t1 -= (s1 - fileLength) / rate;
            s1 = fileLength;
        }

        if (t1 - t0 < 1.0e-4)
            return;

        Piece p { ctx.track, file, t0, t1, s0, s1, stretch, ctx.disabled, ctx.gain, ctx.transpose, ctx.name };

        if (std::abs (g0 - ctx.fadeInAt) < 1.0e-6)
            p.fadeIn = ctx.fadeInLength;

        if (std::abs (g1 - ctx.fadeOutAt) < 1.0e-6)
            p.fadeOut = ctx.fadeOutLength;

        pieces.push_back (p);
    }

    void addNotes (const juce::XmlElement& notes, const Context& ctx)
    {
        if (ctx.track == nullptr)
            return;

        struct Note { double g0, g1; int key, velocity; };
        std::vector<Note> found;

        for (auto* n : notes.getChildWithTagNameIterator ("Note"))
        {
            auto t = num (*n, "time");
            auto g0 = ctx.map (t), g1 = std::min (ctx.w1, ctx.map (t + num (*n, "duration", 0.25)));

            if (g0 < ctx.w0 - 1.0e-9 || g0 >= ctx.w1 || g1 <= g0)
                continue;

            found.push_back ({ g0, g1, juce::jlimit (0, 127, (int) num (*n, "key", 60)),
                               juce::jlimit (1, 127, juce::roundToInt (num (*n, "vel", 0.8) * 127.0)) });
        }

        auto c0 = ctx.w0, c1 = ctx.w1;

        for (auto& n : found)
        {
            if (! std::isfinite (ctx.w0)) c0 = std::isfinite (c0) ? std::min (c0, n.g0) : n.g0;
            if (! std::isfinite (ctx.w1)) c1 = std::isfinite (c1) ? std::max (c1, n.g1) : n.g1;
        }

        if (! std::isfinite (c0) || ! std::isfinite (c1) || c1 <= c0)
            return;

        auto range = te::TimeRange (te::TimePosition::fromSeconds (toSeconds (c0)), te::TimePosition::fromSeconds (toSeconds (c1)));
        auto clip = ctx.track->insertMIDIClip (ctx.name.isNotEmpty() ? ctx.name : ctx.track->getName(), range, nullptr);

        if (clip == nullptr)
            return;

        auto startBeat = toBeats (c0);
        auto& sequence = clip->getSequence();

        for (auto& n : found)
        {
            auto b0 = toBeats (n.g0), b1 = toBeats (n.g1);
            sequence.addNote (n.key, te::BeatPosition::fromBeats (b0 - startBeat),
                              te::BeatDuration::fromBeats (std::max (1.0e-3, b1 - b0)), n.velocity, 0, nullptr);
        }

        clip->disabled = ctx.disabled;
        midiTracks.insert (ctx.track);
        ++summary.midiClips;
    }

    //==============================================================================
    void createAudioClips()
    {
        std::stable_sort (pieces.begin(), pieces.end(), [] (const Piece& a, const Piece& b)
        {
            return a.track != b.track ? a.track < b.track : a.t0 < b.t0;
        });

        // A clip whose warps were all in a line (or split at a tempo change that didn't
        // change its stretch) comes back together as one clip
        std::vector<Piece> merged;

        for (auto& p : pieces)
        {
            if (! merged.empty())
            {
                auto& last = merged.back();
                auto rateA = (last.s1 - last.s0) / (last.t1 - last.t0), rateB = (p.s1 - p.s0) / (p.t1 - p.t0);

                if (last.track == p.track && last.file == p.file && last.disabled == p.disabled && last.name == p.name
                     && last.gain == p.gain && last.transpose == p.transpose
                     && std::abs (last.t1 - p.t0) < 1.0e-6 && std::abs (last.s1 - p.s0) < 1.0e-5
                     && std::abs (rateA - rateB) < 1.0e-4 * rateA && last.fadeOut == 0.0 && p.fadeIn == 0.0)
                {
                    last.t1 = p.t1;
                    last.s1 = p.s1;
                    last.fadeOut = p.fadeOut;
                    continue;
                }
            }

            merged.push_back (p);
        }

        // A clip warped with many markers (a whole song warped to the grid) comes out as a run of
        // pieces at slightly different speeds: put each run back together as one clip with warp
        // markers. Re-pitched clips stay as pieces, as warping keeps the pitch.
        std::vector<std::vector<Piece>> runs;

        for (auto& p : merged)
        {
            if (! runs.empty())
            {
                auto& last = runs.back().back();

                if (last.track == p.track && last.file == p.file && last.disabled == p.disabled && last.name == p.name
                     && last.gain == p.gain && last.transpose == p.transpose && last.stretch && p.stretch
                     && std::abs (last.t1 - p.t0) < 1.0e-6 && std::abs (last.s1 - p.s0) < 1.0e-5
                     && last.fadeOut == 0.0 && p.fadeIn == 0.0)
                {
                    runs.back().push_back (p);
                    continue;
                }
            }

            runs.push_back ({ p });
        }

        for (auto& run : runs)
        {
            auto& p = run.front();
            auto name = p.name.isNotEmpty() ? p.name : p.file.getFileNameWithoutExtension();
            te::WaveAudioClip::Ptr clip;

            if (run.size() == 1)
            {
                clip = SessionImport::addAudioClip (*p.track, name, p.file, p.t0, p.t1, p.s0, (p.s1 - p.s0) / (p.t1 - p.t0), p.stretch);
            }
            else
            {
                std::vector<std::pair<double, double>> points { { p.t0, p.s0 } };

                for (auto& piece : run)
                    points.push_back ({ piece.t1, piece.s1 });

                clip = SessionImport::addWarpedAudioClip (*p.track, name, p.file, points);
            }

            if (clip == nullptr)
                continue;

            clip->setFadeIn (te::TimeDuration::fromSeconds (p.fadeIn));
            clip->setFadeOut (te::TimeDuration::fromSeconds (run.back().fadeOut));
            clip->disabled = p.disabled;

            if (p.gain != 1.0)
                clip->setGainDB (juce::Decibels::gainToDecibels ((float) p.gain, -100.0f));

            if (p.transpose != 0.0)
                clip->setPitchChange ((float) p.transpose);

            ++summary.audioClips;
        }
    }
};

//==============================================================================
Importer::Importer (const juce::File& f) : impl (std::make_unique<Impl>())
{
    impl->source = f;
    impl->zip = std::make_unique<ZipReader> (f);

    auto* entry = impl->zip->find ("project.xml");

    if (entry == nullptr)
    {
        impl->error = "\"" + f.getFileName() + "\" isn't a DAWproject file (it has no project.xml inside).";
        return;
    }

    if (auto in = impl->zip->open (*entry))
        impl->project = juce::parseXML (in->readEntireStreamAsString());

    if (impl->project == nullptr || ! impl->project->hasTagName ("Project"))
    {
        impl->project.reset();
        impl->error = "\"" + f.getFileName() + "\" couldn't be read: its project.xml is damaged.";
    }
}

Importer::Importer (std::unique_ptr<juce::XmlElement> project, const juce::File& baseFolder) : impl (std::make_unique<Impl>())
{
    impl->source = baseFolder.getChildFile ("converted.dawproject");
    impl->project = std::move (project);

    if (impl->project == nullptr || ! impl->project->hasTagName ("Project"))
        impl->error = "The project couldn't be converted.";
}

Importer::~Importer() = default;

juce::String Importer::getError() const     { return impl->error; }

bool Importer::extractAudio (const juce::File& destDir, std::function<bool (double)> progress)
{
    if (impl->project == nullptr)
        return false;

    struct Source { juce::String path; const ZipReader::Entry* entry = nullptr; juce::File external; juce::int64 size = 0; };
    std::vector<Source> sources;
    std::set<juce::String> seen;
    juce::int64 total = 0;

    std::function<void (const juce::XmlElement&)> collect = [&] (const juce::XmlElement& e)
    {
        if (e.hasTagName ("File"))
        {
            auto path = e.getStringAttribute ("path");

            if (path.isNotEmpty() && seen.insert (path).second)
            {
                Source s;
                s.path = path;

                // Embedded unless it says otherwise; fall back to the other place either way
                if (! flag (e, "external", false))
                    s.entry = impl->zip != nullptr ? impl->zip->find (path) : nullptr;

                if (s.entry == nullptr)
                {
                    auto f = juce::File::isAbsolutePath (path) ? juce::File (path)
                                                               : impl->source.getParentDirectory().getChildFile (path);
                    if (f.existsAsFile())
                        s.external = f;
                    else
                        s.entry = impl->zip != nullptr ? impl->zip->find (path) : nullptr;
                }

                s.size = s.entry != nullptr ? s.entry->size : s.external.getSize();
                total += s.size;

                if (s.entry != nullptr || s.external != juce::File())
                    sources.push_back (s);
            }
        }

        for (auto* child : e.getChildIterator())
            collect (*child);
    };

    collect (*impl->project);
    destDir.createDirectory();

    juce::int64 done = 0;
    juce::HeapBlock<char> buffer (1 << 20);

    for (auto& s : sources)
    {
        auto name = archiveNameFor (s.path);
        auto dest = destDir.getNonexistentChildFile (juce::File::createFileWithoutCheckingPath (name).getFileNameWithoutExtension(),
                                                     juce::File::createFileWithoutCheckingPath (name).getFileExtension(), false);

        std::unique_ptr<juce::InputStream> in;

        if (s.entry != nullptr)
            in = impl->zip->open (*s.entry);
        else
            in = s.external.createInputStream();

        if (in == nullptr)
            continue;

        {
            juce::FileOutputStream out (dest);

            if (! out.openedOk())
                continue;

            for (int n; (n = in->read (buffer, 1 << 20)) > 0;)
            {
                out.write (buffer, (size_t) n);
                done += n;

                if (progress && ! progress (total > 0 ? (double) done / (double) total : 1.0))
                {
                    out.flush();
                    return false;
                }
            }
        }

        impl->localFiles[s.path] = dest;
    }

    return true;
}

ImportSummary Importer::build (te::Edit& edit)
{
    auto& d = *impl;
    d.edit = &edit;
    d.summary = {};

    if (d.project == nullptr)
        return d.summary;

    if (auto* app = d.project->getChildByName ("Application"))
        d.summary.applicationName = app->getStringAttribute ("name");

    SessionImport::removePlaceholderTracks (edit);

    d.buildTempo();

    if (auto* structure = d.project->getChildByName ("Structure"))
        d.buildTracks (*structure, nullptr);

    if (auto* arrangement = d.project->getChildByName ("Arrangement"))
    {
        if (auto* lanes = arrangement->getChildByName ("Lanes"))
        {
            d.arrangementUnit = unitOf (*lanes, "timeUnit", Unit::beats);
            d.findTempoChanges();

            Impl::Context root;
            root.unit = d.arrangementUnit;
            d.walk (*lanes, root);
        }
    }

    d.createAudioClips();
    SessionImport::finish (edit, d.midiTracks, d.usedFiles, d.summary);
    d.summary.notes.add ("Plug-ins, sends and automation aren't brought across.");
    return d.summary;
}

//==============================================================================
namespace
{
    struct Exporter
    {
        te::Edit& edit;
        ExportPlan plan;
        int nextId = 1;
        std::map<juce::String, juce::String> archivePaths;     // full path -> path in the archive
        std::set<juce::String> usedNames;
        std::vector<std::pair<te::AudioTrack*, juce::String>> laneTracks;
        std::vector<double> warpBeats;                          // tempo changes (and beats inside ramps)
        int reversed = 0, missing = 0, returns = 0;

        explicit Exporter (te::Edit& e) : edit (e) {}

        juce::String id()                       { return "id" + juce::String (nextId++); }
        te::TempoSequence& tempo()              { return edit.tempoSequence; }
        double beatsAt (double seconds)         { return tempo().toBeats (te::TimePosition::fromSeconds (seconds)).inBeats(); }
        double secondsAt (double beats)         { return tempo().toTime (te::BeatPosition::fromBeats (beats)).inSeconds(); }

        juce::String archivePathFor (const juce::File& f)
        {
            auto key = f.getFullPathName();

            if (auto it = archivePaths.find (key); it != archivePaths.end())
                return it->second;

            auto base = f.getFileNameWithoutExtension(), ext = f.getFileExtension();
            auto name = base + ext;

            for (int i = 2; usedNames.count (name.toLowerCase()) > 0; ++i)
                name = base + " " + juce::String (i) + ext;

            usedNames.insert (name.toLowerCase());
            auto path = "audio/" + name;
            archivePaths[key] = path;
            plan.audioFiles.push_back ({ f, path });
            return path;
        }

        void addMixer (juce::XmlElement& channel, te::VolumeAndPanPlugin* volume, bool muted)
        {
            auto* mute = channel.createNewChildElement ("Mute");
            mute->setAttribute ("value", muted ? "true" : "false");
            mute->setAttribute ("id", id());
            mute->setAttribute ("name", "Mute");

            auto* pan = channel.createNewChildElement ("Pan");
            pan->setAttribute ("max", 1.0);
            pan->setAttribute ("min", 0.0);
            pan->setAttribute ("unit", "normalized");
            pan->setAttribute ("value", volume != nullptr ? (volume->getPan() + 1.0) / 2.0 : 0.5);
            pan->setAttribute ("id", id());
            pan->setAttribute ("name", "Pan");

            auto* vol = channel.createNewChildElement ("Volume");
            vol->setAttribute ("max", 2.0);
            vol->setAttribute ("min", 0.0);
            vol->setAttribute ("unit", "linear");
            vol->setAttribute ("value", volume != nullptr ? (double) juce::Decibels::decibelsToGain (volume->getVolumeDb(), -100.0f) : 1.0);
            vol->setAttribute ("id", id());
            vol->setAttribute ("name", "Volume");
        }

        static juce::String contentTypeFor (te::AudioTrack& t)
        {
            bool audio = false, notes = false;

            for (auto* c : t.getClips())
            {
                audio = audio || dynamic_cast<te::WaveAudioClip*> (c) != nullptr;
                notes = notes || dynamic_cast<te::MidiClip*> (c) != nullptr;
            }

            if (! audio && ! notes)
                for (auto* p : t.pluginList)
                    notes = notes || p->isSynth();

            return audio && notes ? "audio notes" : notes ? "notes" : "audio";
        }

        void addTrack (juce::XmlElement& parent, te::Track& t, const juce::String& destination)
        {
            auto* folder = dynamic_cast<te::FolderTrack*> (&t);
            auto* audio = dynamic_cast<te::AudioTrack*> (&t);

            if (folder == nullptr && audio == nullptr)
                return;

            if (audio != nullptr && Mixing::isReturnTrack (*audio))
            {
                ++returns;
                return;
            }

            auto trackId = id(), channelId = id();
            auto* track = parent.createNewChildElement ("Track");
            track->setAttribute ("contentType", folder != nullptr ? "tracks" : contentTypeFor (*audio));
            track->setAttribute ("loaded", "true");
            track->setAttribute ("id", trackId);
            track->setAttribute ("name", t.getName());
            track->setAttribute ("color", colourString (t.getColour()));

            auto* volume = folder != nullptr ? folder->getVolumePlugin() : audio->getVolumePlugin();
            bool isSubmix = folder != nullptr && volume != nullptr;

            auto* channel = track->createNewChildElement ("Channel");
            channel->setAttribute ("audioChannels", 2);
            channel->setAttribute ("destination", destination);
            channel->setAttribute ("role", isSubmix ? "submix" : "regular");
            channel->setAttribute ("solo", t.isSolo (false) ? "true" : "false");
            channel->setAttribute ("id", channelId);
            addMixer (*channel, volume, t.isMuted (false));

            if (folder != nullptr)
            {
                for (auto* child : folder->getAllSubTracks (false))
                    addTrack (*track, *child, isSubmix ? channelId : destination);
            }
            else
            {
                laneTracks.push_back ({ audio, trackId });
            }
        }

        //==============================================================================
        /** A stretch of a clip: edit beats -> source seconds, linear in beats or in seconds. */
        struct Span { double b0, b1, s0, s1; bool linearInBeats; };

        std::vector<Span> spansFor (te::WaveAudioClip& c)
        {
            std::vector<Span> spans;
            auto pos = c.getPosition();
            auto start = pos.getStart().inSeconds(), end = pos.getEnd().inSeconds();

            if (c.getAutoTempo())
            {
                auto bps = c.getLoopInfo().getBeatsPerSecond (c.getAudioFile().getInfo());

                if (bps > 0.0)
                {
                    auto b0 = c.getStartBeat().inBeats(), b1 = c.getEndBeat().inBeats();
                    auto offset = c.getOffsetInBeats().inBeats();

                    if (! c.isLooping())
                    {
                        spans.push_back ({ b0, b1, offset / bps, (offset + b1 - b0) / bps, true });
                    }
                    else
                    {
                        auto loopStart = c.getLoopStartBeats().inBeats(), loopLength = c.getLoopLengthBeats().inBeats();
                        auto content = loopStart + std::fmod (std::max (0.0, offset), loopLength);
                        auto b = b0;

                        for (int i = 0; b < b1 - 1.0e-9 && i < 10000; ++i)
                        {
                            auto length = std::min (loopStart + loopLength - content, b1 - b);
                            spans.push_back ({ b, b + length, content / bps, (content + length) / bps, true });
                            b += length;
                            content = loopStart;
                        }
                    }

                    return spans;
                }
            }

            auto speed = c.getSpeedRatio();
            auto offset = pos.getOffset().inSeconds();
            auto add = [&] (double t0, double t1, double s0)
            {
                spans.push_back ({ beatsAt (t0), beatsAt (t1), s0, s0 + (t1 - t0) * speed, false });
            };

            if (! c.isLooping())
            {
                add (start, end, offset * speed);
            }
            else
            {
                auto loopStart = c.getLoopStart().inSeconds(), loopLength = c.getLoopLength().inSeconds();
                int i = 0;

                for (auto t = start - offset; t < end - 1.0e-9 && i < 10000; t += loopLength, ++i)
                {
                    auto t0 = std::max (t, start), t1 = std::min (t + loopLength, end);

                    if (t1 > t0)
                        add (t0, t1, (loopStart + (t0 - t)) * speed);
                }
            }

            // With warp markers the clip plays a warped copy: those positions are warp time. Split
            // each span at the markers and map it back to the file.
            if (c.getWarpTime())
            {
                auto& warp = c.getWarpTimeManager();
                std::vector<Span> warped;

                for (auto& span : spans)
                {
                    auto t0 = secondsAt (span.b0), t1 = secondsAt (span.b1);
                    auto rate = (span.s1 - span.s0) / std::max (1.0e-9, t1 - t0);
                    std::vector<double> cuts { span.s0 };

                    for (auto* m : warp.getMarkers())
                        if (auto w = m->warpTime.inSeconds(); w > span.s0 + 1.0e-9 && w < span.s1 - 1.0e-9)
                            cuts.push_back (w);

                    cuts.push_back (span.s1);

                    for (size_t i = 0; i + 1 < cuts.size(); ++i)
                    {
                        auto a = t0 + (cuts[i] - span.s0) / rate, b = t0 + (cuts[i + 1] - span.s0) / rate;
                        warped.push_back ({ beatsAt (a), beatsAt (b),
                                            warp.warpTimeToSourceTime (te::TimePosition::fromSeconds (cuts[i])).inSeconds(),
                                            warp.warpTimeToSourceTime (te::TimePosition::fromSeconds (cuts[i + 1])).inSeconds(), false });
                    }
                }

                spans = std::move (warped);
            }

            return spans;
        }

        void exportAudioClip (juce::XmlElement& clips, te::WaveAudioClip& c)
        {
            auto file = c.getOriginalFile();

            if (! file.existsAsFile())
            {
                ++missing;
                return;
            }

            if (c.getIsReversed())
                ++reversed;

            te::AudioFile af (edit.engine, file);
            auto path = archivePathFor (file);
            auto algorithm = (! c.getAutoTempo() && ! c.getWarpTime() && c.getTimeStretchMode() == te::TimeStretcher::disabled) ? "repitch" : "stretch";
            auto spans = spansFor (c);

            // Spans that follow on from each other (in time and in the file) share one clip with a
            // warp marker between them; a loop's jump back starts a new clip
            std::vector<std::vector<Span>> groups;

            for (auto& span : spans)
            {
                if (span.b1 - span.b0 < 1.0e-6)
                    continue;

                if (! groups.empty() && std::abs (groups.back().back().b1 - span.b0) < 1.0e-6
                     && std::abs (groups.back().back().s1 - span.s0) < 1.0e-5)
                    groups.back().push_back (span);
                else
                    groups.push_back ({ span });
            }

            for (size_t g = 0; g < groups.size(); ++g)
            {
                auto& group = groups[g];
                auto b0 = group.front().b0;
                auto length = group.back().b1 - b0;

                auto* clip = clips.createNewChildElement ("Clip");
                clip->setAttribute ("time", b0);
                clip->setAttribute ("duration", length);
                clip->setAttribute ("contentTimeUnit", "beats");
                clip->setAttribute ("playStart", 0.0);
                clip->setAttribute ("fadeTimeUnit", "seconds");
                clip->setAttribute ("fadeInTime", g == 0 ? c.getFadeIn().inSeconds() : 0.0);
                clip->setAttribute ("fadeOutTime", g + 1 == groups.size() ? c.getFadeOut().inSeconds() : 0.0);
                clip->setAttribute ("name", c.getName());

                if (c.disabled)
                    clip->setAttribute ("enable", "false");

                auto* warps = clip->createNewChildElement ("Warps");
                warps->setAttribute ("contentTimeUnit", "seconds");
                warps->setAttribute ("timeUnit", "beats");
                warps->setAttribute ("id", id());

                auto* audio = warps->createNewChildElement ("Audio");
                audio->setAttribute ("algorithm", algorithm);
                audio->setAttribute ("channels", af.getNumChannels());
                audio->setAttribute ("duration", af.getLength());
                audio->setAttribute ("sampleRate", (int) af.getSampleRate());
                audio->setAttribute ("id", id());
                audio->createNewChildElement ("File")->setAttribute ("path", path);

                auto addWarp = [warps] (double beat, double content)
                {
                    auto* w = warps->createNewChildElement ("Warp");
                    w->setAttribute ("time", beat);
                    w->setAttribute ("contentTime", content);
                };

                addWarp (0.0, group.front().s0);

                for (auto& s : group)
                {
                    // Linear in seconds: needs a marker wherever the tempo changes, as the
                    // reading DAW interpolates warps in beats
                    if (! s.linearInBeats)
                    {
                        auto t0 = secondsAt (s.b0), rate = (s.s1 - s.s0) / std::max (1.0e-9, secondsAt (s.b1) - t0);

                        for (auto b : warpBeats)
                            if (b > s.b0 + 1.0e-6 && b < s.b1 - 1.0e-6)
                                addWarp (b - b0, s.s0 + (secondsAt (b) - t0) * rate);
                    }

                    addWarp (s.b1 - b0, s.s1);
                }
            }
        }

        void exportMidiClip (juce::XmlElement& clips, te::MidiClip& c)
        {
            auto b0 = c.getStartBeat().inBeats(), b1 = c.getEndBeat().inBeats();

            auto* clip = clips.createNewChildElement ("Clip");
            clip->setAttribute ("time", b0);
            clip->setAttribute ("duration", b1 - b0);
            clip->setAttribute ("contentTimeUnit", "beats");
            clip->setAttribute ("playStart", 0.0);
            clip->setAttribute ("name", c.getName());

            if (c.disabled)
                clip->setAttribute ("enable", "false");

            auto* notes = clip->createNewChildElement ("Notes");
            notes->setAttribute ("timeUnit", "beats");
            notes->setAttribute ("id", id());

            auto add = [&] (const te::MidiNote& n, double editBeat, double maxLength)
            {
                if (editBeat < b0 - 1.0e-9 || editBeat >= b1)
                    return;

                auto length = std::min ({ n.getLengthBeats().inBeats(), maxLength, b1 - editBeat });
                auto* note = notes->createNewChildElement ("Note");
                note->setAttribute ("time", editBeat - b0);
                note->setAttribute ("duration", length);
                note->setAttribute ("channel", 0);
                note->setAttribute ("key", n.getNoteNumber());
                note->setAttribute ("vel", n.getVelocity() / 127.0);
                note->setAttribute ("rel", 0.5);
            };

            auto& sequence = c.getSequence();
            auto offset = c.getOffsetInBeats().inBeats();

            if (! c.isLooping())
            {
                for (auto* n : sequence.getNotes())
                    add (*n, b0 - offset + n->getStartBeat().inBeats(), inf);
            }
            else
            {
                auto loopStart = c.getLoopStartBeats().inBeats(), loopLength = c.getLoopLengthBeats().inBeats();
                int i = 0;

                for (auto at = b0 - offset; at < b1 && i < 10000; at += loopLength, ++i)
                    for (auto* n : sequence.getNotes())
                        if (auto nb = n->getStartBeat().inBeats(); nb >= loopStart && nb < loopStart + loopLength)
                            add (*n, at + nb - loopStart, loopStart + loopLength - nb);
            }
        }

        //==============================================================================
        void run (const juce::String& projectName)
        {
            auto& ts = tempo();

            for (int i = 1; i < ts.getNumTempos(); ++i)
                warpBeats.push_back (ts.getTempo (i)->getStartBeat().inBeats());

            for (int i = 0; i + 1 < ts.getNumTempos(); ++i)
                if (std::abs (std::abs (ts.getTempo (i)->getCurve()) - 1.0f) > 1.0e-4f)
                    for (auto b = std::floor (ts.getTempo (i)->getStartBeat().inBeats()) + 1.0; b < ts.getTempo (i + 1)->getStartBeat().inBeats(); b += 1.0)
                        warpBeats.push_back (b);

            std::sort (warpBeats.begin(), warpBeats.end());

            plan.project = std::make_unique<juce::XmlElement> ("Project");
            auto& project = *plan.project;
            project.setAttribute ("version", "1.0");

            auto* app = project.createNewChildElement ("Application");
            app->setAttribute ("name", "Elsewhere");
            app->setAttribute ("version", ELSEWHERE_VERSION);

            auto tempoId = id(), timeSigId = id();
            auto* transport = project.createNewChildElement ("Transport");
            auto* tempoElement = transport->createNewChildElement ("Tempo");
            tempoElement->setAttribute ("max", 999.0);
            tempoElement->setAttribute ("min", 20.0);
            tempoElement->setAttribute ("unit", "bpm");
            tempoElement->setAttribute ("value", ts.getTempo (0)->getBpm());
            tempoElement->setAttribute ("id", tempoId);
            tempoElement->setAttribute ("name", "Tempo");

            auto* sig = transport->createNewChildElement ("TimeSignature");
            sig->setAttribute ("denominator", ts.getTimeSig (0)->denominator.get());
            sig->setAttribute ("numerator", ts.getTimeSig (0)->numerator.get());
            sig->setAttribute ("id", timeSigId);

            // Tracks, then the master (other DAWs list it last)
            auto masterChannelId = id();
            auto* structure = project.createNewChildElement ("Structure");

            for (auto* t : te::getAllTracks (edit))
                if (t->getParentTrack() == nullptr)
                    addTrack (*structure, *t, masterChannelId);

            auto* master = structure->createNewChildElement ("Track");
            master->setAttribute ("contentType", "audio notes");
            master->setAttribute ("loaded", "true");
            master->setAttribute ("id", id());
            master->setAttribute ("name", "Master");

            auto* masterChannel = master->createNewChildElement ("Channel");
            masterChannel->setAttribute ("audioChannels", 2);
            masterChannel->setAttribute ("role", "master");
            masterChannel->setAttribute ("solo", "false");
            masterChannel->setAttribute ("id", masterChannelId);
            addMixer (*masterChannel, edit.getMasterVolumePlugin().get(), false);

            auto* arrangement = project.createNewChildElement ("Arrangement");
            arrangement->setAttribute ("id", id());

            if (ts.getNumTempos() > 1)
            {
                auto* automation = arrangement->createNewChildElement ("TempoAutomation");
                automation->setAttribute ("unit", "bpm");
                automation->setAttribute ("timeUnit", "beats");
                automation->setAttribute ("id", id());
                automation->createNewChildElement ("Target")->setAttribute ("parameter", tempoId);

                for (int i = 0; i < ts.getNumTempos(); ++i)
                {
                    auto* t = ts.getTempo (i);
                    auto* p = automation->createNewChildElement ("RealPoint");
                    p->setAttribute ("time", t->getStartBeat().inBeats());
                    p->setAttribute ("value", t->getBpm());
                    p->setAttribute ("interpolation", std::abs (std::abs (t->getCurve()) - 1.0f) < 1.0e-4f ? "hold" : "linear");
                }
            }

            if (ts.getNumTimeSigs() > 1)
            {
                auto* automation = arrangement->createNewChildElement ("TimeSignatureAutomation");
                automation->setAttribute ("timeUnit", "beats");
                automation->setAttribute ("id", id());
                automation->createNewChildElement ("Target")->setAttribute ("parameter", timeSigId);

                for (int i = 0; i < ts.getNumTimeSigs(); ++i)
                {
                    auto* s = ts.getTimeSig (i);
                    auto* p = automation->createNewChildElement ("TimeSignaturePoint");
                    p->setAttribute ("time", s->getStartBeat().inBeats());
                    p->setAttribute ("numerator", s->numerator.get());
                    p->setAttribute ("denominator", s->denominator.get());
                }
            }

            auto* lanes = arrangement->createNewChildElement ("Lanes");
            lanes->setAttribute ("timeUnit", "beats");
            lanes->setAttribute ("id", id());

            for (auto& [track, trackId] : laneTracks)
            {
                auto* trackLanes = lanes->createNewChildElement ("Lanes");
                trackLanes->setAttribute ("track", trackId);
                trackLanes->setAttribute ("id", id());

                auto* clips = trackLanes->createNewChildElement ("Clips");
                clips->setAttribute ("id", id());

                for (auto* c : track->getClips())
                {
                    if (auto* wave = dynamic_cast<te::WaveAudioClip*> (c))
                        exportAudioClip (*clips, *wave);
                    else if (auto* midi = dynamic_cast<te::MidiClip*> (c))
                        exportMidiClip (*clips, *midi);
                }
            }

            plan.metadata = std::make_unique<juce::XmlElement> ("MetaData");
            plan.metadata->createNewChildElement ("Title")->addTextElement (projectName);

            if (missing > 0)
                plan.warnings.add (juce::String (missing) + (missing == 1 ? " clip's audio file is" : " clips' audio files are")
                                    + " missing, so " + (missing == 1 ? "it was" : "they were") + " left out.");
            if (reversed > 0)
                plan.warnings.add (juce::String (reversed) + (reversed == 1 ? " reversed clip was" : " reversed clips were")
                                    + " exported playing forwards.");
            if (returns > 0)
                plan.warnings.add ("Return tracks and sends were left out.");

            plan.warnings.add ("Plug-ins and automation aren't included.");
        }
    };
}

ExportPlan planExport (te::Edit& edit, const juce::String& projectName)
{
    Exporter e (edit);
    e.run (projectName);
    return std::move (e.plan);
}

juce::Result writeExport (const ExportPlan& plan, const juce::File& destination)
{
    if (plan.project == nullptr)
        return juce::Result::fail ("Nothing to export.");

    auto temp = destination.getSiblingFile (destination.getFileName() + ".partial");
    temp.deleteFile();

    {
        juce::FileOutputStream out (temp);

        if (! out.openedOk())
            return juce::Result::fail ("Couldn't write to " + destination.getParentDirectory().getFullPathName());

        ZipWriter zip (out);
        bool ok = zip.addText ("project.xml", plan.project->toString())
                   && zip.addText ("metadata.xml", plan.metadata->toString());

        for (auto& [file, path] : plan.audioFiles)
            ok = ok && zip.addFile (path, file, {});

        ok = ok && zip.finish();

        if (! ok)
        {
            temp.deleteFile();
            return juce::Result::fail ("Writing the file failed. Is the disk full?");
        }
    }

    destination.deleteFile();

    if (! temp.moveFileTo (destination))
        return juce::Result::fail ("Couldn't save " + destination.getFullPathName());

    return juce::Result::ok();
}

} // namespace elsewhere::DawProject
