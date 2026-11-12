// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

#pragma once

namespace elsewhere
{
    /** Shows the bundle's icon in the Dock, even when the Dock can't read the bundle. */
    void setDockIconFromBundle();

    /** Sets the look of native windows, menus and dialogs: 0 follows macOS, 1 light, 2 dark. */
    void setAppAppearance (int mode);
}
