// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"
#include "Export.h"
#include "Normalise.h"
#include "Project.h"
#include "devices/DeviceBase.h"
#include "devices/MasterDevice.h"

namespace elsewhere
{
//==============================================================================
/**
    A mastering project: the songs of an EP or album, each one an ordinary Elsewhere project,
    mastered together. It's a project folder like any other, marked as a mastering project in
    its file:

        My Album Project/
            My Album.elsewhere      <- the album: one track per song, in order
            Renders/                <- each song's mix, rendered from its own project
            Exports/My Album/       <- the mastered songs, numbered, and the whole album

    Each song is a track with one clip, which plays a render of that song's mix. The track holds
    where the song's project is (relative to the album when possible, so the two can move together)
    and the gap before it, and its device chain is the song's own mastering chain. The album's chain
    is the master track's, after every song's.

    When a song's project changes (its file is newer than the render), it's rendered again in the
    background: open the song (double-click it), work on it, come back, and the album has it.
*/
namespace Mastering
{
    inline const juce::Identifier masteringId ("elsewhereMastering"),       // on the edit: this is a mastering project
                                  targetId ("elsewhereMasteringTarget"),    // on the edit: the album's loudness, LUFS
                                  songPathId ("elsewhereSongPath"),         // the song's project, relative to the album
                                  songFileId ("elsewhereSongFile"),         // ...and where it was, in full
                                  gapId ("elsewhereSongGap"),               // seconds of silence before the song
                                  renderedFromId ("elsewhereSongRenderedFrom"),  // the project's time when it was rendered
                                  lengthId ("elsewhereSongLength"),         // seconds, of the last render
                                  mixLufsId ("elsewhereSongMixLufs"),       // the render, as it comes from the song
                                  masterLufsId ("elsewhereSongMasterLufs"), // after the song's and the album's chains
                                  masterPeakId ("elsewhereSongMasterPeak");  // true peak there, dBTP

    constexpr double defaultGap = 2.0, defaultTarget = -14.0;
    constexpr const char* rendersFolder = "Renders";

    inline bool isMastering (const te::Edit& edit)     { return (bool) edit.state.getProperty (masteringId, false); }

    /** Quick check of a project file, without loading it. */
    inline bool isMasteringFile (const juce::File& f)
    {
        juce::FileInputStream in (f);
        auto head = in.openedOk() ? in.readString().substring (0, 2000) : juce::String();
        return head.contains (juce::String (masteringId.toString()) + "=\"1\"");
    }

    inline juce::File folderOf (const te::Edit& edit)
    {
        return edit.editFileRetriever ? edit.editFileRetriever().getParentDirectory() : juce::File();
    }

    inline juce::String nameOf (const te::Edit& edit)
    {
        return edit.editFileRetriever ? edit.editFileRetriever().getFileNameWithoutExtension() : juce::String ("Album");
    }

    //==============================================================================
    // Where a song's project is

    struct SongRef
    {
        juce::String relative, absolute;
    };

    inline SongRef makeRef (const juce::File& song, const juce::File& albumFolder)
    {
        SongRef r;
        r.absolute = song.getFullPathName();

        // A song on another drive (Windows) can't be reached from here: the full path is all there is
        auto rel = song.getRelativePathFrom (albumFolder);

        if (! juce::File::isAbsolutePath (rel))
            r.relative = rel.replaceCharacter ('\\', '/');

        return r;
    }

    /** Finds a song: beside the album as it was (the folders moved together), else where it was.
        When neither is there, where it's expected, so the caller can say it's missing. */
    inline juce::File resolve (const SongRef& r, const juce::File& albumFolder)
    {
        if (r.relative.isNotEmpty())
            if (auto f = albumFolder.getChildFile (r.relative); f.existsAsFile())
                return f;

        if (r.absolute.isNotEmpty() && juce::File::isAbsolutePath (r.absolute))
            if (auto f = juce::File (r.absolute); f.existsAsFile())
                return f;

        if (r.relative.isNotEmpty())
            return albumFolder.getChildFile (r.relative);

        return juce::File::isAbsolutePath (r.absolute) ? juce::File (r.absolute) : juce::File();
    }

    /** When a song's project last changed: its file, or a collaborator's snapshot of it if newer
        (a shared project loads whichever has the most in it, see Project::chooseFileToLoad). */
    inline juce::int64 songModTime (const juce::File& song)
    {
        auto t = song.getLastModificationTime().toMilliseconds();

        for (auto& f : song.getParentDirectory().getChildFile ("Collab/snapshots").findChildFiles (juce::File::findFiles, false, "*" + song.getFileExtension()))
            t = std::max (t, f.getLastModificationTime().toMilliseconds());

        return t;
    }

    /** True if the render is missing, or was made from an older (or newer) version of the song. */
    inline bool needsRender (const juce::File& song, const juce::File& render, juce::int64 renderedFrom)
    {
        return ! render.existsAsFile() || songModTime (song) != renderedFrom;
    }

    //==============================================================================
    // Songs in the album

    inline bool isSong (const te::Track& t)
    {
        return t.state.hasProperty (songPathId) || t.state.hasProperty (songFileId);
    }

