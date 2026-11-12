// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "../Common.h"

namespace elsewhere::DeviceTests
{
    /** Renders test signals through every Elsewhere device, checks the results and writes
        WAVs and panel snapshots to outDir. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
