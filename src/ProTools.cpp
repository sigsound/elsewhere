// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ProTools.h"

namespace elsewhere::ProTools
{
namespace
{
    /** libptformat's C entry point (libs/ptformat-c), loaded from the app bundle. */
    struct Library
    {
        Library()
        {
           #if JUCE_MAC
            auto file = juce::File::getSpecialLocation (juce::File::currentApplicationFile).getChildFile ("Contents/Frameworks/libptformat.dylib");
           #elif JUCE_WINDOWS
            auto file = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("ptformat.dll");
           #else
            auto file = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getSiblingFile ("libptformat.so");
           #endif

            if (! lib.open (file.getFullPathName()))
                return;

            abiVersion = reinterpret_cast<int (*)()> (lib.getFunction ("ptformat_c_abi_version"));
            load = reinterpret_cast<char* (*) (const char*, int*)> (lib.getFunction ("ptformat_c_load"));
            free = reinterpret_cast<void (*) (char*)> (lib.getFunction ("ptformat_c_free"));

            if (abiVersion == nullptr || load == nullptr || free == nullptr || abiVersion() != 1)
                load = nullptr;
        }

        bool isLoaded() const       { return load != nullptr; }

        static Library& get()
        {
            static Library instance;
            return instance;
        }

        juce::DynamicLibrary lib;
        int (*abiVersion)() = nullptr;
        char* (*load) (const char*, int*) = nullptr;
        void (*free) (char*) = nullptr;
    };
}

bool isAvailable()     { return Library::get().isLoaded(); }

//==============================================================================
struct Importer::Impl
{
    struct Region
    {
        juce::String name, file;
        juce::int64 start = 0, offset = 0, length = 0;
    };

    struct Track
    {
        juce::String name;
        std::vector<Region> regions;
        std::optional<double> pan;
    };

    juce::File source;
    juce::String error;
    juce::var data;
    double rate = 48000.0;
    int version = 0, midiTracks = 0;
    std::vector<Track> tracks;
    std::map<juce::String, juce::File> found, local;     // file name -> where it is / its copy in the project
    juce::StringArray missing;

    void read()
    {
        auto& library = Library::get();

        if (! library.isLoaded())
        {
            error = "Elsewhere's Pro Tools session reader (the ptformat library) is missing from this copy of the app. Reinstall Elsewhere.";
            return;
        }

        // The reader opens files with fopen, which on Windows can't take every file name:
        // give it a copy with a plain one
        auto temp = juce::File::getSpecialLocation (juce::File::tempDirectory)
                        .getNonexistentChildFile ("elsewhere-session", source.getFileExtension(), false);

        if (! source.copyFileTo (temp))
        {
            error = "Couldn't read \"" + source.getFileName() + "\".";
            return;
        }

        int code = 0;
        auto* json = library.load (temp.getFullPathName().toRawUTF8(), &code);
        temp.deleteFile();

        if (json == nullptr)
        {
            error = code == -3 ? "\"" + source.getFileName() + "\" was saved by a version of Pro Tools that Elsewhere can't read yet."
                               : "\"" + source.getFileName() + "\" couldn't be read as a Pro Tools session.";
            return;
        }

        data = juce::JSON::parse (juce::String::fromUTF8 (json));
        library.free (json);

        version = (int) data["version"];
        rate = (double) data["sessionRate"];

        if (rate <= 0.0)
            rate = 48000.0;

        if (auto* midi = data["midiTracks"].getArray())
        {
            std::set<int> seen;
            for (auto& m : *midi)
                seen.insert ((int) m["index"]);
            midiTracks = (int) seen.size();
        }

        // One entry per region placement: gather them by track, in the session's order
        std::map<int, Track> byIndex;

        if (auto* list = data["tracks"].getArray())
        {
            for (auto& t : *list)
            {
                auto& track = byIndex[(int) t["index"]];
                track.name = t["name"].toString();
                track.regions.push_back ({ t["region"].toString(), t["file"].toString(),
                                           (juce::int64) t["start"], (juce::int64) t["offset"], (juce::int64) t["length"] });
            }
        }

        // A stereo track plays a mono .L and .R file: give each side its own track, panned apart
        for (auto& [index, track] : byIndex)
        {
            Track left { track.name + " L", {}, -1.0 }, right { track.name + " R", {}, 1.0 }, other { track.name, {}, {} };

            for (auto& r : track.regions)
            {
                auto side = SessionImport::stereoSideFromName (r.file);
                (side == -1.0 ? left : side == 1.0 ? right : other).regions.push_back (r);
            }

            if (! left.regions.empty() && ! right.regions.empty())
            {
                for (auto& r : other.regions)
                    left.regions.push_back (r);

                tracks.push_back (left);
                tracks.push_back (right);
            }
            else
            {
                tracks.push_back (track);
            }
        }

        if (tracks.empty())
        {
            error = "\"" + source.getFileName() + "\" has no audio clips on its tracks.";
            return;
        }

        // The audio lives in the session folder's "Audio Files" (or somewhere near it)
        std::map<juce::String, juce::File> index;
        auto folder = source.getParentDirectory();

        for (auto& t : tracks)
        {
            for (auto& r : t.regions)
            {
                if (found.count (r.file) > 0 || missing.contains (r.file))
                    continue;

                auto f = folder.getChildFile ("Audio Files").getChildFile (r.file);

                if (! f.existsAsFile())
                    f = SessionImport::findFileNear (folder, r.file, index);

                if (f.existsAsFile())
                    found[r.file] = f;
                else
                    missing.add (r.file);
            }
        }
    }

