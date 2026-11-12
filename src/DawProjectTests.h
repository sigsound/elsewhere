// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::DawProjectTests
{
    /** Imports a Bitwig-style .dawproject (nested clips, warps, a tempo change, loops, MIDI,
        a group, a return and a missing file), checks where everything lands, then exports it
        and imports the export to check it round-trips. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
