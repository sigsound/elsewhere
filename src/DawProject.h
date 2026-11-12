// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SessionImport.h"

namespace elsewhere
{
//==============================================================================
/**
    DAWproject (.dawproject) import and export: the open project format Bitwig, Studio One
    and Cubase/Nuendo read and write.

    What comes across: tracks and groups, audio clips at the right place (offsets, stretch,
    loops and fades), MIDI notes, the tempo and time-signature maps, and each track's
    volume, pan, mute and solo. Plug-ins, sends and automation don't.

    Audio clips in other DAWs often sit inside clip containers and "Warps" (time-stretch
    maps). Import flattens all of that into plain Elsewhere clips: a clip whose stretch changes
    part-way through becomes several clips, one per evenly-stretched stretch.
*/
namespace DawProject
{
    inline constexpr const char* fileExtension = ".dawproject";

    using ImportSummary = SessionImport::Summary;

    class Importer : public SessionImport::Importer
    {
    public:
        /** Opens the file and parses project.xml. Check getError() afterwards. */
        explicit Importer (const juce::File& dawprojectFile);

        /** For other formats converted to DAWproject in memory (Ableton): audio is referenced by
            absolute path. Clips may also carry "elsewhereGain" (linear) and "elsewhereTranspose"
            (semitones) attributes, which DAWproject itself doesn't have. */
        Importer (std::unique_ptr<juce::XmlElement> project, const juce::File& baseFolder);
        ~Importer() override;

        juce::String getError() const override;
        bool extractAudio (const juce::File& destDir, std::function<bool (double)> progress = {}) override;
        ImportSummary build (te::Edit&) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };

    //==============================================================================
    /** An export gathered from an Edit (message thread), written by writeExport (any thread). */
    struct ExportPlan
    {
        std::unique_ptr<juce::XmlElement> project, metadata;
        std::vector<std::pair<juce::File, juce::String>> audioFiles;   // file -> path in the archive
        juce::StringArray warnings;
    };

    ExportPlan planExport (te::Edit&, const juce::String& projectName);
    juce::Result writeExport (const ExportPlan&, const juce::File& destination);
}

} // namespace elsewhere
