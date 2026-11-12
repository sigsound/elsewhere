// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "SessionImport.h"

namespace elsewhere
{
//==============================================================================
/**
    Pro Tools sessions (.ptx, .ptf, .pts), opened without Pro Tools.

    Read by libptformat (LGPL-2.1+), which ships as a separate shared library inside the app
    (libptformat.dylib on a Mac, ptformat.dll on Windows) and is loaded at run time; see
    docs/LICENSING.md. If it's missing, Pro Tools import just isn't available.

    Brings across each audio track's clips at the right place, with the audio from the
    session's Audio Files folder. A session doesn't give us fades, clip gain, volume, pan
    or tempo, and its MIDI isn't brought across yet.
*/
namespace ProTools
{
    inline constexpr const char* fileExtensions = "ptx;ptf;pts";

    /** Whether the session reader library could be loaded. */
    bool isAvailable();

    class Importer : public SessionImport::Importer
    {
    public:
        explicit Importer (const juce::File& session);
        ~Importer() override;

        juce::String getError() const override;
        bool extractAudio (const juce::File& destDir, std::function<bool (double)> progress = {}) override;
        SessionImport::Summary build (te::Edit&) override;

        /** What the reader found, as JSON (for the self-test). */
        juce::var getSessionData() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl;
    };
}

} // namespace elsewhere
