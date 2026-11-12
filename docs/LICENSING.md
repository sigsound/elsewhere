# Licensing

Elsewhere is free software under the **GNU Affero General Public License, version 3 or later**
(AGPL-3.0-or-later), copyright Fuzz LLC. The full text is in `LICENSE`. In short: anyone may use, study,
change and share Elsewhere, and anyone who distributes it (changed or not) must pass on the same freedoms,
with the source code.

The user-facing notices are in `resources/ThirdPartyNotices.txt`, shown in the About window (Help > About
Elsewhere on Windows, the Elsewhere menu on a Mac). Update them whenever a dependency changes.

## Why the AGPL

Elsewhere is built on JUCE, whose open-source licence is the AGPLv3, and Tracktion Engine, which is GPLv3 or
later. A program combined with them is covered by those licences anyway, so Elsewhere's own code uses the
AGPL too, keeping the whole thing under one licence. New source files start with:

```cpp
// Copyright (C) 2026 Fuzz LLC
// SPDX-License-Identifier: AGPL-3.0-or-later
```

## What every official build must do

- **Point to its source code.** About Elsewhere links to the release tag of its version
  (`ELSEWHERE_SOURCE_URL/tree/v<version>`), so the tag has to be pushed for every release
  (see `docs/RELEASING.md`).
- **Carry the notices.** `ThirdPartyNotices.txt` is built into the app, with the GPLv3 and AGPLv3 texts.
- **Credit Tracktion Engine.** The About window says "Powered by Tracktion Engine". The GPL doesn't require
  it, but it's a courtesy Tracktion asks for.
- **No EULA.** The AGPL doesn't allow extra restrictions, so Elsewhere ships without a licence agreement.

## Third-party code

Everything in `ThirdPartyNotices.txt` is compatible with the AGPL: MIT, BSD, Apache-2.0, MPL-2.0 and similar
licences, public-domain code, and the CC0 drum samples. Two need more than a credit:

### libptformat (LGPL-2.1-or-later)

The Pro Tools session reader. The LGPL sits comfortably inside an AGPL app, but Elsewhere keeps the setup it
used when it was closed source, since it costs nothing and keeps the library easy to swap:

1. **It stays a separate library.** It's built as `libptformat.dylib` (`Elsewhere.app/Contents/Frameworks/`)
   or `ptformat.dll` (next to `Elsewhere.exe`), and Elsewhere loads it at run time (`src/ProTools.cpp`)
   through the small C interface in `libs/ptformat-c/`.
2. **Its source ships with the app.** The build packs `ptformat-source.zip` (the library as we build it, with
   our patch applied, plus `libs/ptformat-c` and build instructions) into
   `Contents/Resources/Licenses/` on a Mac and `Licenses\` on Windows.
3. **Our changes to it stay LGPL.** That's `patches/ptformat-double-ratefactor.patch` and all of
   `libs/ptformat-c/`. They go into the zip automatically if they're made in those folders or as a
   `patches/ptformat-*.patch`.

`ThirdPartyNotices.txt` section 27 explains this in plain English and includes the full LGPL text.

### Tracktion Engine (GPL-3.0-or-later)

Elsewhere builds Tracktion Engine with a few patches (`patches/tracktion_engine-*.patch`). The patched
engine is covered by the GPL, like the rest of the source.

## Closed products: Somewhere and the plug-ins

Somewhere (the iPhone and Android sampler) and Elsewhere's paid plug-ins are closed source. They reuse
Elsewhere code, such as `src/devices/Dsp.h`, under Fuzz LLC's own rights as the copyright holder, not under
the AGPL. That only works for code Fuzz LLC owns, which is why every outside contribution to this repository
comes with a contributor agreement (see `CONTRIBUTING.md`).

- **Somewhere** uses neither JUCE nor Tracktion Engine (it builds `Dsp.h` against a small stand-in for
  `juce_core`), so it needs no licence from either.
- **The plug-ins** are built with JUCE, so they need a commercial JUCE licence. They'd also need a commercial
  Tracktion licence if they use Tracktion Engine.
