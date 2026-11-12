// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "SessionImport.h"
#include "DawProject.h"
#include "Aaf.h"
#include "ProTools.h"
#include "Ableton.h"
#include "Cubase.h"
#include "Mixing.h"

namespace elsewhere::SessionImport
{

juce::String Summary::describe() const
{
    juce::String s;
    s << "Imported " << tracks << (tracks == 1 ? " track" : " tracks") << " with " << audioClips
      << (audioClips == 1 ? " audio clip" : " audio clips");

    if (midiClips > 0)
        s << " and " << midiClips << (midiClips == 1 ? " MIDI clip" : " MIDI clips");

    if (applicationName.isNotEmpty())
        s << " from " << applicationName;

    s << ".\n\n";

    if (! missingFiles.isEmpty())
    {
        s << "These audio files couldn't be found, so their clips were left out:\n";

        for (int i = 0; i < std::min (missingFiles.size(), 8); ++i)
            s << "    " << missingFiles[i] << "\n";

        if (missingFiles.size() > 8)
            s << "    ...and " << (missingFiles.size() - 8) << " more\n";

        s << "\n";
    }

    if (skippedReturnTracks > 0)
        s << skippedReturnTracks << (skippedReturnTracks == 1 ? " effect (return) track was" : " effect (return) tracks were")
          << " left out.\n\n";

    if (mixedSampleRates)
        s << "The audio files have different sample rates. They'll play at the right speed, but converting them "
             "to match gives the best quality.\n\n";

    if (headroomDb > 0.05)
        s << "With every track at its imported level the mix peaked " << juce::String (headroomDb - 1.0, 1)
          << " dB over full scale, so the master was turned down " << juce::String (headroomDb, 1)
          << " dB to stop it clipping. The balance between tracks is unchanged.\n\n";

    s << notes.joinIntoString ("\n\n");
    return s.trim();
}

bool canImport (const juce::File& f)
{
    return f.hasFileExtension (DawProject::fileExtension) || f.hasFileExtension (Aaf::fileExtension)
            || f.hasFileExtension (ProTools::fileExtensions) || f.hasFileExtension (Ableton::fileExtension)
            || f.hasFileExtension (Cubase::fileExtensions);
}

std::unique_ptr<Importer> open (const juce::File& f)
{
    if (f.hasFileExtension (Aaf::fileExtension))
        return std::make_unique<Aaf::Importer> (f);

    if (f.hasFileExtension (Cubase::fileExtensions))
        return std::make_unique<Cubase::Importer> (f);

    if (f.hasFileExtension (Ableton::fileExtension))
        return std::make_unique<Ableton::Importer> (f);

    if (f.hasFileExtension (ProTools::fileExtensions))
        return std::make_unique<ProTools::Importer> (f);

    return std::make_unique<DawProject::Importer> (f);
}

//==============================================================================
void removePlaceholderTracks (te::Edit& edit)
{
    for (auto t : te::getAudioTracks (edit))
        if (! Mixing::isReturnTrack (*t) && t->getClips().isEmpty())
            edit.deleteTrack (t);
}

te::WaveAudioClip::Ptr addAudioClip (te::AudioTrack& track, const juce::String& name, const juce::File& file,
                                      double start, double end, double sourceStart, double speed, bool stretch)
{
    if (std::abs (speed - 1.0) < 1.0e-4)
        speed = 1.0;

    te::ClipPosition position { { te::TimePosition::fromSeconds (start), te::TimePosition::fromSeconds (end) },
                                te::TimeDuration::fromSeconds (sourceStart / speed) };
    auto clip = track.insertWaveClip (name, file, position, false);

    if (clip == nullptr)
        return {};

    clip->setAutoTempo (false);

    if (speed != 1.0)
    {
        clip->setTimeStretchMode (stretch ? te::TimeStretcher::defaultMode : te::TimeStretcher::disabled);
        clip->setSpeedRatio (speed);
        clip->setPosition (position);     // the length may have been capped before the speed was set
    }

    clip->setFadeIn ({});
    clip->setFadeOut ({});
    return clip;
}

te::WaveAudioClip::Ptr addWarpedAudioClip (te::AudioTrack& track, const juce::String& name, const juce::File& file,
                                            const std::vector<std::pair<double, double>>& points)
{
    if (points.size() < 2)
        return {};

    auto start = points.front().first, end = points.back().first;
    auto clip = addAudioClip (track, name, file, start, end, 0.0);

    if (clip == nullptr)
        return {};

    // Warp time runs with the timeline, lined up with the file where the clip starts, so the
    // clip plays its warped copy from there at normal speed. The file before that point is
    // left unwarped (it isn't heard).
    auto offset = points.front().second;
    auto& warp = clip->getWarpTimeManager();
    warp.state.getChildWithName (te::IDs::WARPMARKERS).removeAllChildren (nullptr);
    warp.insertMarker ({});

    for (auto& [t, s] : points)
        if (s > 1.0e-9)
            warp.insertMarker ({ te::TimePosition::fromSeconds (s), te::TimePosition::fromSeconds (offset + t - start) });

    warp.setWarpEndMarkerTime (te::TimePosition::fromSeconds (offset + end - start));
    clip->setTimeStretchMode (te::TimeStretcher::defaultMode);
    clip->setWarpTime (true);
    clip->updateSourceFile();      // start rendering the warped copy it'll play
    clip->setPosition ({ { te::TimePosition::fromSeconds (start), te::TimePosition::fromSeconds (end) },
                         te::TimeDuration::fromSeconds (offset) });
    return clip;
}

juce::File findFileNear (const juce::File& folder, const juce::String& fileName, std::map<juce::String, juce::File>& index)
{
    if (index.empty())
    {
        int budget = 50000;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, true, "*", juce::File::findFiles))
        {
            index.insert ({ entry.getFile().getFileName().toLowerCase(), entry.getFile() });

            if (--budget <= 0)
                break;
        }