    /** The songs, in album order. */
    inline juce::Array<te::AudioTrack*> getSongs (te::Edit& edit)
    {
        juce::Array<te::AudioTrack*> songs;

        for (auto t : te::getAudioTracks (edit))
            if (isSong (*t))
                songs.add (t);

        return songs;
    }

    inline SongRef refOf (const te::Track& t)      { return { t.state[songPathId].toString(), t.state[songFileId].toString() }; }

    inline juce::File songFileOf (te::Track& t)    { return resolve (refOf (t), folderOf (t.edit)); }

    inline juce::String songNameOf (te::Track& t)
    {
        auto name = t.getName().trim();
        return name.isNotEmpty() ? name : songFileOf (t).getFileNameWithoutExtension();
    }

    inline te::WaveAudioClip* clipOf (te::AudioTrack& t)
    {
        for (auto c : t.getClips())
            if (auto w = dynamic_cast<te::WaveAudioClip*> (c))
                return w;

        return nullptr;
    }

    inline double gapOf (const te::Track& t)       { return (double) t.state.getProperty (gapId, defaultGap); }

    /** The song's length on the timeline: its clip, or its last render before there's a clip. */
    inline double lengthOf (te::AudioTrack& t)
    {
        if (auto c = clipOf (t))
            return c->getPosition().getLength().inSeconds();

        return (double) t.state.getProperty (lengthId, 0.0);
    }

    inline std::optional<double> getProperty (const te::Track& t, const juce::Identifier& id)
    {
        if (! t.state.hasProperty (id))
            return std::nullopt;

        return (double) t.state[id];
    }

    inline double targetOf (const te::Edit& edit)   { return (double) edit.state.getProperty (targetId, defaultTarget); }

    inline void setTarget (te::Edit& edit, double lufs)
    {
        edit.state.setProperty (targetId, juce::jlimit (-24.0, -6.0, lufs), &edit.getUndoManager());
    }

    /** The song's own mastering chain, and the album's (the master track's). */
    inline MasterDevice* chainOf (te::Track& t)
    {
        for (auto p : t.pluginList)
            if (auto m = dynamic_cast<MasterDevice*> (p))
                return m;

        return nullptr;
    }

    inline MasterDevice* albumChainOf (te::Edit& edit)
    {
        for (auto p : edit.getMasterPluginList())
            if (auto m = dynamic_cast<MasterDevice*> (p))
                return m;

        return nullptr;
    }

    //==============================================================================
    // The timeline: each song starts its gap after the one before ends

    struct Slot
    {
        double gap = 0.0, length = 0.0;
    };

    /** Where each song starts. A negative gap overlaps the song before (a crossfade), but never
        so far that a song starts before the one before it, or before the start. */
    inline std::vector<double> layout (const std::vector<Slot>& slots)
    {
        std::vector<double> starts;
        double prevStart = 0.0, prevEnd = 0.0;

        for (size_t i = 0; i < slots.size(); ++i)
        {
            auto start = std::max ({ 0.0, i == 0 ? slots[i].gap : prevEnd + slots[i].gap, prevStart });
            starts.push_back (start);
            prevStart = start;
            prevEnd = start + std::max (0.0, slots[i].length);
        }

        return starts;
    }

    /** Puts every song's clip where the order and the gaps say. */
    inline void arrange (te::Edit& edit)
    {
        auto songs = getSongs (edit);
        std::vector<Slot> slots;

        for (auto t : songs)
            slots.push_back ({ gapOf (*t), lengthOf (*t) });

        auto starts = layout (slots);

        for (int i = 0; i < songs.size(); ++i)
            if (auto c = clipOf (*songs[i]))
                if (std::abs (c->getPosition().getStart().inSeconds() - starts[(size_t) i]) > 1.0e-6)
                    c->setStart (te::TimePosition::fromSeconds (starts[(size_t) i]), false, true);
    }

    inline te::TimeRange rangeOf (te::AudioTrack& t)
    {
        if (auto c = clipOf (t))
            return c->getEditTimeRange();

        return {};
    }

    /** The end of the last song to finish. */
    inline te::TimePosition albumEnd (te::Edit& edit)
    {
        te::TimePosition end;

        for (auto t : getSongs (edit))
            end = std::max (end, rangeOf (*t).getEnd());

        return end;
    }

