// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::RoutingTests
{
    /** Buses, tracks with no output and sidechains: which routes would loop, and rendered audio
        showing a keyed compressor ducking, Listen, buses and their stems. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
