// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::CubaseTests
{
    /** Imports a Cubase project written by the test in Cubase's object format (class
        declarations and back-references, a clip shared by reference, musical and linear
        tracks, overlapping takes, a moved file and a missing one) and checks where everything
        lands. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