    /** Moves a song to a place in the order (0 = first). The album's lead-in (the first song's gap)
        stays at the start. */
    inline void moveSong (te::Edit& edit, te::AudioTrack& song, int newIndex)
    {
        auto songs = getSongs (edit);
        auto from = songs.indexOf (&song);
        newIndex = juce::jlimit (0, juce::jmax (0, songs.size() - 1), newIndex);

        if (from < 0 || from == newIndex)
            return;

        auto* oldFirst = songs.getFirst();
        songs.remove (from);
        te::Track* preceding = newIndex > 0 ? songs[newIndex - 1] : nullptr;

        // First: after whatever comes before the songs (nothing, normally)
        if (preceding == nullptr)
        {
            auto all = te::getTopLevelTracks (edit);

            for (int j = all.indexOf (songs.getFirst()); --j >= 0;)
                if (dynamic_cast<te::AudioTrack*> (all[j]) != nullptr || dynamic_cast<te::FolderTrack*> (all[j]) != nullptr)
                {
                    preceding = all[j];
                    break;
                }
        }

        edit.moveTrack (&song, te::TrackInsertPoint (nullptr, preceding));

        if (auto* newFirst = getSongs (edit).getFirst(); newFirst != oldFirst)
        {
            auto& um = edit.getUndoManager();
            auto leadIn = gapOf (*oldFirst);
            oldFirst->state.setProperty (gapId, gapOf (*newFirst), &um);
            newFirst->state.setProperty (gapId, leadIn, &um);
        }

        arrange (edit);
    }

    /** Adds a song at the end (or at `index`), with its own mastering chain. The clip comes when
        the song has been rendered. */
    inline te::AudioTrack* addSong (te::Edit& edit, const juce::File& song, int index = -1)
    {
        auto songs = getSongs (edit);
        te::Track* preceding = songs.isEmpty() ? nullptr : songs.getLast();

        if (preceding == nullptr)
            for (auto t : te::getTopLevelTracks (edit))
                if (dynamic_cast<te::AudioTrack*> (t) != nullptr || dynamic_cast<te::FolderTrack*> (t) != nullptr)
                    preceding = t;

        auto track = edit.insertNewAudioTrack (te::TrackInsertPoint (nullptr, preceding), nullptr);

        if (track == nullptr)
            return nullptr;

        auto ref = makeRef (song, folderOf (edit));
        auto& um = edit.getUndoManager();
        track->setName (song.getFileNameWithoutExtension());
        track->setColour (EngineHelpers::trackColourFor (songs.size()));
        track->state.setProperty (songPathId, ref.relative, &um);
        track->state.setProperty (songFileId, ref.absolute, &um);
        track->state.setProperty (gapId, songs.isEmpty() ? 0.0 : defaultGap, &um);

        if (auto chain = DeviceRegistry::create (edit, MasterDevice::xmlTypeName))
        {
            track->pluginList.insertPlugin (chain, 0, nullptr);

            if (auto m = dynamic_cast<MasterDevice*> (chain.get()))
                m->target.param().setParameter ((float) juce::jlimit (-20.0, -6.0, targetOf (edit)), juce::sendNotification);
        }

        if (index >= 0 && index < songs.size())
            moveSong (edit, *track, index);

        return track.get();
    }

    /** Points a song at its project somewhere else (after it moved). */
    inline void relink (te::Track& t, const juce::File& song)
    {
        auto ref = makeRef (song, folderOf (t.edit));
        auto& um = t.edit.getUndoManager();
        t.state.setProperty (songPathId, ref.relative, &um);
        t.state.setProperty (songFileId, ref.absolute, &um);
        t.state.removeProperty (renderedFromId, &um);   // render it again from there
    }

    inline void setGap (te::AudioTrack& song, double seconds)
    {
        song.state.setProperty (gapId, juce::jlimit (-30.0, 120.0, seconds), &song.edit.getUndoManager());
        arrange (song.edit);
    }

    //==============================================================================
    // Loudness

    /** Integrated loudness (BS.1770, LUFS), true peak (dBTP, 4x oversampled) and length of a file. */
    struct Loudness
    {
        double lufs = -std::numeric_limits<double>::infinity();
        double truePeak = -std::numeric_limits<double>::infinity();
        double seconds = 0.0;
        bool ok = false;
    };

    /** Interpolates four samples for every one in, to find peaks between samples (BS.1770 annex 2):
        a 48-tap windowed-sinc low-pass, in four phases. */
    class TruePeak
    {
    public:
        TruePeak()
        {
            constexpr int n = taps * phases;
            const auto pi = juce::MathConstants<double>::pi;

            for (int i = 0; i < n; ++i)
            {
                auto x = (i - (n - 1) / 2.0) / (double) phases;
                auto sinc = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (pi * x) / (pi * x);
                auto window = 0.42 - 0.5 * std::cos (2.0 * pi * i / (n - 1)) + 0.08 * std::cos (4.0 * pi * i / (n - 1));
                coeffs[(size_t) (i % phases)][(size_t) (i / phases)] = sinc * window;
            }
        }

        /** Feeds one sample; returns the largest magnitude among it and the three between it and
            the one before. */
        float process (float x)
        {
            history[(size_t) pos] = x;
            float peak = std::abs (x);

            for (int p = 0; p < phases; ++p)
            {
                double y = 0.0;

                for (int k = 0; k < taps; ++k)
                    y += coeffs[(size_t) p][(size_t) k] * history[(size_t) ((pos - k + taps) % taps)];

                peak = std::max (peak, (float) std::abs (y));
            }

            pos = (pos + 1) % taps;
            return peak;
        }

    private:
        static constexpr int taps = 12, phases = 4;
        std::array<std::array<double, taps>, phases> coeffs {};
        std::array<float, taps> history {};
        int pos = 0;
    };

