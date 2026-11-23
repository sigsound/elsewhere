// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#import <AppKit/AppKit.h>
#include "SystemAccessibility.h"

namespace elsewhere
{
bool systemPrefersReducedMotion()
{
    return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldReduceMotion;
}

bool systemPrefersMoreContrast()
{
    return NSWorkspace.sharedWorkspace.accessibilityDisplayShouldIncreaseContrast;
}

bool systemScreenReaderRunning()
{
    return NSWorkspace.sharedWorkspace.voiceOverEnabled;
}
}
