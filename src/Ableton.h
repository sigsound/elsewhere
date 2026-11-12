// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SessionImport.h"

namespace elsewhere
{
//==============================================================================
/**
    Ableton Live Sets (.als), from Live 8 onwards.

    A Live Set is gzipped XML whose arrangement model is close to DAWproject's: clips in
    beats, with warp markers mapping their content to seconds in the file. So it's converted
    to DAWproject in memory and built by the DAWproject importer: audio clips with their
    warping, loops, fades, gain and transposition; MIDI notes; tempo and its automation;
    groups; and each track's volume, pan and mute. Session View clips, devices, sends and
    return tracks aren't brought across.
*/
namespace Ableton
{
    inline constexpr const char* fileExtension = ".als";

    class Importer : public SessionImport::Importer
    {
    public:
        explicit Importer (const juce::File& liveSet);
        ~Importer() override;

        juce::String getError() const override;
        bool extractAudio (const juce::File& destDir, std::function<bool (double)> progress = {}) override;
        SessionImport::Summary build (te::Edit&) override;

        /** The DAWproject the set was converted to (for the self-test). */
        const juce::XmlElement* getConvertedProject() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}

} // namespace elsewhere