    inline Loudness measureFile (const juce::File& f, const std::atomic<bool>* cancel = nullptr)
    {
        Loudness result;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (f));

        if (reader == nullptr || reader->sampleRate <= 0.0 || reader->numChannels == 0)
            return result;

        const auto numChannels = (int) reader->numChannels;
        const auto sr = reader->sampleRate;
        const auto step = juce::jmax (1, juce::roundToInt (sr * 0.1));
        std::vector<std::pair<Normalise::Biquad, Normalise::Biquad>> filters ((size_t) numChannels, Normalise::kWeighting (sr));
        std::vector<TruePeak> peaks ((size_t) numChannels);
        std::vector<double> quarters, blocks;
        double energy = 0.0;
        int inQuarter = 0;
        float peak = 0.0f;
        juce::AudioBuffer<float> buffer (numChannels, 16384);

        for (juce::int64 pos = 0; pos < reader->lengthInSamples;)
        {
            if (cancel != nullptr && cancel->load())
                return result;

            auto n = (int) std::min<juce::int64> (buffer.getNumSamples(), reader->lengthInSamples - pos);
            reader->read (&buffer, 0, n, pos, true, true);

            for (int i = 0; i < n; ++i)
            {
                double e = 0.0;

                for (int ch = 0; ch < numChannels; ++ch)
                {
                    auto x = buffer.getSample (ch, i);
                    peak = std::max (peak, peaks[(size_t) ch].process (x));
                    auto& [shelf, highPass] = filters[(size_t) ch];
                    auto y = highPass.process (shelf.process ((double) x));
                    e += y * y;
                }

                energy += e;

                if (++inQuarter == step)
                {
                    quarters.push_back (energy);
                    energy = 0.0;
                    inQuarter = 0;
                }
            }

            pos += n;
        }

        for (size_t i = 0; i + 4 <= quarters.size(); ++i)
            blocks.push_back ((quarters[i] + quarters[i + 1] + quarters[i + 2] + quarters[i + 3]) / (4.0 * step));

        result.lufs = Normalise::loudnessOf (blocks);
        result.truePeak = peak > 0.0f ? juce::Decibels::gainToDecibels ((double) peak, -1000.0) : -std::numeric_limits<double>::infinity();
        result.seconds = (double) reader->lengthInSamples / sr;
        result.ok = true;
        return result;
    }

    /** The gain each song's chain needs to reach the target: what it has, moved by how far it
        missed. A song that couldn't be measured (or is silent) keeps its gain. */
    inline std::vector<float> matchGains (const std::vector<double>& measuredLufs, const std::vector<float>& gains,
                                          double target, float minGain = -24.0f, float maxGain = 24.0f)
    {
        std::vector<float> result (gains);

        for (size_t i = 0; i < measuredLufs.size() && i < gains.size(); ++i)
            if (std::isfinite (measuredLufs[i]) && measuredLufs[i] > -70.0)
                result[i] = juce::jlimit (minGain, maxGain, gains[i] + (float) (target - measuredLufs[i]));

        return result;
    }

    /** True when every measured song is within `tolerance` of the target. */
    inline bool isMatched (const std::vector<double>& measuredLufs, double target, double tolerance = 0.5)
    {
        for (auto m : measuredLufs)
            if (std::isfinite (m) && m > -70.0 && std::abs (m - target) > tolerance)
                return false;

        return true;
    }

    /** "-14.2", or a dash before it's known. */
    inline juce::String formatDb (std::optional<double> v)
    {
        if (! v.has_value() || ! std::isfinite (*v) || *v < -200.0)
            return juce::String::fromUTF8 ("\xe2\x80\x94");

        return juce::String (*v, 1);
    }

    inline juce::String formatTime (double seconds)
    {
        auto s = juce::roundToInt (std::max (0.0, seconds));
        return juce::String (s / 60) + ":" + juce::String (s % 60).paddedLeft ('0', 2);
    }

    //==============================================================================
    // Exporting

    /** "01 First Song.wav", "02 Second Song.wav"... numbered in album order (three digits past 99),
        with names a file can have, and no two alike. */
    inline juce::StringArray songFileNames (const juce::StringArray& songNames, const juce::String& ext)
    {
        juce::StringArray result, used;
        const int digits = songNames.size() > 99 ? 3 : 2;

        for (int i = 0; i < songNames.size(); ++i)
        {
            auto name = juce::File::createLegalFileName (songNames[i].trim());

            if (name.isEmpty())
                name = "Track " + juce::String (i + 1);

            auto base = name;

            for (int n = 2; used.contains (name, true); ++n)
                name = base + " " + juce::String (n);

            used.add (name);
            result.add (juce::String (i + 1).paddedLeft ('0', digits) + " " + name + ext);
        }

        return result;
    }

    /** The whole album as one file: "My Album (Full Album).wav". */
    inline juce::String albumFileName (const juce::String& albumName, const juce::String& ext)
    {
        auto name = juce::File::createLegalFileName (albumName.trim());
        return (name.isEmpty() ? juce::String ("Album") : name) + " (Full Album)" + ext;
    }

