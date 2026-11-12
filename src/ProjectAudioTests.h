// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::ProjectAudioTests
{
    /** A recording from another folder: collected into the project, survives Save As and a move,
        and missing files are found again by folder search. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
