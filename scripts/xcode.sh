#!/bin/bash
# Copyright (C) 2026 Fuzz LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
# Generates build-xcode/Elsewhere.xcodeproj with two schemes:
#   "Elsewhere (Debug)"   - debugger-friendly, catches assertion failures
#   "Elsewhere (Release)" - full speed, for actually playing and recording
# Re-run after adding source files. Usage: scripts/xcode.sh [--open]
set -euo pipefail
cd "$(dirname "$0")/.."

# Signing team: set ELSEWHERE_DEVELOPMENT_TEAM in your environment for signed builds
# (docs/RELEASING.md); without it Xcode signs locally
cmake -S . -B build-xcode -G Xcode -DELSEWHERE_DEVELOPMENT_TEAM="${ELSEWHERE_DEVELOPMENT_TEAM:-}"

SCHEMES="build-xcode/Elsewhere.xcodeproj/xcshareddata/xcschemes"
BASE="$SCHEMES/Elsewhere.xcscheme"

if [ ! -f "$BASE" ]; then
    echo "Expected $BASE to be generated" >&2
    exit 1
fi

for CONFIG in Debug Release; do
    OUT="$SCHEMES/Elsewhere ($CONFIG).xcscheme"
    # Run, Profile and Analyze all use the chosen configuration
    sed -E "s/buildConfiguration *= *\"[A-Za-z]+\"/buildConfiguration=\"$CONFIG\"/g" "$BASE" > "$OUT"
done

rm "$BASE"

# Hide CMake's helper targets (ALL_BUILD, ZERO_CHECK...) from the scheme menu
mkdir -p build-xcode/Elsewhere.xcodeproj/project.xcworkspace/xcshareddata
cat > build-xcode/Elsewhere.xcodeproj/project.xcworkspace/xcshareddata/WorkspaceSettings.xcsettings <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>IDEWorkspaceSharedSettings_AutocreateContextsIfNeeded</key>
    <false/>
</dict>
</plist>
PLIST

echo "Schemes: Elsewhere (Debug), Elsewhere (Release)"
[ "${1:-}" = "--open" ] && open build-xcode/Elsewhere.xcodeproj
exit 0
