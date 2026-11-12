// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

#include "Common.h"

namespace elsewhere::PerfTest
{
    /** Plays a project the way an audio interface would, without opening one: its own engine,
        driven one buffer at a time from a real-time thread, paced like a device. Reports how long
        each audio callback took against its deadline, for each number of audio threads.

        --perf-test <project.elsewhere> <outDir> [bufferSize] [seconds]
        Use it on a copy of a project: it loads it as a normal project does. */
    int run (const juce::StringArray& args);
}
