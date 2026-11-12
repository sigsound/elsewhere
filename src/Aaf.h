// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SessionImport.h"

namespace elsewhere
{
//==============================================================================
/**
    AAF (.aaf) import: how Pro Tools, Logic, Media Composer, Premiere and Resolve hand
    sessions to each other.

    Brings across each audio track with its clips at the right place (offsets, clip gain,
    fades and crossfades), the audio itself (embedded in the AAF, or alongside it), and track
    volume and pan. AAF has no tempo, plug-ins or sends.

    Written from the AAF and Microsoft compound file specifications (no AAF SDK): it reads
    the file's structured storage directly and follows each clip through its master and
    file source mobs to the audio.
*/
namespace Aaf
{
    inline constexpr const char* fileExtension = ".aaf";

    class Importer : public SessionImport::Importer
    {
    public:
        explicit Importer (const juce::File& aafFile);
        ~Importer() override;

        juce::String getError() const override;
        bool extractAudio (const juce::File& destDir, std::function<bool (double)> progress = {}) override;
        SessionImport::Summary build (te::Edit&) override;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}

} // namespace elsewhere
