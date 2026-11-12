// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "CubaseTests.h"
#include "Cubase.h"
#include "Project.h"

namespace elsewhere::CubaseTests
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

    /** Writes Cubase's archive format: big-endian, objects introduced by their class name the
        first time and by the offset of that declaration afterwards. */
    struct ArchiveWriter
    {
        std::vector<juce::uint8> bytes;
        std::map<juce::String, size_t> declared;

        void u8 (int v)         { bytes.push_back ((juce::uint8) v); }
        void u16 (int v)        { u8 (v >> 8); u8 (v); }
        void u32 (juce::uint32 v) { u16 ((int) (v >> 16)); u16 ((int) (v & 0xffff)); }

        void f64 (double d)
        {
            juce::uint64 b;
            std::memcpy (&b, &d, 8);
            u32 ((juce::uint32) (b >> 32));
            u32 ((juce::uint32) b);
        }

        void f32 (float f)
        {
            juce::uint32 b;
            std::memcpy (&b, &f, 4);
            u32 (b);
        }

        void string (const juce::String& s)
        {
            auto utf8 = s.toStdString();
            u32 ((juce::uint32) utf8.size() + 1);
            for (auto c : utf8) u8 ((juce::uint8) c);
            u8 (0);
        }

        void zeros (int n)      { for (int i = 0; i < n; ++i) u8 (0); }

        /** Starts an object; returns where its length goes (what references point at). */
        size_t begin (const juce::String& cls, int version = 0)
        {
            if (auto it = declared.find (cls); it != declared.end())
            {
                u32 (0x80000000u | (juce::uint32) it->second);
            }
            else
            {
                declared[cls] = bytes.size();
                u32 (0xffffffffu);
                string (cls);
                u16 (version);
            }

            auto lengthAt = bytes.size();
            u32 (0);
            return lengthAt;
        }

        void end (size_t lengthAt)
        {
            auto length = (juce::uint32) (bytes.size() - lengthAt - 4);
            for (int i = 0; i < 4; ++i)
                bytes[lengthAt + (size_t) i] = (juce::uint8) (length >> (24 - 8 * i));
        }
    };

    void chunk (juce::MemoryOutputStream& out, const char* tag, const std::vector<juce::uint8>& body)
    {
        out.write (tag, 4);
        out.writeIntBigEndian ((int) body.size());
        out.write (body.data(), body.size());
    }

    void root (juce::MemoryOutputStream& out, const juce::String& name, const juce::String& cls)
    {
        std::vector<juce::uint8> b;
        auto add = [&b] (const juce::String& s)
        {
            auto n = (juce::uint32) s.length();
            for (int i = 0; i < 4; ++i) b.push_back ((juce::uint8) (n >> (24 - 8 * i)));
            for (auto c : s.toStdString()) b.push_back ((juce::uint8) c);
        };
        add (name);
        add (cls);
        chunk (out, "ROOT", b);
    }

    /** An FNPath: a file name and the folder it was recorded in. */
    size_t writePath (ArchiveWriter& w, const juce::String& file, const juce::String& folder)
    {
        auto path = w.begin ("FNPath");
        w.string (file);
        w.string ("wav");
        w.zeros (6);
        w.string (folder);
        w.end (path);
        return path;
    }

    /** An AudioFile: length, format and sample rate, then its path (a reference here). */
    size_t writeAudioFile (ArchiveWriter& w, size_t path)
    {
        auto audio = w.begin ("AudioFile");
        w.u32 (0);
        w.u32 (441000);      // frames
        w.u16 (16);
        w.u16 (2);
        w.u16 (1);
        w.f32 (44100.0f);
        w.u32 ((juce::uint32) path);
        w.end (audio);
        return audio;
    }

    void writeSegment (ArchiveWriter& w, size_t audioFile, juce::int64 fileStart, juce::int64 length, juce::int64 at)
    {
        auto seg = w.begin ("AClusterSegment");
        w.u32 ((juce::uint32) audioFile);
        w.u32 ((juce::uint32) (fileStart >> 32)); w.u32 ((juce::uint32) fileStart);
        w.u32 ((juce::uint32) (length >> 32));    w.u32 ((juce::uint32) length);
        w.u32 ((juce::uint32) (at >> 32));        w.u32 ((juce::uint32) at);
        w.end (seg);
    }

    /** An audio clip as Cubase writes it: name, path, and a cluster of segments placing parts
        of audio files along the clip. "edited" adds a processed second file for 1..2 s. */
    size_t writeClip (ArchiveWriter& w, const juce::String& name, const juce::String& file, const juce::String& folder,
                      const juce::String& editedFile = {})
    {
        auto clip = w.begin ("PAudioClip", 4);
        w.string (name);
        w.zeros (8);
        auto path = writePath (w, file, folder);
        auto cluster = w.begin ("AudioCluster");
        w.zeros (10);
        auto original = writeAudioFile (w, path);

        if (editedFile.isEmpty())
        {
            writeSegment (w, original, 0, 441000, 0);
        }
        else
        {
            auto editPath = writePath (w, editedFile, folder);
            auto edited = writeAudioFile (w, editPath);
            writeSegment (w, original, 0, 44100, 0);
            writeSegment (w, edited, 0, 44100, 44100);
            writeSegment (w, original, 88200, 352800, 88200);
        }

        w.end (cluster);
        w.end (clip);
        return clip;
    }

    void writeEvent (ArchiveWriter& w, double start, double lengthSamples, double offsetSamples,
                     std::function<void()> clip)
    {
        auto e = w.begin ("MAudioEvent", 3);
        w.u16 (0);
        w.f64 (start);
        w.f64 (lengthSamples);
        w.f64 (offsetSamples);
        clip();
        w.zeros (32);
        w.f32 (1.0f);
        w.zeros (4);
        w.end (e);
    }

    juce::MemoryBlock writeProject (const juce::File& otherFolder)
    {
        ArchiveWriter w;
        auto arrangement = w.begin ("PArrangement", 3);
        auto rootObj = w.begin ("MRoot", 5);
        w.zeros (10);
        w.f64 (600.0);
        w.zeros (8);
        auto list = w.begin ("MTrackList");
        w.zeros (22);

        // Musical track: tempo 100, so 480 ticks = 0.6 s
        auto drums = w.begin ("MAudioTrackEvent", 1);
        w.zeros (26);
        auto drumList = w.begin ("MListNode");
        w.string ("Drums");
        w.u32 (0);

        auto tempo = w.begin ("MTempoTrackEvent", 2);
        w.u32 (1);
        w.zeros (22);
        w.f32 (100.0f);
        w.u32 (1);
        w.end (tempo);

        auto sig = w.begin ("MSignatureTrackEvent", 2);
        w.u32 (1);
        w.u32 (0);
        w.u16 (3);
        w.u16 (4);
        w.zeros (12);
        w.end (sig);

        size_t drumClip = 0;
        writeEvent (w, 960, 44100, 22050, [&] { drumClip = writeClip (w, "drum take", "drums.wav", "/Volumes/Old Drive/Song/Audio/"); });
        writeEvent (w, 2400, 88200, 0, [&] { w.u32 ((juce::uint32) drumClip); });          // the same clip, by reference
        writeEvent (w, 3840, 44100, 132300, [&] { w.u32 ((juce::uint32) drumClip); });     // overlaps the one before
        w.end (drumList);
        w.end (drums);

        // Linear track: starts in seconds; its file is only where its folder says
        auto vox = w.begin ("MAudioTrackEvent", 1);
        w.zeros (26);
        auto voxList = w.begin ("MListNode");
        w.string ("Vox");
        w.u32 (1);
        w.f64 (1.0);
        writeEvent (w, 7.5, 44100, 0, [&] { writeClip (w, "vox take", "vox.wav", otherFolder.getFullPathName() + "/"); });
        w.end (voxList);
        w.end (vox);

        // A clip partly processed: 0.5 s in, for 2 s, it plays the original, the edited file, then the original
        auto keys = w.begin ("MAudioTrackEvent", 1);
        w.zeros (26);
        auto keysList = w.begin ("MListNode");
        w.string ("Keys");
        w.u32 (0);
        writeEvent (w, 0, 88200, 22050, [&] { writeClip (w, "keys take", "keys.wav", "/Volumes/Old Drive/Song/Audio/", "keys-edit.wav"); });
        w.end (keysList);
        w.end (keys);

        auto gone = w.begin ("MAudioTrackEvent", 1);
        w.zeros (26);
        auto goneList = w.begin ("MListNode");
        w.string ("Gone");
        w.u32 (0);
        writeEvent (w, 0, 44100, 0, [&] { writeClip (w, "gone", "missing.wav", "/nowhere/"); });
        w.end (goneList);
        w.end (gone);

        w.end (list);
        w.end (rootObj);
        w.end (arrangement);

        ArchiveWriter version;
        auto v = version.begin ("PAppVersion", 1);
        version.string ("Cubase Studio");
        version.string ("Version 4.1.1");
        version.end (v);

        juce::MemoryOutputStream body;
        body.write ("NUND", 4);
        root (body, "Version", "PAppVersion");
        chunk (body, "ARCH", version.bytes);
        root (body, "Arrangement1", "PArrangement");
        chunk (body, "ARCH", w.bytes);

        juce::MemoryOutputStream file;
        file.write ("RIFF", 4);
        file.writeIntBigEndian ((int) body.getDataSize());
        file << body.getMemoryBlock();
        return file.getMemoryBlock();
    }

    void writeSine (const juce::File& f)
    {
        juce::AudioBuffer<float> b (2, 441000);
        for (int i = 0; i < b.getNumSamples(); ++i)
            for (int ch = 0; ch < 2; ++ch)
                b.setSample (ch, i, 0.3f * (float) std::sin (juce::MathConstants<double>::twoPi * 220.0 * i / 44100.0));

        f.getParentDirectory().createDirectory();
        f.deleteFile();
        std::unique_ptr<juce::OutputStream> out (f.createOutputStream());
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (out, juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16));
        writer->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    te::AudioTrack* findTrack (te::Edit& edit, const juce::String& name)
    {
        for (auto t : te::getAudioTracks (edit))
            if (t->getName() == name)
                return t;
        return nullptr;
    }

    juce::Array<te::WaveAudioClip*> clipsOf (te::AudioTrack* t)
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
}

