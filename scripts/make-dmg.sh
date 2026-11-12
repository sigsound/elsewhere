#!/bin/bash
# Copyright (C) 2026 Fuzz LLC
# SPDX-License-Identifier: AGPL-3.0-or-later
# Builds a universal (Intel + Apple Silicon) Release build and packages it as a .dmg.
#
# Signing, best available:
#   1. "Developer ID Application" certificate in your keychain -> signed + notarized + stapled,
#      so it opens on anyone's Mac with a normal double-click.
#      Notarization uses the keychain profile "elsewhere-notary" (one-time setup, see README).
#   2. Otherwise -> ad-hoc signed; recipients must right-click > Open the first time.
#
# Usage: scripts/make-dmg.sh
set -euo pipefail
cd "$(dirname "$0")/.."

VERSION=$(cat VERSION | tr -d '[:space:]')
NOTARY_PROFILE="elsewhere-notary"

cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_DEPLOYMENT_TARGET=10.15
cmake --build build --target Elsewhere

APP="build/Elsewhere_artefacts/Release/Elsewhere.app"
ENTITLEMENTS="build/Elsewhere_artefacts/JuceLibraryCode/Elsewhere.entitlements"
OUT="dist/Elsewhere-${VERSION}.dmg"
STAGE="build/dmg-stage"

IDENTITY=$(security find-identity -v -p codesigning | grep -o '"Developer ID Application: [^"]*"' | head -1 | tr -d '"' || true)

if [ -n "$IDENTITY" ]; then
    echo "Signing with: $IDENTITY"
    codesign --force --deep --options runtime --timestamp --entitlements "$ENTITLEMENTS" --sign "$IDENTITY" "$APP"
else
    echo "No Developer ID Application certificate found: ad-hoc signing (recipients must right-click > Open)."
    codesign --force --deep --options runtime --entitlements "$ENTITLEMENTS" --sign - "$APP"
fi

codesign --verify --deep --strict "$APP"

rm -rf "$STAGE" && mkdir -p "$STAGE" dist
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"

if [ -n "$IDENTITY" ]; then
    OPEN_STEP="2. Open Elsewhere from Applications."
else
    OPEN_STEP="2. The first time, RIGHT-CLICK Elsewhere > Open (this build isn't notarised by Apple).
   If macOS still refuses: System Settings > Privacy & Security > \"Open Anyway\"."
fi

cat > "$STAGE/READ ME FIRST.txt" <<TXT
Elsewhere ${VERSION}

1. Drag Elsewhere into Applications.
${OPEN_STEP}
3. Choose your audio interface in Options > Audio / MIDI Settings.
4. Scan for your plug-ins in Options > Plug-in Manager.

Working together: open a project that has been shared with you in Google Drive or Dropbox
(File > Open Project, pick the .elsewhere file). You'll see each other's
changes within a few seconds.
TXT

# The mounted disk shows the app icon too
cp "$APP/Contents/Resources/AppIcon.icns" "$STAGE/.VolumeIcon.icns"

rm -f "$OUT" "build/elsewhere-rw.dmg"
hdiutil create -volname "Elsewhere ${VERSION}" -srcfolder "$STAGE" -ov -format UDRW "build/elsewhere-rw.dmg" >/dev/null
MOUNT=$(hdiutil attach -readwrite -noverify -noautoopen "build/elsewhere-rw.dmg" | grep -o '/Volumes/.*$' | head -1)
SetFile -a C "$MOUNT"
hdiutil detach "$MOUNT" -quiet
hdiutil convert "build/elsewhere-rw.dmg" -format UDZO -o "$OUT" >/dev/null
rm -f "build/elsewhere-rw.dmg"

if [ -n "$IDENTITY" ]; then
    codesign --force --timestamp --sign "$IDENTITY" "$OUT"

    if xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" >/dev/null 2>&1; then
        echo "Notarizing (usually takes a few minutes)..."
        xcrun notarytool submit "$OUT" --keychain-profile "$NOTARY_PROFILE" --wait
        xcrun stapler staple "$OUT"
        spctl --assess --type open --context context:primary-signature -v "$OUT" || true
    else
        echo "Signed but NOT notarized: set up the '$NOTARY_PROFILE' keychain profile (see README) to notarize."
    fi
fi

echo "Built $OUT"
lipo -archs "$APP/Contents/MacOS/Elsewhere"
