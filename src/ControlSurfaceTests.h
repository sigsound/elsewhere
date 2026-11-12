// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::ControlSurfaceTests
{
    /** Mackie Control: surface messages moving faders, muting and playing. Returns the
        number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
