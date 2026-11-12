// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::TakesTests
{
    /** Takes and comping: lanes, swipe comps, whole takes, unused takes, a loop recording turned
        into takes, undo, and takes surviving a save and reopen. Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
