// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace elsewhere
{
    /** The system's Reduce Motion setting (macOS), or "Show animations" turned off (Windows). */
    bool systemPrefersReducedMotion();

    /** Increase Contrast (macOS), or a high contrast theme (Windows). */
    bool systemPrefersMoreContrast();

    /** True while VoiceOver (macOS) or a screen reader such as Narrator or NVDA (Windows) is running. */
    bool systemScreenReaderRunning();
}
