# Contributing to Elsewhere

Thanks for helping. Bug reports, fixes, new devices, importers for other DAWs' files and improvements to
playing together are all welcome.

## Before you start

- **Bugs:** open an issue with what you did, what you expected and what happened. A small project that shows
  the problem helps a lot.
- **Ideas and features:** start a discussion first. For anything bigger than a fix, agree on the approach
  before writing it, so your time isn't spent on something that doesn't fit.
- **Security problems:** don't open an issue; see [SECURITY.md](SECURITY.md).

## Building and testing

Follow [Build](README.md#build) in the README. The app has self-tests built in. Each one runs and quits, and
returns non-zero if something failed. Run the ones that cover your change:

```bash
APP=build/Elsewhere_artefacts/Release/Elsewhere.app/Contents/MacOS/Elsewhere
$APP --device-test /tmp/elsewhere-tests          # built-in devices, drum patterns, modulators
$APP --dawproject-test /tmp/elsewhere-tests      # DAWproject import and export
$APP --aaf-test /tmp/elsewhere-tests             # AAF import
$APP --ableton-test /tmp/elsewhere-tests         # Ableton Live set import
$APP --cubase-test /tmp/elsewhere-tests          # Cubase project import
$APP --project-audio-test /tmp/elsewhere-tests   # keeping project audio inside the project
$APP --takes-test /tmp/elsewhere-tests           # takes, take lanes and comping
$APP --record-test /tmp/elsewhere-tests          # recording: every sample, in place, under load
$APP --midi-lanes-test /tmp/elsewhere-tests      # velocity and controller lanes
$APP --routing-test /tmp/elsewhere-tests         # buses, no-output tracks and sidechains
$APP --surfaces-test /tmp/elsewhere-tests        # Mackie Control surfaces
$APP --export-test /tmp/elsewhere-tests          # exporting the mix and stems (MP3 when LAME is installed)
$APP --plugin-test /tmp/elsewhere-tests          # hosting (needs Surge XT, Valhalla Supermassive, Sitala)
```

Collaboration and live playing have their own scripts, which run two copies of Elsewhere against each other:
`scripts/tests/collab-sync.sh` and `scripts/tests/live-session.sh` (see the comments at the top of each).

Test runs use their own settings folder, so they never touch your projects, audio setup or plug-in list.

## Writing the change

- **Match the code around it:** JUCE style (a space before parentheses, `camelCase`), everything in the
  `elsewhere` namespace, and comments that explain why rather than what.
- **New files** start with the licence header:
  ```cpp
  // Copyright (C) 2026 Fuzz LLC
  // SPDX-License-Identifier: AGPL-3.0-or-later
  ```
- **Text people see** is short, plain and specific. Say what happened and what to do next.
- **Commit messages** say what the change does, in a sentence ("Keep project audio inside the project; find
  missing audio").
- **Third-party code:** check its licence works with the AGPL, and add it to
  `resources/ThirdPartyNotices.txt`.

## The contributor agreement

Before your first pull request can be merged, you'll be asked to sign the Elsewhere Contributor License
Agreement. It's done once, in the pull request.

You keep the copyright in your work. The agreement gives Fuzz LLC the right to use it under other licences
as well as the AGPL. Fuzz LLC makes Somewhere (the iPhone and Android sampler) and paid plug-ins, which share
some of Elsewhere's code. Elsewhere itself stays free and open source; the agreement is what lets the code
you improve here also improve those.

## Code of Conduct

Everyone taking part follows the [Code of Conduct](CODE_OF_CONDUCT.md).
