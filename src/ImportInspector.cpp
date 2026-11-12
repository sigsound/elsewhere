// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#include "ImportInspector.h"
#include "SessionImport.h"
#include "ProTools.h"
#include "Ableton.h"
#include "Cubase.h"
#include "Project.h"

namespace elsewhere::ImportInspector
{

int run (te::Engine& engine, const juce::File& session, const juce::File& outDir)
{
    juce::String log;
    log << "Importing " << session.getFullPathName() << "\n\n";

    auto importer = SessionImport::open (session);

    if (importer->getError().isNotEmpty())
    {
        log << "ERROR: " << importer->getError() << "\n";
        std::cout << log;
        return 1;
    }

    outDir.createDirectory();

    // What the Pro Tools reader saw, before Elsewhere does anything with it
    if (auto* pt = dynamic_cast<ProTools::Importer*> (importer.get()))
        outDir.getChildFile ("ptformat.json").replaceWithText (juce::JSON::toString (pt->getSessionData()));

    // Formats converted to DAWproject first: what they were converted to
    const juce::XmlElement* converted = nullptr;
    if (auto* ab = dynamic_cast<Ableton::Importer*> (importer.get()))  converted = ab->getConvertedProject();
    if (auto* cu = dynamic_cast<Cubase::Importer*> (importer.get()))   converted = cu->getConvertedProject();
    if (converted != nullptr)
        converted->writeTo (outDir.getChildFile ("as-dawproject.xml"));

    outDir.getChildFile ("Imported Project").deleteRecursively();
    auto editFile = Project::createProjectFolder (outDir, "Imported");

    if (! importer->extractAudio (editFile.getParentDirectory().getChildFile ("Samples/Imported")))
        log << "WARNING: copying the audio failed or was cancelled\n";

    Project project (engine, editFile);
    auto& edit = project.getEdit();
    auto summary = importer->build (edit);
    summary.headroomDb = SessionImport::addHeadroom (edit);
    project.save();

    log << summary.describe() << "\n";

    if (auto master = edit.getMasterVolumePlugin())
        log << "Master volume " << juce::String (master->getVolumeDb(), 2) << " dB\n\n";

    for (auto* t : te::getAllTracks (edit))
    {
        auto* at = dynamic_cast<te::AudioTrack*> (t);

        if (at == nullptr)
            continue;

        auto* vol = at->getVolumePlugin();
        log << "Track \"" << at->getName() << "\"";

        if (vol != nullptr)
            log << "  volume " << juce::String (vol->getVolumeDb(), 2) << " dB  pan " << juce::String (vol->getPan(), 2);

        log << "\n";

        for (auto* c : at->getClips())
        {
            auto pos = c->getPosition();
            log << "    " << juce::String (pos.getStart().inSeconds(), 4) << " - " << juce::String (pos.getEnd().inSeconds(), 4)
                << " s  offset " << juce::String (pos.getOffset().inSeconds(), 4) << "  \"" << c->getName() << "\"";

            if (auto* w = dynamic_cast<te::WaveAudioClip*> (c))
            {
                log << "  " << w->getOriginalFile().getFileName();

                if (w->getGainDB() != 0.0f)                log << "  gain " << juce::String (w->getGainDB(), 2) << " dB";
                if (w->getFadeIn() > te::TimeDuration())   log << "  fade in " << juce::String (w->getFadeIn().inSeconds(), 3);
                if (w->getFadeOut() > te::TimeDuration())  log << "  fade out " << juce::String (w->getFadeOut().inSeconds(), 3);
                if (w->getSpeedRatio() != 1.0)             log << "  speed " << juce::String (w->getSpeedRatio(), 4);
            }

            log << "\n";
        }
    }

    log << "\nProject: " << editFile.getFullPathName() << "\n";
    outDir.getChildFile ("import-report.txt").replaceWithText (log);
    std::cout << log;
    return 0;
}

} // namespace elsewhere::ImportInspector
