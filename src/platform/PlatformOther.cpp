// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Windows / Linux side of MacDockIcon.mm: the app icon comes from the executable's
// resources, and window frames follow the system theme, so there's nothing to do.
#include "MacDockIcon.h"

namespace elsewhere
{
void setDockIconFromBundle() {}
void setAppAppearance (int) {}
}