    /** A folder for this export that isn't there yet: "My Album", then "My Album 2"... */
    inline juce::File newExportFolder (const juce::File& exports, const juce::String& albumName)
    {
        auto name = juce::File::createLegalFileName (albumName.trim());

        if (name.isEmpty())
            name = "Album";

        auto f = exports.getChildFile (name);

        for (int n = 2; f.exists(); ++n)
            f = exports.getChildFile (name + " " + juce::String (n));

        return f;
    }

    /** Each song mastered, as its own file (its chain, then the album's, its fades and nothing of
        the songs either side), and the whole album as one: the format comes from the settings;
        their loudness target doesn't apply, as the songs' chains set their loudness. */
    inline Export::Plan planExport (te::Edit& edit, const Export::Settings& s, const juce::File& folder,
                                    bool eachSong, bool wholeAlbum)
    {
        Export::Plan p;
        p.folder = folder;
        auto ext = Export::extensionFor (s.format);

        if (s.reserveRights)
            p.notice = RightsNotice::Notice { edit.getEditMetadata().artist };

        auto add = [&] (te::RenderSpecification spec, const juce::String& what)
        {
            if (auto result = te::validateRenderSpecification (edit, spec); result.failed())
                p.problems.add (what + ": " + result.getErrorMessage());
            else if (auto job = te::createRenderJob (edit, spec))
            {
                job->name = what;
                p.jobs.push_back (std::move (*job));
            }
        };

        juce::Array<te::AudioTrack*> songs;
        juce::StringArray names;

        for (auto t : getSongs (edit))
        {
            if (clipOf (*t) == nullptr)
            {
                p.problems.add (songNameOf (*t) + ": not rendered yet");
                continue;
            }

            songs.add (t);
            names.add (songNameOf (*t));
        }

        if (eachSong)
        {
            auto files = songFileNames (names, ext);

            for (int i = 0; i < songs.size(); ++i)
            {
                auto spec = Export::renderSpecFor (s);
                spec.tracks = { songs[i]->itemID };
                spec.time = rangeOf (*songs[i]);
                spec.includeTails = false;      // the song ends where its clip (and fade) does
                spec.useMasterPlugins = true;
                spec.destination = folder.getChildFile (files[i]);
                add (spec, names[i]);
                ++p.numStems;
            }
        }

        if (wholeAlbum && ! songs.isEmpty())
        {
            auto spec = Export::renderSpecFor (s);
            spec.time = te::TimeRange (te::TimePosition(), albumEnd (edit));
            spec.useMasterPlugins = true;

            for (auto t : te::getAudioTracks (edit))
                if (! isSong (*t))
                    spec.mutedTracks.add (t->itemID);

            spec.destination = folder.getChildFile (albumFileName (nameOf (edit), ext));
            p.mixFile = spec.destination;
            add (spec, "Full album");
        }

        return p;
    }

    //==============================================================================
    // Opening a song from the album, and coming back to it

    /** Remembers that a song was opened from an album, so it can go back. */
    inline void setReturn (juce::PropertiesFile& props, const juce::File& albumFile, const juce::File& song)
    {
        props.setValue ("masteringReturnAlbum", albumFile.getFullPathName());
        props.setValue ("masteringReturnSong", song.getFullPathName());
    }

    /** The album a project was opened from, if it was (and the album is still there). */
    inline juce::File getReturn (juce::PropertiesFile& props, const juce::File& project)
    {
        auto album = juce::File::createFileWithoutCheckingPath (props.getValue ("masteringReturnAlbum"));

        if (props.getValue ("masteringReturnSong") != project.getFullPathName() || ! album.existsAsFile())
            return {};

        return album;
    }

    //==============================================================================
    /** Makes a new mastering project, saved and ready to open. */
    inline juce::File createProject (te::Engine& engine, const juce::File& parentDir, const juce::String& name)
    {
        auto file = Project::createProjectFolder (parentDir, name);
        file.getParentDirectory().getChildFile (rendersFolder).createDirectory();

        Project p (engine, file);
        auto& edit = p.getEdit();
        edit.state.setProperty (masteringId, true, nullptr);
        edit.state.setProperty (targetId, defaultTarget, nullptr);

        for (auto t : te::getAudioTracks (edit))
            edit.deleteTrack (t);

        if (auto chain = DeviceRegistry::create (edit, MasterDevice::xmlTypeName))
            edit.getMasterPluginList().insertPlugin (chain, 0, nullptr);

        edit.getUndoManager().clearUndoHistory();
        p.save (false);
        return file;
    }

    //==============================================================================
    /**
        Keeps the album's renders up to date and measures how loud the mastered songs are.

        Renders: every few seconds each song's project is checked; one that's newer than its render
        (or has none) is rendered again, one at a time, in the background, from what's saved. Only
        the song's project is opened for that, so the album keeps playing.

        Measuring renders the album itself, a song at a time, so it stops playback first.
    */
    class Album : private juce::Timer
    {
    public:
        enum class Status { upToDate, waiting, rendering, missing, failed };

        explicit Album (te::Edit& e) : edit (e)
        {
            cleanUpRenders();
            startTimer (2000);
            juce::MessageManager::callAsync ([this, a = alive] { if (*a) check(); });
        }

