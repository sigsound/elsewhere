// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::PluginTests
{
    /** Loads the installed third-party plug-ins used for testing (Surge XT, Valhalla
        Supermassive, Sitala), plays them, and checks sound, tails, state and editors.
        Returns the number of failures. */
    int run (te::Engine&, const juce::File& outDir);
}
