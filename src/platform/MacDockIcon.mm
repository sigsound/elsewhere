// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#import <AppKit/AppKit.h>
#include "MacDockIcon.h"

namespace elsewhere
{
void setDockIconFromBundle()
{
    // The Dock normally reads the icon from the app bundle itself, but it isn't allowed to look
    // inside privacy-protected folders (e.g. builds in ~/Documents), and then shows a blank
    // placeholder. The running app can always read its own bundle, so hand the icon over directly.
    if (NSImage* icon = [NSImage imageNamed: NSImageNameApplicationIcon])
        [NSApp setApplicationIconImage: icon];
}

void setAppAppearance (int mode)
{
    if (@available (macOS 10.14, *))
        NSApp.appearance = mode == 1 ? [NSAppearance appearanceNamed: NSAppearanceNameAqua]
                         : mode == 2 ? [NSAppearance appearanceNamed: NSAppearanceNameDarkAqua]
                                     : nil;
}
}
