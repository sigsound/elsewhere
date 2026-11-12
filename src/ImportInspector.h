// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::ImportInspector
{
    /** Imports any session file (Pro Tools, AAF, DAWproject) into a new project in outDir and
        writes out every track and clip that came across, for checking real files against the
        DAW they came from. Returns 0 if the import worked. */
    int run (te::Engine&, const juce::File& session, const juce::File& outDir);
}