    bool extract (const juce::File& dest, const std::function<bool (double)>& progress)
    {
        dest.createDirectory();
        juce::int64 total = 0, done = 0;

        for (auto& [name, f] : found)
            total += f.getSize();

        juce::HeapBlock<char> buffer (1 << 20);

        for (auto& [name, f] : found)
        {
            auto copy = dest.getNonexistentChildFile (f.getFileNameWithoutExtension(), f.getFileExtension(), false);
            std::unique_ptr<juce::InputStream> in (f.createInputStream());
            juce::FileOutputStream out (copy);

            if (in == nullptr || ! out.openedOk())
                continue;

            for (int n; (n = in->read (buffer, 1 << 20)) > 0;)
            {
                out.write (buffer, (size_t) n);
                done += n;

                if (progress && ! progress (total > 0 ? (double) done / (double) total : 1.0))
                    return false;
            }

            out.flush();
            local[name] = copy;
        }

        return true;
    }

    SessionImport::Summary build (te::Edit& edit)
    {
        SessionImport::Summary summary;
        summary.applicationName = "Pro Tools";
        summary.missingFiles = missing;

        SessionImport::removePlaceholderTracks (edit);

        te::Track* preceding = nullptr;
        juce::Array<juce::File> used;

        for (auto& t : tracks)
        {
            auto at = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, preceding), nullptr);

            if (at == nullptr)
                continue;

            preceding = at.get();
            ++summary.tracks;
            at->setName (t.name);
            at->setColour (EngineHelpers::trackColourFor (te::getAudioTracks (edit).indexOf (at.get())));

            if (t.pan)
                if (auto* volume = at->getVolumePlugin())
                    volume->setPan ((float) *t.pan);

            for (auto& r : t.regions)
            {
                auto it = local.find (r.file);

                if (it == local.end() || r.length <= 0)
                    continue;

                used.addIfNotAlreadyThere (it->second);
                auto start = (double) r.start / rate;
                auto name = r.name.isNotEmpty() ? r.name : it->second.getFileNameWithoutExtension();

                if (SessionImport::addAudioClip (*at, name, it->second, start, start + (double) r.length / rate, (double) r.offset / rate) != nullptr)
                    ++summary.audioClips;
            }
        }

        SessionImport::finish (edit, {}, used, summary);

        summary.notes.add ("Pro Tools sessions don't give Elsewhere their tempo. Set it in the transport bar: the audio stays where it is.");
        summary.notes.add ("Fades, clip gain, volume, pan and plug-ins aren't brought across from a Pro Tools session"
                           " (stereo tracks are split into left and right tracks, panned apart).");

        if (midiTracks > 0)
            summary.notes.add (juce::String (midiTracks) + (midiTracks == 1 ? " MIDI track was" : " MIDI tracks were")
                                + " left out: MIDI from Pro Tools sessions isn't supported yet.");

        return summary;
    }
};

//==============================================================================
Importer::Importer (const juce::File& f) : impl (std::make_unique<Impl>())
{
    impl->source = f;
    impl->read();
}

Importer::~Importer() = default;

juce::String Importer::getError() const     { return impl->error; }
juce::var Importer::getSessionData() const  { return impl->data; }

bool Importer::extractAudio (const juce::File& destDir, std::function<bool (double)> progress)
{
    return impl->error.isEmpty() && impl->extract (destDir, progress);
}

SessionImport::Summary Importer::build (te::Edit& edit)
{
    return impl->build (edit);
}

} // namespace elsewhere::ProTools
