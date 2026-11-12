// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::ExportTests
{
    /** Exporting: which tracks become stems and what they're called, the files' formats, the
        stems adding up exactly to the mix, and loudness targets. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