        ~Album() override
        {
            stopTimer();
            *alive = false;
            cancelMeasuring = true;
            renderQueue.reset();
            measureQueue.reset();
            pool.removeAllJobs (true, 10000);
        }

        /** Something to show has changed: a song's state, a measurement. */
        std::function<void()> onChange;

        /** Rendering a song's mix: where it is, or what went wrong. */
        Status getStatus (te::AudioTrack& t) const
        {
            if (! songFileOf (t).existsAsFile())
                return Status::missing;

            if (t.itemID == renderingId)
                return Status::rendering;

            if (failures.count (t.itemID))
                return Status::failed;

            if (std::find (toRender.begin(), toRender.end(), t.itemID) != toRender.end())
                return Status::waiting;

            return clipOf (t) != nullptr ? Status::upToDate : Status::waiting;
        }

        juce::String getError (te::AudioTrack& t) const
        {
            auto it = failures.find (t.itemID);
            return it != failures.end() ? it->second : juce::String();
        }

        float getRenderProgress() const
        {
            return renderQueue != nullptr ? renderQueue->getTotalProgress() : 0.0f;
        }

        bool isBusy() const     { return renderingId.isValid() || ! toRender.empty() || isMeasuring(); }

        /** Looks for songs that changed or appeared, and renders them. */
        void check()
        {
            for (auto t : getSongs (edit))
            {
                auto song = songFileOf (*t);

                if (! song.existsAsFile() || t->itemID == renderingId)
                    continue;

                auto clip = clipOf (*t);
                auto render = clip != nullptr ? clip->getSourceFileReference().getFile() : juce::File();
                auto from = (juce::int64) t->state.getProperty (renderedFromId, (juce::int64) 0);

                if (! needsRender (song, render, from))
                    continue;

                // Failed for this version already: wait for it to change
                if (auto it = failedAt.find (t->itemID); it != failedAt.end() && it->second == songModTime (song))
                    continue;

                if (std::find (toRender.begin(), toRender.end(), t->itemID) == toRender.end())
                    toRender.push_back (t->itemID);
            }

            changed();
            renderNext();
        }

        /** Renders a song again even though it looks up to date. */
        void renderAgain (te::AudioTrack& t)
        {
            failures.erase (t.itemID);
            failedAt.erase (t.itemID);
            t.state.removeProperty (renderedFromId, nullptr);
            check();
        }

        //==============================================================================
        bool isMeasuring() const            { return measureQueue != nullptr || measuring > 0; }
        juce::String getActivity() const    { return activity; }

        /** Measures every song as it comes out of its chain and the album's. With match, then sets
            each song's chain to reach the album's target, and measures again, until they're all
            there (a few rounds: the limiters make it more than a sum). */
        void measure (bool match)
        {
            if (isMeasuring())
                return;

            matching = match;
            round = 0;
            edit.getTransport().stop (false, true);

            if (match)
            {
                edit.getUndoManager().beginNewTransaction ("Match Loudness");
                auto target = (float) juce::jlimit (-20.0, -6.0, targetOf (edit));

                for (auto t : getSongs (edit))
                    if (auto chain = chainOf (*t))
                        chain->target.param().setParameter (target, juce::sendNotification);

                if (auto album = albumChainOf (edit))
                    album->target.param().setParameter (target, juce::sendNotification);
            }

            startMeasuring();
        }

        void cancelMeasure()
        {
            cancelMeasuring = true;

            if (measureQueue != nullptr)
                measureQueue->cancelAll();
        }

        /** The sample rate renders are made at: the interface's, or 48 kHz. */
        double renderRate() const
        {
            auto rate = edit.engine.getDeviceManager().getSampleRate();
            return rate >= 44100.0 && rate <= 192000.0 ? rate : 48000.0;
        }

        /** The song's render (or measurement) file names, kept apart per version so the one
            playing is never written over. */
        static juce::File renderFileFor (const juce::File& albumFolder, const juce::String& songName, juce::int64 version)
        {
            return albumFolder.getChildFile (rendersFolder)
                     .getChildFile (juce::File::createLegalFileName (songName) + " " + juce::String::toHexString (version) + ".wav");
        }

        /** Puts a finished render on the song's track: a new clip, or the clip's audio replaced
            (keeping its fades and gain). */
        static void applyRender (te::AudioTrack& t, const juce::File& render, juce::int64 renderedFrom, double seconds)
        {
            auto& um = t.edit.getUndoManager();
            auto length = te::TimeDuration::fromSeconds (seconds);

            if (auto clip = clipOf (t))
            {
                clip->getSourceFileReference().setToFile (render, te::SourceFileReference::PathStyle::alwaysRelative, false);
                clip->setOffset ({});
                clip->setLength (length, true);
            }
            else if (auto c = t.insertWaveClip (songNameOf (t), render, { { te::TimePosition(), length }, {} }, false))
            {
                c->setAutoTempo (false);    // a song's mix plays as it was rendered
                c->setFadeIn (te::TimeDuration());
                c->setFadeOut (te::TimeDuration());
            }

            t.state.setProperty (renderedFromId, renderedFrom, &um);
            t.state.setProperty (lengthId, seconds, &um);
            t.state.removeProperty (masterLufsId, &um);    // measured from the old one
            t.state.removeProperty (masterPeakId, &um);
            arrange (t.edit);
        }

