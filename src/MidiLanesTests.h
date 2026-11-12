// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::MidiLanesTests
{
    /** The piano roll's velocity and controller lane: painting velocities, drawing controllers,
        their values and undo, and the lane's mouse gestures. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
