// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::RecordTests
{
    /** Records through the engine the way an audio interface would, without opening one: a known
        signal goes in at the inputs, and what lands in Samples/Recorded and on the timeline is
        checked against it, sample by sample. Covers what was recorded (every sample, in order,
        within one 24-bit step), where it was placed (device latency, punch, loop passes), many
        inputs at once, and what a late audio callback does to a take.

        --record-test <outDir> */
    int run (const juce::StringArray& args);
}
