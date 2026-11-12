// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::AafTests
{
    /** Imports the AAF written by scripts/make-aaf-fixture.py (Pro Tools / Media Composer style:
        embedded and external audio, 48 kHz and 25 fps tracks, clip gain, crossfades, a panned
        stereo pair, track volume) and checks where everything lands. Returns the number of failures. */
    int run (te::Engine&, const juce::File& fixtureDir);
}
