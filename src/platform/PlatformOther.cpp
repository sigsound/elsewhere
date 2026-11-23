// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later

// The Windows / Linux side of MacDockIcon.mm and MacSystemAccessibility.mm: the app icon comes
// from the executable's resources, and window frames follow the system theme, so there's nothing
// to do for those. The accessibility settings come from SystemParametersInfo.
#include "MacDockIcon.h"
#include "SystemAccessibility.h"

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
#endif

namespace elsewhere
{
void setDockIconFromBundle() {}
void setAppAppearance (int) {}

#if defined (_WIN32)
bool systemPrefersReducedMotion()
{
    BOOL animate = TRUE;
    return SystemParametersInfoW (SPI_GETCLIENTAREAANIMATION, 0, &animate, 0) && ! animate;
}

bool systemPrefersMoreContrast()
{
    HIGHCONTRASTW hc {};
    hc.cbSize = sizeof (hc);
    return SystemParametersInfoW (SPI_GETHIGHCONTRAST, sizeof (hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

bool systemScreenReaderRunning()
{
    BOOL running = FALSE;
    return SystemParametersInfoW (SPI_GETSCREENREADER, 0, &running, 0) && running;
}
#else
bool systemPrefersReducedMotion()   { return false; }
bool systemPrefersMoreContrast()    { return false; }
bool systemScreenReaderRunning()    { return false; }
#endif
}