    private:
        te::Edit& edit;
        std::shared_ptr<bool> alive = std::make_shared<bool> (true);
        juce::ThreadPool pool { 1 };

        std::deque<te::EditItemID> toRender;
        te::EditItemID renderingId;
        std::unique_ptr<te::Edit> songEdit;
        std::unique_ptr<te::RenderQueue> renderQueue;
        std::map<te::EditItemID, juce::String> failures;
        std::map<te::EditItemID, juce::int64> failedAt;

        std::unique_ptr<te::RenderQueue> measureQueue;
        std::atomic<bool> cancelMeasuring { false };
        int measuring = 0, round = 0;
        bool matching = false;
        juce::String activity;
        juce::File measureFolder;

        void changed()
        {
            if (onChange)
                onChange();
        }

        void timerCallback() override   { check(); }

        te::AudioTrack* findSong (te::EditItemID id) const
        {
            return dynamic_cast<te::AudioTrack*> (te::findTrackForID (edit, id));
        }

        void fail (te::EditItemID id, juce::int64 version, const juce::String& why)
        {
            failures[id] = why;
            failedAt[id] = version;
        }

        void renderNext()
        {
            if (renderingId.isValid() || toRender.empty())
                return;

            auto id = toRender.front();
            toRender.pop_front();
            auto* t = findSong (id);

            if (t == nullptr)
                return renderNext();

            auto song = songFileOf (*t);
            auto version = songModTime (song);     // taken first: a save while rendering renders again
            failures.erase (id);

            songEdit = Project::openForRendering (edit.engine, song);

            if (songEdit == nullptr)
            {
                fail (id, version, "Couldn't open " + song.getFileName());
                return later();
            }

            auto length = songEdit->getLength();

            if (length <= te::TimeDuration::fromSeconds (0.01))
            {
                fail (id, version, "Nothing plays in " + song.getFileNameWithoutExtension());
                songEdit.reset();
                return later();
            }

            te::RenderSpecification spec;
            spec.format = te::RenderFormat::wav;
            spec.bitDepth = 32;         // floating point, so nothing clips before the mastering chain
            spec.sampleRate = renderRate();
            spec.time = te::TimeRange (te::TimePosition(), length);
            spec.includeTails = true;
            spec.channelLayout = "stereo";
            spec.useMasterPlugins = true;   // the song as it sounds in its own project
            spec.destination = renderFileFor (folderOf (edit), songNameOf (*t), version);
            spec.destination.getParentDirectory().createDirectory();

            for (auto st : te::getAudioTracks (*songEdit))
                if (st->getOutput().outputsToNone())
                    spec.mutedTracks.add (st->itemID);

            if (auto result = te::validateRenderSpecification (*songEdit, spec); result.failed())
            {
                fail (id, version, result.getErrorMessage());
                songEdit.reset();
                return later();
            }

            auto job = te::createRenderJob (*songEdit, spec);

            if (! job.has_value())
            {
                fail (id, version, "Couldn't render " + song.getFileNameWithoutExtension());
                songEdit.reset();
                return later();
            }

            renderingId = id;
            renderQueue = std::make_unique<te::RenderQueue>();
            auto handle = renderQueue->addJob (std::move (*job));
            renderQueue->onFinished = [this, id, version, handle, a = alive]
            {
                // The queue is still inside this: finish afterwards
                juce::MessageManager::callAsync ([this, id, version, handle, a]
                {
                    if (*a)
                        renderFinished (id, version, *handle);
                });
            };
            renderQueue->start();
            changed();
        }

        void later()
        {
            changed();
            juce::MessageManager::callAsync ([this, a = alive] { if (*a) renderNext(); });
        }

        void renderFinished (te::EditItemID id, juce::int64 version, te::RenderQueue::Job& job)
        {
            auto state = job.getState();
            auto file = job.getFile();
            auto error = job.getError();
            renderQueue.reset();
            songEdit.reset();
            renderingId = {};

            if (auto* t = findSong (id))
            {
                if (state == te::RenderQueue::Job::State::completed && file.existsAsFile())
                {
                    measureMix (id, file, version);
                }
                else if (state != te::RenderQueue::Job::State::cancelled)
                {
                    fail (id, version, error.isNotEmpty() ? error : juce::String ("The render didn't finish"));
                    juce::ignoreUnused (t);
                }
            }

            later();
        }