        index.insert ({ "", juce::File() });    // indexed, even if empty
    }

    auto it = index.find (fileName.toLowerCase());
    return it != index.end() ? it->second : juce::File();
}

std::optional<double> stereoSideFromName (const juce::String& fileName)
{
    auto ext = fileName.fromLastOccurrenceOf (".", false, false);
    auto name = ext.length() > 1 && ext.length() <= 4 && ! ext.equalsIgnoreCase ("L") && ! ext.equalsIgnoreCase ("R")
                    ? fileName.upToLastOccurrenceOf (".", false, false) : fileName;

    while (name.isNotEmpty() && (juce::CharacterFunctions::isDigit (name.getLastCharacter()) || name.endsWithChar ('_')))
        name = name.dropLastCharacters (1);

    for (auto sep : { ".", "_", "-", " " })
    {
        if (name.endsWithIgnoreCase (juce::String (sep) + "L"))  return -1.0;
        if (name.endsWithIgnoreCase (juce::String (sep) + "R"))  return 1.0;
    }

    return {};
}

double addHeadroom (te::Edit& edit)
{
    auto length = edit.getLength();

    if (length < 0.1s)
        return 0.0;

    juce::BigInteger tracks;
    auto all = te::getAllTracks (edit);

    for (int i = 0; i < all.size(); ++i)
        tracks.setBit (i);

    auto stats = te::Renderer::measureStatistics ("Checking the mix's level", edit, { te::TimePosition(), te::toPosition (length) },
                                                  tracks, 512, 44100.0);

    if (stats.peak <= 1.0f)
        return 0.0;

    auto down = (double) juce::Decibels::gainToDecibels (stats.peak) + 1.0;

    if (auto master = edit.getMasterVolumePlugin())
        master->setVolumeDb ((float) (master->getVolumeDb() - down));

    return down;
}

void finish (te::Edit& edit, const std::set<te::AudioTrack*>& midiTracks, const juce::Array<juce::File>& audioFiles, Summary& summary)
{
    for (auto* t : midiTracks)
    {
        bool hasAudio = false;

        for (auto* c : t->getClips())
            hasAudio = hasAudio || dynamic_cast<te::WaveAudioClip*> (c) != nullptr;

        // Instruments don't come across: give MIDI tracks Elsewhere's default synth so they make a sound
        if (! hasAudio)
            EngineHelpers::insertBuiltInPlugin (*t, te::FourOscPlugin::xmlTypeName, 0);
    }

    for (auto t : te::getAudioTracks (edit))
        t->getClipSlotList().ensureNumberOfSlots (edit.getSceneList().getScenes().size());

    Mixing::ensureSendsOnAllTracks (edit);

    for (auto& f : audioFiles)
    {
        te::AudioFile af (edit.engine, f);

        if (! af.isValid())
            continue;

        if (summary.firstSampleRate == 0.0)
            summary.firstSampleRate = af.getSampleRate();
        else if (! juce::approximatelyEqual (summary.firstSampleRate, af.getSampleRate()))
            summary.mixedSampleRates = true;
    }
}

} // namespace elsewhere::SessionImport
