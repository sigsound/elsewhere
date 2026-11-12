// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::AbletonTests
{
    /** Imports two Live Sets written by the test (Live 12 and Live 8 layouts): warped audio at
        another tempo, a tempo jump, an unwarped clip, a loop, clip gain and transposition, MIDI,
        a group, a muted track, a return and relative file paths. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