        /** The render's loudness, off the message thread; then it goes on the timeline. */
        void measureMix (te::EditItemID id, const juce::File& file, juce::int64 version)
        {
            ++measuring;
            pool.addJob ([this, id, file, version, a = alive]
            {
                auto m = measureFile (file);

                juce::MessageManager::callAsync ([this, id, file, version, m, a]
                {
                    if (! *a)
                        return;

                    --measuring;

                    if (auto* t = findSong (id))
                    {
                        if (! m.ok || m.seconds <= 0.0)
                        {
                            fail (id, version, "The render couldn't be read");
                        }
                        else
                        {
                            // Its own step, so undoing what you did last doesn't take it back
                            edit.getUndoManager().beginNewTransaction ("Update " + songNameOf (*t));
                            applyRender (*t, file, version, m.seconds);
                            t->state.setProperty (mixLufsId, m.lufs, nullptr);
                        }
                    }

                    changed();
                });
            });
        }

        //==============================================================================
        void startMeasuring()
        {
            cancelMeasuring = false;
            measureFolder = folderOf (edit).getChildFile (rendersFolder).getChildFile ("Measuring");
            measureFolder.createDirectory();
            measureQueue = std::make_unique<te::RenderQueue>();
            auto songs = getSongs (edit);
            int n = 0;

            for (auto t : songs)
            {
                if (clipOf (*t) == nullptr)
                    continue;

                te::RenderSpecification spec;
                spec.format = te::RenderFormat::wav;
                spec.bitDepth = 32;
                spec.sampleRate = renderRate();
                spec.tracks = { t->itemID };
                spec.time = rangeOf (*t);
                spec.includeTails = false;
                spec.channelLayout = "stereo";
                spec.useMasterPlugins = true;
                spec.destination = measureFolder.getChildFile (t->itemID.toString() + ".wav");

                if (te::validateRenderSpecification (edit, spec).wasOk())
                    if (auto job = te::createRenderJob (edit, spec))
                    {
                        job->name = t->itemID.toString();
                        measureQueue->addJob (std::move (*job));
                        ++n;
                    }
            }

            if (n == 0)
            {
                measureQueue.reset();
                activity = {};
                return changed();
            }

            total = n;
            done = 0;
            activity = matching ? "Matching loudness, round " + juce::String (round + 1) + "..." : "Measuring...";

            measureQueue->onJobFinished = [this] (te::RenderQueue::Job& job)
            {
                auto id = te::EditItemID::fromVar (job.getName());

                if (job.getState() != te::RenderQueue::Job::State::completed)
                    return;

                ++measuring;
                pool.addJob ([this, id, file = job.getFile(), a = alive]
                {
                    auto m = measureFile (file, &cancelMeasuring);
                    file.deleteFile();

                    juce::MessageManager::callAsync ([this, id, m, a]
                    {
                        if (! *a)
                            return;

                        --measuring;
                        ++done;

                        if (auto* t = findSong (id); t != nullptr && m.ok)
                        {
                            t->state.setProperty (masterLufsId, m.lufs, nullptr);
                            t->state.setProperty (masterPeakId, m.truePeak, nullptr);
                        }

                        measured();
                    });
                });
            };
            measureQueue->onFinished = [this, a = alive]
            {
                juce::MessageManager::callAsync ([this, a]
                {
                    if (! *a)
                        return;

                    measureQueue.reset();
                    measured();
                });
            };

            measureQueue->start();
            changed();
        }

        int total = 0, done = 0;

        /** After each song's measurement: when they're all in, match them (another round) or stop. */
        void measured()
        {
            if (! matching)
                activity = measureQueue != nullptr ? "Measuring " + juce::String (done) + " of " + juce::String (total) + "..." : juce::String();

            changed();

            if (measureQueue != nullptr || measuring > 0)
                return;

            if (! matching || cancelMeasuring)
            {
                activity = {};
                matching = false;
                return changed();
            }

            auto songs = getSongs (edit);
            std::vector<double> lufs;
            std::vector<float> gains;
            juce::Array<MasterDevice*> chains;

            for (auto t : songs)
            {
                auto chain = chainOf (*t);
                auto m = getProperty (*t, masterLufsId);

                if (chain == nullptr || ! m.has_value() || clipOf (*t) == nullptr)
                    continue;

                chains.add (chain);
                lufs.push_back (*m);
                gains.push_back (chain->inputGain.get());
            }

            auto target = targetOf (edit);

            if (isMatched (lufs, target, 0.3) || ++round >= 4)
            {
                activity = {};
                matching = false;
                return changed();
            }

            auto newGains = matchGains (lufs, gains, target);

            for (int i = 0; i < chains.size(); ++i)
                chains[i]->inputGain.param().setParameter (newGains[(size_t) i], juce::sendNotification);

            startMeasuring();
        }

        /** Renders nothing plays any more (left by earlier versions of songs). */
        void cleanUpRenders()
        {
            auto folder = folderOf (edit).getChildFile (rendersFolder);
            folder.getChildFile ("Measuring").deleteRecursively();
            std::set<juce::String> used;

            for (auto t : getSongs (edit))
                if (auto c = clipOf (*t))
                    used.insert (c->getSourceFileReference().getFile().getFullPathName());

            for (auto f : folder.findChildFiles (juce::File::findFiles, false, "*.wav"))
                if (used.count (f.getFullPathName()) == 0)
                    f.deleteFile();
        }

        JUCE_DECLARE_NON_COPYABLE (Album)
    };
}

} // namespace elsewhere
