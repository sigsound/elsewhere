// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere
{
//==============================================================================
/**
    The AI training notice an export can carry: a statement in the file's own metadata that the
    recording isn't licensed for AI training, with the rights holder reserving it from text and
    data mining (the machine-readable opt-out Article 4(3) of EU Directive 2019/790 provides for).

    It's written into the finished file, after rendering, in the places each format keeps such
    things, so that people see it in their players and software reading the file finds it:

    - WAV: a LIST/INFO chunk (ICOP copyright, ICMT comment) and an XMP packet in a _PMX chunk
    - AIFF: "(c) " and ANNO chunks, and XMP in an APPL chunk
    - FLAC and Ogg Vorbis: COPYRIGHT, COMMENT and TDM_RESERVATION comments
    - MP3: an ID3v2 tag with TCOP, COMM and TXXX:TDM_RESERVATION frames, and XMP in a PRIV frame

    The XMP says the same in standard vocabularies: dc:rights, xmpRights:UsageTerms, the PLUS
    DataMining property (prohibited for AI and machine learning training) and TDMRep's
    tdm:reservation. Only metadata changes: the audio is byte for byte what was rendered.
*/
namespace RightsNotice
{
    struct Notice
    {
        juce::String owner;                 // the artist, when the song has one
        int year = juce::Time::getCurrentTime().getYear();

        /** "2026 Artist. All rights reserved.", the form ID3's TCOP and AIFF's (c) chunk want. */
        juce::String bareCopyright() const
        {
            return juce::String (year) + (owner.trim().isNotEmpty() ? " " + owner.trim() : juce::String()) + ". All rights reserved.";
        }

        juce::String copyright() const      { return juce::String::fromUTF8 ("\xc2\xa9 ") + bareCopyright(); }
        juce::String asciiCopyright() const { return "Copyright " + bareCopyright(); }

        juce::String terms() const
        {
            return "Not licensed for AI training. The rights holder reserves this recording from text and data mining, "
                   "including use to train AI models, under Article 4(3) of Directive (EU) 2019/790 and similar laws in other countries.";
        }

        juce::String xmp() const
        {
            auto esc = [] (juce::String s)
            {
                return s.replace ("&", "&amp;").replace ("<", "&lt;").replace (">", "&gt;").replace ("\"", "&quot;");
            };

            juce::String x;
            x << "<?xpacket begin=\"" << juce::String::fromUTF8 ("\xef\xbb\xbf") << "\" id=\"W5M0MpCehiHzreSzNTczkc9d\"?>\n"
              << "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
              << " <rdf:RDF xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
              << "  <rdf:Description rdf:about=\"\"\n"
              << "    xmlns:dc=\"http://purl.org/dc/elements/1.1/\"\n"
              << "    xmlns:xmpRights=\"http://ns.adobe.com/xap/1.0/rights/\"\n"
              << "    xmlns:plus=\"http://ns.useplus.org/ldf/xmp/1.0/\"\n"
              << "    xmlns:tdm=\"http://www.w3.org/ns/tdmrep/\">\n"
              << "   <dc:rights><rdf:Alt><rdf:li xml:lang=\"x-default\">" << esc (copyright()) << "</rdf:li></rdf:Alt></dc:rights>\n"
              << "   <xmpRights:Marked>True</xmpRights:Marked>\n"
              << "   <xmpRights:UsageTerms><rdf:Alt><rdf:li xml:lang=\"x-default\">" << esc (terms()) << "</rdf:li></rdf:Alt></xmpRights:UsageTerms>\n"
              << "   <plus:DataMining>http://ns.useplus.org/ldf/vocab/DMI-PROHIBITED-AIMLTRAINING</plus:DataMining>\n"
              << "   <tdm:reservation>1</tdm:reservation>\n"
              << "  </rdf:Description>\n"
              << " </rdf:RDF>\n"
              << "</x:xmpmeta>\n"
              << "<?xpacket end=\"w\"?>";
            return x;
        }

        /** The comments for FLAC and Ogg, KEY=value. */
        juce::StringArray vorbisComments() const
        {
            return { "COPYRIGHT=" + copyright(), "COMMENT=" + terms(), "TDM_RESERVATION=1" };
        }
    };

    //==============================================================================
    namespace detail
    {
        using Bytes = std::vector<uint8_t>;

        inline void add (Bytes& b, const void* p, size_t n)     { auto* c = static_cast<const uint8_t*> (p); b.insert (b.end(), c, c + n); }
        inline void add (Bytes& b, const char* s)               { add (b, s, std::strlen (s)); }
        inline void addUtf8 (Bytes& b, const juce::String& s)   { add (b, s.toRawUTF8(), s.getNumBytesAsUTF8()); }
        inline void le32 (Bytes& b, uint32_t v)                 { for (int i = 0; i < 4; ++i) b.push_back ((uint8_t) (v >> (8 * i))); }
        inline void be32 (Bytes& b, uint32_t v)                 { for (int i = 3; i >= 0; --i) b.push_back ((uint8_t) (v >> (8 * i))); }
        inline uint32_t readLE32 (const uint8_t* p)             { return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24); }
        inline uint32_t readBE32 (const uint8_t* p)             { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | (uint32_t) p[3]; }

        inline bool readAll (const juce::File& f, Bytes& b)
        {
            juce::MemoryBlock m;

            if (! f.loadFileAsData (m))
                return false;

            auto* p = static_cast<const uint8_t*> (m.getData());
            b.assign (p, p + m.getSize());
            return true;
        }

        /** Replaces the file's contents, through a temporary file so a failure leaves it as it was. */
        inline juce::String replaceWith (const juce::File& f, const Bytes& b)
        {
            juce::TemporaryFile temp (f);

            if (! temp.getFile().replaceWithData (b.data(), b.size()) || ! temp.overwriteTargetFileWithTemporary())
                return "couldn't write the file";

            return {};
        }

        /** Adds chunks to the end of a RIFF or IFF file in place and updates its size: the chunks go
            first, so if writing them fails the file is still the valid file it was. */
        inline juce::String appendChunks (const juce::File& f, const Bytes& chunks, bool bigEndian)
        {
            uint8_t header[12];

            if (f.getSize() < 12 || juce::FileInputStream (f).read (header, 12) != 12)
                return "couldn't read the file";

            auto size = bigEndian ? readBE32 (header + 4) : readLE32 (header + 4);

            if ((juce::int64) size + 8 != f.getSize())
                return "the file's size doesn't match its header";

            auto newSize = (juce::uint64) size + chunks.size();

            if (newSize > 0xffffffffu)
                return "the file is too big to add to";

            juce::FileOutputStream out (f);                     // opens at the end

            if (! out.openedOk() || ! out.write (chunks.data(), chunks.size()))
                return "couldn't write the file";

            out.flush();
            Bytes sizeBytes;
            bigEndian ? be32 (sizeBytes, (uint32_t) newSize) : le32 (sizeBytes, (uint32_t) newSize);

            if (! out.setPosition (4) || ! out.write (sizeBytes.data(), 4))
                return "couldn't update the file's size";

            out.flush();
            return out.getStatus().wasOk() ? juce::String() : out.getStatus().getErrorMessage();
        }

        inline void chunk (Bytes& b, const char* id, const Bytes& body, bool bigEndian)
        {
            add (b, id, 4);
            bigEndian ? be32 (b, (uint32_t) body.size()) : le32 (b, (uint32_t) body.size());
            b.insert (b.end(), body.begin(), body.end());

            if (body.size() % 2 != 0)
                b.push_back (0);
        }

        inline Bytes text (const juce::String& s, bool terminated)
        {
            Bytes b;
            addUtf8 (b, s);

            if (terminated)
                b.push_back (0);

            return b;
        }

        //==============================================================================
        inline juce::String writeWav (const juce::File& f, const Notice& n)
        {
            Bytes list, chunks;
            add (list, "INFO");
            chunk (list, "ICOP", text (n.asciiCopyright(), true), false);
            chunk (list, "ICMT", text (n.terms(), true), false);
            chunk (chunks, "LIST", list, false);
            chunk (chunks, "_PMX", text (n.xmp(), false), false);
            return appendChunks (f, chunks, false);
        }

        inline juce::String writeAiff (const juce::File& f, const Notice& n)
        {
            Bytes appl, chunks;
            add (appl, "XMP ");
            addUtf8 (appl, n.xmp());
            chunk (chunks, "(c) ", text (n.bareCopyright(), false), true);
            chunk (chunks, "ANNO", text (n.terms(), false), true);
            chunk (chunks, "APPL", appl, true);
            return appendChunks (f, chunks, true);
        }

        //==============================================================================
        /** A Vorbis comment packet body (FLAC's has no packet type or framing bit around it), with
            the notice's comments replacing any with the same names. */
        inline Bytes vorbisCommentBody (const juce::String& vendor, juce::StringArray existing, const Notice& n)
        {
            auto ours = n.vorbisComments();

            for (int i = existing.size(); --i >= 0;)
                for (auto& c : ours)
                    if (existing[i].upToFirstOccurrenceOf ("=", false, false).equalsIgnoreCase (c.upToFirstOccurrenceOf ("=", false, false)))
                        existing.remove (i);

            existing.addArray (ours);

            Bytes b;
            le32 (b, (uint32_t) vendor.getNumBytesAsUTF8());
            addUtf8 (b, vendor);
            le32 (b, (uint32_t) existing.size());

            for (auto& c : existing)
            {
                le32 (b, (uint32_t) c.getNumBytesAsUTF8());
                addUtf8 (b, c);
            }

            return b;
        }

        /** Reads a Vorbis comment body: false if it's malformed. */
        inline bool parseVorbisComments (const uint8_t* p, size_t size, juce::String& vendor, juce::StringArray& comments)
        {
            size_t pos = 0;
            auto str = [&] (juce::String& out)
            {
                if (pos + 4 > size) return false;
                auto len = readLE32 (p + pos);
                pos += 4;
                if (len > size - pos) return false;
                out = juce::String::fromUTF8 ((const char*) p + pos, (int) len);
                pos += len;
                return true;
            };

            if (! str (vendor) || pos + 4 > size)
                return false;

            auto count = readLE32 (p + pos);
            pos += 4;

            for (uint32_t i = 0; i < count; ++i)
            {
                juce::String c;
                if (! str (c)) return false;
                comments.add (c);
            }

            return true;
        }

        inline juce::String writeFlac (const juce::File& f, const Notice& n)
        {
            Bytes in;

            if (! readAll (f, in) || in.size() < 8 || std::memcmp (in.data(), "fLaC", 4) != 0)
                return "couldn't read the FLAC file";

            struct Block { int type; size_t start, length; };
            std::vector<Block> blocks;
            size_t pos = 4;

            for (bool last = false; ! last;)
            {
                if (pos + 4 > in.size())
                    return "the FLAC file's metadata is cut short";

                last = (in[pos] & 0x80) != 0;
                auto length = ((size_t) in[pos + 1] << 16) | ((size_t) in[pos + 2] << 8) | in[pos + 3];

                if (pos + 4 + length > in.size())
                    return "the FLAC file's metadata is cut short";

                blocks.push_back ({ in[pos] & 0x7f, pos + 4, length });
                pos += 4 + length;
            }

            if (blocks.empty() || blocks.front().type != 0)
                return "the FLAC file doesn't start with its stream info";

            juce::String vendor ("Elsewhere");
            juce::StringArray comments;

            for (auto& b : blocks)
                if (b.type == 4)
                    parseVorbisComments (in.data() + b.start, b.length, vendor, comments);

            std::vector<Bytes> bodies;
            std::vector<int> types;

            for (auto& b : blocks)
            {
                if (b.type == 4)
                    continue;

                bodies.emplace_back (in.begin() + (std::ptrdiff_t) b.start, in.begin() + (std::ptrdiff_t) (b.start + b.length));
                types.push_back (b.type);

                if (b.type == 0)    // the comments go straight after the stream info
                {
                    bodies.push_back (vorbisCommentBody (vendor, comments, n));
                    types.push_back (4);
                }
            }

            Bytes out;
            add (out, "fLaC");

            for (size_t i = 0; i < bodies.size(); ++i)
            {
                if (bodies[i].size() >= (1u << 24))
                    return "the comments are too long for FLAC";

                out.push_back ((uint8_t) (types[i] | (i == bodies.size() - 1 ? 0x80 : 0)));
                out.push_back ((uint8_t) (bodies[i].size() >> 16));
                out.push_back ((uint8_t) (bodies[i].size() >> 8));
                out.push_back ((uint8_t) bodies[i].size());
                out.insert (out.end(), bodies[i].begin(), bodies[i].end());
            }

            out.insert (out.end(), in.begin() + (std::ptrdiff_t) pos, in.end());
            return replaceWith (f, out);
        }

        //==============================================================================
        inline uint32_t oggCrc (const uint8_t* p, size_t n)
        {
            static const auto table = []
            {
                std::array<uint32_t, 256> t {};

                for (uint32_t i = 0; i < 256; ++i)
                {
                    auto r = i << 24;
                    for (int k = 0; k < 8; ++k)
                        r = (r & 0x80000000u) ? (r << 1) ^ 0x04c11db7u : (r << 1);
                    t[i] = r;
                }

                return t;
            }();

            uint32_t crc = 0;

            for (size_t i = 0; i < n; ++i)
                crc = (crc << 8) ^ table[((crc >> 24) ^ p[i]) & 0xff];

            return crc;
        }

        inline juce::String writeOgg (const juce::File& f, const Notice& n)
        {
            Bytes in;

            if (! readAll (f, in))
                return "couldn't read the Ogg file";

            struct Page { size_t start, end; uint8_t type; uint32_t serial, seq; Bytes lacing; size_t body; };
            std::vector<Page> pages;

            for (size_t pos = 0; pos < in.size();)
            {
                if (pos + 27 > in.size() || std::memcmp (in.data() + pos, "OggS", 4) != 0)
                    return "the Ogg file is damaged";

                Page pg;
                pg.start = pos;
                pg.type = in[pos + 5];
                pg.serial = readLE32 (in.data() + pos + 14);
                pg.seq = readLE32 (in.data() + pos + 18);
                int segments = in[pos + 26];

                if (pos + 27 + (size_t) segments > in.size())
                    return "the Ogg file is damaged";

                pg.lacing.assign (in.begin() + (std::ptrdiff_t) (pos + 27), in.begin() + (std::ptrdiff_t) (pos + 27 + (size_t) segments));
                pg.body = pos + 27 + (size_t) segments;
                size_t bodyLength = 0;

                for (auto l : pg.lacing)
                    bodyLength += l;

                pg.end = pg.body + bodyLength;

                if (pg.end > in.size())
                    return "the Ogg file is damaged";

                pages.push_back (std::move (pg));
                pos = pages.back().end;
            }

            // The three Vorbis headers: identification, comments, setup. The first is alone on the
            // first page, and the setup ends a page (the encoder flushes before the audio starts)
            std::vector<Bytes> packets (1);
            size_t headerPages = 0;

            for (; headerPages < pages.size() && packets.size() <= 3; ++headerPages)
            {
                auto& pg = pages[headerPages];

                if (pg.serial != pages.front().serial)
                    return "the Ogg file has more than one stream";

                size_t at = pg.body;

                for (size_t i = 0; i < pg.lacing.size(); ++i)
                {
                    if (packets.size() > 3)
                        return "the Ogg file's headers share a page with its audio";

                    packets.back().insert (packets.back().end(), in.begin() + (std::ptrdiff_t) at, in.begin() + (std::ptrdiff_t) (at + pg.lacing[i]));
                    at += pg.lacing[i];

                    if (pg.lacing[i] < 255)
                        packets.emplace_back();
                }

                if (headerPages == 0 && packets.size() != 2)
                    return "the Ogg file's first page isn't its identification header";
            }

            packets.pop_back();

            if (packets.size() != 3 || packets[1].size() < 7 || packets[1][0] != 3 || std::memcmp (packets[1].data() + 1, "vorbis", 6) != 0)
                return "the Ogg file isn't Vorbis";

            juce::String vendor;
            juce::StringArray comments;

            if (! parseVorbisComments (packets[1].data() + 7, packets[1].size() - 7, vendor, comments))
                return "the Ogg file's comments are damaged";

            Bytes comment;
            comment.push_back (3);
            add (comment, "vorbis");
            auto body = vorbisCommentBody (vendor, comments, n);
            comment.insert (comment.end(), body.begin(), body.end());
            comment.push_back (1);     // framing bit

            Bytes out (in.begin() + (std::ptrdiff_t) pages[0].start, in.begin() + (std::ptrdiff_t) pages[0].end);
            auto serial = pages.front().serial;
            uint32_t seq = 1;

            auto writePage = [&] (uint8_t type, uint64_t granule, uint32_t pageSeq, const Bytes& lacing, const uint8_t* data, size_t size)
            {
                auto start = out.size();
                add (out, "OggS");
                out.push_back (0);
                out.push_back (type);
                for (int i = 0; i < 8; ++i) out.push_back ((uint8_t) (granule >> (8 * i)));
                le32 (out, serial);
                le32 (out, pageSeq);
                le32 (out, 0);
                out.push_back ((uint8_t) lacing.size());
                out.insert (out.end(), lacing.begin(), lacing.end());
                add (out, data, size);
                auto crc = oggCrc (out.data() + start, out.size() - start);
                for (int i = 0; i < 4; ++i) out[start + 22 + (size_t) i] = (uint8_t) (crc >> (8 * i));
            };

            // The new comments and the setup, paged again: a page of up to 255 segments, its
            // granule position 0 where a header packet ends on it and -1 where none does
            {
                Bytes lacing, data;
                bool continued = false, startsContinued = false, packetEnds = false;

                auto flush = [&]
                {
                    writePage (startsContinued ? 1 : 0, packetEnds ? 0 : ~(uint64_t) 0, seq++, lacing, data.data(), data.size());
                    lacing.clear();
                    data.clear();
                    startsContinued = continued;
                    packetEnds = false;
                };

                for (auto* p : { &comment, &packets[2] })
                {
                    size_t left = p->size(), at = 0;

                    do
                    {
                        auto len = std::min<size_t> (left, 255);
                        lacing.push_back ((uint8_t) len);
                        add (data, p->data() + at, len);
                        at += len;
                        left -= len;
                        continued = len == 255;
                        packetEnds = packetEnds || ! continued;

                        if (lacing.size() == 255)
                            flush();
                    }
                    while (continued);
                }

                if (! lacing.empty())
                    flush();
            }

            // The audio pages follow, renumbered when the headers took a different number of pages
            for (size_t i = headerPages; i < pages.size(); ++i)
            {
                auto& pg = pages[i];
                auto start = out.size();
                out.insert (out.end(), in.begin() + (std::ptrdiff_t) pg.start, in.begin() + (std::ptrdiff_t) pg.end);

                if (pg.serial == serial)
                {
                    auto newSeq = pg.seq + seq - (uint32_t) headerPages;

                    for (int k = 0; k < 4; ++k) out[start + 18 + (size_t) k] = (uint8_t) (newSeq >> (8 * k));
                    for (int k = 0; k < 4; ++k) out[start + 22 + (size_t) k] = 0;
                    auto crc = oggCrc (out.data() + start, out.size() - start);
                    for (int k = 0; k < 4; ++k) out[start + 22 + (size_t) k] = (uint8_t) (crc >> (8 * k));
                }
            }

            return replaceWith (f, out);
        }

        //==============================================================================
        /** A string as ID3 wants it in encoding 1: UTF-16 with a byte order mark. */
        inline void utf16 (Bytes& b, const juce::String& s, bool terminated)
        {
            b.push_back (0xff);
            b.push_back (0xfe);

            for (auto p = s.toUTF16(); ! p.isEmpty();)
            {
                auto c = p.getAddress();
                ++p;

                for (auto* u = c; u < p.getAddress(); ++u)
                {
                    b.push_back ((uint8_t) (*u & 0xff));
                    b.push_back ((uint8_t) (*u >> 8));
                }
            }

            if (terminated)
            {
                b.push_back (0);
                b.push_back (0);
            }
        }

        inline void syncsafe (Bytes& b, uint32_t v)
        {
            for (int i = 3; i >= 0; --i)
                b.push_back ((uint8_t) ((v >> (7 * i)) & 0x7f));
        }

        inline juce::String writeMp3 (const juce::File& f, const Notice& n)
        {
            Bytes in;

            if (! readAll (f, in))
                return "couldn't read the MP3 file";

            // Join a tag that's already there (the encoder writes one when given tags), else start one
            int version = 3;
            size_t audioStart = 0;
            Bytes frames;

            if (in.size() >= 10 && std::memcmp (in.data(), "ID3", 3) == 0)
            {
                version = in[3];

                if ((version != 3 && version != 4) || in[5] != 0)
                    return "the MP3 file's ID3 tag is a kind this can't add to";

                auto size = ((size_t) (in[6] & 0x7f) << 21) | ((size_t) (in[7] & 0x7f) << 14) | ((size_t) (in[8] & 0x7f) << 7) | (in[9] & 0x7f);

                if (10 + size > in.size())
                    return "the MP3 file's ID3 tag is damaged";

                // Its frames, without the padding after them
                size_t pos = 10;

                while (pos + 10 <= 10 + size && in[pos] != 0)
                {
                    auto fs = version == 4 ? (((size_t) (in[pos + 4] & 0x7f) << 21) | ((size_t) (in[pos + 5] & 0x7f) << 14) | ((size_t) (in[pos + 6] & 0x7f) << 7) | (in[pos + 7] & 0x7f))
                                           : (size_t) readBE32 (in.data() + pos + 4);

                    if (pos + 10 + fs > 10 + size)
                        return "the MP3 file's ID3 tag is damaged";

                    juce::String id ((const char*) in.data() + pos, 4);

                    if (id != "TCOP")    // the notice's copyright replaces any already there
                        frames.insert (frames.end(), in.begin() + (std::ptrdiff_t) pos, in.begin() + (std::ptrdiff_t) (pos + 10 + fs));

                    pos += 10 + fs;
                }

                audioStart = 10 + size;
            }

            auto frame = [&] (const char* id, const Bytes& body)
            {
                add (frames, id, 4);
                version == 4 ? syncsafe (frames, (uint32_t) body.size()) : be32 (frames, (uint32_t) body.size());
                frames.push_back (0);
                frames.push_back (0);
                frames.insert (frames.end(), body.begin(), body.end());
            };

            Bytes tcop { 1 }, comm { 1, 'e', 'n', 'g' }, txxx { 1 }, priv;
            utf16 (tcop, n.bareCopyright(), false);
            utf16 (comm, {}, true);
            utf16 (comm, n.terms(), false);
            utf16 (txxx, "TDM_RESERVATION", true);
            utf16 (txxx, "1", false);
            add (priv, "XMP", 4);      // with its terminating zero
            addUtf8 (priv, n.xmp());

            frame ("TCOP", tcop);
            frame ("COMM", comm);
            frame ("TXXX", txxx);
            frame ("PRIV", priv);

            if (frames.size() >= (1u << 28))
                return "the ID3 tag would be too big";

            Bytes out { 'I', 'D', '3', (uint8_t) version, 0, 0 };
            syncsafe (out, (uint32_t) frames.size());
            out.insert (out.end(), frames.begin(), frames.end());
            out.insert (out.end(), in.begin() + (std::ptrdiff_t) audioStart, in.end());
            return replaceWith (f, out);
        }
    }

    //==============================================================================
    /** Writes the notice into a finished WAV, AIFF, FLAC, Ogg Vorbis or MP3 file. Returns why it
        couldn't, or an empty string when it did. */
    inline juce::String write (const juce::File& f, const Notice& n)
    {
        char magic[12] = {};

        if (f.getSize() < 12 || juce::FileInputStream (f).read (magic, 12) != 12)
            return "couldn't read the file";

        if (std::memcmp (magic, "RIFF", 4) == 0 && std::memcmp (magic + 8, "WAVE", 4) == 0)   return detail::writeWav (f, n);
        if (std::memcmp (magic, "RF64", 4) == 0)                                              return "WAV files over 4 GB can't carry it";
        if (std::memcmp (magic, "FORM", 4) == 0)                                              return detail::writeAiff (f, n);
        if (std::memcmp (magic, "fLaC", 4) == 0)                                              return detail::writeFlac (f, n);
        if (std::memcmp (magic, "OggS", 4) == 0)                                              return detail::writeOgg (f, n);
        if (f.hasFileExtension ("mp3"))                                                       return detail::writeMp3 (f, n);

        return "it's not a format that can carry it";
    }
}

} // namespace elsewhere
