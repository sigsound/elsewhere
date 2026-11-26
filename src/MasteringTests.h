// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::MasteringTests
{
    /** Mastering projects: finding songs when folders move, knowing when a render is out of date,
        the album's layout, loudness measurement and matching, export names, and a whole album
        rendered, matched and exported. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