int run (te::Engine& engine, const juce::File& outDir)
{
    Log log;
    outDir.createDirectory();
    auto projectDir = outDir.getChildFile ("Song");
    auto otherFolder = outDir.getChildFile ("Elsewhere");
    projectDir.deleteRecursively();
    otherFolder.deleteRecursively();

    writeSine (projectDir.getChildFile ("Audio/drums.wav"));
    writeSine (otherFolder.getChildFile ("vox.wav"));
    writeSine (projectDir.getChildFile ("Audio/keys.wav"));
    writeSine (projectDir.getChildFile ("Edits/keys-edit.wav"));
    auto cpr = projectDir.getChildFile ("Song.cpr");
    auto data = writeProject (otherFolder);
    cpr.replaceWithData (data.getData(), data.getSize());

    auto importDir = outDir.getChildFile ("import");
    importDir.deleteRecursively();
    auto editFile = Project::createProjectFolder (importDir, "Imported");

    Cubase::Importer importer (cpr);
    log.check (importer.getError().isEmpty(), "opened the project " + importer.getError());
    log.check (importer.extractAudio (editFile.getParentDirectory().getChildFile ("Samples/Imported")), "copied the audio");

    if (auto* xml = importer.getConvertedProject())
        xml->writeTo (outDir.getChildFile ("Song-as-dawproject.xml"));

    Project project (engine, editFile);
    auto& edit = project.getEdit();
    auto summary = importer.build (edit);
    log.info (summary.describe().replace ("\n", "\n        "));

    log.check (summary.applicationName == "Cubase Studio 4.1.1", "application: " + summary.applicationName);
    log.check (summary.missingFiles.contains ("missing.wav"), "reported the missing file");
    log.near (edit.tempoSequence.getTempo (0)->getBpm(), 100.0, "tempo");
    log.check (edit.tempoSequence.getTimeSig (0)->numerator == 3, "time signature 3/4");

    // 960 ticks at 100 bpm = 1.2 s; 1 s long from 0.5 s in. Then the shared clip at 3 s for 2 s,
    // cut off at 4.8 s where the next one (from 3 s in) starts
    auto d = clipsOf (findTrack (edit, "Drums"));
    log.check (d.size() == 3, "Drums has 3 clips: " + juce::String (d.size()));
    checkClip (log, d[0], 1.2, 2.2, 0.5, "musical clip");
    checkClip (log, d[1], 3.0, 4.8, 0.0, "shared clip, cut where the next one covers it");
    checkClip (log, d[2], 4.8, 5.8, 3.0, "overlapping later clip");

    if (d.size() == 3)
        log.check (d[1]->getOriginalFile().getFileName() == d[0]->getOriginalFile().getFileName(), "shared clip plays the same file");

    // Linear: 7.5 is seconds
    auto v = clipsOf (findTrack (edit, "Vox"));
    log.check (v.size() == 1, "Vox has 1 clip (found through its folder)");
    checkClip (log, v[0], 7.5, 8.5, 0.0, "linear clip");

    // Through its segments: original 0.5..1 s, then the edited file 0..1 s, then the original from 2 s
    auto k = clipsOf (findTrack (edit, "Keys"));
    log.check (k.size() == 3, "processed clip plays as 3 pieces: " + juce::String (k.size()));
    checkClip (log, k[0], 0.0, 0.5, 0.5, "before the processed part");
    checkClip (log, k[1], 0.5, 1.5, 0.0, "the processed part");
    checkClip (log, k[2], 1.5, 2.0, 2.0, "after the processed part");

    if (k.size() == 3)
        log.check (k[1]->getOriginalFile().getFileName().startsWith ("keys-edit") && k[0]->getOriginalFile().getFileName().startsWith ("keys"),
                   "the processed part plays the edited file");

    log.text << "\n" << (log.failures == 0 ? "All passed" : juce::String (log.failures) + " failed") << "\n";
    outDir.getChildFile ("cubase-test.txt").replaceWithText (log.text);
    std::cout << log.text;
    return log.failures;
}

} // namespace elsewhere::CubaseTests
